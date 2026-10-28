/* SHM transport bounds failures are synchronous and do not consume a slot. */
#include "fss/transport_abi.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef fss_transport_t *(*create_fn_t)(const char *);
typedef void (*destroy_fn_t)(fss_transport_t *);

static int check_transport_abi(void *dl)
{
    fss_transport_abi_version_fn_t version =
        (fss_transport_abi_version_fn_t)dlsym(dl, "fss_transport_abi_version");
    fss_transport_capabilities_fn_t capabilities =
        (fss_transport_capabilities_fn_t)dlsym(dl, "fss_transport_capabilities");
    return version && capabilities &&
           fss_transport_abi_compatible(version(), capabilities());
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    void *dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!dl || !check_transport_abi(dl)) return 1;
    create_fn_t create = (create_fn_t)dlsym(dl, "fss_transport_create");
    destroy_fn_t destroy = (destroy_fn_t)dlsym(dl, "fss_transport_destroy");
    if (!create || !destroy) return 1;

    char channel[64];
    snprintf(channel, sizeof(channel), "hardening_%ld", (long)getpid());
    fss_transport_t *t = create(NULL);
    fss_channel_t *ch = t ? t->channel(t->self, channel, 0, 1, NULL, 0) : NULL;
    fss_endpoint_t ep = {0};
    uint8_t bytes[4097] = {0};
    fss_carrier_t oversized = { .id = UINT64_C(1), .data_ptr = bytes,
                                .data_size = sizeof(bytes) };
    int ok = ch && t->attach(t->self, ch, &ep, FSS_ROLE_INITIATOR) == FSS_SEND_OK &&
             ep.send && ep.send(ep.send_ctx, &oversized) == 3;

    /* Meta that cannot be flattened must also fail before it reserves a slot. */
    uint8_t invalid_meta[2049] = {0};
    fss_carrier_t bad_meta = { .id = UINT64_C(2), .meta_ptr = invalid_meta,
                               .meta_size = sizeof(invalid_meta),
                               .data_ptr = bytes, .data_size = 1 };
    fss_carrier_t missing_meta = { .id = UINT64_C(3), .meta_size = 1,
                                   .data_ptr = bytes, .data_size = 1 };
    ok = ok && ep.send(ep.send_ctx, &bad_meta) == 3 &&
         ep.send(ep.send_ctx, &missing_meta) == 3;
    destroy(t);
    dlclose(dl);
    return ok ? 0 : 1;
}
