#include "internal.h"

#include <stdlib.h>
#include <string.h>

static en_error_code_t observe_candidate_health(en_controller_t *controller, const en_intent_t *intent);

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
    controller->state.transition_state = EN_TRANSITION_IDLE;
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
    en_audit_append(controller, "INTENT_CREATED", "intent accepted", intent->intent_id);

    en_error_code_t err = observe_candidate_health(controller, intent);
    if (err != EN_ERR_NONE) {
        return err;
    }

    en_selection_result_t selection = {0};
    err = en_select_path(controller, intent, &selection);
    if (err != EN_ERR_NONE) {
        memset(result, 0, sizeof(*result));
        en_copy_id(result->intent_id, sizeof(result->intent_id), intent->intent_id);
        result->transition_state = controller->state.transition_state;
        result->explanation = selection;
        en_error_append(controller, err, "path selection failed");
        return err;
    }

    controller->state.transition_state = EN_TRANSITION_CANDIDATE_SELECTED;
    en_audit_append(controller, "PATH_SELECTED", selection.reason, selection.selected_path);

    en_path_t *target_path = en_find_path(controller, selection.selected_path);
    if (target_path == NULL) {
        return EN_ERR_NOT_FOUND;
    }

    const char *applied_path = en_get_applied_path(controller, traffic_key);
    if (en_streq(applied_path, target_path->path_id)) {
        controller->state.transition_state = EN_TRANSITION_IDLE;
        en_audit_append(controller, "PATH_MAINTAINED", "active path remains selected", target_path->path_id);
        memset(result, 0, sizeof(*result));
        en_copy_id(result->intent_id, sizeof(result->intent_id), intent->intent_id);
        en_copy_id(result->selected_path, sizeof(result->selected_path), selection.selected_path);
        result->transition_state = controller->state.transition_state;
        result->explanation = selection;
        return EN_ERR_NONE;
    }

    err = en_transition_path(controller, intent, target_path);

    memset(result, 0, sizeof(*result));
    en_copy_id(result->intent_id, sizeof(result->intent_id), intent->intent_id);
    en_copy_id(result->selected_path, sizeof(result->selected_path), selection.selected_path);
    result->transition_state = controller->state.transition_state;
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

static en_error_code_t observe_one(en_controller_t *controller, const char *path_id)
{
    en_path_t *path = en_find_path(controller, path_id);
    if (path == NULL || controller->health_probe.validate_path == NULL) {
        return path == NULL ? EN_ERR_NOT_FOUND : EN_ERR_INVALID_ARGUMENT;
    }
    en_path_health_t health = {0};
    en_error_code_t err = controller->health_probe.validate_path(controller->health_probe.ctx, path, &health);
    if (err != EN_ERR_NONE) {
        return err;
    }
    store_health(controller, &health);
    return EN_ERR_NONE;
}

static en_error_code_t observe_candidate_health(en_controller_t *controller, const en_intent_t *intent)
{
    const en_path_selection_t *selection = &intent->path_selection;
    if (selection->candidate_count > 0) {
        for (size_t i = 0; i < selection->candidate_count; i++) {
            en_error_code_t err = observe_one(controller, selection->candidates[i]);
            if (err != EN_ERR_NONE) {
                return err;
            }
        }
        return EN_ERR_NONE;
    }
    if (selection->mode == EN_SELECT_EXPLICIT) {
        return observe_one(controller, selection->path_id);
    }
    for (size_t i = 0; i < controller->state.path_count; i++) {
        en_error_code_t err = observe_one(controller, controller->state.paths[i].path_id);
        if (err != EN_ERR_NONE) {
            return err;
        }
    }
    return EN_ERR_NONE;
}
