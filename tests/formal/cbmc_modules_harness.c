#include "internal.h"
#include "eventnet/mock_adapters.h"
#include "eventnet/render_commands.h"
#include "eventnet/strongswan_observer.h"
#include "eventnet/vpp_observer.h"
#include "eventnet/vpp_api_transport.h"
#include "eventnet/apply_plan.h"

#include <assert.h>
#include <string.h>

size_t nondet_size(void);

void harness_audit_capacity(void)
{
    en_controller_t controller;
    controller.audit_count = EN_MAX_EVENTS;
    en_audit_append(&controller, NULL, NULL, NULL);
    assert(controller.audit_count == EN_MAX_EVENTS);
    controller.audit_count = (size_t)-1;
    en_audit_append(&controller, NULL, NULL, NULL);
    assert(controller.audit_count == (size_t)-1);
    en_audit_append(NULL, NULL, NULL, NULL);
}

void harness_error_capacity(void)
{
    en_controller_t controller;
    controller.state.error_count = EN_MAX_ERRORS;
    en_error_append(&controller, EN_ERR_INVALID_ARGUMENT, NULL);
    assert(controller.state.error_count == EN_MAX_ERRORS);
    controller.state.error_count = (size_t)-1;
    en_error_append(&controller, EN_ERR_INVALID_ARGUMENT, NULL);
    assert(controller.state.error_count == (size_t)-1);
    en_error_append(NULL, EN_ERR_INVALID_ARGUMENT, NULL);
}

void harness_swan_mock(void)
{
    en_strongswan_adapter_t adapter = en_strongswan_mock_adapter();
    en_tunnel_t desired;
    en_tunnel_t observed;
    assert(adapter.ensure_tunnel(adapter.ctx, &desired, &observed) == EN_ERR_NONE);
    assert(observed.state == EN_TUNNEL_ESTABLISHED);
    assert(observed.health == EN_HEALTH_HEALTHY);
    assert(adapter.remove_tunnel(adapter.ctx, &desired, &observed) == EN_ERR_NONE);
    assert(observed.state == EN_TUNNEL_ABSENT);
    assert(adapter.ensure_tunnel(adapter.ctx, NULL, &observed) == EN_ERR_INVALID_ARGUMENT);
    assert(adapter.remove_tunnel(adapter.ctx, &desired, NULL) == EN_ERR_INVALID_ARGUMENT);
}

void harness_vpp_failure(void)
{
    en_vpp_mock_t mock = {0};
    en_path_t path;
    en_vpp_adapter_t adapter = en_vpp_mock_adapter(&mock);
    mock.fail_next_update = true;
    assert(adapter.install_path(adapter.ctx, "key", &path) == EN_ERR_FORWARDING_UPDATE_FAILED);
    assert(!mock.fail_next_update);
    assert(mock.active_count == 0);
    assert(mock.install_count == 0);
    assert(adapter.install_path(adapter.ctx, NULL, &path) == EN_ERR_INVALID_ARGUMENT);
    assert(adapter.remove_path(adapter.ctx, "key", NULL) == EN_ERR_INVALID_ARGUMENT);
}

void harness_command_rejection(void)
{
    en_tunnel_t tunnel = {0};
    char output[8];
    strcpy(tunnel.tunnel_id, "x;y");
    size_t capacity = nondet_size();
    __CPROVER_assume(capacity <= sizeof(output));
    assert(en_render_swanctl_initiate(&tunnel, output, capacity) == EN_ERR_INVALID_ARGUMENT);
    assert(en_render_swanctl_terminate(&tunnel, output, capacity) == EN_ERR_INVALID_ARGUMENT);
    assert(en_render_swanctl_list_sas_uri(&tunnel, NULL, output, capacity) == EN_ERR_INVALID_ARGUMENT);
}

void harness_transport_arguments(void)
{
    en_vpp_api_transport_t transport;
    en_vpp_api_transport_init(&transport);
    assert(!en_vpp_api_transport_is_connected(&transport));
    assert(en_vpp_api_transport_open(&transport, "app", NULL, 0, 1) == EN_ERR_INVALID_ARGUMENT);
    assert(en_vpp_api_transport_open(&transport, NULL, NULL, 1, 1) == EN_ERR_INVALID_ARGUMENT);
    assert(!en_vpp_api_transport_is_connected(&transport));
    assert(en_vpp_api_transport_dispatch(&transport) == EN_ERR_STATE_CONFLICT);
}

void harness_observer_empty(void)
{
    en_strongswan_sa_observation_t sa;
    en_vpp_route_observation_t route;
    en_vpp_interface_observation_t interface;
    assert(en_strongswan_parse_list_sas("", "child", &sa, NULL, 0) == EN_ERR_NOT_FOUND);
    assert(sa.state == EN_TUNNEL_ABSENT);
    assert(en_vpp_parse_show_ip_fib("", "10.0.0.0/24", &route, NULL, 0) == EN_ERR_NOT_FOUND);
    assert(!route.present);
    assert(en_vpp_parse_show_interface("", "host-test", &interface, NULL, 0) == EN_ERR_NOT_FOUND);
    assert(!interface.present);
}

void harness_path_count_predicate(void)
{
    en_path_t path;
    path.waypoint_count = nondet_size();
    path.segment_count = nondet_size();
    path.route_count = nondet_size();
    assert(en_path_counts_valid(&path) == (path.waypoint_count <= EN_MAX_WAYPOINTS &&
        path.segment_count <= EN_MAX_SEGMENTS && path.route_count <= EN_MAX_ROUTES));
    assert(!en_path_counts_valid(NULL));
}

void harness_plan_counts(void)
{
    en_apply_plan_t plan;
    plan.command_count = EN_MAX_PLAN_COMMANDS + 1;
    plan.rollback_command_count = 0;
    assert(en_apply_plan_run(&plan, true) == EN_ERR_INVALID_ARGUMENT);
    assert(en_apply_plan_write_shell_script(&plan, "unused") == EN_ERR_INVALID_ARGUMENT);
    assert(en_apply_plan_write_swanctl_conf(&plan, "unused") == EN_ERR_INVALID_ARGUMENT);
    plan.command_count = 0;
    plan.rollback_command_count = (size_t)-1;
    assert(en_apply_plan_run(&plan, true) == EN_ERR_INVALID_ARGUMENT);
    assert(en_apply_plan_write_shell_script(&plan, "unused") == EN_ERR_INVALID_ARGUMENT);
    assert(en_apply_plan_write_swanctl_conf(&plan, "unused") == EN_ERR_INVALID_ARGUMENT);
}
