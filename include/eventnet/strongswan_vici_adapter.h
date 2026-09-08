#ifndef EVENTNET_STRONGSWAN_VICI_ADAPTER_H
#define EVENTNET_STRONGSWAN_VICI_ADAPTER_H

#include "eventnet/controller.h"
#include "eventnet/strongswan_observer.h"

typedef struct en_strongswan_vici_client en_strongswan_vici_client_t;

typedef en_error_code_t (*en_vici_ensure_tunnel_fn)(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed);
typedef en_error_code_t (*en_vici_remove_tunnel_fn)(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed);
typedef en_error_code_t (*en_vici_observe_tunnel_fn)(void *ctx, const char *child_id, en_strongswan_sa_observation_t *observation);

typedef struct {
    en_vici_ensure_tunnel_fn ensure_tunnel;
    en_vici_remove_tunnel_fn remove_tunnel;
    en_vici_observe_tunnel_fn observe_tunnel;
    void *ctx;
} en_strongswan_vici_ctx_t;

en_strongswan_adapter_t en_strongswan_vici_adapter(en_strongswan_vici_ctx_t *ctx);
en_error_code_t en_strongswan_vici_bind_client(en_strongswan_vici_ctx_t *ctx, en_strongswan_vici_client_t *client);
en_error_code_t en_strongswan_vici_observe_tunnel(en_strongswan_vici_ctx_t *ctx, const char *child_id, en_strongswan_sa_observation_t *observation);
en_error_code_t en_strongswan_vici_observe_event_json(
    en_strongswan_vici_ctx_t *ctx,
    const char *child_id,
    const char *path_id,
    long long timestamp_ms,
    char *output,
    size_t output_len
);

#endif
