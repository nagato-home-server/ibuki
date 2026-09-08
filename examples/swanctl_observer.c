#include "eventnet/strongswan_observer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s LIST_SAS_OUTPUT CHILD_ID\n", program);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "usage: %s LIST_SAS_OUTPUT CHILD_ID [--event PATH_ID]\n", argv[0]);
        return 2;
    }
    FILE *file = strcmp(argv[1], "-") == 0 ? stdin : fopen(argv[1], "rb");
    if (file == NULL) {
        perror(argv[1]);
        return 1;
    }
    size_t size = 0;
    char *output = NULL;
    if (file == stdin) {
        output = calloc(1024 * 1024 + 1, 1);
        if (output != NULL) size = fread(output, 1, 1024 * 1024, file);
        if (ferror(file)) size = 0;
    } else {
        if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 1; }
        long file_size = ftell(file);
        if (file_size < 0 || file_size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            fprintf(stderr, "invalid or oversized list-sas output\n");
            return 1;
        }
        size = (size_t)file_size;
        output = calloc(size + 1, 1);
        if (output != NULL && fread(output, 1, size, file) != size) size = 0;
    }
    if (output == NULL || size == 0) {
        free(output);
        if (file != stdin) fclose(file);
        fprintf(stderr, "failed to read list-sas output\n");
        return 1;
    }
    if (file != stdin) fclose(file);
    en_strongswan_sa_observation_t observation = {0};
    char error[128] = {0};
    en_error_code_t status = en_strongswan_parse_list_sas(output, argv[2], &observation, error, sizeof(error));
    free(output);
    if (status != EN_ERR_NONE) {
        fprintf(stderr, "observation failed: %s\n", error);
        return 1;
    }
    if (argc == 5) {
        if (strcmp(argv[3], "--event") != 0) { usage(argv[0]); return 2; }
        char event[512] = {0};
        long long timestamp_ms = (long long)time(NULL) * 1000;
        if (en_strongswan_observation_to_event_json(&observation, argv[4], timestamp_ms, event, sizeof(event)) != EN_ERR_NONE) return 1;
        fputs(event, stdout);
        return 0;
    }
    printf("child_id: %s\nstate: %s\nhealth: %s\n", observation.child_id,
        observation.state == EN_TUNNEL_ESTABLISHED ? "established" :
        observation.state == EN_TUNNEL_REKEYING ? "rekeying" :
        observation.state == EN_TUNNEL_ESTABLISHING ? "establishing" :
        observation.state == EN_TUNNEL_DELETING ? "deleting" : "failed",
        observation.health == EN_HEALTH_HEALTHY ? "healthy" :
        observation.health == EN_HEALTH_DEGRADED ? "degraded" :
        observation.health == EN_HEALTH_FAILED ? "failed" : "unknown");
    return 0;
}
