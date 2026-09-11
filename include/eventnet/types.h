#ifndef EVENTNET_TYPES_H
#define EVENTNET_TYPES_H

#include <stdbool.h>
#include <stddef.h>

#define EN_MAX_ID_LEN 64
#define EN_MAX_ENDPOINTS 8
#define EN_MAX_CAPABILITIES 8
#define EN_MAX_NODES 16
#define EN_MAX_PATHS 16
#define EN_MAX_SEGMENTS 8
#define EN_MAX_TUNNELS 32
#define EN_MAX_CANDIDATES 16
#define EN_MAX_EVENTS 256
#define EN_MAX_ERRORS 64
#define EN_MAX_WAYPOINTS 8
#define EN_MAX_COMPARISONS 8
#define EN_MAX_VPP_EDGES 16
#define EN_MAX_ALLOWED_VLANS 32
#define EN_MAX_ROUTES 16
#define EN_MAX_TRAFFIC_KEY_LEN (EN_MAX_ID_LEN * 2 + 24)

typedef enum {
    EN_ADMIN_ENABLED = 0,
    EN_ADMIN_DISABLED = 1
} en_admin_state_t;

typedef enum {
    EN_TUNNEL_ABSENT = 0,
    EN_TUNNEL_CONFIGURED,
    EN_TUNNEL_ESTABLISHING,
    EN_TUNNEL_ESTABLISHED,
    EN_TUNNEL_REKEYING,
    EN_TUNNEL_DEGRADED,
    EN_TUNNEL_FAILED,
    EN_TUNNEL_DELETING,
    EN_TUNNEL_UNKNOWN
} en_tunnel_state_t;

typedef enum {
    EN_PATH_UNAVAILABLE = 0,
    EN_PATH_PREPARING,
    EN_PATH_READY,
    EN_PATH_ACTIVE,
    EN_PATH_STANDBY,
    EN_PATH_DEGRADED,
    EN_PATH_FAILED,
    EN_PATH_DRAINING,
    EN_PATH_UNKNOWN
} en_path_state_t;

typedef enum {
    EN_HEALTH_UNKNOWN = 0,
    EN_HEALTH_HEALTHY,
    EN_HEALTH_DEGRADED,
    EN_HEALTH_UNHEALTHY,
    EN_HEALTH_FAILED
} en_health_state_t;

typedef enum {
    EN_TRANSITION_IDLE = 0,
    EN_TRANSITION_CANDIDATE_SELECTED,
    EN_TRANSITION_PREPARING,
    EN_TRANSITION_VALIDATING,
    EN_TRANSITION_READY,
    EN_TRANSITION_PAUSING,
    EN_TRANSITION_DRAINING,
    EN_TRANSITION_SWITCHING,
    EN_TRANSITION_VERIFYING,
    EN_TRANSITION_COMPLETED,
    EN_TRANSITION_ROLLING_BACK,
    EN_TRANSITION_FAILED
} en_transition_state_t;

typedef enum {
    EN_ERR_NONE = 0,
    EN_ERR_TUNNEL_ESTABLISH_TIMEOUT,
    EN_ERR_PATH_VALIDATION_FAILED,
    EN_ERR_FORWARDING_UPDATE_FAILED,
    EN_ERR_STATE_CONFLICT,
    EN_ERR_ROLLBACK_FAILED,
    EN_ERR_OBSERVATION_TIMEOUT,
    EN_ERR_INVALID_ARGUMENT,
    EN_ERR_NOT_FOUND,
    EN_ERR_NO_CANDIDATE
} en_error_code_t;

typedef enum {
    EN_SELECT_EXPLICIT = 0,
    EN_SELECT_PRIORITY,
    EN_SELECT_EVALUATED
} en_path_selection_mode_t;

typedef enum {
    EN_TRANSITION_IMMEDIATE = 0,
    EN_TRANSITION_GRACEFUL,
    EN_TRANSITION_FLOW_PRESERVE
} en_transition_strategy_t;

typedef enum {
    EN_COMPARE_PACKET_LOSS = 0,
    EN_COMPARE_LATENCY,
    EN_COMPARE_HOP_COUNT,
    EN_COMPARE_ADMIN_PRIORITY,
    EN_COMPARE_PATH_ID
} en_comparison_key_t;

typedef struct {
    char node_id[EN_MAX_ID_LEN];
    char role[EN_MAX_ID_LEN];
    char endpoints[EN_MAX_ENDPOINTS][EN_MAX_ID_LEN];
    size_t endpoint_count;
    char capabilities[EN_MAX_CAPABILITIES][EN_MAX_ID_LEN];
    size_t capability_count;
    en_admin_state_t administrative_state;
} en_node_t;

typedef struct {
    char tunnel_id[EN_MAX_ID_LEN];
    char tunnel_type[EN_MAX_ID_LEN];
    char local_node[EN_MAX_ID_LEN];
    char remote_node[EN_MAX_ID_LEN];
    char local_endpoint[EN_MAX_ID_LEN];
    char remote_endpoint[EN_MAX_ID_LEN];
    char local_id[EN_MAX_ID_LEN];
    char remote_id[EN_MAX_ID_LEN];
    char psk[EN_MAX_ID_LEN];
    char auth_method[EN_MAX_ID_LEN];
    char local_cert[EN_MAX_ID_LEN];
    char remote_cacerts[EN_MAX_ID_LEN];
    char local_traffic_selector[EN_MAX_ID_LEN];
    char remote_traffic_selector[EN_MAX_ID_LEN];
    char gre_interface[EN_MAX_ID_LEN];
    char gre_outer_local_endpoint[EN_MAX_ID_LEN];
    char gre_outer_remote_endpoint[EN_MAX_ID_LEN];
    char gre_local_address[EN_MAX_ID_LEN];
    char gre_remote_address[EN_MAX_ID_LEN];
    int gre_instance;
    int gre_mtu;
    int vpp_local_sa_id;
    int vpp_remote_sa_id;
    int vpp_local_spi;
    int vpp_remote_spi;
    char vpp_crypto_algorithm[EN_MAX_ID_LEN];
    char vpp_crypto_key[EN_MAX_ID_LEN];
    char vpp_integrity_algorithm[EN_MAX_ID_LEN];
    char vpp_integrity_key[EN_MAX_ID_LEN];
    char protocol[EN_MAX_ID_LEN];
    en_tunnel_state_t state;
    en_health_state_t health;
} en_tunnel_t;

typedef struct {
    char segment_id[EN_MAX_ID_LEN];
    char from_node[EN_MAX_ID_LEN];
    char to_node[EN_MAX_ID_LEN];
    char tunnel_id[EN_MAX_ID_LEN];
} en_segment_t;

typedef struct {
    char route_id[EN_MAX_ID_LEN];
    char node_id[EN_MAX_ID_LEN];
    char destination_prefix[EN_MAX_ID_LEN];
    char next_hop[EN_MAX_ID_LEN];
    char interface_name[EN_MAX_ID_LEN];
    int table_id;
    int metric;
} en_route_t;

typedef struct {
    char path_id[EN_MAX_ID_LEN];
    char source[EN_MAX_ID_LEN];
    char destination[EN_MAX_ID_LEN];
    char route_destination_prefix[EN_MAX_ID_LEN];
    char route_next_hop[EN_MAX_ID_LEN];
    char egress_tunnel_id[EN_MAX_ID_LEN];
    char waypoints[EN_MAX_WAYPOINTS][EN_MAX_ID_LEN];
    size_t waypoint_count;
    en_route_t routes[EN_MAX_ROUTES];
    size_t route_count;
    bool routes_explicit;
    en_segment_t segments[EN_MAX_SEGMENTS];
    size_t segment_count;
    int priority;
    en_admin_state_t administrative_state;
    en_path_state_t operational_state;
} en_path_t;

typedef struct {
    char source[EN_MAX_ID_LEN];
    char destination[EN_MAX_ID_LEN];
    bool has_vlan_id;
    int vlan_id;
} en_traffic_selector_t;

typedef struct {
    bool has_max_rtt_ms;
    double max_rtt_ms;
    bool has_max_packet_loss_percent;
    double max_packet_loss_percent;
    bool has_hysteresis_percent;
    double hysteresis_percent;
    int failure_threshold;
    int recovery_threshold;
    int hold_down_ms;
    char forbidden_waypoints[EN_MAX_WAYPOINTS][EN_MAX_ID_LEN];
    size_t forbidden_waypoint_count;
    char required_waypoints[EN_MAX_WAYPOINTS][EN_MAX_ID_LEN];
    size_t required_waypoint_count;
    char required_capabilities[EN_MAX_CAPABILITIES][EN_MAX_ID_LEN];
    size_t required_capability_count;
} en_path_constraints_t;

typedef struct {
    en_path_selection_mode_t mode;
    char path_id[EN_MAX_ID_LEN];
    char candidates[EN_MAX_CANDIDATES][EN_MAX_ID_LEN];
    size_t candidate_count;
    en_path_constraints_t constraints;
    en_comparison_key_t comparison_order[EN_MAX_COMPARISONS];
    size_t comparison_count;
} en_path_selection_t;

typedef struct {
    en_transition_strategy_t strategy;
    int max_pause_ms;
    int drain_timeout_ms;
    int timeout_ms;
    int retry_count;
    int retry_backoff_ms;
} en_transition_policy_t;

typedef struct {
    bool enabled;
    char path_id[EN_MAX_ID_LEN];
} en_fallback_policy_t;

typedef struct {
    char intent_id[EN_MAX_ID_LEN];
    en_traffic_selector_t traffic;
    bool block_non_ipsec;
    bool deny_unmatched_vlan;
    en_path_selection_t path_selection;
    en_transition_policy_t transition;
    en_fallback_policy_t fallback;
} en_intent_t;

typedef struct {
    char node_id[EN_MAX_ID_LEN];
    char port_id[EN_MAX_ID_LEN];
    char host_interface[EN_MAX_ID_LEN];
    char vpp_interface[EN_MAX_ID_LEN];
    char namespace_interface[EN_MAX_ID_LEN];
    char namespace_address[EN_MAX_ID_LEN];
    char vpp_address[EN_MAX_ID_LEN];
    char next_hop[EN_MAX_ID_LEN];
    int allowed_vlans[EN_MAX_ALLOWED_VLANS];
    size_t allowed_vlan_count;
} en_vpp_edge_t;

typedef struct {
    char path_id[EN_MAX_ID_LEN];
    char source_node[EN_MAX_ID_LEN];
    char target[EN_MAX_ID_LEN];
    int sequence;
    en_health_state_t state;
    double rtt_ms;
    double packet_loss_percent;
    double jitter_ms;
    int consecutive_failures;
    int consecutive_successes;
    bool has_table_id;
    int table_id;
    char observed_destination_prefix[EN_MAX_ID_LEN];
    char observed_next_hop[EN_MAX_ID_LEN];
    char observed_tunnel_id[EN_MAX_ID_LEN];
    char observed_interface_name[EN_MAX_ID_LEN];
    bool has_route_observation;
    bool has_interface_observation;
    en_health_state_t route_state;
    en_health_state_t interface_state;
    long long last_updated_ms;
} en_path_health_t;

typedef struct {
    char event_type[EN_MAX_ID_LEN];
    char message[128];
    char ref_id[EN_MAX_ID_LEN];
    long long timestamp_ms;
} en_audit_event_t;

typedef struct {
    en_error_code_t code;
    char message[128];
} en_error_t;

typedef struct {
    char selected_path[EN_MAX_ID_LEN];
    char candidates[EN_MAX_CANDIDATES][EN_MAX_ID_LEN];
    size_t candidate_count;
    char excluded_path_ids[EN_MAX_CANDIDATES][EN_MAX_ID_LEN];
    char excluded_reasons[EN_MAX_CANDIDATES][128];
    size_t excluded_count;
    char reason[128];
} en_selection_result_t;

typedef struct {
    char intent_id[EN_MAX_ID_LEN];
    char selected_path[EN_MAX_ID_LEN];
    en_transition_state_t transition_state;
    en_selection_result_t explanation;
} en_reconcile_result_t;

size_t en_path_hop_count(const en_path_t *path);
const char *en_health_state_name(en_health_state_t state);
const char *en_transition_state_name(en_transition_state_t state);
const char *en_error_code_name(en_error_code_t code);

#endif
