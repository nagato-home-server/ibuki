#ifndef EVENTNET_STRONGSWAN_VICI_CLIENT_H
#define EVENTNET_STRONGSWAN_VICI_CLIENT_H

#include "eventnet/types.h"

typedef struct en_strongswan_vici_client en_strongswan_vici_client_t;
typedef void (*en_strongswan_vici_event_fn)(void *context, const char *event_json);
typedef bool (*en_strongswan_vici_stop_fn)(void *context);

en_error_code_t en_strongswan_vici_client_open(
    en_strongswan_vici_client_t **client,
    const char *uri,
    char *error,
    size_t error_len
);
en_error_code_t en_strongswan_vici_client_version(
    en_strongswan_vici_client_t *client,
    char *output,
    size_t output_len,
    char *error,
    size_t error_len
);
en_error_code_t en_strongswan_vici_client_initiate(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    char *error,
    size_t error_len
);
en_error_code_t en_strongswan_vici_client_terminate(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    char *error,
    size_t error_len
);
en_error_code_t en_strongswan_vici_client_observe_child(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    en_strongswan_sa_observation_t *observation,
    char *error,
    size_t error_len
);
en_error_code_t en_strongswan_vici_client_monitor_child(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    const char *path_id,
    long long duration_ms,
    en_strongswan_vici_event_fn event_fn,
    void *event_context,
    char *error,
    size_t error_len
);
en_error_code_t en_strongswan_vici_client_monitor_child_until(
    en_strongswan_vici_client_t *client,
    const char *child_id,
    const char *path_id,
    long long duration_ms,
    en_strongswan_vici_event_fn event_fn,
    void *event_context,
    en_strongswan_vici_stop_fn stop_fn,
    void *stop_context,
    char *error,
    size_t error_len
);
void en_strongswan_vici_client_close(en_strongswan_vici_client_t *client);

#endif
