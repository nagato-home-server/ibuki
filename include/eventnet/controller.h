#ifndef EVENTNET_CONTROLLER_H
#define EVENTNET_CONTROLLER_H

#include "eventnet/types.h"

typedef struct en_controller en_controller_t;

typedef struct {
    en_error_code_t (*ensure_tunnel)(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed);
    en_error_code_t (*remove_tunnel)(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed);
    void *ctx;
} en_strongswan_adapter_t;

typedef struct {
    en_error_code_t (*install_path)(void *ctx, const char *traffic_key, const en_path_t *path);
    en_error_code_t (*remove_path)(void *ctx, const char *traffic_key, const en_path_t *path);
    const char *(*active_path)(void *ctx, const char *traffic_key);
    void *ctx;
} en_vpp_adapter_t;

typedef struct {
    en_error_code_t (*validate_path)(void *ctx, const en_path_t *path, en_path_health_t *health);
    void *ctx;
} en_health_probe_t;

en_controller_t *en_controller_create(
    const en_path_t *paths,
    size_t path_count,
    en_strongswan_adapter_t strongswan,
    en_vpp_adapter_t vpp,
    en_health_probe_t health_probe
);

en_controller_t *en_controller_create_with_tunnels(
    const en_path_t *paths,
    size_t path_count,
    const en_tunnel_t *tunnels,
    size_t tunnel_count,
    en_strongswan_adapter_t strongswan,
    en_vpp_adapter_t vpp,
    en_health_probe_t health_probe
);

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
);

void en_controller_destroy(en_controller_t *controller);

en_error_code_t en_controller_submit_intent(
    en_controller_t *controller,
    const en_intent_t *intent,
    en_reconcile_result_t *result
);

size_t en_controller_audit_events(
    const en_controller_t *controller,
    const en_audit_event_t **events
);

size_t en_controller_errors(
    const en_controller_t *controller,
    const en_error_t **errors
);

const char *en_controller_applied_path(
    const en_controller_t *controller,
    const char *traffic_key
);

const en_path_t *en_controller_find_path(
    const en_controller_t *controller,
    const char *path_id
);

bool en_controller_path_nodes_enabled(
    const en_controller_t *controller,
    const en_path_t *path
);

en_error_code_t en_controller_restore_applied_path(
    en_controller_t *controller,
    const char *traffic_key,
    const char *path_id
);

void en_make_traffic_key(const en_traffic_selector_t *traffic, char *buf, size_t buf_len);

#endif
