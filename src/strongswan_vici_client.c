#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "eventnet/strongswan_vici_client.h"

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libvici.h>
#include "eventnet/strongswan_observer.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

struct en_strongswan_vici_client {
    vici_conn_t *connection;
};

static bool valid_child_id(const char *child_id)
{
    if (child_id == NULL || child_id[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)child_id; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == ':' || *cursor == '.' || *cursor == '_' || *cursor == '-')) return false;
    }
    return true;
}

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error != NULL && error_len > 0) snprintf(error, error_len, "%s", message == NULL ? "" : message);
}

en_error_code_t en_strongswan_vici_client_open(
    en_strongswan_vici_client_t **client,
    const char *uri,
    char *error,
    size_t error_len)
{
    if (client == NULL) return EN_ERR_INVALID_ARGUMENT;
    *client = NULL;
    vici_init();
    en_strongswan_vici_client_t *candidate = calloc(1, sizeof(*candidate));
    if (candidate == NULL) {
        vici_deinit();
        set_error(error, error_len, "unable to allocate VICI client");
        return EN_ERR_STATE_CONFLICT;
    }
    candidate->connection = vici_connect((char *)uri);
    if (candidate->connection == NULL) {
        set_error(error, error_len, strerror(errno));
        free(candidate);
        vici_deinit();
        return EN_ERR_STATE_CONFLICT;
    }
    *client = candidate;
    return EN_ERR_NONE;
}

en_error_code_t en_strongswan_vici_client_version(
    en_strongswan_vici_client_t *client,
    char *output,
    size_t output_len,
    char *error,
    size_t error_len)
{
    if (client == NULL || client->connection == NULL || output == NULL || output_len == 0) return EN_ERR_INVALID_ARGUMENT;
    output[0] = '\0';
    vici_req_t *request = vici_begin("version");
    if (request == NULL) {
        set_error(error, error_len, "unable to create VICI request");
        return EN_ERR_STATE_CONFLICT;
    }
    vici_res_t *response = vici_submit(request, client->connection);
    if (response == NULL) {
        set_error(error, error_len, strerror(errno));
        return EN_ERR_STATE_CONFLICT;
    }
    const char *daemon = vici_find_str(response, "", "daemon");
    const char *version = vici_find_str(response, "", "version");
    const char *sysname = vici_find_str(response, "", "sysname");
    const char *release = vici_find_str(response, "", "release");
    const char *machine = vici_find_str(response, "", "machine");
    int written = snprintf(output, output_len, "%s %s (%s, %s, %s)",
        daemon == NULL ? "" : daemon, version == NULL ? "" : version,
        sysname == NULL ? "" : sysname, release == NULL ? "" : release,
        machine == NULL ? "" : machine);
    vici_free_res(response);
    if (written < 0 || (size_t)written >= output_len) {
        set_error(error, error_len, "VICI version response is too long");
        return EN_ERR_INVALID_ARGUMENT;
    }
    return EN_ERR_NONE;
}

static en_error_code_t child_command(
    en_strongswan_vici_client_t *client,
    const char *command,
    const char *child_id,
    char *error,
    size_t error_len)
{
    if (client == NULL || client->connection == NULL || command == NULL || !valid_child_id(child_id)) return EN_ERR_INVALID_ARGUMENT;
    vici_req_t *request = vici_begin(command);
    if (request == NULL) {
        set_error(error, error_len, "unable to create VICI request");
        return EN_ERR_STATE_CONFLICT;
    }
    vici_add_key_valuef(request, "child", "%s", child_id);
    vici_res_t *response = vici_submit(request, client->connection);
    if (response == NULL) {
        set_error(error, error_len, strerror(errno));
        return EN_ERR_STATE_CONFLICT;
    }
    const char *success = vici_find_str(response, "no", "success");
    if (success == NULL || strcmp(success, "yes") != 0) {
        set_error(error, error_len, vici_find_str(response, "", "errmsg"));
        vici_free_res(response);
        return EN_ERR_STATE_CONFLICT;
    }
    vici_free_res(response);
    return EN_ERR_NONE;
}

en_error_code_t en_strongswan_vici_client_initiate(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    char *error,
    size_t error_len)
{
    return child_command(client, "initiate", child_id, error, error_len);
}

en_error_code_t en_strongswan_vici_client_terminate(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    char *error,
    size_t error_len)
{
    return child_command(client, "terminate", child_id, error, error_len);
}

typedef struct {
    const char *child_id;
    char state[32];
    bool in_target;
    bool found;
} child_observation_t;

typedef struct {
    child_observation_t observation;
    const char *path_id;
    en_strongswan_vici_event_fn event_fn;
    void *event_context;
    volatile bool closed;
} monitor_capture_t;

static int capture_child_value(void *context, vici_res_t *response, char *name, void *value, int length)
{
    (void)response;
    child_observation_t *capture = context;
    if (capture == NULL || name == NULL || value == NULL || length < 0 || !capture->in_target) return 0;
    if (strcmp(name, "state") == 0 && (size_t)length < sizeof(capture->state)) {
        memcpy(capture->state, value, (size_t)length);
        capture->state[length] = '\0';
        capture->found = true;
    }
    return 0;
}

static int capture_child(void *context, vici_res_t *response, char *name)
{
    child_observation_t *capture = context;
    if (capture == NULL || name == NULL) return 0;
    capture->in_target = strcmp(name, capture->child_id) == 0;
    int result = vici_parse_cb(response, NULL, capture_child_value, NULL, capture);
    capture->in_target = false;
    return result;
}

static int capture_ike(void *context, vici_res_t *response, char *name)
{
    if (name != NULL && strcmp(name, "child-sas") == 0) {
        return vici_parse_cb(response, capture_child, capture_child_value, NULL, context);
    }
    return 0;
}

static void capture_list_event(void *context, char *name, vici_res_t *response)
{
    if (name != NULL && response != NULL) {
        vici_parse_cb(response, capture_ike, capture_child_value, NULL, context);
    }
}

static void capture_monitor_event(void *context, char *name, vici_res_t *response)
{
    (void)name;
    monitor_capture_t *capture = context;
    if (capture == NULL || response == NULL || capture->event_fn == NULL) return;
    capture->observation.found = false;
    capture->observation.state[0] = '\0';
    vici_parse_cb(response, capture_ike, capture_child_value, NULL, &capture->observation);
    if (capture->observation.found) {
        en_strongswan_sa_observation_t observation = {0};
        char event[512] = {0};
        char error[128] = {0};
        int written = snprintf(event, sizeof(event), "%s:\n  state: %s\n",
            capture->observation.child_id, capture->observation.state);
        if (written > 0 && (size_t)written < sizeof(event) &&
            en_strongswan_parse_list_sas(event, capture->observation.child_id, &observation, error, sizeof(error)) == EN_ERR_NONE &&
            en_strongswan_observation_to_event_json(&observation, capture->path_id, en_now_ms(), event, sizeof(event)) == EN_ERR_NONE) {
            capture->event_fn(capture->event_context, event);
        }
    }
}

static void wait_duration(long long duration_ms)
{
    if (duration_ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)(duration_ms > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : duration_ms));
#else
    struct timespec delay = {duration_ms / 1000, (long)(duration_ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
#endif
}

static void monitor_close(void *context)
{
    monitor_capture_t *capture = context;
    if (capture != NULL) capture->closed = true;
}

en_error_code_t en_strongswan_vici_client_observe_child(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    en_strongswan_sa_observation_t *observation,
    char *error,
    size_t error_len)
{
    if (client == NULL || client->connection == NULL || !valid_child_id(child_id) || observation == NULL) return EN_ERR_INVALID_ARGUMENT;
    child_observation_t capture = {.child_id = child_id};
    if (vici_register(client->connection, "list-sa", capture_list_event, &capture) != 0) {
        set_error(error, error_len, strerror(errno));
        return EN_ERR_STATE_CONFLICT;
    }
    vici_req_t *request = vici_begin("list-sas");
    if (request == NULL) {
        vici_register(client->connection, "list-sa", NULL, NULL);
        set_error(error, error_len, "unable to create VICI request");
        return EN_ERR_STATE_CONFLICT;
    }
    vici_add_key_valuef(request, "child", "%s", child_id);
    vici_res_t *response = vici_submit(request, client->connection);
    if (response != NULL) vici_free_res(response);
    vici_register(client->connection, "list-sa", NULL, NULL);
    if (response == NULL) {
        set_error(error, error_len, strerror(errno));
        return EN_ERR_STATE_CONFLICT;
    }
    if (!capture.found) {
        set_error(error, error_len, "VICI CHILD_SA was not found or has no state");
        return EN_ERR_NOT_FOUND;
    }
    char list_sas_output[128] = {0};
    int written = snprintf(list_sas_output, sizeof(list_sas_output), "%s:\n  state: %s\n", child_id, capture.state);
    if (written < 0 || (size_t)written >= sizeof(list_sas_output)) return EN_ERR_INVALID_ARGUMENT;
    return en_strongswan_parse_list_sas(list_sas_output, child_id, observation, error, error_len);
}

en_error_code_t en_strongswan_vici_client_monitor_child_until(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    const char *path_id,
    long long duration_ms,
    en_strongswan_vici_event_fn event_fn,
    void *event_context,
    en_strongswan_vici_stop_fn stop_fn,
    void *stop_context,
    char *error,
    size_t error_len)
{
    if (client == NULL || client->connection == NULL || !valid_child_id(child_id) ||
        path_id == NULL || path_id[0] == '\0' || duration_ms < 0 || event_fn == NULL) return EN_ERR_INVALID_ARGUMENT;
    monitor_capture_t capture = {
        .observation = {.child_id = child_id},
        .path_id = path_id,
        .event_fn = event_fn,
        .event_context = event_context,
    };
    if (vici_register(client->connection, "child-updown", capture_monitor_event, &capture) != 0) {
        set_error(error, error_len, strerror(errno));
        return EN_ERR_STATE_CONFLICT;
    }
    vici_on_close(client->connection, monitor_close, &capture);
    if (duration_ms == 0) {
        while (!capture.closed && !(stop_fn != NULL && stop_fn(stop_context))) wait_duration(100);
    } else {
        wait_duration(duration_ms);
    }
    int unregister_status = vici_register(client->connection, "child-updown", NULL, NULL);
    vici_on_close(client->connection, NULL, NULL);
    if (capture.closed) {
        set_error(error, error_len, "VICI connection closed during monitor");
        return EN_ERR_STATE_CONFLICT;
    }
    if (unregister_status != 0) {
        set_error(error, error_len, strerror(errno));
        return EN_ERR_STATE_CONFLICT;
    }
    return EN_ERR_NONE;
}

en_error_code_t en_strongswan_vici_client_monitor_child(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    const char *path_id,
    long long duration_ms,
    en_strongswan_vici_event_fn event_fn,
    void *event_context,
    char *error,
    size_t error_len)
{
    return en_strongswan_vici_client_monitor_child_until(client, child_id, path_id, duration_ms, event_fn, event_context,
        NULL, NULL, error, error_len);
}

void en_strongswan_vici_client_close(en_strongswan_vici_client_t *client)
{
    if (client == NULL) return;
    if (client->connection != NULL) vici_disconnect(client->connection);
    free(client);
    vici_deinit();
}
