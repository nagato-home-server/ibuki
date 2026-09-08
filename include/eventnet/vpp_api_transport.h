#ifndef EVENTNET_VPP_API_TRANSPORT_H
#define EVENTNET_VPP_API_TRANSPORT_H

#include "eventnet/types.h"

#if defined(EVENTNET_ENABLE_VPP_API)
#include <vapi/vapi.h>
#endif

typedef void (*en_vpp_api_event_fn)(void *context, unsigned int message_id, void *message);

typedef struct {
#if defined(EVENTNET_ENABLE_VPP_API)
    vapi_ctx_t context;
#else
    void *context;
#endif
    bool connected;
    char application_name[EN_MAX_ID_LEN];
    int max_outstanding_requests;
    int response_queue_size;
    en_vpp_api_event_fn event_callback;
    void *event_context;
} en_vpp_api_transport_t;

void en_vpp_api_transport_init(en_vpp_api_transport_t *transport);
en_error_code_t en_vpp_api_transport_open(
    en_vpp_api_transport_t *transport,
    const char *application_name,
    const char *chroot_prefix,
    int max_outstanding_requests,
    int response_queue_size
);
en_error_code_t en_vpp_api_transport_dispatch(en_vpp_api_transport_t *transport);
en_error_code_t en_vpp_api_transport_alloc_message(en_vpp_api_transport_t *transport, size_t message_size, void **message);
en_error_code_t en_vpp_api_transport_send_message(en_vpp_api_transport_t *transport, void *message);
en_error_code_t en_vpp_api_transport_free_message(en_vpp_api_transport_t *transport, void *message);
en_error_code_t en_vpp_api_transport_is_message_available(const en_vpp_api_transport_t *transport, unsigned int message_id, bool *available);
en_error_code_t en_vpp_api_transport_get_fd(const en_vpp_api_transport_t *transport, int *fd);
en_error_code_t en_vpp_api_transport_set_event_callback(
    en_vpp_api_transport_t *transport,
    en_vpp_api_event_fn callback,
    void *event_context
);
en_error_code_t en_vpp_api_transport_close(en_vpp_api_transport_t *transport);
bool en_vpp_api_transport_is_connected(const en_vpp_api_transport_t *transport);

#endif
