#include "eventnet/vpp_observer.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int parse_table_id(const char *text, int *table_id)
{
    char *end = NULL;
    long parsed;
    if (text == NULL || table_id == NULL || text[0] == '\0') return 0;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0 || parsed > INT_MAX) return 0;
    *table_id = (int)parsed;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 6 && argc != 8) {
        fprintf(stderr, "usage: %s SHOW_IP_FIB_OUTPUT DESTINATION_PREFIX [--event PATH_ID ROUTE_ID [--table TABLE_ID]]\n", argv[0]);
        return 2;
    }
    FILE *file = strcmp(argv[1], "-") == 0 ? stdin : fopen(argv[1], "rb");
    if (file == NULL) return 1;
    size_t size = 0;
    char *output = NULL;
    if (file == stdin) {
        output = calloc(1024 * 1024 + 1, 1);
        if (output != NULL) size = fread(output, 1, 1024 * 1024, file);
        if (ferror(file)) size = 0;
    } else {
        if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 1; }
        long file_size = ftell(file);
        if (file_size < 0 || file_size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 1; }
        size = (size_t)file_size;
        output = calloc(size + 1, 1);
        if (output != NULL && fread(output, 1, size, file) != size) size = 0;
        fclose(file);
    }
    if (output == NULL || size == 0) { free(output); fprintf(stderr, "failed to read show-ip-fib output\n"); return 1; }
    en_vpp_route_observation_t observation = {0};
    char error[128] = {0};
    int table_id = -1;
    if (argc == 8 && (strcmp(argv[6], "--table") != 0 || !parse_table_id(argv[7], &table_id))) {
        free(output);
        fprintf(stderr, "invalid table id\n");
        return 2;
    }
    en_error_code_t status = en_vpp_parse_show_ip_fib_in_table(output, argv[2], table_id, &observation, error, sizeof(error));
    free(output);
    if (status != EN_ERR_NONE) { fprintf(stderr, "observation failed: %s\n", error); return 1; }
    if (argc == 6 || argc == 8) {
        if (strcmp(argv[3], "--event") != 0) return 2;
        char event[512] = {0};
        if (en_vpp_route_observation_to_event_json(&observation, argv[4], argv[5], (long long)time(NULL) * 1000, event, sizeof(event)) != EN_ERR_NONE) return 1;
        fputs(event, stdout);
        return 0;
    }
    printf("destination_prefix: %s\npresent: %s\nnext_hop: %s\ninterface: %s\n", observation.destination_prefix,
        observation.present ? "yes" : "no", observation.next_hop, observation.interface_name);
    if (observation.table_id >= 0) printf("table_id: %d\n", observation.table_id);
    return 0;
}
