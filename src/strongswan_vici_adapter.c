#include "eventnet/strongswan_vici_adapter.h"

#include <string.h>

#if defined(EVENTNET_ENABLE_STRONGSWAN_VICI)
#include "eventnet/strongswan_vici_client.h"
#endif

static en_error_code_t ensure_tunnel(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    en_strongswan_vici_ctx_t *vici = ctx;
    if (vici == NULL || vici->ensure_tunnel == NULL || desired == NULL || observed == NULL || desired->tunnel_id[0] == '\0') return EN_ERR_INVALID_ARGUMENT;
    en_error_code_t err = vici->ensure_tunnel(vici->ctx, desired, observed);
    if (err != EN_ERR_NONE) return err;
    return strcmp(observed->tunnel_id, desired->tunnel_id) == 0 ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
}

static en_error_code_t remove_tunnel(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    en_strongswan_vici_ctx_t *vici = ctx;
    if (vici == NULL || vici->remove_tunnel == NULL || desired == NULL || observed == NULL || desired->tunnel_id[0] == '\0') return EN_ERR_INVALID_ARGUMENT;
    en_error_code_t err = vici->remove_tunnel(vici->ctx, desired, observed);
    if (err != EN_ERR_NONE) return err;
    return strcmp(observed->tunnel_id, desired->tunnel_id) == 0 ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
}

en_error_code_t en_strongswan_vici_observe_tunnel(en_strongswan_vici_ctx_t *ctx, const char *child_id, en_strongswan_sa_observation_t *observation)
{
    if (ctx == NULL || ctx->observe_tunnel == NULL || child_id == NULL || child_id[0] == '\0' || observation == NULL) return EN_ERR_INVALID_ARGUMENT;
    en_error_code_t err = ctx->observe_tunnel(ctx->ctx, child_id, observation);
    if (err != EN_ERR_NONE) return err;
    return strcmp(observation->child_id, child_id) == 0 ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
}

en_error_code_t en_strongswan_vici_observe_event_json(en_strongswan_vici_ctx_t *ctx, const char *child_id, const char *path_id, long long timestamp_ms, char *output, size_t output_len)
{
    en_strongswan_sa_observation_t observation = {0};
    en_error_code_t err = en_strongswan_vici_observe_tunnel(ctx, child_id, &observation);
    if (err != EN_ERR_NONE) return err;
    return en_strongswan_observation_to_event_json(&observation, path_id, timestamp_ms, output, output_len);
}

en_strongswan_adapter_t en_strongswan_vici_adapter(en_strongswan_vici_ctx_t *ctx)
{
    en_strongswan_adapter_t adapter = {
        .ensure_tunnel = ensure_tunnel,
        .remove_tunnel = remove_tunnel,
        .ctx = ctx,
    };
    return adapter;
}

#if defined(EVENTNET_ENABLE_STRONGSWAN_VICI)
static en_error_code_t client_ensure_tunnel(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    char error[128] = {0};
    en_strongswan_sa_observation_t observation = {0};
    if (ctx == NULL || desired == NULL || observed == NULL) return EN_ERR_INVALID_ARGUMENT;
    en_error_code_t err = en_strongswan_vici_client_initiate(ctx, desired->tunnel_id, error, sizeof(error));
    if (err != EN_ERR_NONE) return err;
    err = en_strongswan_vici_client_observe_child(ctx, desired->tunnel_id, &observation, error, sizeof(error));
    if (err != EN_ERR_NONE) return err;
    *observed = *desired;
    observed->state = observation.state;
    observed->health = observation.health;
    return EN_ERR_NONE;
}

static en_error_code_t client_remove_tunnel(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    char error[128] = {0};
    if (ctx == NULL || desired == NULL || observed == NULL) return EN_ERR_INVALID_ARGUMENT;
    en_error_code_t err = en_strongswan_vici_client_terminate(ctx, desired->tunnel_id, error, sizeof(error));
    if (err != EN_ERR_NONE) return err;
    *observed = *desired;
    observed->state = EN_TUNNEL_DOWN;
    observed->health = EN_HEALTH_FAILED;
    return EN_ERR_NONE;
}

static en_error_code_t client_observe_tunnel(void *ctx, const char *child_id, en_strongswan_sa_observation_t *observation)
{
    char error[128] = {0};
    return en_strongswan_vici_client_observe_child(ctx, child_id, observation, error, sizeof(error));
}

en_error_code_t en_strongswan_vici_bind_client(en_strongswan_vici_ctx_t *ctx, en_strongswan_vici_client_t *client)
{
    if (ctx == NULL || client == NULL) return EN_ERR_INVALID_ARGUMENT;
    ctx->ensure_tunnel = client_ensure_tunnel;
    ctx->remove_tunnel = client_remove_tunnel;
    ctx->observe_tunnel = client_observe_tunnel;
    ctx->ctx = client;
    return EN_ERR_NONE;
}
#else
en_error_code_t en_strongswan_vici_bind_client(en_strongswan_vici_ctx_t *ctx, en_strongswan_vici_client_t *client)
{
    if (ctx == NULL || client == NULL) return EN_ERR_INVALID_ARGUMENT;
    return EN_ERR_STATE_CONFLICT;
}
#endif
