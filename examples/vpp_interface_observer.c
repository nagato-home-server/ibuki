#include "eventnet/vpp_observer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *read_output(FILE *file)
{
    size_t used = 0;
    size_t capacity = 4096;
    char *output = calloc(capacity, 1);
    if (output == NULL) return NULL;
    while (!feof(file)) {
        if (used + 1 >= capacity) {
            size_t next_capacity = capacity * 2;
            char *expanded = realloc(output, next_capacity);
            if (expanded == NULL || next_capacity > 1024 * 1024 + 1) {
                free(expanded == NULL ? output : expanded);
                return NULL;
            }
            output = expanded;
            memset(output + capacity, 0, next_capacity - capacity);
            capacity = next_capacity;
        }
        size_t received = fread(output + used, 1, capacity - used - 1, file);
        used += received;
        if (ferror(file)) {
            free(output);
            return NULL;
        }
    }
    return used == 0 ? (free(output), NULL) : output;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "usage: %s SHOW_INTERFACE_OUTPUT INTERFACE [--event PATH_ID]\n", argv[0]);
        return 2;
    }
    FILE *file = strcmp(argv[1], "-") == 0 ? stdin : fopen(argv[1], "rb");
    if (file == NULL) return 1;
    char *output = read_output(file);
    if (file != stdin) fclose(file);
    if (output == NULL) {
        fprintf(stderr, "failed to read show-interface output\n");
        return 1;
    }
    en_vpp_interface_observation_t observation = {0};
    char error[128] = {0};
    en_error_code_t status = en_vpp_parse_show_interface(output, argv[2], &observation, error, sizeof(error));
    free(output);
    if (status == EN_ERR_NOT_FOUND && argc == 5) {
        memset(&observation, 0, sizeof(observation));
        snprintf(observation.interface_name, sizeof(observation.interface_name), "%s", argv[2]);
    } else if (status != EN_ERR_NONE) {
        fprintf(stderr, "observation failed: %s\n", error);
        return 1;
    }
    if (argc == 5) {
        if (strcmp(argv[3], "--event") != 0) return 2;
        char event[512] = {0};
        if (en_vpp_interface_observation_to_event_json(&observation, argv[4], (long long)time(NULL) * 1000, event, sizeof(event)) != EN_ERR_NONE) return 1;
        fputs(event, stdout);
        return 0;
    }
    printf("interface: %s\npresent: %s\nup: %s\n", observation.interface_name,
        observation.present ? "yes" : "no", observation.up ? "yes" : "no");
    return 0;
}
