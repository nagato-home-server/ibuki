#include "eventnet/vpp_api_transport.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *program)
{
    printf("usage: %s [application-name] [chroot-prefix]\n", program);
    printf("  opens VPP Binary API, reports its receive fd, then closes it\n");
}

int main(int argc, char **argv)
{
    if (argc > 3 || (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))) {
        usage(argv[0]);
        return argc > 3 ? 2 : 0;
    }
    const char *application_name = argc > 1 ? argv[1] : "ibuki-vpp-probe";
    const char *chroot_prefix = argc > 2 ? argv[2] : NULL;
    en_vpp_api_transport_t transport;
    en_vpp_api_transport_init(&transport);
    en_error_code_t err = en_vpp_api_transport_open(&transport, application_name, chroot_prefix, 8, 8);
    if (err != EN_ERR_NONE) {
        fprintf(stderr, "VPP API open failed: %s\n", en_error_code_name(err));
        return 1;
    }
    int fd = -1;
    err = en_vpp_api_transport_get_fd(&transport, &fd);
    if (err != EN_ERR_NONE) {
        fprintf(stderr, "VPP API receive fd failed: %s\n", en_error_code_name(err));
        en_vpp_api_transport_close(&transport);
        return 1;
    }
    printf("VPP API connected: application=%s receive_fd=%d\n", application_name, fd);
    err = en_vpp_api_transport_close(&transport);
    if (err != EN_ERR_NONE) {
        fprintf(stderr, "VPP API close failed: %s\n", en_error_code_name(err));
        return 1;
    }
    printf("VPP API closed\n");
    return 0;
}
