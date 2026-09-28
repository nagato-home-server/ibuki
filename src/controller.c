#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static en_error_code_t observe_candidate_health(en_controller_t *controller, const en_intent_t *intent, en_transition_metrics_t *metrics);
static en_error_code_t observe_one(en_controller_t *controller, const char *path_id, en_transition_metrics_t *metrics);
static bool selection_observes_path(const en_path_selection_t *selection, const char *path_id);

static void reset_result(en_reconcile_result_t *result, const en_intent_t *intent, const char *traffic_key,
    const en_transition_metrics_t *metrics)
{
    memset(result, 0, sizeof(*result));
    en_copy_id(result->intent_id, sizeof(result->intent_id), intent->intent_id);
    en_copy_id(result->traffic_key, sizeof(result->traffic_key), traffic_key);
    if (metrics != NULL) result->metrics = *metrics;
}

en_controller_t *en_controller_create(
    const en_path_t *paths,
    size_t path_count,
    en_strongswan_adapter_t strongswan,
    en_vpp_adapter_t vpp,
    en_health_probe_t health_probe
)
{
    return en_controller_create_with_nodes_and_tunnels(NULL, 0, paths, path_count, NULL, 0, strongswan, vpp, health_probe);
}

en_controller_t *en_controller_create_with_tunnels(
    const en_path_t *paths,
    size_t path_count,
    const en_tunnel_t *tunnels,
    size_t tunnel_count,
    en_strongswan_adapter_t strongswan,
    en_vpp_adapter_t vpp,
    en_health_probe_t health_probe
)
{
    return en_controller_create_with_nodes_and_tunnels(NULL, 0, paths, path_count, tunnels, tunnel_count, strongswan, vpp, health_probe);
}

en_controller_t *en_controller_create_with_nodes_and_tunnels(
    const en_node_t *nodes,
    size_t node_count,
    const en_path_t *paths,
    size_t path_count,
    const en_tunnel_t *tunnels,
    size_t tunnel_count,
    en_strongswan_adapter_t strongswan,
    en_vpp_adapter_t vpp,
    en_health_probe_t health_probe
)
{
    if (paths == NULL || path_count > EN_MAX_PATHS || node_count > EN_MAX_NODES || tunnel_count > EN_MAX_TUNNELS) {
        return NULL;
    }
    en_controller_t *controller = calloc(1, sizeof(*controller));
    if (controller == NULL) {
        return NULL;
    }
    memcpy(controller->state.paths, paths, sizeof(en_path_t) * path_count);
    controller->state.path_count = path_count;
    if (nodes != NULL && node_count > 0) {
        memcpy(controller->state.nodes, nodes, sizeof(en_node_t) * node_count);
        controller->state.node_count = node_count;
    }
    if (tunnels != NULL && tunnel_count > 0) {
        memcpy(controller->state.desired_tunnels, tunnels, sizeof(en_tunnel_t) * tunnel_count);
        controller->state.desired_tunnel_count = tunnel_count;
    }
    controller->strongswan = strongswan;
    controller->vpp = vpp;
    controller->health_probe = health_probe;
    return controller;
}

void en_controller_destroy(en_controller_t *controller)
{
    free(controller);
}

en_error_code_t en_controller_submit_intent(
    en_controller_t *controller,
    const en_intent_t *intent,
    en_reconcile_result_t *result
)
{
    if (controller == NULL || intent == NULL || result == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&intent->traffic, traffic_key, sizeof(traffic_key));
    en_transition_metrics_t metrics = {0};
    reset_result(result, intent, traffic_key, &metrics);
    bool intent_updated = false;
    for (size_t index = 0; index < controller->state.intent_count; index++) {
        if (en_streq(controller->state.intents[index].intent_id, intent->intent_id)) {
            controller->state.intents[index] = *intent;
            intent_updated = true;
            break;
        }
    }
    if (!intent_updated) {
        if (controller->state.intent_count >= EN_MAX_CANDIDATES) {
            return EN_ERR_INVALID_ARGUMENT;
        }
        controller->state.intents[controller->state.intent_count++] = *intent;
    }
    if (!en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_IDLE)) return EN_ERR_INVALID_ARGUMENT;
    en_audit_append(controller, "INTENT_CREATED", "intent accepted", intent->intent_id);

    unsigned long long decision_start = en_monotonic_ns();
    en_error_code_t err = observe_candidate_health(controller, intent, &metrics);
    unsigned long long decision_after_probe = en_monotonic_ns();
    if (err != EN_ERR_NONE) {
        if (decision_start && decision_after_probe >= decision_start) metrics.decision_ns = decision_after_probe - decision_start;
        (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_FAILED);
        reset_result(result, intent, traffic_key, &metrics);
        (void)en_transition_status_get(controller, traffic_key, intent->intent_id, &result->transition_state);
        result->explanation.selected_path[0] = '\0';
        return err;
    }

    const char *active_path_id = en_get_applied_path(controller, traffic_key);
    if (intent->fallback.enabled && active_path_id != NULL && active_path_id[0] != '\0' &&
        !selection_observes_path(&intent->path_selection, active_path_id)) {
        err = observe_one(controller, active_path_id, &metrics);
        if (err != EN_ERR_NONE) {
            unsigned long long decision_end = en_monotonic_ns();
            if (decision_start && decision_end >= decision_start) metrics.decision_ns = decision_end - decision_start;
            (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_FAILED);
            reset_result(result, intent, traffic_key, &metrics);
            (void)en_transition_status_get(controller, traffic_key, intent->intent_id, &result->transition_state);
            en_error_append(controller, err, "active path health observation failed");
            return err;
        }
    }

    en_selection_result_t selection = {0};
    err = en_select_path(controller, intent, &selection);

    if (intent->fallback.enabled && active_path_id != NULL && active_path_id[0] != '\0' &&
        intent->fallback.path_id[0] != '\0') {
        en_path_health_t *active_health = en_find_health(controller, active_path_id);
        if (active_health != NULL &&
            (active_health->state == EN_HEALTH_FAILED || active_health->state == EN_HEALTH_UNHEALTHY)) {
            if (!selection_observes_path(&intent->path_selection, intent->fallback.path_id) &&
                !en_streq(active_path_id, intent->fallback.path_id)) {
                en_error_code_t probe_error = observe_one(controller, intent->fallback.path_id, &metrics);
                if (probe_error != EN_ERR_NONE) {
                    err = probe_error;
                }
            }
            if (err == EN_ERR_NONE || err == EN_ERR_NO_CANDIDATE) {
                en_intent_t fallback_intent = *intent;
                fallback_intent.path_selection.mode = EN_SELECT_EXPLICIT;
                en_copy_id(fallback_intent.path_selection.path_id,
                    sizeof(fallback_intent.path_selection.path_id), intent->fallback.path_id);
                en_selection_result_t fallback_selection = {0};
                en_error_code_t fallback_error = en_select_path(controller, &fallback_intent, &fallback_selection);
                if (fallback_error == EN_ERR_NONE) {
                    selection = fallback_selection;
                    snprintf(selection.reason, sizeof(selection.reason),
                        "active path %s failed; using configured fallback %s", active_path_id, intent->fallback.path_id);
                    if (selection.excluded_count < EN_MAX_CANDIDATES) {
                        size_t excluded = selection.excluded_count++;
                        en_copy_id(selection.excluded_path_ids[excluded], sizeof(selection.excluded_path_ids[excluded]), active_path_id);
                        snprintf(selection.excluded_reasons[excluded], sizeof(selection.excluded_reasons[excluded]), "%s", "active path failed");
                    }
                    err = EN_ERR_NONE;
                } else if (fallback_error != EN_ERR_NO_CANDIDATE) {
                    err = fallback_error;
                }
            }
        }
    }

    unsigned long long decision_end = en_monotonic_ns();
    if (decision_start && decision_end >= decision_start) metrics.decision_ns = decision_end - decision_start;
    if (err != EN_ERR_NONE) {
        (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_FAILED);
        reset_result(result, intent, traffic_key, &metrics);
        (void)en_transition_status_get(controller, traffic_key, intent->intent_id, &result->transition_state);
        result->explanation = selection;
        en_error_append(controller, err, "path selection failed");
        return err;
    }

    (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_CANDIDATE_SELECTED);
    en_audit_append(controller, "PATH_SELECTED", selection.reason, selection.selected_path);

    en_path_t *target_path = en_find_path(controller, selection.selected_path);
    if (target_path == NULL) {
        (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_FAILED);
        reset_result(result, intent, traffic_key, &metrics);
        result->explanation = selection;
        (void)en_transition_status_get(controller, traffic_key, intent->intent_id, &result->transition_state);
        return EN_ERR_NOT_FOUND;
    }

    const char *applied_path = en_get_applied_path(controller, traffic_key);
    if (en_streq(applied_path, target_path->path_id) && en_applied_path_verified(controller, traffic_key)) {
        (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_IDLE);
        en_audit_append(controller, "PATH_MAINTAINED", "active path remains selected", target_path->path_id);
        reset_result(result, intent, traffic_key, &metrics);
        en_copy_id(result->selected_path, sizeof(result->selected_path), selection.selected_path);
        (void)en_transition_status_get(controller, traffic_key, intent->intent_id, &result->transition_state);
        result->explanation = selection;
        return EN_ERR_NONE;
    }

    err = en_transition_path(controller, intent, target_path, &metrics);

    reset_result(result, intent, traffic_key, &metrics);
    en_copy_id(result->selected_path, sizeof(result->selected_path), selection.selected_path);
    (void)en_transition_status_get(controller, traffic_key, intent->intent_id, &result->transition_state);
    result->explanation = selection;
    return err;
}

static void store_health(en_controller_t *controller, const en_path_health_t *health)
{
    en_path_health_t *existing = en_find_health(controller, health->path_id);
    if (existing != NULL) {
        *existing = *health;
        return;
    }
    if (controller->state.health_count < EN_MAX_PATHS) {
        controller->state.health[controller->state.health_count++] = *health;
    }
}

static en_error_code_t observe_one(en_controller_t *controller, const char *path_id, en_transition_metrics_t *metrics)
{
    en_path_t *path = en_find_path(controller, path_id);
    if (path == NULL || controller->health_probe.validate_path == NULL) {
        return path == NULL ? EN_ERR_NOT_FOUND : EN_ERR_INVALID_ARGUMENT;
    }
    en_path_health_t health = {0};
    unsigned long long start = en_monotonic_ns();
    en_error_code_t err = controller->health_probe.validate_path(controller->health_probe.ctx, path, &health);
    unsigned long long end = en_monotonic_ns();
    if (metrics != NULL) {
        metrics->health_probe_count++;
        if (start && end >= start) metrics->health_probe_ns += end - start;
    }
    if (err != EN_ERR_NONE) {
        return err;
    }
    store_health(controller, &health);
    return EN_ERR_NONE;
}

static bool selection_observes_path(const en_path_selection_t *selection, const char *path_id)
{
    if (selection == NULL || path_id == NULL) return false;
    for (size_t index = 0; index < selection->candidate_count; index++) {
        if (en_streq(selection->candidates[index], path_id)) return true;
    }
    return selection->mode == EN_SELECT_EXPLICIT && en_streq(selection->path_id, path_id);
}

static en_error_code_t observe_candidate_health(en_controller_t *controller, const en_intent_t *intent, en_transition_metrics_t *metrics)
{
    const en_path_selection_t *selection = &intent->path_selection;
    if (selection->candidate_count > 0) {
        char traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
        en_make_traffic_key(&intent->traffic, traffic_key, sizeof(traffic_key));
        bool has_active_path = en_get_applied_path(controller, traffic_key) != NULL;
        if (selection->mode == EN_SELECT_PRIORITY && !has_active_path) {
            for (size_t i = 0; i < selection->candidate_count; i++) {
                en_error_code_t err = observe_one(controller, selection->candidates[i], metrics);
                if (err != EN_ERR_NONE) return err;

                en_intent_t prefix_intent = *intent;
                prefix_intent.path_selection.candidate_count = i + 1;
                en_selection_result_t prefix_selection = {0};
                err = en_select_path(controller, &prefix_intent, &prefix_selection);
                if (err == EN_ERR_NONE) return EN_ERR_NONE;
                if (err != EN_ERR_NO_CANDIDATE) return err;
            }
            return EN_ERR_NONE;
        }
        for (size_t i = 0; i < selection->candidate_count; i++) {
            en_error_code_t err = observe_one(controller, selection->candidates[i], metrics);
            if (err != EN_ERR_NONE) {
                return err;
            }
        }
        return EN_ERR_NONE;
    }
    if (selection->mode == EN_SELECT_EXPLICIT) {
        return observe_one(controller, selection->path_id, metrics);
    }
    if (selection->mode == EN_SELECT_PRIORITY || selection->mode == EN_SELECT_EVALUATED) {
        return EN_ERR_NONE;
    }
    for (size_t i = 0; i < controller->state.path_count; i++) {
        en_error_code_t err = observe_one(controller, controller->state.paths[i].path_id, metrics);
        if (err != EN_ERR_NONE) {
            return err;
        }
    }
    return EN_ERR_NONE;
}
