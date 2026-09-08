#ifndef EVENTNET_COMMAND_ADAPTERS_H
#define EVENTNET_COMMAND_ADAPTERS_H

#include "eventnet/controller.h"

typedef struct {
    bool dry_run;
    bool verify_tunnel;
    bool connections_loaded;
    bool block_non_ipsec;
    char swanctl_uri[256];
    char swanctl_config_file[256];
    char ensure_tunnel_command[256];
    char verify_tunnel_command[256];
    char remove_tunnel_command[256];
    char xfrm_block_tunnels[EN_MAX_TUNNELS][EN_MAX_ID_LEN];
    size_t xfrm_block_count;
} en_strongswan_command_ctx_t;

typedef struct {
    bool dry_run;
    bool verify_route;
    bool has_vlan_id;
    bool deny_unmatched_vlan;
    int vlan_id;
    char vppctl_socket[256];
    char install_path_command[256];
    const en_tunnel_t *tunnels;
    size_t tunnel_count;
    const en_vpp_edge_t *edges;
    size_t edge_count;
    char active_paths[EN_MAX_CANDIDATES][EN_MAX_ID_LEN];
    char traffic_keys[EN_MAX_CANDIDATES][EN_MAX_TRAFFIC_KEY_LEN];
    const en_path_t *active_path_objects[EN_MAX_CANDIDATES];
    size_t active_count;
} en_vpp_command_ctx_t;

typedef struct {
    bool dry_run;
    char validate_path_command[256];
} en_health_command_ctx_t;

en_strongswan_adapter_t en_strongswan_command_adapter(en_strongswan_command_ctx_t *ctx);
en_vpp_adapter_t en_vpp_command_adapter(en_vpp_command_ctx_t *ctx);
en_health_probe_t en_health_command_probe(en_health_command_ctx_t *ctx);

#endif
