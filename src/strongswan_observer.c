#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "eventnet/strongswan_observer.h"

#include <ctype.h>
#include <stdio.h>
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

static bool same_text(const char *left, const char *right)
{
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) return false;
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
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

static bool child_header(const char *line, const char *child_id, size_t *indent)
{
    size_t line_indent = indentation(line);
    const char *value = skip_space(line);
    size_t child_len = strlen(child_id);
    if (strncmp(value, child_id, child_len) != 0 || value[child_len] != ':') return false;
    if (indent != NULL) *indent = line_indent;
    return true;
}

static bool state_value(const char *line, char *value, size_t value_len)
{
    const char *marker = strstr(line, "state:");
    if (marker == NULL) return false;
    marker = skip_space(marker + strlen("state:"));
    size_t length = 0;
    while (marker[length] != '\0' && marker[length] != '\r' && marker[length] != '\n' && !isspace((unsigned char)marker[length])) length++;
    if (length == 0 || length >= value_len) return false;
    memcpy(value, marker, length);
    value[length] = '\0';
    return true;
}

static void map_state(const char *value, en_tunnel_state_t *state, en_health_state_t *health)
{
    if (same_text(value, "INSTALLED")) {
        *state = EN_TUNNEL_ESTABLISHED;
        *health = EN_HEALTH_HEALTHY;
    } else if (same_text(value, "REKEYING")) {
        *state = EN_TUNNEL_REKEYING;
        *health = EN_HEALTH_HEALTHY;
    } else if (same_text(value, "CONNECTING") || same_text(value, "ROUTED")) {
        *state = EN_TUNNEL_ESTABLISHING;
        *health = EN_HEALTH_UNKNOWN;
    } else if (same_text(value, "DELETING") || same_text(value, "DESTROYING")) {
        *state = EN_TUNNEL_DELETING;
        *health = EN_HEALTH_DEGRADED;
    } else {
        *state = EN_TUNNEL_FAILED;
        *health = EN_HEALTH_FAILED;
    }
}

en_error_code_t en_strongswan_parse_list_sas(const char *output, const char *child_id,
    en_strongswan_sa_observation_t *observation, char *error, size_t error_len)
{
    if (output == NULL || child_id == NULL || observation == NULL || !valid_label(child_id)) {
        set_error(error, error_len, "invalid swanctl observation argument");
        return EN_ERR_INVALID_ARGUMENT;
    }
    memset(observation, 0, sizeof(*observation));
    snprintf(observation->child_id, sizeof(observation->child_id), "%s", child_id);
    char line[512];
    const char *cursor = output;
    bool inside = false;
    bool found_state = false;
    size_t child_indent = 0;
    while (*cursor != '\0') {
        size_t length = 0;
        while (cursor[length] != '\0' && cursor[length] != '\n' && length + 1 < sizeof(line)) length++;
        memcpy(line, cursor, length);
        line[length] = '\0';
        if (child_header(line, child_id, &child_indent)) inside = true;
        else if (inside && indentation(line) <= child_indent && skip_space(line)[0] != '\0') break;
        else if (inside) {
            char state_text[32] = {0};
            if (state_value(line, state_text, sizeof(state_text))) {
                map_state(state_text, &observation->state, &observation->health);
                found_state = true;
            }
        }
        cursor += length;
        if (*cursor == '\n') cursor++;
    }
    if (!inside || !found_state) {
        set_error(error, error_len, "swanctl child SA was not found or has no state");
        return EN_ERR_NOT_FOUND;
    }
    return EN_ERR_NONE;
}

en_error_code_t en_strongswan_observation_to_event_json(const en_strongswan_sa_observation_t *observation,
    const char *path_id, long long timestamp_ms, char *output, size_t output_len)
{
    if (observation == NULL || path_id == NULL || !valid_label(path_id) || !valid_label(observation->child_id) || output == NULL || output_len == 0 || timestamp_ms < 0) return EN_ERR_INVALID_ARGUMENT;
    const char *state = observation->state == EN_TUNNEL_ESTABLISHED ? "installed" :
        observation->state == EN_TUNNEL_REKEYING ? "rekeying" : "failed";
    int written = snprintf(output, output_len,
        "{\"schema\":\"ibuki.event.tunnel.v1\",\"path_id\":\"%s\",\"tunnel_id\":\"%s\",\"state\":\"%s\",\"timestamp_ms\":%lld}\n",
        path_id, observation->child_id, state, timestamp_ms);
    return written > 0 && (size_t)written < output_len ? EN_ERR_NONE : EN_ERR_INVALID_ARGUMENT;
}
