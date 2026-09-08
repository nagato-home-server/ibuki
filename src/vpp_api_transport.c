#include "eventnet/vpp_api_transport.h"

#include <stdio.h>
#include <string.h>

void en_vpp_api_transport_init(en_vpp_api_transport_t *transport)
{
    if (transport == NULL) return;
    memset(transport, 0, sizeof(*transport));
}

#if defined(EVENTNET_ENABLE_VPP_API)
static vapi_error_e generic_event_callback(vapi_ctx_t context, void *callback_context, vapi_msg_id_t message_id, void *message)
{
    (void)context;
    en_vpp_api_transport_t *transport = callback_context;
    if (transport != NULL && transport->event_callback != NULL) {
        transport->event_callback(transport->event_context, (unsigned int)message_id, message);
    }
    return VAPI_OK;
}
#endif

en_error_code_t en_vpp_api_transport_open(
    en_vpp_api_transport_t *transport,
    const char *application_name,
    const char *chroot_prefix,
    int max_outstanding_requests,
    int response_queue_size
)
{
    if (transport == NULL || application_name == NULL || application_name[0] == '\0' ||
        strlen(application_name) >= sizeof(transport->application_name) ||
        max_outstanding_requests <= 0 || response_queue_size <= 0) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    if (transport->connected) return EN_ERR_STATE_CONFLICT;
#if !defined(EVENTNET_ENABLE_VPP_API)
    (void)chroot_prefix;
    return EN_ERR_NOT_FOUND;
#else
    if (vapi_ctx_alloc(&transport->context) != VAPI_OK) return EN_ERR_STATE_CONFLICT;
    vapi_error_e result = vapi_connect(
        transport->context,
        application_name,
        chroot_prefix,
        max_outstanding_requests,
        response_queue_size,
        VAPI_MODE_BLOCKING,
        true
    );
    if (result != VAPI_OK) {
        vapi_ctx_free(transport->context);
        transport->context = NULL;
        return EN_ERR_STATE_CONFLICT;
    }
    snprintf(transport->application_name, sizeof(transport->application_name), "%s", application_name);
    transport->max_outstanding_requests = max_outstanding_requests;
    transport->response_queue_size = response_queue_size;
    transport->connected = true;
    return EN_ERR_NONE;
#endif
}

en_error_code_t en_vpp_api_transport_dispatch(en_vpp_api_transport_t *transport)
{
    if (transport == NULL || !transport->connected) return EN_ERR_STATE_CONFLICT;
#if defined(EVENTNET_ENABLE_VPP_API)
    return vapi_dispatch_one(transport->context) == VAPI_OK ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
#else
    return EN_ERR_NOT_FOUND;
#endif
}

en_error_code_t en_vpp_api_transport_alloc_message(en_vpp_api_transport_t *transport, size_t message_size, void **message)
{
    if (transport == NULL || message == NULL || message_size == 0 || !transport->connected) return EN_ERR_INVALID_ARGUMENT;
#if defined(EVENTNET_ENABLE_VPP_API)
    *message = vapi_msg_alloc(transport->context, message_size);
    return *message != NULL ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
#else
    (void)message_size;
    *message = NULL;
    return EN_ERR_NOT_FOUND;
#endif
}

en_error_code_t en_vpp_api_transport_send_message(en_vpp_api_transport_t *transport, void *message)
{
    if (transport == NULL || message == NULL || !transport->connected) return EN_ERR_INVALID_ARGUMENT;
#if defined(EVENTNET_ENABLE_VPP_API)
    return vapi_send(transport->context, message) == VAPI_OK ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
#else
    return EN_ERR_NOT_FOUND;
#endif
}

en_error_code_t en_vpp_api_transport_free_message(en_vpp_api_transport_t *transport, void *message)
{
    if (transport == NULL || message == NULL || !transport->connected) return EN_ERR_INVALID_ARGUMENT;
#if defined(EVENTNET_ENABLE_VPP_API)
    vapi_msg_free(transport->context, message);
    return EN_ERR_NONE;
#else
    return EN_ERR_NOT_FOUND;
#endif
}

en_error_code_t en_vpp_api_transport_is_message_available(const en_vpp_api_transport_t *transport, unsigned int message_id, bool *available)
{
    if (transport == NULL || available == NULL || !transport->connected) return EN_ERR_INVALID_ARGUMENT;
#if defined(EVENTNET_ENABLE_VPP_API)
    *available = vapi_is_msg_available(transport->context, (vapi_msg_id_t)message_id);
    return EN_ERR_NONE;
#else
    (void)message_id;
    *available = false;
    return EN_ERR_NOT_FOUND;
#endif
}

en_error_code_t en_vpp_api_transport_get_fd(const en_vpp_api_transport_t *transport, int *fd)
{
    if (transport == NULL || fd == NULL || !transport->connected) return EN_ERR_INVALID_ARGUMENT;
#if defined(EVENTNET_ENABLE_VPP_API)
    return vapi_get_fd(transport->context, fd) == VAPI_OK ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
#else
    return EN_ERR_NOT_FOUND;
#endif
}

en_error_code_t en_vpp_api_transport_set_event_callback(
    en_vpp_api_transport_t *transport,
    en_vpp_api_event_fn callback,
    void *event_context
)
{
    if (transport == NULL || !transport->connected) return EN_ERR_INVALID_ARGUMENT;
#if defined(EVENTNET_ENABLE_VPP_API)
    transport->event_callback = callback;
    transport->event_context = event_context;
    if (callback == NULL) {
        vapi_clear_generic_event_cb(transport->context);
    } else {
        vapi_set_generic_event_cb(transport->context, generic_event_callback, transport);
    }
    return EN_ERR_NONE;
#else
    (void)callback;
    (void)event_context;
    return EN_ERR_NOT_FOUND;
#endif
}

en_error_code_t en_vpp_api_transport_close(en_vpp_api_transport_t *transport)
{
    if (transport == NULL) return EN_ERR_INVALID_ARGUMENT;
    if (!transport->connected) return EN_ERR_NONE;
#if defined(EVENTNET_ENABLE_VPP_API)
    vapi_clear_generic_event_cb(transport->context);
    if (vapi_disconnect(transport->context) != VAPI_OK) return EN_ERR_STATE_CONFLICT;
    vapi_ctx_free(transport->context);
    transport->context = NULL;
#endif
    transport->event_callback = NULL;
    transport->event_context = NULL;
    transport->connected = false;
    return EN_ERR_NONE;
}

bool en_vpp_api_transport_is_connected(const en_vpp_api_transport_t *transport)
{
    return transport != NULL && transport->connected;
}
