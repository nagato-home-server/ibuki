#include "eventnet/apply_plan.h"
#include "eventnet/controller.h"
#include "eventnet/command_adapters.h"
#include "eventnet/telemetry.h"
#include "eventnet/strongswan_observer.h"
#include "eventnet/vpp_observer.h"
#include "eventnet/vpp_api_adapter.h"
#include "eventnet/vpp_api_transport.h"
#include "eventnet/strongswan_vici_adapter.h"
#include "eventnet/mock_adapters.h"
#include "eventnet/render_commands.h"
#include "eventnet/topology.h"
#include "eventnet/yaml_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#define ASSERT_TRUE(expr) do { if (!(expr)) { fprintf(stderr, "assert failed: %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)
#define ASSERT_STREQ(left, right) do { \
    const char *actual_value = (left); \
    const char *expected_value = (right); \
    if (actual_value == NULL || expected_value == NULL || strcmp(actual_value, expected_value) != 0) { \
        fprintf(stderr, "string assertion failed: %s:%d: actual='%s' expected='%s'\\n", __FILE__, __LINE__, actual_value == NULL ? "(null)" : actual_value, expected_value == NULL ? "(null)" : expected_value); \
        exit(1); \
    } \
} while (0)

static void make_test_file_private(const char *filename)
{
#if !defined(_WIN32)
    (void)chmod(filename, 0600);
#else
    (void)filename;
#endif
}

typedef struct {
    int install_count;
    int remove_count;
    int observe_count;
} vpp_api_test_state_t;

typedef struct {
    int ensure_count;
    int remove_count;
    int observe_count;
} vici_test_state_t;

typedef struct {
    int add_route_count;
    int remove_route_count;
    int create_vlan_count;
    int remove_vlan_count;
    int ensure_vrf_count;
    int fail_add_route_at;
    char order[32];
    size_t order_length;
} vpp_message_test_state_t;

static void vpp_message_record(vpp_message_test_state_t *state, char marker)
{
    if (state->order_length + 1 < sizeof(state->order)) state->order[state->order_length++] = marker;
    state->order[state->order_length] = '\0';
}

static en_error_code_t vpp_message_add_route(void *ctx, const en_route_t *route)
{
    vpp_message_test_state_t *state = ctx;
    ASSERT_TRUE(state != NULL && route != NULL);
    state->add_route_count++;
    vpp_message_record(state, 'A');
    if (state->fail_add_route_at > 0 && state->add_route_count == state->fail_add_route_at) return EN_ERR_FORWARDING_UPDATE_FAILED;
    return EN_ERR_NONE;
}

static en_error_code_t vpp_message_remove_route(void *ctx, const en_route_t *route)
{
    vpp_message_test_state_t *state = ctx;
    ASSERT_TRUE(state != NULL && route != NULL);
    state->remove_route_count++;
    vpp_message_record(state, 'R');
    return EN_ERR_NONE;
}

static en_error_code_t vpp_message_create_vlan(void *ctx, const en_vpp_edge_t *edge, int vlan_id)
{
    vpp_message_test_state_t *state = ctx;
    ASSERT_TRUE(state != NULL && edge != NULL && vlan_id == 100);
    state->create_vlan_count++;
    vpp_message_record(state, 'V');
    return EN_ERR_NONE;
}

static en_error_code_t vpp_message_remove_vlan(void *ctx, const en_vpp_edge_t *edge, int vlan_id)
{
    vpp_message_test_state_t *state = ctx;
    ASSERT_TRUE(state != NULL && edge != NULL && vlan_id == 100);
    state->remove_vlan_count++;
    vpp_message_record(state, 'v');
    return EN_ERR_NONE;
}

static en_error_code_t vpp_message_ensure_vrf(void *ctx, int table_id)
{
    vpp_message_test_state_t *state = ctx;
    ASSERT_TRUE(state != NULL && table_id == 100);
    state->ensure_vrf_count++;
    vpp_message_record(state, 'F');
    return EN_ERR_NONE;
}

static en_error_code_t vpp_message_set_vlan_table(void *ctx, const en_vpp_edge_t *edge, int vlan_id, int table_id)
{
    vpp_message_test_state_t *state = ctx;
    ASSERT_TRUE(state != NULL && edge != NULL && vlan_id == 100 && table_id == 100);
    vpp_message_record(state, 'T');
    return EN_ERR_NONE;
}

static en_error_code_t vici_test_ensure(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    vici_test_state_t *state = ctx;
    ASSERT_TRUE(desired != NULL && observed != NULL);
    *observed = *desired;
    state->ensure_count++;
    return EN_ERR_NONE;
}

static en_error_code_t vici_test_remove(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    vici_test_state_t *state = ctx;
    ASSERT_TRUE(desired != NULL && observed != NULL);
    *observed = *desired;
    state->remove_count++;
    return EN_ERR_NONE;
}

static en_error_code_t vpp_api_test_install(void *ctx, const char *traffic_key, const en_path_t *path)
{
    vpp_api_test_state_t *state = ctx;
    ASSERT_TRUE(traffic_key != NULL && path != NULL);
    state->install_count++;
    return EN_ERR_NONE;
}

static en_error_code_t vpp_api_test_remove(void *ctx, const char *traffic_key, const en_path_t *path)
{
    vpp_api_test_state_t *state = ctx;
    ASSERT_TRUE(traffic_key != NULL && path != NULL);
    state->remove_count++;
    return EN_ERR_NONE;
}

static const char *vpp_api_test_active(void *ctx, const char *traffic_key)
{
    (void)ctx;
    (void)traffic_key;
    return "path-api";
}

static en_error_code_t vpp_api_test_observe(void *ctx, const char *destination_prefix, en_vpp_route_observation_t *observation)
{
    vpp_api_test_state_t *state = ctx;
    ASSERT_TRUE(destination_prefix != NULL && observation != NULL);
    snprintf(observation->destination_prefix, sizeof(observation->destination_prefix), "%s", destination_prefix);
    snprintf(observation->next_hop, sizeof(observation->next_hop), "%s", "198.51.100.1");
    observation->present = true;
    state->observe_count++;
    return EN_ERR_NONE;
}

static en_error_code_t vpp_api_bad_observe(void *ctx, const char *destination_prefix, en_vpp_route_observation_t *observation)
{
    (void)ctx;
    (void)destination_prefix;
    ASSERT_TRUE(observation != NULL);
    snprintf(observation->destination_prefix, sizeof(observation->destination_prefix), "%s", "10.10.9.0/24");
    observation->present = true;
    return EN_ERR_NONE;
}

static en_error_code_t vpp_api_bad_value_observe(void *ctx, const char *destination_prefix, en_vpp_route_observation_t *observation)
{
    (void)ctx;
    ASSERT_TRUE(destination_prefix != NULL && observation != NULL);
    snprintf(observation->destination_prefix, sizeof(observation->destination_prefix), "%s", destination_prefix);
    snprintf(observation->next_hop, sizeof(observation->next_hop), "%s", "198.51.100.1;drop");
    observation->present = true;
    return EN_ERR_NONE;
}

static en_error_code_t vpp_api_test_observe_interface(void *ctx, const char *interface_name, en_vpp_interface_observation_t *observation)
{
    (void)ctx;
    ASSERT_TRUE(interface_name != NULL && observation != NULL);
    snprintf(observation->interface_name, sizeof(observation->interface_name), "%s", interface_name);
    observation->present = true;
    observation->up = true;
    return EN_ERR_NONE;
}

static en_error_code_t vpp_api_bad_interface_observe(void *ctx, const char *interface_name, en_vpp_interface_observation_t *observation)
{
    (void)ctx;
    (void)interface_name;
    ASSERT_TRUE(observation != NULL);
    snprintf(observation->interface_name, sizeof(observation->interface_name), "%s", "other-if");
    observation->present = true;
    return EN_ERR_NONE;
}

static en_error_code_t vici_api_bad_ensure(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    (void)ctx;
    ASSERT_TRUE(desired != NULL && observed != NULL);
    snprintf(observed->tunnel_id, sizeof(observed->tunnel_id), "%s", "tun-wrong");
    return EN_ERR_NONE;
}

static en_error_code_t vici_api_bad_observe(void *ctx, const char *child_id, en_strongswan_sa_observation_t *observation)
{
    (void)ctx;
    (void)child_id;
    ASSERT_TRUE(observation != NULL);
    snprintf(observation->child_id, sizeof(observation->child_id), "%s", "tun-wrong");
    return EN_ERR_NONE;
}

static en_error_code_t vici_test_observe(void *ctx, const char *child_id, en_strongswan_sa_observation_t *observation)
{
    vici_test_state_t *state = ctx;
    ASSERT_TRUE(child_id != NULL && observation != NULL);
    snprintf(observation->child_id, sizeof(observation->child_id), "%s", child_id);
    observation->state = EN_TUNNEL_ESTABLISHED;
    observation->health = EN_HEALTH_HEALTHY;
    state->observe_count++;
    return EN_ERR_NONE;
}

static en_controller_t *make_controller(en_vpp_mock_t *vpp_mock, en_health_probe_mock_t *health_mock)
{
    en_path_t paths[EN_MAX_PATHS];
    size_t path_count = en_initial_demo_paths(paths, EN_MAX_PATHS);
    en_controller_t *controller = en_controller_create(
        paths,
        path_count,
        en_strongswan_mock_adapter(),
        en_vpp_mock_adapter(vpp_mock),
        en_health_probe_mock_adapter(health_mock)
    );
    ASSERT_TRUE(controller != NULL);
    return controller;
}

static en_intent_t base_intent(en_path_selection_mode_t mode)
{
    en_intent_t intent = {0};
    snprintf(intent.intent_id, sizeof(intent.intent_id), "%s", "intent-a-b");
    snprintf(intent.traffic.source, sizeof(intent.traffic.source), "%s", "site-a");
    snprintf(intent.traffic.destination, sizeof(intent.traffic.destination), "%s", "site-b");
    intent.path_selection.mode = mode;
    intent.transition.strategy = EN_TRANSITION_IMMEDIATE;
    intent.transition.max_pause_ms = 0;
    intent.transition.drain_timeout_ms = 0;
    intent.transition.timeout_ms = 5000;
    intent.fallback.enabled = true;
    snprintf(intent.fallback.path_id, sizeof(intent.fallback.path_id), "%s", "path-via-hub");
    return intent;
}

static void test_priority_selects_first_healthy_path(void)
{
    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = make_controller(&vpp_mock, &health_mock);
    en_intent_t intent = base_intent(EN_SELECT_PRIORITY);
    intent.path_selection.candidate_count = 2;
    snprintf(intent.path_selection.candidates[0], sizeof(intent.path_selection.candidates[0]), "%s", "path-direct");
    snprintf(intent.path_selection.candidates[1], sizeof(intent.path_selection.candidates[1]), "%s", "path-via-hub");

    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-direct");

    en_controller_destroy(controller);
}

static void test_evaluated_excludes_failed_path(void)
{
    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_path_health_t failed = {0};
    snprintf(failed.path_id, sizeof(failed.path_id), "%s", "path-direct");
    failed.state = EN_HEALTH_FAILED;
    en_health_probe_mock_set(&health_mock, failed);

    en_controller_t *controller = make_controller(&vpp_mock, &health_mock);
    en_intent_t intent = base_intent(EN_SELECT_EVALUATED);
    intent.path_selection.candidate_count = 2;
    snprintf(intent.path_selection.candidates[0], sizeof(intent.path_selection.candidates[0]), "%s", "path-direct");
    snprintf(intent.path_selection.candidates[1], sizeof(intent.path_selection.candidates[1]), "%s", "path-via-relay-c");
    intent.path_selection.comparison_count = 2;
    intent.path_selection.comparison_order[0] = EN_COMPARE_HOP_COUNT;
    intent.path_selection.comparison_order[1] = EN_COMPARE_PATH_ID;

    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-via-relay-c");
    ASSERT_TRUE(result.explanation.excluded_count == 1);
    ASSERT_STREQ(result.explanation.excluded_path_ids[0], "path-direct");

    en_controller_destroy(controller);
}

static void test_failed_forwarding_rolls_back_to_previous_path(void)
{
    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = make_controller(&vpp_mock, &health_mock);

    en_intent_t first = base_intent(EN_SELECT_EXPLICIT);
    snprintf(first.path_selection.path_id, sizeof(first.path_selection.path_id), "%s", "path-via-hub");
    en_reconcile_result_t first_result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &first, &first_result) == EN_ERR_NONE);

    en_intent_t second = base_intent(EN_SELECT_EXPLICIT);
    snprintf(second.intent_id, sizeof(second.intent_id), "%s", "intent-a-b-2");
    snprintf(second.path_selection.path_id, sizeof(second.path_selection.path_id), "%s", "path-direct");
    vpp_mock.fail_next_update = true;
    en_reconcile_result_t second_result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &second, &second_result) == EN_ERR_FORWARDING_UPDATE_FAILED);

    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&first.traffic, traffic_key, sizeof(traffic_key));
    ASSERT_STREQ(en_controller_applied_path(controller, traffic_key), "path-via-hub");

    en_controller_destroy(controller);
}

static void test_successful_switch_removes_previous_path(void)
{
    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = make_controller(&vpp_mock, &health_mock);

    en_intent_t first = base_intent(EN_SELECT_EXPLICIT);
    snprintf(first.path_selection.path_id, sizeof(first.path_selection.path_id), "%s", "path-direct");
    en_reconcile_result_t first_result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &first, &first_result) == EN_ERR_NONE);

    en_intent_t second = first;
    snprintf(second.intent_id, sizeof(second.intent_id), "%s", "intent-a-b-switch");
    snprintf(second.path_selection.path_id, sizeof(second.path_selection.path_id), "%s", "path-via-hub");
    en_reconcile_result_t second_result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &second, &second_result) == EN_ERR_NONE);
    ASSERT_TRUE(vpp_mock.remove_count == 1);
    ASSERT_STREQ(vpp_mock.active_paths[0], "path-via-hub");
    en_controller_destroy(controller);
}

static void test_forwarding_failure_retries_before_rollback(void)
{
    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = make_controller(&vpp_mock, &health_mock);
    en_intent_t intent = base_intent(EN_SELECT_EXPLICIT);
    snprintf(intent.path_selection.path_id, sizeof(intent.path_selection.path_id), "%s", "path-direct");
    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    intent.transition.retry_count = 1;
    intent.transition.retry_backoff_ms = 0;
    snprintf(intent.path_selection.path_id, sizeof(intent.path_selection.path_id), "%s", "path-via-hub");
    vpp_mock.fail_next_update = true;
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-via-hub");
    ASSERT_TRUE(vpp_mock.install_count >= 2);
    en_controller_destroy(controller);
}

static void test_graceful_switch_drains_previous_path(void)
{
    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = make_controller(&vpp_mock, &health_mock);

    en_intent_t first = base_intent(EN_SELECT_EXPLICIT);
    snprintf(first.path_selection.path_id, sizeof(first.path_selection.path_id), "%s", "path-direct");
    en_reconcile_result_t first_result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &first, &first_result) == EN_ERR_NONE);

    en_intent_t second = first;
    snprintf(second.intent_id, sizeof(second.intent_id), "%s", "intent-a-b-graceful");
    snprintf(second.path_selection.path_id, sizeof(second.path_selection.path_id), "%s", "path-via-hub");
    second.transition.strategy = EN_TRANSITION_GRACEFUL;
    second.transition.max_pause_ms = 1;
    second.transition.drain_timeout_ms = 1;
    en_reconcile_result_t second_result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &second, &second_result) == EN_ERR_NONE);
    ASSERT_TRUE(vpp_mock.remove_count == 1);

    const en_audit_event_t *events = NULL;
    size_t event_count = en_controller_audit_events(controller, &events);
    bool saw_draining = false;
    for (size_t index = 0; index < event_count; index++) {
        if (strcmp(events[index].event_type, "DRAINING") == 0) saw_draining = true;
    }
    ASSERT_TRUE(saw_draining);
    en_controller_destroy(controller);
}

static void test_yaml_config_loads_paths_and_intents(void)
{
    const char *filename = "eventnet-test-routes.yaml";
    FILE *file = fopen(filename, "w");
    ASSERT_TRUE(file != NULL);
    fputs(
        "tunnels:\n"
        "  - id: tun-a-b\n"
        "    local_node: site-a\n"
        "    remote_node: site-b\n"
        "    local_endpoint: 203.0.113.10\n"
        "    remote_endpoint: 203.0.113.20\n"
        "    local_ts: 10.0.1.0/24\n"
        "    remote_ts: 10.0.2.0/24\n"
        "  - id: tun-a-hub\n"
        "    local_node: site-a\n"
        "    remote_node: hub-1\n"
        "    local_endpoint: 203.0.113.10\n"
        "    remote_endpoint: 203.0.113.1\n"
        "  - id: tun-hub-b\n"
        "    local_node: hub-1\n"
        "    remote_node: site-b\n"
        "    remote_endpoint: 203.0.113.20\n"
        "paths:\n"
        "  - id: path-direct\n"
        "    source: site-a\n"
        "    destination: site-b\n"
        "    priority: 10\n"
        "    segments:\n"
        "      - id: seg-a-b\n"
        "        from: site-a\n"
        "        to: site-b\n"
        "        tunnel_id: tun-a-b\n"
        "  - id: path-via-hub\n"
        "    source: site-a\n"
        "    destination: site-b\n"
        "    waypoints:\n"
        "      - hub-1\n"
        "    priority: 30\n"
        "    segments:\n"
        "      - id: seg-a-hub\n"
        "        from: site-a\n"
        "        to: hub-1\n"
        "        tunnel_id: tun-a-hub\n"
        "      - id: seg-hub-b\n"
        "        from: hub-1\n"
        "        to: site-b\n"
        "        tunnel_id: tun-hub-b\n"
        "intents:\n"
        "  - id: intent-a-b\n"
        "    traffic:\n"
        "      source: site-a\n"
        "      destination: site-b\n"
        "    path_selection:\n"
        "      mode: priority\n"
        "      candidates:\n"
        "        - path-direct\n"
        "        - path-via-hub\n"
        "    transition:\n"
        "      strategy: immediate\n"
        "    fallback:\n"
        "      enabled: true\n"
        "      path_id: path-via-hub\n",
        file
    );
    fclose(file);

    en_yaml_config_t config = {0};
    char error[256] = {0};
    ASSERT_TRUE(en_yaml_config_load_file(filename, &config, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(config.tunnel_count == 3);
    ASSERT_TRUE(config.path_count == 2);
    ASSERT_TRUE(config.intent_count == 1);
    ASSERT_STREQ(config.paths[0].path_id, "path-direct");
    ASSERT_STREQ(config.paths[1].waypoints[0], "hub-1");
    ASSERT_TRUE(config.intents[0].path_selection.mode == EN_SELECT_PRIORITY);
    ASSERT_STREQ(config.intents[0].path_selection.candidates[0], "path-direct");

    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = en_controller_create_with_tunnels(
        config.paths,
        config.path_count,
        config.tunnels,
        config.tunnel_count,
        en_strongswan_mock_adapter(),
        en_vpp_mock_adapter(&vpp_mock),
        en_health_probe_mock_adapter(&health_mock)
    );
    ASSERT_TRUE(controller != NULL);
    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &config.intents[0], &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-direct");

    en_controller_destroy(controller);
    remove(filename);
}

static void test_command_adapters_can_drive_controller_dry_run(void)
{
    en_path_t paths[EN_MAX_PATHS];
    size_t path_count = en_initial_demo_paths(paths, EN_MAX_PATHS);
    en_tunnel_t tunnels[1] = {0};
    snprintf(tunnels[0].tunnel_id, sizeof(tunnels[0].tunnel_id), "%s", "tun-a-b");
    snprintf(tunnels[0].remote_endpoint, sizeof(tunnels[0].remote_endpoint), "%s", "203.0.113.20");
    en_strongswan_command_ctx_t strongswan_ctx = {
        .dry_run = true,
        .ensure_tunnel_command = "swanctl --initiate --child {tunnel_id}",
        .verify_tunnel_command = "swanctl --list-sas --child {tunnel_id}",
        .remove_tunnel_command = "swanctl --terminate --child {tunnel_id}",
    };
    en_vpp_command_ctx_t vpp_ctx = {
        .dry_run = true,
        .tunnels = tunnels,
        .tunnel_count = 1,
    };
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = en_controller_create(
        paths,
        path_count,
        en_strongswan_command_adapter(&strongswan_ctx),
        en_vpp_command_adapter(&vpp_ctx),
        en_health_probe_mock_adapter(&health_mock)
    );
    ASSERT_TRUE(controller != NULL);

    en_intent_t intent = base_intent(EN_SELECT_EXPLICIT);
    snprintf(intent.path_selection.path_id, sizeof(intent.path_selection.path_id), "%s", "path-direct");
    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-direct");

    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&intent.traffic, traffic_key, sizeof(traffic_key));
    ASSERT_STREQ(en_controller_applied_path(controller, traffic_key), "path-direct");
    en_controller_destroy(controller);
}

static void test_command_adapter_vlan_acl_cleanup_dry_run(void)
{
    en_path_t paths[EN_MAX_PATHS];
    en_initial_demo_paths(paths, EN_MAX_PATHS);
    en_tunnel_t tunnel = {0};
    snprintf(tunnel.tunnel_id, sizeof(tunnel.tunnel_id), "%s", "tun-a-b");
    snprintf(tunnel.local_endpoint, sizeof(tunnel.local_endpoint), "%s", "203.0.113.10");
    snprintf(tunnel.remote_endpoint, sizeof(tunnel.remote_endpoint), "%s", "203.0.113.20");
    en_vpp_edge_t edge = {0};
    snprintf(edge.node_id, sizeof(edge.node_id), "%s", "site-a");
    snprintf(edge.vpp_interface, sizeof(edge.vpp_interface), "%s", "host-vpp-site-a");
    en_vpp_command_ctx_t context = {
        .dry_run = true,
        .has_vlan_id = true,
        .deny_unmatched_vlan = true,
        .vlan_id = 100,
        .edges = &edge,
        .edge_count = 1,
        .tunnels = &tunnel,
        .tunnel_count = 1,
    };
    en_vpp_adapter_t adapter = en_vpp_command_adapter(&context);
    ASSERT_TRUE(adapter.install_path(adapter.ctx, "site-a->site-b|vlan=100", &paths[1]) == EN_ERR_NONE);
    ASSERT_TRUE(adapter.install_path(adapter.ctx, "site-a->site-b|vlan=100|tenant-b", &paths[1]) == EN_ERR_NONE);
    ASSERT_TRUE(context.active_count == 2);
    ASSERT_TRUE(context.active_path_objects[0] == &paths[1]);
    ASSERT_TRUE(context.active_path_objects[1] == &paths[1]);
    ASSERT_TRUE(adapter.remove_path(adapter.ctx, "site-a->site-b|vlan=100", &paths[1]) == EN_ERR_NONE);
    ASSERT_TRUE(context.active_count == 1);
    ASSERT_TRUE(context.active_path_objects[0] == &paths[1]);
    ASSERT_STREQ(context.traffic_keys[0], "site-a->site-b|vlan=100|tenant-b");
    ASSERT_TRUE(adapter.remove_path(adapter.ctx, "site-a->site-b|vlan=100|tenant-b", &paths[1]) == EN_ERR_NONE);
    ASSERT_TRUE(context.active_count == 0);
}

static void test_repeated_intent_submission_does_not_exhaust_state(void)
{
    en_path_t paths[EN_MAX_PATHS];
    size_t path_count = en_initial_demo_paths(paths, EN_MAX_PATHS);
    en_health_probe_mock_t health_mock = {0};
    en_vpp_mock_t vpp_mock = {0};
    en_controller_t *controller = en_controller_create(
        paths, path_count, en_strongswan_mock_adapter(), en_vpp_mock_adapter(&vpp_mock),
        en_health_probe_mock_adapter(&health_mock));
    ASSERT_TRUE(controller != NULL);
    en_intent_t intent = base_intent(EN_SELECT_EXPLICIT);
    snprintf(intent.path_selection.path_id, sizeof(intent.path_selection.path_id), "%s", "path-direct");
    for (int index = 0; index < EN_MAX_CANDIDATES + 10; index++) {
        en_reconcile_result_t result = {0};
        ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    }
    en_controller_destroy(controller);
}

static void test_applied_path_can_be_restored(void)
{
    en_path_t paths[EN_MAX_PATHS];
    size_t path_count = en_initial_demo_paths(paths, EN_MAX_PATHS);
    en_health_probe_mock_t health_mock = {0};
    en_vpp_mock_t vpp_mock = {0};
    en_controller_t *controller = en_controller_create(
        paths, path_count, en_strongswan_mock_adapter(), en_vpp_mock_adapter(&vpp_mock),
        en_health_probe_mock_adapter(&health_mock));
    ASSERT_TRUE(controller != NULL);
    ASSERT_TRUE(en_controller_restore_applied_path(controller, "site-a->site-b", "path-direct") == EN_ERR_NONE);
    ASSERT_STREQ(en_controller_applied_path(controller, "site-a->site-b"), "path-direct");
    ASSERT_TRUE(en_controller_restore_applied_path(controller, "site-a->site-c", "path-via-hub") == EN_ERR_NONE);
    ASSERT_STREQ(en_controller_applied_path(controller, "site-a->site-b"), "path-direct");
    ASSERT_STREQ(en_controller_applied_path(controller, "site-a->site-c"), "path-via-hub");
    ASSERT_TRUE(en_controller_find_path(controller, "path-direct")->operational_state == EN_PATH_ACTIVE);
    ASSERT_TRUE(en_controller_find_path(controller, "path-via-hub")->operational_state == EN_PATH_ACTIVE);
    ((en_path_t *)en_controller_find_path(controller, "path-direct"))->administrative_state = EN_ADMIN_DISABLED;
    ASSERT_TRUE(en_controller_restore_applied_path(controller, "site-a->site-b", "path-direct") == EN_ERR_STATE_CONFLICT);
    ((en_path_t *)en_controller_find_path(controller, "path-direct"))->administrative_state = EN_ADMIN_ENABLED;
    ASSERT_TRUE(en_controller_restore_applied_path(controller, "site-a->site-b", "missing") == EN_ERR_NOT_FOUND);
    en_controller_destroy(controller);
}

static void test_forwarding_failure_reinstalls_previous_path(void)
{
    en_path_t paths[EN_MAX_PATHS];
    size_t path_count = en_initial_demo_paths(paths, EN_MAX_PATHS);
    en_health_probe_mock_t health_mock = {0};
    en_vpp_mock_t vpp_mock = {0};
    en_controller_t *controller = en_controller_create(
        paths, path_count, en_strongswan_mock_adapter(), en_vpp_mock_adapter(&vpp_mock),
        en_health_probe_mock_adapter(&health_mock));
    ASSERT_TRUE(controller != NULL);
    en_intent_t intent = base_intent(EN_SELECT_EXPLICIT);
    snprintf(intent.path_selection.path_id, sizeof(intent.path_selection.path_id), "%s", "path-direct");
    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    vpp_mock.fail_next_update = true;
    snprintf(intent.path_selection.path_id, sizeof(intent.path_selection.path_id), "%s", "path-via-hub");
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) != EN_ERR_NONE);
    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&intent.traffic, traffic_key, sizeof(traffic_key));
    ASSERT_STREQ(en_controller_applied_path(controller, traffic_key), "path-direct");
    ASSERT_STREQ(vpp_mock.active_paths[0], "path-direct");
    en_controller_destroy(controller);
}

static void test_renderers_generate_swanctl_and_vppctl(void)
{
    en_tunnel_t tunnel = {0};
    snprintf(tunnel.tunnel_id, sizeof(tunnel.tunnel_id), "%s", "tun-a-b");
    snprintf(tunnel.local_endpoint, sizeof(tunnel.local_endpoint), "%s", "203.0.113.10");
    snprintf(tunnel.remote_endpoint, sizeof(tunnel.remote_endpoint), "%s", "203.0.113.20");
    snprintf(tunnel.local_traffic_selector, sizeof(tunnel.local_traffic_selector), "%s", "10.0.1.0/24");
    snprintf(tunnel.remote_traffic_selector, sizeof(tunnel.remote_traffic_selector), "%s", "10.0.2.0/24");

    en_path_t path = {0};
    snprintf(path.path_id, sizeof(path.path_id), "%s", "path-direct");
    snprintf(path.route_destination_prefix, sizeof(path.route_destination_prefix), "%s", "10.0.2.0/24");
    snprintf(path.egress_tunnel_id, sizeof(path.egress_tunnel_id), "%s", "tun-a-b");

    char command[512] = {0};
    ASSERT_TRUE(en_render_swanctl_initiate(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "swanctl --initiate --child tun-a-b");
    ASSERT_TRUE(en_render_swanctl_initiate_uri(&tunnel, "unix:///run/eventnet/site-a/charon.vici", command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "swanctl --uri unix:///run/eventnet/site-a/charon.vici --initiate --child tun-a-b");
    ASSERT_TRUE(en_render_swanctl_load_conns_uri("unix:///run/eventnet/site-a/charon.vici", "/run/ibuki/swanctl.conf", command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "swanctl --uri unix:///run/eventnet/site-a/charon.vici --load-conns --file /run/ibuki/swanctl.conf");
    ASSERT_TRUE(en_render_swanctl_load_conns_uri("unix:///run/eventnet/site-a/charon.vici;id", "/run/ibuki/swanctl.conf", command, sizeof(command)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_render_swanctl_load_conns_uri("unix:///run/eventnet/site-a/charon.vici", "/run/ibuki/swanctl.conf;touch", command, sizeof(command)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_render_swanctl_initiate_uri(&tunnel, "unix:///run/eventnet/site-a/charon.vici;id", command, sizeof(command)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_render_vpp_route_replace(&path, &tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl ip route add 10.0.2.0/24 via 203.0.113.20");
    char short_command[16] = {0};
    ASSERT_TRUE(en_render_swanctl_initiate(&tunnel, short_command, sizeof(short_command)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_render_vpp_route_replace(&path, &tunnel, short_command, sizeof(short_command)) == EN_ERR_INVALID_ARGUMENT);

    en_tunnel_t unsafe = tunnel;
    snprintf(unsafe.tunnel_id, sizeof(unsafe.tunnel_id), "%s", "tun-a-b;touch");
    ASSERT_TRUE(en_render_swanctl_initiate(&unsafe, command, sizeof(command)) == EN_ERR_INVALID_ARGUMENT);

    en_route_t unsafe_route = {0};
    snprintf(unsafe_route.destination_prefix, sizeof(unsafe_route.destination_prefix), "%s", "10.0.2.0/24;touch");
    snprintf(unsafe_route.next_hop, sizeof(unsafe_route.next_hop), "%s", "203.0.113.20");
    ASSERT_TRUE(en_render_vpp_route_replace_entry(&unsafe_route, command, sizeof(command)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_render_vpp_route_delete_with_tunnel(&path, &tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl ip route del 10.0.2.0/24 via 203.0.113.20");
}

static void test_renderers_generate_vpp_gre_over_ipsec(void)
{
    en_tunnel_t tunnel = {0};
    snprintf(tunnel.tunnel_id, sizeof(tunnel.tunnel_id), "%s", "gre-a-b");
    snprintf(tunnel.tunnel_type, sizeof(tunnel.tunnel_type), "%s", "gre_over_ipsec");
    snprintf(tunnel.local_endpoint, sizeof(tunnel.local_endpoint), "%s", "203.0.113.10");
    snprintf(tunnel.remote_endpoint, sizeof(tunnel.remote_endpoint), "%s", "203.0.113.20");
    snprintf(tunnel.gre_interface, sizeof(tunnel.gre_interface), "%s", "gre0");
    snprintf(tunnel.gre_local_address, sizeof(tunnel.gre_local_address), "%s", "10.255.0.1/30");
    snprintf(tunnel.gre_remote_address, sizeof(tunnel.gre_remote_address), "%s", "10.255.0.2");
    tunnel.gre_instance = 0;
    tunnel.gre_mtu = 1400;

    en_path_t path = {0};
    snprintf(path.route_destination_prefix, sizeof(path.route_destination_prefix), "%s", "10.0.2.0/24");
    char command[512] = {0};
    ASSERT_TRUE(en_render_vpp_gre_create(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl create gre tunnel src 203.0.113.10 dst 203.0.113.20 instance 0");
    ASSERT_TRUE(en_render_vpp_gre_set_address(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl set interface ip address gre0 10.255.0.1/30");
    ASSERT_TRUE(en_render_vpp_gre_set_mtu(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl set interface mtu 1400 gre0");
    ASSERT_TRUE(en_render_vpp_gre_set_up(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl set interface state gre0 up");
    ASSERT_TRUE(en_render_vpp_gre_route_replace(&path, &tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl ip route add 10.0.2.0/24 via 10.255.0.2 gre0");
    en_route_t logical_route = {0};
    logical_route.table_id = -1;
    logical_route.metric = -1;
    snprintf(logical_route.destination_prefix, sizeof(logical_route.destination_prefix), "%s", "10.0.2.0/24");
    ASSERT_TRUE(en_route_resolve_tunnel_egress(&logical_route, &tunnel, &logical_route) == EN_ERR_NONE);
    ASSERT_STREQ(logical_route.next_hop, "10.255.0.2");
    ASSERT_STREQ(logical_route.interface_name, "gre0");
    ASSERT_TRUE(en_render_vpp_route_replace_entry(&logical_route, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl ip route add 10.0.2.0/24 via 10.255.0.2 gre0");
    ASSERT_TRUE(en_render_swanctl_conf(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(command, "children.gre-a-b.mode=transport") != NULL);
    ASSERT_TRUE(strstr(command, "local_ts=203.0.113.10/32[gre]") != NULL);
    ASSERT_TRUE(strstr(command, "remote_ts=203.0.113.20/32[gre]") != NULL);
    snprintf(tunnel.gre_outer_local_endpoint, sizeof(tunnel.gre_outer_local_endpoint), "%s", "172.16.1.1");
    snprintf(tunnel.gre_outer_remote_endpoint, sizeof(tunnel.gre_outer_remote_endpoint), "%s", "172.16.2.1");
    ASSERT_TRUE(en_render_swanctl_conf(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(command, "children.gre-a-b.mode=tunnel") != NULL);
    ASSERT_TRUE(strstr(command, "local_ts=172.16.1.1/32[gre]") != NULL);
    ASSERT_TRUE(strstr(command, "remote_ts=172.16.2.1/32[gre]") != NULL);
    ASSERT_TRUE(en_render_vpp_gre_create(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl create gre tunnel src 172.16.1.1 dst 172.16.2.1 instance 0");
    ASSERT_TRUE(en_render_vpp_gre_delete(&tunnel, command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl create gre tunnel src 172.16.1.1 dst 172.16.2.1 instance 0 del");

    en_tunnel_t invalid = tunnel;
    invalid.gre_interface[0] = '\0';
    ASSERT_TRUE(en_render_vpp_gre_create(&invalid, command, sizeof(command)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_path_events_parse_as_health_records(void)
{
    en_path_health_t health = {0};
    char error[128] = {0};
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.path.v1\",\"event\":\"path_failed\",\"path_id\":\"path-direct\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_STREQ(health.path_id, "path-direct");
    ASSERT_TRUE(health.state == EN_HEALTH_FAILED);
    ASSERT_TRUE(health.packet_loss_percent == 100.0);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.path.v1\",\"event\":\"path_recovered\",\"path_id\":\"path-direct\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(health.state == EN_HEALTH_HEALTHY);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"degraded\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(health.state == EN_HEALTH_DEGRADED);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"source\":\"site-a\",\"target\":\"203.0.113.9\",\"sequence\":4,\"state\":\"healthy\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_STREQ(health.source_node, "site-a");
    ASSERT_STREQ(health.target, "203.0.113.9");
    ASSERT_TRUE(health.sequence == 4);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"source\":\"site-a;inject\",\"target\":\"203.0.113.9\",\"state\":\"healthy\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"source\":\"site-a\",\"target\":\"203.0.113.9;inject\",\"state\":\"healthy\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"sequence\":1.5,\"state\":\"healthy\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"sequence\":-1,\"state\":\"healthy\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"failed\",\"rtt_ms\":0,\"packet_loss_percent\":100,\"jitter_ms\":0,\"consecutive_failures\":3,\"consecutive_successes\":0,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(health.consecutive_failures == 3);
    ASSERT_TRUE(health.consecutive_successes == 0);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"failed\",\"rtt_ms\":0,\"packet_loss_percent\":100,\"jitter_ms\":0,\"consecutive_failures\":1.5,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"failed\",\"rtt_ms\":0,\"packet_loss_percent\":100,\"jitter_ms\":0,\"consecutive_failures\":-1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"typo\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":-1,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":20,\"packet_loss_percent\":101,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":nan,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":20oops,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":+20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":01,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":20.,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1e30}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct;inject\",\"state\":\"healthy\",\"rtt_ms\":20,\"packet_loss_percent\":0,\"jitter_ms\":1,\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.path.v1\",\"note\":\"ibuki.telemetry.path_health.v1\",\"event\":\"path_failed\",\"path_id\":\"path-direct\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.unknown.v1\",\"note\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "prefix {\"schema\":\"ibuki.event.path.v1\",\"event\":\"path_failed\",\"path_id\":\"path-direct\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.path.v1\",\"event\":\"path_failed\",\"path_id\":\"path-direct\",\"timestamp_ms\":1000} trailing",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.tunnel.v1\",\"path_id\":\"path-direct\",\"tunnel_id\":\"tun-a-b\",\"state\":\"installed\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(health.state == EN_HEALTH_HEALTHY);
    ASSERT_STREQ(health.observed_tunnel_id, "tun-a-b");
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.vpp.route.v1\",\"path_id\":\"path-direct\",\"tunnel_id\":\"route-a-b\",\"state\":\"deleted\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(health.state == EN_HEALTH_FAILED);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.vpp.route.v1\",\"path_id\":\"path-direct\",\"tunnel_id\":\"route-a-b\",\"table_id\":100,\"state\":\"up\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(health.has_table_id && health.table_id == 100);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.vpp.route.v1\",\"path_id\":\"path-direct\",\"tunnel_id\":\"route-a-b\",\"table_id\":1.5,\"state\":\"up\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_telemetry_parse_json_line(
        "{\"schema\":\"ibuki.event.vpp.route.v1\",\"path_id\":\"path-direct\",\"tunnel_id\":\"route-a-b\",\"table_id\":-1,\"state\":\"up\",\"timestamp_ms\":1000}",
        &health, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_health_probe_ignores_out_of_order_records(void)
{
    en_health_probe_mock_t health_mock = {0};
    en_path_health_t newest = {0};
    en_path_health_t older = {0};
    snprintf(newest.path_id, sizeof(newest.path_id), "%s", "path-direct");
    newest.state = EN_HEALTH_HEALTHY;
    newest.last_updated_ms = 2000;
    newest.sequence = 2;
    snprintf(newest.source_node, sizeof(newest.source_node), "%s", "site-a");
    older = newest;
    older.state = EN_HEALTH_FAILED;
    older.sequence = 1;
    en_health_probe_mock_set(&health_mock, newest);
    en_health_probe_mock_set(&health_mock, older);
    ASSERT_TRUE(health_mock.override_count == 1);
    ASSERT_TRUE(health_mock.overrides[0].state == EN_HEALTH_HEALTHY);
    ASSERT_TRUE(health_mock.overrides[0].last_updated_ms == 2000);
}

static void test_vlan_health_requires_route_and_interface(void)
{
    en_health_probe_mock_t health_mock = {0};
    health_mock.require_interface_and_route = true;
    en_path_t path = {0};
    snprintf(path.path_id, sizeof(path.path_id), "%s", "path-vlan");
    en_health_probe_t probe = en_health_probe_mock_adapter(&health_mock);
    en_path_health_t route = {0};
    snprintf(route.path_id, sizeof(route.path_id), "%s", "path-vlan");
    route.state = EN_HEALTH_HEALTHY;
    route.route_state = EN_HEALTH_HEALTHY;
    route.has_route_observation = true;
    route.last_updated_ms = 1000;
    en_health_probe_mock_set(&health_mock, route);
    en_path_health_t result = {0};
    ASSERT_TRUE(probe.validate_path(probe.ctx, &path, &result) == EN_ERR_NONE);
    ASSERT_TRUE(result.state == EN_HEALTH_FAILED);

    en_path_health_t interface = {0};
    snprintf(interface.path_id, sizeof(interface.path_id), "%s", "path-vlan");
    interface.state = EN_HEALTH_HEALTHY;
    interface.interface_state = EN_HEALTH_HEALTHY;
    interface.has_interface_observation = true;
    snprintf(interface.observed_interface_name, sizeof(interface.observed_interface_name), "%s", "host-vpp-site-a.100");
    interface.last_updated_ms = 1001;
    en_health_probe_mock_set(&health_mock, interface);
    ASSERT_TRUE(probe.validate_path(probe.ctx, &path, &result) == EN_ERR_NONE);
    ASSERT_TRUE(result.state == EN_HEALTH_HEALTHY);

    interface.interface_state = EN_HEALTH_FAILED;
    interface.state = EN_HEALTH_FAILED;
    interface.last_updated_ms = 1002;
    en_health_probe_mock_set(&health_mock, interface);
    ASSERT_TRUE(probe.validate_path(probe.ctx, &path, &result) == EN_ERR_NONE);
    ASSERT_TRUE(result.state == EN_HEALTH_FAILED);
}

static void test_telemetry_file_schema_validation(void)
{
#if defined(_WIN32)
    const char *filename = "eventnet-test-telemetry.jsonl";
#else
    const char *filename = "/tmp/eventnet-test-telemetry.jsonl";
#endif
    const char *valid_record = "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"path-direct\",\"state\":\"healthy\",\"rtt_ms\":10,\"packet_loss_percent\":0,\"jitter_ms\":0,\"timestamp_ms\":1000}\n";
    en_path_health_t records[EN_MAX_PATHS] = {0};
    size_t record_count = 0;
    char error[128] = {0};
    FILE *file = fopen(filename, "w");
    ASSERT_TRUE(file != NULL);
    fputs("\n", file);
    fputs(valid_record, file);
    fclose(file);
    make_test_file_private(filename);
    ASSERT_TRUE(en_telemetry_load_jsonl(filename, records, EN_MAX_PATHS, &record_count, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(record_count == 1);

    file = fopen(filename, "w");
    ASSERT_TRUE(file != NULL);
    fputs("{\"schema\":\"ibuki.event.vpp.interface.v1\",\"path_id\":\"path-direct\",\"interface_name\":\"ipsec0\",\"state\":\"up\",\"timestamp_ms\":1000}\n", file);
    fclose(file);
    make_test_file_private(filename);
    record_count = 0;
    ASSERT_TRUE(en_telemetry_load_jsonl(filename, records, EN_MAX_PATHS, &record_count, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(record_count == 1 && records[0].state == EN_HEALTH_HEALTHY);
    ASSERT_STREQ(records[0].observed_interface_name, "ipsec0");

    file = fopen(filename, "w");
    ASSERT_TRUE(file != NULL);
    fputs("not-json\n", file);
    fclose(file);
    make_test_file_private(filename);
    record_count = 0;
    ASSERT_TRUE(en_telemetry_load_jsonl(filename, records, EN_MAX_PATHS, &record_count, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);

    file = fopen(filename, "w");
    ASSERT_TRUE(file != NULL);
    fputs("{\"schema\":\"ibuki.telemetry.path_health.v2\",\"path_id\":\"path-direct\"}\n", file);
    fclose(file);
    make_test_file_private(filename);
    record_count = 0;
    ASSERT_TRUE(en_telemetry_load_jsonl(filename, records, EN_MAX_PATHS, &record_count, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    remove(filename);
}

static void test_pubkey_certificate_path_validation(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.tunnels[0].tunnel_id, sizeof(config.tunnels[0].tunnel_id), "%s", "tun-cert");
    snprintf(config.tunnels[0].local_node, sizeof(config.tunnels[0].local_node), "%s", "site-a");
    snprintf(config.tunnels[0].remote_node, sizeof(config.tunnels[0].remote_node), "%s", "site-b");
    snprintf(config.tunnels[0].auth_method, sizeof(config.tunnels[0].auth_method), "%s", "pubkey");
    snprintf(config.tunnels[0].local_cert, sizeof(config.tunnels[0].local_cert), "%s", "site-a.crt\nlocal { auth = psk }");
    config.tunnel_count = 1;
    snprintf(config.paths[0].path_id, sizeof(config.paths[0].path_id), "%s", "path-cert");
    snprintf(config.paths[0].source, sizeof(config.paths[0].source), "%s", "site-a");
    snprintf(config.paths[0].destination, sizeof(config.paths[0].destination), "%s", "site-b");
    snprintf(config.paths[0].egress_tunnel_id, sizeof(config.paths[0].egress_tunnel_id), "%s", "tun-psk");
    snprintf(config.paths[0].route_destination_prefix, sizeof(config.paths[0].route_destination_prefix), "%s", "10.0.2.0/24");
    snprintf(config.paths[0].segments[0].segment_id, sizeof(config.paths[0].segments[0].segment_id), "%s", "seg-cert");
    snprintf(config.paths[0].segments[0].tunnel_id, sizeof(config.paths[0].segments[0].tunnel_id), "%s", "tun-cert");
    config.paths[0].segment_count = 1;
    config.path_count = 1;
    char error[128] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_psk_is_quoted_and_injection_is_rejected(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.tunnels[0].tunnel_id, sizeof(config.tunnels[0].tunnel_id), "%s", "tun-psk");
    snprintf(config.tunnels[0].local_node, sizeof(config.tunnels[0].local_node), "%s", "site-a");
    snprintf(config.tunnels[0].remote_node, sizeof(config.tunnels[0].remote_node), "%s", "site-b");
    snprintf(config.tunnels[0].psk, sizeof(config.tunnels[0].psk), "%s", "safe-secret");
    snprintf(config.tunnels[0].local_endpoint, sizeof(config.tunnels[0].local_endpoint), "%s", "203.0.113.10");
    snprintf(config.tunnels[0].remote_endpoint, sizeof(config.tunnels[0].remote_endpoint), "%s", "203.0.113.20");
    snprintf(config.tunnels[0].local_traffic_selector, sizeof(config.tunnels[0].local_traffic_selector), "%s", "10.0.1.0/24");
    snprintf(config.tunnels[0].remote_traffic_selector, sizeof(config.tunnels[0].remote_traffic_selector), "%s", "10.0.2.0/24");
    config.tunnel_count = 1;
    snprintf(config.paths[0].path_id, sizeof(config.paths[0].path_id), "%s", "path-psk");
    snprintf(config.paths[0].source, sizeof(config.paths[0].source), "%s", "site-a");
    snprintf(config.paths[0].destination, sizeof(config.paths[0].destination), "%s", "site-b");
    snprintf(config.paths[0].egress_tunnel_id, sizeof(config.paths[0].egress_tunnel_id), "%s", "tun-psk");
    snprintf(config.paths[0].route_destination_prefix, sizeof(config.paths[0].route_destination_prefix), "%s", "10.0.2.0/24");
    snprintf(config.paths[0].segments[0].segment_id, sizeof(config.paths[0].segments[0].segment_id), "%s", "seg-psk");
    snprintf(config.paths[0].segments[0].tunnel_id, sizeof(config.paths[0].segments[0].tunnel_id), "%s", "tun-psk");
    config.paths[0].segment_count = 1;
    config.path_count = 1;
    snprintf(config.intents[0].intent_id, sizeof(config.intents[0].intent_id), "%s", "intent-psk");
    snprintf(config.intents[0].traffic.source, sizeof(config.intents[0].traffic.source), "%s", "site-a");
    snprintf(config.intents[0].traffic.destination, sizeof(config.intents[0].traffic.destination), "%s", "site-b");
    config.intents[0].path_selection.mode = EN_SELECT_EXPLICIT;
    snprintf(config.intents[0].path_selection.path_id, sizeof(config.intents[0].path_selection.path_id), "%s", "path-psk");
    config.intent_count = 1;
    char error[128] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NONE);
    en_apply_plan_t plan = {0};
    ASSERT_TRUE(en_apply_plan_from_config(&config, &config.intents[0], &config.paths[0], &plan) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(plan.swanctl_conf, "secret = \"safe-secret\"") != NULL);
    char long_conf_filename[EN_MAX_COMMAND_LEN + 1];
    memset(long_conf_filename, 'x', sizeof(long_conf_filename) - 1);
    long_conf_filename[sizeof(long_conf_filename) - 1] = '\0';
    ASSERT_TRUE(en_apply_plan_from_config_with_file(&config, &config.intents[0], &config.paths[0], long_conf_filename, &plan) == EN_ERR_INVALID_ARGUMENT);
    snprintf(config.tunnels[0].psk, sizeof(config.tunnels[0].psk), "%s", "bad\"\nsecret");
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_swanctl_list_sas_observer_parser(void)
{
    const char *output =
        "list-sas:\n"
        "  ike-a:\n"
        "    children:\n"
        "      tun-a-b:\n"
        "        state: INSTALLED\n"
        "        mode: TUNNEL\n";
    en_strongswan_sa_observation_t observation = {0};
    char error[128] = {0};
    ASSERT_TRUE(en_strongswan_parse_list_sas(output, "tun-a-b", &observation, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(observation.state == EN_TUNNEL_ESTABLISHED);
    ASSERT_TRUE(observation.health == EN_HEALTH_HEALTHY);
    char event[512] = {0};
    ASSERT_TRUE(en_strongswan_observation_to_event_json(&observation, "path-direct", 1000, event, sizeof(event)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(event, "ibuki.event.tunnel.v1") != NULL);
    ASSERT_TRUE(strstr(event, "\"state\":\"installed\"") != NULL);
    ASSERT_TRUE(en_strongswan_parse_list_sas(output, "missing", &observation, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    ASSERT_TRUE(en_strongswan_parse_list_sas(output, "tun-a-b\"inject", &observation, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_vpp_show_ip_fib_observer_parser(void)
{
    const char *output = "10.10.2.0/24\n  unicast via 203.0.113.9 ipsec0\n";
    en_vpp_route_observation_t observation = {0};
    char error[128] = {0};
    ASSERT_TRUE(en_vpp_parse_show_ip_fib(output, "10.10.2.0/24", &observation, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(observation.present);
    ASSERT_STREQ(observation.next_hop, "203.0.113.9");
    ASSERT_STREQ(observation.interface_name, "ipsec0");
    en_vpp_route_observation_t missing_observation = {0};
    ASSERT_TRUE(en_vpp_parse_show_ip_fib(output, "10.10.9.0/24", &missing_observation, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    const char *vrf_output = "ipv4-VRF:0, fib_index:0\n10.10.2.0/24\n  unicast via 192.0.2.1 ipsec0\nipv4-VRF:100, fib_index:1\n10.10.2.0/24\n  unicast via 198.51.100.1 ipsec1\n";
    en_vpp_route_observation_t vrf_observation = {0};
    ASSERT_TRUE(en_vpp_parse_show_ip_fib_in_table(vrf_output, "10.10.2.0/24", 100, &vrf_observation, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_STREQ(vrf_observation.next_hop, "198.51.100.1");
    ASSERT_TRUE(vrf_observation.table_id == 100);
    en_vpp_route_observation_t wrong_vrf_observation = {0};
    ASSERT_TRUE(en_vpp_parse_show_ip_fib_in_table(vrf_output, "10.10.2.0/24", 200, &wrong_vrf_observation, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    en_vpp_interface_observation_t interface_observation = {0};
    ASSERT_TRUE(en_vpp_parse_show_interface("host-vpp-site-a.100 up\n", "host-vpp-site-a.100", &interface_observation, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(interface_observation.present && interface_observation.up);
    ASSERT_TRUE(en_vpp_parse_show_interface("host-vpp-site-a.100 down\n", "host-vpp-site-a.100", &interface_observation, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(interface_observation.present && !interface_observation.up);
    ASSERT_TRUE(en_vpp_parse_show_interface("host-vpp-site-b.100 up\n", "host-vpp-site-a.100", &interface_observation, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    char event[512] = {0};
    ASSERT_TRUE(en_vpp_route_observation_to_event_json(&observation, "path-direct", "route-a-b", 1000, event, sizeof(event)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(event, "ibuki.event.vpp.route.v1") != NULL);
    ASSERT_TRUE(strstr(event, "\"state\":\"up\"") != NULL);
    ASSERT_TRUE(en_vpp_route_observation_to_event_json(&vrf_observation, "path-direct", "route-vrf", 1000, event, sizeof(event)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(event, "\"table_id\":100") != NULL);
    ASSERT_TRUE(strstr(event, "\"destination_prefix\":\"10.10.2.0/24\"") != NULL);
    ASSERT_TRUE(strstr(event, "\"next_hop\":\"198.51.100.1\"") != NULL);
    en_path_health_t route_health = {0};
    ASSERT_TRUE(en_telemetry_parse_json_line(event, &route_health, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_STREQ(route_health.observed_destination_prefix, "10.10.2.0/24");
    ASSERT_STREQ(route_health.observed_next_hop, "198.51.100.1");
    ASSERT_TRUE(route_health.has_table_id && route_health.table_id == 100);
    ASSERT_TRUE(en_vpp_route_observation_to_event_json(&observation, "path-direct\"inject", "route-a-b", 1000, event, sizeof(event)) == EN_ERR_INVALID_ARGUMENT);
    en_vpp_route_observation_t invalid_observation = observation;
    snprintf(invalid_observation.next_hop, sizeof(invalid_observation.next_hop), "%s", "203.0.113.9\"inject");
    ASSERT_TRUE(en_vpp_route_observation_to_event_json(&invalid_observation, "path-direct", "route-a-b", 1000, event, sizeof(event)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_vpp_parse_show_ip_fib("10.10.2.0/24\n  unicast via 203.0.113.9\"inject ipsec0\n", "10.10.2.0/24", &invalid_observation, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    en_vpp_route_observation_t down_observation = {0};
    down_observation.table_id = -1;
    ASSERT_TRUE(en_vpp_route_observation_to_event_json(&down_observation, "path-direct", "route-a-b", 1000, event, sizeof(event)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(event, "\"state\":\"down\"") != NULL);
}

static void test_vpp_api_adapter_forwards_callbacks(void)
{
    vpp_api_test_state_t state = {0};
    en_vpp_api_ctx_t api = {
        .install_path = vpp_api_test_install,
        .remove_path = vpp_api_test_remove,
        .active_path = vpp_api_test_active,
        .observe_route = vpp_api_test_observe,
        .observe_interface = vpp_api_test_observe_interface,
        .ctx = &state,
    };
    en_vpp_adapter_t adapter = en_vpp_api_adapter(&api);
    en_path_t path = {0};
    snprintf(path.path_id, sizeof(path.path_id), "%s", "path-api");
    ASSERT_TRUE(adapter.install_path(adapter.ctx, "traffic", &path) == EN_ERR_NONE);
    ASSERT_TRUE(adapter.remove_path(adapter.ctx, "traffic", &path) == EN_ERR_NONE);
    ASSERT_TRUE(strcmp(adapter.active_path(adapter.ctx, "traffic"), "path-api") == 0);
    ASSERT_TRUE(state.install_count == 1 && state.remove_count == 1);
    en_vpp_route_observation_t observation = {0};
    ASSERT_TRUE(en_vpp_api_observe_route(&api, "10.0.2.0/24", &observation) == EN_ERR_NONE);
    ASSERT_TRUE(observation.present && strcmp(observation.destination_prefix, "10.0.2.0/24") == 0);
    ASSERT_TRUE(state.observe_count == 1);
    char event[512] = {0};
    ASSERT_TRUE(en_vpp_api_observe_event_json(&api, "10.0.2.0/24", "path-api", "route-api", 1000, event, sizeof(event)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(event, "ibuki.event.vpp.route.v1") != NULL);
    en_path_health_t health = {0};
    ASSERT_TRUE(en_telemetry_parse_json_line(event, &health, NULL, 0) == EN_ERR_NONE);
    ASSERT_TRUE(strcmp(health.path_id, "path-api") == 0 && health.state == EN_HEALTH_HEALTHY);
    ASSERT_TRUE(state.observe_count == 2);
    en_vpp_api_ctx_t incomplete = {0};
    ASSERT_TRUE(en_vpp_api_observe_route(&incomplete, "10.0.2.0/24", &observation) == EN_ERR_INVALID_ARGUMENT);
    api.observe_route = vpp_api_bad_observe;
    ASSERT_TRUE(en_vpp_api_observe_route(&api, "10.0.2.0/24", &observation) == EN_ERR_STATE_CONFLICT);
    api.observe_route = vpp_api_bad_value_observe;
    ASSERT_TRUE(en_vpp_api_observe_route(&api, "10.0.2.0/24", &observation) == EN_ERR_STATE_CONFLICT);
    en_vpp_interface_observation_t interface_observation = {0};
    ASSERT_TRUE(en_vpp_api_observe_interface(&api, "host-vpp-site-a.100", &interface_observation) == EN_ERR_NONE);
    ASSERT_TRUE(interface_observation.present && interface_observation.up);
    api.observe_interface = vpp_api_bad_interface_observe;
    ASSERT_TRUE(en_vpp_api_observe_interface(&api, "host-vpp-site-a.100", &interface_observation) == EN_ERR_STATE_CONFLICT);
    en_vpp_interface_observation_t event_interface = {0};
    snprintf(event_interface.interface_name, sizeof(event_interface.interface_name), "%s", "host-vpp-site-a.100");
    event_interface.present = true;
    event_interface.up = true;
    ASSERT_TRUE(en_vpp_interface_observation_to_event_json(&event_interface, "path-api", 1000, event, sizeof(event)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(event, "ibuki.event.vpp.interface.v1") != NULL);
    ASSERT_TRUE(en_telemetry_parse_json_line(event, &health, NULL, 0) == EN_ERR_NONE);
    ASSERT_STREQ(health.observed_interface_name, "host-vpp-site-a.100");
    event_interface.up = false;
    ASSERT_TRUE(en_vpp_interface_observation_to_event_json(&event_interface, "path-api", 1000, event, sizeof(event)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(event, "\"state\":\"down\"") != NULL);
    ASSERT_TRUE(en_telemetry_parse_json_line(event, &health, NULL, 0) == EN_ERR_NONE);
    ASSERT_TRUE(health.state == EN_HEALTH_FAILED);
}

static void test_vpp_api_path_operations_order_and_validation(void)
{
    vpp_message_test_state_t state = {0};
    en_vpp_api_message_ops_t operations = {
        .add_route = vpp_message_add_route,
        .remove_route = vpp_message_remove_route,
        .create_vlan_subinterface = vpp_message_create_vlan,
        .remove_vlan_subinterface = vpp_message_remove_vlan,
        .set_vlan_interface_table = vpp_message_set_vlan_table,
        .ensure_vrf = vpp_message_ensure_vrf,
        .ctx = &state,
    };
    en_path_t path = {0};
    snprintf(path.path_id, sizeof(path.path_id), "%s", "path-api");
    snprintf(path.source, sizeof(path.source), "%s", "site-a");
    snprintf(path.destination, sizeof(path.destination), "%s", "site-b");
    path.route_count = 1;
    snprintf(path.routes[0].route_id, sizeof(path.routes[0].route_id), "%s", "route-api");
    snprintf(path.routes[0].node_id, sizeof(path.routes[0].node_id), "%s", "site-a");
    snprintf(path.routes[0].destination_prefix, sizeof(path.routes[0].destination_prefix), "%s", "10.10.2.0/24");
    snprintf(path.routes[0].next_hop, sizeof(path.routes[0].next_hop), "%s", "203.0.113.9");
    path.routes[0].table_id = 100;
    en_vpp_edge_t edge = {0};
    snprintf(edge.node_id, sizeof(edge.node_id), "%s", "site-a");
    snprintf(edge.vpp_interface, sizeof(edge.vpp_interface), "%s", "host-vpp-site-a");
    en_traffic_selector_t traffic = {.has_vlan_id = true, .vlan_id = 100};
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_NONE);
    ASSERT_STREQ(state.order, "VFTA");
    ASSERT_TRUE(state.create_vlan_count == 1 && state.ensure_vrf_count == 1 && state.add_route_count == 1);
    snprintf(path.waypoints[0], sizeof(path.waypoints[0]), "%s", "hub-1");
    path.waypoint_count = 1;
    en_vpp_edge_t waypoint_edge = {0};
    snprintf(waypoint_edge.node_id, sizeof(waypoint_edge.node_id), "%s", "hub-1");
    snprintf(waypoint_edge.vpp_interface, sizeof(waypoint_edge.vpp_interface), "%s", "host-vpp-hub-1");
    en_vpp_edge_t edges_with_waypoint[2] = {edge, waypoint_edge};
    state.order_length = 0;
    state.order[0] = '\0';
    state.create_vlan_count = 0;
    state.ensure_vrf_count = 0;
    state.add_route_count = 0;
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, edges_with_waypoint, 2, false) == EN_ERR_NONE);
    ASSERT_STREQ(state.order, "VVFTA");
    ASSERT_TRUE(state.create_vlan_count == 2 && state.ensure_vrf_count == 1 && state.add_route_count == 1);
    path.waypoint_count = 0;
    en_vpp_api_message_ops_t missing_vlan_table = operations;
    missing_vlan_table.set_vlan_interface_table = NULL;
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&missing_vlan_table, &path, &traffic, &edge, 1, false) == EN_ERR_INVALID_ARGUMENT);
    snprintf(path.routes[0].interface_name, sizeof(path.routes[0].interface_name), "%s", "host-vpp-missing");
    state.order_length = 0;
    state.order[0] = '\0';
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_NOT_FOUND);
    ASSERT_STREQ(state.order, "");
    path.routes[0].interface_name[0] = '\0';
    en_vpp_api_message_ops_t missing_vrf = operations;
    missing_vrf.ensure_vrf = NULL;
    state.order_length = 0;
    state.order[0] = '\0';
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&missing_vrf, &path, &traffic, &edge, 1, false) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_STREQ(state.order, "");
    path.route_count = 2;
    path.routes[1] = path.routes[0];
    snprintf(path.routes[1].route_id, sizeof(path.routes[1].route_id), "%s", "route-api-default-fib");
    path.routes[1].table_id = 0;
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_INVALID_ARGUMENT);
    path.route_count = 1;
    state.order_length = 0;
    state.order[0] = '\0';
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, true) == EN_ERR_NONE);
    ASSERT_STREQ(state.order, "Rv");
    ASSERT_TRUE(state.remove_route_count == 1 && state.remove_vlan_count == 1);
    operations.add_route = NULL;
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_INVALID_ARGUMENT);
    operations.remove_route = NULL;
    operations.add_route = vpp_message_add_route;
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_INVALID_ARGUMENT);
    operations.remove_route = vpp_message_remove_route;
    path.routes[0].next_hop[0] = '\0';
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_INVALID_ARGUMENT);
    traffic.has_vlan_id = false;
    traffic.vlan_id = -1;
    snprintf(path.routes[0].next_hop, sizeof(path.routes[0].next_hop), "%s", "203.0.113.9");
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_NONE);

    path.route_count = 2;
    path.routes[1] = path.routes[0];
    snprintf(path.routes[1].route_id, sizeof(path.routes[1].route_id), "%s", "route-api-2");
    state.ensure_vrf_count = 0;
    state.add_route_count = 0;
    state.order_length = 0;
    state.order[0] = '\0';
    traffic.vlan_id = 0;
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_NONE);
    ASSERT_TRUE(state.ensure_vrf_count == 1 && state.add_route_count == 2);
    state.add_route_count = 0;
    state.remove_route_count = 0;
    state.remove_vlan_count = 0;
    state.fail_add_route_at = 2;
    state.order_length = 0;
    state.order[0] = '\0';
    traffic.has_vlan_id = true;
    traffic.vlan_id = 100;
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_FORWARDING_UPDATE_FAILED);
    ASSERT_STREQ(state.order, "VFTAARv");
    ASSERT_TRUE(state.remove_route_count == 1 && state.remove_vlan_count == 1);
    state.fail_add_route_at = 0;
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, NULL, 1, false) == EN_ERR_INVALID_ARGUMENT);
    snprintf(path.routes[0].route_id, sizeof(path.routes[0].route_id), "%s", "route;inject");
    ASSERT_TRUE(en_vpp_api_apply_path_operations(&operations, &path, &traffic, &edge, 1, false) == EN_ERR_INVALID_ARGUMENT);
    snprintf(path.routes[0].route_id, sizeof(path.routes[0].route_id), "%s", "route-api");

    en_vpp_api_message_ctx_t message_context = {
        .operations = operations,
        .traffic = traffic,
        .edges = &edge,
        .edge_count = 1,
    };
    en_vpp_adapter_t message_adapter = en_vpp_api_message_adapter(&message_context);
    ASSERT_TRUE(message_adapter.install_path(message_adapter.ctx, "site-a->site-b", &path) == EN_ERR_NONE);
    ASSERT_TRUE(message_adapter.remove_path(message_adapter.ctx, "site-a->site-b", &path) == EN_ERR_NONE);
    ASSERT_TRUE(message_adapter.active_path == NULL);
    edge.allowed_vlan_count = 1;
    edge.allowed_vlans[0] = 200;
    message_context.traffic.has_vlan_id = true;
    message_context.traffic.vlan_id = 100;
    ASSERT_TRUE(message_adapter.install_path(message_adapter.ctx, "site-a->site-b", &path) == EN_ERR_INVALID_ARGUMENT);
}

static void test_vpp_api_transport_validates_lifecycle(void)
{
    en_vpp_api_transport_t transport;
    en_vpp_api_transport_init(&transport);
    ASSERT_TRUE(!en_vpp_api_transport_is_connected(&transport));
    ASSERT_TRUE(en_vpp_api_transport_close(&transport) == EN_ERR_NONE);
    ASSERT_TRUE(en_vpp_api_transport_dispatch(&transport) == EN_ERR_STATE_CONFLICT);
    void *message = NULL;
    ASSERT_TRUE(en_vpp_api_transport_alloc_message(&transport, 8, &message) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_vpp_api_transport_send_message(&transport, (void *)1) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_vpp_api_transport_free_message(&transport, (void *)1) == EN_ERR_INVALID_ARGUMENT);
    bool message_available = true;
    ASSERT_TRUE(en_vpp_api_transport_is_message_available(&transport, 1, &message_available) == EN_ERR_INVALID_ARGUMENT);
    int fd = -1;
    ASSERT_TRUE(en_vpp_api_transport_get_fd(&transport, &fd) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_vpp_api_transport_set_event_callback(&transport, NULL, NULL) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_vpp_api_transport_open(&transport, NULL, NULL, 4, 4) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_vpp_api_transport_open(&transport, "ibuki-test", NULL, 0, 4) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_vpp_api_transport_open(&transport, "ibuki-test", NULL, 4, 0) == EN_ERR_INVALID_ARGUMENT);
#if !defined(EVENTNET_ENABLE_VPP_API)
    ASSERT_TRUE(en_vpp_api_transport_open(&transport, "ibuki-test", NULL, 4, 4) == EN_ERR_NOT_FOUND);
    ASSERT_TRUE(en_vpp_api_transport_get_fd(&transport, &fd) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_vpp_api_transport_set_event_callback(&transport, NULL, NULL) == EN_ERR_INVALID_ARGUMENT);
#endif
    ASSERT_TRUE(!en_vpp_api_transport_is_connected(&transport));
}

static void test_strongswan_vici_adapter_forwards_callbacks(void)
{
    en_strongswan_vici_ctx_t bind_context = {0};
    ASSERT_TRUE(en_strongswan_vici_bind_client(NULL, NULL) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(en_strongswan_vici_bind_client(&bind_context, NULL) == EN_ERR_INVALID_ARGUMENT);
    vici_test_state_t state = {0};
    en_strongswan_vici_ctx_t vici = {
        .ensure_tunnel = vici_test_ensure,
        .remove_tunnel = vici_test_remove,
        .observe_tunnel = vici_test_observe,
        .ctx = &state,
    };
    en_strongswan_adapter_t adapter = en_strongswan_vici_adapter(&vici);
    en_tunnel_t desired = {0};
    en_tunnel_t observed = {0};
    snprintf(desired.tunnel_id, sizeof(desired.tunnel_id), "%s", "tun-vici");
    ASSERT_TRUE(adapter.ensure_tunnel(adapter.ctx, &desired, &observed) == EN_ERR_NONE);
    ASSERT_TRUE(adapter.remove_tunnel(adapter.ctx, &desired, &observed) == EN_ERR_NONE);
    ASSERT_TRUE(state.ensure_count == 1 && state.remove_count == 1);
    en_strongswan_sa_observation_t observation = {0};
    ASSERT_TRUE(en_strongswan_vici_observe_tunnel(&vici, "tun-vici", &observation) == EN_ERR_NONE);
    ASSERT_TRUE(strcmp(observation.child_id, "tun-vici") == 0);
    ASSERT_TRUE(observation.state == EN_TUNNEL_ESTABLISHED && observation.health == EN_HEALTH_HEALTHY);
    ASSERT_TRUE(state.observe_count == 1);
    char event[512] = {0};
    ASSERT_TRUE(en_strongswan_vici_observe_event_json(&vici, "tun-vici", "path-api", 1000, event, sizeof(event)) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(event, "ibuki.event.tunnel.v1") != NULL);
    en_path_health_t health = {0};
    ASSERT_TRUE(en_telemetry_parse_json_line(event, &health, NULL, 0) == EN_ERR_NONE);
    ASSERT_TRUE(strcmp(health.path_id, "path-api") == 0 && health.state == EN_HEALTH_HEALTHY);
    ASSERT_TRUE(state.observe_count == 2);
    en_strongswan_vici_ctx_t incomplete = {0};
    ASSERT_TRUE(en_strongswan_vici_observe_tunnel(&incomplete, "tun-vici", &observation) == EN_ERR_INVALID_ARGUMENT);
    vici.ensure_tunnel = vici_api_bad_ensure;
    adapter = en_strongswan_vici_adapter(&vici);
    ASSERT_TRUE(adapter.ensure_tunnel(adapter.ctx, &desired, &observed) == EN_ERR_STATE_CONFLICT);
    vici.observe_tunnel = vici_api_bad_observe;
    ASSERT_TRUE(en_strongswan_vici_observe_tunnel(&vici, "tun-vici", &observation) == EN_ERR_STATE_CONFLICT);
}

static void test_apply_plan_generates_swanctl_conf_and_commands(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.tunnels[0].tunnel_id, sizeof(config.tunnels[0].tunnel_id), "%s", "tun-a-b");
    snprintf(config.tunnels[0].local_node, sizeof(config.tunnels[0].local_node), "%s", "site-a");
    snprintf(config.tunnels[0].remote_node, sizeof(config.tunnels[0].remote_node), "%s", "site-b");
    snprintf(config.tunnels[0].local_endpoint, sizeof(config.tunnels[0].local_endpoint), "%s", "203.0.113.10");
    snprintf(config.tunnels[0].remote_endpoint, sizeof(config.tunnels[0].remote_endpoint), "%s", "203.0.113.20");
    snprintf(config.tunnels[0].local_traffic_selector, sizeof(config.tunnels[0].local_traffic_selector), "%s", "10.0.1.0/24");
    snprintf(config.tunnels[0].remote_traffic_selector, sizeof(config.tunnels[0].remote_traffic_selector), "%s", "10.0.2.0/24");
    config.tunnel_count = 1;

    snprintf(config.paths[0].path_id, sizeof(config.paths[0].path_id), "%s", "path-direct");
    snprintf(config.paths[0].source, sizeof(config.paths[0].source), "%s", "site-a");
    snprintf(config.paths[0].destination, sizeof(config.paths[0].destination), "%s", "site-b");
    snprintf(config.paths[0].route_destination_prefix, sizeof(config.paths[0].route_destination_prefix), "%s", "10.0.2.0/24");
    snprintf(config.paths[0].egress_tunnel_id, sizeof(config.paths[0].egress_tunnel_id), "%s", "tun-a-b");
    snprintf(config.paths[0].segments[0].segment_id, sizeof(config.paths[0].segments[0].segment_id), "%s", "seg-a-b");
    snprintf(config.paths[0].segments[0].tunnel_id, sizeof(config.paths[0].segments[0].tunnel_id), "%s", "tun-a-b");
    config.paths[0].segment_count = 1;
    config.path_count = 1;

    snprintf(config.intents[0].intent_id, sizeof(config.intents[0].intent_id), "%s", "intent-a-b");
    config.intents[0].path_selection.mode = EN_SELECT_EXPLICIT;
    snprintf(config.intents[0].path_selection.path_id, sizeof(config.intents[0].path_selection.path_id), "%s", "path-direct");
    config.intent_count = 1;

    char error[256] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NONE);

    en_apply_plan_t plan = {0};
    ASSERT_TRUE(en_apply_plan_from_config(&config, &config.intents[0], &config.paths[0], &plan) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(plan.swanctl_conf, "connections") != NULL);
    ASSERT_TRUE(strstr(plan.swanctl_conf, "tun-a-b") != NULL);
    ASSERT_TRUE(plan.command_count == 3);
    ASSERT_STREQ(plan.commands[0], "swanctl --load-conns --file eventnet-swanctl.conf");
    ASSERT_STREQ(plan.commands[1], "swanctl --initiate --child tun-a-b");
    ASSERT_STREQ(plan.commands[2], "vppctl ip route add 10.0.2.0/24 via 203.0.113.20");
    ASSERT_TRUE(plan.rollback_command_count == 2);
    ASSERT_STREQ(plan.rollback_commands[0], "vppctl ip route del 10.0.2.0/24");
    ASSERT_STREQ(plan.rollback_commands[1], "swanctl --terminate --child tun-a-b");
    ASSERT_TRUE(plan.has_command_rollbacks && !plan.command_has_rollback[0] && plan.command_has_rollback[1] && plan.command_has_rollback[2]);
    ASSERT_STREQ(plan.command_rollbacks[1], "swanctl --terminate --child tun-a-b");
    ASSERT_STREQ(plan.command_rollbacks[2], "vppctl ip route del 10.0.2.0/24");

    config.intents[0].block_non_ipsec = true;
    memset(&plan, 0, sizeof(plan));
    ASSERT_TRUE(en_apply_plan_from_config(&config, &config.intents[0], &config.paths[0], &plan) == EN_ERR_NONE);
    ASSERT_TRUE(plan.command_count == 5);
    ASSERT_STREQ(plan.commands[2], "ip xfrm policy add dir out src 10.0.1.0/24 dst 10.0.2.0/24 priority 10000 action block");
    ASSERT_STREQ(plan.commands[3], "ip xfrm policy add dir in src 10.0.2.0/24 dst 10.0.1.0/24 priority 10000 action block");
    ASSERT_TRUE(plan.rollback_command_count == 4);
    ASSERT_STREQ(plan.rollback_commands[2], "ip xfrm policy delete dir out src 10.0.1.0/24 dst 10.0.2.0/24 priority 10000");
    ASSERT_STREQ(plan.rollback_commands[3], "ip xfrm policy delete dir in src 10.0.2.0/24 dst 10.0.1.0/24 priority 10000");
    ASSERT_TRUE(plan.has_command_rollbacks && plan.command_has_rollback[1] && plan.command_has_rollback[2] && plan.command_has_rollback[3] && plan.command_has_rollback[4]);
    ASSERT_TRUE(en_apply_plan_write_shell_script(&plan, "eventnet-test-apply.sh") == EN_ERR_NONE);
    remove("eventnet-test-apply.sh");

    snprintf(config.tunnels[0].auth_method, sizeof(config.tunnels[0].auth_method), "%s", "pubkey");
    snprintf(config.tunnels[0].local_cert, sizeof(config.tunnels[0].local_cert), "%s", "site-a.crt");
    snprintf(config.tunnels[0].remote_cacerts, sizeof(config.tunnels[0].remote_cacerts), "%s", "ca.crt");
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NONE);
    memset(&plan, 0, sizeof(plan));
    ASSERT_TRUE(en_apply_plan_from_config(&config, &config.intents[0], &config.paths[0], &plan) == EN_ERR_NONE);
    ASSERT_TRUE(strstr(plan.swanctl_conf, "auth = pubkey") != NULL);
    ASSERT_TRUE(strstr(plan.swanctl_conf, "certs = site-a.crt") != NULL);
    ASSERT_TRUE(strstr(plan.swanctl_conf, "cacerts = ca.crt") != NULL);
}

static void test_apply_plan_execution_boundary(void)
{
    en_apply_plan_t plan = {0};
    snprintf(plan.commands[0], sizeof(plan.commands[0]), "%s", "true");
    plan.command_count = 1;
    ASSERT_TRUE(en_apply_plan_run(&plan, true) == EN_ERR_NONE);
#if !defined(_WIN32)
    ASSERT_TRUE(en_apply_plan_run(&plan, false) == EN_ERR_NONE);
    snprintf(plan.commands[0], sizeof(plan.commands[0]), "%s", "true;echo injected");
    ASSERT_TRUE(en_apply_plan_run(&plan, false) == EN_ERR_STATE_CONFLICT);
    memset(&plan, 0, sizeof(plan));
    snprintf(plan.commands[0], sizeof(plan.commands[0]), "%s", "false");
    snprintf(plan.rollback_commands[0], sizeof(plan.rollback_commands[0]), "%s", "true");
    plan.command_count = 1;
    plan.rollback_command_count = 1;
    ASSERT_TRUE(en_apply_plan_run(&plan, false) == EN_ERR_STATE_CONFLICT);
    snprintf(plan.rollback_commands[0], sizeof(plan.rollback_commands[0]), "%s", "false");
    ASSERT_TRUE(en_apply_plan_run(&plan, false) == EN_ERR_ROLLBACK_FAILED);
    memset(&plan, 0, sizeof(plan));
    snprintf(plan.commands[0], sizeof(plan.commands[0]), "%s", "true");
    snprintf(plan.commands[1], sizeof(plan.commands[1]), "%s", "false");
    snprintf(plan.rollback_commands[0], sizeof(plan.rollback_commands[0]), "%s", "false");
    snprintf(plan.command_rollbacks[0], sizeof(plan.command_rollbacks[0]), "%s", "true");
    plan.command_count = 2;
    plan.rollback_command_count = 1;
    plan.command_has_rollback[0] = true;
    plan.has_command_rollbacks = true;
    ASSERT_TRUE(en_apply_plan_run(&plan, false) == EN_ERR_STATE_CONFLICT);
#endif
}

static void test_path_selection_thresholds_prevent_flapping(void)
{
    en_path_t paths[2] = {0};
    snprintf(paths[0].path_id, sizeof(paths[0].path_id), "%s", "path-direct");
    snprintf(paths[0].source, sizeof(paths[0].source), "%s", "site-a");
    snprintf(paths[0].destination, sizeof(paths[0].destination), "%s", "site-b");
    paths[0].administrative_state = EN_ADMIN_ENABLED;
    snprintf(paths[1].path_id, sizeof(paths[1].path_id), "%s", "path-hub");
    snprintf(paths[1].source, sizeof(paths[1].source), "%s", "site-a");
    snprintf(paths[1].destination, sizeof(paths[1].destination), "%s", "site-b");
    paths[1].administrative_state = EN_ADMIN_ENABLED;

    en_health_probe_mock_t health = {0};
    en_vpp_mock_t vpp = {0};
    en_intent_t intent = {0};
    snprintf(intent.intent_id, sizeof(intent.intent_id), "%s", "intent-threshold");
    snprintf(intent.traffic.source, sizeof(intent.traffic.source), "%s", "site-a");
    snprintf(intent.traffic.destination, sizeof(intent.traffic.destination), "%s", "site-b");
    intent.path_selection.mode = EN_SELECT_PRIORITY;
    snprintf(intent.path_selection.candidates[0], sizeof(intent.path_selection.candidates[0]), "%s", "path-direct");
    snprintf(intent.path_selection.candidates[1], sizeof(intent.path_selection.candidates[1]), "%s", "path-hub");
    intent.path_selection.candidate_count = 2;
    intent.path_selection.constraints.failure_threshold = 2;
    intent.path_selection.constraints.recovery_threshold = 2;

    en_path_health_t direct = {0};
    snprintf(direct.path_id, sizeof(direct.path_id), "%s", "path-direct");
    direct.state = EN_HEALTH_HEALTHY;
    direct.consecutive_successes = 2;
    en_health_probe_mock_set(&health, direct);
    en_path_health_t hub = {0};
    snprintf(hub.path_id, sizeof(hub.path_id), "%s", "path-hub");
    hub.state = EN_HEALTH_HEALTHY;
    hub.consecutive_successes = 2;
    en_health_probe_mock_set(&health, hub);

    en_controller_t *controller = en_controller_create(paths, 2, en_strongswan_mock_adapter(),
        en_vpp_mock_adapter(&vpp), en_health_probe_mock_adapter(&health));
    ASSERT_TRUE(controller != NULL);
    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-direct");

    direct.state = EN_HEALTH_FAILED;
    direct.consecutive_failures = 1;
    direct.consecutive_successes = 0;
    en_health_probe_mock_set(&health, direct);
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-direct");

    direct.consecutive_failures = 2;
    en_health_probe_mock_set(&health, direct);
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-hub");

    direct.state = EN_HEALTH_HEALTHY;
    direct.consecutive_failures = 0;
    direct.consecutive_successes = 1;
    en_health_probe_mock_set(&health, direct);
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-hub");

    direct.consecutive_successes = 2;
    en_health_probe_mock_set(&health, direct);
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-direct");
    en_controller_destroy(controller);
}

static void test_hold_down_prevents_healthy_path_switch(void)
{
    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = make_controller(&vpp_mock, &health_mock);
    en_intent_t intent = base_intent(EN_SELECT_EXPLICIT);
    snprintf(intent.path_selection.path_id, sizeof(intent.path_selection.path_id), "%s", "path-direct");
    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    intent.path_selection.constraints.hold_down_ms = 60000;
    snprintf(intent.path_selection.path_id, sizeof(intent.path_selection.path_id), "%s", "path-via-hub");
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NO_CANDIDATE);
    ASSERT_STREQ(result.explanation.excluded_reasons[0], "active path hold-down not elapsed");
    intent.path_selection.constraints.hold_down_ms = 0;
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-via-hub");
    en_controller_destroy(controller);
}

static void test_evaluated_hysteresis_prevents_small_quality_switch(void)
{
    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = make_controller(&vpp_mock, &health_mock);
    en_intent_t initial = base_intent(EN_SELECT_EXPLICIT);
    snprintf(initial.path_selection.path_id, sizeof(initial.path_selection.path_id), "%s", "path-direct");
    en_reconcile_result_t initial_result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &initial, &initial_result) == EN_ERR_NONE);
    en_intent_t intent = base_intent(EN_SELECT_EVALUATED);
    snprintf(intent.path_selection.candidates[0], sizeof(intent.path_selection.candidates[0]), "%s", "path-direct");
    snprintf(intent.path_selection.candidates[1], sizeof(intent.path_selection.candidates[1]), "%s", "path-via-relay-c");
    intent.path_selection.candidate_count = 2;
    intent.path_selection.comparison_order[0] = EN_COMPARE_LATENCY;
    intent.path_selection.comparison_count = 1;
    intent.path_selection.constraints.has_hysteresis_percent = true;
    intent.path_selection.constraints.hysteresis_percent = 20.0;
    en_path_health_t direct = {0};
    snprintf(direct.path_id, sizeof(direct.path_id), "%s", "path-direct");
    direct.state = EN_HEALTH_HEALTHY;
    direct.rtt_ms = 10.0;
    direct.consecutive_successes = 3;
    en_path_health_t relay = direct;
    snprintf(relay.path_id, sizeof(relay.path_id), "%s", "path-via-relay-c");
    relay.rtt_ms = 9.0;
    en_health_probe_mock_set(&health_mock, direct);
    en_health_probe_mock_set(&health_mock, relay);
    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-direct");
    relay.rtt_ms = 5.0;
    en_health_probe_mock_set(&health_mock, relay);
    ASSERT_TRUE(en_controller_submit_intent(controller, &intent, &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-via-relay-c");
    en_controller_destroy(controller);
}

static void test_yaml_validation_rejects_unknown_tunnel(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.paths[0].path_id, sizeof(config.paths[0].path_id), "%s", "path-bad");
    snprintf(config.paths[0].source, sizeof(config.paths[0].source), "%s", "site-a");
    snprintf(config.paths[0].destination, sizeof(config.paths[0].destination), "%s", "site-b");
    snprintf(config.paths[0].segments[0].tunnel_id, sizeof(config.paths[0].segments[0].tunnel_id), "%s", "missing-tunnel");
    config.paths[0].segment_count = 1;
    config.path_count = 1;
    char error[256] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
}

static void test_yaml_loader_rejects_truncated_values(void)
{
    const char *filename = "eventnet-test-overlong.yaml";
    FILE *file = fopen(filename, "w");
    ASSERT_TRUE(file != NULL);
    fputs("tunnels:\n  - id: tun-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n", file);
    fclose(file);

    en_yaml_config_t config = {0};
    char error[128] = {0};
    ASSERT_TRUE(en_yaml_config_load_file(filename, &config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    remove(filename);
}

static void test_yaml_config_loads_route_variants(void)
{
    en_yaml_config_t config = {0};
    char error[256] = {0};
    en_error_code_t load_err = en_yaml_config_load_file("samples/route-examples.yaml", &config, error, sizeof(error));
    if (load_err != EN_ERR_NONE) {
        load_err = en_yaml_config_load_file("../samples/route-examples.yaml", &config, error, sizeof(error));
    }
    ASSERT_TRUE(load_err == EN_ERR_NONE);
    ASSERT_TRUE(config.path_count == 8);

    const en_path_t *legacy = NULL;
    const en_path_t *bidir = NULL;
    const en_path_t *hub = NULL;
    const en_path_t *attrs = NULL;
    const en_path_t *chain = NULL;
    const en_path_t *asymmetric = NULL;
    const en_path_t *backup = NULL;
    for (size_t i = 0; i < config.path_count; i++) {
        if (strcmp(config.paths[i].path_id, "path-legacy-single-route") == 0) {
            legacy = &config.paths[i];
        } else if (strcmp(config.paths[i].path_id, "path-bidirectional-routes") == 0) {
            bidir = &config.paths[i];
        } else if (strcmp(config.paths[i].path_id, "path-hub-node-routes") == 0) {
            hub = &config.paths[i];
        } else if (strcmp(config.paths[i].path_id, "path-route-attributes") == 0) {
            attrs = &config.paths[i];
        } else if (strcmp(config.paths[i].path_id, "path-relay-chain-routes") == 0) {
            chain = &config.paths[i];
        } else if (strcmp(config.paths[i].path_id, "path-asymmetric-routes") == 0) {
            asymmetric = &config.paths[i];
        } else if (strcmp(config.paths[i].path_id, "path-priority-backup") == 0) {
            backup = &config.paths[i];
        }
    }

    ASSERT_TRUE(legacy != NULL);
    ASSERT_TRUE(legacy->route_count == 1);
    ASSERT_TRUE(!legacy->routes_explicit);
    ASSERT_STREQ(legacy->routes[0].destination_prefix, "10.10.2.0/24");

    ASSERT_TRUE(bidir != NULL);
    ASSERT_TRUE(bidir->route_count == 2);
    ASSERT_TRUE(bidir->routes_explicit);
    ASSERT_STREQ(bidir->routes[1].node_id, "site-b");
    ASSERT_STREQ(bidir->routes[1].destination_prefix, "10.10.1.0/24");

    ASSERT_TRUE(hub != NULL);
    ASSERT_TRUE(hub->route_count == 4);
    ASSERT_STREQ(hub->routes[2].node_id, "hub-1");
    ASSERT_STREQ(hub->routes[2].next_hop, "203.0.113.18");

    ASSERT_TRUE(attrs != NULL);
    ASSERT_TRUE(attrs->route_count == 1);
    ASSERT_STREQ(attrs->routes[0].interface_name, "ipsec0");
    ASSERT_TRUE(attrs->routes[0].table_id == 100);
    ASSERT_TRUE(attrs->routes[0].metric == 20);

    ASSERT_TRUE(chain != NULL);
    ASSERT_TRUE(chain->route_count == 4);
    ASSERT_STREQ(chain->routes[1].node_id, "relay-c");
    ASSERT_STREQ(chain->routes[1].next_hop, "203.0.113.25");

    ASSERT_TRUE(asymmetric != NULL);
    ASSERT_TRUE(asymmetric->route_count == 3);
    ASSERT_STREQ(asymmetric->routes[2].destination_prefix, "10.10.1.0/24");

    ASSERT_TRUE(backup != NULL);
    ASSERT_TRUE(backup->route_count == 1);

    const en_intent_t *vlan_hub = NULL;
    const en_intent_t *vlan_direct = NULL;
    for (size_t i = 0; i < config.intent_count; i++) {
        if (strcmp(config.intents[i].intent_id, "intent-vlan-security-hub") == 0) {
            vlan_hub = &config.intents[i];
        } else if (strcmp(config.intents[i].intent_id, "intent-vlan-direct") == 0) {
            vlan_direct = &config.intents[i];
        }
    }
    ASSERT_TRUE(vlan_hub != NULL);
    ASSERT_TRUE(vlan_hub->traffic.has_vlan_id);
    ASSERT_TRUE(vlan_hub->traffic.vlan_id == 100);
    ASSERT_TRUE(vlan_hub->path_selection.constraints.required_waypoint_count == 1);
    ASSERT_STREQ(vlan_hub->path_selection.constraints.required_waypoints[0], "hub-1");
    ASSERT_TRUE(vlan_direct != NULL);
    ASSERT_TRUE(vlan_direct->traffic.vlan_id == 200);
    ASSERT_TRUE(vlan_direct->block_non_ipsec);
    ASSERT_TRUE(vlan_direct->path_selection.constraints.failure_threshold == 3);
    ASSERT_TRUE(vlan_direct->path_selection.constraints.recovery_threshold == 2);

    const en_intent_t *stability = NULL;
    for (size_t i = 0; i < config.intent_count; i++) {
        if (strcmp(config.intents[i].intent_id, "intent-evaluated-stability") == 0) stability = &config.intents[i];
    }
    ASSERT_TRUE(stability != NULL);
    ASSERT_TRUE(stability->path_selection.mode == EN_SELECT_EVALUATED);
    ASSERT_TRUE(stability->path_selection.constraints.has_hysteresis_percent);
    ASSERT_TRUE(stability->path_selection.constraints.hysteresis_percent == 15.0);
    ASSERT_TRUE(stability->path_selection.constraints.hold_down_ms == 0);
    ASSERT_TRUE(stability->transition.retry_count == 1);
    ASSERT_TRUE(stability->transition.retry_backoff_ms == 25);

    char vlan_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&vlan_hub->traffic, vlan_key, sizeof(vlan_key));
    ASSERT_STREQ(vlan_key, "site-a->site-b|vlan=100");

    char command[EN_MAX_COMMAND_LEN] = {0};
    ASSERT_TRUE(en_render_vpp_route_replace_entry(&attrs->routes[0], command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl ip route add 10.10.2.0/24 table 100 preference 20 via 203.0.113.9 ipsec0");
    ASSERT_TRUE(en_render_vpp_route_delete_entry(&attrs->routes[0], command, sizeof(command)) == EN_ERR_NONE);
    ASSERT_STREQ(command, "vppctl ip route del 10.10.2.0/24 table 100 via 203.0.113.9 ipsec0");
}

static en_error_code_t load_yaml_sample(const char *filename, en_yaml_config_t *config, char *error, size_t error_len)
{
    en_error_code_t load_err = en_yaml_config_load_file(filename, config, error, error_len);
    if (load_err != EN_ERR_NOT_FOUND) {
        return load_err;
    }

    char relative_filename[256] = {0};
    snprintf(relative_filename, sizeof(relative_filename), "../%s", filename);
    return en_yaml_config_load_file(relative_filename, config, error, error_len);
}

static void test_yaml_loader_segmentless_route_boundary(void)
{
    en_yaml_config_t config = {0};
    char error[256] = {0};
    ASSERT_TRUE(load_yaml_sample("samples/agent-legacy-no-segment.yaml", &config, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(config.path_count == 1);
    ASSERT_TRUE(config.paths[0].segment_count == 0);
    ASSERT_TRUE(!config.paths[0].routes_explicit);
    ASSERT_TRUE(config.paths[0].route_count == 1);
    ASSERT_STREQ(config.paths[0].routes[0].next_hop, "192.0.2.2");

    config = (en_yaml_config_t){0};
    error[0] = '\0';
    ASSERT_TRUE(load_yaml_sample("samples/legacy-explicit-no-segment.yaml", &config, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(config.path_count == 1);
    ASSERT_TRUE(config.paths[0].segment_count == 0);
    ASSERT_TRUE(config.paths[0].routes_explicit);
    ASSERT_TRUE(config.paths[0].route_count == 1);

    config = (en_yaml_config_t){0};
    error[0] = '\0';
    ASSERT_TRUE(load_yaml_sample("samples/invalid-empty-no-segment.yaml", &config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    ASSERT_TRUE(strstr(error, "path without segments requires a legacy route or explicit routes") != NULL);
}

static void test_yaml_config_loads_vpp_vlan_allowlist(void)
{
    en_yaml_config_t config = {0};
    char error[256] = {0};
    en_error_code_t load_err = en_yaml_config_load_file("samples/vpp-vlan-netns.yaml", &config, error, sizeof(error));
    if (load_err != EN_ERR_NONE) {
        load_err = en_yaml_config_load_file("../samples/vpp-vlan-netns.yaml", &config, error, sizeof(error));
    }
    ASSERT_TRUE(load_err == EN_ERR_NONE);
    ASSERT_TRUE(config.vpp_edge_count == 2);
    ASSERT_TRUE(config.vpp_edges[0].allowed_vlan_count == 3);
    ASSERT_TRUE(config.vpp_edges[0].allowed_vlans[0] == 1);
    ASSERT_TRUE(config.vpp_edges[0].allowed_vlans[1] == 100);
    ASSERT_TRUE(config.vpp_edges[0].allowed_vlans[2] == 4094);
    snprintf(config.paths[0].routes[0].interface_name, sizeof(config.paths[0].routes[0].interface_name), "%s", "host-vpp-missing");
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
}

static void test_yaml_loader_rejects_invalid_vpp_vlan_allowlist(void)
{
    const char *filename = "eventnet-test-invalid-vlan-list.yaml";
    const char *prefix = "vpp_edges:\n  - node_id: site-a\n    vpp_interface: host-vpp-site-a\n    next_hop: 172.16.1.2\n    allowed_vlans:\n";
    const char *cases[] = {"      - 100\n      - 100\n", "      - 4095\n"};
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        FILE *file = fopen(filename, "w");
        ASSERT_TRUE(file != NULL);
        fputs(prefix, file);
        fputs(cases[index], file);
        fclose(file);
        en_yaml_config_t config = {0};
        char error[128] = {0};
        ASSERT_TRUE(en_yaml_config_load_file(filename, &config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    }
    remove(filename);
}

static void test_yaml_loader_supports_multiport_vpp_edges(void)
{
    const char *filename = "eventnet-test-multiport-vpp.yaml";
    FILE *file = fopen(filename, "w");
    ASSERT_TRUE(file != NULL);
    fputs("vpp_edges:\n"
          "  - node_id: hub-1\n"
          "    port_id: hub-a\n"
          "    vpp_interface: host-vpp-hub-a\n"
          "    next_hop: 172.16.3.2\n"
          "  - node_id: hub-1\n"
          "    port_id: hub-b\n"
          "    vpp_interface: host-vpp-hub-b\n"
          "    next_hop: 172.16.4.2\n"
          "paths:\n"
          "  - id: path-a-b\n"
          "    source: site-a\n"
          "    destination: site-b\n"
          "    routes:\n"
          "      - id: route-a-b\n"
          "        node_id: hub-1\n"
          "        destination_prefix: 10.0.2.0/24\n"
          "        next_hop: 172.16.3.2\n", file);
    fclose(file);
    en_yaml_config_t config = {0};
    char error[256] = {0};
    ASSERT_TRUE(en_yaml_config_load_file(filename, &config, error, sizeof(error)) == EN_ERR_NONE);
    ASSERT_TRUE(config.vpp_edge_count == 2);
    ASSERT_STREQ(config.vpp_edges[0].port_id, "hub-a");
    ASSERT_STREQ(config.vpp_edges[1].port_id, "hub-b");
    snprintf(config.vpp_edges[1].port_id, sizeof(config.vpp_edges[1].port_id), "%s", "hub-b;inject");
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    snprintf(config.vpp_edges[1].port_id, sizeof(config.vpp_edges[1].port_id), "%s", "hub-a");
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    snprintf(config.vpp_edges[1].port_id, sizeof(config.vpp_edges[1].port_id), "%s", "hub-b");
    snprintf(config.vpp_edges[1].vpp_interface, sizeof(config.vpp_edges[1].vpp_interface), "%s", "host-vpp-hub-a");
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    remove(filename);
}

static void test_yaml_loader_rejects_unknown_vpp_route_interface(void)
{
    const char *filename = "eventnet-test-unknown-vpp-route-interface.yaml";
    FILE *file = fopen(filename, "w");
    ASSERT_TRUE(file != NULL);
    fputs("vpp_edges:\n"
          "  - node_id: site-a\n"
          "    port_id: edge\n"
          "    vpp_interface: host-vpp-site-a\n"
          "    next_hop: 172.16.1.2\n"
          "paths:\n"
          "  - id: path-a-b\n"
          "    source: site-a\n"
          "    destination: site-b\n"
          "    routes:\n"
          "      - id: route-a-b\n"
          "        node_id: site-a\n"
          "        destination_prefix: 10.10.2.0/24\n"
          "        next_hop: 172.16.1.2\n"
          "        interface: host-vpp-missing\n", file);
    fclose(file);
    en_yaml_config_t config = {0};
    char error[256] = {0};
    ASSERT_TRUE(en_yaml_config_load_file(filename, &config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    remove(filename);
}

static void test_yaml_config_loads_node_capabilities(void)
{
    en_yaml_config_t config = {0};
    char error[256] = {0};
    en_error_code_t load_err = en_yaml_config_load_file("samples/node-capabilities.yaml", &config, error, sizeof(error));
    if (load_err != EN_ERR_NONE) {
        load_err = en_yaml_config_load_file("../samples/node-capabilities.yaml", &config, error, sizeof(error));
    }
    ASSERT_TRUE(load_err == EN_ERR_NONE);
    ASSERT_TRUE(config.node_count == 2);
    ASSERT_STREQ(config.nodes[0].node_id, "site-a");
    ASSERT_STREQ(config.nodes[0].role, "edge");
    ASSERT_TRUE(config.nodes[0].endpoint_count == 1);
    ASSERT_STREQ(config.nodes[0].endpoints[0], "203.0.113.10");
    ASSERT_TRUE(config.nodes[0].capability_count == 2);
    ASSERT_STREQ(config.nodes[0].capabilities[1], "vpp");
    ASSERT_STREQ(config.nodes[1].role, "cloud-gateway");
    ASSERT_STREQ(config.nodes[1].capabilities[1], "cloud-vpn");
    ASSERT_TRUE(config.intents[0].path_selection.constraints.required_capability_count == 1);
    ASSERT_STREQ(config.intents[0].path_selection.constraints.required_capabilities[0], "ipsec");

    config.paths[0].destination[0] = 'x';
    config.paths[0].destination[1] = '\0';
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    snprintf(config.paths[0].destination, sizeof(config.paths[0].destination), "%s", "site-b");
    snprintf(config.paths[0].waypoints[0], sizeof(config.paths[0].waypoints[0]), "%s", "unknown-node");
    config.paths[0].waypoint_count = 1;
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    config.paths[0].waypoint_count = 0;
    snprintf(config.intents[0].traffic.source, sizeof(config.intents[0].traffic.source), "%s", "unknown-node");
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    snprintf(config.intents[0].traffic.source, sizeof(config.intents[0].traffic.source), "%s", "site-a");
    snprintf(config.intents[0].traffic.destination, sizeof(config.intents[0].traffic.destination), "%s", "unknown-node");
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    snprintf(config.intents[0].traffic.destination, sizeof(config.intents[0].traffic.destination), "%s", "site-b");
    snprintf(config.intents[0].path_selection.constraints.required_waypoints[0],
        sizeof(config.intents[0].path_selection.constraints.required_waypoints[0]), "%s", "unknown-node");
    config.intents[0].path_selection.constraints.required_waypoint_count = 1;
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    config.intents[0].path_selection.constraints.required_waypoint_count = 0;
    snprintf(config.intents[0].path_selection.constraints.forbidden_waypoints[0],
        sizeof(config.intents[0].path_selection.constraints.forbidden_waypoints[0]), "%s", "unknown-node");
    config.intents[0].path_selection.constraints.forbidden_waypoint_count = 1;
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_NOT_FOUND);
    config.intents[0].path_selection.constraints.forbidden_waypoint_count = 0;

    en_health_probe_mock_t health_mock = {0};
    en_vpp_mock_t vpp_mock = {0};
    en_controller_t *controller = en_controller_create_with_nodes_and_tunnels(
        config.nodes, config.node_count, config.paths, config.path_count,
        config.tunnels, config.tunnel_count, en_strongswan_mock_adapter(),
        en_vpp_mock_adapter(&vpp_mock), en_health_probe_mock_adapter(&health_mock));
    ASSERT_TRUE(controller != NULL);
    en_reconcile_result_t result = {0};
    ASSERT_TRUE(en_controller_submit_intent(controller, &config.intents[0], &result) == EN_ERR_NONE);
    ASSERT_STREQ(result.selected_path, "path-a-b");
    en_controller_destroy(controller);

    snprintf(config.nodes[2].node_id, sizeof(config.nodes[2].node_id), "%s", "hub-1");
    config.nodes[2].administrative_state = EN_ADMIN_DISABLED;
    config.node_count = 3;
    snprintf(config.paths[0].waypoints[0], sizeof(config.paths[0].waypoints[0]), "%s", "hub-1");
    config.paths[0].waypoint_count = 1;
    controller = en_controller_create_with_nodes_and_tunnels(
        config.nodes, config.node_count, config.paths, config.path_count,
        config.tunnels, config.tunnel_count, en_strongswan_mock_adapter(),
        en_vpp_mock_adapter(&vpp_mock), en_health_probe_mock_adapter(&health_mock));
    ASSERT_TRUE(controller != NULL);
    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(en_controller_submit_intent(controller, &config.intents[0], &result) == EN_ERR_NO_CANDIDATE);
    ASSERT_STREQ(result.explanation.excluded_reasons[0], "path node is administratively disabled");
    en_controller_destroy(controller);

    config.nodes[2].administrative_state = EN_ADMIN_ENABLED;
    config.nodes[1].administrative_state = EN_ADMIN_DISABLED;
    controller = en_controller_create_with_nodes_and_tunnels(
        config.nodes, config.node_count, config.paths, config.path_count,
        config.tunnels, config.tunnel_count, en_strongswan_mock_adapter(),
        en_vpp_mock_adapter(&vpp_mock), en_health_probe_mock_adapter(&health_mock));
    ASSERT_TRUE(controller != NULL);
    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(en_controller_submit_intent(controller, &config.intents[0], &result) == EN_ERR_NO_CANDIDATE);
    ASSERT_STREQ(result.explanation.excluded_reasons[0], "path node is administratively disabled");
    en_controller_destroy(controller);

    config.paths[0].waypoint_count = 0;
    config.node_count = 2;
    config.nodes[1].administrative_state = EN_ADMIN_ENABLED;
    snprintf(config.nodes[0].capabilities[0], sizeof(config.nodes[0].capabilities[0]), "%s", "vpp");
    controller = en_controller_create_with_nodes_and_tunnels(
        config.nodes, config.node_count, config.paths, config.path_count,
        config.tunnels, config.tunnel_count, en_strongswan_mock_adapter(),
        en_vpp_mock_adapter(&vpp_mock), en_health_probe_mock_adapter(&health_mock));
    ASSERT_TRUE(controller != NULL);
    memset(&result, 0, sizeof(result));
    ASSERT_TRUE(en_controller_submit_intent(controller, &config.intents[0], &result) == EN_ERR_NO_CANDIDATE);
    en_controller_destroy(controller);
}

static void test_yaml_validation_rejects_duplicate_path_id(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.tunnels[0].tunnel_id, sizeof(config.tunnels[0].tunnel_id), "%s", "tun-a-b");
    snprintf(config.tunnels[0].local_node, sizeof(config.tunnels[0].local_node), "%s", "site-a");
    snprintf(config.tunnels[0].remote_node, sizeof(config.tunnels[0].remote_node), "%s", "site-b");
    config.tunnel_count = 1;
    for (size_t i = 0; i < 2; i++) {
        snprintf(config.paths[i].path_id, sizeof(config.paths[i].path_id), "%s", "path-dup");
        snprintf(config.paths[i].source, sizeof(config.paths[i].source), "%s", "site-a");
        snprintf(config.paths[i].destination, sizeof(config.paths[i].destination), "%s", "site-b");
        snprintf(config.paths[i].segments[0].tunnel_id, sizeof(config.paths[i].segments[0].tunnel_id), "%s", "tun-a-b");
        config.paths[i].segment_count = 1;
    }
    config.path_count = 2;
    char error[256] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_yaml_validation_rejects_invalid_vlan_id(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.intents[0].intent_id, sizeof(config.intents[0].intent_id), "%s", "intent-vlan-invalid");
    config.intents[0].traffic.has_vlan_id = true;
    config.intents[0].traffic.vlan_id = 4095;
    config.intent_count = 1;
    char error[128] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    config.intents[0].traffic.vlan_id = 100;
    config.intents[0].traffic.has_vlan_id = false;
    config.intents[0].deny_unmatched_vlan = true;
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    config.intents[0].traffic.has_vlan_id = true;
}

static void test_yaml_validation_rejects_negative_transition_timing(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.intents[0].intent_id, sizeof(config.intents[0].intent_id), "%s", "intent-negative-timing");
    config.intents[0].transition.max_pause_ms = -1;
    config.intent_count = 1;
    char error[128] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_yaml_validation_rejects_invalid_stability_settings(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.intents[0].intent_id, sizeof(config.intents[0].intent_id), "%s", "intent-invalid-stability");
    config.intents[0].transition.retry_count = -1;
    config.intent_count = 1;
    char error[128] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    config.intents[0].transition.retry_count = 0;
    config.intents[0].path_selection.constraints.hold_down_ms = -1;
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
    config.intents[0].path_selection.constraints.hold_down_ms = 0;
    config.intents[0].path_selection.constraints.has_hysteresis_percent = true;
    config.intents[0].path_selection.constraints.hysteresis_percent = 101.0;
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_yaml_validation_rejects_conflicting_route_policy(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.tunnels[0].tunnel_id, sizeof(config.tunnels[0].tunnel_id), "%s", "tun-a-b");
    snprintf(config.tunnels[0].local_node, sizeof(config.tunnels[0].local_node), "%s", "site-a");
    snprintf(config.tunnels[0].remote_node, sizeof(config.tunnels[0].remote_node), "%s", "site-b");
    config.tunnel_count = 1;
    snprintf(config.paths[0].path_id, sizeof(config.paths[0].path_id), "%s", "path-a");
    snprintf(config.paths[0].source, sizeof(config.paths[0].source), "%s", "site-a");
    snprintf(config.paths[0].destination, sizeof(config.paths[0].destination), "%s", "site-b");
    snprintf(config.paths[0].segments[0].tunnel_id, sizeof(config.paths[0].segments[0].tunnel_id), "%s", "tun-a-b");
    config.paths[0].segment_count = 1;
    snprintf(config.paths[0].waypoints[0], sizeof(config.paths[0].waypoints[0]), "%s", "hub-1");
    snprintf(config.paths[0].waypoints[1], sizeof(config.paths[0].waypoints[1]), "%s", "hub-1");
    config.paths[0].waypoint_count = 2;
    config.path_count = 1;
    char error[128] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);

    config.paths[0].waypoint_count = 0;
    snprintf(config.intents[0].intent_id, sizeof(config.intents[0].intent_id), "%s", "intent-a-b");
    config.intents[0].path_selection.mode = EN_SELECT_PRIORITY;
    config.intents[0].path_selection.constraints.required_waypoint_count = 1;
    config.intents[0].path_selection.constraints.forbidden_waypoint_count = 1;
    snprintf(config.intents[0].path_selection.constraints.required_waypoints[0], EN_MAX_ID_LEN, "%s", "hub-1");
    snprintf(config.intents[0].path_selection.constraints.forbidden_waypoints[0], EN_MAX_ID_LEN, "%s", "hub-1");
    config.intent_count = 1;
    memset(error, 0, sizeof(error));
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

static void test_yaml_validation_rejects_unsafe_identifiers(void)
{
    en_yaml_config_t config = {0};
    snprintf(config.paths[0].path_id, sizeof(config.paths[0].path_id), "%s", "path\"bad");
    snprintf(config.paths[0].source, sizeof(config.paths[0].source), "%s", "site-a");
    snprintf(config.paths[0].destination, sizeof(config.paths[0].destination), "%s", "site-b");
    config.path_count = 1;
    char error[128] = {0};
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);

    memset(&config, 0, sizeof(config));
    memset(config.paths[0].path_id, 'p', sizeof(config.paths[0].path_id));
    config.path_count = 1;
    memset(error, 0, sizeof(error));
    ASSERT_TRUE(en_yaml_config_validate(&config, error, sizeof(error)) == EN_ERR_INVALID_ARGUMENT);
}

int main(void)
{
    test_priority_selects_first_healthy_path();
    test_evaluated_excludes_failed_path();
    test_failed_forwarding_rolls_back_to_previous_path();
    test_successful_switch_removes_previous_path();
    test_forwarding_failure_retries_before_rollback();
    test_graceful_switch_drains_previous_path();
    test_path_selection_thresholds_prevent_flapping();
    test_hold_down_prevents_healthy_path_switch();
    test_evaluated_hysteresis_prevents_small_quality_switch();
    test_yaml_config_loads_paths_and_intents();
    test_command_adapters_can_drive_controller_dry_run();
    test_command_adapter_vlan_acl_cleanup_dry_run();
    test_repeated_intent_submission_does_not_exhaust_state();
    test_applied_path_can_be_restored();
    test_forwarding_failure_reinstalls_previous_path();
    test_renderers_generate_swanctl_and_vppctl();
    test_renderers_generate_vpp_gre_over_ipsec();
    test_path_events_parse_as_health_records();
    test_health_probe_ignores_out_of_order_records();
    test_vlan_health_requires_route_and_interface();
    test_telemetry_file_schema_validation();
    test_pubkey_certificate_path_validation();
    test_psk_is_quoted_and_injection_is_rejected();
    test_swanctl_list_sas_observer_parser();
    test_vpp_show_ip_fib_observer_parser();
    test_vpp_api_adapter_forwards_callbacks();
    test_vpp_api_path_operations_order_and_validation();
    test_vpp_api_transport_validates_lifecycle();
    test_strongswan_vici_adapter_forwards_callbacks();
    test_apply_plan_generates_swanctl_conf_and_commands();
    test_apply_plan_execution_boundary();
    test_yaml_config_loads_route_variants();
    test_yaml_loader_segmentless_route_boundary();
    test_yaml_config_loads_vpp_vlan_allowlist();
    test_yaml_loader_rejects_invalid_vpp_vlan_allowlist();
    test_yaml_loader_supports_multiport_vpp_edges();
    test_yaml_loader_rejects_unknown_vpp_route_interface();
    test_yaml_config_loads_node_capabilities();
    test_yaml_validation_rejects_unknown_tunnel();
    test_yaml_loader_rejects_truncated_values();
    test_yaml_validation_rejects_duplicate_path_id();
    test_yaml_validation_rejects_invalid_vlan_id();
    test_yaml_validation_rejects_negative_transition_timing();
    test_yaml_validation_rejects_invalid_stability_settings();
    test_yaml_validation_rejects_conflicting_route_policy();
    test_yaml_validation_rejects_unsafe_identifiers();
    printf("all tests passed\n");
    return 0;
}
