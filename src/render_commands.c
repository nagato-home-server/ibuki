#include "eventnet/render_commands.h"

#include <stdio.h>
#include <ctype.h>

static int valid_command_token(const char *value)
{
    if (value == NULL || value[0] == '\0') return 0;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == ':' || *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-')) return 0;
    }
    return 1;
}

#define FORMAT_COMMAND(buffer, buffer_len, ...) do { \
    int format_length = snprintf((buffer), (buffer_len), __VA_ARGS__); \
    if (format_length < 0 || (size_t)format_length >= (buffer_len)) return EN_ERR_INVALID_ARGUMENT; \
} while (0)

en_error_code_t en_render_swanctl_initiate(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
)
{
    return en_render_swanctl_initiate_uri(tunnel, NULL, buf, buf_len);
}

en_error_code_t en_render_swanctl_initiate_uri(
    const en_tunnel_t *tunnel,
    const char *uri,
    char *buf,
    size_t buf_len
)
{
    if (tunnel == NULL || buf == NULL || buf_len == 0 || tunnel->tunnel_id[0] == '\0' || !valid_command_token(tunnel->tunnel_id)) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (uri != NULL && uri[0] != '\0' && !valid_command_token(uri)) return EN_ERR_INVALID_ARGUMENT;
    if (uri == NULL || uri[0] == '\0') {
        FORMAT_COMMAND(buf, buf_len, "swanctl --initiate --child %s", tunnel->tunnel_id);
    } else {
        FORMAT_COMMAND(buf, buf_len, "swanctl --uri %s --initiate --child %s", uri, tunnel->tunnel_id);
    }
    return EN_ERR_NONE;
}

en_error_code_t en_render_swanctl_terminate(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
)
{
    return en_render_swanctl_terminate_uri(tunnel, NULL, buf, buf_len);
}

en_error_code_t en_render_swanctl_terminate_uri(
    const en_tunnel_t *tunnel,
    const char *uri,
    char *buf,
    size_t buf_len
)
{
    if (tunnel == NULL || buf == NULL || buf_len == 0 || tunnel->tunnel_id[0] == '\0' || !valid_command_token(tunnel->tunnel_id)) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (uri != NULL && uri[0] != '\0' && !valid_command_token(uri)) return EN_ERR_INVALID_ARGUMENT;
    if (uri == NULL || uri[0] == '\0') {
        FORMAT_COMMAND(buf, buf_len, "swanctl --terminate --child %s", tunnel->tunnel_id);
    } else {
        FORMAT_COMMAND(buf, buf_len, "swanctl --uri %s --terminate --child %s", uri, tunnel->tunnel_id);
    }
    return EN_ERR_NONE;
}

en_error_code_t en_render_swanctl_conf(
    const en_tunnel_t *tunnel,
    char *buf,
    size_t buf_len
)
{
    if (tunnel == NULL || buf == NULL || buf_len == 0 || tunnel->tunnel_id[0] == '\0' ||
        !valid_command_token(tunnel->tunnel_id) || !valid_command_token(tunnel->local_endpoint) ||
        !valid_command_token(tunnel->remote_endpoint) || !valid_command_token(tunnel->local_traffic_selector) ||
        !valid_command_token(tunnel->remote_traffic_selector)) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    FORMAT_COMMAND(
        buf,
        buf_len,
        "connections.%s.local_addrs=%s connections.%s.remote_addrs=%s "
        "connections.%s.children.%s.local_ts=%s connections.%s.children.%s.remote_ts=%s",
        tunnel->tunnel_id,
        tunnel->local_endpoint,
        tunnel->tunnel_id,
        tunnel->remote_endpoint,
        tunnel->tunnel_id,
        tunnel->tunnel_id,
        tunnel->local_traffic_selector,
        tunnel->tunnel_id,
        tunnel->tunnel_id,
        tunnel->remote_traffic_selector
    );
    return EN_ERR_NONE;
}

en_error_code_t en_render_swanctl_list_sas_uri(
    const en_tunnel_t *tunnel,
    const char *uri,
    char *buf,
    size_t buf_len
)
{
    if (tunnel == NULL || buf == NULL || buf_len == 0 || tunnel->tunnel_id[0] == '\0' || !valid_command_token(tunnel->tunnel_id)) return EN_ERR_INVALID_ARGUMENT;
    if (uri != NULL && uri[0] != '\0' && !valid_command_token(uri)) return EN_ERR_INVALID_ARGUMENT;
    if (uri == NULL || uri[0] == '\0') {
        FORMAT_COMMAND(buf, buf_len, "swanctl --list-sas --child %s", tunnel->tunnel_id);
    } else {
        FORMAT_COMMAND(buf, buf_len, "swanctl --uri %s --list-sas --child %s", uri, tunnel->tunnel_id);
    }
    return EN_ERR_NONE;
}

en_error_code_t en_render_swanctl_load_conns_uri(
    const char *uri,
    const char *filename,
    char *buf,
    size_t buf_len
)
{
    if (filename == NULL || filename[0] == '\0' || buf == NULL || buf_len == 0 || !valid_command_token(filename)) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (uri != NULL && uri[0] != '\0' && !valid_command_token(uri)) return EN_ERR_INVALID_ARGUMENT;
    if (uri == NULL || uri[0] == '\0') FORMAT_COMMAND(buf, buf_len, "swanctl --load-conns --file %s", filename);
    else FORMAT_COMMAND(buf, buf_len, "swanctl --uri %s --load-conns --file %s", uri, filename);
    return EN_ERR_NONE;
}

en_error_code_t en_render_vpp_route_replace(
    const en_path_t *path,
    const en_tunnel_t *egress_tunnel,
    char *buf,
    size_t buf_len
)
{
    if (path == NULL || egress_tunnel == NULL || buf == NULL || buf_len == 0) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    const char *next_hop = path->route_next_hop[0] == '\0' ? egress_tunnel->remote_endpoint : path->route_next_hop;
    if (path->route_destination_prefix[0] == '\0' || next_hop[0] == '\0' ||
        !valid_command_token(path->route_destination_prefix) || !valid_command_token(next_hop)) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    FORMAT_COMMAND(
        buf,
        buf_len,
        "vppctl ip route add %s via %s",
        path->route_destination_prefix,
        next_hop
    );
    return EN_ERR_NONE;
}

en_error_code_t en_render_vpp_route_replace_entry(
    const en_route_t *route,
    char *buf,
    size_t buf_len
)
{
    if (route == NULL || buf == NULL || buf_len == 0) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (route->destination_prefix[0] == '\0' || route->next_hop[0] == '\0' ||
        !valid_command_token(route->destination_prefix) || !valid_command_token(route->next_hop) ||
        (route->interface_name[0] != '\0' && !valid_command_token(route->interface_name))) {
        return EN_ERR_INVALID_ARGUMENT;
    }

    char suffix[128] = {0};
    if (route->table_id >= 0 && route->metric >= 0) {
        snprintf(suffix, sizeof(suffix), " table %d preference %d", route->table_id, route->metric);
    } else if (route->table_id >= 0) {
        snprintf(suffix, sizeof(suffix), " table %d", route->table_id);
    } else if (route->metric >= 0) {
        snprintf(suffix, sizeof(suffix), " preference %d", route->metric);
    }

    if (route->interface_name[0] != '\0') {
        FORMAT_COMMAND(
            buf,
            buf_len,
            "vppctl ip route add %s%s via %s %s",
            route->destination_prefix,
            suffix,
            route->next_hop,
            route->interface_name
        );
    } else {
        FORMAT_COMMAND(
            buf,
            buf_len,
            "vppctl ip route add %s%s via %s",
            route->destination_prefix,
            suffix,
            route->next_hop
        );
    }
    return EN_ERR_NONE;
}

en_error_code_t en_render_vpp_route_delete(
    const en_path_t *path,
    char *buf,
    size_t buf_len
)
{
    if (path == NULL || buf == NULL || buf_len == 0 || path->route_destination_prefix[0] == '\0' ||
        !valid_command_token(path->route_destination_prefix)) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s", path->route_destination_prefix);
    return EN_ERR_NONE;
}

en_error_code_t en_render_vpp_route_delete_with_tunnel(
    const en_path_t *path,
    const en_tunnel_t *egress_tunnel,
    char *buf,
    size_t buf_len
)
{
    if (path == NULL || egress_tunnel == NULL || buf == NULL || buf_len == 0 ||
        path->route_destination_prefix[0] == '\0' || egress_tunnel->remote_endpoint[0] == '\0' ||
        !valid_command_token(path->route_destination_prefix) || !valid_command_token(egress_tunnel->remote_endpoint)) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s via %s", path->route_destination_prefix, egress_tunnel->remote_endpoint);
    return EN_ERR_NONE;
}

en_error_code_t en_render_vpp_route_delete_entry(
    const en_route_t *route,
    char *buf,
    size_t buf_len
)
{
    if (route == NULL || buf == NULL || buf_len == 0 || route->destination_prefix[0] == '\0' ||
        !valid_command_token(route->destination_prefix) ||
        (route->next_hop[0] != '\0' && !valid_command_token(route->next_hop)) ||
        (route->interface_name[0] != '\0' && !valid_command_token(route->interface_name))) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    const char *interface_name = route->interface_name;
    if (route->next_hop[0] != '\0') {
        if (route->table_id >= 0 && interface_name[0] != '\0') FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s table %d via %s %s", route->destination_prefix, route->table_id, route->next_hop, interface_name);
        else if (route->table_id >= 0) FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s table %d via %s", route->destination_prefix, route->table_id, route->next_hop);
        else if (interface_name[0] != '\0') FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s via %s %s", route->destination_prefix, route->next_hop, interface_name);
        else FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s via %s", route->destination_prefix, route->next_hop);
    } else if (route->table_id >= 0 && interface_name[0] != '\0') FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s %s table %d", route->destination_prefix, interface_name, route->table_id);
    else if (route->table_id >= 0) FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s table %d", route->destination_prefix, route->table_id);
    else if (interface_name[0] != '\0') FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s %s", route->destination_prefix, interface_name);
    else FORMAT_COMMAND(buf, buf_len, "vppctl ip route del %s", route->destination_prefix);
    return EN_ERR_NONE;
}
