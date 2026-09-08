#ifndef EVENTNET_VPP_OBSERVER_H
#define EVENTNET_VPP_OBSERVER_H

#include "eventnet/types.h"

typedef struct {
    char destination_prefix[EN_MAX_ID_LEN];
    char next_hop[EN_MAX_ID_LEN];
    char interface_name[EN_MAX_ID_LEN];
    int table_id;
    bool present;
} en_vpp_route_observation_t;

typedef struct {
    char interface_name[EN_MAX_ID_LEN];
    bool present;
    bool up;
} en_vpp_interface_observation_t;

en_error_code_t en_vpp_parse_show_ip_fib(
    const char *output,
    const char *destination_prefix,
    en_vpp_route_observation_t *observation,
    char *error,
    size_t error_len
);

en_error_code_t en_vpp_parse_show_ip_fib_in_table(
    const char *output,
    const char *destination_prefix,
    int table_id,
    en_vpp_route_observation_t *observation,
    char *error,
    size_t error_len
);

en_error_code_t en_vpp_parse_show_interface(
    const char *output,
    const char *interface_name,
    en_vpp_interface_observation_t *observation,
    char *error,
    size_t error_len
);

en_error_code_t en_vpp_route_observation_to_event_json(
    const en_vpp_route_observation_t *observation,
    const char *path_id,
    const char *route_id,
    long long timestamp_ms,
    char *output,
    size_t output_len
);

en_error_code_t en_vpp_interface_observation_to_event_json(
    const en_vpp_interface_observation_t *observation,
    const char *path_id,
    long long timestamp_ms,
    char *output,
    size_t output_len
);

#endif
