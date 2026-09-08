#ifndef EVENTNET_YAML_CONFIG_H
#define EVENTNET_YAML_CONFIG_H

#include "eventnet/types.h"

typedef struct {
    en_node_t nodes[EN_MAX_NODES];
    size_t node_count;
    en_tunnel_t tunnels[EN_MAX_TUNNELS];
    size_t tunnel_count;
    en_path_t paths[EN_MAX_PATHS];
    size_t path_count;
    en_intent_t intents[EN_MAX_CANDIDATES];
    size_t intent_count;
    en_vpp_edge_t vpp_edges[EN_MAX_VPP_EDGES];
    size_t vpp_edge_count;
} en_yaml_config_t;

en_error_code_t en_yaml_config_load_file(const char *filename, en_yaml_config_t *config, char *error, size_t error_len);
en_error_code_t en_yaml_config_validate(const en_yaml_config_t *config, char *error, size_t error_len);

#endif
