#include "eventnet/topology.h"

#include <stdio.h>
#include <string.h>

static void set_segment(en_segment_t *segment, const char *segment_id, const char *from_node, const char *to_node, const char *tunnel_id)
{
    snprintf(segment->segment_id, sizeof(segment->segment_id), "%s", segment_id);
    snprintf(segment->from_node, sizeof(segment->from_node), "%s", from_node);
    snprintf(segment->to_node, sizeof(segment->to_node), "%s", to_node);
    snprintf(segment->tunnel_id, sizeof(segment->tunnel_id), "%s", tunnel_id);
}

size_t en_initial_demo_paths(en_path_t *paths, size_t capacity)
{
    if (paths == NULL || capacity < 3) {
        return 0;
    }
    memset(paths, 0, sizeof(en_path_t) * 3);

    en_path_t *hub = &paths[0];
    snprintf(hub->path_id, sizeof(hub->path_id), "%s", "path-via-hub");
    snprintf(hub->source, sizeof(hub->source), "%s", "site-a");
    snprintf(hub->destination, sizeof(hub->destination), "%s", "site-b");
    snprintf(hub->route_destination_prefix, sizeof(hub->route_destination_prefix), "%s", "10.0.2.0/24");
    snprintf(hub->route_next_hop, sizeof(hub->route_next_hop), "%s", "203.0.113.1");
    snprintf(hub->egress_tunnel_id, sizeof(hub->egress_tunnel_id), "%s", "tun-a-hub");
    snprintf(hub->waypoints[0], sizeof(hub->waypoints[0]), "%s", "hub-1");
    hub->waypoint_count = 1;
    hub->segment_count = 2;
    hub->priority = 30;
    hub->administrative_state = EN_ADMIN_ENABLED;
    hub->operational_state = EN_PATH_UNKNOWN;
    set_segment(&hub->segments[0], "seg-a-hub", "site-a", "hub-1", "tun-a-hub");
    set_segment(&hub->segments[1], "seg-hub-b", "hub-1", "site-b", "tun-hub-b");

    en_path_t *direct = &paths[1];
    snprintf(direct->path_id, sizeof(direct->path_id), "%s", "path-direct");
    snprintf(direct->source, sizeof(direct->source), "%s", "site-a");
    snprintf(direct->destination, sizeof(direct->destination), "%s", "site-b");
    snprintf(direct->route_destination_prefix, sizeof(direct->route_destination_prefix), "%s", "10.0.2.0/24");
    snprintf(direct->route_next_hop, sizeof(direct->route_next_hop), "%s", "203.0.113.20");
    snprintf(direct->egress_tunnel_id, sizeof(direct->egress_tunnel_id), "%s", "tun-a-b");
    direct->waypoint_count = 0;
    direct->segment_count = 1;
    direct->priority = 10;
    direct->administrative_state = EN_ADMIN_ENABLED;
    direct->operational_state = EN_PATH_UNKNOWN;
    set_segment(&direct->segments[0], "seg-a-b", "site-a", "site-b", "tun-a-b");

    en_path_t *relay = &paths[2];
    snprintf(relay->path_id, sizeof(relay->path_id), "%s", "path-via-relay-c");
    snprintf(relay->source, sizeof(relay->source), "%s", "site-a");
    snprintf(relay->destination, sizeof(relay->destination), "%s", "site-b");
    snprintf(relay->route_destination_prefix, sizeof(relay->route_destination_prefix), "%s", "10.0.2.0/24");
    snprintf(relay->route_next_hop, sizeof(relay->route_next_hop), "%s", "203.0.113.30");
    snprintf(relay->egress_tunnel_id, sizeof(relay->egress_tunnel_id), "%s", "tun-a-relay-c");
    snprintf(relay->waypoints[0], sizeof(relay->waypoints[0]), "%s", "relay-c");
    relay->waypoint_count = 1;
    relay->segment_count = 2;
    relay->priority = 20;
    relay->administrative_state = EN_ADMIN_ENABLED;
    relay->operational_state = EN_PATH_UNKNOWN;
    set_segment(&relay->segments[0], "seg-a-relay-c", "site-a", "relay-c", "tun-a-relay-c");
    set_segment(&relay->segments[1], "seg-relay-c-b", "relay-c", "site-b", "tun-relay-c-b");

    return 3;
}
