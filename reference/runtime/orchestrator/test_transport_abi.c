/* Negotiation is outside the frozen fss_transport_t vtable. */
#include "fss/transport_abi.h"

#include <stddef.h>
#include <stdio.h>

int main(void)
{
    int ok = 1;

    /* These are runtime checks as well as the header's compile-time guards. */
    ok = ok && sizeof(fss_transport_t) == 32;
    ok = ok && offsetof(fss_transport_t, self) == 0;
    ok = ok && offsetof(fss_transport_t, channel) == 8;
    ok = ok && offsetof(fss_transport_t, attach) == 16;
    ok = ok && offsetof(fss_transport_t, send_to) == 24;

    ok = ok && fss_transport_abi_compatible(FSS_TRANSPORT_ABI_VERSION,
                                              FSS_TRANSPORT_CAP_CHANNEL_ABI);
    /* A different major and every absent required capability must reject. */
    ok = ok && !fss_transport_abi_compatible(UINT32_C(0x00020000),
                                               FSS_TRANSPORT_CAP_CHANNEL_ABI);
    ok = ok && !fss_transport_abi_compatible(FSS_TRANSPORT_ABI_VERSION,
                                               FSS_TRANSPORT_CAP_ATTACH |
                                               FSS_TRANSPORT_CAP_SEND_TO);
    ok = ok && !fss_transport_abi_compatible(FSS_TRANSPORT_ABI_VERSION,
                                               FSS_TRANSPORT_CAP_CHANNEL |
                                               FSS_TRANSPORT_CAP_SEND_TO);
    ok = ok && !fss_transport_abi_compatible(FSS_TRANSPORT_ABI_VERSION,
                                               FSS_TRANSPORT_CAP_CHANNEL |
                                               FSS_TRANSPORT_CAP_ATTACH);
    if (!ok) fprintf(stderr, "transport ABI negotiation/layout test failed\n");
    return ok ? 0 : 1;
}
