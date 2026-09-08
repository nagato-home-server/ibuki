#include "eventnet/controller.h"
#include "eventnet/mock_adapters.h"
#include "eventnet/strongswan_vici_adapter.h"
#include "eventnet/strongswan_vici_client.h"
#include "eventnet/telemetry.h"
#include "eventnet/yaml_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

static const en_intent_t *find_intent(const en_yaml_config_t *config, const char *intent_id)
{
    if (config == NULL) return NULL;
    for (size_t index = 0; index < config->intent_count; index++) {
        if (intent_id == NULL || strcmp(config->intents[index].intent_id, intent_id) == 0) return &config->intents[index];
    }
    return NULL;
}

typedef struct {
    en_controller_t *controller;
    const en_intent_t *intent;
    en_health_probe_mock_t *health;
    int event_count;
    int failure_count;
} monitor_context_t;

static void monitor_event(void *context, const char *event_json)
{
    monitor_context_t *monitor = context;
    if (monitor == NULL || event_json == NULL) return;
    en_path_health_t health = {0};
    char error[256] = {0};
    if (en_telemetry_parse_json_line(event_json, &health, error, sizeof(error)) != EN_ERR_NONE) {
        fprintf(stderr, "VICI event parse failed: %s\n", error);
        monitor->failure_count++;
        return;
    }
    en_health_probe_mock_set(monitor->health, health);
    en_reconcile_result_t result = {0};
    en_error_code_t status = en_controller_submit_intent(monitor->controller, monitor->intent, &result);
    if (status != EN_ERR_NONE) {
        fprintf(stderr, "VICI event reconcile failed: %s\n", en_error_code_name(status));
        monitor->failure_count++;
        return;
    }
    monitor->event_count++;
    printf("vici_event: path=%s state=%s selected=%s transition=%s\n",
        health.path_id, en_health_state_name(health.state), result.selected_path,
        en_transition_state_name(result.transition_state));
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 8 || (argc > 4 && (argc != 8 || strcmp(argv[4], "--monitor") != 0))) {
        fprintf(stderr, "usage: %s VICI_URI YAML [INTENT_ID] [--monitor CHILD_ID PATH_ID DURATION_MS]\n", argv[0]);
        return 2;
    }
    en_yaml_config_t config = {0};
    char error[256] = {0};
    if (en_yaml_config_load_file(argv[2], &config, error, sizeof(error)) != EN_ERR_NONE) {
        fprintf(stderr, "yaml load failed: %s\n", error);
        return 1;
    }
    const en_intent_t *intent = find_intent(&config, argc == 4 ? argv[3] : NULL);
    if (intent == NULL) {
        fprintf(stderr, "intent not found\n");
        return 1;
    }
    en_strongswan_vici_client_t *client = NULL;
    if (en_strongswan_vici_client_open(&client, argv[1], error, sizeof(error)) != EN_ERR_NONE) {
        fprintf(stderr, "VICI connection failed: %s\n", error);
        return 1;
    }
    en_strongswan_vici_ctx_t vici_context = {0};
    if (en_strongswan_vici_bind_client(&vici_context, client) != EN_ERR_NONE) {
        fprintf(stderr, "VICI client binding failed\n");
        en_strongswan_vici_client_close(client);
        return 1;
    }
    en_health_probe_mock_t health = {0};
    en_vpp_mock_t vpp = {0};
    en_controller_t *controller = en_controller_create_with_nodes_and_tunnels(
        config.nodes, config.node_count, config.paths, config.path_count,
        config.tunnels, config.tunnel_count, en_strongswan_vici_adapter(&vici_context),
        en_vpp_mock_adapter(&vpp), en_health_probe_mock_adapter(&health));
    if (controller == NULL) {
        fprintf(stderr, "controller creation failed\n");
        en_strongswan_vici_client_close(client);
        return 1;
    }
    en_reconcile_result_t result = {0};
    en_error_code_t status = en_controller_submit_intent(controller, intent, &result);
    printf("intent: %s\nselected_path: %s\ntransition_state: %s\nreason: %s\n",
        result.intent_id, result.selected_path, en_transition_state_name(result.transition_state), result.explanation.reason);
    if (status == EN_ERR_NONE && argc == 8) {
        char *end = NULL;
        errno = 0;
        long long duration_ms = strtoll(argv[7], &end, 10);
        if (errno == ERANGE || end == argv[7] || *end != '\0' || duration_ms < 0 || duration_ms > LLONG_MAX) {
            fprintf(stderr, "invalid monitor duration\n");
            status = EN_ERR_INVALID_ARGUMENT;
        } else {
            monitor_context_t monitor = {
                .controller = controller,
                .intent = intent,
                .health = &health,
            };
            if (en_strongswan_vici_client_monitor_child(client, argv[5], argv[6], duration_ms,
                    monitor_event, &monitor, error, sizeof(error)) != EN_ERR_NONE) {
                fprintf(stderr, "VICI monitor failed: %s\n", error);
                status = EN_ERR_STATE_CONFLICT;
            } else {
                printf("VICI event monitor completed: events=%d failures=%d\n",
                    monitor.event_count, monitor.failure_count);
                if (monitor.failure_count != 0) status = EN_ERR_STATE_CONFLICT;
            }
        }
    }
    en_controller_destroy(controller);
    en_strongswan_vici_client_close(client);
    if (status != EN_ERR_NONE) {
        fprintf(stderr, "controller reconcile failed: %s\n", en_error_code_name(status));
        return 1;
    }
    printf("VICI controller reconcile passed\n");
    return 0;
}
