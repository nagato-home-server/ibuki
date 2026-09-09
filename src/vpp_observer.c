#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "eventnet/vpp_observer.h"
#include "eventnet/json_output.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error != NULL && error_len > 0) snprintf(error, error_len, "%s", message);
}

static bool valid_label(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == ':' || *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-')) return false;
    }
    return true;
}

static const char *skip_space(const char *value)
{
    while (*value == ' ' || *value == '\t') value++;
    return value;
}

static size_t indentation(const char *line)
{
    size_t count = 0;
    while (line[count] == ' ' || line[count] == '\t') count++;
    return count;
}

static bool route_header(const char *line, const char *prefix)
{
    const char *value = skip_space(line);
    size_t prefix_len = strlen(prefix);
    return strncmp(value, prefix, prefix_len) == 0 &&
        (value[prefix_len] == '\0' || value[prefix_len] == '\r' || value[prefix_len] == '\n' || value[prefix_len] == ' ' || value[prefix_len] == ',');
}

static bool token_after(const char *line, const char *marker, char *value, size_t value_len)
{
    const char *start = strstr(line, marker);
    if (start == NULL) return false;
    start = skip_space(start + strlen(marker));
    size_t length = 0;
    while (start[length] != '\0' && start[length] != '\r' && start[length] != '\n' && !isspace((unsigned char)start[length])) length++;
    if (length == 0 || length >= value_len) return false;
    memcpy(value, start, length);
    value[length] = '\0';
    return true;
}

static bool has_word(const char *line, const char *word)
{
    char copy[512] = {0};
    snprintf(copy, sizeof(copy), "%s", line);
    for (char *token = strtok(copy, " \t\r\n"); token != NULL; token = strtok(NULL, " \t\r\n")) {
        if (strcmp(token, word) == 0) return true;
    }
    return false;
}

en_error_code_t en_vpp_parse_show_ip_fib_in_table(const char *output, const char *destination_prefix, int table_id,
    en_vpp_route_observation_t *observation, char *error, size_t error_len)
{
    if (output == NULL || destination_prefix == NULL || observation == NULL || !valid_label(destination_prefix)) {
        set_error(error, error_len, "invalid VPP observation argument");
        return EN_ERR_INVALID_ARGUMENT;
    }
    memset(observation, 0, sizeof(*observation));
    observation->table_id = -1;
    snprintf(observation->destination_prefix, sizeof(observation->destination_prefix), "%s", destination_prefix);
    char line[512];
    const char *cursor = output;
    bool inside = false;
    size_t header_indent = 0;
    int current_table = -1;
    while (*cursor != '\0') {
        size_t length = 0;
        while (cursor[length] != '\0' && cursor[length] != '\n' && length + 1 < sizeof(line)) length++;
        memcpy(line, cursor, length);
        line[length] = '\0';
        const char *vrf_marker = strstr(line, "ipv4-VRF:");
        if (vrf_marker != NULL) {
            char *end = NULL;
            long parsed_table = strtol(vrf_marker + strlen("ipv4-VRF:"), &end, 10);
            if (end != vrf_marker + strlen("ipv4-VRF:") && parsed_table >= 0 && parsed_table <= INT_MAX) current_table = (int)parsed_table;
        }
        if (route_header(line, destination_prefix) && (table_id < 0 || current_table == table_id)) {
            inside = true;
            header_indent = indentation(line);
        } else if (inside && indentation(line) <= header_indent && skip_space(line)[0] != '\0') {
            break;
        } else if (inside && token_after(line, "via ", observation->next_hop, sizeof(observation->next_hop))) {
            token_after(line, "via ", observation->next_hop, sizeof(observation->next_hop));
            const char *last_space = strrchr(line, ' ');
            if (last_space != NULL && last_space[1] != '\0') snprintf(observation->interface_name, sizeof(observation->interface_name), "%s", skip_space(last_space));
            if (!valid_label(observation->next_hop) || !valid_label(observation->interface_name)) {
                set_error(error, error_len, "VPP route contains invalid identity");
                return EN_ERR_INVALID_ARGUMENT;
            }
            observation->table_id = current_table;
            observation->present = true;
            return EN_ERR_NONE;
        }
        cursor += length;
        if (*cursor == '\n') cursor++;
    }
    if (!inside) set_error(error, error_len, "VPP route was not found");
    else set_error(error, error_len, "VPP route has no next-hop");
    return EN_ERR_NOT_FOUND;
}

en_error_code_t en_vpp_parse_show_ip_fib(const char *output, const char *destination_prefix,
    en_vpp_route_observation_t *observation, char *error, size_t error_len)
{
    return en_vpp_parse_show_ip_fib_in_table(output, destination_prefix, -1, observation, error, error_len);
}

en_error_code_t en_vpp_parse_show_interface(const char *output, const char *interface_name,
    en_vpp_interface_observation_t *observation, char *error, size_t error_len)
{
    if (output == NULL || interface_name == NULL || observation == NULL || interface_name[0] == '\0') {
        set_error(error, error_len, "invalid VPP interface observation argument");
        return EN_ERR_INVALID_ARGUMENT;
    }
    memset(observation, 0, sizeof(*observation));
    snprintf(observation->interface_name, sizeof(observation->interface_name), "%s", interface_name);
    char line[512];
    const char *cursor = output;
    while (*cursor != '\0') {
        size_t length = 0;
        while (cursor[length] != '\0' && cursor[length] != '\n' && length + 1 < sizeof(line)) length++;
        memcpy(line, cursor, length);
        line[length] = '\0';
        const char *first = skip_space(line);
        size_t name_len = strlen(interface_name);
        if (strncmp(first, interface_name, name_len) == 0 && (first[name_len] == ' ' || first[name_len] == '\t')) {
            observation->present = true;
            observation->up = has_word(line, "up");
            return EN_ERR_NONE;
        }
        cursor += length;
        if (*cursor == '\n') cursor++;
    }
    set_error(error, error_len, "VPP interface was not found");
    return EN_ERR_NOT_FOUND;
}

en_error_code_t en_vpp_route_observation_to_event_json(const en_vpp_route_observation_t *observation,
    const char *path_id, const char *route_id, long long timestamp_ms, char *output, size_t output_len)
{
    if (observation == NULL || path_id == NULL || route_id == NULL || !valid_label(path_id) || !valid_label(route_id) ||
        (observation->destination_prefix[0] != '\0' && !valid_label(observation->destination_prefix)) ||
        (observation->next_hop[0] != '\0' && !valid_label(observation->next_hop)) ||
        (observation->interface_name[0] != '\0' && !valid_label(observation->interface_name)) ||
        output == NULL || output_len == 0 || timestamp_ms < 0) return EN_ERR_INVALID_ARGUMENT;
    const char *state = observation->present ? "up" : "down";
    bool has_route_details = observation->destination_prefix[0] != '\0' && observation->next_hop[0] != '\0';
    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = document == NULL ? NULL : yyjson_mut_obj(document);
    bool valid = root != NULL && yyjson_mut_obj_add_str(document, root, "schema", "ibuki.event.vpp.route.v1") &&
        yyjson_mut_obj_add_str(document, root, "path_id", path_id) && yyjson_mut_obj_add_str(document, root, "tunnel_id", route_id);
    if (valid && has_route_details) valid = yyjson_mut_obj_add_str(document, root, "destination_prefix", observation->destination_prefix) && yyjson_mut_obj_add_str(document, root, "next_hop", observation->next_hop);
    if (valid && observation->table_id >= 0) valid = yyjson_mut_obj_add_int(document, root, "table_id", observation->table_id);
    if (valid) valid = yyjson_mut_obj_add_str(document, root, "state", state) && yyjson_mut_obj_add_sint(document, root, "timestamp_ms", timestamp_ms);
    if (!valid) { yyjson_mut_doc_free(document); return EN_ERR_INVALID_ARGUMENT; }
    yyjson_mut_doc_set_root(document, root);
    en_error_code_t result = en_json_mut_doc_to_buffer(document, output, output_len);
    yyjson_mut_doc_free(document);
    if (result == EN_ERR_NONE) {
        size_t length = strlen(output);
        if (length + 2 > output_len) return EN_ERR_INVALID_ARGUMENT;
        output[length] = '\n'; output[length + 1] = '\0';
    }
    return result;
}

en_error_code_t en_vpp_interface_observation_to_event_json(const en_vpp_interface_observation_t *observation,
    const char *path_id, long long timestamp_ms, char *output, size_t output_len)
{
    if (observation == NULL || path_id == NULL || !valid_label(path_id) || observation->interface_name[0] == '\0' ||
        !valid_label(observation->interface_name) || output == NULL || output_len == 0 || timestamp_ms < 0) return EN_ERR_INVALID_ARGUMENT;
    yyjson_mut_doc *document = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = document == NULL ? NULL : yyjson_mut_obj(document);
    bool valid = root != NULL && yyjson_mut_obj_add_str(document, root, "schema", "ibuki.event.vpp.interface.v1") &&
        yyjson_mut_obj_add_str(document, root, "path_id", path_id) && yyjson_mut_obj_add_str(document, root, "interface_name", observation->interface_name) &&
        yyjson_mut_obj_add_str(document, root, "state", observation->present && observation->up ? "up" : "down") &&
        yyjson_mut_obj_add_sint(document, root, "timestamp_ms", timestamp_ms);
    if (!valid) { yyjson_mut_doc_free(document); return EN_ERR_INVALID_ARGUMENT; }
    yyjson_mut_doc_set_root(document, root);
    en_error_code_t result = en_json_mut_doc_to_buffer(document, output, output_len);
    yyjson_mut_doc_free(document);
    if (result == EN_ERR_NONE) {
        size_t length = strlen(output);
        if (length + 2 > output_len) return EN_ERR_INVALID_ARGUMENT;
        output[length] = '\n'; output[length + 1] = '\0';
    }
    return result;
}
