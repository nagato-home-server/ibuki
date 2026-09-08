#include "eventnet/controller.h"
#include "eventnet/mock_adapters.h"
#include "eventnet/yaml_config.h"

#include <stdio.h>
#include <string.h>

static const en_path_t *find_path(const en_yaml_config_t *config, const char *path_id)
{
    for (size_t i = 0; i < config->path_count; i++) {
        if (strcmp(config->paths[i].path_id, path_id) == 0) {
            return &config->paths[i];
        }
    }
    return NULL;
}

static const en_tunnel_t *find_tunnel(const en_yaml_config_t *config, const char *tunnel_id)
{
    for (size_t i = 0; i < config->tunnel_count; i++) {
        if (strcmp(config->tunnels[i].tunnel_id, tunnel_id) == 0) {
            return &config->tunnels[i];
        }
    }
    return NULL;
}

static const en_intent_t *find_intent(const en_yaml_config_t *config, const char *intent_id)
{
    if (intent_id == NULL) {
        return config->intent_count == 0 ? NULL : &config->intents[0];
    }
    for (size_t i = 0; i < config->intent_count; i++) {
        if (strcmp(config->intents[i].intent_id, intent_id) == 0) {
            return &config->intents[i];
        }
    }
    return NULL;
}

static const en_vpp_edge_t *find_vpp_edge(const en_yaml_config_t *config, const char *node_id)
{
    for (size_t i = 0; i < config->vpp_edge_count; i++) {
        if (strcmp(config->vpp_edges[i].node_id, node_id) == 0) {
            return &config->vpp_edges[i];
        }
    }
    return NULL;
}

static const en_vpp_edge_t *find_vpp_edge_for_route(
    const en_yaml_config_t *config, const en_route_t *route)
{
    if (config == NULL || route == NULL) return NULL;
    for (size_t index = 0; index < config->vpp_edge_count; index++) {
        const en_vpp_edge_t *edge = &config->vpp_edges[index];
        if (strcmp(edge->node_id, route->node_id) != 0) continue;
        if (route->interface_name[0] == '\0' ||
            strcmp(route->interface_name, edge->vpp_interface) == 0 ||
            strcmp(route->interface_name, edge->host_interface) == 0) return edge;
    }
    return NULL;
}

static bool vpp_edge_allows_vlan(const en_vpp_edge_t *edge, int vlan_id)
{
    if (edge == NULL || edge->allowed_vlan_count == 0) return true;
    for (size_t index = 0; index < edge->allowed_vlan_count; index++) {
        if (edge->allowed_vlans[index] == vlan_id) return true;
    }
    return false;
}

static bool path_uses_node(const en_path_t *path, const char *node_id)
{
    if (path == NULL || node_id == NULL || node_id[0] == '\0') return false;
    if (strcmp(path->source, node_id) == 0 || strcmp(path->destination, node_id) == 0) return true;
    for (size_t index = 0; index < path->waypoint_count; index++) {
        if (strcmp(path->waypoints[index], node_id) == 0) return true;
    }
    for (size_t index = 0; index < path->segment_count; index++) {
        if (strcmp(path->segments[index].from_node, node_id) == 0 ||
            strcmp(path->segments[index].to_node, node_id) == 0) return true;
    }
    for (size_t index = 0; index < path->route_count; index++) {
        if (strcmp(path->routes[index].node_id, node_id) == 0) return true;
    }
    return false;
}

static bool path_has_conflicting_tables(const en_path_t *path)
{
    if (path == NULL) return true;
    for (size_t index = 0; index < path->route_count; index++) {
        const en_route_t *route = &path->routes[index];
        if (route->table_id < 0) continue;
        for (size_t previous = 0; previous < index; previous++) {
            const en_route_t *prior = &path->routes[previous];
            if (prior->table_id >= 0 && strcmp(prior->node_id, route->node_id) == 0 && prior->table_id != route->table_id) return true;
        }
    }
    return false;
}

static const char *runtime_kind(const en_yaml_config_t *config, const en_path_t *path)
{
    if (path->segment_count == 0 && !path->routes_explicit &&
        path->route_destination_prefix[0] != '\0' && path->route_next_hop[0] != '\0') {
        return "vpp";
    }
    if (path->segment_count == 1 && strcmp(path->segments[0].tunnel_id, "tun-a-b") == 0) {
        return "direct";
    }
    if (
        path->segment_count == 2 &&
        strcmp(path->segments[0].tunnel_id, "tun-a-hub") == 0 &&
        strcmp(path->segments[1].tunnel_id, "tun-hub-b") == 0
    ) {
        return "hub";
    }
    if (config->vpp_edge_count > 0 && path->segment_count > 0) {
        return "vpp";
    }
    return "unsupported";
}

static void write_xfrm_block_runtime(FILE *file, const en_yaml_config_t *config, const en_path_t *path)
{
    for (size_t index = 0; index < path->segment_count; index++) {
        const en_segment_t *segment = &path->segments[index];
        const en_tunnel_t *tunnel = find_tunnel(config, segment->tunnel_id);
        if (tunnel == NULL) continue;
        fprintf(file, "ip netns exec %s ip xfrm policy add dir out src %s dst %s priority 10000 action block\n",
            segment->from_node, tunnel->local_traffic_selector, tunnel->remote_traffic_selector);
        fprintf(file, "ip netns exec %s ip xfrm policy add dir in src %s dst %s priority 10000 action block\n",
            segment->from_node, tunnel->remote_traffic_selector, tunnel->local_traffic_selector);
        fprintf(file, "ip netns exec %s ip xfrm policy add dir out src %s dst %s priority 10000 action block\n",
            segment->to_node, tunnel->remote_traffic_selector, tunnel->local_traffic_selector);
        fprintf(file, "ip netns exec %s ip xfrm policy add dir in src %s dst %s priority 10000 action block\n",
            segment->to_node, tunnel->local_traffic_selector, tunnel->remote_traffic_selector);
    }
}

static int write_apply_script(const char *filename, const en_yaml_config_t *config, const en_intent_t *intent, const en_path_t *path, const char *kind)
{
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        return 1;
    }
    fprintf(file, "#!/usr/bin/env sh\n");
    fprintf(file, "set -eu\n\n");
    fprintf(file, "ROOT_DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")/../..\" && pwd)\n");
    fprintf(file, "cd \"$ROOT_DIR\"\n\n");
    fprintf(file, "ROLLBACK_SCRIPT=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)/rollback-selected.sh\n");
    fprintf(file, "rollback_on_error() {\n");
    fprintf(file, "  status=$?\n");
    fprintf(file, "  if [ \"$status\" -ne 0 ]; then sh \"$ROLLBACK_SCRIPT\" || true; fi\n");
    fprintf(file, "  exit \"$status\"\n");
    fprintf(file, "}\n");
    fprintf(file, "trap rollback_on_error EXIT\n\n");
    fprintf(file, "printf 'eventnet selected path: %s\\n'\n", path->path_id);
    fprintf(file, "printf 'eventnet runtime kind: %s\\n'\n\n", kind);
    if (strcmp(kind, "direct") == 0) {
        fprintf(file, "sudo sh scripts/vm-netns-ipsec-hub-stop.sh 2>/dev/null || true\n");
        fprintf(file, "sudo sh scripts/vm-netns-ipsec-direct-start.sh\n");
        fprintf(file, "sudo sh scripts/vm-netns-ipsec-direct-smoke.sh\n");
    } else if (strcmp(kind, "hub") == 0) {
        fprintf(file, "sudo sh scripts/vm-netns-ipsec-direct-stop.sh 2>/dev/null || true\n");
        fprintf(file, "sudo sh scripts/vm-netns-ipsec-hub-start.sh\n");
        fprintf(file, "sudo sh scripts/vm-netns-ipsec-hub-smoke.sh\n");
    } else if (strcmp(kind, "vpp") == 0) {
        fprintf(file, "sudo sh scripts/vm-vpp-netns-setup.sh\n");
        fprintf(file, "DRY_RUN=0 sh out/netns-runtime/vpp-netns-route-plan.sh\n");
    } else {
        fprintf(file, "echo 'unsupported path for current netns runtime: %s' >&2\n", path->path_id);
        fprintf(file, "exit 1\n");
    }
    if (intent->block_non_ipsec) {
        fprintf(file, "\n# Block cleartext traffic outside the selected IPsec selectors.\n");
        write_xfrm_block_runtime(file, config, path);
    }
    fprintf(file, "\nif [ \"${EVENTNET_INJECT_FAILURE:-0}\" = \"after-runtime\" ]; then\n");
    fprintf(file, "  printf '%%s\\n' 'injected runtime failure for rollback smoke' >&2\n");
    fprintf(file, "  exit 97\n");
    fprintf(file, "fi\n");
    fclose(file);
    return 0;
}

static int write_integrated_script(const char *filename, const en_yaml_config_t *config, const en_intent_t *intent, const en_path_t *path, const char *kind)
{
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        return 1;
    }
    fprintf(file, "#!/usr/bin/env sh\n");
    fprintf(file, "set -eu\n\n");
    fprintf(file, "ROOT_DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")/../..\" && pwd)\n");
    fprintf(file, "cd \"$ROOT_DIR\"\n\n");
    fprintf(file, "ROLLBACK_SCRIPT=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)/rollback-selected.sh\n");
    fprintf(file, "if [ \"$(id -u)\" != \"0\" ]; then\n");
    fprintf(file, "  printf 'Re-running integrated runtime as root...\\n'\n");
    fprintf(file, "  exec sudo sh \"$0\" \"$@\"\n");
    fprintf(file, "fi\n\n");
    fprintf(file, "rollback_on_error() {\n");
    fprintf(file, "  status=$?\n");
    fprintf(file, "  if [ \"$status\" -ne 0 ]; then sh \"$ROLLBACK_SCRIPT\" || true; fi\n");
    fprintf(file, "  exit \"$status\"\n");
    fprintf(file, "}\n");
    fprintf(file, "trap rollback_on_error EXIT\n\n");
    fprintf(file, "ensure_dummy_lan() {\n");
    fprintf(file, "  ns=\"$1\"\n");
    fprintf(file, "  addr=\"$2\"\n");
    fprintf(file, "  ip netns exec \"$ns\" ip link show lan0 >/dev/null 2>&1 || \\\n");
    fprintf(file, "    ip netns exec \"$ns\" ip link add lan0 type dummy\n");
    fprintf(file, "  ip netns exec \"$ns\" ip addr flush dev lan0\n");
    fprintf(file, "  ip netns exec \"$ns\" ip addr add \"$addr\" dev lan0\n");
    fprintf(file, "  ip netns exec \"$ns\" ip link set lan0 up\n");
    fprintf(file, "}\n\n");
    fprintf(file, "apply_vpp_netns_runtime() {\n");
    fprintf(file, "  sh scripts/vm-vpp-netns-setup.sh\n");
    fprintf(file, "  DRY_RUN=0 sh out/netns-runtime/vpp-netns-route-plan.sh\n");
    fprintf(file, "  ip netns exec site-a ip route replace 10.10.2.0/24 via 172.16.1.1\n");
    fprintf(file, "  ip netns exec site-b ip route replace 10.10.1.0/24 via 172.16.2.1\n");
    fprintf(file, "  printf '\\n== integrated VPP forwarding: site-a -> site-b ==\\n'\n");
    fprintf(file, "  ip netns exec site-a ping -c 3 -I 10.10.1.1 10.10.2.1\n");
    fprintf(file, "  printf '\\n== integrated VPP forwarding: site-b -> site-a ==\\n'\n");
    fprintf(file, "  ip netns exec site-b ping -c 3 -I 10.10.2.1 10.10.1.1\n");
    fprintf(file, "}\n\n");
    fprintf(file, "printf 'eventnet integrated selected path: %s\\n'\n", path->path_id);
    fprintf(file, "printf 'eventnet integrated runtime kind: %s\\n'\n\n", kind);
    fprintf(file, "ensure_dummy_lan site-a 10.10.1.1/24\n");
    fprintf(file, "ensure_dummy_lan site-b 10.10.2.1/24\n\n");
    if (strcmp(kind, "direct") == 0) {
        fprintf(file, "sh scripts/vm-netns-ipsec-hub-stop.sh 2>/dev/null || true\n");
        fprintf(file, "sh scripts/vm-netns-ipsec-direct-start.sh\n");
        fprintf(file, "sh scripts/vm-netns-ipsec-direct-smoke.sh\n");
    } else if (strcmp(kind, "hub") == 0) {
        fprintf(file, "sh scripts/vm-netns-ipsec-direct-stop.sh 2>/dev/null || true\n");
        fprintf(file, "sh scripts/vm-netns-ipsec-hub-start.sh\n");
        fprintf(file, "sh scripts/vm-netns-ipsec-hub-smoke.sh\n");
    } else if (strcmp(kind, "vpp") == 0) {
        fprintf(file, "sh scripts/vm-vpp-netns-setup.sh\n");
        fprintf(file, "DRY_RUN=0 sh out/netns-runtime/vpp-netns-route-plan.sh\n");
    } else {
        fprintf(file, "echo 'unsupported path for current integrated runtime: %s' >&2\n", path->path_id);
        fprintf(file, "exit 1\n");
    }
    if (intent->block_non_ipsec) {
        fprintf(file, "\n# Block cleartext traffic outside the selected IPsec selectors.\n");
        write_xfrm_block_runtime(file, config, path);
    }
    fprintf(file, "\napply_vpp_netns_runtime\n");
    fprintf(file, "if [ \"${EVENTNET_INJECT_FAILURE:-0}\" = \"after-runtime\" ]; then\n");
    fprintf(file, "  printf '%%s\\n' 'injected runtime failure for rollback smoke' >&2\n");
    fprintf(file, "  exit 97\n");
    fprintf(file, "fi\n");
    fprintf(file, "printf '\\nIntegrated controller runtime passed: IPsec path and VPP forwarding were controlled from one generated plan.\\n'\n");
    fclose(file);
    return 0;
}

static int write_rollback_script(const char *filename, const char *kind)
{
    FILE *file = fopen(filename, "w");
    if (file == NULL) return 1;
    fprintf(file, "#!/usr/bin/env sh\nset -eu\n\n");
    fprintf(file, "ROOT_DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")/../..\" && pwd)\ncd \"$ROOT_DIR\"\n\n");
    fprintf(file, "if [ \"$(id -u)\" != \"0\" ]; then exec sudo sh \"$0\" \"$@\"; fi\n\n");
    fprintf(file, "printf 'eventnet runtime rollback: %s\\n'\n", kind);
    if (strcmp(kind, "direct") == 0) {
        fprintf(file, "sh scripts/vm-netns-ipsec-direct-stop.sh\n");
    } else if (strcmp(kind, "hub") == 0) {
        fprintf(file, "sh scripts/vm-netns-ipsec-hub-stop.sh\n");
    } else if (strcmp(kind, "vpp") == 0) {
        fprintf(file, "sh scripts/vm-vpp-netns-clean.sh\n");
    } else {
        fprintf(file, "printf '%%s\\n' 'unsupported runtime rollback' >&2\nexit 1\n");
    }
    fprintf(file, "printf '%%s\\n' 'eventnet runtime rollback completed'\n");
    fclose(file);
    return 0;
}

static int write_summary(const char *filename, const en_yaml_config_t *config, const en_intent_t *intent, const en_path_t *path, const char *kind)
{
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        return 1;
    }
    fprintf(file, "selected_path: %s\n", path->path_id);
    fprintf(file, "runtime_kind: %s\n", kind);
    fprintf(file, "traffic_source: %s\n", intent->traffic.source);
    fprintf(file, "traffic_destination: %s\n", intent->traffic.destination);
    if (intent->traffic.has_vlan_id) fprintf(file, "vlan_id: %d\n", intent->traffic.vlan_id);
    if (intent->deny_unmatched_vlan) fprintf(file, "deny_unmatched_vlan: true\n");
    fprintf(file, "required_waypoints:\n");
    for (size_t i = 0; i < intent->path_selection.constraints.required_waypoint_count; i++) {
        fprintf(file, "  - %s\n", intent->path_selection.constraints.required_waypoints[i]);
    }
    fprintf(file, "source: %s\n", path->source);
    fprintf(file, "destination: %s\n", path->destination);
    fprintf(file, "route_destination_prefix: %s\n", path->route_destination_prefix);
    fprintf(file, "route_next_hop: %s\n", path->route_next_hop);
    fprintf(file, "routes:\n");
    for (size_t i = 0; i < path->route_count; i++) {
        const en_route_t *route = &path->routes[i];
        fprintf(file, "  - id: %s\n", route->route_id);
        fprintf(file, "    node_id: %s\n", route->node_id);
        fprintf(file, "    destination_prefix: %s\n", route->destination_prefix);
        fprintf(file, "    next_hop: %s\n", route->next_hop);
        fprintf(file, "    interface: %s\n", route->interface_name);
        if (route->table_id >= 0) {
            fprintf(file, "    table: %d\n", route->table_id);
        }
        if (route->metric >= 0) {
            fprintf(file, "    metric: %d\n", route->metric);
        }
    }
    fprintf(file, "vpp_edges:\n");
    for (size_t i = 0; i < config->vpp_edge_count; i++) {
        const en_vpp_edge_t *edge = &config->vpp_edges[i];
        fprintf(file, "  - node_id: %s\n", edge->node_id);
        if (edge->port_id[0] != '\0') fprintf(file, "    port_id: %s\n", edge->port_id);
        fprintf(file, "    vpp_interface: %s\n", edge->vpp_interface);
        fprintf(file, "    next_hop: %s\n", edge->next_hop);
        if (edge->allowed_vlan_count > 0) {
            fprintf(file, "    allowed_vlans:\n");
            for (size_t vlan_index = 0; vlan_index < edge->allowed_vlan_count; vlan_index++) {
                fprintf(file, "      - %d\n", edge->allowed_vlans[vlan_index]);
            }
        }
    }
    fprintf(file, "segments:\n");
    for (size_t i = 0; i < path->segment_count; i++) {
        const en_segment_t *segment = &path->segments[i];
        const en_tunnel_t *tunnel = find_tunnel(config, segment->tunnel_id);
        fprintf(file, "  - id: %s\n", segment->segment_id);
        fprintf(file, "    from: %s\n", segment->from_node);
        fprintf(file, "    to: %s\n", segment->to_node);
        fprintf(file, "    tunnel_id: %s\n", segment->tunnel_id);
        if (tunnel != NULL) {
            fprintf(file, "    local_endpoint: %s\n", tunnel->local_endpoint);
            fprintf(file, "    remote_endpoint: %s\n", tunnel->remote_endpoint);
            fprintf(file, "    local_ts: %s\n", tunnel->local_traffic_selector);
            fprintf(file, "    remote_ts: %s\n", tunnel->remote_traffic_selector);
        }
    }
    fclose(file);
    return 0;
}

static int write_vpp_route_plan(const char *filename, const en_yaml_config_t *config, const en_path_t *path)
{
    const en_tunnel_t *first_tunnel = NULL;
    const en_tunnel_t *last_tunnel = NULL;
    if (path->segment_count > 0) {
        first_tunnel = find_tunnel(config, path->segments[0].tunnel_id);
        last_tunnel = find_tunnel(config, path->segments[path->segment_count - 1].tunnel_id);
    }
    if ((path->segment_count > 0 && (first_tunnel == NULL || last_tunnel == NULL)) ||
        (path->segment_count == 0 && ((!path->routes_explicit &&
            (path->route_destination_prefix[0] == '\0' || path->route_next_hop[0] == '\0')) ||
            (path->routes_explicit && path->route_count == 0)))) {
        return 1;
    }

    const char *source_prefix = first_tunnel == NULL ? "" : first_tunnel->local_traffic_selector;
    const char *destination_prefix = first_tunnel == NULL || path->route_destination_prefix[0] != '\0' ?
        path->route_destination_prefix : last_tunnel->remote_traffic_selector;

    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        return 1;
    }
    fprintf(file, "#!/usr/bin/env sh\n");
    fprintf(file, "set -eu\n\n");
    fprintf(file, "VPPCTL=\"${VPPCTL:-vppctl}\"\n");
    fprintf(file, "VPPCTL_SOCKET=\"${VPPCTL_SOCKET:-}\"\n");
    fprintf(file, "DRY_RUN=\"${DRY_RUN:-1}\"\n\n");
    fprintf(file, "run_vpp() {\n");
    fprintf(file, "  if [ \"$DRY_RUN\" = \"1\" ]; then\n");
    fprintf(file, "    if [ -n \"$VPPCTL_SOCKET\" ]; then printf '[dry-run] %%s -s %%s %%s\\n' \"$VPPCTL\" \"$VPPCTL_SOCKET\" \"$*\"; else printf '[dry-run] %%s %%s\\n' \"$VPPCTL\" \"$*\"; fi\n");
    fprintf(file, "  else\n");
    fprintf(file, "    if [ -n \"$VPPCTL_SOCKET\" ]; then \"$VPPCTL\" -s \"$VPPCTL_SOCKET\" \"$@\"; else \"$VPPCTL\" \"$@\"; fi\n");
    fprintf(file, "  fi\n");
    fprintf(file, "}\n\n");
    fprintf(file, "ensure_vpp_table() {\n");
    fprintf(file, "  table=\"$1\"\n");
    fprintf(file, "  if [ \"$DRY_RUN\" = \"1\" ]; then\n");
    fprintf(file, "    run_vpp ip table add \"$table\"\n");
    fprintf(file, "  elif ! run_vpp show ip fib | grep -q \"ipv4-VRF:$table\"; then\n");
    fprintf(file, "    run_vpp ip table add \"$table\"\n");
    fprintf(file, "  fi\n");
    fprintf(file, "}\n\n");
    fprintf(file, "printf 'VPP route plan for path: %s\\n'\n", path->path_id);
    fprintf(file, "printf 'source_prefix: %s\\n'\n", source_prefix);
    fprintf(file, "printf 'destination_prefix: %s\\n'\n\n", destination_prefix);

    if (path->routes_explicit) {
        for (size_t i = 0; i < path->route_count; i++) {
            const en_route_t *route = &path->routes[i];
            if (route->table_id > 0) {
                bool already_emitted = false;
                for (size_t j = 0; j < i; j++) {
                    if (path->routes[j].table_id == route->table_id) already_emitted = true;
                }
                if (!already_emitted) fprintf(file, "ensure_vpp_table %d\n", route->table_id);
            }
        }
        for (size_t i = 0; i < path->route_count; i++) {
            const en_route_t *route = &path->routes[i];
            fprintf(file, "printf '# explicit route %s on node %s\\n'\n", route->route_id, route->node_id);
            fprintf(file, "run_vpp ip route add %s", route->destination_prefix);
            if (route->table_id >= 0) {
                fprintf(file, " table %d", route->table_id);
            }
            fprintf(file, " via %s", route->next_hop);
            if (route->interface_name[0] != '\0') {
                fprintf(file, " %s", route->interface_name);
            }
            if (route->metric >= 0) {
                fprintf(file, " preference %d", route->metric);
            }
            fprintf(file, "\n");
        }
    } else if (path->segment_count == 0) {
        fprintf(file, "printf '# node %s: legacy destination route\n'\n", path->source);
        fprintf(file, "run_vpp ip route add %s via %s\n", destination_prefix, path->route_next_hop);
    } else if (path->segment_count == 1) {
        fprintf(file, "printf '# node %s: destination route\\n'\n", path->source);
        fprintf(file, "run_vpp ip route add %s via %s\n", destination_prefix, first_tunnel->remote_endpoint);
        fprintf(file, "printf '# node %s: source return route\\n'\n", path->destination);
        fprintf(file, "run_vpp ip route add %s via %s\n", source_prefix, first_tunnel->local_endpoint);
    } else {
        fprintf(file, "printf '# node %s: destination route to first waypoint\\n'\n", path->source);
        fprintf(file, "run_vpp ip route add %s via %s\n", destination_prefix, first_tunnel->remote_endpoint);
        for (size_t i = 0; i < path->segment_count - 1; i++) {
            const en_segment_t *incoming_segment = &path->segments[i];
            const en_segment_t *outgoing_segment = &path->segments[i + 1];
            const en_tunnel_t *incoming_tunnel = find_tunnel(config, incoming_segment->tunnel_id);
            const en_tunnel_t *outgoing_tunnel = find_tunnel(config, outgoing_segment->tunnel_id);
            if (incoming_tunnel == NULL || outgoing_tunnel == NULL) {
                fclose(file);
                return 1;
            }
            fprintf(file, "printf '# node %s: source return route\\n'\n", incoming_segment->to_node);
            fprintf(file, "run_vpp ip route add %s via %s\n", source_prefix, incoming_tunnel->local_endpoint);
            fprintf(file, "printf '# node %s: destination route\\n'\n", outgoing_segment->from_node);
            fprintf(file, "run_vpp ip route add %s via %s\n", destination_prefix, outgoing_tunnel->remote_endpoint);
        }
        fprintf(file, "printf '# node %s: source return route\\n'\n", path->destination);
        fprintf(file, "run_vpp ip route add %s via %s\n", source_prefix, last_tunnel->local_endpoint);
    }

    fclose(file);
    return 0;
}

static int write_vpp_netns_route_plan(const char *filename, const en_yaml_config_t *config, const en_intent_t *intent, const en_path_t *path)
{
    const en_tunnel_t *first_tunnel = NULL;
    const en_tunnel_t *last_tunnel = NULL;
    if (path->segment_count > 0) {
        first_tunnel = find_tunnel(config, path->segments[0].tunnel_id);
        last_tunnel = find_tunnel(config, path->segments[path->segment_count - 1].tunnel_id);
    }
    if ((path->segment_count > 0 && (first_tunnel == NULL || last_tunnel == NULL)) ||
        (path->segment_count == 0 && ((!path->routes_explicit &&
            (path->route_destination_prefix[0] == '\0' || path->route_next_hop[0] == '\0')) ||
            (path->routes_explicit && path->route_count == 0)))) {
        return 1;
    }

    const char *source_prefix = first_tunnel == NULL ? "" : first_tunnel->local_traffic_selector;
    const char *destination_prefix = first_tunnel == NULL || path->route_destination_prefix[0] != '\0' ?
        path->route_destination_prefix : last_tunnel->remote_traffic_selector;
    const en_vpp_edge_t *source_edge = find_vpp_edge(config, path->source);
    const en_vpp_edge_t *destination_edge = find_vpp_edge(config, path->destination);
    const char *source_next_hop = source_edge == NULL ? "172.16.1.2" : source_edge->next_hop;
    const char *destination_next_hop = destination_edge == NULL ? "172.16.2.2" : destination_edge->next_hop;
    const char *source_vpp_interface = source_edge == NULL ? "host-vpp-site-a" : source_edge->vpp_interface;
    const char *destination_vpp_interface = destination_edge == NULL ? "host-vpp-site-b" : destination_edge->vpp_interface;
    if (intent->traffic.has_vlan_id) {
        for (size_t index = 0; index < config->vpp_edge_count; index++) {
            const en_vpp_edge_t *edge = &config->vpp_edges[index];
            if (path_uses_node(path, edge->node_id) && !vpp_edge_allows_vlan(edge, intent->traffic.vlan_id)) {
                fprintf(stderr, "VLAN %d is not allowed on the selected VPP edge\n", intent->traffic.vlan_id);
                return 1;
            }
        }
    }
    if (intent->traffic.has_vlan_id && path_has_conflicting_tables(path)) {
        fprintf(stderr, "VLAN path has conflicting FIB tables for one node\n");
        return 1;
    }

    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        return 1;
    }
    fprintf(file, "#!/usr/bin/env sh\n");
    fprintf(file, "set -eu\n\n");
    fprintf(file, "VPPCTL=\"${VPPCTL:-vppctl}\"\n");
    fprintf(file, "VPPCTL_SOCKET=\"${VPPCTL_SOCKET:-}\"\n");
    fprintf(file, "DRY_RUN=\"${DRY_RUN:-1}\"\n\n");
    fprintf(file, "run_vpp() {\n");
    fprintf(file, "  if [ \"$DRY_RUN\" = \"1\" ]; then\n");
    fprintf(file, "    if [ -n \"$VPPCTL_SOCKET\" ]; then printf '[dry-run] %%s -s %%s %%s\\n' \"$VPPCTL\" \"$VPPCTL_SOCKET\" \"$*\"; else printf '[dry-run] %%s %%s\\n' \"$VPPCTL\" \"$*\"; fi\n");
    fprintf(file, "  else\n");
    fprintf(file, "    if [ -n \"$VPPCTL_SOCKET\" ]; then \"$VPPCTL\" -s \"$VPPCTL_SOCKET\" \"$@\"; else \"$VPPCTL\" \"$@\"; fi\n");
    fprintf(file, "  fi\n");
    fprintf(file, "}\n\n");
    fprintf(file, "ensure_vpp_table() {\n");
    fprintf(file, "  table=\"$1\"\n");
    fprintf(file, "  if [ \"$DRY_RUN\" = \"1\" ]; then\n");
    fprintf(file, "    run_vpp ip table add \"$table\"\n");
    fprintf(file, "  elif ! run_vpp show ip fib | grep -q \"ipv4-VRF:$table\"; then\n");
    fprintf(file, "    run_vpp ip table add \"$table\"\n");
    fprintf(file, "  fi\n");
    fprintf(file, "}\n\n");
    fprintf(file, "ensure_vlan_subinterface() {\n");
    fprintf(file, "  parent=\"$1\"\n");
    fprintf(file, "  vlan=\"$2\"\n");
    fprintf(file, "  if [ \"$DRY_RUN\" = \"1\" ]; then\n");
    fprintf(file, "    run_vpp create sub-interfaces \"$parent\" \"$vlan\"\n");
    fprintf(file, "  elif ! \"$VPPCTL\" show interface \"$parent.$vlan\" >/dev/null 2>&1; then\n");
    fprintf(file, "    run_vpp create sub-interfaces \"$parent\" \"$vlan\"\n");
    fprintf(file, "  fi\n");
    fprintf(file, "  run_vpp set interface \"$parent.$vlan\" up\n");
    fprintf(file, "}\n\n");
    fprintf(file, "set_vlan_interface_table() {\n");
    fprintf(file, "  interface=\"$1\"\n");
    fprintf(file, "  table=\"$2\"\n");
    fprintf(file, "  run_vpp set interface ip table \"$interface\" \"$table\"\n");
    fprintf(file, "}\n\n");
    fprintf(file, "ensure_unmatched_vlan_acl() {\n");
    fprintf(file, "  parent=\"$1\"\n");
    fprintf(file, "  acl_index=\"$2\"\n");
    fprintf(file, "  run_vpp set acl-plugin acl index \"$acl_index\" deny src 0.0.0.0/0 dst 0.0.0.0/0 , deny src ::/0 dst ::/0 tag ibuki-vlan-deny-\"$acl_index\"\n");
    fprintf(file, "  run_vpp set acl-plugin interface \"$parent\" input acl \"$acl_index\"\n");
    fprintf(file, "}\n\n");
    fprintf(file, "printf 'VPP netns route plan for path: %s\\n'\n", path->path_id);
    fprintf(file, "printf 'source_prefix: %s via %s %s\\n'\n", source_prefix, path->source, source_vpp_interface);
    fprintf(file, "printf 'destination_prefix: %s via %s %s\\n'\n\n", destination_prefix, path->destination, destination_vpp_interface);
    if (intent->traffic.has_vlan_id) {
        fprintf(file, "printf 'VLAN policy: vlan_id=%d\\n'\n", intent->traffic.vlan_id);
        for (size_t index = 0; index < config->vpp_edge_count; index++) {
            const en_vpp_edge_t *edge = &config->vpp_edges[index];
            if (path_uses_node(path, edge->node_id) && edge->vpp_interface[0] != '\0') {
                fprintf(file, "ensure_vlan_subinterface %s %d\n", edge->vpp_interface, intent->traffic.vlan_id);
            }
        }
        if (intent->deny_unmatched_vlan) {
            fprintf(file, "printf 'VLAN unmatched traffic policy: deny parent interface\\n'\n");
            int acl_index = 10000 + intent->traffic.vlan_id;
            size_t acl_offset = 0;
            for (size_t index = 0; index < config->vpp_edge_count; index++) {
                const en_vpp_edge_t *edge = &config->vpp_edges[index];
                if (path_uses_node(path, edge->node_id) && edge->vpp_interface[0] != '\0') {
                    fprintf(file, "ensure_unmatched_vlan_acl %s %d\n", edge->vpp_interface, acl_index + (int)acl_offset++);
                }
            }
        }
    }
    if (path->routes_explicit) {
        for (size_t i = 0; i < path->route_count; i++) {
            const en_route_t *route = &path->routes[i];
            if (route->table_id > 0) {
                bool already_emitted = false;
                for (size_t j = 0; j < i; j++) {
                    if (path->routes[j].table_id == route->table_id) already_emitted = true;
                }
                if (!already_emitted) fprintf(file, "ensure_vpp_table %d\n", route->table_id);
            }
        }
        for (size_t i = 0; i < path->route_count; i++) {
            const en_route_t *route = &path->routes[i];
            if (route->table_id <= 0 || !intent->traffic.has_vlan_id) continue;
            const en_vpp_edge_t *edge = find_vpp_edge_for_route(config, route);
            if (edge == NULL || edge->vpp_interface[0] == '\0') continue;
            bool emitted = false;
            for (size_t j = 0; j < i; j++) {
                if (strcmp(path->routes[j].node_id, route->node_id) == 0 && path->routes[j].table_id >= 0) {
                    emitted = true;
                    break;
                }
            }
            if (!emitted) fprintf(file, "set_vlan_interface_table %s.%d %d\n", edge->vpp_interface, intent->traffic.vlan_id, route->table_id);
        }
        for (size_t i = 0; i < path->route_count; i++) {
            const en_route_t *route = &path->routes[i];
            const en_vpp_edge_t *edge = find_vpp_edge_for_route(config, route);
            if (edge == NULL) {
                fprintf(file, "printf '# skip explicit route %s: no vpp_edge for node %s\\n'\n", route->route_id, route->node_id);
                continue;
            }
            fprintf(file, "printf '# explicit netns route %s on node %s via %s\\n'\n", route->route_id, route->node_id, edge->vpp_interface);
            fprintf(file, "run_vpp ip route add %s", route->destination_prefix);
            if (route->table_id >= 0) fprintf(file, " table %d", route->table_id);
            fprintf(file, " via %s", edge->next_hop);
            if (edge->vpp_interface[0] != '\0') {
                if (intent->traffic.has_vlan_id) fprintf(file, " %s.%d", edge->vpp_interface, intent->traffic.vlan_id);
                else fprintf(file, " %s", edge->vpp_interface);
            }
            if (route->metric >= 0) {
                fprintf(file, " preference %d", route->metric);
            }
            fprintf(file, "\n");
        }
    } else if (path->segment_count == 0) {
        if (intent->traffic.has_vlan_id) {
            fprintf(file, "run_vpp ip route add %s via %s %s.%d\n", destination_prefix,
                path->route_next_hop, source_vpp_interface, intent->traffic.vlan_id);
        } else {
            fprintf(file, "run_vpp ip route add %s via %s %s\n", destination_prefix,
                path->route_next_hop, source_vpp_interface);
        }
    } else {
        if (intent->traffic.has_vlan_id) {
            fprintf(file, "run_vpp ip route add %s via %s %s.%d\n", source_prefix, source_next_hop, source_vpp_interface, intent->traffic.vlan_id);
            fprintf(file, "run_vpp ip route add %s via %s %s.%d\n", destination_prefix, destination_next_hop, destination_vpp_interface, intent->traffic.vlan_id);
        } else {
            fprintf(file, "run_vpp ip route add %s via %s\n", source_prefix, source_next_hop);
            fprintf(file, "run_vpp ip route add %s via %s\n", destination_prefix, destination_next_hop);
        }
    }
    fclose(file);
    return 0;
}

static void set_failed_health(en_health_probe_mock_t *health_mock, const char *path_id)
{
    en_path_health_t health = {0};
    snprintf(health.path_id, sizeof(health.path_id), "%s", path_id);
    health.state = EN_HEALTH_FAILED;
    health.rtt_ms = 0.0;
    health.packet_loss_percent = 100.0;
    health.consecutive_failures = 3;
    health.consecutive_successes = 0;
    en_health_probe_mock_set(health_mock, health);
}

static void usage(const char *program)
{
    printf("usage: %s [--intent ID] [--path PATH_ID] [--active-path PATH_ID] [--fail-path PATH_ID] [--out-dir DIR] YAML\n", program);
}

int main(int argc, char **argv)
{
    const char *filename = "samples/linux-vm-netns.yaml";
    const char *intent_id = NULL;
    const char *forced_path_id = NULL;
    const char *active_path_id = NULL;
    const char *failed_paths[EN_MAX_PATHS] = {0};
    size_t failed_path_count = 0;
    const char *out_dir = "out/netns-runtime";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--intent") == 0 && i + 1 < argc) {
            intent_id = argv[++i];
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            forced_path_id = argv[++i];
        } else if (strcmp(argv[i], "--active-path") == 0 && i + 1 < argc) {
            active_path_id = argv[++i];
        } else if (strcmp(argv[i], "--fail-path") == 0 && i + 1 < argc) {
            if (failed_path_count >= EN_MAX_PATHS) {
                fprintf(stderr, "too many --fail-path values\n");
                return 1;
            }
            failed_paths[failed_path_count++] = argv[++i];
        } else if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            filename = argv[i];
        }
    }

    en_yaml_config_t config = {0};
    char error[256] = {0};
    en_error_code_t err = en_yaml_config_load_file(filename, &config, error, sizeof(error));
    if (err != EN_ERR_NONE) {
        fprintf(stderr, "yaml load failed: %s: %s\n", en_error_code_name(err), error);
        return 1;
    }

    const en_intent_t *intent = find_intent(&config, intent_id);
    if (intent == NULL) {
        fprintf(stderr, "intent not found\n");
        return 1;
    }

    const en_path_t *selected_path = NULL;
    char selected_path_id[EN_MAX_ID_LEN] = {0};
    char reason[128] = "forced path";
    if (forced_path_id != NULL) {
        selected_path = find_path(&config, forced_path_id);
        if (selected_path == NULL) {
            fprintf(stderr, "path not found: %s\n", forced_path_id);
            return 1;
        }
        snprintf(selected_path_id, sizeof(selected_path_id), "%s", forced_path_id);
    } else {
        en_vpp_mock_t vpp_mock = {0};
        en_health_probe_mock_t health_mock = {0};
        for (size_t i = 0; i < failed_path_count; i++) {
            set_failed_health(&health_mock, failed_paths[i]);
        }
        if (
            active_path_id != NULL &&
            failed_path_count > 0 &&
            intent->fallback.enabled &&
            intent->fallback.path_id[0] != '\0'
        ) {
            for (size_t i = 0; i < failed_path_count; i++) {
                if (strcmp(active_path_id, failed_paths[i]) == 0) {
                    selected_path = find_path(&config, intent->fallback.path_id);
                    if (selected_path == NULL) {
                        fprintf(stderr, "fallback path not found: %s\n", intent->fallback.path_id);
                        return 1;
                    }
                    snprintf(selected_path_id, sizeof(selected_path_id), "%s", selected_path->path_id);
                    snprintf(reason, sizeof(reason), "active path %s failed; using fallback %s", active_path_id, selected_path->path_id);
                    break;
                }
            }
        }
        if (selected_path != NULL) {
            goto selected;
        }
        en_controller_t *controller = en_controller_create_with_nodes_and_tunnels(
            config.nodes,
            config.node_count,
            config.paths,
            config.path_count,
            config.tunnels,
            config.tunnel_count,
            en_strongswan_mock_adapter(),
            en_vpp_mock_adapter(&vpp_mock),
            en_health_probe_mock_adapter(&health_mock)
        );
        if (controller == NULL) {
            fprintf(stderr, "controller create failed\n");
            return 1;
        }
        en_reconcile_result_t result = {0};
        err = en_controller_submit_intent(controller, intent, &result);
        en_controller_destroy(controller);
        if (err != EN_ERR_NONE) {
            fprintf(stderr, "intent reconcile failed: %s\n", en_error_code_name(err));
            return 1;
        }
        snprintf(selected_path_id, sizeof(selected_path_id), "%s", result.selected_path);
        selected_path = find_path(&config, selected_path_id);
        snprintf(reason, sizeof(reason), "%s", result.explanation.reason);
    }

selected:
    if (selected_path == NULL) {
        fprintf(stderr, "selected path not found: %s\n", selected_path_id);
        return 1;
    }

    const char *kind = runtime_kind(&config, selected_path);
    char apply_script[256] = {0};
    char integrated_script[256] = {0};
    char rollback_script[256] = {0};
    char summary[256] = {0};
    char vpp_plan[256] = {0};
    char vpp_netns_plan[256] = {0};
    snprintf(apply_script, sizeof(apply_script), "%s/apply-selected.sh", out_dir);
    snprintf(integrated_script, sizeof(integrated_script), "%s/apply-integrated.sh", out_dir);
    snprintf(rollback_script, sizeof(rollback_script), "%s/rollback-selected.sh", out_dir);
    snprintf(summary, sizeof(summary), "%s/selected-path.txt", out_dir);
    snprintf(vpp_plan, sizeof(vpp_plan), "%s/vpp-route-plan.sh", out_dir);
    snprintf(vpp_netns_plan, sizeof(vpp_netns_plan), "%s/vpp-netns-route-plan.sh", out_dir);

    if (write_apply_script(apply_script, &config, intent, selected_path, kind) != 0) {
        fprintf(stderr, "failed to write %s\n", apply_script);
        return 1;
    }
    if (write_integrated_script(integrated_script, &config, intent, selected_path, kind) != 0) {
        fprintf(stderr, "failed to write %s\n", integrated_script);
        return 1;
    }
    if (write_rollback_script(rollback_script, kind) != 0) {
        fprintf(stderr, "failed to write %s\n", rollback_script);
        return 1;
    }
    if (write_summary(summary, &config, intent, selected_path, kind) != 0) {
        fprintf(stderr, "failed to write %s\n", summary);
        return 1;
    }
    if (write_vpp_route_plan(vpp_plan, &config, selected_path) != 0) {
        fprintf(stderr, "failed to write %s\n", vpp_plan);
        return 1;
    }
    if (write_vpp_netns_route_plan(vpp_netns_plan, &config, intent, selected_path) != 0) {
        fprintf(stderr, "failed to write %s\n", vpp_netns_plan);
        return 1;
    }

    printf("intent: %s\n", intent->intent_id);
    printf("selected_path: %s\n", selected_path->path_id);
    printf("runtime_kind: %s\n", kind);
    printf("reason: %s\n", reason);
    printf("wrote: %s\n", apply_script);
    printf("wrote: %s\n", integrated_script);
    printf("wrote: %s\n", rollback_script);
    printf("wrote: %s\n", summary);
    printf("wrote: %s\n", vpp_plan);
    printf("wrote: %s\n", vpp_netns_plan);

    return strcmp(kind, "unsupported") == 0 ? 2 : 0;
}
