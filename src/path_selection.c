#include "internal.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool waypoint_contains(char waypoints[EN_MAX_WAYPOINTS][EN_MAX_ID_LEN], size_t count, const char *needle);
static bool exclusion_reason(const en_controller_t *controller, const en_path_t *path, const en_path_health_t *health, const en_path_health_t *active_health, const en_path_constraints_t *constraints, bool active, bool active_path_exists, long long active_since_ms, char *reason, size_t reason_len);
static bool node_has_capability(const en_controller_t *controller, const char *node_id, const char *capability);
static bool path_nodes_enabled(const en_controller_t *controller, const en_path_t *path);
static bool path_supports_capabilities(const en_controller_t *controller, const en_path_t *path, const en_path_constraints_t *constraints);
static int compare_paths(const en_path_t *left, const en_path_t *right, const en_path_health_t *left_health, const en_path_health_t *right_health, const en_path_selection_t *selection);
static double metric_value(const en_path_t *path, const en_path_health_t *health, en_comparison_key_t key);

en_error_code_t en_select_path(en_controller_t *controller, const en_intent_t *intent, en_selection_result_t *result)
{
    if (controller == NULL || intent == NULL || result == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));

    const en_path_selection_t *selection = &intent->path_selection;
    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&intent->traffic, traffic_key, sizeof(traffic_key));
    const char *active_path_id = en_get_applied_path(controller, traffic_key);
    bool active_path_exists = active_path_id != NULL && active_path_id[0] != '\0';
    long long active_since_ms = en_get_applied_since_ms(controller, traffic_key);
    en_path_health_t *active_health = active_path_exists ? en_find_health(controller, active_path_id) : NULL;
    if (selection->mode == EN_SELECT_EXPLICIT) {
        en_path_t *path = en_find_path(controller, selection->path_id);
        if (path == NULL) {
            return EN_ERR_NOT_FOUND;
        }
        char reason[128] = {0};
        if (exclusion_reason(controller, path, en_find_health(controller, path->path_id), active_health, &selection->constraints,
                en_streq(active_path_id, path->path_id), active_path_exists, active_since_ms, reason, sizeof(reason))) {
            en_copy_id(result->excluded_path_ids[0], sizeof(result->excluded_path_ids[0]), path->path_id);
            snprintf(result->excluded_reasons[0], sizeof(result->excluded_reasons[0]), "%s", reason);
            result->excluded_count = 1;
            return EN_ERR_NO_CANDIDATE;
        }
        en_copy_id(result->selected_path, sizeof(result->selected_path), path->path_id);
        en_copy_id(result->candidates[0], sizeof(result->candidates[0]), path->path_id);
        result->candidate_count = 1;
        snprintf(result->reason, sizeof(result->reason), "explicit path requested");
        return EN_ERR_NONE;
    }

    size_t candidate_count = selection->candidate_count;
    for (size_t i = 0; i < candidate_count; i++) {
        en_copy_id(result->candidates[i], sizeof(result->candidates[i]), selection->candidates[i]);
    }
    result->candidate_count = candidate_count;

    if (selection->mode == EN_SELECT_PRIORITY) {
        for (size_t i = 0; i < candidate_count; i++) {
            en_path_t *path = en_find_path(controller, selection->candidates[i]);
            if (path == NULL) {
                return EN_ERR_NOT_FOUND;
            }
            char reason[128] = {0};
            if (!exclusion_reason(controller, path, en_find_health(controller, path->path_id), active_health, &selection->constraints,
                    en_streq(active_path_id, path->path_id), active_path_exists, active_since_ms, reason, sizeof(reason))) {
                en_copy_id(result->selected_path, sizeof(result->selected_path), path->path_id);
                snprintf(result->reason, sizeof(result->reason), "first usable priority candidate");
                return EN_ERR_NONE;
            }
            size_t idx = result->excluded_count++;
            en_copy_id(result->excluded_path_ids[idx], sizeof(result->excluded_path_ids[idx]), path->path_id);
            snprintf(result->excluded_reasons[idx], sizeof(result->excluded_reasons[idx]), "%s", reason);
        }
        return EN_ERR_NO_CANDIDATE;
    }

    if (selection->mode == EN_SELECT_EVALUATED) {
        en_path_t *best = NULL;
        en_path_health_t *best_health = NULL;
        for (size_t i = 0; i < candidate_count; i++) {
            en_path_t *path = en_find_path(controller, selection->candidates[i]);
            if (path == NULL) {
                return EN_ERR_NOT_FOUND;
            }
            en_path_health_t *health = en_find_health(controller, path->path_id);
            char reason[128] = {0};
            if (exclusion_reason(controller, path, health, active_health, &selection->constraints,
                    en_streq(active_path_id, path->path_id), active_path_exists, active_since_ms, reason, sizeof(reason))) {
                size_t idx = result->excluded_count++;
                en_copy_id(result->excluded_path_ids[idx], sizeof(result->excluded_path_ids[idx]), path->path_id);
                snprintf(result->excluded_reasons[idx], sizeof(result->excluded_reasons[idx]), "%s", reason);
                continue;
            }
            if (best == NULL || compare_paths(path, best, health, best_health, selection) < 0) {
                best = path;
                best_health = health;
            }
        }
        if (best == NULL) {
            return EN_ERR_NO_CANDIDATE;
        }
        if (active_path_exists && active_health != NULL && selection->constraints.has_hysteresis_percent &&
            !en_streq(best->path_id, active_path_id) &&
            (active_health->state == EN_HEALTH_HEALTHY || active_health->state == EN_HEALTH_DEGRADED)) {
            en_path_t *active_path = en_find_path(controller, active_path_id);
            en_comparison_key_t primary_key = selection->comparison_count == 0 ? EN_COMPARE_PATH_ID : selection->comparison_order[0];
            if (active_path != NULL && primary_key != EN_COMPARE_PATH_ID) {
                double active_value = metric_value(active_path, active_health, primary_key);
                double best_value = metric_value(best, best_health, primary_key);
                double baseline = fmax(fabs(active_value), 1.0);
                double improvement = ((active_value - best_value) / baseline) * 100.0;
                if (improvement < selection->constraints.hysteresis_percent) {
                    en_copy_id(result->selected_path, sizeof(result->selected_path), active_path_id);
                    snprintf(result->reason, sizeof(result->reason), "active path retained by hysteresis");
                    return EN_ERR_NONE;
                }
            }
        }
        en_copy_id(result->selected_path, sizeof(result->selected_path), best->path_id);
        snprintf(result->reason, sizeof(result->reason), "best evaluated candidate by comparison_order");
        return EN_ERR_NONE;
    }

    return EN_ERR_INVALID_ARGUMENT;
}

static bool exclusion_reason(const en_controller_t *controller, const en_path_t *path, const en_path_health_t *health, const en_path_health_t *active_health, const en_path_constraints_t *constraints, bool active, bool active_path_exists, long long active_since_ms, char *reason, size_t reason_len)
{
    if (path->administrative_state != EN_ADMIN_ENABLED) {
        snprintf(reason, reason_len, "administratively disabled");
        return true;
    }
    if (!path_nodes_enabled(controller, path)) {
        snprintf(reason, reason_len, "path node is administratively disabled");
        return true;
    }
    if (!active && active_path_exists && active_health != NULL && active_since_ms > 0 && constraints->hold_down_ms > 0 &&
        (active_health->state == EN_HEALTH_HEALTHY || active_health->state == EN_HEALTH_DEGRADED)) {
        long long elapsed_ms = en_now_ms() - active_since_ms;
        if (elapsed_ms >= 0 && elapsed_ms < constraints->hold_down_ms) {
            snprintf(reason, reason_len, "active path hold-down not elapsed");
            return true;
        }
    }
    if (health != NULL && (health->state == EN_HEALTH_UNHEALTHY || health->state == EN_HEALTH_FAILED) &&
        !(active && constraints->failure_threshold > 0 && health->consecutive_failures < constraints->failure_threshold)) {
        snprintf(reason, reason_len, "health is %s", en_health_state_name(health->state));
        return true;
    }
    if (health != NULL && active_path_exists && !active && path->operational_state == EN_PATH_STANDBY &&
        constraints->recovery_threshold > 0 &&
        (health->state == EN_HEALTH_HEALTHY || health->state == EN_HEALTH_DEGRADED) &&
        health->consecutive_successes < constraints->recovery_threshold) {
        snprintf(reason, reason_len, "recovery threshold not met");
        return true;
    }
    if (health != NULL && constraints->has_max_rtt_ms && health->rtt_ms > constraints->max_rtt_ms) {
        snprintf(reason, reason_len, "rtt constraint violation");
        return true;
    }
    if (health != NULL && constraints->has_max_packet_loss_percent && health->packet_loss_percent > constraints->max_packet_loss_percent) {
        snprintf(reason, reason_len, "packet loss constraint violation");
        return true;
    }
    for (size_t i = 0; i < constraints->forbidden_waypoint_count; i++) {
        if (waypoint_contains((char (*)[EN_MAX_ID_LEN])path->waypoints, path->waypoint_count, constraints->forbidden_waypoints[i])) {
            snprintf(reason, reason_len, "forbidden waypoint constraint violation");
            return true;
        }
    }
    for (size_t i = 0; i < constraints->required_waypoint_count; i++) {
        if (!waypoint_contains((char (*)[EN_MAX_ID_LEN])path->waypoints, path->waypoint_count, constraints->required_waypoints[i])) {
            snprintf(reason, reason_len, "required waypoint constraint violation");
            return true;
        }
    }
    if (!path_supports_capabilities(controller, path, constraints)) {
        snprintf(reason, reason_len, "required node capability is unavailable");
        return true;
    }
    return false;
}

static bool node_has_capability(const en_controller_t *controller, const char *node_id, const char *capability)
{
    const en_node_t *node = en_find_node(controller, node_id);
    if (node == NULL || capability == NULL) return false;
    for (size_t index = 0; index < node->capability_count; index++) {
        if (en_streq(node->capabilities[index], capability)) return true;
    }
    return false;
}

static bool path_nodes_enabled(const en_controller_t *controller, const en_path_t *path)
{
    if (controller == NULL || path == NULL || controller->state.node_count == 0) return true;
    const en_node_t *source = en_find_node(controller, path->source);
    const en_node_t *destination = en_find_node(controller, path->destination);
    if (source == NULL || destination == NULL || source->administrative_state != EN_ADMIN_ENABLED ||
        destination->administrative_state != EN_ADMIN_ENABLED) return false;
    for (size_t index = 0; index < path->waypoint_count; index++) {
        const en_node_t *waypoint = en_find_node(controller, path->waypoints[index]);
        if (waypoint == NULL || waypoint->administrative_state != EN_ADMIN_ENABLED) return false;
    }
    for (size_t index = 0; index < path->segment_count; index++) {
        const en_segment_t *segment = &path->segments[index];
        const en_node_t *from = en_find_node(controller, segment->from_node);
        const en_node_t *to = en_find_node(controller, segment->to_node);
        if (from == NULL || to == NULL || from->administrative_state != EN_ADMIN_ENABLED ||
            to->administrative_state != EN_ADMIN_ENABLED) return false;
    }
    return true;
}

static bool path_supports_capabilities(const en_controller_t *controller, const en_path_t *path, const en_path_constraints_t *constraints)
{
    if (constraints == NULL || constraints->required_capability_count == 0) return true;
    if (controller == NULL || path == NULL) return false;
    for (size_t capability_index = 0; capability_index < constraints->required_capability_count; capability_index++) {
        const char *capability = constraints->required_capabilities[capability_index];
        if (!node_has_capability(controller, path->source, capability) ||
            !node_has_capability(controller, path->destination, capability)) return false;
        for (size_t segment_index = 0; segment_index < path->segment_count; segment_index++) {
            const en_segment_t *segment = &path->segments[segment_index];
            if (!node_has_capability(controller, segment->from_node, capability) ||
                !node_has_capability(controller, segment->to_node, capability)) return false;
        }
    }
    return true;
}

static bool waypoint_contains(char waypoints[EN_MAX_WAYPOINTS][EN_MAX_ID_LEN], size_t count, const char *needle)
{
    for (size_t i = 0; i < count; i++) {
        if (en_streq(waypoints[i], needle)) {
            return true;
        }
    }
    return false;
}

static int compare_paths(const en_path_t *left, const en_path_t *right, const en_path_health_t *left_health, const en_path_health_t *right_health, const en_path_selection_t *selection)
{
    size_t count = selection->comparison_count == 0 ? 1 : selection->comparison_count;
    for (size_t i = 0; i < count; i++) {
        en_comparison_key_t key = selection->comparison_count == 0 ? EN_COMPARE_PATH_ID : selection->comparison_order[i];
        if (key == EN_COMPARE_PATH_ID) {
            int cmp = strcmp(left->path_id, right->path_id);
            if (cmp != 0) {
                return cmp;
            }
            continue;
        }
        double left_value = metric_value(left, left_health, key);
        double right_value = metric_value(right, right_health, key);
        if (left_value < right_value) {
            return -1;
        }
        if (left_value > right_value) {
            return 1;
        }
    }
    return strcmp(left->path_id, right->path_id);
}

static double metric_value(const en_path_t *path, const en_path_health_t *health, en_comparison_key_t key)
{
    switch (key) {
    case EN_COMPARE_PACKET_LOSS:
        return health == NULL ? DBL_MAX : health->packet_loss_percent;
    case EN_COMPARE_LATENCY:
        return health == NULL ? DBL_MAX : health->rtt_ms;
    case EN_COMPARE_HOP_COUNT:
        return (double)en_path_hop_count(path);
    case EN_COMPARE_ADMIN_PRIORITY:
        return (double)path->priority;
    case EN_COMPARE_PATH_ID:
        return 0.0;
    }
    return DBL_MAX;
}
