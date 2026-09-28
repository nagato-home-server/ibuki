#include "internal.h"
#include "eventnet/mock_adapters.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

static bool health_expired(const en_health_probe_mock_t *mock, const en_path_health_t *health, long long now)
{
    return mock->max_age_ms > 0 && (health->last_updated_ms > now ||
        now - health->last_updated_ms > mock->max_age_ms);
}

static void mark_expired(en_path_health_t *health)
{
    health->state = EN_HEALTH_FAILED;
    health->packet_loss_percent = 100.0;
    health->consecutive_successes = 0;
    health->consecutive_failures = INT_MAX;
    health->has_interface_observation = false;
    health->has_route_observation = false;
}

static bool observations_expired(const en_health_probe_mock_t *mock, size_t index, long long now)
{
    if (!mock->require_interface_and_route || mock->max_age_ms <= 0) return false;
    const en_path_health_t *health = &mock->overrides[index];
    return (health->has_route_observation && (mock->route_observed_ms[index] > now || now - mock->route_observed_ms[index] > mock->max_age_ms)) ||
        (health->has_interface_observation && (mock->interface_observed_ms[index] > now || now - mock->interface_observed_ms[index] > mock->max_age_ms));
}

bool en_health_probe_mock_expire(en_health_probe_mock_t *mock, long long now_ms)
{
    bool changed = false;
    if (mock == NULL) return false;
    for (size_t i = 0; i < mock->override_count; i++) {
        en_path_health_t *health = &mock->overrides[i];
        if ((health_expired(mock, health, now_ms) || observations_expired(mock, i, now_ms)) &&
            (health->state != EN_HEALTH_FAILED || health->consecutive_failures != INT_MAX)) {
            mark_expired(health);
            changed = true;
        }
    }
    return changed;
}

static en_error_code_t validate_path(void *ctx, const en_path_t *path, en_path_health_t *health)
{
    en_health_probe_mock_t *mock = ctx;
    if (path == NULL || health == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (mock != NULL) {
        for (size_t i = 0; i < mock->override_count; i++) {
            if (strcmp(mock->overrides[i].path_id, path->path_id) == 0) {
                *health = mock->overrides[i];
                if (health_expired(mock, health, en_now_ms()) || observations_expired(mock, i, en_now_ms())) mark_expired(health);
                if (mock->require_interface_and_route &&
                    (!health->has_interface_observation || !health->has_route_observation)) {
                    health->state = EN_HEALTH_FAILED;
                    health->packet_loss_percent = 100.0;
                }
                return EN_ERR_NONE;
            }
        }
    }
    snprintf(health->path_id, sizeof(health->path_id), "%s", path->path_id);
    health->state = EN_HEALTH_HEALTHY;
    health->rtt_ms = 10.0 * (double)en_path_hop_count(path);
    health->packet_loss_percent = 0.0;
    health->jitter_ms = 1.0;
    health->consecutive_failures = 0;
    health->consecutive_successes = 3;
    health->last_updated_ms = en_now_ms();
    return EN_ERR_NONE;
}

en_health_probe_t en_health_probe_mock_adapter(en_health_probe_mock_t *mock)
{
    en_health_probe_t adapter = {
        .validate_path = validate_path,
        .ctx = mock,
    };
    return adapter;
}

void en_health_probe_mock_set(en_health_probe_mock_t *mock, en_path_health_t health)
{
    if (mock == NULL) {
        return;
    }
    for (size_t i = 0; i < mock->override_count; i++) {
        if (strcmp(mock->overrides[i].path_id, health.path_id) == 0) {
            if (health.last_updated_ms < mock->overrides[i].last_updated_ms) {
                return;
            }
            if (health.last_updated_ms == mock->overrides[i].last_updated_ms &&
                health.sequence > 0 && mock->overrides[i].sequence > 0 &&
                health.source_node[0] != '\0' &&
                strcmp(health.source_node, mock->overrides[i].source_node) == 0 &&
                health.sequence < mock->overrides[i].sequence) {
                return;
            }
            en_path_health_t *current = &mock->overrides[i];
            if (health.has_route_observation) mock->route_observed_ms[i] = health.last_updated_ms;
            if (health.has_interface_observation) mock->interface_observed_ms[i] = health.last_updated_ms;
            if (health.has_route_observation || health.has_interface_observation) {
                if (health.has_route_observation) {
                    current->has_route_observation = true;
                    current->route_state = health.route_state;
                }
                if (health.has_interface_observation) {
                    current->has_interface_observation = true;
                    current->interface_state = health.interface_state;
                    snprintf(current->observed_interface_name, sizeof(current->observed_interface_name), "%s", health.observed_interface_name);
                }
                if (health.observed_destination_prefix[0] != '\0') {
                    snprintf(current->observed_destination_prefix, sizeof(current->observed_destination_prefix), "%s", health.observed_destination_prefix);
                    snprintf(current->observed_next_hop, sizeof(current->observed_next_hop), "%s", health.observed_next_hop);
                    current->has_table_id = health.has_table_id;
                    current->table_id = health.table_id;
                }
                current->state = EN_HEALTH_HEALTHY;
                if ((current->has_route_observation && current->route_state == EN_HEALTH_FAILED) ||
                    (current->has_interface_observation && current->interface_state == EN_HEALTH_FAILED)) {
                    current->state = EN_HEALTH_FAILED;
                }
                current->packet_loss_percent = current->state == EN_HEALTH_FAILED ? 100.0 : 0.0;
                current->last_updated_ms = health.last_updated_ms;
            } else {
                bool had_route_observation = current->has_route_observation;
                en_health_state_t previous_route_state = current->route_state;
                bool had_interface_observation = current->has_interface_observation;
                en_health_state_t previous_interface_state = current->interface_state;
                bool had_table_id = current->has_table_id;
                int previous_table_id = current->table_id;
                char prev_dst_prefix[EN_MAX_ID_LEN] = {0};
                char previous_next_hop[EN_MAX_ID_LEN] = {0};
                char previous_interface_name[EN_MAX_ID_LEN] = {0};
                snprintf(prev_dst_prefix, sizeof(prev_dst_prefix), "%s", current->observed_destination_prefix);
                snprintf(previous_next_hop, sizeof(previous_next_hop), "%s", current->observed_next_hop);
                snprintf(previous_interface_name, sizeof(previous_interface_name), "%s", current->observed_interface_name);
                *current = health;
                current->has_route_observation = had_route_observation;
                current->route_state = previous_route_state;
                current->has_interface_observation = had_interface_observation;
                current->interface_state = previous_interface_state;
                current->has_table_id = had_table_id;
                current->table_id = previous_table_id;
                snprintf(current->observed_destination_prefix, sizeof(current->observed_destination_prefix), "%s", prev_dst_prefix);
                snprintf(current->observed_next_hop, sizeof(current->observed_next_hop), "%s", previous_next_hop);
                snprintf(current->observed_interface_name, sizeof(current->observed_interface_name), "%s", previous_interface_name);
            }
            return;
        }
    }
    if (mock->override_count < EN_MAX_PATHS) {
        if (health.has_route_observation) mock->route_observed_ms[mock->override_count] = health.last_updated_ms;
        if (health.has_interface_observation) mock->interface_observed_ms[mock->override_count] = health.last_updated_ms;
        mock->overrides[mock->override_count++] = health;
    }
}
