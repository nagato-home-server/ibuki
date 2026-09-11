#ifndef EVENTNET_RENDER_COMMANDS_H
#define EVENTNET_RENDER_COMMANDS_H

#include "eventnet/types.h"

en_error_code_t en_render_swanctl_initiate(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_swanctl_initiate_uri(
    const en_tunnel_t *tunnel,
    const char *uri,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_swanctl_terminate(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_swanctl_terminate_uri(
    const en_tunnel_t *tunnel,
    const char *uri,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_swanctl_list_sas_uri(
    const en_tunnel_t *tunnel,
    const char *uri,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_swanctl_load_conns_uri(
    const char *uri,
    const char *filename,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_swanctl_conf(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_route_replace(
    const en_path_t *path,
    const en_tunnel_t *egress_tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_route_replace_entry(
    const en_route_t *route,
    char *buf,
    size_t buf_len
);

/* Resolve the logical L3 egress supplied by a tunnel backend. */
en_error_code_t en_route_resolve_tunnel_egress(
    const en_route_t *route,
    const en_tunnel_t *tunnel,
    en_route_t *resolved_route
);

en_error_code_t en_render_vpp_route_delete(
    const en_path_t *path,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_route_delete_with_tunnel(
    const en_path_t *path,
    const en_tunnel_t *egress_tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_route_delete_entry(
    const en_route_t *route,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_gre_create(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_gre_delete(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_gre_set_address(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_gre_set_mtu(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_gre_set_up(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_gre_route_replace(
    const en_path_t *path,
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

en_error_code_t en_render_vpp_gre_route_delete(
    const en_path_t *path,
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
);

#endif
