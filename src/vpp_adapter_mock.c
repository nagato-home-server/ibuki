#include "eventnet/mock_adapters.h"

#include <stdio.h>
#include <string.h>

static en_error_code_t install_path(void *ctx, const char *traffic_key, const en_path_t *path)
{
    en_vpp_mock_t *mock = ctx;
    if (mock == NULL || traffic_key == NULL || path == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (mock->fail_update_count > 0) {
        mock->fail_update_count--;
        return EN_ERR_FORWARDING_UPDATE_FAILED;
    }
    if (mock->fail_next_update) {
        mock->fail_next_update = false;
        return EN_ERR_FORWARDING_UPDATE_FAILED;
    }
    mock->install_count++;
    for (size_t i = 0; i < mock->active_count; i++) {
        if (strcmp(mock->traffic_keys[i], traffic_key) == 0) {
            snprintf(mock->active_paths[i], sizeof(mock->active_paths[i]), "%s", path->path_id);
            return EN_ERR_NONE;
        }
    }
    if (mock->active_count >= EN_MAX_CANDIDATES) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    size_t idx = mock->active_count++;
    snprintf(mock->traffic_keys[idx], sizeof(mock->traffic_keys[idx]), "%s", traffic_key);
    snprintf(mock->active_paths[idx], sizeof(mock->active_paths[idx]), "%s", path->path_id);
    return EN_ERR_NONE;
}

static const char *active_path(void *ctx, const char *traffic_key)
{
    en_vpp_mock_t *mock = ctx;
    if (mock == NULL || traffic_key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < mock->active_count; i++) {
        if (strcmp(mock->traffic_keys[i], traffic_key) == 0) {
            return mock->active_paths[i];
        }
    }
    return NULL;
}

static en_error_code_t remove_path(void *ctx, const char *traffic_key, const en_path_t *path)
{
    en_vpp_mock_t *mock = ctx;
    if (mock == NULL || traffic_key == NULL || path == NULL) return EN_ERR_INVALID_ARGUMENT;
    mock->remove_count++;
    for (size_t i = 0; i < mock->active_count; i++) {
        if (strcmp(mock->traffic_keys[i], traffic_key) == 0 && strcmp(mock->active_paths[i], path->path_id) == 0) {
            for (size_t j = i + 1; j < mock->active_count; j++) {
                snprintf(mock->traffic_keys[j - 1], sizeof(mock->traffic_keys[j - 1]), "%s", mock->traffic_keys[j]);
                snprintf(mock->active_paths[j - 1], sizeof(mock->active_paths[j - 1]), "%s", mock->active_paths[j]);
            }
            mock->active_count--;
            return EN_ERR_NONE;
        }
    }
    return EN_ERR_NONE;
}

static en_error_code_t graceful_switch(void *ctx, const char *traffic_key, const en_path_t *previous_path,
    const en_path_t *target_path, int max_pause_ms, int drain_timeout_ms)
{
    en_vpp_mock_t *mock = ctx;
    if (mock == NULL || traffic_key == NULL || previous_path == NULL || target_path == NULL ||
        max_pause_ms < 0 || drain_timeout_ms < 0) return EN_ERR_INVALID_ARGUMENT;
    mock->graceful_switch_count++;
    /* The mock models an immediate drain (zero remaining flows); it does not
       claim to test dataplane flow tracking. */
    return install_path(ctx, traffic_key, target_path);
}

en_vpp_adapter_t en_vpp_mock_adapter(en_vpp_mock_t *mock)
{
    en_vpp_adapter_t adapter = {
        .install_path = install_path,
        .remove_path = remove_path,
        .active_path = active_path,
        .graceful_switch = mock != NULL && mock->supports_graceful_switch ? graceful_switch : NULL,
        .ctx = mock,
    };
    return adapter;
}
