#include "eventnet/yaml_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    YAML_SECTION_NONE = 0,
    YAML_SECTION_NODES,
    YAML_SECTION_TUNNELS,
    YAML_SECTION_PATHS,
    YAML_SECTION_INTENTS,
    YAML_SECTION_VPP_EDGES
} yaml_top_section_t;

typedef enum {
    YAML_CONTEXT_NONE = 0,
    YAML_CONTEXT_PATH_WAYPOINTS,
    YAML_CONTEXT_PATH_SEGMENTS,
    YAML_CONTEXT_PATH_ROUTES,
    YAML_CONTEXT_INTENT_TRAFFIC,
    YAML_CONTEXT_INTENT_SELECTION,
    YAML_CONTEXT_INTENT_CANDIDATES,
    YAML_CONTEXT_INTENT_CONSTRAINTS,
    YAML_CONTEXT_INTENT_FORBIDDEN_WAYPOINTS,
    YAML_CONTEXT_INTENT_REQUIRED_WAYPOINTS,
    YAML_CONTEXT_INTENT_REQUIRED_CAPABILITIES,
    YAML_CONTEXT_INTENT_COMPARISON_ORDER,
    YAML_CONTEXT_INTENT_TRANSITION,
    YAML_CONTEXT_INTENT_FALLBACK,
    YAML_CONTEXT_VPP_EDGE_ALLOWED_VLANS,
    YAML_CONTEXT_NODE_ENDPOINTS,
    YAML_CONTEXT_NODE_CAPABILITIES
} yaml_context_t;

typedef struct {
    yaml_top_section_t top;
    yaml_context_t context;
    en_path_t *path;
    en_node_t *node;
    en_segment_t *segment;
    en_route_t *route;
    en_tunnel_t *tunnel;
    en_intent_t *intent;
    en_vpp_edge_t *vpp_edge;
} yaml_parse_state_t;

static void set_error(char *error, size_t error_len, size_t line_no, const char *message);
static char *trim(char *text);
static void strip_comment(char *text);
static int indent_of(const char *line);
static bool split_key_value(char *text, char **key, char **value);
static void clean_scalar(char *text);
static en_error_code_t parse_line(en_yaml_config_t *config, yaml_parse_state_t *state, char *line, size_t line_no, char *error, size_t error_len);
static en_error_code_t parse_tunnel_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no);
static en_error_code_t parse_node_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no);
static en_error_code_t parse_path_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no);
static en_error_code_t parse_segment_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no);
static en_error_code_t parse_route_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no);
static en_error_code_t parse_intent_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no);
static en_error_code_t parse_vpp_edge_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no);
static en_error_code_t parse_selection_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no);
static en_error_code_t parse_constraints_kv(yaml_parse_state_t *state, const char *key, const char *value);
static en_error_code_t parse_transition_kv(yaml_parse_state_t *state, const char *key, const char *value);
static en_error_code_t parse_fallback_kv(yaml_parse_state_t *state, const char *key, const char *value);
static en_path_selection_mode_t parse_selection_mode(const char *value, bool *ok);
static en_transition_strategy_t parse_transition_strategy(const char *value, bool *ok);
static en_comparison_key_t parse_comparison_key(const char *value, bool *ok);
static bool parse_bool(const char *value);
static void copy_id(char *dst, size_t dst_len, const char *src);
static void copy_address_without_cidr(char *dst, size_t dst_len, const char *src);
static void yaml_normalize(en_yaml_config_t *config);
static bool yaml_has_path(const en_yaml_config_t *config, const char *path_id);
static bool yaml_has_tunnel(const en_yaml_config_t *config, const char *tunnel_id);
static bool yaml_has_node(const en_yaml_config_t *config, const char *node_id);
static bool yaml_has_vpp_edge(const en_yaml_config_t *config, const char *node_id);
static bool yaml_has_vpp_edge_interface(const en_yaml_config_t *config, const en_route_t *route);
static const en_tunnel_t *yaml_find_tunnel(const en_yaml_config_t *config, const char *tunnel_id);

static bool valid_config_token(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-' || *cursor == ':')) return false;
    }
    return true;
}

static bool valid_secret(const char *value)
{
    if (value == NULL || value[0] == '\0') return true;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (*cursor < 33 || *cursor > 126 || *cursor == '"' || *cursor == '\\') return false;
    }
    return true;
}

static int parse_integer(const char *value)
{
    char *end = NULL;
    long parsed;
    if (value == NULL || value[0] == '\0') return INT_MIN;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' || parsed < INT_MIN + 1L || parsed > INT_MAX) return INT_MIN;
    return (int)parsed;
}

static double parse_number(const char *value)
{
    char *end = NULL;
    double parsed;
    if (value == NULL || value[0] == '\0') return NAN;
    errno = 0;
    parsed = strtod(value, &end);
    if (errno == ERANGE || end == value || *end != '\0' || !isfinite(parsed)) return NAN;
    return parsed;
}

en_error_code_t en_yaml_config_load_file(const char *filename, en_yaml_config_t *config, char *error, size_t error_len)
{
    if (filename == NULL || config == NULL) {
        set_error(error, error_len, 0, "invalid argument");
        return EN_ERR_INVALID_ARGUMENT;
    }

    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        set_error(error, error_len, 0, "failed to open yaml file");
        return EN_ERR_NOT_FOUND;
    }

    memset(config, 0, sizeof(*config));
    yaml_parse_state_t state = {0};
    char line[512];
    size_t line_no = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        line_no++;
        en_error_code_t err = parse_line(config, &state, line, line_no, error, error_len);
        if (err != EN_ERR_NONE) {
            fclose(file);
            return err;
        }
    }

    fclose(file);
    yaml_normalize(config);
    return en_yaml_config_validate(config, error, error_len);
}

en_error_code_t en_yaml_config_validate(const en_yaml_config_t *config, char *error, size_t error_len)
{
    if (config == NULL) {
        set_error(error, error_len, 0, "config is null");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (config->path_count == 0) {
        set_error(error, error_len, 0, "at least one path is required");
        return EN_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < config->node_count; i++) {
        const en_node_t *node = &config->nodes[i];
        if (node->node_id[0] == '\0' || !valid_config_token(node->node_id)) {
            set_error(error, error_len, 0, "node requires a valid id");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (node->role[0] != '\0' && !valid_config_token(node->role)) {
            set_error(error, error_len, 0, "node role contains invalid characters");
            return EN_ERR_INVALID_ARGUMENT;
        }
        for (size_t j = 0; j < node->endpoint_count; j++) {
            if (!valid_config_token(node->endpoints[j])) {
                set_error(error, error_len, 0, "node endpoint contains invalid characters");
                return EN_ERR_INVALID_ARGUMENT;
            }
        }
        for (size_t j = 0; j < node->capability_count; j++) {
            if (!valid_config_token(node->capabilities[j])) {
                set_error(error, error_len, 0, "node capability contains invalid characters");
                return EN_ERR_INVALID_ARGUMENT;
            }
            for (size_t k = j + 1; k < node->capability_count; k++) {
                if (strcmp(node->capabilities[j], node->capabilities[k]) == 0) {
                    set_error(error, error_len, 0, "duplicate node capability");
                    return EN_ERR_INVALID_ARGUMENT;
                }
            }
        }
        for (size_t j = i + 1; j < config->node_count; j++) {
            if (strcmp(node->node_id, config->nodes[j].node_id) == 0) {
                set_error(error, error_len, 0, "duplicate node id");
                return EN_ERR_INVALID_ARGUMENT;
            }
        }
    }
    for (size_t i = 0; i < config->tunnel_count; i++) {
        const en_tunnel_t *tunnel = &config->tunnels[i];
        if (tunnel->tunnel_id[0] == '\0' || tunnel->local_node[0] == '\0' || tunnel->remote_node[0] == '\0') {
            set_error(error, error_len, 0, "tunnel requires id, local_node, and remote_node");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (!valid_config_token(tunnel->tunnel_id) || !valid_config_token(tunnel->local_node) || !valid_config_token(tunnel->remote_node)) {
            set_error(error, error_len, 0, "tunnel identifier contains invalid characters");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (tunnel->tunnel_type[0] != '\0' && strcmp(tunnel->tunnel_type, "ipsec") != 0 && strcmp(tunnel->tunnel_type, "gre_over_ipsec") != 0) {
            set_error(error, error_len, 0, "tunnel type must be ipsec or gre_over_ipsec");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(tunnel->tunnel_type, "gre_over_ipsec") == 0) {
            if (tunnel->gre_interface[0] == '\0' || tunnel->gre_local_address[0] == '\0' || tunnel->gre_remote_address[0] == '\0') {
                set_error(error, error_len, 0, "gre_over_ipsec tunnel requires gre_interface, gre_local_address, and gre_remote_address");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if (!valid_config_token(tunnel->gre_interface) || !valid_config_token(tunnel->gre_local_address) || !valid_config_token(tunnel->gre_remote_address) ||
                (tunnel->gre_instance < -1 || tunnel->gre_instance > 1048575 || tunnel->gre_mtu < 0 || tunnel->gre_mtu > 65535)) {
                set_error(error, error_len, 0, "gre_over_ipsec tunnel has invalid GRE settings");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if ((tunnel->gre_outer_local_endpoint[0] != '\0' && !valid_config_token(tunnel->gre_outer_local_endpoint)) ||
                (tunnel->gre_outer_remote_endpoint[0] != '\0' && !valid_config_token(tunnel->gre_outer_remote_endpoint))) {
                set_error(error, error_len, 0, "gre_over_ipsec tunnel has invalid outer endpoint");
                return EN_ERR_INVALID_ARGUMENT;
            }
        }
        if (config->node_count > 0 && (!yaml_has_node(config, tunnel->local_node) || !yaml_has_node(config, tunnel->remote_node))) {
            set_error(error, error_len, 0, "tunnel references unknown node");
            return EN_ERR_NOT_FOUND;
        }
        if (tunnel->auth_method[0] != '\0' && strcmp(tunnel->auth_method, "psk") != 0 && strcmp(tunnel->auth_method, "pubkey") != 0) {
            set_error(error, error_len, 0, "tunnel auth_method must be psk or pubkey");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(tunnel->auth_method, "pubkey") == 0 && tunnel->local_cert[0] == '\0') {
            set_error(error, error_len, 0, "pubkey tunnel requires local_cert");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (tunnel->local_cert[0] != '\0' && !valid_config_token(tunnel->local_cert)) {
            set_error(error, error_len, 0, "tunnel local_cert contains invalid characters");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (tunnel->remote_cacerts[0] != '\0' && !valid_config_token(tunnel->remote_cacerts)) {
            set_error(error, error_len, 0, "tunnel remote_cacerts contains invalid characters");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (!valid_secret(tunnel->psk)) {
            set_error(error, error_len, 0, "tunnel psk contains invalid characters");
            return EN_ERR_INVALID_ARGUMENT;
        }
        for (size_t j = i + 1; j < config->tunnel_count; j++) {
            if (strcmp(tunnel->tunnel_id, config->tunnels[j].tunnel_id) == 0) {
                set_error(error, error_len, 0, "duplicate tunnel id");
                return EN_ERR_INVALID_ARGUMENT;
            }
        }
    }
    for (size_t i = 0; i < config->path_count; i++) {
        const en_path_t *path = &config->paths[i];
        if (path->path_id[0] == '\0' || path->source[0] == '\0' || path->destination[0] == '\0') {
            set_error(error, error_len, 0, "path requires id, source, and destination");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (!valid_config_token(path->path_id) || !valid_config_token(path->source) || !valid_config_token(path->destination) || path->priority == INT_MIN) {
            set_error(error, error_len, 0, "path identifier or priority is invalid");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (config->node_count > 0 && (!yaml_has_node(config, path->source) || !yaml_has_node(config, path->destination))) {
            set_error(error, error_len, 0, "path references unknown node");
            return EN_ERR_NOT_FOUND;
        }
        for (size_t j = i + 1; j < config->path_count; j++) {
            if (strcmp(path->path_id, config->paths[j].path_id) == 0) {
                set_error(error, error_len, 0, "duplicate path id");
                return EN_ERR_INVALID_ARGUMENT;
            }
        }
        if (path->segment_count == 0 &&
            ((!path->routes_explicit &&
              (path->route_destination_prefix[0] == '\0' || path->route_next_hop[0] == '\0')) ||
             (path->routes_explicit && path->route_count == 0))) {
            set_error(error, error_len, 0, "path without segments requires a legacy route or explicit routes");
            return EN_ERR_INVALID_ARGUMENT;
        }
        for (size_t j = 0; j < path->segment_count; j++) {
            if ((path->segments[j].segment_id[0] != '\0' && !valid_config_token(path->segments[j].segment_id)) ||
                (path->segments[j].from_node[0] != '\0' && !valid_config_token(path->segments[j].from_node)) ||
                (path->segments[j].to_node[0] != '\0' && !valid_config_token(path->segments[j].to_node)) ||
                !valid_config_token(path->segments[j].tunnel_id)) {
                set_error(error, error_len, 0, "path segment identifier contains invalid characters");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if (!yaml_has_tunnel(config, path->segments[j].tunnel_id)) {
                set_error(error, error_len, 0, "path segment references unknown tunnel_id");
                return EN_ERR_NOT_FOUND;
            }
            if (config->node_count > 0 &&
                ((path->segments[j].from_node[0] != '\0' && !yaml_has_node(config, path->segments[j].from_node)) ||
                 (path->segments[j].to_node[0] != '\0' && !yaml_has_node(config, path->segments[j].to_node)))) {
                set_error(error, error_len, 0, "path segment references unknown node");
                return EN_ERR_NOT_FOUND;
            }
        }
        for (size_t j = 0; j < path->route_count; j++) {
            const en_route_t *route = &path->routes[j];
            if (path->routes_explicit && route->node_id[0] == '\0') {
                set_error(error, error_len, 0, "explicit path route requires node_id");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if (route->destination_prefix[0] == '\0') {
                set_error(error, error_len, 0, "path route requires destination_prefix");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if ((route->route_id[0] != '\0' && !valid_config_token(route->route_id)) ||
                !valid_config_token(route->node_id) || !valid_config_token(route->destination_prefix) ||
                !valid_config_token(route->next_hop) ||
                (route->interface_name[0] != '\0' && !valid_config_token(route->interface_name))) {
                set_error(error, error_len, 0, "path route contains invalid characters");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if (route->next_hop[0] == '\0') {
                set_error(error, error_len, 0, "path route requires next_hop");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if (config->node_count > 0 && !yaml_has_node(config, route->node_id)) {
                set_error(error, error_len, 0, "path route references unknown node");
                return EN_ERR_NOT_FOUND;
            }
            if (route->table_id == INT_MIN || route->metric == INT_MIN || route->table_id < -1 || route->metric < -1) {
                set_error(error, error_len, 0, "path route table and metric must be non-negative");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if (route->route_id[0] != '\0') {
                for (size_t k = j + 1; k < path->route_count; k++) {
                    if (strcmp(route->route_id, path->routes[k].route_id) == 0) {
                        set_error(error, error_len, 0, "duplicate route id within path");
                        return EN_ERR_INVALID_ARGUMENT;
                    }
                }
            }
            if (config->vpp_edge_count > 0 && !yaml_has_vpp_edge(config, route->node_id)) {
                set_error(error, error_len, 0, "path route node_id has no vpp_edge");
                return EN_ERR_NOT_FOUND;
            }
            if (config->vpp_edge_count > 0 && route->interface_name[0] != '\0' &&
                !yaml_has_vpp_edge_interface(config, route)) {
                set_error(error, error_len, 0, "path route interface has no matching vpp_edge");
                return EN_ERR_NOT_FOUND;
            }
        }
        for (size_t j = 0; j < path->waypoint_count; j++) {
            if (config->node_count > 0 && !yaml_has_node(config, path->waypoints[j])) {
                set_error(error, error_len, 0, "path waypoint references unknown node");
                return EN_ERR_NOT_FOUND;
            }
            for (size_t k = j + 1; k < path->waypoint_count; k++) {
                if (strcmp(path->waypoints[j], path->waypoints[k]) == 0) {
                    set_error(error, error_len, 0, "duplicate waypoint within path");
                    return EN_ERR_INVALID_ARGUMENT;
                }
            }
        }
        if (path->egress_tunnel_id[0] != '\0' && !yaml_has_tunnel(config, path->egress_tunnel_id)) {
            set_error(error, error_len, 0, "path egress_tunnel_id references unknown tunnel");
            return EN_ERR_NOT_FOUND;
        }
    }
    for (size_t i = 0; i < config->intent_count; i++) {
        const en_intent_t *intent = &config->intents[i];
        if (intent->intent_id[0] == '\0') {
            set_error(error, error_len, 0, "intent requires id");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (!valid_config_token(intent->intent_id)) {
            set_error(error, error_len, 0, "intent identifier contains invalid characters");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (intent->traffic.has_vlan_id && (intent->traffic.vlan_id == INT_MIN || intent->traffic.vlan_id < 1 || intent->traffic.vlan_id > 4094)) {
            set_error(error, error_len, 0, "vlan_id must be between 1 and 4094");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (intent->deny_unmatched_vlan && !intent->traffic.has_vlan_id) {
            set_error(error, error_len, 0, "deny_unmatched_vlan requires vlan_id");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (config->node_count > 0 && !yaml_has_node(config, intent->traffic.source)) {
            set_error(error, error_len, 0, "intent source references unknown node");
            return EN_ERR_NOT_FOUND;
        }
        if (config->node_count > 0 && !yaml_has_node(config, intent->traffic.destination)) {
            set_error(error, error_len, 0, "intent destination references unknown node");
            return EN_ERR_NOT_FOUND;
        }
        if (intent->transition.max_pause_ms == INT_MIN || intent->transition.drain_timeout_ms == INT_MIN ||
            intent->transition.timeout_ms == INT_MIN || intent->transition.retry_count == INT_MIN ||
            intent->transition.retry_backoff_ms == INT_MIN || intent->transition.max_pause_ms < 0 ||
            intent->transition.drain_timeout_ms < 0 || intent->transition.timeout_ms < 0 || intent->transition.retry_count < 0 ||
            intent->transition.retry_backoff_ms < 0) {
            set_error(error, error_len, 0, "transition timing must be non-negative");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if ((intent->path_selection.constraints.has_max_rtt_ms &&
             (!isfinite(intent->path_selection.constraints.max_rtt_ms) || intent->path_selection.constraints.max_rtt_ms < 0.0)) ||
            (intent->path_selection.constraints.has_max_packet_loss_percent &&
             (!isfinite(intent->path_selection.constraints.max_packet_loss_percent) || intent->path_selection.constraints.max_packet_loss_percent < 0.0 ||
              intent->path_selection.constraints.max_packet_loss_percent > 100.0)) ||
            (intent->path_selection.constraints.has_hysteresis_percent &&
             (!isfinite(intent->path_selection.constraints.hysteresis_percent) || intent->path_selection.constraints.hysteresis_percent < 0.0 ||
              intent->path_selection.constraints.hysteresis_percent > 100.0))) {
            set_error(error, error_len, 0, "path constraints must be finite and non-negative");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (intent->path_selection.constraints.failure_threshold < 0 ||
            intent->path_selection.constraints.recovery_threshold < 0 ||
            intent->path_selection.constraints.hold_down_ms < 0) {
            set_error(error, error_len, 0, "path thresholds must be non-negative");
            return EN_ERR_INVALID_ARGUMENT;
        }
        for (size_t j = i + 1; j < config->intent_count; j++) {
            if (strcmp(intent->intent_id, config->intents[j].intent_id) == 0) {
                set_error(error, error_len, 0, "duplicate intent id");
                return EN_ERR_INVALID_ARGUMENT;
            }
        }
        if (intent->path_selection.mode == EN_SELECT_EXPLICIT && !yaml_has_path(config, intent->path_selection.path_id)) {
            set_error(error, error_len, 0, "explicit intent references unknown path_id");
            return EN_ERR_NOT_FOUND;
        }
        for (size_t j = 0; j < intent->path_selection.candidate_count; j++) {
            if (!yaml_has_path(config, intent->path_selection.candidates[j])) {
                set_error(error, error_len, 0, "intent candidate references unknown path");
                return EN_ERR_NOT_FOUND;
            }
            for (size_t k = j + 1; k < intent->path_selection.candidate_count; k++) {
                if (strcmp(intent->path_selection.candidates[j], intent->path_selection.candidates[k]) == 0) {
                    set_error(error, error_len, 0, "duplicate intent candidate path");
                    return EN_ERR_INVALID_ARGUMENT;
                }
            }
        }
        for (size_t j = 0; j < intent->path_selection.constraints.required_waypoint_count; j++) {
            if (config->node_count > 0 && !yaml_has_node(config, intent->path_selection.constraints.required_waypoints[j])) {
                set_error(error, error_len, 0, "required waypoint references unknown node");
                return EN_ERR_NOT_FOUND;
            }
            for (size_t k = 0; k < intent->path_selection.constraints.forbidden_waypoint_count; k++) {
                if (strcmp(intent->path_selection.constraints.required_waypoints[j], intent->path_selection.constraints.forbidden_waypoints[k]) == 0) {
                    set_error(error, error_len, 0, "waypoint cannot be both required and forbidden");
                    return EN_ERR_INVALID_ARGUMENT;
                }
            }
        }
        for (size_t j = 0; j < intent->path_selection.constraints.forbidden_waypoint_count; j++) {
            if (config->node_count > 0 && !yaml_has_node(config, intent->path_selection.constraints.forbidden_waypoints[j])) {
                set_error(error, error_len, 0, "forbidden waypoint references unknown node");
                return EN_ERR_NOT_FOUND;
            }
        }
        for (size_t j = 0; j < intent->path_selection.constraints.required_capability_count; j++) {
            if (!valid_config_token(intent->path_selection.constraints.required_capabilities[j])) {
                set_error(error, error_len, 0, "required capability contains invalid characters");
                return EN_ERR_INVALID_ARGUMENT;
            }
            for (size_t k = j + 1; k < intent->path_selection.constraints.required_capability_count; k++) {
                if (strcmp(intent->path_selection.constraints.required_capabilities[j], intent->path_selection.constraints.required_capabilities[k]) == 0) {
                    set_error(error, error_len, 0, "duplicate required capability");
                    return EN_ERR_INVALID_ARGUMENT;
                }
            }
        }
    }
    for (size_t i = 0; i < config->vpp_edge_count; i++) {
        const en_vpp_edge_t *edge = &config->vpp_edges[i];
        if (edge->node_id[0] == '\0' || edge->vpp_interface[0] == '\0' || edge->next_hop[0] == '\0' ||
            !valid_config_token(edge->node_id) ||
            (edge->port_id[0] != '\0' && !valid_config_token(edge->port_id))) {
            set_error(error, error_len, 0, "vpp edge requires node_id, vpp_interface, and next_hop");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (edge->vpp_socket[0] != '\0' && !valid_config_token(edge->vpp_socket)) {
            set_error(error, error_len, 0, "vpp edge has invalid vpp_socket");
            return EN_ERR_INVALID_ARGUMENT;
        }
        for (size_t j = i + 1; j < config->vpp_edge_count; j++) {
            bool same_node = strcmp(edge->node_id, config->vpp_edges[j].node_id) == 0;
            bool same_port = edge->port_id[0] == '\0' || config->vpp_edges[j].port_id[0] == '\0' ||
                strcmp(edge->port_id, config->vpp_edges[j].port_id) == 0;
            bool same_vpp_interface = strcmp(edge->vpp_interface, config->vpp_edges[j].vpp_interface) == 0;
            bool same_host_interface = edge->host_interface[0] != '\0' && config->vpp_edges[j].host_interface[0] != '\0' &&
                strcmp(edge->host_interface, config->vpp_edges[j].host_interface) == 0;
            if (same_node && (same_port || same_vpp_interface || same_host_interface)) {
                set_error(error, error_len, 0, "duplicate vpp edge node_id, port_id, or interface");
                return EN_ERR_INVALID_ARGUMENT;
            }
        }
        for (size_t j = 0; j < edge->allowed_vlan_count; j++) {
            if (edge->allowed_vlans[j] < 1 || edge->allowed_vlans[j] > 4094) {
                set_error(error, error_len, 0, "allowed VLAN must be between 1 and 4094");
                return EN_ERR_INVALID_ARGUMENT;
            }
            for (size_t k = j + 1; k < edge->allowed_vlan_count; k++) {
                if (edge->allowed_vlans[j] == edge->allowed_vlans[k]) {
                    set_error(error, error_len, 0, "duplicate allowed VLAN");
                    return EN_ERR_INVALID_ARGUMENT;
                }
            }
        }
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_line(en_yaml_config_t *config, yaml_parse_state_t *state, char *line, size_t line_no, char *error, size_t error_len)
{
    strip_comment(line);
    int indent = indent_of(line);
    char *text = trim(line);
    if (text[0] == '\0') {
        return EN_ERR_NONE;
    }

    if (indent == 0 && strcmp(text, "nodes:") == 0) {
        state->top = YAML_SECTION_NODES;
        state->context = YAML_CONTEXT_NONE;
        state->node = NULL;
        return EN_ERR_NONE;
    }
    if (indent == 0 && strcmp(text, "tunnels:") == 0) {
        state->top = YAML_SECTION_TUNNELS;
        state->context = YAML_CONTEXT_NONE;
        state->tunnel = NULL;
        return EN_ERR_NONE;
    }
    if (indent == 0 && strcmp(text, "paths:") == 0) {
        state->top = YAML_SECTION_PATHS;
        state->context = YAML_CONTEXT_NONE;
        state->path = NULL;
        return EN_ERR_NONE;
    }
    if (indent == 0 && strcmp(text, "intents:") == 0) {
        state->top = YAML_SECTION_INTENTS;
        state->context = YAML_CONTEXT_NONE;
        state->intent = NULL;
        return EN_ERR_NONE;
    }
    if (indent == 0 && strcmp(text, "vpp_edges:") == 0) {
        state->top = YAML_SECTION_VPP_EDGES;
        state->context = YAML_CONTEXT_NONE;
        state->vpp_edge = NULL;
        return EN_ERR_NONE;
    }

    if (state->top == YAML_SECTION_NONE) {
        set_error(error, error_len, line_no, "expected top-level nodes:, tunnels:, paths:, intents:, or vpp_edges:");
        return EN_ERR_INVALID_ARGUMENT;
    }

    if (strncmp(text, "- ", 2) == 0) {
        char *item = trim(text + 2);
        char *key = NULL;
        char *value = NULL;
        bool has_key = split_key_value(item, &key, &value);
        if ((has_key && strlen(value) >= EN_MAX_ID_LEN) || (!has_key && strlen(item) >= EN_MAX_ID_LEN)) {
            set_error(error, error_len, line_no, "YAML value exceeds fixed field capacity");
            return EN_ERR_INVALID_ARGUMENT;
        }

        if (state->top == YAML_SECTION_NODES && indent == 2) {
            if (config->node_count >= EN_MAX_NODES) {
                set_error(error, error_len, line_no, "too many nodes");
                return EN_ERR_INVALID_ARGUMENT;
            }
            state->node = &config->nodes[config->node_count++];
            memset(state->node, 0, sizeof(*state->node));
            state->node->administrative_state = EN_ADMIN_ENABLED;
            state->context = YAML_CONTEXT_NONE;
            if (has_key) return parse_node_kv(state, key, value, error, error_len, line_no);
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_NODES && state->context == YAML_CONTEXT_NODE_ENDPOINTS) {
            if (state->node == NULL || state->node->endpoint_count >= EN_MAX_ENDPOINTS) {
                set_error(error, error_len, line_no, "too many node endpoints");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            copy_id(state->node->endpoints[state->node->endpoint_count++], EN_MAX_ID_LEN, item);
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_NODES && state->context == YAML_CONTEXT_NODE_CAPABILITIES) {
            if (state->node == NULL || state->node->capability_count >= EN_MAX_CAPABILITIES) {
                set_error(error, error_len, line_no, "too many node capabilities");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            copy_id(state->node->capabilities[state->node->capability_count++], EN_MAX_ID_LEN, item);
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_TUNNELS && indent == 2) {
            if (config->tunnel_count >= EN_MAX_TUNNELS) {
                set_error(error, error_len, line_no, "too many tunnels");
                return EN_ERR_INVALID_ARGUMENT;
            }
            state->tunnel = &config->tunnels[config->tunnel_count++];
            memset(state->tunnel, 0, sizeof(*state->tunnel));
            copy_id(state->tunnel->protocol, sizeof(state->tunnel->protocol), "ipsec");
            copy_id(state->tunnel->tunnel_type, sizeof(state->tunnel->tunnel_type), "ipsec");
            state->tunnel->gre_instance = -1;
            copy_id(state->tunnel->auth_method, sizeof(state->tunnel->auth_method), "psk");
            state->tunnel->state = EN_TUNNEL_CONFIGURED;
            state->tunnel->health = EN_HEALTH_UNKNOWN;
            state->context = YAML_CONTEXT_NONE;
            if (has_key) {
                return parse_tunnel_kv(state, key, value, error, error_len, line_no);
            }
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_VPP_EDGES && indent == 2) {
            if (config->vpp_edge_count >= EN_MAX_VPP_EDGES) {
                set_error(error, error_len, line_no, "too many vpp edges");
                return EN_ERR_INVALID_ARGUMENT;
            }
            state->vpp_edge = &config->vpp_edges[config->vpp_edge_count++];
            memset(state->vpp_edge, 0, sizeof(*state->vpp_edge));
            state->context = YAML_CONTEXT_NONE;
            if (has_key) {
                return parse_vpp_edge_kv(state, key, value, error, error_len, line_no);
            }
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_PATHS && indent == 2) {
            if (config->path_count >= EN_MAX_PATHS) {
                set_error(error, error_len, line_no, "too many paths");
                return EN_ERR_INVALID_ARGUMENT;
            }
            state->path = &config->paths[config->path_count++];
            memset(state->path, 0, sizeof(*state->path));
            state->path->administrative_state = EN_ADMIN_ENABLED;
            state->path->operational_state = EN_PATH_UNKNOWN;
            state->context = YAML_CONTEXT_NONE;
            if (has_key) {
                return parse_path_kv(state, key, value, error, error_len, line_no);
            }
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_PATHS && state->context == YAML_CONTEXT_PATH_WAYPOINTS) {
            if (state->path == NULL || state->path->waypoint_count >= EN_MAX_WAYPOINTS) {
                set_error(error, error_len, line_no, "too many path waypoints");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            copy_id(state->path->waypoints[state->path->waypoint_count++], EN_MAX_ID_LEN, item);
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_PATHS && state->context == YAML_CONTEXT_PATH_SEGMENTS) {
            if (state->path == NULL || state->path->segment_count >= EN_MAX_SEGMENTS) {
                set_error(error, error_len, line_no, "too many path segments");
                return EN_ERR_INVALID_ARGUMENT;
            }
            state->segment = &state->path->segments[state->path->segment_count++];
            memset(state->segment, 0, sizeof(*state->segment));
            if (has_key) {
                return parse_segment_kv(state, key, value, error, error_len, line_no);
            }
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_PATHS && state->context == YAML_CONTEXT_PATH_ROUTES) {
            if (state->path == NULL || state->path->route_count >= EN_MAX_ROUTES) {
                set_error(error, error_len, line_no, "too many path routes");
                return EN_ERR_INVALID_ARGUMENT;
            }
            state->route = &state->path->routes[state->path->route_count++];
            memset(state->route, 0, sizeof(*state->route));
            state->route->table_id = -1;
            state->route->metric = -1;
            state->path->routes_explicit = true;
            if (has_key) {
                return parse_route_kv(state, key, value, error, error_len, line_no);
            }
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_INTENTS && indent == 2) {
            if (config->intent_count >= EN_MAX_CANDIDATES) {
                set_error(error, error_len, line_no, "too many intents");
                return EN_ERR_INVALID_ARGUMENT;
            }
            state->intent = &config->intents[config->intent_count++];
            memset(state->intent, 0, sizeof(*state->intent));
            state->intent->transition.strategy = EN_TRANSITION_IMMEDIATE;
            state->intent->transition.max_pause_ms = 50;
            state->intent->transition.drain_timeout_ms = 40;
            state->intent->transition.timeout_ms = 5000;
            state->intent->fallback.enabled = true;
            state->context = YAML_CONTEXT_NONE;
            if (has_key) {
                return parse_intent_kv(state, key, value, error, error_len, line_no);
            }
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_INTENTS && state->context == YAML_CONTEXT_INTENT_CANDIDATES) {
            if (state->intent == NULL || state->intent->path_selection.candidate_count >= EN_MAX_CANDIDATES) {
                set_error(error, error_len, line_no, "too many path candidates");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            copy_id(state->intent->path_selection.candidates[state->intent->path_selection.candidate_count++], EN_MAX_ID_LEN, item);
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_INTENTS && state->context == YAML_CONTEXT_INTENT_FORBIDDEN_WAYPOINTS) {
            en_path_constraints_t *constraints = &state->intent->path_selection.constraints;
            if (constraints->forbidden_waypoint_count >= EN_MAX_WAYPOINTS) {
                set_error(error, error_len, line_no, "too many forbidden waypoints");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            copy_id(constraints->forbidden_waypoints[constraints->forbidden_waypoint_count++], EN_MAX_ID_LEN, item);
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_INTENTS && state->context == YAML_CONTEXT_INTENT_REQUIRED_WAYPOINTS) {
            en_path_constraints_t *constraints = &state->intent->path_selection.constraints;
            if (constraints->required_waypoint_count >= EN_MAX_WAYPOINTS) {
                set_error(error, error_len, line_no, "too many required waypoints");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            copy_id(constraints->required_waypoints[constraints->required_waypoint_count++], EN_MAX_ID_LEN, item);
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_INTENTS && state->context == YAML_CONTEXT_INTENT_REQUIRED_CAPABILITIES) {
            en_path_constraints_t *constraints = &state->intent->path_selection.constraints;
            if (constraints->required_capability_count >= EN_MAX_CAPABILITIES) {
                set_error(error, error_len, line_no, "too many required capabilities");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            copy_id(constraints->required_capabilities[constraints->required_capability_count++], EN_MAX_ID_LEN, item);
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_INTENTS && state->context == YAML_CONTEXT_INTENT_COMPARISON_ORDER) {
            if (state->intent->path_selection.comparison_count >= EN_MAX_COMPARISONS) {
                set_error(error, error_len, line_no, "too many comparison keys");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            bool ok = false;
            en_comparison_key_t key_value = parse_comparison_key(item, &ok);
            if (!ok) {
                set_error(error, error_len, line_no, "unknown comparison key");
                return EN_ERR_INVALID_ARGUMENT;
            }
            state->intent->path_selection.comparison_order[state->intent->path_selection.comparison_count++] = key_value;
            return EN_ERR_NONE;
        }

        if (state->top == YAML_SECTION_VPP_EDGES && state->context == YAML_CONTEXT_VPP_EDGE_ALLOWED_VLANS) {
            if (state->vpp_edge == NULL || state->vpp_edge->allowed_vlan_count >= EN_MAX_ALLOWED_VLANS) {
                set_error(error, error_len, line_no, "too many allowed VLANs");
                return EN_ERR_INVALID_ARGUMENT;
            }
            clean_scalar(item);
            int vlan_id = parse_integer(item);
            if (vlan_id < 1 || vlan_id > 4094) {
                set_error(error, error_len, line_no, "allowed VLAN must be between 1 and 4094");
                return EN_ERR_INVALID_ARGUMENT;
            }
            for (size_t index = 0; index < state->vpp_edge->allowed_vlan_count; index++) {
                if (state->vpp_edge->allowed_vlans[index] == vlan_id) {
                    set_error(error, error_len, line_no, "duplicate allowed VLAN");
                    return EN_ERR_INVALID_ARGUMENT;
                }
            }
            state->vpp_edge->allowed_vlans[state->vpp_edge->allowed_vlan_count++] = vlan_id;
            return EN_ERR_NONE;
        }

        set_error(error, error_len, line_no, "unexpected list item");
        return EN_ERR_INVALID_ARGUMENT;
    }

    char *key = NULL;
    char *value = NULL;
    if (!split_key_value(text, &key, &value)) {
        set_error(error, error_len, line_no, "expected key: value");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (strlen(value) >= EN_MAX_ID_LEN) {
        set_error(error, error_len, line_no, "YAML value exceeds fixed field capacity");
        return EN_ERR_INVALID_ARGUMENT;
    }

    if (state->top == YAML_SECTION_TUNNELS) {
        return parse_tunnel_kv(state, key, value, error, error_len, line_no);
    }

    if (state->top == YAML_SECTION_NODES) {
        return parse_node_kv(state, key, value, error, error_len, line_no);
    }

    if (state->top == YAML_SECTION_PATHS) {
        if (state->context == YAML_CONTEXT_PATH_SEGMENTS && state->segment != NULL && indent >= 6) {
            return parse_segment_kv(state, key, value, error, error_len, line_no);
        }
        if (state->context == YAML_CONTEXT_PATH_ROUTES && state->route != NULL && indent >= 6) {
            return parse_route_kv(state, key, value, error, error_len, line_no);
        }
        return parse_path_kv(state, key, value, error, error_len, line_no);
    }

    if (state->top == YAML_SECTION_VPP_EDGES) {
        return parse_vpp_edge_kv(state, key, value, error, error_len, line_no);
    }

    if (state->top == YAML_SECTION_INTENTS) {
        if (strcmp(key, "id") == 0 || strcmp(key, "intent_id") == 0 || strcmp(key, "block_non_ipsec") == 0 || strcmp(key, "deny_unmatched_vlan") == 0) {
            return parse_intent_kv(state, key, value, error, error_len, line_no);
        }
        if (strcmp(key, "traffic") == 0) {
            state->context = YAML_CONTEXT_INTENT_TRAFFIC;
            return EN_ERR_NONE;
        }
        if (strcmp(key, "path_selection") == 0) {
            state->context = YAML_CONTEXT_INTENT_SELECTION;
            return EN_ERR_NONE;
        }
        if (strcmp(key, "candidates") == 0) {
            state->context = YAML_CONTEXT_INTENT_CANDIDATES;
            return EN_ERR_NONE;
        }
        if (strcmp(key, "constraints") == 0) {
            state->context = YAML_CONTEXT_INTENT_CONSTRAINTS;
            return EN_ERR_NONE;
        }
        if (strcmp(key, "comparison_order") == 0) {
            state->context = YAML_CONTEXT_INTENT_COMPARISON_ORDER;
            return EN_ERR_NONE;
        }
        if (strcmp(key, "transition") == 0) {
            state->context = YAML_CONTEXT_INTENT_TRANSITION;
            return EN_ERR_NONE;
        }
        if (strcmp(key, "fallback") == 0) {
            state->context = YAML_CONTEXT_INTENT_FALLBACK;
            return EN_ERR_NONE;
        }
        if (state->context == YAML_CONTEXT_INTENT_TRAFFIC) {
            return parse_intent_kv(state, key, value, error, error_len, line_no);
        }
        if (state->context == YAML_CONTEXT_INTENT_SELECTION) {
            return parse_selection_kv(state, key, value, error, error_len, line_no);
        }
        if (state->context == YAML_CONTEXT_INTENT_CONSTRAINTS) {
            if (strcmp(key, "forbidden_waypoints") == 0) {
                state->context = YAML_CONTEXT_INTENT_FORBIDDEN_WAYPOINTS;
                return EN_ERR_NONE;
            }
            if (strcmp(key, "required_waypoints") == 0) {
                state->context = YAML_CONTEXT_INTENT_REQUIRED_WAYPOINTS;
                return EN_ERR_NONE;
            }
            if (strcmp(key, "required_capabilities") == 0 || strcmp(key, "capabilities") == 0) {
                state->context = YAML_CONTEXT_INTENT_REQUIRED_CAPABILITIES;
                return EN_ERR_NONE;
            }
            return parse_constraints_kv(state, key, value);
        }
        if (state->context == YAML_CONTEXT_INTENT_TRANSITION) {
            return parse_transition_kv(state, key, value);
        }
        if (state->context == YAML_CONTEXT_INTENT_FALLBACK) {
            return parse_fallback_kv(state, key, value);
        }
        return parse_intent_kv(state, key, value, error, error_len, line_no);
    }

    return EN_ERR_NONE;
}

static en_error_code_t parse_vpp_edge_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no)
{
    if (state->vpp_edge == NULL) {
        set_error(error, error_len, line_no, "vpp edge field without vpp edge item");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(key, "id") == 0 || strcmp(key, "node") == 0 || strcmp(key, "node_id") == 0) {
        copy_id(state->vpp_edge->node_id, sizeof(state->vpp_edge->node_id), value);
    } else if (strcmp(key, "port_id") == 0 || strcmp(key, "port") == 0) {
        copy_id(state->vpp_edge->port_id, sizeof(state->vpp_edge->port_id), value);
    } else if (strcmp(key, "host_interface") == 0 || strcmp(key, "host_if") == 0) {
        copy_id(state->vpp_edge->host_interface, sizeof(state->vpp_edge->host_interface), value);
    } else if (strcmp(key, "vpp_interface") == 0 || strcmp(key, "vpp_if") == 0) {
        copy_id(state->vpp_edge->vpp_interface, sizeof(state->vpp_edge->vpp_interface), value);
    } else if (
        strcmp(key, "namespace_interface") == 0 ||
        strcmp(key, "ns_interface") == 0 ||
        strcmp(key, "ns_if") == 0
    ) {
        copy_id(state->vpp_edge->namespace_interface, sizeof(state->vpp_edge->namespace_interface), value);
    } else if (strcmp(key, "namespace_address") == 0 || strcmp(key, "ns_address") == 0 || strcmp(key, "ns_addr") == 0) {
        copy_id(state->vpp_edge->namespace_address, sizeof(state->vpp_edge->namespace_address), value);
    } else if (strcmp(key, "vpp_address") == 0 || strcmp(key, "vpp_addr") == 0) {
        copy_id(state->vpp_edge->vpp_address, sizeof(state->vpp_edge->vpp_address), value);
    } else if (strcmp(key, "vpp_socket") == 0 || strcmp(key, "vpp_api_socket") == 0 || strcmp(key, "vpp_cli_socket") == 0) {
        copy_id(state->vpp_edge->vpp_socket, sizeof(state->vpp_edge->vpp_socket), value);
    } else if (strcmp(key, "next_hop") == 0) {
        copy_id(state->vpp_edge->next_hop, sizeof(state->vpp_edge->next_hop), value);
    } else if (strcmp(key, "allowed_vlans") == 0) {
        state->context = YAML_CONTEXT_VPP_EDGE_ALLOWED_VLANS;
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_tunnel_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no)
{
    if (state->tunnel == NULL) {
        set_error(error, error_len, line_no, "tunnel field without tunnel item");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(key, "id") == 0 || strcmp(key, "tunnel_id") == 0) {
        copy_id(state->tunnel->tunnel_id, sizeof(state->tunnel->tunnel_id), value);
    } else if (strcmp(key, "type") == 0 || strcmp(key, "tunnel_type") == 0) {
        copy_id(state->tunnel->tunnel_type, sizeof(state->tunnel->tunnel_type), value);
    } else if (strcmp(key, "local_node") == 0) {
        copy_id(state->tunnel->local_node, sizeof(state->tunnel->local_node), value);
    } else if (strcmp(key, "remote_node") == 0) {
        copy_id(state->tunnel->remote_node, sizeof(state->tunnel->remote_node), value);
    } else if (strcmp(key, "local_endpoint") == 0) {
        copy_id(state->tunnel->local_endpoint, sizeof(state->tunnel->local_endpoint), value);
    } else if (strcmp(key, "remote_endpoint") == 0) {
        copy_id(state->tunnel->remote_endpoint, sizeof(state->tunnel->remote_endpoint), value);
    } else if (strcmp(key, "local_id") == 0) {
        copy_id(state->tunnel->local_id, sizeof(state->tunnel->local_id), value);
    } else if (strcmp(key, "remote_id") == 0) {
        copy_id(state->tunnel->remote_id, sizeof(state->tunnel->remote_id), value);
    } else if (strcmp(key, "psk") == 0) {
        copy_id(state->tunnel->psk, sizeof(state->tunnel->psk), value);
    } else if (strcmp(key, "auth") == 0 || strcmp(key, "auth_method") == 0) {
        copy_id(state->tunnel->auth_method, sizeof(state->tunnel->auth_method), value);
    } else if (strcmp(key, "local_cert") == 0 || strcmp(key, "certificate") == 0) {
        copy_id(state->tunnel->local_cert, sizeof(state->tunnel->local_cert), value);
    } else if (strcmp(key, "remote_cacerts") == 0 || strcmp(key, "ca_certificates") == 0) {
        copy_id(state->tunnel->remote_cacerts, sizeof(state->tunnel->remote_cacerts), value);
    } else if (strcmp(key, "local_ts") == 0 || strcmp(key, "local_traffic_selector") == 0) {
        copy_id(state->tunnel->local_traffic_selector, sizeof(state->tunnel->local_traffic_selector), value);
    } else if (strcmp(key, "remote_ts") == 0 || strcmp(key, "remote_traffic_selector") == 0) {
        copy_id(state->tunnel->remote_traffic_selector, sizeof(state->tunnel->remote_traffic_selector), value);
    } else if (strcmp(key, "protocol") == 0) {
        copy_id(state->tunnel->protocol, sizeof(state->tunnel->protocol), value);
    } else if (strcmp(key, "gre_interface") == 0) {
        copy_id(state->tunnel->gre_interface, sizeof(state->tunnel->gre_interface), value);
    } else if (strcmp(key, "gre_outer_local_endpoint") == 0) {
        copy_id(state->tunnel->gre_outer_local_endpoint, sizeof(state->tunnel->gre_outer_local_endpoint), value);
    } else if (strcmp(key, "gre_outer_remote_endpoint") == 0) {
        copy_id(state->tunnel->gre_outer_remote_endpoint, sizeof(state->tunnel->gre_outer_remote_endpoint), value);
    } else if (strcmp(key, "gre_local_address") == 0) {
        copy_id(state->tunnel->gre_local_address, sizeof(state->tunnel->gre_local_address), value);
    } else if (strcmp(key, "gre_remote_address") == 0) {
        copy_id(state->tunnel->gre_remote_address, sizeof(state->tunnel->gre_remote_address), value);
    } else if (strcmp(key, "gre_instance") == 0) {
        state->tunnel->gre_instance = atoi(value);
    } else if (strcmp(key, "mtu") == 0 || strcmp(key, "gre_mtu") == 0) {
        state->tunnel->gre_mtu = atoi(value);
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_node_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no)
{
    if (state->node == NULL) {
        set_error(error, error_len, line_no, "node field without node item");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(key, "id") == 0 || strcmp(key, "node_id") == 0 || strcmp(key, "name") == 0) {
        copy_id(state->node->node_id, sizeof(state->node->node_id), value);
    } else if (strcmp(key, "role") == 0) {
        copy_id(state->node->role, sizeof(state->node->role), value);
    } else if (strcmp(key, "endpoints") == 0 || strcmp(key, "addresses") == 0) {
        state->context = YAML_CONTEXT_NODE_ENDPOINTS;
    } else if (strcmp(key, "capabilities") == 0 || strcmp(key, "capability") == 0) {
        state->context = YAML_CONTEXT_NODE_CAPABILITIES;
    } else if (strcmp(key, "administrative_state") == 0 || strcmp(key, "admin_state") == 0) {
        if (strcmp(value, "enabled") == 0) state->node->administrative_state = EN_ADMIN_ENABLED;
        else if (strcmp(value, "disabled") == 0) state->node->administrative_state = EN_ADMIN_DISABLED;
        else {
            set_error(error, error_len, line_no, "node administrative_state must be enabled or disabled");
            return EN_ERR_INVALID_ARGUMENT;
        }
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_path_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no)
{
    if (state->path == NULL) {
        set_error(error, error_len, line_no, "path field without path item");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(key, "id") == 0 || strcmp(key, "path_id") == 0) {
        copy_id(state->path->path_id, sizeof(state->path->path_id), value);
    } else if (strcmp(key, "source") == 0) {
        copy_id(state->path->source, sizeof(state->path->source), value);
    } else if (strcmp(key, "destination") == 0) {
        copy_id(state->path->destination, sizeof(state->path->destination), value);
    } else if (strcmp(key, "route_destination_prefix") == 0 || strcmp(key, "dst_prefix") == 0) {
        copy_id(state->path->route_destination_prefix, sizeof(state->path->route_destination_prefix), value);
    } else if (strcmp(key, "route_next_hop") == 0 || strcmp(key, "next_hop") == 0) {
        copy_id(state->path->route_next_hop, sizeof(state->path->route_next_hop), value);
    } else if (strcmp(key, "egress_tunnel_id") == 0) {
        copy_id(state->path->egress_tunnel_id, sizeof(state->path->egress_tunnel_id), value);
    } else if (strcmp(key, "priority") == 0) {
        state->path->priority = parse_integer(value);
    } else if (strcmp(key, "waypoints") == 0) {
        state->context = YAML_CONTEXT_PATH_WAYPOINTS;
    } else if (strcmp(key, "segments") == 0) {
        state->context = YAML_CONTEXT_PATH_SEGMENTS;
    } else if (strcmp(key, "routes") == 0) {
        state->context = YAML_CONTEXT_PATH_ROUTES;
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_segment_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no)
{
    if (state->segment == NULL) {
        set_error(error, error_len, line_no, "segment field without segment item");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(key, "id") == 0 || strcmp(key, "segment_id") == 0) {
        copy_id(state->segment->segment_id, sizeof(state->segment->segment_id), value);
    } else if (strcmp(key, "from") == 0 || strcmp(key, "from_node") == 0) {
        copy_id(state->segment->from_node, sizeof(state->segment->from_node), value);
    } else if (strcmp(key, "to") == 0 || strcmp(key, "to_node") == 0) {
        copy_id(state->segment->to_node, sizeof(state->segment->to_node), value);
    } else if (strcmp(key, "tunnel_id") == 0) {
        copy_id(state->segment->tunnel_id, sizeof(state->segment->tunnel_id), value);
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_route_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no)
{
    if (state->route == NULL) {
        set_error(error, error_len, line_no, "route field without route item");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(key, "id") == 0 || strcmp(key, "route_id") == 0) {
        copy_id(state->route->route_id, sizeof(state->route->route_id), value);
    } else if (strcmp(key, "node") == 0 || strcmp(key, "node_id") == 0) {
        copy_id(state->route->node_id, sizeof(state->route->node_id), value);
    } else if (
        strcmp(key, "destination_prefix") == 0 ||
        strcmp(key, "dst_prefix") == 0 ||
        strcmp(key, "prefix") == 0 ||
        strcmp(key, "to") == 0
    ) {
        copy_id(state->route->destination_prefix, sizeof(state->route->destination_prefix), value);
    } else if (strcmp(key, "next_hop") == 0 || strcmp(key, "via") == 0) {
        copy_id(state->route->next_hop, sizeof(state->route->next_hop), value);
    } else if (strcmp(key, "interface") == 0 || strcmp(key, "iface") == 0 || strcmp(key, "via_interface") == 0) {
        copy_id(state->route->interface_name, sizeof(state->route->interface_name), value);
    } else if (strcmp(key, "table") == 0 || strcmp(key, "table_id") == 0 || strcmp(key, "vrf") == 0) {
        state->route->table_id = parse_integer(value);
    } else if (strcmp(key, "metric") == 0 || strcmp(key, "preference") == 0) {
        state->route->metric = parse_integer(value);
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_intent_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no)
{
    if (state->intent == NULL) {
        set_error(error, error_len, line_no, "intent field without intent item");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (strcmp(key, "id") == 0 || strcmp(key, "intent_id") == 0) {
        copy_id(state->intent->intent_id, sizeof(state->intent->intent_id), value);
    } else if (strcmp(key, "block_non_ipsec") == 0) {
        state->intent->block_non_ipsec = parse_bool(value);
    } else if (strcmp(key, "deny_unmatched_vlan") == 0) {
        state->intent->deny_unmatched_vlan = parse_bool(value);
    } else if (strcmp(key, "source") == 0) {
        copy_id(state->intent->traffic.source, sizeof(state->intent->traffic.source), value);
    } else if (strcmp(key, "destination") == 0) {
        copy_id(state->intent->traffic.destination, sizeof(state->intent->traffic.destination), value);
    } else if (strcmp(key, "vlan_id") == 0 || strcmp(key, "vlan") == 0 || strcmp(key, "vlan_tag") == 0) {
        state->intent->traffic.has_vlan_id = true;
        state->intent->traffic.vlan_id = parse_integer(value);
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_selection_kv(yaml_parse_state_t *state, const char *key, const char *value, char *error, size_t error_len, size_t line_no)
{
    if (strcmp(key, "mode") == 0) {
        bool ok = false;
        state->intent->path_selection.mode = parse_selection_mode(value, &ok);
        if (!ok) {
            set_error(error, error_len, line_no, "unknown path selection mode");
            return EN_ERR_INVALID_ARGUMENT;
        }
    } else if (strcmp(key, "path_id") == 0) {
        copy_id(state->intent->path_selection.path_id, sizeof(state->intent->path_selection.path_id), value);
    } else if (strcmp(key, "candidates") == 0) {
        state->context = YAML_CONTEXT_INTENT_CANDIDATES;
    } else if (strcmp(key, "comparison_order") == 0) {
        state->context = YAML_CONTEXT_INTENT_COMPARISON_ORDER;
    } else if (strcmp(key, "constraints") == 0) {
        state->context = YAML_CONTEXT_INTENT_CONSTRAINTS;
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_constraints_kv(yaml_parse_state_t *state, const char *key, const char *value)
{
    en_path_constraints_t *constraints = &state->intent->path_selection.constraints;
    if (strcmp(key, "max_rtt_ms") == 0) {
        constraints->has_max_rtt_ms = true;
        constraints->max_rtt_ms = parse_number(value);
    } else if (strcmp(key, "max_packet_loss_percent") == 0) {
        constraints->has_max_packet_loss_percent = true;
        constraints->max_packet_loss_percent = parse_number(value);
    } else if (strcmp(key, "failure_threshold") == 0) {
        constraints->failure_threshold = parse_integer(value);
    } else if (strcmp(key, "recovery_threshold") == 0) {
        constraints->recovery_threshold = parse_integer(value);
    } else if (strcmp(key, "hold_down_ms") == 0) {
        constraints->hold_down_ms = parse_integer(value);
    } else if (strcmp(key, "hysteresis_percent") == 0) {
        constraints->has_hysteresis_percent = true;
        constraints->hysteresis_percent = parse_number(value);
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_transition_kv(yaml_parse_state_t *state, const char *key, const char *value)
{
    if (strcmp(key, "strategy") == 0) {
        bool ok = false;
        state->intent->transition.strategy = parse_transition_strategy(value, &ok);
        return ok ? EN_ERR_NONE : EN_ERR_INVALID_ARGUMENT;
    } else if (strcmp(key, "max_pause_ms") == 0) {
        state->intent->transition.max_pause_ms = parse_integer(value);
    } else if (strcmp(key, "drain_timeout_ms") == 0) {
        state->intent->transition.drain_timeout_ms = parse_integer(value);
    } else if (strcmp(key, "timeout_ms") == 0) {
        state->intent->transition.timeout_ms = parse_integer(value);
    } else if (strcmp(key, "retry_count") == 0) {
        state->intent->transition.retry_count = parse_integer(value);
    } else if (strcmp(key, "retry_backoff_ms") == 0) {
        state->intent->transition.retry_backoff_ms = parse_integer(value);
    }
    return EN_ERR_NONE;
}

static en_error_code_t parse_fallback_kv(yaml_parse_state_t *state, const char *key, const char *value)
{
    if (strcmp(key, "enabled") == 0) {
        state->intent->fallback.enabled = parse_bool(value);
    } else if (strcmp(key, "path_id") == 0) {
        copy_id(state->intent->fallback.path_id, sizeof(state->intent->fallback.path_id), value);
    }
    return EN_ERR_NONE;
}

static en_path_selection_mode_t parse_selection_mode(const char *value, bool *ok)
{
    *ok = true;
    if (strcmp(value, "explicit") == 0) return EN_SELECT_EXPLICIT;
    if (strcmp(value, "priority") == 0) return EN_SELECT_PRIORITY;
    if (strcmp(value, "evaluated") == 0) return EN_SELECT_EVALUATED;
    *ok = false;
    return EN_SELECT_EXPLICIT;
}

static en_transition_strategy_t parse_transition_strategy(const char *value, bool *ok)
{
    *ok = true;
    if (strcmp(value, "immediate") == 0) return EN_TRANSITION_IMMEDIATE;
    if (strcmp(value, "graceful") == 0) return EN_TRANSITION_GRACEFUL;
    if (strcmp(value, "flow-preserve") == 0) return EN_TRANSITION_FLOW_PRESERVE;
    *ok = false;
    return EN_TRANSITION_IMMEDIATE;
}

static en_comparison_key_t parse_comparison_key(const char *value, bool *ok)
{
    *ok = true;
    if (strcmp(value, "packet_loss") == 0 || strcmp(value, "packet_loss_percent") == 0) return EN_COMPARE_PACKET_LOSS;
    if (strcmp(value, "latency") == 0 || strcmp(value, "rtt") == 0 || strcmp(value, "rtt_ms") == 0) return EN_COMPARE_LATENCY;
    if (strcmp(value, "hop_count") == 0) return EN_COMPARE_HOP_COUNT;
    if (strcmp(value, "administrative_priority") == 0 || strcmp(value, "priority") == 0) return EN_COMPARE_ADMIN_PRIORITY;
    if (strcmp(value, "path_id") == 0) return EN_COMPARE_PATH_ID;
    *ok = false;
    return EN_COMPARE_PATH_ID;
}

static bool parse_bool(const char *value)
{
    return strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "1") == 0;
}

static void set_error(char *error, size_t error_len, size_t line_no, const char *message)
{
    if (error == NULL || error_len == 0) {
        return;
    }
    if (line_no == 0) {
        snprintf(error, error_len, "%s", message);
    } else {
        snprintf(error, error_len, "line %zu: %s", line_no, message);
    }
}

static int indent_of(const char *line)
{
    int indent = 0;
    while (*line == ' ') {
        indent++;
        line++;
    }
    return indent;
}

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    *end = '\0';
    return text;
}

static void strip_comment(char *text)
{
    bool quoted = false;
    char quote = '\0';
    for (char *p = text; *p != '\0'; p++) {
        if ((*p == '"' || *p == '\'') && (!quoted || *p == quote)) {
            quoted = !quoted;
            quote = quoted ? *p : '\0';
        }
        if (!quoted && *p == '#') {
            *p = '\0';
            return;
        }
    }
}

static bool split_key_value(char *text, char **key, char **value)
{
    char *colon = strchr(text, ':');
    if (colon == NULL) {
        return false;
    }
    *colon = '\0';
    *key = trim(text);
    *value = trim(colon + 1);
    clean_scalar(*value);
    return true;
}

static void clean_scalar(char *text)
{
    char *value = trim(text);
    if (value != text) {
        memmove(text, value, strlen(value) + 1);
    }
    size_t len = strlen(text);
    if (len >= 2 && ((text[0] == '"' && text[len - 1] == '"') || (text[0] == '\'' && text[len - 1] == '\''))) {
        memmove(text, text + 1, len - 2);
        text[len - 2] = '\0';
    }
}

static void copy_id(char *dst, size_t dst_len, const char *src)
{
    const char *value = src == NULL ? "" : src;
    if (dst == NULL || dst_len == 0) return;
    if (strlen(value) >= dst_len) {
        dst[0] = '\0';
        return;
    }
    memcpy(dst, value, strlen(value) + 1);
}

static void copy_address_without_cidr(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strcspn(src, "/");
    if (len >= dst_len) {
        dst[0] = '\0';
        return;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void yaml_normalize(en_yaml_config_t *config)
{
    if (config == NULL) {
        return;
    }
    for (size_t i = 0; i < config->path_count; i++) {
        en_path_t *path = &config->paths[i];
        if (path->egress_tunnel_id[0] == '\0' && path->segment_count > 0) {
            copy_id(path->egress_tunnel_id, sizeof(path->egress_tunnel_id), path->segments[0].tunnel_id);
        }
        if (path->route_destination_prefix[0] == '\0') {
            const en_tunnel_t *tunnel = yaml_find_tunnel(config, path->egress_tunnel_id);
            if (tunnel != NULL && tunnel->remote_traffic_selector[0] != '\0') {
                copy_id(path->route_destination_prefix, sizeof(path->route_destination_prefix), tunnel->remote_traffic_selector);
            }
        }
        if (path->route_next_hop[0] == '\0') {
            const en_tunnel_t *tunnel = yaml_find_tunnel(config, path->egress_tunnel_id);
            if (tunnel != NULL && tunnel->remote_endpoint[0] != '\0') {
                copy_id(path->route_next_hop, sizeof(path->route_next_hop), tunnel->remote_endpoint);
            }
        }
        if (path->route_count == 0 && path->route_destination_prefix[0] != '\0') {
            en_route_t *route = &path->routes[path->route_count++];
            memset(route, 0, sizeof(*route));
            copy_id(route->route_id, sizeof(route->route_id), "default");
            copy_id(route->node_id, sizeof(route->node_id), path->source);
            copy_id(route->destination_prefix, sizeof(route->destination_prefix), path->route_destination_prefix);
            copy_id(route->next_hop, sizeof(route->next_hop), path->route_next_hop);
            route->table_id = -1;
            route->metric = -1;
        }
        if (path->route_destination_prefix[0] == '\0' && path->route_count > 0) {
            copy_id(path->route_destination_prefix, sizeof(path->route_destination_prefix), path->routes[0].destination_prefix);
        }
        if (path->route_next_hop[0] == '\0' && path->route_count > 0) {
            copy_id(path->route_next_hop, sizeof(path->route_next_hop), path->routes[0].next_hop);
        }
    }
    for (size_t i = 0; i < config->vpp_edge_count; i++) {
        en_vpp_edge_t *edge = &config->vpp_edges[i];
        if (edge->vpp_interface[0] == '\0' && edge->host_interface[0] != '\0') {
            char host_interface[EN_MAX_ID_LEN] = {0};
            copy_id(host_interface, sizeof(host_interface), edge->host_interface);
            snprintf(edge->vpp_interface, sizeof(edge->vpp_interface), "host-%.58s", host_interface);
        }
        if (edge->next_hop[0] == '\0' && edge->namespace_address[0] != '\0') {
            copy_address_without_cidr(edge->next_hop, sizeof(edge->next_hop), edge->namespace_address);
        }
    }
}

static bool yaml_has_path(const en_yaml_config_t *config, const char *path_id)
{
    for (size_t i = 0; i < config->path_count; i++) {
        if (strcmp(config->paths[i].path_id, path_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool yaml_has_tunnel(const en_yaml_config_t *config, const char *tunnel_id)
{
    return yaml_find_tunnel(config, tunnel_id) != NULL;
}

static bool yaml_has_node(const en_yaml_config_t *config, const char *node_id)
{
    if (config == NULL || node_id == NULL || node_id[0] == '\0') return false;
    for (size_t i = 0; i < config->node_count; i++) {
        if (strcmp(config->nodes[i].node_id, node_id) == 0) return true;
    }
    return false;
}

static bool yaml_has_vpp_edge(const en_yaml_config_t *config, const char *node_id)
{
    if (node_id == NULL || node_id[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < config->vpp_edge_count; i++) {
        if (strcmp(config->vpp_edges[i].node_id, node_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool yaml_has_vpp_edge_interface(const en_yaml_config_t *config, const en_route_t *route)
{
    if (config == NULL || route == NULL || route->node_id[0] == '\0' || route->interface_name[0] == '\0') return false;
    for (size_t index = 0; index < config->vpp_edge_count; index++) {
        const en_vpp_edge_t *edge = &config->vpp_edges[index];
        if (strcmp(edge->node_id, route->node_id) == 0 &&
            (strcmp(edge->vpp_interface, route->interface_name) == 0 ||
             (edge->host_interface[0] != '\0' && strcmp(edge->host_interface, route->interface_name) == 0))) return true;
    }
    return false;
}

static const en_tunnel_t *yaml_find_tunnel(const en_yaml_config_t *config, const char *tunnel_id)
{
    if (config == NULL || tunnel_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < config->tunnel_count; i++) {
        if (strcmp(config->tunnels[i].tunnel_id, tunnel_id) == 0) {
            return &config->tunnels[i];
        }
    }
    return NULL;
}
