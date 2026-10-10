#include "../../src/yaml_config.c"

#include <assert.h>

size_t nondet_size(void);

void harness_yaml_counts(void)
{
    size_t node_count = nondet_size();
    size_t path_count = nondet_size();
    size_t tunnel_count = nondet_size();
    size_t intent_count = nondet_size();
    size_t vpp_edge_count = nondet_size();
    assert(config_counts_valid(node_count, path_count, tunnel_count, intent_count, vpp_edge_count) ==
        (node_count <= EN_MAX_NODES && path_count <= EN_MAX_PATHS && tunnel_count <= EN_MAX_TUNNELS &&
         intent_count <= EN_MAX_CANDIDATES && vpp_edge_count <= EN_MAX_VPP_EDGES));
}

void harness_yaml_context(void)
{
    en_yaml_config_t config;
    yaml_parse_state_t state = {0};
    state.top = YAML_SECTION_INTENTS;
    char line[] = "  path_selection:";
    assert(parse_line(&config, &state, line, 1, NULL, 0) == EN_ERR_INVALID_ARGUMENT);
    assert(state.intent == NULL);
}

void harness_yaml_dedent(void)
{
    en_yaml_config_t config;
    en_intent_t intent = {0};
    yaml_parse_state_t state = {0};
    state.top = YAML_SECTION_INTENTS;
    state.intent = &intent;
    state.context = YAML_CONTEXT_INTENT_REQUIRED_WAYPOINTS;
    state.list_parent = YAML_CONTEXT_INTENT_CONSTRAINTS;
    state.list_indent = 8;
    char line[] = "        forbidden_waypoints:";
    assert(parse_line(&config, &state, line, 1, NULL, 0) == EN_ERR_NONE);
    assert(state.context == YAML_CONTEXT_INTENT_FORBIDDEN_WAYPOINTS);
    assert(state.list_parent == YAML_CONTEXT_INTENT_CONSTRAINTS);
}

void harness_yaml_boolean(void)
{
    bool value = true;
    assert(!parse_bool("tru", &value));
    assert(value);
    assert(parse_bool("false", &value));
    assert(!value);
    assert(parse_bool("yes", &value));
    assert(value);
}
