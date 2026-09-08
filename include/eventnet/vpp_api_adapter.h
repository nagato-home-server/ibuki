#ifndef EVENTNET_VPP_API_ADAPTER_H
#define EVENTNET_VPP_API_ADAPTER_H

#include "eventnet/controller.h"
#include "eventnet/vpp_observer.h"

typedef en_error_code_t (*en_vpp_api_install_fn)(void *ctx, const char *traffic_key, const en_path_t *path);
typedef en_error_code_t (*en_vpp_api_remove_fn)(void *ctx, const char *traffic_key, const en_path_t *path);
typedef const char *(*en_vpp_api_active_fn)(void *ctx, const char *traffic_key);
typedef en_error_code_t (*en_vpp_api_observe_route_fn)(void *ctx, const char *destination_prefix, en_vpp_route_observation_t *observation);
typedef en_error_code_t (*en_vpp_api_observe_interface_fn)(void *ctx, const char *interface_name, en_vpp_interface_observation_t *observation);
typedef en_error_code_t (*en_vpp_api_route_operation_fn)(void *ctx, const en_route_t *route);
typedef en_error_code_t (*en_vpp_api_vlan_operation_fn)(void *ctx, const en_vpp_edge_t *edge, int vlan_id);
typedef en_error_code_t (*en_vpp_api_vlan_table_operation_fn)(void *ctx, const en_vpp_edge_t *edge, int vlan_id, int table_id);
typedef en_error_code_t (*en_vpp_api_vrf_operation_fn)(void *ctx, int table_id);

typedef struct {
    en_vpp_api_route_operation_fn add_route;
    en_vpp_api_route_operation_fn remove_route;
    en_vpp_api_vlan_operation_fn create_vlan_subinterface;
    en_vpp_api_vlan_operation_fn remove_vlan_subinterface;
    en_vpp_api_vlan_table_operation_fn set_vlan_interface_table;
    en_vpp_api_vrf_operation_fn ensure_vrf;
    void *ctx;
} en_vpp_api_message_ops_t;

typedef struct {
    en_vpp_api_message_ops_t operations;
    en_traffic_selector_t traffic;
    const en_vpp_edge_t *edges;
    size_t edge_count;
} en_vpp_api_message_ctx_t;

typedef struct {
    en_vpp_api_install_fn install_path;
    en_vpp_api_remove_fn remove_path;
    en_vpp_api_active_fn active_path;
    en_vpp_api_observe_route_fn observe_route;
    en_vpp_api_observe_interface_fn observe_interface;
    void *ctx;
} en_vpp_api_ctx_t;

en_vpp_adapter_t en_vpp_api_adapter(en_vpp_api_ctx_t *ctx);
en_vpp_adapter_t en_vpp_api_message_adapter(en_vpp_api_message_ctx_t *ctx);
en_error_code_t en_vpp_api_observe_route(en_vpp_api_ctx_t *ctx, const char *destination_prefix, en_vpp_route_observation_t *observation);
en_error_code_t en_vpp_api_observe_interface(en_vpp_api_ctx_t *ctx, const char *interface_name, en_vpp_interface_observation_t *observation);
en_error_code_t en_vpp_api_apply_path_operations(
    const en_vpp_api_message_ops_t *ops,
    const en_path_t *path,
    const en_traffic_selector_t *traffic,
    const en_vpp_edge_t *edges,
    size_t edge_count,
    bool remove
);
en_error_code_t en_vpp_api_observe_event_json(
    en_vpp_api_ctx_t *ctx,
    const char *destination_prefix,
    const char *path_id,
    const char *route_id,
    long long timestamp_ms,
    char *output,
    size_t output_len
);

#endif
