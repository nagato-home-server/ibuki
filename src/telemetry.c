#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "eventnet/telemetry.h"
#include "yyjson.h"

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

en_error_code_t en_telemetry_parse_json_line(const char *line, en_path_health_t *record, char *error, size_t error_len)
{
    if (line == NULL || record == NULL) {
        set_error(error, error_len, "invalid telemetry record argument");
        return EN_ERR_INVALID_ARGUMENT;
    }
    yyjson_doc *document = yyjson_read_opts((char *)(void *)line, strlen(line), YYJSON_READ_NOFLAG, NULL, NULL);
    yyjson_val *root = document == NULL ? NULL : yyjson_doc_get_root(document);
    if (root == NULL || !yyjson_is_obj(root)) {
        if (document != NULL) yyjson_doc_free(document);
        set_error(error, error_len, "telemetry record is not a complete JSON object");
        return EN_ERR_INVALID_ARGUMENT;
    }
    yyjson_val *schema_value = yyjson_obj_get(root, "schema");
    const char *schema = yyjson_get_str(schema_value);
    if (schema == NULL || !valid_label(schema)) {
        yyjson_doc_free(document);
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
        yyjson_doc_free(document);
        set_error(error, error_len, "unsupported telemetry schema");
        return EN_ERR_INVALID_ARGUMENT;
    }
    memset(record, 0, sizeof(*record));
    yyjson_val *path_value = yyjson_obj_get(root, "path_id");
    yyjson_val *timestamp_value = yyjson_obj_get(root, "timestamp_ms");
    const char *path_id = yyjson_get_str(path_value);
    double timestamp = yyjson_get_num(timestamp_value);
    if (path_id == NULL || !valid_label(path_id) || !yyjson_is_num(timestamp_value)) {
        yyjson_doc_free(document);
        set_error(error, error_len, "telemetry record has invalid fields");
        return EN_ERR_INVALID_ARGUMENT;
    }
    snprintf(record->path_id, sizeof(record->path_id), "%s", path_id);
    yyjson_val *state_value = yyjson_obj_get(root, "state");
    const char *state = yyjson_get_str(state_value);
    if (is_health) {
        yyjson_val *rtt_value = yyjson_obj_get(root, "rtt_ms");
        yyjson_val *loss_value = yyjson_obj_get(root, "packet_loss_percent");
        yyjson_val *jitter_value = yyjson_obj_get(root, "jitter_ms");
        if (state == NULL || !yyjson_is_num(rtt_value) || !yyjson_is_num(loss_value) || !yyjson_is_num(jitter_value)) {
            yyjson_doc_free(document);
            set_error(error, error_len, "telemetry record has invalid fields");
            return EN_ERR_INVALID_ARGUMENT;
        }
        record->rtt_ms = yyjson_get_num(rtt_value);
        record->packet_loss_percent = yyjson_get_num(loss_value);
        record->jitter_ms = yyjson_get_num(jitter_value);
        if (strcmp(state, "healthy") == 0) record->state = EN_HEALTH_HEALTHY;
        else if (strcmp(state, "degraded") == 0) record->state = EN_HEALTH_DEGRADED;
        else if (strcmp(state, "failed") == 0 || strcmp(state, "unhealthy") == 0) record->state = EN_HEALTH_FAILED;
        else {
            yyjson_doc_free(document);
            set_error(error, error_len, "unsupported telemetry state");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (!isfinite(record->rtt_ms) || !isfinite(record->packet_loss_percent) || !isfinite(record->jitter_ms) ||
            record->rtt_ms < 0.0 || record->packet_loss_percent < 0.0 || record->packet_loss_percent > 100.0 ||
            record->jitter_ms < 0.0) {
            yyjson_doc_free(document);
            set_error(error, error_len, "telemetry metrics are outside valid range");
            return EN_ERR_INVALID_ARGUMENT;
        }
        yyjson_val *success_value = yyjson_obj_get(root, "consecutive_successes");
        yyjson_val *failure_value = yyjson_obj_get(root, "consecutive_failures");
        bool has_successes = success_value != NULL;
        bool has_failures = failure_value != NULL;
        if ((has_successes && (!yyjson_is_int(success_value) || yyjson_get_sint(success_value) < 0 || yyjson_get_sint(success_value) > INT_MAX)) ||
            (has_failures && (!yyjson_is_int(failure_value) || yyjson_get_sint(failure_value) < 0 || yyjson_get_sint(failure_value) > INT_MAX))) {
            yyjson_doc_free(document);
            set_error(error, error_len, "telemetry consecutive counts are invalid");
            return EN_ERR_INVALID_ARGUMENT;
        }
        record->consecutive_successes = has_successes ? (int)yyjson_get_sint(success_value) : (record->state == EN_HEALTH_HEALTHY ? 1 : 0);
        record->consecutive_failures = has_failures ? (int)yyjson_get_sint(failure_value) : (record->state == EN_HEALTH_FAILED ? 1 : 0);
    } else if (is_path_event) {
        yyjson_val *event_value = yyjson_obj_get(root, "event");
        const char *event = yyjson_get_str(event_value);
        if (event == NULL || (strcmp(event, "path_failed") != 0 && strcmp(event, "path_recovered") != 0)) {
            yyjson_doc_free(document);
            set_error(error, error_len, "unsupported path event");
            return EN_ERR_INVALID_ARGUMENT;
        }
        record->state = strcmp(event, "path_recovered") == 0 ? EN_HEALTH_HEALTHY : EN_HEALTH_FAILED;
        record->packet_loss_percent = record->state == EN_HEALTH_FAILED ? 100.0 : 0.0;
    } else if (is_interface_event) {
        yyjson_val *interface_value = yyjson_obj_get(root, "interface_name");
        const char *interface_name = yyjson_get_str(interface_value);
        if (state == NULL || interface_name == NULL || strlen(interface_name) >= sizeof(record->observed_interface_name) ||
            !valid_label(interface_name)) {
            yyjson_doc_free(document);
            set_error(error, error_len, "telemetry interface event has invalid fields");
            return EN_ERR_INVALID_ARGUMENT;
        }
        snprintf(record->observed_interface_name, sizeof(record->observed_interface_name), "%s", interface_name);
        if (strcmp(state, "up") == 0) {
            record->state = EN_HEALTH_HEALTHY;
            record->packet_loss_percent = 0.0;
        } else if (strcmp(state, "down") == 0) {
            record->state = EN_HEALTH_FAILED;
            record->packet_loss_percent = 100.0;
        } else {
            yyjson_doc_free(document);
            set_error(error, error_len, "unsupported interface event state");
            return EN_ERR_INVALID_ARGUMENT;
        }
        record->has_interface_observation = true;
        record->interface_state = record->state;
    } else {
        yyjson_val *tunnel_value = yyjson_obj_get(root, "tunnel_id");
        const char *tunnel_id = yyjson_get_str(tunnel_value);
        if (state == NULL || tunnel_id == NULL || !valid_label(tunnel_id)) {
            yyjson_doc_free(document);
            set_error(error, error_len, "telemetry event has invalid fields");
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (is_tunnel_event) snprintf(record->observed_tunnel_id, sizeof(record->observed_tunnel_id), "%s", tunnel_id);
        if (strcmp(state, "installed") == 0 || strcmp(state, "rekeying") == 0 || strcmp(state, "up") == 0) {
            record->state = EN_HEALTH_HEALTHY;
            record->packet_loss_percent = 0.0;
        } else if (strcmp(state, "deleted") == 0 || strcmp(state, "down") == 0 || strcmp(state, "failed") == 0) {
            record->state = EN_HEALTH_FAILED;
            record->packet_loss_percent = 100.0;
        } else {
            yyjson_doc_free(document);
            set_error(error, error_len, "unsupported tunnel or route event state");
            return EN_ERR_INVALID_ARGUMENT;
        }
        yyjson_val *table_value = yyjson_obj_get(root, "table_id");
        if (is_route_event && table_value != NULL) {
            if (!yyjson_is_int(table_value) || yyjson_get_sint(table_value) < 0 || yyjson_get_sint(table_value) > INT_MAX) {
                yyjson_doc_free(document);
                set_error(error, error_len, "telemetry route table_id is invalid");
                return EN_ERR_INVALID_ARGUMENT;
            }
            record->table_id = (int)yyjson_get_sint(table_value);
            record->has_table_id = true;
        }
        if (is_route_event) {
            record->has_route_observation = true;
            yyjson_val *destination_value = yyjson_obj_get(root, "destination_prefix");
            yyjson_val *next_hop_value = yyjson_obj_get(root, "next_hop");
            const char *destination_prefix = yyjson_get_str(destination_value);
            const char *next_hop = yyjson_get_str(next_hop_value);
            bool has_destination_prefix = destination_value != NULL;
            bool has_next_hop = next_hop_value != NULL;
            if (has_destination_prefix != has_next_hop ||
                (has_destination_prefix &&
                 (destination_prefix == NULL || next_hop == NULL || strlen(destination_prefix) >= sizeof(record->observed_destination_prefix) ||
                  strlen(next_hop) >= sizeof(record->observed_next_hop) || !valid_label(destination_prefix) || !valid_label(next_hop)))) {
                yyjson_doc_free(document);
                set_error(error, error_len, "telemetry route identity is invalid");
                return EN_ERR_INVALID_ARGUMENT;
            }
            if (has_destination_prefix) {
                snprintf(record->observed_destination_prefix, sizeof(record->observed_destination_prefix), "%s", destination_prefix);
                snprintf(record->observed_next_hop, sizeof(record->observed_next_hop), "%s", next_hop);
            }
            record->route_state = record->state;
        }
    }
    yyjson_val *source_value = yyjson_obj_get(root, "source");
    yyjson_val *target_value = yyjson_obj_get(root, "target");
    const char *source_node = yyjson_get_str(source_value);
    const char *target = yyjson_get_str(target_value);
    bool has_source = source_value != NULL;
    bool has_target = target_value != NULL;
    if (has_source && (source_node == NULL || strlen(source_node) >= sizeof(record->source_node) || !valid_label(source_node))) {
        yyjson_doc_free(document);
        set_error(error, error_len, "telemetry source is invalid");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (has_target && (target == NULL || strlen(target) >= sizeof(record->target) || !valid_label(target))) {
        yyjson_doc_free(document);
        set_error(error, error_len, "telemetry target is invalid");
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (has_source) snprintf(record->source_node, sizeof(record->source_node), "%s", source_node);
    if (has_target) snprintf(record->target, sizeof(record->target), "%s", target);
    yyjson_val *sequence_value = yyjson_obj_get(root, "sequence");
    bool has_sequence = sequence_value != NULL;
    if (has_sequence && (!yyjson_is_int(sequence_value) || yyjson_get_sint(sequence_value) < 0 || yyjson_get_sint(sequence_value) > INT_MAX)) {
        yyjson_doc_free(document);
        set_error(error, error_len, "telemetry sequence is invalid");
        return EN_ERR_INVALID_ARGUMENT;
    }
    record->sequence = has_sequence ? (int)yyjson_get_sint(sequence_value) : 0;
    if (!isfinite(timestamp) || timestamp < 0.0 || timestamp > (double)LLONG_MAX) {
        yyjson_doc_free(document);
        set_error(error, error_len, "telemetry timestamp is outside valid range");
        return EN_ERR_INVALID_ARGUMENT;
    }
    record->last_updated_ms = (long long)timestamp;
    if (!is_health) {
        record->consecutive_successes = record->state == EN_HEALTH_HEALTHY ? 1 : 0;
        record->consecutive_failures = record->state == EN_HEALTH_FAILED ? 1 : 0;
    }
    yyjson_doc_free(document);
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
        const unsigned char *line_cursor = (const unsigned char *)line;
        while (isspace(*line_cursor)) line_cursor++;
        if (*line_cursor == '\0') continue;
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
