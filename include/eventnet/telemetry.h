#ifndef EVENTNET_TELEMETRY_H
#define EVENTNET_TELEMETRY_H

#include "eventnet/types.h"
#include <stdio.h>

FILE *en_telemetry_open_jsonl(const char *filename);

en_error_code_t en_telemetry_parse_json_line(const char *line, en_path_health_t *health, char *error, size_t error_len);
en_error_code_t en_telemetry_load_jsonl(const char *filename, en_path_health_t *health, size_t health_capacity, size_t *health_count, char *error, size_t error_len);

#endif
