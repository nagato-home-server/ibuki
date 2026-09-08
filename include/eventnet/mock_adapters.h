#ifndef EVENTNET_MOCK_ADAPTERS_H
#define EVENTNET_MOCK_ADAPTERS_H

#include "eventnet/controller.h"

typedef struct {
    bool fail_next_update;
    char traffic_keys[EN_MAX_CANDIDATES][EN_MAX_TRAFFIC_KEY_LEN];
    char active_paths[EN_MAX_CANDIDATES][EN_MAX_ID_LEN];
    size_t active_count;
    size_t install_count;
    size_t remove_count;
} en_vpp_mock_t;

typedef struct {
    en_path_health_t overrides[EN_MAX_PATHS];
    size_t override_count;
    bool require_interface_and_route;
} en_health_probe_mock_t;

en_strongswan_adapter_t en_strongswan_mock_adapter(void);
en_vpp_adapter_t en_vpp_mock_adapter(en_vpp_mock_t *mock);
en_health_probe_t en_health_probe_mock_adapter(en_health_probe_mock_t *mock);
void en_health_probe_mock_set(en_health_probe_mock_t *mock, en_path_health_t health);

#endif
