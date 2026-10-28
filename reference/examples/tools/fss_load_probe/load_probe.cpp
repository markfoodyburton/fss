/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss_load_probe — in-proc receive-path smoke test for a SystemC FSS artefact.
 *
 * Opens an artefact .dylib via the in-proc transport and exercises the data
 * path + lifecycle + the three receive paths plus reject.  The in-proc
 * transport is a direct-wire (channel/attach are NULL): edges are wired by
 * resolving the peer endpoint through the placement facility and installing
 * ep->send = peer->receive, with no transport on the path:
 *
 *   Bind data     — fss_place_resolve("spi/socket") + ep_data.send = peer->receive
 *                   installs ep_data's outbound channel; data carriers go via
 *                   fss_send(&ep_data, ...).
 *   Start         — sim::event{Started} delivered to "spi/fss/sim" via a
 *                   placement resolve + direct receive → elaborate + run.
 *   Path 1 (fast) — carrier carries a live tlm_generic_payload; id = FSS_TLM_ID.
 *                   The receiver static_casts and dispatches it (zero copy).
 *   Path 3/4      — a FOREIGN id with the portable flat layout + meta; the
 *                   receiver resolves the bus:: fields by qname (miss=path 4,
 *                   then hit=path 3) and populates a payload.  Sent twice.
 *   Reject        — unknown id, no meta: must NOT be dispatched.
 *   Stop          — sim::event{Stopped} via send_to("fss/sim").
 *
 * It builds a real tlm_generic_payload for Path 1, so this driver is a SystemC
 * program (entry point sc_main; it never calls sc_start — it only constructs
 * payloads and drives the transport).
 */
#include "fss/transport_abi.h"
#include "fss/runtime/placement.h"
#include "fss/known_carriers/tlm_id.h"   /* generated: FSS_TLM_ID / FSS_TLM_META (live carrier) */
#include "fss/protocols/sim.h"      /* generated: FSS_SIM_ID */

#include <dlfcn.h>

#include <systemc>
#include <tlm>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Local convenience: deliver on a bound endpoint's outbound channel.
 * The ABI is just the struct (fss/endpoint.h) — this one-liner is caller
 * sugar, not part of the ABI. */
static inline int fss_send(fss_endpoint_t *ep, fss_carrier_t *c)
{
    if (!ep || !ep->send) return FSS_SEND_UNBOUND;
    return ep->send(ep->send_ctx, c);
}

static constexpr uint64_t FOREIGN_ID = 0xF0F0F0F0F0F0F0F0ULL;  /* != FSS_TLM_ID */

/* sim::EventCode values (from atoms/sim/EventCode.libsonnet; not emitted by fss_compile.py) */
static constexpr uint32_t SIM_EVENTCODE_STARTED = 2;
static constexpr uint32_t SIM_EVENTCODE_STOPPED = 5;

/* Flat-buffer offsets matching known_carriers/tlm/transaction.jsonnet — the
 * COMPLETE (live) carrier layout: real tlm_generic_payload offsets (vtable/
 * core::TypeId at 0).  Used by the path34/bench driver to build a "foreign"
 * flat buffer the receiver resolves by qname from FSS_TLM_META. */
namespace {
  enum : uint32_t {
    OFF_TYPEID   =  0,
    OFF_ADDRESS  =  8,
    OFF_COMMAND  = 16,
    OFF_DATA     = 24,
    OFF_LENGTH   = 32,
    OFF_STATUS   = 36,
    OFF_DMI      = 40,
    OFF_BE       = 48,
    OFF_BE_LEN   = 56,
    OFF_STREAMING = 60,
    OFF_GPOPT    = 64,
    TLM_DATA_SIZE = 72,
  };
  constexpr uint32_t CMD_WRITE  = 1;
  constexpr int32_t  STATUS_OK  = 1;   /* TLM_OK_RESPONSE */
}

/* Path 1: a real generic payload, cast-dispatched (zero copy). */
static bool path1(fss_transport_t *t, fss_endpoint_t *ep)
{
    uint8_t wbuf[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    tlm::tlm_generic_payload gp;
    gp.set_command(tlm::TLM_WRITE_COMMAND);
    gp.set_address(0);
    gp.set_data_ptr(wbuf);
    gp.set_data_length(4);
    gp.set_streaming_width(4);
    gp.set_byte_enable_ptr(nullptr);
    gp.set_byte_enable_length(0);
    gp.set_dmi_allowed(false);
    gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    fss_carrier_t c {};
    c.id = FSS_TLM_ID; c.data_ptr = &gp;
    c.meta_ptr = (void *)FSS_TLM_META; c.meta_size = FSS_TLM_META_LEN;
    std::printf("[path1] send GP id=FSS_TLM_ID data_ptr=%p\n", (void *)&gp);
    if (fss_send(ep, &c) != FSS_SEND_OK ||
        gp.get_response_status() != tlm::TLM_OK_RESPONSE)
        return false;

    uint8_t rbuf[4] = {};
    tlm::tlm_generic_payload rgp;
    rgp.set_command(tlm::TLM_READ_COMMAND);
    rgp.set_address(0);
    rgp.set_data_ptr(rbuf);
    rgp.set_data_length(4);
    rgp.set_streaming_width(4);
    rgp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    fss_carrier_t rc {};
    rc.id = FSS_TLM_ID; rc.data_ptr = &rgp;
    rc.meta_ptr = (void *)FSS_TLM_META; rc.meta_size = FSS_TLM_META_LEN;
    if (fss_send(ep, &rc) != FSS_SEND_OK ||
        rgp.get_response_status() != tlm::TLM_OK_RESPONSE ||
        std::memcmp(rbuf, wbuf, 4) != 0)
        return false;

    std::printf("[path1] OK — cast round-trip; the sender's GP was dispatched\n");
    return true;
}

/* Path 4 then 3: foreign id, portable flat layout + meta → resolve by qname. */
static bool path34(fss_transport_t *t, fss_endpoint_t *ep)
{
    for (int pass = 0; pass < 2; ++pass) {
        uint8_t payload[] = { 0xCA, 0xFE, 0xBA, 0xBE };
        uint8_t flat[TLM_DATA_SIZE] = {};
        uint32_t cmd = CMD_WRITE, len = 4;
        uint64_t addr = 0;
        int32_t  init_st = 0xFF;
        std::memcpy(flat + OFF_ADDRESS, &addr,    sizeof(addr));
        std::memcpy(flat + OFF_COMMAND, &cmd,     sizeof(cmd));
        std::memcpy(flat + OFF_LENGTH,  &len,     sizeof(len));
        uint8_t *pp = payload;                       /* local-ptr: 8-byte pointer */
        std::memcpy(flat + OFF_DATA,    &pp,      sizeof(pp));
        std::memcpy(flat + OFF_STATUS,  &init_st, sizeof(init_st));

        fss_carrier_t c {};
        c.id = FOREIGN_ID; c.data_ptr = flat; c.data_size = TLM_DATA_SIZE;
        c.meta_ptr = (void *)FSS_TLM_META; c.meta_size = FSS_TLM_META_LEN;
        std::printf("[path%d] send foreign flat id=0x%llx %s\n",
                    pass == 0 ? 4 : 3, (unsigned long long)FOREIGN_ID,
                    pass == 0 ? "(cache miss → lookup)" : "(cache hit)");
        int32_t got_st = 0;
        std::memcpy(&got_st, flat + OFF_STATUS, sizeof(got_st));
        if (fss_send(ep, &c) != FSS_SEND_OK) return false;
        std::memcpy(&got_st, flat + OFF_STATUS, sizeof(got_st));
        if (got_st != STATUS_OK) {
            std::printf("[path%d] FAIL: status=%d want %d\n", pass == 0 ? 4 : 3, got_st, STATUS_OK);
            return false;
        }
    }
    std::printf("[path3/4] OK — resolved bus:: fields from meta (miss then cached)\n");
    return true;
}

/* Reject: unknown id, no meta — must not be dispatched (status untouched). */
static bool reject(fss_transport_t *t, fss_endpoint_t *ep)
{
    uint8_t flat[TLM_DATA_SIZE] = {};
    int32_t init_st = 0xFF;
    std::memcpy(flat + OFF_STATUS, &init_st, sizeof(init_st));
    fss_carrier_t c {};
    c.id = 0; c.data_ptr = flat; c.data_size = TLM_DATA_SIZE; c.meta_ptr = nullptr;
    std::printf("[reject] send unknown id, no meta\n");
    fss_send(ep, &c);
    int32_t got_st = 0;
    std::memcpy(&got_st, flat + OFF_STATUS, sizeof(got_st));
    if (got_st != (int32_t)0xFF) return false;
    std::printf("[reject] OK — not dispatched, status untouched\n");
    return true;
}

/* Optional micro-measurement (FSS_BENCH=1): Path-1 cast vs Path-3 cached.
 * Both incur the same cross-kernel dispatch hop; the delta isolates the FSS
 * translation cost (cast vs cached-resolve + scalar populate). */
static void bench(fss_transport_t *t, fss_endpoint_t *ep)
{
    const int N = 50000;
    using clk = std::chrono::steady_clock;

    uint8_t buf[4] = { 1, 2, 3, 4 };
    tlm::tlm_generic_payload gp;
    gp.set_command(tlm::TLM_WRITE_COMMAND); gp.set_address(0);
    gp.set_data_ptr(buf); gp.set_data_length(4); gp.set_streaming_width(4);
    fss_carrier_t c1 {};
    c1.id = FSS_TLM_ID; c1.data_ptr = &gp;
    c1.meta_ptr = (void *)FSS_TLM_META; c1.meta_size = FSS_TLM_META_LEN;

    auto a = clk::now();
    for (int i = 0; i < N; ++i) { gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
                                  fss_send(ep, &c1); }
    auto b = clk::now();

    uint8_t flat[TLM_DATA_SIZE] = {};
    uint32_t cmd = CMD_WRITE, len = 4;
    std::memcpy(flat + OFF_COMMAND, &cmd, sizeof(cmd));
    std::memcpy(flat + OFF_LENGTH,  &len, sizeof(len));
    uint8_t *bp = buf;                                /* local-ptr: 8-byte pointer */
    std::memcpy(flat + OFF_DATA,    &bp,  sizeof(bp));
    fss_carrier_t c3 {};
    c3.id = FOREIGN_ID; c3.data_ptr = flat; c3.data_size = TLM_DATA_SIZE;
    c3.meta_ptr = (void *)FSS_TLM_META; c3.meta_size = FSS_TLM_META_LEN;
    fss_send(ep, &c3);   /* warm the resolver cache */

    auto c = clk::now();
    for (int i = 0; i < N; ++i) fss_send(ep, &c3);
    auto d = clk::now();

    auto ns = [](auto x, auto y, int n) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(y - x).count() / n; };
    long p1 = ns(a, b, N), p3 = ns(c, d, N);
    std::printf("[bench] N=%d  path1(cast)=%ld ns/send  path3(cached)=%ld ns/send  "
                "delta=%ld ns (FSS translation; both include the cross-kernel hop)\n",
                N, p1, p3, p3 - p1);
}

int sc_main(int argc, char *argv[])
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <transport-module> <artefact-dll>\n", argv[0]);
        return 2;
    }

    /* Load the transport plugin. */
    void *dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        std::fprintf(stderr, "dlopen transport %s: %s\n", argv[1], dlerror());
        return 1;
    }
    auto *abi_version = reinterpret_cast<fss_transport_abi_version_fn_t>
        (dlsym(dl, "fss_transport_abi_version"));
    auto *capabilities = reinterpret_cast<fss_transport_capabilities_fn_t>
        (dlsym(dl, "fss_transport_capabilities"));
    if (!abi_version || !capabilities ||
        !fss_transport_abi_compatible(abi_version(), capabilities())) {
        std::fprintf(stderr, "transport ABI query/capability check failed\n");
        dlclose(dl);
        return 1;
    }
    typedef fss_transport_t *(*create_fn_t)(const char *);
    typedef void             (*destroy_fn_t)(fss_transport_t *);
    auto *create_fn  = reinterpret_cast<create_fn_t> (dlsym(dl, "fss_transport_create"));
    auto *destroy_fn = reinterpret_cast<destroy_fn_t>(dlsym(dl, "fss_transport_destroy"));
    if (!create_fn || !destroy_fn) {
        std::fprintf(stderr, "dlsym transport factory: %s\n", dlerror());
        dlclose(dl);
        return 1;
    }

    fss_transport_t *t = create_fn(NULL);
    if (!t) { std::fprintf(stderr, "transport create failed\n"); dlclose(dl); return 1; }

    /* Open the artefact as node "spi" via the placement facility. */
    fss_node_t *node = fss_place_construct("spi", argv[2]);
    int rc = 1;
    if (!node) {
        std::fprintf(stderr, "open failed\n");
        destroy_fn(t); dlclose(dl);
        return rc;
    }

    /* Bind the probe's local data endpoint to the artefact's "socket". */
    fss_endpoint_t ep_data{};
    {
        fss_endpoint_t *peer = fss_place_resolve("spi/socket");
        if (!peer) {
            std::fprintf(stderr, "bind(spi/socket) failed\n");
            fss_place_destroy(node);
            destroy_fn(t); dlclose(dl);
            return rc;
        }
        ep_data.send = peer->receive; ep_data.send_ctx = peer->self;
    }

    /* Lifecycle pings via placement resolve + direct deliver:
     * START — sim::event{Started} → elaborate + run. */
    uint8_t sim_buf[4] = {};
    uint32_t started_code = SIM_EVENTCODE_STARTED;  /* 2 */
    std::memcpy(sim_buf, &started_code, sizeof(started_code));
    fss_carrier_t started_carrier {};
    started_carrier.id = FSS_SIM_ID; started_carrier.data_ptr = sim_buf; started_carrier.data_size = sizeof(sim_buf);
    started_carrier.meta_ptr = (void *)FSS_SIM_META; started_carrier.meta_size = FSS_SIM_META_LEN;
    {
        fss_endpoint_t *sim_ep = fss_place_resolve("spi/fss/sim");
        if (!sim_ep || !sim_ep->receive ||
            sim_ep->receive(sim_ep->self, &started_carrier) != FSS_SEND_OK) {
            std::fprintf(stderr, "send_to(spi/fss/sim, Started) failed\n");
            fss_place_destroy(node);
            destroy_fn(t); dlclose(dl);
            return rc;
        }
    }

    if (path1(t, &ep_data) && path34(t, &ep_data) && reject(t, &ep_data)) {
        std::printf("[OK] %s (path1 cast + path3/4 resolve + reject)\n", argv[2]);
        rc = 0;
        if (std::getenv("FSS_BENCH")) bench(t, &ep_data);
    }

    /* STOP: sim::event{Stopped} → signal_stop() (idempotent), one-shot. */
    uint8_t stop_buf[4] = {};
    uint32_t stopped_code = SIM_EVENTCODE_STOPPED;  /* 5 */
    std::memcpy(stop_buf, &stopped_code, sizeof(stopped_code));
    fss_carrier_t stopped_carrier {};
    stopped_carrier.id = FSS_SIM_ID; stopped_carrier.data_ptr = stop_buf; stopped_carrier.data_size = sizeof(stop_buf);
    stopped_carrier.meta_ptr = (void *)FSS_SIM_META; stopped_carrier.meta_size = FSS_SIM_META_LEN;
    {
        fss_endpoint_t *sim_ep = fss_place_resolve("spi/fss/sim");
        if (sim_ep && sim_ep->receive) sim_ep->receive(sim_ep->self, &stopped_carrier);
    }

    fss_place_destroy(node);
    destroy_fn(t);
    dlclose(dl);
    return rc;
}
