#ifndef EVENTNET_INTERNAL_H
#define EVENTNET_INTERNAL_H

#include "eventnet/controller.h"

typedef struct {
    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN];
    char intent_id[EN_MAX_ID_LEN];
    en_transition_state_t state;
} en_transition_status_t;

typedef struct {
    en_node_t nodes[EN_MAX_NODES];
    size_t node_count;
    en_path_t paths[EN_MAX_PATHS];
    size_t path_count;
    en_intent_t intents[EN_MAX_CANDIDATES];
    size_t intent_count;
    en_tunnel_t desired_tunnels[EN_MAX_TUNNELS];
    size_t desired_tunnel_count;
    en_tunnel_t observed_tunnels[EN_MAX_TUNNELS];
    size_t observed_tunnel_count;
    en_path_health_t health[EN_MAX_PATHS];
    size_t health_count;
    char traffic_keys[EN_MAX_CANDIDATES][EN_MAX_TRAFFIC_KEY_LEN];
    char applied_paths[EN_MAX_CANDIDATES][EN_MAX_ID_LEN];
    long long applied_since_ms[EN_MAX_CANDIDATES];
    bool applied_verified[EN_MAX_CANDIDATES];
    size_t applied_count;
    en_transition_status_t transition_statuses[EN_MAX_CANDIDATES];
    size_t transition_status_count;
    en_error_t errors[EN_MAX_ERRORS];
    size_t error_count;
} en_controller_state_t;

struct en_controller {
    en_controller_state_t state;
    en_audit_event_t audit_events[EN_MAX_EVENTS];
    size_t audit_count;
    en_strongswan_adapter_t strongswan;
    en_vpp_adapter_t vpp;
    en_health_probe_t health_probe;
};

long long en_now_ms(void);
unsigned long long en_monotonic_ns(void);
void en_copy_id(char *dst, size_t dst_len, const char *src);
bool en_streq(const char *left, const char *right);
bool en_transition_status_update(en_controller_t *controller, const char *traffic_key, const char *intent_id, en_transition_state_t state);
bool en_transition_status_get(const en_controller_t *controller, const char *traffic_key, const char *intent_id, en_transition_state_t *state);
void en_audit_append(en_controller_t *controller, const char *event_type, const char *message, const char *ref_id);
void en_error_append(en_controller_t *controller, en_error_code_t code, const char *message);
en_path_t *en_find_path(en_controller_t *controller, const char *path_id);
en_path_health_t *en_find_health(en_controller_t *controller, const char *path_id);
en_tunnel_t *en_find_tunnel(en_controller_t *controller, const char *tunnel_id);
en_tunnel_t *en_find_desired_tunnel(en_controller_t *controller, const char *tunnel_id);
const en_node_t *en_find_node(const en_controller_t *controller, const char *node_id);
const char *en_get_applied_path(en_controller_t *controller, const char *traffic_key);
long long en_get_applied_since_ms(en_controller_t *controller, const char *traffic_key);
void en_set_applied_path(en_controller_t *controller, const char *traffic_key, const char *path_id);
bool en_applied_path_verified(en_controller_t *controller, const char *traffic_key);
void en_invalidate_applied_path(en_controller_t *controller, const char *traffic_key);
void en_clear_applied_path(en_controller_t *controller, const char *traffic_key);
en_error_code_t en_select_path(en_controller_t *controller, const en_intent_t *intent, en_selection_result_t *result);
en_error_code_t en_transition_path(en_controller_t *controller, const en_intent_t *intent, en_path_t *target_path, en_transition_metrics_t *metrics);

#endif
