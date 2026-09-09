#if !defined(_WIN32)
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "eventnet/mock_adapters.h"
#include "eventnet/command_adapters.h"
#include "eventnet/telemetry.h"
#include "eventnet/yaml_config.h"
#include "eventnet/json_output.h"
#if defined(EVENTNET_ENABLE_STRONGSWAN_VICI)
#include "eventnet/strongswan_vici_adapter.h"
#include "eventnet/strongswan_vici_client.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>
#if !defined(_WIN32)
#include <signal.h>
#endif
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
static volatile sig_atomic_t reload_signal_received = 0;
static volatile sig_atomic_t shutdown_signal_received = 0;

static void handle_reload_signal(int signal_number)
{
    if (signal_number == SIGHUP) reload_signal_received = 1;
}

static void handle_shutdown_signal(int signal_number)
{
    if (signal_number == SIGINT || signal_number == SIGTERM) shutdown_signal_received = 1;
}

static int install_signal_handler(int signal_number, void (*handler)(int))
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    return sigaction(signal_number, &action, NULL);
}
#endif

static bool shutdown_requested(void)
{
#if !defined(_WIN32)
    return shutdown_signal_received != 0;
#else
    return false;
#endif
}

static const en_intent_t *find_intent(const en_yaml_config_t *config, const char *intent_id)
{
    for (size_t index = 0; index < config->intent_count; index++) {
        if (intent_id == NULL || strcmp(config->intents[index].intent_id, intent_id) == 0) return &config->intents[index];
    }
    return NULL;
}

static bool intent_references_path(const en_intent_t *intent, const char *path_id)
{
    if (intent == NULL || path_id == NULL || path_id[0] == '\0') return false;
    if (intent->path_selection.mode == EN_SELECT_EXPLICIT && strcmp(intent->path_selection.path_id, path_id) == 0) return true;
    for (size_t index = 0; index < intent->path_selection.candidate_count; index++) {
        if (strcmp(intent->path_selection.candidates[index], path_id) == 0) return true;
    }
    return intent->fallback.enabled && strcmp(intent->fallback.path_id, path_id) == 0;
}

static long long now_ms(void)
{
    struct timespec timestamp;
    timespec_get(&timestamp, TIME_UTC);
    return (long long)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static bool parse_long_long_argument(const char *text, long long minimum, long long maximum, long long *value)
{
    char *end = NULL;
    long long parsed;
    if (text == NULL || text[0] == '\0') return false;
    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed < minimum || parsed > maximum) return false;
    *value = parsed;
    return true;
}

static bool parse_int_argument(const char *text, int minimum, int maximum, int *value)
{
    long long parsed = 0;
    if (!parse_long_long_argument(text, minimum, maximum, &parsed)) return false;
    *value = (int)parsed;
    return true;
}

static bool valid_option_string(const char *value, size_t capacity)
{
    return value != NULL && strlen(value) < capacity;
}

static void sleep_ms(long long milliseconds)
{
    if (milliseconds <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec delay = { milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L };
    nanosleep(&delay, NULL);
#endif
}

static void set_unknown_health(const en_yaml_config_t *config, en_health_probe_mock_t *health_mock)
{
    bool require_interface_and_route = health_mock->require_interface_and_route;
    memset(health_mock, 0, sizeof(*health_mock));
    health_mock->require_interface_and_route = require_interface_and_route;
    for (size_t index = 0; index < config->path_count; index++) {
        en_path_health_t unknown = {0};
        snprintf(unknown.path_id, sizeof(unknown.path_id), "%s", config->paths[index].path_id);
        unknown.state = EN_HEALTH_FAILED;
        unknown.packet_loss_percent = 100.0;
        en_health_probe_mock_set(health_mock, unknown);
    }
}

static const en_path_t *find_telemetry_path(const en_yaml_config_t *config, const char *path_id)
{
    if (config == NULL || path_id == NULL) return NULL;
    for (size_t index = 0; index < config->path_count; index++) {
        if (strcmp(config->paths[index].path_id, path_id) == 0) return &config->paths[index];
    }
    return NULL;
}

static const en_tunnel_t *find_telemetry_tunnel(const en_yaml_config_t *config, const char *tunnel_id)
{
    if (config == NULL || tunnel_id == NULL) return NULL;
    for (size_t index = 0; index < config->tunnel_count; index++) {
        if (strcmp(config->tunnels[index].tunnel_id, tunnel_id) == 0) return &config->tunnels[index];
    }
    return NULL;
}

static bool telemetry_edge_allows_vlan(const en_vpp_edge_t *edge, int vlan_id)
{
    if (edge == NULL || edge->allowed_vlan_count == 0) return true;
    for (size_t index = 0; index < edge->allowed_vlan_count; index++) {
        if (edge->allowed_vlans[index] == vlan_id) return true;
    }
    return false;
}

static bool telemetry_edge_belongs_to_path(const en_path_t *path, const char *node_id)
{
    if (path == NULL || node_id == NULL) return false;
    if (strcmp(node_id, path->source) == 0 || strcmp(node_id, path->destination) == 0) return true;
    for (size_t index = 0; index < path->segment_count; index++) {
        const en_segment_t *segment = &path->segments[index];
        if (strcmp(node_id, segment->from_node) == 0 || strcmp(node_id, segment->to_node) == 0) return true;
    }
    return false;
}

static bool telemetry_interface_matches_path(const en_yaml_config_t *config, const en_intent_t *intent, const en_path_t *path, const char *interface_name)
{
    if (config == NULL || intent == NULL || path == NULL || interface_name == NULL || interface_name[0] == '\0') return false;
    for (size_t index = 0; index < config->vpp_edge_count; index++) {
        const en_vpp_edge_t *edge = &config->vpp_edges[index];
        if (!telemetry_edge_belongs_to_path(path, edge->node_id)) continue;
        if (intent->traffic.has_vlan_id && !telemetry_edge_allows_vlan(edge, intent->traffic.vlan_id)) continue;
        size_t parent_length = strlen(edge->vpp_interface);
        if (parent_length == 0 || strncmp(interface_name, edge->vpp_interface, parent_length) != 0) continue;
        if (intent->traffic.has_vlan_id) {
            char expected_name[EN_MAX_ID_LEN] = {0};
            if (snprintf(expected_name, sizeof(expected_name), "%s.%d", edge->vpp_interface, intent->traffic.vlan_id) >= (int)sizeof(expected_name)) return false;
            if (strcmp(interface_name, expected_name) == 0) return true;
            continue;
        }
        if (interface_name[parent_length] == '\0' || interface_name[parent_length] == '.') return true;
    }
    return false;
}

static bool telemetry_identity_matches(const en_yaml_config_t *config, const en_intent_t *intent, const en_path_health_t *record, char *reason, size_t reason_len)
{
    const en_path_t *path = find_telemetry_path(config, record == NULL ? NULL : record->path_id);
    if (path == NULL || record == NULL) {
        snprintf(reason, reason_len, "telemetry path is not defined");
        return false;
    }
    if (record->source_node[0] != '\0' && strcmp(record->source_node, path->source) != 0) {
        snprintf(reason, reason_len, "telemetry source does not match path source");
        return false;
    }
    if (record->observed_tunnel_id[0] != '\0') {
        bool tunnel_matches = false;
        for (size_t index = 0; index < path->segment_count; index++) {
            if (strcmp(record->observed_tunnel_id, path->segments[index].tunnel_id) == 0) {
                tunnel_matches = true;
                break;
            }
        }
        if (!tunnel_matches) {
            snprintf(reason, reason_len, "telemetry tunnel does not belong to path");
            return false;
        }
    }
    if (record->observed_interface_name[0] != '\0' &&
        !telemetry_interface_matches_path(config, intent, path, record->observed_interface_name)) {
        snprintf(reason, reason_len, "telemetry interface does not belong to path");
        return false;
    }
    if (record->target[0] != '\0') {
        bool target_matches = strcmp(record->target, path->destination) == 0 ||
            (path->route_next_hop[0] != '\0' && strcmp(record->target, path->route_next_hop) == 0);
        for (size_t index = 0; !target_matches && index < path->segment_count; index++) {
            const en_tunnel_t *tunnel = find_telemetry_tunnel(config, path->segments[index].tunnel_id);
            if (tunnel != NULL && (strcmp(record->target, tunnel->local_endpoint) == 0 || strcmp(record->target, tunnel->remote_endpoint) == 0)) {
                target_matches = true;
            }
        }
        if (!target_matches) {
            snprintf(reason, reason_len, "telemetry target does not match path endpoints");
            return false;
        }
    }
    if (record->observed_destination_prefix[0] != '\0' || record->observed_next_hop[0] != '\0') {
        bool route_matches = false;
        if (record->observed_destination_prefix[0] == '\0' || record->observed_next_hop[0] == '\0') {
            snprintf(reason, reason_len, "telemetry route identity is incomplete");
            return false;
        }
        if ((!record->has_table_id || path->route_count == 0) &&
            strcmp(record->observed_destination_prefix, path->route_destination_prefix) == 0 &&
            strcmp(record->observed_next_hop, path->route_next_hop) == 0) route_matches = true;
        for (size_t index = 0; !route_matches && index < path->route_count; index++) {
            const en_route_t *route = &path->routes[index];
            if (strcmp(record->observed_destination_prefix, route->destination_prefix) == 0 &&
                strcmp(record->observed_next_hop, route->next_hop) == 0 &&
                (!record->has_table_id || route->table_id < 0 || route->table_id == record->table_id)) route_matches = true;
        }
        if (!route_matches) {
            snprintf(reason, reason_len, "telemetry route does not match path route");
            return false;
        }
    }
    return true;
}

static int load_health(const char *filename, long long max_age_ms, const en_yaml_config_t *config, const en_intent_t *intent, en_health_probe_mock_t *health_mock)
{
    en_path_health_t records[EN_MAX_PATHS] = {0};
    size_t record_count = 0;
    char error[256] = {0};
    en_error_code_t status = en_telemetry_load_jsonl(filename, records, EN_MAX_PATHS, &record_count, error, sizeof(error));
    if (status != EN_ERR_NONE) {
        fprintf(stderr, "telemetry load failed: %s\n", error);
        return 1;
    }
    health_mock->require_interface_and_route = intent->traffic.has_vlan_id;
    set_unknown_health(config, health_mock);
    size_t accepted_count = 0;
    long long current_time_ms = now_ms();
    for (size_t index = 0; index < record_count; index++) {
        char identity_error[128] = {0};
        if (!telemetry_identity_matches(config, intent, &records[index], identity_error, sizeof(identity_error))) {
            fprintf(stderr, "telemetry identity rejected: %s\n", identity_error);
            return 1;
        }
        long long age_ms = current_time_ms - records[index].last_updated_ms;
        if (max_age_ms > 0 && (age_ms < 0 || age_ms > max_age_ms)) continue;
        en_health_probe_mock_set(health_mock, records[index]);
        accepted_count++;
    }
    printf("telemetry_records: %zu\n", accepted_count);
    return 0;
}

static void print_result(const en_reconcile_result_t *result)
{
    printf("intent: %s\nselected_path: %s\ntransition_state: %s\nreason: %s\n",
        result->intent_id, result->selected_path, en_transition_state_name(result->transition_state), result->explanation.reason);
    for (size_t index = 0; index < result->explanation.excluded_count; index++) {
        printf("excluded: %s (%s)\n", result->explanation.excluded_path_ids[index], result->explanation.excluded_reasons[index]);
    }
}

static void write_status_json(FILE *output, const en_reconcile_result_t *result)
{
    if (output == NULL) return;
    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = document == NULL ? NULL : yyjson_mut_obj(document);
    yyjson_mut_val *excluded = root == NULL ? NULL : yyjson_mut_obj_add_arr(document, root, "excluded");
    bool valid = root != NULL && excluded != NULL &&
        yyjson_mut_obj_add_str(document, root, "schema", "ibuki.status.v1") &&
        yyjson_mut_obj_add_sint(document, root, "timestamp_ms", now_ms()) &&
        yyjson_mut_obj_add_str(document, root, "intent_id", result->intent_id) &&
        yyjson_mut_obj_add_str(document, root, "selected_path", result->selected_path) &&
        yyjson_mut_obj_add_str(document, root, "transition_state", en_transition_state_name(result->transition_state)) &&
        yyjson_mut_obj_add_str(document, root, "reason", result->explanation.reason);
    for (size_t index = 0; valid && index < result->explanation.excluded_count; index++) {
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(document, excluded);
        valid = item != NULL && yyjson_mut_obj_add_str(document, item, "path_id", result->explanation.excluded_path_ids[index]) &&
            yyjson_mut_obj_add_str(document, item, "reason", result->explanation.excluded_reasons[index]);
    }
    if (valid) {
        yyjson_mut_doc_set_root(document, root);
        (void)en_json_mut_doc_write_line(output, document);
    }
    yyjson_mut_doc_free(document);
}

static bool valid_state_field(const char *value, bool path_id)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (isalnum(*cursor) || *cursor == ':' || *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-') continue;
        if (!path_id && (*cursor == '>' || *cursor == '|' || *cursor == '=')) continue;
        return false;
    }
    return true;
}

static int restore_state(en_controller_t *controller, const en_intent_t *intent, const char *filename)
{
    if (filename == NULL) return 0;
    if (controller == NULL || intent == NULL) return 1;
    char expected_traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&intent->traffic, expected_traffic_key, sizeof(expected_traffic_key));
    FILE *input = NULL;
#if defined(_WIN32)
    input = fopen(filename, "r");
#else
    int state_fd = open(filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (state_fd < 0) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "state restore open failed: %s: %s\n", filename, strerror(errno));
        return 1;
    }
    struct stat state_stat;
    if (fstat(state_fd, &state_stat) != 0 || !S_ISREG(state_stat.st_mode) ||
        (state_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(state_fd);
        fprintf(stderr, "state restore rejected unsafe file: %s\n", filename);
        return 1;
    }
    input = fdopen(state_fd, "r");
    if (input == NULL) {
        close(state_fd);
        fprintf(stderr, "state restore open failed: %s\n", filename);
        return 1;
    }
#endif
    if (input == NULL) return 0;
    char line[EN_MAX_TRAFFIC_KEY_LEN + EN_MAX_ID_LEN + 4] = {0};
    int status = 0;
    int restored_count = 0;
    while (fgets(line, sizeof(line), input) != NULL) {
        char *separator = strchr(line, '\t');
        if (strchr(line, '\n') == NULL && !feof(input)) { status = 1; break; }
        if (separator == NULL || strchr(separator + 1, '\t') != NULL) { status = 1; break; }
        *separator++ = '\0';
        separator[strcspn(separator, "\r\n")] = '\0';
        const en_path_t *restored_path = en_controller_find_path(controller, separator);
        if (!valid_state_field(line, false) || !valid_state_field(separator, true) ||
            strcmp(line, expected_traffic_key) != 0 || !intent_references_path(intent, separator) ||
            restored_path == NULL || restored_path->administrative_state != EN_ADMIN_ENABLED ||
            !en_controller_path_nodes_enabled(controller, restored_path)) { status = 1; break; }
        if (en_controller_applied_path(controller, line) != NULL) { status = 1; break; }
        if (en_controller_restore_applied_path(controller, line, separator) != EN_ERR_NONE) { status = 1; break; }
        restored_count++;
    }
    fclose(input);
    if (status != 0) fprintf(stderr, "state restore failed: %s\n", filename);
    else printf("state_restored: %d\n", restored_count);
    return status;
}

static int persist_state(const char *filename, const en_intent_t *intent, const en_reconcile_result_t *result)
{
    if (filename == NULL) return 0;
    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&intent->traffic, traffic_key, sizeof(traffic_key));
    char temporary_filename[512] = {0};
#if defined(_WIN32)
    if (snprintf(temporary_filename, sizeof(temporary_filename), "%s.tmp", filename) >= (int)sizeof(temporary_filename)) {
#else
    if (snprintf(temporary_filename, sizeof(temporary_filename), "%s.tmp-eventnetd-%ld", filename, (long)getpid()) >= (int)sizeof(temporary_filename)) {
#endif
        fprintf(stderr, "state path too long: %s\n", filename);
        return 1;
    }
#if defined(_WIN32)
    FILE *output = fopen(temporary_filename, "w");
#else
    int temporary_fd = open(temporary_filename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    FILE *output = temporary_fd < 0 ? NULL : fdopen(temporary_fd, "w");
#endif
    if (output == NULL) {
#if !defined(_WIN32)
        if (temporary_fd >= 0) close(temporary_fd);
        unlink(temporary_filename);
#endif
        fprintf(stderr, "state save failed: %s\n", filename);
        return 1;
    }
    fprintf(output, "%s\t%s\n", traffic_key, result->selected_path);
    if (fflush(output) != 0 || fclose(output) != 0) {
        remove(temporary_filename);
        fprintf(stderr, "state save failed: %s\n", filename);
        return 1;
    }
#if defined(_WIN32)
    remove(filename);
#endif
    if (rename(temporary_filename, filename) != 0) {
        remove(temporary_filename);
        fprintf(stderr, "state replace failed: %s\n", filename);
        return 1;
    }
    return 0;
}

static FILE *open_status_output(const char *filename)
{
    if (filename == NULL) return NULL;
#if defined(_WIN32)
    return fopen(filename, "a");
#else
    int status_fd = open(filename, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (status_fd < 0) return NULL;
    struct stat status_stat;
    if (fstat(status_fd, &status_stat) != 0 || !S_ISREG(status_stat.st_mode) ||
        (status_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(status_fd);
        return NULL;
    }
    FILE *output = fdopen(status_fd, "a");
    if (output == NULL) close(status_fd);
    return output;
#endif
}

#if defined(EVENTNET_ENABLE_STRONGSWAN_VICI)
typedef struct {
    en_controller_t *controller;
    const en_yaml_config_t *config;
    const en_intent_t *intent;
    en_health_probe_mock_t *health;
    FILE *status_output;
    const char *state_filename;
    int event_count;
    int failure_count;
} vici_monitor_context_t;

static bool eventnetd_vici_should_stop(void *context)
{
    (void)context;
    return shutdown_requested();
}

static void eventnetd_vici_event(void *context, const char *event_json)
{
    vici_monitor_context_t *monitor = context;
    en_path_health_t record = {0};
    en_reconcile_result_t result = {0};
    char parse_error[256] = {0};
    char identity_error[256] = {0};
    if (monitor == NULL || event_json == NULL) return;
    if (en_telemetry_parse_json_line(event_json, &record, parse_error, sizeof(parse_error)) != EN_ERR_NONE ||
        !telemetry_identity_matches(monitor->config, monitor->intent, &record, identity_error, sizeof(identity_error))) {
        monitor->failure_count++;
        fprintf(stderr, "vici event rejected: %s\n", parse_error[0] != '\0' ? parse_error : identity_error);
        return;
    }
    en_health_probe_mock_set(monitor->health, record);
    en_error_code_t status = en_controller_submit_intent(monitor->controller, monitor->intent, &result);
    if (status != EN_ERR_NONE) {
        monitor->failure_count++;
        fprintf(stderr, "vici event reconcile failed: %s\n", en_error_code_name(status));
        return;
    }
    monitor->event_count++;
    print_result(&result);
    write_status_json(monitor->status_output, &result);
    if (persist_state(monitor->state_filename, monitor->intent, &result) != 0) monitor->failure_count++;
    fflush(stdout);
}

static int run_vici_monitor(
    const en_yaml_config_t *config,
    const en_intent_t *intent,
    const char *backend,
    const char *swanctl_uri,
    const char *vppctl_socket,
    bool verify_vpp,
    bool apply,
    FILE *status_output,
    const char *state_filename,
    const char *child_id,
    const char *path_id,
    long long duration_ms)
{
    char error[256] = {0};
    en_strongswan_vici_client_t *client = NULL;
    en_strongswan_vici_ctx_t vici_context = {0};
    en_health_probe_mock_t health = {0};
    en_vpp_command_ctx_t vpp_command = {0};
    en_controller_t *controller = NULL;
    en_reconcile_result_t initial_result = {0};
    if (strcmp(backend, "command") != 0 || swanctl_uri == NULL || child_id == NULL || path_id == NULL) {
        fprintf(stderr, "VICI monitor requires --backend command, --swanctl-uri, child, and path\n");
        return 2;
    }
    if (en_strongswan_vici_client_open(&client, swanctl_uri, error, sizeof(error)) != EN_ERR_NONE) {
        fprintf(stderr, "VICI open failed: %s\n", error);
        return 1;
    }
    if (en_strongswan_vici_bind_client(&vici_context, client) != EN_ERR_NONE) {
        en_strongswan_vici_client_close(client);
        return 1;
    }
    vpp_command.dry_run = !apply;
    vpp_command.verify_route = verify_vpp;
    vpp_command.edges = config->vpp_edges;
    vpp_command.edge_count = config->vpp_edge_count;
    vpp_command.has_vlan_id = intent->traffic.has_vlan_id;
    vpp_command.deny_unmatched_vlan = intent->deny_unmatched_vlan;
    vpp_command.vlan_id = intent->traffic.vlan_id;
    if (vppctl_socket != NULL) snprintf(vpp_command.vppctl_socket, sizeof(vpp_command.vppctl_socket), "%s", vppctl_socket);
    vpp_command.tunnels = config->tunnels;
    vpp_command.tunnel_count = config->tunnel_count;
    set_unknown_health(config, &health);
    controller = en_controller_create_with_nodes_and_tunnels(config->nodes, config->node_count, config->paths, config->path_count,
        config->tunnels, config->tunnel_count, en_strongswan_vici_adapter(&vici_context), en_vpp_command_adapter(&vpp_command),
        en_health_probe_mock_adapter(&health));
    if (controller == NULL || restore_state(controller, intent, state_filename) != 0) {
        en_controller_destroy(controller);
        en_strongswan_vici_client_close(client);
        return 1;
    }
    if (en_controller_submit_intent(controller, intent, &initial_result) != EN_ERR_NONE) {
        fprintf(stderr, "initial VICI reconcile failed\n");
        en_controller_destroy(controller);
        en_strongswan_vici_client_close(client);
        return 1;
    }
    print_result(&initial_result);
    write_status_json(status_output, &initial_result);
    if (persist_state(state_filename, intent, &initial_result) != 0) {
        en_controller_destroy(controller);
        en_strongswan_vici_client_close(client);
        return 1;
    }
    vici_monitor_context_t monitor = {
        .controller = controller,
        .config = config,
        .intent = intent,
        .health = &health,
        .status_output = status_output,
        .state_filename = state_filename,
    };
    printf("vici_monitor: child=%s path=%s duration_ms=%lld\n", child_id, path_id, duration_ms);
    en_error_code_t monitor_status = en_strongswan_vici_client_monitor_child_until(client, child_id, path_id, duration_ms,
        eventnetd_vici_event, &monitor, eventnetd_vici_should_stop, NULL, error, sizeof(error));
    printf("vici_monitor_completed: events=%d failures=%d\n", monitor.event_count, monitor.failure_count);
    en_controller_destroy(controller);
    en_strongswan_vici_client_close(client);
    if (monitor_status != EN_ERR_NONE || monitor.failure_count != 0) {
        fprintf(stderr, "VICI monitor failed: %s\n", error[0] == '\0' ? "event processing failure" : error);
        return 1;
    }
    return 0;
}
#endif

static int run_stream(FILE *input, const en_yaml_config_t *config, const en_intent_t *intent, long long max_age_ms, int count,
    int batch_size, const char *backend, const char *swanctl_uri, const char *swanctl_config_file, const char *vppctl_socket, bool verify_swanctl, bool verify_vpp, bool apply,
    FILE *status_output, const char *state_filename, int max_records_per_second)
{
    en_health_probe_mock_t health_mock = {0};
    en_vpp_mock_t vpp_mock = {0};
    health_mock.require_interface_and_route = intent->traffic.has_vlan_id;
    en_strongswan_command_ctx_t strongswan_command = {0};
    en_vpp_command_ctx_t vpp_command = {0};
    strongswan_command.dry_run = !apply;
    strongswan_command.verify_tunnel = verify_swanctl;
    strongswan_command.block_non_ipsec = intent->block_non_ipsec;
    if (swanctl_uri != NULL) snprintf(strongswan_command.swanctl_uri, sizeof(strongswan_command.swanctl_uri), "%s", swanctl_uri);
    if (swanctl_config_file != NULL) snprintf(strongswan_command.swanctl_config_file, sizeof(strongswan_command.swanctl_config_file), "%s", swanctl_config_file);
    vpp_command.dry_run = !apply;
    vpp_command.verify_route = verify_vpp;
    vpp_command.edges = config->vpp_edges;
    vpp_command.edge_count = config->vpp_edge_count;
    vpp_command.has_vlan_id = intent->traffic.has_vlan_id;
    vpp_command.deny_unmatched_vlan = intent->deny_unmatched_vlan;
    vpp_command.vlan_id = intent->traffic.vlan_id;
    if (vppctl_socket != NULL) snprintf(vpp_command.vppctl_socket, sizeof(vpp_command.vppctl_socket), "%s", vppctl_socket);
    vpp_command.tunnels = config->tunnels;
    vpp_command.tunnel_count = config->tunnel_count;
    en_strongswan_adapter_t strongswan_adapter = en_strongswan_mock_adapter();
    en_vpp_adapter_t vpp_adapter = en_vpp_mock_adapter(&vpp_mock);
    if (strcmp(backend, "command") == 0) {
        strongswan_adapter = en_strongswan_command_adapter(&strongswan_command);
        vpp_adapter = en_vpp_command_adapter(&vpp_command);
    } else if (strcmp(backend, "mock") != 0) {
        fprintf(stderr, "unknown backend: %s\n", backend);
        return 2;
    }
    printf("backend: %s (%s)\n", backend, apply ? "apply" : "dry-run");
    en_controller_t *controller = en_controller_create_with_nodes_and_tunnels(config->nodes, config->node_count, config->paths, config->path_count, config->tunnels, config->tunnel_count,
        strongswan_adapter, vpp_adapter, en_health_probe_mock_adapter(&health_mock));
    if (controller == NULL) {
        fprintf(stderr, "controller create failed\n");
        return 1;
    }
    if (restore_state(controller, intent, state_filename) != 0) {
        en_controller_destroy(controller);
        return 1;
    }
    set_unknown_health(config, &health_mock);
    char line[1024];
    en_path_health_t batch[EN_MAX_PATHS] = {0};
    size_t batch_count = 0;
    int iteration = 0;
    int successful_iterations = 0;
    long long rate_window_start_ms = 0;
    int rate_window_count = 0;
    while (!shutdown_requested() && (count == 0 || iteration < count) && fgets(line, sizeof(line), input) != NULL) {
        if (max_records_per_second > 0) {
            long long current_time_ms = now_ms();
            if (rate_window_start_ms == 0 || current_time_ms - rate_window_start_ms >= 1000) {
                rate_window_start_ms = current_time_ms;
                rate_window_count = 0;
            }
            if (rate_window_count >= max_records_per_second) {
                sleep_ms(1000 - (current_time_ms - rate_window_start_ms));
                rate_window_start_ms = now_ms();
                rate_window_count = 0;
            }
            rate_window_count++;
        }
        en_path_health_t record = {0};
        char error[256] = {0};
        if (en_telemetry_parse_json_line(line, &record, error, sizeof(error)) != EN_ERR_NONE) {
            fprintf(stderr, "telemetry parse failed: %s\n", error);
            en_controller_destroy(controller);
            return 1;
        }
        char identity_error[128] = {0};
        if (!telemetry_identity_matches(config, intent, &record, identity_error, sizeof(identity_error))) {
            fprintf(stderr, "telemetry identity rejected: %s\n", identity_error);
            en_controller_destroy(controller);
            return 1;
        }
        long long age_ms = now_ms() - record.last_updated_ms;
        if (max_age_ms > 0 && (age_ms < 0 || age_ms > max_age_ms)) continue;
        if (batch_count < EN_MAX_PATHS) batch[batch_count++] = record;
        if (batch_count < (size_t)batch_size) continue;
        for (size_t index = 0; index < batch_count; index++) en_health_probe_mock_set(&health_mock, batch[index]);
        batch_count = 0;
        iteration++;
        printf("eventnetd_iteration: %d/%s\n", iteration, count == 0 ? "stream" : "count");
        en_reconcile_result_t result = {0};
        en_error_code_t status = en_controller_submit_intent(controller, intent, &result);
        if (status != EN_ERR_NONE) {
            if (status == EN_ERR_NO_CANDIDATE) {
                fprintf(stderr, "intent unavailable: no_candidate; waiting for next telemetry batch\n");
                continue;
            }
            fprintf(stderr, "intent failed: %s\n", en_error_code_name(status));
            en_controller_destroy(controller);
            return 1;
        }
        print_result(&result);
        successful_iterations++;
        write_status_json(status_output, &result);
        if (persist_state(state_filename, intent, &result) != 0) {
            en_controller_destroy(controller);
            return 1;
        }
    }
    en_controller_destroy(controller);
    if (count > 0 && iteration < count && !shutdown_requested()) {
        fprintf(stderr, "telemetry ended before requested batch count: %d/%d\n", iteration, count);
        return 1;
    }
    return successful_iterations == 0 ? 1 : 0;
}

#if !defined(_WIN32)
static int run_socket_parallel(int listener, const en_yaml_config_t *config, const en_intent_t *intent, long long max_age_ms,
    int batch_size, const char *backend, const char *swanctl_uri, const char *swanctl_config_file, const char *vppctl_socket, bool verify_swanctl, bool verify_vpp,
    bool apply, FILE *status_output, const char *state_filename, int max_records_per_second, long long allowed_uid, int accept_count,
    long long timeout_ms)
{
    if (accept_count < 2) return 2;
    struct pollfd *poll_fds = calloc((size_t)accept_count, sizeof(*poll_fds));
    FILE **client_inputs = calloc((size_t)accept_count, sizeof(*client_inputs));
    if (poll_fds == NULL || client_inputs == NULL) {
        free(poll_fds);
        free(client_inputs);
        return 1;
    }
    FILE *shared_input = tmpfile();
    if (shared_input == NULL) {
        perror("telemetry parallel batch");
        free(poll_fds);
        free(client_inputs);
        return 1;
    }
    int status = 0;
    int connected = 0;
    for (int index = 0; index < accept_count; index++) {
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            perror("telemetry parallel accept");
            status = 1;
            break;
        }
        if (allowed_uid >= 0) {
            struct ucred credentials = {0};
            socklen_t credentials_length = sizeof(credentials);
            if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0 ||
                credentials.uid != (uid_t)allowed_uid) {
                fprintf(stderr, "telemetry socket peer uid rejected\n");
                close(client);
                status = 2;
                break;
            }
        }
        client_inputs[index] = tmpfile();
        if (client_inputs[index] == NULL || fcntl(client, F_SETFL, O_NONBLOCK) != 0) {
            perror("telemetry parallel client");
            if (client_inputs[index] != NULL) fclose(client_inputs[index]);
            close(client);
            status = 1;
            break;
        }
        poll_fds[index].fd = client;
        poll_fds[index].events = POLLIN;
        connected++;
    }
    int active = status == 0 ? connected : 0;
    size_t total_bytes = 0;
    const size_t input_limit = 4U * 1024U * 1024U;
    int poll_timeout = timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms;
    while (status == 0 && active > 0) {
        int poll_status = poll(poll_fds, (nfds_t)connected, poll_timeout);
        if (poll_status < 0) {
            if (errno == EINTR) continue;
            perror("telemetry parallel poll");
            status = 1;
            break;
        }
        if (poll_status == 0) {
            fprintf(stderr, "telemetry parallel receive timed out after %lld ms\n", timeout_ms);
            status = 1;
            break;
        }
        for (int index = 0; index < connected; index++) {
            if (poll_fds[index].fd < 0 || poll_fds[index].revents == 0) continue;
            if ((poll_fds[index].revents & (POLLERR | POLLNVAL)) != 0) {
                status = 1;
                break;
            }
            if ((poll_fds[index].revents & (POLLIN | POLLHUP)) != 0) {
                char buffer[4096];
                ssize_t received = read(poll_fds[index].fd, buffer, sizeof(buffer));
                if (received > 0) {
                    if (total_bytes > input_limit - (size_t)received ||
                        fwrite(buffer, 1, (size_t)received, client_inputs[index]) != (size_t)received) {
                        fprintf(stderr, "telemetry parallel batch exceeds %zu bytes\n", input_limit);
                        status = 1;
                        break;
                    }
                    total_bytes += (size_t)received;
                } else if (received == 0) {
                    close(poll_fds[index].fd);
                    poll_fds[index].fd = -1;
                    active--;
                } else if (errno != EAGAIN && errno != EINTR) {
                    status = 1;
                    break;
                }
            }
        }
    }
    for (int index = 0; index < connected; index++) {
        if (poll_fds[index].fd >= 0) close(poll_fds[index].fd);
    }
    if (status == 0) {
        for (int index = 0; index < connected; index++) {
            char buffer[4096];
            rewind(client_inputs[index]);
            size_t received;
            while ((received = fread(buffer, 1, sizeof(buffer), client_inputs[index])) > 0) {
                if (fwrite(buffer, 1, received, shared_input) != received) {
                    status = 1;
                    break;
                }
            }
            if (ferror(client_inputs[index])) status = 1;
            fclose(client_inputs[index]);
            client_inputs[index] = NULL;
            if (status != 0) break;
        }
    }
    for (int index = 0; index < connected; index++) {
        if (client_inputs[index] != NULL) fclose(client_inputs[index]);
    }
    if (status == 0) {
        rewind(shared_input);
        status = run_stream(shared_input, config, intent, max_age_ms, 0, batch_size, backend, swanctl_uri, swanctl_config_file, vppctl_socket,
            verify_swanctl, verify_vpp, apply, status_output, state_filename, max_records_per_second);
    }
    fclose(shared_input);
    free(poll_fds);
    free(client_inputs);
    return status;
}
#endif

static int run_socket(const char *socket_path, const en_yaml_config_t *config, const en_intent_t *intent, long long max_age_ms, int count,
    int batch_size, const char *backend, const char *swanctl_uri, const char *swanctl_config_file, const char *vppctl_socket, bool verify_swanctl, bool verify_vpp, bool apply,
    FILE *status_output, const char *state_filename, int accept_count, int max_records_per_second, long long allowed_uid,
    int retry_count, long long retry_backoff_ms, bool parallel_socket, long long parallel_timeout_ms)
{
#if defined(_WIN32)
    (void)socket_path;
    (void)config;
    (void)intent;
    (void)max_age_ms;
    (void)count;
    (void)backend;
    (void)swanctl_uri;
    (void)swanctl_config_file;
    (void)vppctl_socket;
    (void)verify_swanctl;
    (void)verify_vpp;
    (void)apply;
    (void)status_output;
    (void)state_filename;
    (void)batch_size;
    (void)accept_count;
    (void)max_records_per_second;
    (void)allowed_uid;
    (void)retry_count;
    (void)retry_backoff_ms;
    (void)parallel_socket;
    (void)parallel_timeout_ms;
    fprintf(stderr, "--telemetry-socket is supported on Linux only\n");
    return 2;
#else
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "telemetry socket path is too long\n");
        return 2;
    }
    umask(0077);
    struct stat existing = {0};
    if (lstat(socket_path, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            fprintf(stderr, "telemetry socket path exists and is not a socket\n");
            return 2;
        }
        unlink(socket_path);
    }
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 1) != 0) {
        perror("telemetry socket setup");
        close(listener);
        unlink(socket_path);
        return 1;
    }
    printf("telemetry_socket: %s\n", socket_path);
    if (parallel_socket) {
        printf("socket_mode: parallel-shared-batch\n");
        int parallel_status = run_socket_parallel(listener, config, intent, max_age_ms, batch_size, backend, swanctl_uri,
            swanctl_config_file, vppctl_socket, verify_swanctl, verify_vpp, apply, status_output, state_filename, max_records_per_second,
            allowed_uid, accept_count, parallel_timeout_ms);
        close(listener);
        unlink(socket_path);
        return parallel_status;
    }
    bool shared_batch = accept_count > 1;
    const size_t shared_input_limit = 4U * 1024U * 1024U;
    size_t shared_input_bytes = 0;
    FILE *shared_input = shared_batch ? tmpfile() : NULL;
    if (shared_batch && shared_input == NULL) {
        perror("telemetry shared batch");
        close(listener);
        unlink(socket_path);
        return 1;
    }
    if (shared_batch) printf("socket_mode: shared-batch\n");
    fflush(stdout);
    int status = 0;
    int connection_count = 0;
    int reconnect_attempt = 0;
    while (!shutdown_signal_received && (accept_count == 0 || connection_count < accept_count)) {
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR && shutdown_signal_received) break;
            perror("telemetry socket accept");
            status = 1;
            break;
        }
        if (allowed_uid >= 0) {
            struct ucred credentials = {0};
            socklen_t credentials_length = sizeof(credentials);
            if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_length) != 0 ||
                credentials.uid != (uid_t)allowed_uid) {
                fprintf(stderr, "telemetry socket peer uid rejected\n");
                close(client);
                status = 2;
                break;
            }
        }
        FILE *input = fdopen(client, "r");
        if (input == NULL) {
            perror("fdopen");
            close(client);
            status = 1;
            break;
        }
        int stream_status = 0;
        if (shared_batch) {
            char line[1024];
            while (fgets(line, sizeof(line), input) != NULL) {
                size_t line_length = strlen(line);
                if (line_length > shared_input_limit - shared_input_bytes) {
                    fprintf(stderr, "telemetry shared batch exceeds %zu bytes\n", shared_input_limit);
                    stream_status = 1;
                    break;
                }
                if (fputs(line, shared_input) == EOF) {
                    stream_status = 1;
                    break;
                }
                shared_input_bytes += line_length;
            }
            if (ferror(input)) stream_status = 1;
        } else {
            stream_status = run_stream(input, config, intent, max_age_ms, count, batch_size, backend, swanctl_uri, swanctl_config_file, vppctl_socket, verify_swanctl, verify_vpp, apply, status_output, state_filename, max_records_per_second);
        }
        fclose(input);
        if (stream_status != 0) {
            if (reconnect_attempt < retry_count) {
                reconnect_attempt++;
                printf("socket_reconnect_retry: %d/%d\n", reconnect_attempt, retry_count);
                fflush(stdout);
                long long retry_delay = retry_backoff_ms;
                if (retry_delay > 0 && retry_delay > LLONG_MAX / reconnect_attempt) retry_delay = LLONG_MAX;
                else retry_delay *= reconnect_attempt;
                sleep_ms(retry_delay);
                continue;
            }
            status = stream_status;
            break;
        }
        connection_count++;
        reconnect_attempt = 0;
    }
    if (shared_input != NULL) {
        if (status == 0) {
            rewind(shared_input);
            status = run_stream(shared_input, config, intent, max_age_ms, 0, batch_size, backend, swanctl_uri, swanctl_config_file, vppctl_socket, verify_swanctl, verify_vpp, apply, status_output, state_filename, max_records_per_second);
        }
        fclose(shared_input);
    }
    close(listener);
    unlink(socket_path);
    return status;
#endif
}

int main(int argc, char **argv)
{
    const char *yaml_filename = "samples/linux-vm-netns.yaml";
    const char *telemetry_filename = NULL;
    const char *socket_path = NULL;
    const char *status_filename = NULL;
    const char *intent_id = NULL;
    const char *backend = "mock";
    const char *swanctl_uri = NULL;
    const char *swanctl_config_file = NULL;
    const char *vppctl_socket = NULL;
    const char *state_filename = NULL;
    const char *vici_monitor_child = NULL;
    const char *vici_monitor_path = NULL;
    bool verify_swanctl = false;
    bool verify_vpp = false;
    bool apply = false;
    bool stdin_mode = false;
    bool reload_config = false;
    bool reload_on_sighup = false;
    long long max_age_ms = 60000;
    long long interval_ms = 0;
    int count = 1;
    int batch_size = 1;
    int socket_accept_count = 1;
    int socket_retry_count = 0;
    int max_records_per_second = 0;
    long long socket_allowed_uid = -1;
    long long socket_retry_backoff_ms = 0;
    bool parallel_socket = false;
    long long parallel_timeout_ms = 30000;
    long long vici_monitor_duration_ms = 0;
    for (int index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--telemetry") == 0) {
            if (index + 1 >= argc) { fprintf(stderr, "missing --telemetry value\n"); return 2; }
            telemetry_filename = argv[++index];
        }
        else if (strcmp(argv[index], "--telemetry-socket") == 0) {
            if (index + 1 >= argc) { fprintf(stderr, "missing --telemetry-socket value\n"); return 2; }
            socket_path = argv[++index];
        }
        else if (strcmp(argv[index], "--status-jsonl") == 0) {
            if (index + 1 >= argc) { fprintf(stderr, "missing --status-jsonl value\n"); return 2; }
            status_filename = argv[++index];
        }
        else if (strcmp(argv[index], "--intent") == 0) {
            if (index + 1 >= argc) { fprintf(stderr, "missing --intent value\n"); return 2; }
            intent_id = argv[++index];
        }
        else if (strcmp(argv[index], "--max-age-ms") == 0) {
            if (index + 1 >= argc || !parse_long_long_argument(argv[++index], 0, LLONG_MAX, &max_age_ms)) { fprintf(stderr, "invalid --max-age-ms\n"); return 2; }
        }
        else if (strcmp(argv[index], "--interval-ms") == 0) {
            if (index + 1 >= argc || !parse_long_long_argument(argv[++index], 0, LLONG_MAX, &interval_ms)) { fprintf(stderr, "invalid --interval-ms\n"); return 2; }
        }
        else if (strcmp(argv[index], "--count") == 0) {
            if (index + 1 >= argc || !parse_int_argument(argv[++index], 0, INT_MAX, &count)) { fprintf(stderr, "invalid --count\n"); return 2; }
        }
        else if (strcmp(argv[index], "--batch-size") == 0) {
            if (index + 1 >= argc || !parse_int_argument(argv[++index], 1, EN_MAX_PATHS, &batch_size)) { fprintf(stderr, "invalid --batch-size\n"); return 2; }
        }
        else if (strcmp(argv[index], "--socket-accept-count") == 0) {
            if (index + 1 >= argc || !parse_int_argument(argv[++index], 0, INT_MAX, &socket_accept_count)) { fprintf(stderr, "invalid --socket-accept-count\n"); return 2; }
        }
        else if (strcmp(argv[index], "--socket-retry-count") == 0) {
            if (index + 1 >= argc || !parse_int_argument(argv[++index], 0, INT_MAX, &socket_retry_count)) { fprintf(stderr, "invalid --socket-retry-count\n"); return 2; }
        }
        else if (strcmp(argv[index], "--socket-retry-backoff-ms") == 0) {
            if (index + 1 >= argc || !parse_long_long_argument(argv[++index], 0, LLONG_MAX, &socket_retry_backoff_ms)) { fprintf(stderr, "invalid --socket-retry-backoff-ms\n"); return 2; }
        }
        else if (strcmp(argv[index], "--max-records-per-second") == 0) {
            if (index + 1 >= argc || !parse_int_argument(argv[++index], 0, INT_MAX, &max_records_per_second)) { fprintf(stderr, "invalid --max-records-per-second\n"); return 2; }
        }
        else if (strcmp(argv[index], "--socket-uid") == 0) {
            if (index + 1 >= argc || !parse_long_long_argument(argv[++index], -1, LLONG_MAX, &socket_allowed_uid)) { fprintf(stderr, "invalid --socket-uid\n"); return 2; }
        }
        else if (strcmp(argv[index], "--socket-parallel") == 0) parallel_socket = true;
        else if (strcmp(argv[index], "--socket-parallel-timeout-ms") == 0) {
            if (index + 1 >= argc || !parse_long_long_argument(argv[++index], 1, LLONG_MAX, &parallel_timeout_ms)) { fprintf(stderr, "invalid --socket-parallel-timeout-ms\n"); return 2; }
        }
        else if (strcmp(argv[index], "--backend") == 0) {
            if (index + 1 >= argc) { fprintf(stderr, "missing --backend value\n"); return 2; }
            backend = argv[++index];
        }
        else if (strcmp(argv[index], "--swanctl-uri") == 0) {
            if (index + 1 >= argc || !valid_option_string(argv[index + 1], 256)) { fprintf(stderr, "invalid --swanctl-uri\n"); return 2; }
            swanctl_uri = argv[++index];
        }
        else if (strcmp(argv[index], "--swanctl-config") == 0) {
            if (index + 1 >= argc || !valid_option_string(argv[index + 1], 256)) { fprintf(stderr, "invalid --swanctl-config\n"); return 2; }
            swanctl_config_file = argv[++index];
        }
        else if (strcmp(argv[index], "--vppctl-socket") == 0) {
            if (index + 1 >= argc || !valid_option_string(argv[index + 1], 256)) { fprintf(stderr, "invalid --vppctl-socket\n"); return 2; }
            vppctl_socket = argv[++index];
        }
        else if (strcmp(argv[index], "--vici-monitor-child") == 0) {
            if (index + 1 >= argc || !valid_option_string(argv[index + 1], 256)) { fprintf(stderr, "invalid --vici-monitor-child\n"); return 2; }
            vici_monitor_child = argv[++index];
        }
        else if (strcmp(argv[index], "--vici-monitor-path") == 0) {
            if (index + 1 >= argc || !valid_option_string(argv[index + 1], 256)) { fprintf(stderr, "invalid --vici-monitor-path\n"); return 2; }
            vici_monitor_path = argv[++index];
        }
        else if (strcmp(argv[index], "--vici-monitor-duration-ms") == 0) {
            if (index + 1 >= argc || !parse_long_long_argument(argv[++index], 0, LLONG_MAX, &vici_monitor_duration_ms)) { fprintf(stderr, "invalid --vici-monitor-duration-ms\n"); return 2; }
        }
        else if (strcmp(argv[index], "--verify-swanctl") == 0) verify_swanctl = true;
        else if (strcmp(argv[index], "--verify-vpp") == 0) verify_vpp = true;
        else if (strcmp(argv[index], "--state-file") == 0) {
            if (index + 1 >= argc) { fprintf(stderr, "missing --state-file value\n"); return 2; }
            state_filename = argv[++index];
        }
        else if (strcmp(argv[index], "--apply") == 0) apply = true;
        else if (strcmp(argv[index], "--telemetry-stdin") == 0) stdin_mode = true;
        else if (strcmp(argv[index], "--reload-config") == 0) reload_config = true;
        else if (strcmp(argv[index], "--reload-on-sighup") == 0) reload_on_sighup = true;
        else if (strcmp(argv[index], "--once") == 0) count = 1;
        else if (strcmp(argv[index], "--help") == 0) {
            printf("usage: %s YAML [--telemetry FILE|--telemetry-stdin|--telemetry-socket PATH] [--status-jsonl FILE] [--state-file FILE] [--intent ID] [--backend mock|command] [--swanctl-uri URI] [--swanctl-config FILE] [--vppctl-socket PATH] [--vici-monitor-child CHILD] [--vici-monitor-path PATH] [--vici-monitor-duration-ms N] [--verify-swanctl] [--verify-vpp] [--batch-size N] [--socket-accept-count N] [--socket-parallel] [--socket-parallel-timeout-ms N] [--socket-retry-count N] [--socket-retry-backoff-ms N] [--socket-uid UID] [--max-records-per-second N] [--reload-config] [--reload-on-sighup] [--apply] [--interval-ms N] [--count N] [--max-age-ms N]\n", argv[0]);
            return 0;
        } else yaml_filename = argv[index];
    }
    if ((!stdin_mode && telemetry_filename == NULL && socket_path == NULL) ||
        (stdin_mode && (telemetry_filename != NULL || socket_path != NULL)) ||
        (telemetry_filename != NULL && socket_path != NULL) || count < 0 || batch_size <= 0 || batch_size > EN_MAX_PATHS || interval_ms < 0 || max_age_ms < 0 || socket_accept_count < 0 || socket_retry_count < 0 || socket_retry_backoff_ms < 0 || max_records_per_second < 0 || socket_allowed_uid < -1 ||
        ((socket_accept_count == 0 || socket_accept_count > 1) && state_filename == NULL) || (reload_config && (stdin_mode || socket_path != NULL || state_filename == NULL)) ||
        (reload_on_sighup && (!reload_config || stdin_mode || socket_path != NULL)) || parallel_timeout_ms < 1 ||
        (parallel_socket && (socket_path == NULL || socket_accept_count < 2)) ||
        ((vici_monitor_child == NULL) != (vici_monitor_path == NULL)) ||
        (vici_monitor_child != NULL && (stdin_mode || telemetry_filename != NULL || socket_path != NULL || reload_config))) {
        fprintf(stderr, "choose one telemetry input and use non-negative numeric options\n");
        return 2;
    }
#if defined(_WIN32)
    if (reload_on_sighup) {
        fprintf(stderr, "--reload-on-sighup is supported on Linux only\n");
        return 2;
    }
#else
    if (reload_on_sighup && install_signal_handler(SIGHUP, handle_reload_signal) != 0) {
        fprintf(stderr, "failed to install SIGHUP handler\n");
        return 1;
    }
    if (install_signal_handler(SIGINT, handle_shutdown_signal) != 0 || install_signal_handler(SIGTERM, handle_shutdown_signal) != 0) {
        fprintf(stderr, "failed to install shutdown handler\n");
        return 1;
    }
#endif
    en_yaml_config_t config = {0};
    char error[256] = {0};
    if (en_yaml_config_load_file(yaml_filename, &config, error, sizeof(error)) != EN_ERR_NONE) {
        fprintf(stderr, "yaml load failed: %s\n", error);
        return 1;
    }
    const en_intent_t *intent = find_intent(&config, intent_id);
    if (intent == NULL) {
        fprintf(stderr, "intent not found\n");
        return 1;
    }
    FILE *status_output = NULL;
    if (status_filename != NULL) {
        status_output = open_status_output(status_filename);
        if (status_output == NULL) {
            fprintf(stderr, "status output open failed: %s\n", status_filename);
            return 1;
        }
    }
#if defined(EVENTNET_ENABLE_STRONGSWAN_VICI)
    if (vici_monitor_child != NULL) {
        int status = run_vici_monitor(&config, intent, backend, swanctl_uri, vppctl_socket, verify_vpp, apply,
            status_output, state_filename, vici_monitor_child, vici_monitor_path, vici_monitor_duration_ms);
        if (status_output != NULL) fclose(status_output);
        return status;
    }
#else
    if (vici_monitor_child != NULL) {
        fprintf(stderr, "VICI monitor requires EVENTNET_ENABLE_STRONGSWAN_VICI build\n");
        if (status_output != NULL) fclose(status_output);
        return 2;
    }
#endif
    if (stdin_mode) {
        int status = run_stream(stdin, &config, intent, max_age_ms, count, batch_size, backend, swanctl_uri, swanctl_config_file, vppctl_socket, verify_swanctl, verify_vpp, apply, status_output, state_filename, max_records_per_second);
        if (status_output != NULL) fclose(status_output);
        return status;
    }
    if (socket_path != NULL) {
        int status = run_socket(socket_path, &config, intent, max_age_ms, count, batch_size, backend, swanctl_uri, swanctl_config_file, vppctl_socket, verify_swanctl, verify_vpp, apply, status_output, state_filename, socket_accept_count, max_records_per_second, socket_allowed_uid, socket_retry_count, socket_retry_backoff_ms, parallel_socket, parallel_timeout_ms);
        if (status_output != NULL) fclose(status_output);
        return status;
    }
    if (reload_config) {
        en_yaml_config_t active_config = config;
        if (reload_on_sighup) {
            printf("sighup_reload: waiting\n");
            fflush(stdout);
        }
        for (int iteration = 0; (reload_on_sighup ? (count == 0 || iteration < count) : iteration < count) && !shutdown_requested(); iteration++) {
            if (reload_on_sighup) {
#if !defined(_WIN32)
                while (!reload_signal_received) sleep_ms(100);
                reload_signal_received = 0;
#endif
            } else if (iteration > 0) sleep_ms(interval_ms);
            en_yaml_config_t reloaded_config = {0};
            char reload_error[256] = {0};
            if (en_yaml_config_load_file(yaml_filename, &reloaded_config, reload_error, sizeof(reload_error)) == EN_ERR_NONE) {
                active_config = reloaded_config;
                printf("config_reload: applied\n");
            } else {
                fprintf(stderr, "yaml reload failed; keeping previous configuration: %s\n", reload_error);
            }
            const en_intent_t *reloaded_intent = find_intent(&active_config, intent_id);
            if (reloaded_intent == NULL) {
                fprintf(stderr, "intent not found in active yaml configuration\n");
                if (status_output != NULL) fclose(status_output);
                return 1;
            }
            FILE *reloaded_input = en_telemetry_open_jsonl(telemetry_filename);
            if (reloaded_input == NULL) {
                fprintf(stderr, "telemetry open failed: %s\n", telemetry_filename);
                if (status_output != NULL) fclose(status_output);
                return 1;
            }
            int reload_status = run_stream(reloaded_input, &active_config, reloaded_intent, max_age_ms, 1, batch_size, backend,
                swanctl_uri, swanctl_config_file, vppctl_socket, verify_swanctl, verify_vpp, apply, status_output, state_filename, max_records_per_second);
            fclose(reloaded_input);
            if (reload_status != 0) {
                if (status_output != NULL) fclose(status_output);
                return reload_status;
            }
        }
        if (status_output != NULL) fclose(status_output);
        return 0;
    }
    if (telemetry_filename != NULL) {
        FILE *telemetry_input = en_telemetry_open_jsonl(telemetry_filename);
        if (telemetry_input == NULL) {
            fprintf(stderr, "telemetry open failed: %s\n", telemetry_filename);
            if (status_output != NULL) fclose(status_output);
            return 1;
        }
        int stream_status = run_stream(telemetry_input, &config, intent, max_age_ms, count, batch_size,
            backend, swanctl_uri, swanctl_config_file, vppctl_socket, verify_swanctl, verify_vpp, apply,
            status_output, state_filename, max_records_per_second);
        fclose(telemetry_input);
        if (status_output != NULL) fclose(status_output);
        return stream_status;
    }
    en_health_probe_mock_t health_mock = {0};
    en_vpp_mock_t vpp_mock = {0};
    en_strongswan_command_ctx_t strongswan_command = {0};
    en_vpp_command_ctx_t vpp_command = {0};
    strongswan_command.dry_run = !apply;
    strongswan_command.verify_tunnel = verify_swanctl;
    strongswan_command.block_non_ipsec = intent->block_non_ipsec;
    if (swanctl_uri != NULL) snprintf(strongswan_command.swanctl_uri, sizeof(strongswan_command.swanctl_uri), "%s", swanctl_uri);
    if (swanctl_config_file != NULL) snprintf(strongswan_command.swanctl_config_file, sizeof(strongswan_command.swanctl_config_file), "%s", swanctl_config_file);
    vpp_command.dry_run = !apply;
    vpp_command.verify_route = verify_vpp;
    vpp_command.edges = config.vpp_edges;
    vpp_command.edge_count = config.vpp_edge_count;
    vpp_command.has_vlan_id = intent->traffic.has_vlan_id;
    vpp_command.deny_unmatched_vlan = intent->deny_unmatched_vlan;
    vpp_command.vlan_id = intent->traffic.vlan_id;
    if (vppctl_socket != NULL) snprintf(vpp_command.vppctl_socket, sizeof(vpp_command.vppctl_socket), "%s", vppctl_socket);
    vpp_command.tunnels = config.tunnels;
    vpp_command.tunnel_count = config.tunnel_count;
    en_strongswan_adapter_t strongswan_adapter = en_strongswan_mock_adapter();
    en_vpp_adapter_t vpp_adapter = en_vpp_mock_adapter(&vpp_mock);
    if (strcmp(backend, "command") == 0) {
        strongswan_adapter = en_strongswan_command_adapter(&strongswan_command);
        vpp_adapter = en_vpp_command_adapter(&vpp_command);
    } else if (strcmp(backend, "mock") != 0) {
        fprintf(stderr, "unknown backend: %s\n", backend);
        return 2;
    }
    printf("backend: %s (%s)\n", backend, apply ? "apply" : "dry-run");
    en_controller_t *controller = en_controller_create_with_nodes_and_tunnels(config.nodes, config.node_count, config.paths, config.path_count, config.tunnels, config.tunnel_count,
        strongswan_adapter, vpp_adapter, en_health_probe_mock_adapter(&health_mock));
    if (controller == NULL) {
        fprintf(stderr, "controller create failed\n");
        return 1;
    }
    if (restore_state(controller, intent, state_filename) != 0) {
        en_controller_destroy(controller);
        if (status_output != NULL) fclose(status_output);
        return 1;
    }
    for (int iteration = 0; iteration < count && !shutdown_requested(); iteration++) {
        printf("eventnetd_iteration: %d/%d\n", iteration + 1, count);
        if (max_records_per_second > 0 && iteration > 0) sleep_ms(1000 / max_records_per_second);
        if (load_health(telemetry_filename, max_age_ms, &config, intent, &health_mock) != 0) {
            en_controller_destroy(controller);
            return 1;
        }
        en_reconcile_result_t result = {0};
        en_error_code_t status = en_controller_submit_intent(controller, intent, &result);
        if (status != EN_ERR_NONE) {
            fprintf(stderr, "intent failed: %s\n", en_error_code_name(status));
            en_controller_destroy(controller);
            return 1;
        }
        print_result(&result);
        write_status_json(status_output, &result);
        if (persist_state(state_filename, intent, &result) != 0) {
            en_controller_destroy(controller);
            if (status_output != NULL) fclose(status_output);
            return 1;
        }
        if (iteration + 1 < count) sleep_ms(interval_ms);
    }
    en_controller_destroy(controller);
    if (status_output != NULL) fclose(status_output);
    return 0;
}
