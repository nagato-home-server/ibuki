#ifndef EVENTNET_STRONGSWAN_OBSERVER_H
#define EVENTNET_STRONGSWAN_OBSERVER_H

#include "eventnet/types.h"

typedef struct {
    char child_id[EN_MAX_ID_LEN];
    en_tunnel_state_t state;
    en_health_state_t health;
} en_strongswan_sa_observation_t;

en_error_code_t en_strongswan_parse_list_sas(
    const char *output,
    const char *child_id,
    en_strongswan_sa_observation_t *observation,
    char *error,
    size_t error_len
);

en_error_code_t en_strongswan_observation_to_event_json(
    const en_strongswan_sa_observation_t *observation,
    const char *path_id,
    long long timestamp_ms,
    char *output,
    size_t output_len
);

#endif
