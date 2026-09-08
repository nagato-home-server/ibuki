#include "eventnet/vpp_api_adapter.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

static bool valid_observation_token(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == ':' || *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-')) return false;
    }
    return true;
}

static en_error_code_t install_path(void *ctx, const char *traffic_key, const en_path_t *path)
{
    en_vpp_api_ctx_t *api = ctx;
    if (api == NULL || api->install_path == NULL) return EN_ERR_INVALID_ARGUMENT;
    return api->install_path(api->ctx, traffic_key, path);
}

static en_error_code_t remove_path(void *ctx, const char *traffic_key, const en_path_t *path)
{
    en_vpp_api_ctx_t *api = ctx;
    if (api == NULL || api->remove_path == NULL) return EN_ERR_INVALID_ARGUMENT;
    return api->remove_path(api->ctx, traffic_key, path);
}

static const char *active_path(void *ctx, const char *traffic_key)
{
    en_vpp_api_ctx_t *api = ctx;
    if (api == NULL || api->active_path == NULL) return NULL;
    return api->active_path(api->ctx, traffic_key);
}

static en_error_code_t message_install_path(void *ctx, const char *traffic_key, const en_path_t *path)
{
    en_vpp_api_message_ctx_t *message = ctx;
    if (message == NULL || traffic_key == NULL || traffic_key[0] == '\0' || path == NULL) return EN_ERR_INVALID_ARGUMENT;
    return en_vpp_api_apply_path_operations(&message->operations, path, &message->traffic, message->edges, message->edge_count, false);
}

static en_error_code_t message_remove_path(void *ctx, const char *traffic_key, const en_path_t *path)
{
    en_vpp_api_message_ctx_t *message = ctx;
    if (message == NULL || traffic_key == NULL || traffic_key[0] == '\0' || path == NULL) return EN_ERR_INVALID_ARGUMENT;
    return en_vpp_api_apply_path_operations(&message->operations, path, &message->traffic, message->edges, message->edge_count, true);
}

en_vpp_adapter_t en_vpp_api_message_adapter(en_vpp_api_message_ctx_t *ctx)
{
    en_vpp_adapter_t adapter = {
        .install_path = message_install_path,
        .remove_path = message_remove_path,
        .active_path = NULL,
        .ctx = ctx,
    };
    return adapter;
}

static bool path_contains_node(const en_path_t *path, const char *node_id)
{
    if (path == NULL || node_id == NULL) return false;
    if (strcmp(path->source, node_id) == 0 || strcmp(path->destination, node_id) == 0) return true;
    for (size_t index = 0; index < path->waypoint_count; index++) {
        if (strcmp(path->waypoints[index], node_id) == 0) return true;
    }
    for (size_t index = 0; index < path->segment_count; index++) {
        if (strcmp(path->segments[index].from_node, node_id) == 0 || strcmp(path->segments[index].to_node, node_id) == 0) return true;
    }
    return false;
}

static bool edge_allows_vlan(const en_vpp_edge_t *edge, int vlan_id)
{
    if (edge == NULL || edge->allowed_vlan_count == 0) return true;
    for (size_t index = 0; index < edge->allowed_vlan_count; index++) {
        if (edge->allowed_vlans[index] == vlan_id) return true;
    }
    return false;
}

static int table_for_node(const en_path_t *path, const char *node_id)
{
    int table_id = -1;
    bool found = false;
    if (path == NULL || node_id == NULL) return -2;
    for (size_t index = 0; index < path->route_count; index++) {
        const en_route_t *route = &path->routes[index];
        if (strcmp(route->node_id, node_id) != 0 || route->table_id < 0) continue;
        if (found && table_id != route->table_id) return -2;
        table_id = route->table_id;
        found = true;
    }
    return table_id;
}

static bool edge_matches_route(const en_vpp_edge_t *edge, const en_route_t *route)
{
    if (edge == NULL || route == NULL || strcmp(edge->node_id, route->node_id) != 0) return false;
    return route->interface_name[0] == '\0' ||
        strcmp(route->interface_name, edge->vpp_interface) == 0 ||
        strcmp(route->interface_name, edge->host_interface) == 0;
}

static bool valid_route_operation(const en_route_t *route)
{
    return route != NULL && route->destination_prefix[0] != '\0' && route->next_hop[0] != '\0' &&
        (route->route_id[0] == '\0' || valid_observation_token(route->route_id)) &&
        (route->node_id[0] == '\0' || valid_observation_token(route->node_id)) &&
        valid_observation_token(route->destination_prefix) && valid_observation_token(route->next_hop) &&
        (route->interface_name[0] == '\0' || valid_observation_token(route->interface_name)) && route->table_id >= -1;
}

static void rollback_install(
    const en_vpp_api_message_ops_t *ops,
    const en_route_t *routes,
    size_t route_count,
    const en_vpp_edge_t *const *edges,
    size_t edge_count,
    int vlan_id)
{
    if (ops == NULL) return;
    for (size_t index = route_count; index > 0; index--) {
        (void)ops->remove_route(ops->ctx, &routes[index - 1]);
    }
    if (vlan_id >= 0 && ops->remove_vlan_subinterface != NULL) {
        for (size_t index = edge_count; index > 0; index--) {
        (void)ops->remove_vlan_subinterface(ops->ctx, edges[index - 1], vlan_id);
        }
    }
}

en_error_code_t en_vpp_api_apply_path_operations(
    const en_vpp_api_message_ops_t *ops,
    const en_path_t *path,
    const en_traffic_selector_t *traffic,
    const en_vpp_edge_t *edges,
    size_t edge_count,
    bool remove)
{
    if (ops == NULL || path == NULL || traffic == NULL || (edge_count > 0 && edges == NULL) || edge_count > EN_MAX_VPP_EDGES ||
        ops->remove_route == NULL || (remove ? false : ops->add_route == NULL)) return EN_ERR_INVALID_ARGUMENT;
    if (path->route_count > EN_MAX_ROUTES ||
        (traffic->has_vlan_id && (traffic->vlan_id < 0 || traffic->vlan_id > 4095))) return EN_ERR_INVALID_ARGUMENT;
    if (!remove) {
        for (size_t index = 0; index < path->route_count; index++) {
            if (path->routes[index].table_id > 0 && ops->ensure_vrf == NULL) return EN_ERR_INVALID_ARGUMENT;
        }
    }
    if (traffic->has_vlan_id && (remove ? ops->remove_vlan_subinterface == NULL :
        (ops->create_vlan_subinterface == NULL || ops->remove_vlan_subinterface == NULL))) return EN_ERR_INVALID_ARGUMENT;
    if (traffic->has_vlan_id && !remove) {
        for (size_t index = 0; index < edge_count; index++) {
            if (!path_contains_node(path, edges[index].node_id)) continue;
            int table_id = table_for_node(path, edges[index].node_id);
            if (table_id < -1 || (table_id > 0 && ops->set_vlan_interface_table == NULL)) return EN_ERR_INVALID_ARGUMENT;
        }
    }
    if (path->route_count == 0) {
        en_route_t implicit_route = {0};
        snprintf(implicit_route.route_id, sizeof(implicit_route.route_id), "%s", path->path_id);
        snprintf(implicit_route.node_id, sizeof(implicit_route.node_id), "%s", path->source);
        snprintf(implicit_route.destination_prefix, sizeof(implicit_route.destination_prefix), "%s", path->route_destination_prefix);
        snprintf(implicit_route.next_hop, sizeof(implicit_route.next_hop), "%s", path->route_next_hop);
        implicit_route.table_id = -1;
        if (!valid_route_operation(&implicit_route)) return EN_ERR_INVALID_ARGUMENT;
    } else {
        for (size_t index = 0; index < path->route_count; index++) {
            if (!valid_route_operation(&path->routes[index])) return EN_ERR_INVALID_ARGUMENT;
            if (path->routes[index].interface_name[0] != '\0') {
                bool matching_edge = false;
                for (size_t edge_index = 0; edge_index < edge_count; edge_index++) {
                    if (edge_matches_route(&edges[edge_index], &path->routes[index])) {
                        matching_edge = true;
                        break;
                    }
                }
                if (!matching_edge) return EN_ERR_NOT_FOUND;
            }
        }
    }
    en_route_t applied_routes[EN_MAX_ROUTES + 1] = {0};
    size_t applied_route_count = 0;
    const en_vpp_edge_t *created_edges[EN_MAX_VPP_EDGES] = {0};
    size_t created_edge_count = 0;
    for (size_t index = 0; index < edge_count; index++) {
        if (!path_contains_node(path, edges[index].node_id)) continue;
        if (traffic->has_vlan_id && !edge_allows_vlan(&edges[index], traffic->vlan_id)) return EN_ERR_INVALID_ARGUMENT;
        if (!remove && traffic->has_vlan_id) {
            en_error_code_t status = ops->create_vlan_subinterface(ops->ctx, &edges[index], traffic->vlan_id);
            if (status != EN_ERR_NONE) {
                for (size_t rollback_index = created_edge_count; rollback_index > 0; rollback_index--) {
                    (void)ops->remove_vlan_subinterface(ops->ctx, created_edges[rollback_index - 1], traffic->vlan_id);
                }
                return status;
            }
            created_edges[created_edge_count++] = &edges[index];
        }
    }
    if (path->route_count > 0) {
        for (size_t index = 0; index < path->route_count; index++) {
            const en_route_t *route = &path->routes[index];
            if (!remove && route->table_id > 0 && ops->ensure_vrf != NULL) {
                bool table_already_prepared = false;
                for (size_t previous_index = 0; previous_index < index; previous_index++) {
                    if (path->routes[previous_index].table_id == route->table_id) {
                        table_already_prepared = true;
                        break;
                    }
                }
                if (!table_already_prepared) {
                    en_error_code_t status = ops->ensure_vrf(ops->ctx, route->table_id);
                    if (status != EN_ERR_NONE) {
                        rollback_install(ops, applied_routes, applied_route_count, created_edges, created_edge_count, traffic->has_vlan_id ? traffic->vlan_id : -1);
                        return status;
                    }
                }
            }
            if (!remove && traffic->has_vlan_id && route->table_id > 0 && ops->set_vlan_interface_table != NULL) {
                bool table_already_set = false;
                for (size_t previous_index = 0; previous_index < index; previous_index++) {
                    const en_route_t *previous_route = &path->routes[previous_index];
                    bool same_target = strcmp(previous_route->node_id, route->node_id) == 0 &&
                        (route->interface_name[0] == '\0' ||
                         (previous_route->interface_name[0] != '\0' &&
                          strcmp(previous_route->interface_name, route->interface_name) == 0));
                    if (same_target && previous_route->table_id >= 0) {
                        table_already_set = true;
                        break;
                    }
                }
                if (!table_already_set) {
                    for (size_t edge_index = 0; edge_index < edge_count; edge_index++) {
                        if (edge_matches_route(&edges[edge_index], route)) {
                            en_error_code_t table_status = ops->set_vlan_interface_table(ops->ctx, &edges[edge_index], traffic->vlan_id, route->table_id);
                            if (table_status != EN_ERR_NONE) {
                                rollback_install(ops, applied_routes, applied_route_count, created_edges, created_edge_count, traffic->has_vlan_id ? traffic->vlan_id : -1);
                                return table_status;
                            }
                            break;
                        }
                    }
                }
            }
            en_error_code_t status = remove ? ops->remove_route(ops->ctx, route) : ops->add_route(ops->ctx, route);
            if (status != EN_ERR_NONE) {
                if (!remove) rollback_install(ops, applied_routes, applied_route_count, created_edges, created_edge_count, traffic->has_vlan_id ? traffic->vlan_id : -1);
                return status;
            }
            if (!remove) applied_routes[applied_route_count++] = *route;
        }
    } else {
        en_route_t route = {0};
        snprintf(route.route_id, sizeof(route.route_id), "%s", path->path_id);
        snprintf(route.node_id, sizeof(route.node_id), "%s", path->source);
        snprintf(route.destination_prefix, sizeof(route.destination_prefix), "%s", path->route_destination_prefix);
        snprintf(route.next_hop, sizeof(route.next_hop), "%s", path->route_next_hop);
        route.table_id = -1;
        en_error_code_t status = remove ? ops->remove_route(ops->ctx, &route) : ops->add_route(ops->ctx, &route);
        if (status != EN_ERR_NONE) {
            if (!remove) rollback_install(ops, applied_routes, applied_route_count, created_edges, created_edge_count, traffic->has_vlan_id ? traffic->vlan_id : -1);
            return status;
        }
        if (!remove) applied_routes[applied_route_count++] = route;
    }
    if (remove && traffic->has_vlan_id) {
        for (size_t index = 0; index < edge_count; index++) {
            if (!path_contains_node(path, edges[index].node_id)) continue;
            en_error_code_t status = ops->remove_vlan_subinterface(ops->ctx, &edges[index], traffic->vlan_id);
            if (status != EN_ERR_NONE) return status;
        }
    }
    return EN_ERR_NONE;
}

en_error_code_t en_vpp_api_observe_route(en_vpp_api_ctx_t *ctx, const char *destination_prefix, en_vpp_route_observation_t *observation)
{
    if (ctx == NULL || ctx->observe_route == NULL) return EN_ERR_INVALID_ARGUMENT;
    if (destination_prefix == NULL || destination_prefix[0] == '\0' || observation == NULL) return EN_ERR_INVALID_ARGUMENT;
    en_error_code_t err = ctx->observe_route(ctx->ctx, destination_prefix, observation);
    if (err != EN_ERR_NONE) return err;
    if (observation->destination_prefix[0] == '\0' || strcmp(observation->destination_prefix, destination_prefix) != 0 ||
        !valid_observation_token(observation->destination_prefix) || observation->table_id < -1 ||
        (observation->present && (!valid_observation_token(observation->next_hop) ||
            (observation->interface_name[0] != '\0' && !valid_observation_token(observation->interface_name))))) {
        return EN_ERR_STATE_CONFLICT;
    }
    return EN_ERR_NONE;
}

en_error_code_t en_vpp_api_observe_interface(en_vpp_api_ctx_t *ctx, const char *interface_name, en_vpp_interface_observation_t *observation)
{
    if (ctx == NULL || ctx->observe_interface == NULL || interface_name == NULL || interface_name[0] == '\0' ||
        !valid_observation_token(interface_name) || observation == NULL) return EN_ERR_INVALID_ARGUMENT;
    en_error_code_t err = ctx->observe_interface(ctx->ctx, interface_name, observation);
    if (err != EN_ERR_NONE) return err;
    if (observation->interface_name[0] == '\0' || strcmp(observation->interface_name, interface_name) != 0 ||
        !valid_observation_token(observation->interface_name)) return EN_ERR_STATE_CONFLICT;
    return EN_ERR_NONE;
}

en_error_code_t en_vpp_api_observe_event_json(en_vpp_api_ctx_t *ctx, const char *destination_prefix, const char *path_id, const char *route_id, long long timestamp_ms, char *output, size_t output_len)
{
    en_vpp_route_observation_t observation = {0};
    en_error_code_t err = en_vpp_api_observe_route(ctx, destination_prefix, &observation);
    if (err != EN_ERR_NONE) return err;
    return en_vpp_route_observation_to_event_json(&observation, path_id, route_id, timestamp_ms, output, output_len);
}

en_vpp_adapter_t en_vpp_api_adapter(en_vpp_api_ctx_t *ctx)
{
    en_vpp_adapter_t adapter = {
        .install_path = install_path,
        .remove_path = remove_path,
        .active_path = active_path,
        .ctx = ctx,
    };
    return adapter;
}
