#include "eventnet/strongswan_vici_client.h"
#include "eventnet/strongswan_observer.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static const char *tunnel_state_name(en_tunnel_state_t state)
{
    switch (state) {
    case EN_TUNNEL_ESTABLISHED: return "established";
    case EN_TUNNEL_REKEYING: return "rekeying";
    case EN_TUNNEL_FAILED: return "failed";
    default: return "unknown";
    }
}

static void print_event(void *context, const char *event_json)
{
    (void)context;
    if (event_json != NULL) fputs(event_json, stdout);
    fflush(stdout);
}

static void sleep_ms(long long milliseconds)
{
    if (milliseconds <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)(milliseconds > 0xFFFFFFFFLL ? 0xFFFFFFFFLL : milliseconds));
#else
    struct timespec delay = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&delay, NULL);
#endif
}

static int parse_nonnegative_long_long(const char *value, long long *result)
{
    if (value == NULL || value[0] == '\0' || result == NULL) return 0;
    char *end = NULL;
    errno = 0;
    long long parsed = strtoll(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed < 0) return 0;
    *result = parsed;
    return 1;
}

static int parse_nonnegative_int(const char *value, int *result)
{
    long long parsed = 0;
    if (!parse_nonnegative_long_long(value, &parsed) || parsed > INT_MAX || result == NULL) return 0;
    *result = (int)parsed;
    return 1;
}

static int run_monitor(const char *uri, const char *child_id, const char *path_id,
    long long duration_ms, int retry_count, long long backoff_ms)
{
    char error[256] = {0};
    for (int attempt = 0; attempt <= retry_count; attempt++) {
        en_strongswan_vici_client_t *client = NULL;
        if (en_strongswan_vici_client_open(&client, uri, error, sizeof(error)) == EN_ERR_NONE) {
            en_error_code_t status = en_strongswan_vici_client_monitor_child(
                client, child_id, path_id, duration_ms, print_event, NULL, error, sizeof(error));
            en_strongswan_vici_client_close(client);
            if (status == EN_ERR_NONE) {
                fprintf(stderr, "VICI monitor completed: %s attempts=%d\n", child_id, attempt + 1);
                return 0;
            }
        }
        if (attempt < retry_count) {
            fprintf(stderr, "VICI monitor attempt %d failed: %s; retrying\n", attempt + 1, error);
            sleep_ms(backoff_ms);
        }
    }
    fprintf(stderr, "VICI monitor failed after %d attempt(s): %s\n", retry_count + 1, error);
    return 1;
}

int main(int argc, char **argv)
{
    const char *uri = argc > 1 ? argv[1] : NULL;
    const char *action = argc > 2 ? argv[2] : "version";
    const char *child_id = argc > 3 ? argv[3] : NULL;
    if (strcmp(action, "monitor") == 0 || strcmp(action, "monitor-forever") == 0) {
        long long duration_ms = 0;
        int retry_count = 0;
        long long backoff_ms = 250;
        if (argc < 6 || child_id == NULL || argv[4] == NULL ||
            !parse_nonnegative_long_long(argv[5], &duration_ms) ||
            (strcmp(action, "monitor") == 0 && duration_ms < 1) ||
            (argc > 6 && !parse_nonnegative_int(argv[6], &retry_count)) ||
            (argc > 7 && !parse_nonnegative_long_long(argv[7], &backoff_ms))) {
            fprintf(stderr, "usage: %s [URI] monitor CHILD_ID PATH_ID DURATION_MS [RETRY_COUNT] [BACKOFF_MS]\n", argv[0]);
            return 2;
        }
        return run_monitor(uri, child_id, argv[4], duration_ms, retry_count, backoff_ms);
    }
    en_strongswan_vici_client_t *client = NULL;
    char error[256] = {0};
    if (en_strongswan_vici_client_open(&client, uri, error, sizeof(error)) != EN_ERR_NONE) {
        fprintf(stderr, "VICI connection failed: %s\n", error);
        return 1;
    }
    if (strcmp(action, "version") == 0) {
        char version[256] = {0};
        if (en_strongswan_vici_client_version(client, version, sizeof(version), error, sizeof(error)) != EN_ERR_NONE) {
            fprintf(stderr, "VICI version request failed: %s\n", error);
            en_strongswan_vici_client_close(client);
            return 1;
        }
        printf("VICI connected: %s\n", version);
    } else if (strcmp(action, "initiate") == 0 || strcmp(action, "terminate") == 0 || strcmp(action, "observe") == 0) {
        if (child_id == NULL) {
            fprintf(stderr, "child id is required for %s\n", action);
            en_strongswan_vici_client_close(client);
            return 2;
        }
        en_strongswan_sa_observation_t observation = {0};
        en_error_code_t status = strcmp(action, "initiate") == 0
            ? en_strongswan_vici_client_initiate(client, child_id, error, sizeof(error))
            : strcmp(action, "terminate") == 0
                ? en_strongswan_vici_client_terminate(client, child_id, error, sizeof(error))
            : en_strongswan_vici_client_observe_child(client, child_id, &observation, error, sizeof(error));
        if (status != EN_ERR_NONE) {
            fprintf(stderr, "VICI %s failed: %s\n", action, error);
            en_strongswan_vici_client_close(client);
            return 1;
        }
        if (strcmp(action, "observe") == 0) {
            printf("VICI observe succeeded: %s state=%s health=%s\n", child_id,
                tunnel_state_name(observation.state), en_health_state_name(observation.health));
        } else {
            printf("VICI %s succeeded: %s\n", action, child_id);
        }
    } else {
        fprintf(stderr, "usage: %s [URI] [version|initiate|terminate|observe] [CHILD_ID]\n", argv[0]);
        fprintf(stderr, "       %s [URI] monitor CHILD_ID PATH_ID DURATION_MS [RETRY_COUNT] [BACKOFF_MS]\n", argv[0]);
        fprintf(stderr, "       %s [URI] monitor-forever CHILD_ID PATH_ID 0 [RETRY_COUNT] [BACKOFF_MS]\n", argv[0]);
        en_strongswan_vici_client_close(client);
        return 2;
    }
    en_strongswan_vici_client_close(client);
    return 0;
}
