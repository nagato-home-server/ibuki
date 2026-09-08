#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "eventnet/telemetry.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static bool valid_label(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (size_t index = 0; value[index] != '\0'; index++) {
        unsigned char character = (unsigned char)value[index];
        if (!isalnum(character) && character != ':' && character != '/' && character != '.' && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

static bool json_string(const char *line, const char *key, char *value, size_t value_len)
{
    char marker[96];
    snprintf(marker, sizeof(marker), "\"%s\":\"", key);
    const char *start = strstr(line, marker);
    if (start == NULL || value_len == 0) return false;
    start += strlen(marker);
    const char *end = strchr(start, '\"');
    if (end == NULL || (size_t)(end - start) >= value_len) return false;
    memcpy(value, start, (size_t)(end - start));
    value[end - start] = '\0';
    return true;
}

static bool json_number(const char *line, const char *key, double *value)
{
    char marker[96];
    char *end = NULL;
    const char *cursor;
    const char *number_end;
    double parsed;
    snprintf(marker, sizeof(marker), "\"%s\":", key);
    const char *start = strstr(line, marker);
    if (start == NULL) return false;
    cursor = start + strlen(marker);
    while (isspace((unsigned char)*cursor)) cursor++;
    number_end = cursor;
    if (*number_end == '-') number_end++;
    if (*number_end == '0') {
        number_end++;
        if (isdigit((unsigned char)*number_end)) return false;
    } else {
        if (!isdigit((unsigned char)*number_end)) return false;
        while (isdigit((unsigned char)*number_end)) number_end++;
    }
    if (*number_end == '.') {
        number_end++;
        if (!isdigit((unsigned char)*number_end)) return false;
        while (isdigit((unsigned char)*number_end)) number_end++;
    }
    if (*number_end == 'e' || *number_end == 'E') {
        number_end++;
        if (*number_end == '+' || *number_end == '-') number_end++;
        if (!isdigit((unsigned char)*number_end)) return false;
        while (isdigit((unsigned char)*number_end)) number_end++;
    }
    errno = 0;
    parsed = strtod(cursor, &end);
    if (errno == ERANGE || end != number_end || !isfinite(parsed)) return false;
    end = (char *)number_end;
    while (isspace((unsigned char)*end)) end++;
    if (*end != ',' && *end != '}') return false;
    *value = parsed;
    return true;
}

static bool json_integer(const char *line, const char *key, int *value)
{
    double parsed = 0.0;
    if (!json_number(line, key, &parsed) || parsed < 0.0 || parsed > (double)INT_MAX || floor(parsed) != parsed) return false;
    *value = (int)parsed;
    return true;
}

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error != NULL && error_len > 0) snprintf(error, error_len, "%s", message);
}

FILE *en_telemetry_open_jsonl(const char *filename)
{
#if defined(_WIN32)
    return fopen(filename, "r");
#else
    int file_descriptor = open(filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file_descriptor < 0) return NULL;
    struct stat file_stat;
    if (fstat(file_descriptor, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
        (file_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(file_descriptor);
        errno = EPERM;
        return NULL;
    }
    FILE *file = fdopen(file_descriptor, "r");
    if (file == NULL) close(file_descriptor);
    return file;
#endif
}

static bool has_json_object_bounds(const char *line)
{
    const unsigned char *cursor = (const unsigned char *)line;
    while (*cursor != '\0' && isspace(*cursor)) cursor++;
    if (*cursor++ != '{') return false;
    const unsigned char *end = cursor + strlen((const char *)cursor);
    while (end > cursor && isspace(end[-1])) end--;
    return end > cursor && end[-1] == '}';
}

en_error_code_t en_telemetry_parse_json_line(const char *line, en_path_health_t *record, char *error, size_t error_len)
{
    if (line == NULL || record == NULL) {
        set_error(error, error_len, "invalid telemetry record argument");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (!has_json_object_bounds(line)) {
        set_error(error, error_len, "telemetry record is not a complete JSON object");
        return EN_ERR_INVALID_ARGUMENT;
    }
    char schema[EN_MAX_ID_LEN] = {0};
    if (!json_string(line, "schema", schema, sizeof(schema))) {
        set_error(error, error_len, "telemetry record has no schema");
        return EN_ERR_INVALID_ARGUMENT;
    }
    bool is_health = strcmp(schema, "ibuki.telemetry.path_health.v1") == 0;
    bool is_path_event = strcmp(schema, "ibuki.event.path.v1") == 0;
    bool is_tunnel_event = strcmp(schema, "ibuki.event.tunnel.v1") == 0;
    bool is_route_event = strcmp(schema, "ibuki.event.vpp.route.v1") == 0;
    bool is_interface_event = strcmp(schema, "ibuki.event.vpp.interface.v1") == 0;
    bool is_event = is_path_event || is_tunnel_event || is_route_event || is_interface_event;
    if (!is_health && !is_event) {
        set_error(error, error_len, "unsupported telemetry schema");
        return EN_ERR_INVALID_ARGUMENT;
    }
    memset(record, 0, sizeof(*record));
    char state[EN_MAX_ID_LEN] = {0};
    char event[EN_MAX_ID_LEN] = {0};
    char source_node[EN_MAX_ID_LEN] = {0};
    char target[EN_MAX_ID_LEN] = {0};
    int sequence = 0;
    double timestamp = 0.0;
    if (!json_string(line, "path_id", record->path_id, sizeof(record->path_id)) ||
        !valid_label(record->path_id) || !json_number(line, "timestamp_ms", &timestamp)) {
        set_error(error, error_len, "telemetry record has invalid fields");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (is_health) {
        if (!json_string(line, "state", state, sizeof(state)) ||
            !json_number(line, "rtt_ms", &record->rtt_ms) ||
            !json_number(line, "packet_loss_percent", &record->packet_loss_percent) ||
            !json_number(line, "jitter_ms", &record->jitter_ms)) {
            set_error(error, error_len, "telemetry record has invalid fields");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(state, "healthy") == 0) record->state = EN_HEALTH_HEALTHY;
        else if (strcmp(state, "degraded") == 0) record->state = EN_HEALTH_DEGRADED;
        else if (strcmp(state, "failed") == 0 || strcmp(state, "unhealthy") == 0) record->state = EN_HEALTH_FAILED;
        else {
            set_error(error, error_len, "unsupported telemetry state");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (!isfinite(record->rtt_ms) || !isfinite(record->packet_loss_percent) || !isfinite(record->jitter_ms) ||
            record->rtt_ms < 0.0 || record->packet_loss_percent < 0.0 || record->packet_loss_percent > 100.0 ||
            record->jitter_ms < 0.0) {
            set_error(error, error_len, "telemetry metrics are outside valid range");
            return EN_ERR_INVALID_ARGUMENT;
        }
        int consecutive_successes = 0;
        int consecutive_failures = 0;
        bool has_successes = strstr(line, "\"consecutive_successes\":") != NULL;
        bool has_failures = strstr(line, "\"consecutive_failures\":") != NULL;
        if ((has_successes && (!json_integer(line, "consecutive_successes", &consecutive_successes) || consecutive_successes < 0)) ||
            (has_failures && (!json_integer(line, "consecutive_failures", &consecutive_failures) || consecutive_failures < 0))) {
            set_error(error, error_len, "telemetry consecutive counts are invalid");
            return EN_ERR_INVALID_ARGUMENT;
        }
        record->consecutive_successes = has_successes ? consecutive_successes : (record->state == EN_HEALTH_HEALTHY ? 1 : 0);
        record->consecutive_failures = has_failures ? consecutive_failures : (record->state == EN_HEALTH_FAILED ? 1 : 0);
    } else if (is_path_event) {
        if (!json_string(line, "event", event, sizeof(event)) ||
            (strcmp(event, "path_failed") != 0 && strcmp(event, "path_recovered") != 0)) {
            set_error(error, error_len, "unsupported path event");
            return EN_ERR_INVALID_ARGUMENT;
        }
        record->state = strcmp(event, "path_recovered") == 0 ? EN_HEALTH_HEALTHY : EN_HEALTH_FAILED;
        record->packet_loss_percent = record->state == EN_HEALTH_FAILED ? 100.0 : 0.0;
    } else if (is_interface_event) {
        if (!json_string(line, "state", state, sizeof(state)) ||
            !json_string(line, "interface_name", record->observed_interface_name, sizeof(record->observed_interface_name)) ||
            !valid_label(record->observed_interface_name)) {
            set_error(error, error_len, "telemetry interface event has invalid fields");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(state, "up") == 0) {
            record->state = EN_HEALTH_HEALTHY;
            record->packet_loss_percent = 0.0;
        } else if (strcmp(state, "down") == 0) {
            record->state = EN_HEALTH_FAILED;
            record->packet_loss_percent = 100.0;
        } else {
            set_error(error, error_len, "unsupported interface event state");
            return EN_ERR_INVALID_ARGUMENT;
        }
        record->has_interface_observation = true;
        record->interface_state = record->state;
    } else {
        char state_text[EN_MAX_ID_LEN] = {0};
        char tunnel_id[EN_MAX_ID_LEN] = {0};
        if (!json_string(line, "state", state_text, sizeof(state_text)) ||
            !json_string(line, "tunnel_id", tunnel_id, sizeof(tunnel_id)) || !valid_label(tunnel_id)) {
            set_error(error, error_len, "telemetry event has invalid fields");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (is_tunnel_event) snprintf(record->observed_tunnel_id, sizeof(record->observed_tunnel_id), "%s", tunnel_id);
        if (strcmp(state_text, "installed") == 0 || strcmp(state_text, "rekeying") == 0 || strcmp(state_text, "up") == 0) {
            record->state = EN_HEALTH_HEALTHY;
            record->packet_loss_percent = 0.0;
        } else if (strcmp(state_text, "deleted") == 0 || strcmp(state_text, "down") == 0 || strcmp(state_text, "failed") == 0) {
            record->state = EN_HEALTH_FAILED;
            record->packet_loss_percent = 100.0;
        } else {
            set_error(error, error_len, "unsupported tunnel or route event state");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (is_route_event && strstr(line, "\"table_id\":") != NULL) {
            if (!json_integer(line, "table_id", &record->table_id)) {
                set_error(error, error_len, "telemetry route table_id is invalid");
                return EN_ERR_INVALID_ARGUMENT;
            }
            record->has_table_id = true;
        }
        if (is_route_event) {
            record->has_route_observation = true;
            bool has_destination_prefix = strstr(line, "\"destination_prefix\":\"") != NULL;
            bool has_next_hop = strstr(line, "\"next_hop\":\"") != NULL;
            if (has_destination_prefix != has_next_hop ||
                (has_destination_prefix &&
                 (!json_string(line, "destination_prefix", record->observed_destination_prefix, sizeof(record->observed_destination_prefix)) ||
                  !json_string(line, "next_hop", record->observed_next_hop, sizeof(record->observed_next_hop)) ||
                  !valid_label(record->observed_destination_prefix) || !valid_label(record->observed_next_hop)))) {
                set_error(error, error_len, "telemetry route identity is invalid");
                return EN_ERR_INVALID_ARGUMENT;
            }
            record->route_state = record->state;
        }
    }
    bool has_source = strstr(line, "\"source\":\"") != NULL;
    bool has_target = strstr(line, "\"target\":\"") != NULL;
    if (has_source && (!json_string(line, "source", source_node, sizeof(source_node)) || !valid_label(source_node))) {
        set_error(error, error_len, "telemetry source is invalid");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (has_target && (!json_string(line, "target", target, sizeof(target)) || !valid_label(target))) {
        set_error(error, error_len, "telemetry target is invalid");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (has_source) snprintf(record->source_node, sizeof(record->source_node), "%s", source_node);
    if (has_target) snprintf(record->target, sizeof(record->target), "%s", target);
    bool has_sequence = strstr(line, "\"sequence\":") != NULL;
    if (has_sequence && (!json_integer(line, "sequence", &sequence) || sequence < 0)) {
        set_error(error, error_len, "telemetry sequence is invalid");
        return EN_ERR_INVALID_ARGUMENT;
    }
    record->sequence = has_sequence ? sequence : 0;
    if (!isfinite(timestamp) || timestamp < 0.0 || timestamp > (double)LLONG_MAX) {
        set_error(error, error_len, "telemetry timestamp is outside valid range");
        return EN_ERR_INVALID_ARGUMENT;
    }
    record->last_updated_ms = (long long)timestamp;
    if (!is_health) {
        record->consecutive_successes = record->state == EN_HEALTH_HEALTHY ? 1 : 0;
        record->consecutive_failures = record->state == EN_HEALTH_FAILED ? 1 : 0;
    }
    return EN_ERR_NONE;
}

en_error_code_t en_telemetry_load_jsonl(const char *filename, en_path_health_t *health, size_t health_capacity, size_t *health_count, char *error, size_t error_len)
{
    if (filename == NULL || health == NULL || health_count == NULL || health_capacity == 0) {
        set_error(error, error_len, "invalid telemetry argument");
        return EN_ERR_INVALID_ARGUMENT;
    }
    FILE *file = en_telemetry_open_jsonl(filename);
    if (file == NULL) {
        set_error(error, error_len, "failed to open telemetry file");
        return EN_ERR_NOT_FOUND;
    }
    *health_count = 0;
    size_t total_bytes = 0;
    const size_t input_limit = 4U * 1024U * 1024U;
    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        size_t line_length = strlen(line);
        if (total_bytes > input_limit - line_length) {
            fclose(file);
            set_error(error, error_len, "telemetry input exceeds size limit");
            return EN_ERR_INVALID_ARGUMENT;
        }
        total_bytes += line_length;
        char schema[EN_MAX_ID_LEN] = {0};
        const unsigned char *line_cursor = (const unsigned char *)line;
        while (isspace(*line_cursor)) line_cursor++;
        if (*line_cursor == '\0') continue;
        if (!json_string(line, "schema", schema, sizeof(schema))) {
            fclose(file);
            set_error(error, error_len, "telemetry record has no schema");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (strcmp(schema, "ibuki.telemetry.path_health.v1") != 0 && strcmp(schema, "ibuki.event.path.v1") != 0 &&
            strcmp(schema, "ibuki.event.tunnel.v1") != 0 && strcmp(schema, "ibuki.event.vpp.route.v1") != 0 &&
            strcmp(schema, "ibuki.event.vpp.interface.v1") != 0) {
            fclose(file);
            set_error(error, error_len, "unsupported telemetry schema");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (*health_count >= health_capacity) {
            fclose(file);
            set_error(error, error_len, "too many telemetry records");
            return EN_ERR_INVALID_ARGUMENT;
        }
        en_path_health_t *record = &health[*health_count];
        if (en_telemetry_parse_json_line(line, record, error, error_len) != EN_ERR_NONE) {
            fclose(file);
            return EN_ERR_INVALID_ARGUMENT;
        }
        (*health_count)++;
    }
    fclose(file);
    if (*health_count == 0) {
        set_error(error, error_len, "no telemetry records found");
        return EN_ERR_NOT_FOUND;
    }
    return EN_ERR_NONE;
}
