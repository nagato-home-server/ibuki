#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "internal.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <stdio.h>
#include <limits.h>

static void sleep_ms(int milliseconds);
static en_error_code_t prepare_path(en_controller_t *controller, en_path_t *path);
static en_error_code_t validate_path(en_controller_t *controller, en_path_t *path, const char *traffic_key, const char *intent_id, en_transition_metrics_t *metrics);
static en_error_code_t commit_path(en_controller_t *controller, const en_intent_t *intent, en_path_t *target_path, en_path_t *previous_path, const char *traffic_key);
static en_error_code_t confirm_path(en_controller_t *controller, en_path_t *target_path, const char *traffic_key, const char *intent_id);
static en_error_code_t cleanup_previous(en_controller_t *controller, const char *traffic_key, en_path_t *previous_path, en_path_t *target_path);
static en_error_code_t prepare_retry(en_controller_t *controller, const en_intent_t *intent, en_path_t *path);
static en_error_code_t validate_retry(en_controller_t *controller, const en_intent_t *intent, en_path_t *path, const char *traffic_key, en_transition_metrics_t *metrics);
static en_error_code_t commit_retry(en_controller_t *controller, const en_intent_t *intent, en_path_t *target_path, en_path_t *previous_path, const char *traffic_key);
static en_error_code_t confirm_retry(en_controller_t *controller, const en_intent_t *intent, en_path_t *target_path, const char *traffic_key);
static en_error_code_t rollback(en_controller_t *controller, const char *traffic_key, const char *intent_id, const char *old_path_id, en_path_t *target_path);
static en_error_code_t rollback_and_report(en_controller_t *controller, const char *traffic_key, const char *intent_id, const char *old_path_id, en_path_t *target_path, en_error_code_t transition_error, en_transition_metrics_t *metrics);
static bool path_uses_tunnel(const en_path_t *path, const char *tunnel_id);
static en_error_code_t remove_unshared_tunnels(en_controller_t *controller, const char *traffic_key, const en_path_t *target_path, const en_path_t *rollback_path);

en_error_code_t en_transition_path(en_controller_t *controller, const en_intent_t *intent, en_path_t *target_path, en_transition_metrics_t *metrics)
{
    if (controller == NULL || intent == NULL || target_path == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }

    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN] = {0};
    en_make_traffic_key(&intent->traffic, traffic_key, sizeof(traffic_key));
    const char *current_path = en_get_applied_path(controller, traffic_key);
    char old_path_id[EN_MAX_ID_LEN] = {0};
    en_copy_id(old_path_id, sizeof(old_path_id), current_path);

    if (intent->transition.strategy == EN_TRANSITION_GRACEFUL && old_path_id[0] != '\0' &&
        !en_streq(old_path_id, target_path->path_id) && controller->vpp.graceful_switch == NULL) {
        (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_FAILED);
        en_error_append(controller, EN_ERR_STATE_CONFLICT, "VPP adapter does not support flow-aware graceful switching");
        en_audit_append(controller, "TRANSITION_REJECTED", "flow-aware graceful switch is unavailable", target_path->path_id);
        return EN_ERR_STATE_CONFLICT;
    }

    (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_PREPARING);
    en_audit_append(controller, "TRANSITION_STARTED", "preparing target path", target_path->path_id);

    unsigned long long phase_start = en_monotonic_ns();
    en_error_code_t err = prepare_retry(controller, intent, target_path);
    unsigned long long phase_end = en_monotonic_ns();
    if (metrics != NULL && phase_start && phase_end >= phase_start) metrics->prepare_ns += phase_end - phase_start;
    if (err != EN_ERR_NONE) {
        en_error_append(controller, err, "target path preparation failed");
        return rollback_and_report(controller, traffic_key, intent->intent_id, old_path_id, target_path, err, metrics);
    }

    phase_start = en_monotonic_ns();
    err = validate_retry(controller, intent, target_path, traffic_key, metrics);
    phase_end = en_monotonic_ns();
    if (metrics != NULL && phase_start && phase_end >= phase_start) metrics->validate_ns += phase_end - phase_start;
    if (err != EN_ERR_NONE) {
        en_error_append(controller, err, "target path validation failed");
        return rollback_and_report(controller, traffic_key, intent->intent_id, old_path_id, target_path, err, metrics);
    }

    en_path_t *previous_path = en_find_path(controller, old_path_id);
    phase_start = en_monotonic_ns();
    err = commit_retry(controller, intent, target_path, previous_path, traffic_key);
    phase_end = en_monotonic_ns();
    if (metrics != NULL && phase_start && phase_end >= phase_start) metrics->commit_ns += phase_end - phase_start;
    if (err != EN_ERR_NONE) {
        en_error_append(controller, err, "forwarding commit failed");
        return rollback_and_report(controller, traffic_key, intent->intent_id, old_path_id, target_path, err, metrics);
    }

    phase_start = en_monotonic_ns();
    err = confirm_retry(controller, intent, target_path, traffic_key);
    phase_end = en_monotonic_ns();
    if (metrics != NULL && phase_start && phase_end >= phase_start) metrics->post_validation_ns += phase_end - phase_start;
    if (err != EN_ERR_NONE) {
        return rollback_and_report(controller, traffic_key, intent->intent_id, old_path_id, target_path, err, metrics);
    }

    phase_start = en_monotonic_ns();
    err = cleanup_previous(controller, traffic_key, previous_path, target_path);
    phase_end = en_monotonic_ns();
    if (metrics != NULL && phase_start && phase_end >= phase_start) metrics->post_validation_ns += phase_end - phase_start;
    if (err != EN_ERR_NONE) {
        en_error_append(controller, err, "previous path cleanup failed");
        return rollback_and_report(controller, traffic_key, intent->intent_id, old_path_id, target_path, err, metrics);
    }

    (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_COMPLETED);
    en_audit_append(controller, "TRANSITION_COMPLETED", "target path is active", target_path->path_id);
    return EN_ERR_NONE;
}

static en_error_code_t rollback_and_report(
    en_controller_t *controller,
    const char *traffic_key,
    const char *intent_id,
    const char *old_path_id,
    en_path_t *target_path,
    en_error_code_t transition_error,
    en_transition_metrics_t *metrics
)
{
    unsigned long long start = en_monotonic_ns();
    en_error_code_t rollback_error = rollback(controller, traffic_key, intent_id, old_path_id, target_path);
    unsigned long long end = en_monotonic_ns();
    if (metrics != NULL && start && end >= start) metrics->rollback_ns += end - start;
    return rollback_error == EN_ERR_NONE ? transition_error : rollback_error;
}

static void transition_retry_wait(en_controller_t *controller, const en_intent_t *intent, int attempt, const char *phase, const char *path_id)
{
    if (intent->transition.retry_backoff_ms <= 0) return;
    long long delay = (long long)intent->transition.retry_backoff_ms * (attempt + 1LL);
    if (delay > INT_MAX) delay = INT_MAX;
    en_audit_append(controller, "TRANSITION_RETRY", phase, path_id);
    sleep_ms((int)delay);
}

static en_error_code_t prepare_retry(en_controller_t *controller, const en_intent_t *intent, en_path_t *path)
{
    en_error_code_t error = EN_ERR_NONE;
    for (int attempt = 0; attempt <= intent->transition.retry_count; attempt++) {
        error = prepare_path(controller, path);
        if (error == EN_ERR_NONE || attempt == intent->transition.retry_count) return error;
        transition_retry_wait(controller, intent, attempt, "retry target preparation", path->path_id);
    }
    return error;
}

static en_error_code_t validate_retry(en_controller_t *controller, const en_intent_t *intent, en_path_t *path, const char *traffic_key, en_transition_metrics_t *metrics)
{
    en_error_code_t error = EN_ERR_NONE;
    for (int attempt = 0; attempt <= intent->transition.retry_count; attempt++) {
        error = validate_path(controller, path, traffic_key, intent->intent_id, metrics);
        if (error == EN_ERR_NONE || attempt == intent->transition.retry_count) return error;
        transition_retry_wait(controller, intent, attempt, "retry target validation", path->path_id);
    }
    return error;
}

static en_error_code_t commit_retry(en_controller_t *controller, const en_intent_t *intent, en_path_t *target_path, en_path_t *previous_path, const char *traffic_key)
{
    en_error_code_t error = EN_ERR_NONE;
    for (int attempt = 0; attempt <= intent->transition.retry_count; attempt++) {
        error = commit_path(controller, intent, target_path, previous_path, traffic_key);
        if (error == EN_ERR_NONE || attempt == intent->transition.retry_count) return error;
        transition_retry_wait(controller, intent, attempt, "retry forwarding commit", target_path->path_id);
    }
    return error;
}

static en_error_code_t confirm_retry(en_controller_t *controller, const en_intent_t *intent, en_path_t *target_path, const char *traffic_key)
{
    en_error_code_t error = EN_ERR_NONE;
    for (int attempt = 0; attempt <= intent->transition.retry_count; attempt++) {
        error = confirm_path(controller, target_path, traffic_key, intent->intent_id);
        if (error == EN_ERR_NONE || attempt == intent->transition.retry_count) return error;
        transition_retry_wait(controller, intent, attempt, "retry forwarding verification", target_path->path_id);
    }
    return error;
}

static en_error_code_t prepare_path(en_controller_t *controller, en_path_t *path)
{
    path->operational_state = EN_PATH_PREPARING;
    en_audit_append(controller, "PREPARE", "prepare target tunnels", path->path_id);
    for (size_t i = 0; i < path->segment_count; i++) {
        en_segment_t *segment = &path->segments[i];
        en_tunnel_t desired = {0};
        en_tunnel_t *configured = en_find_desired_tunnel(controller, segment->tunnel_id);
        if (configured != NULL) {
            desired = *configured;
        } else {
            en_copy_id(desired.tunnel_id, sizeof(desired.tunnel_id), segment->tunnel_id);
            en_copy_id(desired.local_node, sizeof(desired.local_node), segment->from_node);
            en_copy_id(desired.remote_node, sizeof(desired.remote_node), segment->to_node);
            en_copy_id(desired.protocol, sizeof(desired.protocol), "ipsec");
            desired.state = EN_TUNNEL_CONFIGURED;
            desired.health = EN_HEALTH_UNKNOWN;
        }

        en_tunnel_t observed = {0};
        en_error_code_t err = controller->strongswan.ensure_tunnel(controller->strongswan.ctx, &desired, &observed);
        if (err != EN_ERR_NONE) {
            return err;
        }

        en_tunnel_t *existing = en_find_tunnel(controller, observed.tunnel_id);
        if (existing != NULL) {
            *existing = observed;
        } else if (controller->state.observed_tunnel_count < EN_MAX_TUNNELS) {
            controller->state.observed_tunnels[controller->state.observed_tunnel_count++] = observed;
        } else {
            return EN_ERR_INVALID_ARGUMENT;
        }
    }
    path->operational_state = EN_PATH_READY;
    return EN_ERR_NONE;
}

static en_error_code_t validate_path(en_controller_t *controller, en_path_t *path, const char *traffic_key, const char *intent_id, en_transition_metrics_t *metrics)
{
    (void)en_transition_status_update(controller, traffic_key, intent_id, EN_TRANSITION_VALIDATING);
    en_audit_append(controller, "READY", "validate target path", path->path_id);
    if (controller->health_probe.validate_path == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    en_path_health_t health = {0};
    unsigned long long probe_start = en_monotonic_ns();
    en_error_code_t err = controller->health_probe.validate_path(controller->health_probe.ctx, path, &health);
    unsigned long long probe_end = en_monotonic_ns();
    if (metrics != NULL) {
        metrics->health_probe_count++;
        if (probe_start && probe_end >= probe_start) metrics->health_probe_ns += probe_end - probe_start;
    }
    if (err != EN_ERR_NONE) {
        return err;
    }
    en_path_health_t *existing = en_find_health(controller, health.path_id);
    if (existing != NULL) {
        *existing = health;
    } else if (controller->state.health_count < EN_MAX_PATHS) {
        controller->state.health[controller->state.health_count++] = health;
    }
    if (health.state != EN_HEALTH_HEALTHY && health.state != EN_HEALTH_DEGRADED) {
        return EN_ERR_PATH_VALIDATION_FAILED;
    }
    (void)en_transition_status_update(controller, traffic_key, intent_id, EN_TRANSITION_READY);
    return EN_ERR_NONE;
}

static en_error_code_t commit_path(en_controller_t *controller, const en_intent_t *intent, en_path_t *target_path, en_path_t *previous_path, const char *traffic_key)
{
    en_audit_append(controller, "COMMIT", "commit forwarding switch", target_path->path_id);
    if (intent->transition.strategy == EN_TRANSITION_GRACEFUL) {
        if (previous_path != NULL && previous_path != target_path) {
            (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_PAUSING);
            en_audit_append(controller, "PAUSING", "begin flow-aware graceful switch", previous_path->path_id);
            previous_path->operational_state = EN_PATH_DRAINING;
            (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_DRAINING);
            en_audit_append(controller, "DRAINING", "waiting for established flows to drain", previous_path->path_id);
            if (controller->vpp.graceful_switch == NULL) return EN_ERR_STATE_CONFLICT;
            en_error_code_t drain_error = controller->vpp.graceful_switch(
                controller->vpp.ctx, traffic_key, previous_path, target_path,
                intent->transition.max_pause_ms, intent->transition.drain_timeout_ms);
            if (drain_error != EN_ERR_NONE) return drain_error;
            en_set_applied_path(controller, traffic_key, target_path->path_id);
            target_path->operational_state = EN_PATH_ACTIVE;
            return EN_ERR_NONE;
        }
    } else if (intent->transition.strategy == EN_TRANSITION_FLOW_PRESERVE) {
        en_error_append(controller, EN_ERR_STATE_CONFLICT, "flow-preserve is reserved for stage 2");
        return EN_ERR_STATE_CONFLICT;
    }

    (void)en_transition_status_update(controller, traffic_key, intent->intent_id, EN_TRANSITION_SWITCHING);
    if (controller->vpp.install_path == NULL) {
        en_error_append(controller, EN_ERR_INVALID_ARGUMENT, "vpp adapter is missing install_path");
        return EN_ERR_INVALID_ARGUMENT;
    }
    en_error_code_t err = controller->vpp.install_path(controller->vpp.ctx, traffic_key, target_path);
    if (err != EN_ERR_NONE) {
        en_error_append(controller, EN_ERR_FORWARDING_UPDATE_FAILED, "forwarding update failed");
        return EN_ERR_FORWARDING_UPDATE_FAILED;
    }
    en_set_applied_path(controller, traffic_key, target_path->path_id);
    target_path->operational_state = EN_PATH_ACTIVE;
    return EN_ERR_NONE;
}

static en_error_code_t cleanup_previous(en_controller_t *controller, const char *traffic_key, en_path_t *previous_path, en_path_t *target_path)
{
    if (previous_path == NULL || previous_path == target_path) return EN_ERR_NONE;
    if (controller->vpp.remove_path != NULL &&
        controller->vpp.remove_path(controller->vpp.ctx, traffic_key, previous_path) != EN_ERR_NONE) {
        return EN_ERR_FORWARDING_UPDATE_FAILED;
    }
    if (controller->strongswan.remove_tunnel != NULL &&
        remove_unshared_tunnels(controller, traffic_key, previous_path, target_path) != EN_ERR_NONE) {
        return EN_ERR_TUNNEL_ESTABLISH_TIMEOUT;
    }
    previous_path->operational_state = EN_PATH_STANDBY;
    for (size_t i = 0; i < controller->state.applied_count; i++) {
        if (!en_streq(controller->state.traffic_keys[i], traffic_key) &&
            en_streq(controller->state.applied_paths[i], previous_path->path_id)) {
            previous_path->operational_state = EN_PATH_ACTIVE;
            break;
        }
    }
    en_audit_append(controller, "PREVIOUS_PATH_REMOVED", "previous path cleanup completed", previous_path->path_id);
    return EN_ERR_NONE;
}

static en_error_code_t confirm_path(en_controller_t *controller, en_path_t *target_path, const char *traffic_key, const char *intent_id)
{
    (void)en_transition_status_update(controller, traffic_key, intent_id, EN_TRANSITION_VERIFYING);
    en_audit_append(controller, "CONFIRM", "confirm forwarding state", target_path->path_id);
    if (controller->vpp.active_path == NULL) {
        return EN_ERR_NONE;
    }
    const char *active = controller->vpp.active_path(controller->vpp.ctx, traffic_key);
    if (!en_streq(active, target_path->path_id)) {
        en_error_append(controller, EN_ERR_FORWARDING_UPDATE_FAILED, "forwarding verification failed");
        return EN_ERR_FORWARDING_UPDATE_FAILED;
    }
    return EN_ERR_NONE;
}

static en_error_code_t rollback(en_controller_t *controller, const char *traffic_key, const char *intent_id, const char *old_path_id, en_path_t *target_path)
{
    (void)en_transition_status_update(controller, traffic_key, intent_id, EN_TRANSITION_ROLLING_BACK);
    en_path_t *rollback_path = en_find_path(controller, old_path_id);
    bool cleanup_failed = false;
    en_invalidate_applied_path(controller, traffic_key);
    if (target_path != NULL && controller->strongswan.remove_tunnel != NULL) {
        en_error_code_t tunnel_error = remove_unshared_tunnels(controller, traffic_key, target_path, rollback_path);
        if (tunnel_error != EN_ERR_NONE) {
            en_error_append(controller, EN_ERR_ROLLBACK_FAILED, "new tunnel removal failed");
            cleanup_failed = true;
        }
    }
    if (target_path != NULL && controller->vpp.remove_path != NULL) {
        en_error_code_t remove_error = controller->vpp.remove_path(controller->vpp.ctx, traffic_key, target_path);
        if (remove_error != EN_ERR_NONE) {
            en_error_append(controller, EN_ERR_ROLLBACK_FAILED, "new path removal failed");
            cleanup_failed = true;
        }
    }
    if (old_path_id == NULL || old_path_id[0] == '\0') {
        if (!cleanup_failed) en_clear_applied_path(controller, traffic_key);
        en_audit_append(controller, "ROLLBACK_EMPTY", "initial transition has no previous path", "");
        (void)en_transition_status_update(controller, traffic_key, intent_id, EN_TRANSITION_FAILED);
        return cleanup_failed ? EN_ERR_ROLLBACK_FAILED : EN_ERR_NONE;
    }
    if (rollback_path == NULL || controller->vpp.install_path == NULL ||
        prepare_path(controller, rollback_path) != EN_ERR_NONE ||
        controller->vpp.install_path(controller->vpp.ctx, traffic_key, rollback_path) != EN_ERR_NONE) {
        en_error_append(controller, EN_ERR_ROLLBACK_FAILED, "previous path reinstall failed");
        (void)en_transition_status_update(controller, traffic_key, intent_id, EN_TRANSITION_FAILED);
        return EN_ERR_ROLLBACK_FAILED;
    }
    en_set_applied_path(controller, traffic_key, old_path_id);
    rollback_path->operational_state = EN_PATH_ACTIVE;
    if (cleanup_failed) {
        en_invalidate_applied_path(controller, traffic_key);
        en_audit_append(controller, "ROLLBACK_INCOMPLETE", "previous path reinstalled; cleanup failed", old_path_id);
        (void)en_transition_status_update(controller, traffic_key, intent_id, EN_TRANSITION_FAILED);
        return EN_ERR_ROLLBACK_FAILED;
    }
    en_audit_append(controller, "ROLLBACK_COMPLETED", "previous path restored", old_path_id);
    (void)en_transition_status_update(controller, traffic_key, intent_id, EN_TRANSITION_ROLLED_BACK);
    return EN_ERR_NONE;
}

static bool path_uses_tunnel(const en_path_t *path, const char *tunnel_id)
{
    if (path == NULL || tunnel_id == NULL) return false;
    for (size_t index = 0; index < path->segment_count; index++) {
        if (en_streq(path->segments[index].tunnel_id, tunnel_id)) return true;
    }
    return false;
}

static en_error_code_t remove_unshared_tunnels(en_controller_t *controller, const char *traffic_key, const en_path_t *target_path, const en_path_t *rollback_path)
{
    en_error_code_t result = EN_ERR_NONE;
    for (size_t index = 0; index < target_path->segment_count; index++) {
        const char *tunnel_id = target_path->segments[index].tunnel_id;
        if (path_uses_tunnel(rollback_path, tunnel_id)) continue;
        bool shared = false;
        for (size_t i = 0; i < controller->state.applied_count; i++) {
            if (en_streq(controller->state.traffic_keys[i], traffic_key)) continue;
            if (path_uses_tunnel(en_find_path(controller, controller->state.applied_paths[i]), tunnel_id)) shared = true;
        }
        if (shared) continue;
        en_tunnel_t *observed = en_find_tunnel(controller, tunnel_id);
        if (observed == NULL || observed->state == EN_TUNNEL_ABSENT) continue;
        en_tunnel_t removed = {0};
        en_error_code_t error = controller->strongswan.remove_tunnel(controller->strongswan.ctx, observed, &removed);
        if (error != EN_ERR_NONE) result = error;
        else {
            *observed = removed;
            observed->state = EN_TUNNEL_ABSENT;
        }
    }
    return result;
}

static void sleep_ms(int milliseconds)
{
    if (milliseconds <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}
