#include "internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

long long en_now_ms(void)
{
#ifdef _WIN32
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    return (long long)(ticks.QuadPart / 10000ULL) - 11644473600000LL;
#else
    struct timeval current_time;
    if (gettimeofday(&current_time, NULL) != 0) return (long long)time(NULL) * 1000LL;
    return (long long)current_time.tv_sec * 1000LL + current_time.tv_usec / 1000L;
#endif
}

void en_copy_id(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

bool en_streq(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

size_t en_path_hop_count(const en_path_t *path)
{
    return path == NULL ? 0 : path->waypoint_count + 1;
}

en_path_t *en_find_path(en_controller_t *controller, const char *path_id)
{
    if (controller == NULL || path_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < controller->state.path_count; i++) {
        if (en_streq(controller->state.paths[i].path_id, path_id)) {
            return &controller->state.paths[i];
        }
    }
    return NULL;
}

en_path_health_t *en_find_health(en_controller_t *controller, const char *path_id)
{
    if (controller == NULL || path_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < controller->state.health_count; i++) {
        if (en_streq(controller->state.health[i].path_id, path_id)) {
            return &controller->state.health[i];
        }
    }
    return NULL;
}

en_tunnel_t *en_find_tunnel(en_controller_t *controller, const char *tunnel_id)
{
    if (controller == NULL || tunnel_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < controller->state.observed_tunnel_count; i++) {
        if (en_streq(controller->state.observed_tunnels[i].tunnel_id, tunnel_id)) {
            return &controller->state.observed_tunnels[i];
        }
    }
    return NULL;
}

en_tunnel_t *en_find_desired_tunnel(en_controller_t *controller, const char *tunnel_id)
{
    if (controller == NULL || tunnel_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < controller->state.desired_tunnel_count; i++) {
        if (en_streq(controller->state.desired_tunnels[i].tunnel_id, tunnel_id)) {
            return &controller->state.desired_tunnels[i];
        }
    }
    return NULL;
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
        const en_node_t *from = en_find_node(controller, path->segments[index].from_node);
        const en_node_t *to = en_find_node(controller, path->segments[index].to_node);
        if (from == NULL || to == NULL || from->administrative_state != EN_ADMIN_ENABLED ||
            to->administrative_state != EN_ADMIN_ENABLED) return false;
    }
    return true;
}

const en_path_t *en_controller_find_path(const en_controller_t *controller, const char *path_id)
{
    return en_find_path((en_controller_t *)controller, path_id);
}

bool en_controller_path_nodes_enabled(const en_controller_t *controller, const en_path_t *path)
{
    return path_nodes_enabled(controller, path);
}

const en_node_t *en_find_node(const en_controller_t *controller, const char *node_id)
{
    if (controller == NULL || node_id == NULL) return NULL;
    for (size_t index = 0; index < controller->state.node_count; index++) {
        if (en_streq(controller->state.nodes[index].node_id, node_id)) return &controller->state.nodes[index];
    }
    return NULL;
}

const char *en_get_applied_path(en_controller_t *controller, const char *traffic_key)
{
    if (controller == NULL || traffic_key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < controller->state.applied_count; i++) {
        if (en_streq(controller->state.traffic_keys[i], traffic_key)) {
            return controller->state.applied_paths[i];
        }
    }
    return NULL;
}

long long en_get_applied_since_ms(en_controller_t *controller, const char *traffic_key)
{
    if (controller == NULL || traffic_key == NULL) return 0;
    for (size_t i = 0; i < controller->state.applied_count; i++) {
        if (en_streq(controller->state.traffic_keys[i], traffic_key)) return controller->state.applied_since_ms[i];
    }
    return 0;
}

void en_set_applied_path(en_controller_t *controller, const char *traffic_key, const char *path_id)
{
    if (controller == NULL || traffic_key == NULL || path_id == NULL) {
        return;
    }
    for (size_t i = 0; i < controller->state.applied_count; i++) {
        if (en_streq(controller->state.traffic_keys[i], traffic_key)) {
            en_copy_id(controller->state.applied_paths[i], sizeof(controller->state.applied_paths[i]), path_id);
            controller->state.applied_since_ms[i] = en_now_ms();
            return;
        }
    }
    if (controller->state.applied_count < EN_MAX_CANDIDATES) {
        size_t idx = controller->state.applied_count++;
        en_copy_id(controller->state.traffic_keys[idx], sizeof(controller->state.traffic_keys[idx]), traffic_key);
        en_copy_id(controller->state.applied_paths[idx], sizeof(controller->state.applied_paths[idx]), path_id);
        controller->state.applied_since_ms[idx] = en_now_ms();
    }
}

en_error_code_t en_controller_restore_applied_path(en_controller_t *controller, const char *traffic_key, const char *path_id)
{
    if (controller == NULL || traffic_key == NULL || path_id == NULL || traffic_key[0] == '\0' || path_id[0] == '\0') {
        return EN_ERR_INVALID_ARGUMENT;
    }
    en_path_t *restored_path = en_find_path(controller, path_id);
    if (restored_path == NULL) return EN_ERR_NOT_FOUND;
    if (restored_path->administrative_state != EN_ADMIN_ENABLED || !path_nodes_enabled(controller, restored_path)) {
        return EN_ERR_STATE_CONFLICT;
    }
    restored_path->operational_state = EN_PATH_ACTIVE;
    en_set_applied_path(controller, traffic_key, path_id);
    return EN_ERR_NONE;
}

void en_make_traffic_key(const en_traffic_selector_t *traffic, char *buf, size_t buf_len)
{
    if (buf_len == 0) {
        return;
    }
    if (traffic == NULL) {
        buf[0] = '\0';
        return;
    }
    if (traffic->has_vlan_id) {
        snprintf(buf, buf_len, "%s->%s|vlan=%d", traffic->source, traffic->destination, traffic->vlan_id);
    } else {
        snprintf(buf, buf_len, "%s->%s", traffic->source, traffic->destination);
    }
}

const char *en_controller_applied_path(const en_controller_t *controller, const char *traffic_key)
{
    return en_get_applied_path((en_controller_t *)controller, traffic_key);
}

const char *en_health_state_name(en_health_state_t state)
{
    switch (state) {
    case EN_HEALTH_UNKNOWN: return "unknown";
    case EN_HEALTH_HEALTHY: return "healthy";
    case EN_HEALTH_DEGRADED: return "degraded";
    case EN_HEALTH_UNHEALTHY: return "unhealthy";
    case EN_HEALTH_FAILED: return "failed";
    }
    return "invalid";
}

const char *en_transition_state_name(en_transition_state_t state)
{
    switch (state) {
    case EN_TRANSITION_IDLE: return "idle";
    case EN_TRANSITION_CANDIDATE_SELECTED: return "candidate_selected";
    case EN_TRANSITION_PREPARING: return "preparing";
    case EN_TRANSITION_VALIDATING: return "validating";
    case EN_TRANSITION_READY: return "ready";
    case EN_TRANSITION_PAUSING: return "pausing";
    case EN_TRANSITION_DRAINING: return "draining";
    case EN_TRANSITION_SWITCHING: return "switching";
    case EN_TRANSITION_VERIFYING: return "verifying";
    case EN_TRANSITION_COMPLETED: return "completed";
    case EN_TRANSITION_ROLLING_BACK: return "rolling_back";
    case EN_TRANSITION_FAILED: return "failed";
    }
    return "invalid";
}

const char *en_error_code_name(en_error_code_t code)
{
    switch (code) {
    case EN_ERR_NONE: return "none";
    case EN_ERR_TUNNEL_ESTABLISH_TIMEOUT: return "tunnel_establish_timeout";
    case EN_ERR_PATH_VALIDATION_FAILED: return "path_validation_failed";
    case EN_ERR_FORWARDING_UPDATE_FAILED: return "forwarding_update_failed";
    case EN_ERR_STATE_CONFLICT: return "state_conflict";
    case EN_ERR_ROLLBACK_FAILED: return "rollback_failed";
    case EN_ERR_OBSERVATION_TIMEOUT: return "observation_timeout";
    case EN_ERR_INVALID_ARGUMENT: return "invalid_argument";
    case EN_ERR_NOT_FOUND: return "not_found";
    case EN_ERR_NO_CANDIDATE: return "no_candidate";
    }
    return "invalid";
}
