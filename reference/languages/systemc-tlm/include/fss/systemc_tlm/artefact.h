/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * fss/systemc_tlm/artefact.h — SystemC-TLM binding's artefact wrapper.
 *
 * Turns a sc_module class into an FSS-deployable artefact per the
 * artefact ABI convention in docs/spec/05-abi-conventions.md.  One
 * instance per loaded module: owns the SystemC kernel thread, walks
 * the SystemC hierarchy under top_ on get_endpoint(local_name), and
 * auto-wraps the discovered TLM target socket as an FSS endpoint
 * speaking the TLM carrier (known_carriers/tlm/).
 *
 * Lifecycle (bind-then-send, increment 1.6):
 *   fss_create       → constructs Module top(name) on the kernel thread,
 *                      then parks: the bind window is open.  Host thread
 *                      BLOCKS until top is constructed (constructed_ set).
 *   get_endpoint     → resolves the constructed hierarchy (sc_find_object
 *                      is safe pre-sc_start over the constructed tree).
 *                      get_endpoint("fss/sim") returns a binding-provided
 *                      standard control port; no model involvement needed.
 *   bind(local,"fss/sim") OR send_to("fss/sim", …) → a peer delivers a
 *                      sim::event carrier to the fss/sim control port.
 *   send_to("fss/config", config::param_request) → set a model CCI parameter
 *                      by name.  Sent in the bind window, so it is applied
 *                      pre-elaboration (the in-band replacement for any
 *                      C++ "configure" hook).  See config_receive.
 *   sim::event{Bound}  → "you are wired" (sent by whoever bound this node's
 *                      endpoints).  Arms the START BARRIER (see below).
 *   sim::event{Elaborated} → received from a DOWNSTREAM time-peer: "I am
 *                      running".  Counts toward this node's downstream-ready set.
 *   sim::event{Stopped} → signals stop without destroy.
 *   fss_destroy      → signals stop (idempotent); joins kernel thread.
 *
 * START BARRIER — the upward readiness wave (sim-status, on its OWN channel).
 *   A node may only run once every node it synchronously depends on downstream
 *   is already running, so the federation starts SINK-FIRST and the wave flows
 *   UP: a node closes its bind window (elaborate + run) when it is Bound AND every
 *   downstream peer has sent Elaborated on one of its fss/sim/<tag> sinks; the sink
 *   (no downstream) runs as soon as it is Bound.  Having elaborated and armed its
 *   window drivers — and with its downstream already running — it announces
 *   Elaborated UP on its fss/sim port (bound to its upstream peer, or to the
 *   orchestrator if it is the source) and only THEN enters sc_start().  Its
 *   upstream begins on that signal, so by the time the upstream sends a window or
 *   a poll this node is already servicing it.  sim-status and time-sync travel on
 *   SEPARATE channels — fss/sim is a dedicated, delivery-confirmed lifecycle link;
 *   fss/time carries only fire-and-forget windows, never a readiness signal.
 *
 * Each DLL statically embeds its own SystemC and is loaded RTLD_LOCAL,
 * so sc_curr_simcontext is per-DLL.  TLM b_transport is called directly
 * on the target socket's fw interface from the wrapper's dispatch
 * SC_THREAD.  The host's send() crosses the thread boundary by ringing a
 * suspending sc_prim_channel doorbell (async_request_update) — the only
 * thread-safe way to poke the kernel from a foreign thread — so the kernel
 * OS-blocks waiting for work rather than spinning a keep-alive thread.
 * See wrapper_hub::doorbell.
 *
 * Endpoint discovery: get_endpoint("local_name") builds the full path
 * top_->name() + "." + local_name, calls sc_find_object, and wraps the
 * object as an FSS endpoint.  A socket's direction maps to the endpoint's
 * direction — the two halves of the symmetric endpoint:
 *
 *   TLM TARGET socket    → an endpoint that RECEIVES carriers.  Inbound:
 *                          a delivered carrier is turned into a b_transport
 *                          on the model's target socket (endpoint.receive).
 *   TLM INITIATOR socket  → an endpoint that SENDS carriers.  Outbound: the
 *                          model's own b_transport is bridged to a carrier
 *                          and pushed through endpoint.send (installed by
 *                          the transport at bind).  See initiator_stub.
 *
 * So a federation author does the same thing for either kind — get_endpoint
 * then bind — and the socket kind decides which way carriers flow.  On
 * success the wrapper caches and returns a stable fss_endpoint_t *; on a name
 * that is neither a target nor an initiator socket it returns nullptr.
 * (sc_signal and other endpoint kinds will be added as protocols need them.)
 */
#ifndef FSS_SYSTEMC_TLM_ARTEFACT_H
#define FSS_SYSTEMC_TLM_ARTEFACT_H

#include <systemc>
#include <tlm>

#include <cci_configuration>
#include <cci/utils/broker.h>

#include "fss/endpoint.h"
#include "fss/transport_abi.h"   /* FSS_SEND_* status codes */
#include "fss/known_carriers/tlm_id.h"   /* generated: FSS_TLM_ID / FSS_TLM_META (the live, TypeId-bearing carrier) */
#include "fss/c/meta.h"                  /* Path-B: resolve fields by qname */
#include "fss/protocols/sim.h"      /* generated: FSS_SIM_ID */
#include "fss/protocols/config.h"   /* generated: FSS_CONFIG_ID */
#include "fss/protocols/time_sync.h" /* generated: FSS_TIME_ID */
#include "fss/systemc_tlm/sc_sync_window.h" /* vendored: kernel time-window gate */

#include <tlm_utils/tlm_quantumkeeper.h>
#include <memory>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>

namespace fss::systemc_tlm {

/* The TLM carrier id (FSS_TLM_ID) is the COMPLETE live carrier: the bus:: fields
 * at their real tlm_generic_payload offsets PLUS a core::TypeId atom (the vtable
 * marker at offset 0).  It is a static compile-time constant — the TypeId atom
 * is in the meta structurally, so id == hash(meta) holds with no runtime
 * stamping.
 *
 * Two safety layers let an in-proc peer cast data_ptr straight to the live GP:
 *   1. id == FSS_TLM_ID — a cheap, NON-dereferencing gate.  Because core::TypeId
 *      is in the hash, this id means "the live form."  A wire transport drops the
 *      non_serializable TypeId atom and re-hashes to a DIFFERENT (flat) id, so a
 *      serialised carrier never carries FSS_TLM_ID → the cast is only ever
 *      attempted on a genuine live object, never on flat bytes.
 *   2. typeid(*gp) == typeid(tlm_generic_payload) — a runtime RTTI confirm THROUGH
 *      the vtable the TypeId atom points at.  Catches the one case the id can't:
 *      two DLLs in one process compiled against different SystemC ABIs that share
 *      the carrier shape (same id, different layout).  Proven to work across
 *      RTLD_LOCAL .dylibs.  On mismatch we fall back to the meta-walk path. */
inline bool tlm_is_live(const fss_carrier_t *c)
{
    if (c->id != FSS_TLM_ID || !c->data_ptr) return false;
    /* Safe: the id gate guarantees data_ptr is a live polymorphic object, so the
     * vptr read by typeid is valid.  Confirm it is OUR tlm_generic_payload. */
    return typeid(*static_cast<tlm::tlm_generic_payload *>(c->data_ptr))
           == typeid(tlm::tlm_generic_payload);
}

/* Resolve a field's byte offset in a carrier by walking its meta for the named
 * atom.  This is THE universal consumption primitive (the C++ binding's "get me
 * atom X" call), used for every interface except the TLM live fast path.
 *
 * The carrier's meta is ALWAYS present by the time it reaches the artefact: a
 * model attaches it when it builds a carrier, and a transport that strips it on
 * the wire as an optimisation re-attaches it before delivery.  So we always walk
 * `c->meta_ptr` — the carrier's OWN self-description — and never a local copy.
 * The carrier id is used ONLY as the resolve CACHE KEY (so repeated lookups for
 * the same carrier shape are O(1)); it is an optimisation of the walk, never a
 * semantic gate.  (The one exception is the TLM fast path in endpoint_receive,
 * where the id gates a zero-copy static_cast to a live object.) */
inline bool fss_field_offset(fss_meta_cache_t *cache, const fss_carrier_t *c,
                             const char *qname, uint32_t *off, uint32_t *sz)
{
    if (!c->meta_ptr || !c->meta_size) return false;   /* meta MUST be present */
    return fss_meta_resolve(cache, c->id, c->meta_ptr, c->meta_size, qname, off, sz) != 0;
}

/* Optional model hooks.  A wrapped SystemC module that needs to EMIT a typed FSS
 * carrier (one the binding can't auto-bridge from a TLM socket — e.g. a CAN
 * controller deriving a can::frame) inherits fss_model_hooks and, in fss_attach,
 * declares its outbound endpoint(s) via the fss_emitter the binding passes.  It
 * keeps the returned endpoint pointer and sends on it from its own process
 * (b_transport).  Models that don't emit a custom carrier ignore this entirely
 * (the binding dynamic_casts and no-ops), so existing models are unaffected. */
struct fss_emitter {
    /* Create a bare OUTBOUND endpoint named `name` (receive=null; send installed
     * by the transport at bind, advertised via get_endpoint).  Stable pointer. */
    virtual fss_endpoint_t *declare_outbound(const char *name) = 0;
    virtual ~fss_emitter() = default;
};
struct fss_model_hooks {
    /* Called once, after construction, before elaboration: declare outbounds. */
    virtual void fss_attach(fss_emitter & /*binding*/) {}
    /* Called on the KERNEL thread after ALL known downstream timing peers have
     * DETACHED (opened their windows).  `port_index` is the fss/time port whose
     * first detach completed that quorum (0 = "fss/time", 1.. =
     * "fss/time/<tag>" in allocation order).  An upstream peer detach does not
     * call this hook.  Return true to stop this node too — the policy that
     * cascades teardown sink→source through fss/time.  Default false (ignore —
     * keep running).  A node that self-terminates (for example a sink that has
     * received everything) just calls sc_stop() itself. */
    virtual bool fss_peer_detached(int /*port_index*/) { return false; }
    /* Called when a config::param_request carries a JSON ParamValue (the CAN
     * platform's config form: a flat object of integer values, e.g.
     * {"frames":1000,"period_us":2}).  Called before SIM_STARTED, so the model
     * can adjust run parameters pre-run.  Default: ignore (keep baked-in
     * defaults).  Integer/CCI Set carriers still go through apply_config's CCI
     * path; this hook is only for the JSON form. */
    virtual void fss_configure(const char * /*json*/) {}
    virtual ~fss_model_hooks() = default;
};

template <class Module>
class artefact : public fss_emitter {
    struct endpoint_entry;   /* defined below; referenced by new_endpoint() */
public:
    explicit artefact(const char *name)
        : name_(name ? name : "")
    {
        /* A deployable artefact instance must be named: the name roots the
         * SystemC hierarchy (Module top(name_)) and prefixes CCI parameter
         * paths in fss/config.  An absent/empty name is a deployment error,
         * not something to paper over with a default. */
        if (name_.empty())
            SC_REPORT_FATAL("fss/systemc_tlm",
                            "artefact instance name must be non-empty");

        kernel_ = std::thread([this]() { run(); });

        /* Block until the kernel thread has constructed Module.  After this the
         * bind window is open: sc_find_object over the constructed hierarchy is
         * safe, endpoints can be resolved, and configuration carriers may be
         * delivered to the fss/config control port (applied pre-elaboration).
         * Elaboration is deferred until start(). */
        std::unique_lock<std::mutex> lk(mu_);
        construct_cv_.wait(lk, [this] { return constructed_; });
    }

    ~artefact()
    {
        signal_stop();
        if (kernel_.joinable())
            kernel_.join();
        for (auto &e : endpoints_)
            if (e.cache) fss_meta_cache_destroy(e.cache);
        if (sim_port_.cache)    fss_meta_cache_destroy(sim_port_.cache);
        if (config_port_.cache) fss_meta_cache_destroy(config_port_.cache);
        for (int i = 0; i < n_sim_down_;  ++i)
            if (sim_down_[i].cache)  fss_meta_cache_destroy(sim_down_[i].cache);
        for (int i = 0; i < n_time_ports_; ++i)
            if (time_ports_[i].cache) fss_meta_cache_destroy(time_ports_[i].cache);
    }

    artefact(const artefact &)            = delete;
    artefact &operator=(const artefact &) = delete;

    /*
     * Walk the SystemC hierarchy under top_ to find local_name
     * (e.g. "socket" or "mem.socket"), wrap the discovered TLM target
     * socket as an FSS endpoint, cache and return a stable pointer.
     * Returns nullptr if not found or not a TLM target socket.
     *
     * "fss/sim" is a standard control port provided by the binding
     * (not model-provided); it is returned on first call and cached.
     * Any other reserved "fss/" name returns nullptr (advertised by
     * presence — null means unsupported, per the spec).
     *
     * Called in the bind window (after construction, before start()).
     * Thread-safe w.r.t. the parked kernel thread: discovery uses
     * sc_find_object over the constructed hierarchy, valid pre-elaboration.
     */
    /* fss_emitter: create a bare outbound endpoint the model emits on.  It joins
     * endpoints_, so get_endpoint(name) returns it (its cache-lookup runs before
     * socket discovery) and the host binds its send. */
    fss_endpoint_t *declare_outbound(const char *name) override
    {
        auto &e = new_endpoint(name);
        e.endpoint.receive = nullptr;   /* outbound: no inbound delivery */
        return &e.endpoint;
    }

    fss_endpoint_t *get_endpoint(const char *local_name)
    {
        if (!local_name) return nullptr;

        /* Standard control ports (binding-provided, not model): resolved and
         * cached on first request.  Any other reserved "fss/" name returns
         * nullptr (advertised by presence — null means unsupported). */
        if (std::strcmp(local_name, "fss/sim") == 0)
            return ready_control_port(sim_port_, sim_port_ready_,
                                      &artefact::sim_receive);
        /* "fss/sim/<tag>" — one inbound sink per DOWNSTREAM peer; its sim::events
         * (Elaborated) feed the run gate.  The count is this node's n_downstream. */
        if (std::strncmp(local_name, "fss/sim/", 8) == 0)
            return ready_sim_down_port(local_name);
        if (std::strcmp(local_name, "fss/config") == 0)
            return ready_control_port(config_port_, config_port_ready_,
                                      &artefact::config_receive);
        /* Any name beginning "fss/time" is a timing peer port — "fss/time" is
         * peer 0, "fss/time/<tag>" (etc.) allocate further peers (one per hop). */
        if (std::strncmp(local_name, "fss/time", 8) == 0)
            return ready_time_port(local_name);

        /* Cache lookup. */
        for (auto &e : endpoints_) {
            if (e.name == local_name) return &e.endpoint;
        }

        /* Walk the hierarchy ONCE, then classify the object by socket kind.
         * A model endpoint name may address a SUB-MODEL of a multi-model artefact
         * with a "submodel/socket" path (e.g. "init/socket" in a combined CAN
         * artefact whose hub builds init+ctrl+dev): map '/' to the SystemC
         * hierarchy separator '.' so it resolves under the hub root.  A plain
         * single-model name (no '/') is unaffected.  Reserved "fss/..." names were
         * already handled above, so they never reach here. */
        if (!top_) return nullptr;
        std::string rel(local_name);
        for (char &ch : rel) if (ch == '/') ch = '.';
        std::string full_path = std::string(top_->name()) + "." + rel;
        sc_core::sc_object *obj = sc_core::sc_find_object(full_path.c_str());
        if (!obj) return nullptr;

        /* TLM TARGET socket → an endpoint that RECEIVES carriers (inbound:
         * carrier → the model's target socket via b_transport).  The base-class
         * cast also matches multi_passthrough_target_socket and any other
         * socket deriving from tlm_base_target_socket_b<32> with the default
         * transport interfaces — they need no dedicated branch.  Bind a local
         * bw stub so the model's target socket is a bound SystemC port (the real
         * initiator is the REMOTE FSS peer, reached by callback, not a port
         * binding).  Legal here: the bind window is before sc_start completes
         * elaboration. */
        if (auto *tgt = dynamic_cast<tlm::tlm_base_target_socket_b<32> *>(obj)) {
            auto &e = new_endpoint(local_name);
            e.fw_if            = &tgt->get_base_interface();
            e.cache            = fss_meta_cache_create();
            e.endpoint.receive = &artefact::endpoint_receive;
            e.bw_stub.reset(new target_bw_stub);
            tgt->get_base_port().bind(*e.bw_stub);
            return &e.endpoint;
        }

        /* TLM INITIATOR socket → an endpoint that SENDS carriers (outbound: the
         * model's b_transport → carrier → endpoint.send).  Bind the model's
         * initiator port to a fw stub that bridges b_transport to the transport-
         * installed send; receive is null (nothing is delivered inbound on an
         * initiator endpoint).  The base cast likewise matches the multi-socket
         * initiators.  Binding here is legal: we are in the bind window. */
        if (auto *ini = dynamic_cast<tlm::tlm_base_initiator_socket_b<32> *>(obj)) {
            auto &e = new_endpoint(local_name);
            e.endpoint.receive = nullptr;   /* outbound endpoint: no inbound delivery */
            e.stub.reset(new initiator_stub);
            e.stub->ep = &e.endpoint;
            ini->get_base_port().bind(*e.stub);
            return &e.endpoint;
        }

        return nullptr;  /* neither a target nor an initiator socket */
    }

private:
    /* Lazily initialise a binding-provided control port (fss/sim, fss/config)
     * and return its endpoint.  The two ports share an identical inbound-only
     * shape — owner back-pointer, self pointer, a static receive callback, no
     * outbound send — differing only in which callback they carry. */
    template <class Port>
    fss_endpoint_t *ready_control_port(Port &p, bool &ready, fss_carrier_fn rx)
    {
        if (!ready) {
            p.owner             = this;
            p.endpoint.self     = &p;
            p.endpoint.receive  = rx;
            p.endpoint.send     = nullptr;   /* artefact endpoints are inbound only */
            p.endpoint.send_ctx = nullptr;
            p.cache             = fss_meta_cache_create();  /* (id,qname)→offset */
            ready = true;
        }
        return &p.endpoint;
    }

    /* Resolve a timing-peer port by name, allocating a new one on first request
     * (capped at MAX_TIME_PORTS).  Each peer gets its own inbound-receive endpoint
     * tagged with its index; its send is installed by the transport at bind.
     * Presence of any time port arms the window drivers in run(). */
    fss_endpoint_t *ready_time_port(const char *nm)
    {
        for (int i = 0; i < n_time_ports_; ++i)
            if (time_ports_[i].name == nm) return &time_ports_[i].endpoint;
        if (n_time_ports_ >= MAX_TIME_PORTS) return nullptr;
        int i = n_time_ports_++;
        auto &p = time_ports_[i];
        p.owner             = this;
        p.index             = i;
        p.name              = nm;
        p.endpoint.self     = &p;
        p.endpoint.receive  = &artefact::time_receive;
        p.endpoint.send     = nullptr;   /* installed by the transport at bind */
        p.endpoint.send_ctx = nullptr;
        p.cache             = fss_meta_cache_create();  /* (id,qname)→offset */
        time_port_ready_    = true;
        return &p.endpoint;
    }

    /* Resolve a DOWNSTREAM sim sink ("fss/sim/<tag>") by name, allocating on first
     * request (capped at MAX_SIM_DOWN).  Each downstream peer gets its own inbound
     * sim-receive endpoint; the number allocated IS this node's n_downstream (the
     * run gate awaits one Elaborated per downstream peer).  Inbound only — a node
     * never SENDS on a downstream sink; it reports UP on fss/sim. */
    fss_endpoint_t *ready_sim_down_port(const char *nm)
    {
        for (int i = 0; i < n_sim_down_; ++i)
            if (sim_down_[i].name == nm) return &sim_down_[i].endpoint;
        if (n_sim_down_ >= MAX_SIM_DOWN) return nullptr;
        auto &p = sim_down_[n_sim_down_++];
        p.owner             = this;
        p.name              = nm;
        p.endpoint.self     = &p;
        p.endpoint.receive  = &artefact::sim_receive;
        p.endpoint.send     = nullptr;   /* inbound only */
        p.endpoint.send_ctx = nullptr;
        p.cache             = fss_meta_cache_create();  /* (id,qname)→offset */
        return &p.endpoint;
    }

    /* Append a fresh data endpoint_entry for local_name with the fields common
     * to both socket directions set (name, owner, self back-pointer, send/
     * send_ctx null until the transport binds).  The caller fills the
     * direction-specific bits.  endpoints_ is a deque, so the returned
     * reference's address stays stable across later appends. */
    endpoint_entry &new_endpoint(const char *local_name)
    {
        endpoints_.emplace_back();
        auto &e = endpoints_.back();
        e.name              = local_name;
        e.owner             = this;
        e.endpoint.self     = &e;
        e.endpoint.send     = nullptr;   /* installed by the transport at bind */
        e.endpoint.send_ctx = nullptr;
        return e;
    }

public:
    /* START BARRIER, part 1 — "you are wired" (sim::event{Bound}).  Whoever bound
     * this node's endpoints sends this once binding is complete; it arms the run
     * gate (a node never runs before it is wired, else its sends have no target).
     * Classify our timing peers now (all get_endpoint calls are done by the time
     * we are told we are wired) so the gate knows how many downstream peers to
     * await.  Non-blocking: the kernel thread leaves the bind window on its own
     * once the gate opens (wired AND every downstream peer running). */
    void on_wired()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            classify_chain_locked();
            wired_ = true;
        }
        /* PULL config now that we are wired (the orchestrator installed our
         * fss/config send at bind): query the single orchestrator responder and
         * apply the reply BEFORE the run gate opens, so the value is set
         * pre-elaboration — the in-band replacement for the old config push. */
        pull_config();
        start_cv_.notify_all();
    }

    /* Query the orchestrator's fss/config responder with a config::param_request
     * {Get}; the round-trip reply lands in-place (synchronous for inproc; the
     * transport blocks for shm/zenoh) with config::ParamValue (the run JSON) and
     * config::Status filled.  Apply the JSON via the model's configure hook when
     * Status is Ok.  Offsets come from the meta (match-or-walk).  No-op if nothing
     * bound our config send (a node nobody configures runs with model defaults). */
    void pull_config()
    {
        if (!config_port_ready_) return;
        fss_endpoint_t *cfg = &config_port_.endpoint;
        if (!cfg->send) return;   /* nobody configures us → model defaults */
        /* RETRY across an async transport's route setup: the in-place round-trip is
         * immediate for inproc/shm (succeeds on attempt 0, no sleep), but a freshly
         * bound zenoh querier may fire before its peer queryable is discovered.
         * Bounded; if the responder is genuinely unreachable we fall back to defaults. */
        for (int attempt = 0; attempt < 300; ++attempt) {
            uint8_t buf[60]; std::memset(buf, 0, sizeof(buf));
            uint32_t op = CFG_OP_GET; std::memcpy(buf, &op, sizeof(op));
            fss_carrier_t c; std::memset(&c, 0, sizeof(c));
            c.id = FSS_CONFIG_ID; c.data_ptr = buf; c.data_size = sizeof(buf);
            c.meta_ptr = (void *)FSS_CONFIG_META; c.meta_size = FSS_CONFIG_META_LEN;
            if (cfg->send(cfg->send_ctx, &c) == FSS_SEND_OK) {
                uint32_t o_val = 0, o_stat = 0, sz = 0;
                if (fss_field_offset(config_port_.cache, &c, "config::ParamValue", &o_val, &sz)) {
                    int32_t st = CFG_OK;
                    if (fss_field_offset(config_port_.cache, &c, "config::Status", &o_stat, &sz))
                        std::memcpy(&st, buf + o_stat, sizeof(st));
                    const char *pv = reinterpret_cast<const char *>(buf + o_val);
                    if (st == CFG_OK && pv[0] == '{') {
                        if (hooks_) hooks_->fss_configure(pv);
                        return;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    /* START BARRIER, part 2 — a DOWNSTREAM time-peer reported Elaborated ("I am
     * running").  Count it toward the run gate; when all downstream peers are
     * running (and we are wired) the kernel thread elaborates and runs. */
    void on_downstream_ready()
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            ++downstream_ready_;
        }
        start_cv_.notify_all();
    }

private:
    /* Fix this node's downstream set from its SIM wiring.  A downstream child has a
     * named fss/sim/<tag> sink.  Its matching timing peer is fss/time/<tag>; this
     * gives the otherwise directionless time links an endpoint identity.  The
     * untagged fss/time form remains the single-peer shorthand, but only when there
     * is exactly one downstream child and one timing peer.  Called under mu_ once
     * binding is complete. */
    void classify_chain_locked()
    {
        if (chain_classified_) return;
        n_downstream_     = n_sim_down_;
        n_downstream_time_ = 0;
        for (int i = 0; i < n_time_ports_; ++i)
            time_ports_[i].downstream = false;

        for (int child = 0; child < n_sim_down_; ++child) {
            const std::string time_name =
                std::string("fss/time/") + sim_down_[child].name.substr(8);
            for (int i = 0; i < n_time_ports_; ++i) {
                if (time_ports_[i].name == time_name) {
                    time_ports_[i].downstream = true;
                    ++n_downstream_time_;
                    break;
                }
            }
        }
        /* Old single-sink configurations commonly use fss/time rather than a
         * tagged peer port.  Preserve that form without guessing in fanout cases. */
        if (n_sim_down_ == 1 && n_time_ports_ == 1 && n_downstream_time_ == 0) {
            time_ports_[0].downstream = true;
            n_downstream_time_ = 1;
        }
        chain_classified_ = true;
    }

    /* Announce a lifecycle event UP the chain on our dedicated fss/sim port, whose
     * send the orchestrator bound to our upstream — a peer's fss/sim/<tag> sink, or
     * the orchestrator itself if we are the source.  A DEDICATED, delivery-confirmed
     * sim channel (NOT the fire-and-forget time link): retry until delivered, the
     * readiness barrier's guarantee that no Elaborated is ever lost at startup. */
    void send_sim_up(uint32_t code)
    {
        fss_endpoint_t *up = &sim_port_.endpoint;
        if (!up->send) return;
        uint32_t v = code;          /* sim::EventCode @0 (uint .size 4) */
        fss_carrier_t c; std::memset(&c, 0, sizeof(c));
        c.id = FSS_SIM_ID; c.data_ptr = &v; c.data_size = sizeof(v);
        c.meta_ptr = (void *)FSS_SIM_META; c.meta_size = FSS_SIM_META_LEN;
        while (!stop_flag_.load(std::memory_order_acquire) &&
               up->send(up->send_ctx, &c) != FSS_SEND_OK)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    /* A request is shared by its caller, the dispatch queue, and the owner-wide
     * outstanding registry.  This keeps its completion objects alive when stop or
     * an exception races the dispatch thread. */
    struct request_state {
        enum class phase { queued, running, completed };

        tlm::tlm_fw_transport_if<> *fw_if = nullptr;
        tlm::tlm_generic_payload   *gp = nullptr;
        std::mutex                  mu;
        std::condition_variable     cv;
        phase                       state = phase::queued;
        bool                        cancel = false;
    };
    using request_ptr = std::shared_ptr<request_state>;

    /* Outbound bridge for an INITIATOR socket.  Implements the TLM forward
     * interface; the model's initiator socket binds to it (get_endpoint does
     * the bind).  When the model calls b_transport, this wraps the live
     * generic payload in a Path-1 TLM carrier (data_ptr IS the gp) and
     * delivers it via the endpoint's transport-installed send.  The peer sets
     * the response on the same gp (shared in-proc), so b_transport returns
     * with it populated — the exact mirror of endpoint_receive's fast path. */
    struct initiator_stub : tlm::tlm_fw_transport_if<> {
        fss_endpoint_t *ep = nullptr;

        void b_transport(tlm::tlm_generic_payload &gp, sc_core::sc_time &) override
        {
            fss_carrier_t c;
            c.id        = FSS_TLM_ID;          /* the live, TypeId-bearing carrier */
            c.data_ptr  = &gp;                 /* Path 1: data_ptr IS the live gp */
            /* FSS_TLM_META describes the GP's REAL offsets + the core::TypeId
             * marker, so an in-proc peer casts (id match + typeid confirm) while a
             * wire transport can walk the live object — read each field at its real
             * offset, inline bus::Data, drop the non_serializable TypeId, re-hash to
             * the flat id.  data_size spans the whole object for the serialiser. */
            c.data_size = sizeof(tlm::tlm_generic_payload);
            c.meta_ptr  = (void *)FSS_TLM_META;
            c.meta_size = FSS_TLM_META_LEN;
            if (!ep || !ep->send || ep->send(ep->send_ctx, &c) != FSS_SEND_OK)
                gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        }

        /* b_transport-only bridge; the rest of the fw interface is stubbed. */
        tlm::tlm_sync_enum nb_transport_fw(tlm::tlm_generic_payload &,
                                           tlm::tlm_phase &, sc_core::sc_time &) override
        { return tlm::TLM_COMPLETED; }
        bool get_direct_mem_ptr(tlm::tlm_generic_payload &, tlm::tlm_dmi &) override
        { return false; }
        unsigned int transport_dbg(tlm::tlm_generic_payload &) override
        { return 0; }
    };

    /* Backward stub bound to a model TARGET socket's base port.  A TLM target
     * socket must be bound to an initiator for SystemC elaboration (the bw
     * channel: nb responses / invalidate_direct_mem_ptr).  In an FSS artefact
     * the real initiator is the REMOTE peer, which the FSS bind reaches by a
     * callback — not a SystemC port binding — so the model's target socket
     * would be left unbound.  The wrapper therefore binds this local bw stub to
     * it (the exact mirror of initiator_stub, which the wrapper binds to a
     * model INITIATOR socket).  The model needs no scaffolding of its own.
     * Forward delivery still goes via the target's fw interface (fw_if). */
    struct target_bw_stub : tlm::tlm_bw_transport_if<> {
        tlm::tlm_sync_enum nb_transport_bw(tlm::tlm_generic_payload &,
                                           tlm::tlm_phase &, sc_core::sc_time &) override
        { return tlm::TLM_COMPLETED; }
        void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) override {}
    };

    struct endpoint_entry {
        std::string                  name;
        fss_endpoint_t               endpoint;
        tlm::tlm_fw_transport_if<>  *fw_if = nullptr;  /* target: resolved fw interface */
        std::unique_ptr<initiator_stub>  stub;          /* initiator socket: outbound bridge */
        std::unique_ptr<target_bw_stub>  bw_stub;       /* target socket: bw bind for elaboration */
        artefact                    *owner = nullptr;   /* back-pointer to wrapper */
        fss_meta_cache_t            *cache = nullptr;    /* (id,qname)→offset, foreign carriers */
    };

    /* Is the carrier's payload field carried INLINE (layout "") rather than as
     * an fss:local-ptr?  The TLM GP carrier passes Data by pointer (the slot
     * holds the payload's address); can::frame carries ≤8 inline bytes (the slot
     * IS the payload).  Read the layout from the meta so endpoint_receive aliases
     * the payload the right way.  The payload role is bus::Data (TLM) or can::Data
     * (a native CAN frame — same role, different vocabulary).  Not found → treat
     * as pointer (the TLM default). */
    static bool data_is_inline(const void *meta, uint64_t meta_size)
    {
        int n = fss_meta_field_count(meta, meta_size);
        for (int i = 0; i < n; ++i) {
            fss_meta_field_t f;
            if (!fss_meta_field_at(meta, meta_size, i, &f)) break;
            if (std::strcmp(f.qname, "bus::Data") == 0)
                return f.layout[0] == '\0';
        }
        return false;
    }

    /* Static receive callback (fss_carrier_fn): self is endpoint_entry *.
     * Returns FSS_SEND_OK once dispatched, or a reason if rejected. */
    static int endpoint_receive(void *self, fss_carrier_t *carrier)
    {
        auto *entry = static_cast<endpoint_entry *>(self);
        if (!entry || !carrier) return FSS_SEND_NO_RECEIVER;
        if (!entry->fw_if || !entry->owner || !carrier->data_ptr) return FSS_SEND_NO_RECEIVER;

        auto *art = entry->owner;

        tlm::tlm_generic_payload  local;   /* used only on the foreign path */
        tlm::tlm_generic_payload *gp;      /* what we dispatch */

        uint8_t *data            = nullptr;
        uint32_t off_status      = 0;
        uint32_t off_dmi         = 0;
        bool     write_back_stat = false;
        bool     have_dmi        = false;

        if (tlm_is_live(carrier)) {
            /* PATH 1 (fast): in-proc TLM — data_ptr IS a live generic payload.
             * tlm_is_live() gated on id == FSS_TLM_ID (the live, TypeId-bearing
             * carrier — a flat/wire carrier has a different id) AND confirmed via
             * typeid== through the vtable that core::TypeId points at, so this
             * static_cast only ever sees a genuine, ABI-matching live object.  No
             * copy, no field walk, no write-back (the response is set on the real
             * gp, which the sender shares). */
            gp = static_cast<tlm::tlm_generic_payload *>(carrier->data_ptr);
        } else {
            /* Foreign carrier: resolve the bus:: fields from the meta by qname,
             * cached by (id,qname) — cache hit = path 3, miss = path 4 — then
             * eager-populate a vanilla payload (copy the scalar header, alias
             * the data pointer).  Reject if the meta is absent or a required
             * field isn't there; never reinterpret unknown bytes. */
            if (!carrier->meta_ptr) return FSS_SEND_NO_RECEIVER;
            data = static_cast<uint8_t *>(carrier->data_ptr);
            const void *m = carrier->meta_ptr; uint64_t ml = carrier->meta_size;
            auto *cache = entry->cache;
            uint32_t off_cmd = 0, off_addr, off_len, off_data, sz, sz_cmd = 0;
            /* bus::Locator/Length/Data are required to build a payload transaction.
             * bus::Operation is OPTIONAL: a minimal CAN data frame (e.g. sccan::frame,
             * pycan::frame) carries no rtr/operation field, so default to WRITE — a
             * delivered frame is a broadcast write.  This is the reuse claim made
             * concrete: the binding reads whatever the sender's meta provides, never
             * demanding a field a given carrier has no reason to carry. */
            if (!fss_meta_resolve(cache, carrier->id, m, ml, "bus::Locator",   &off_addr, &sz) ||
                !fss_meta_resolve(cache, carrier->id, m, ml, "bus::Length",    &off_len,  &sz) ||
                !fss_meta_resolve(cache, carrier->id, m, ml, "bus::Data",      &off_data, &sz)) {
                return FSS_SEND_NO_RECEIVER;  /* required field absent — reject */
            }
            bool have_cmd = fss_meta_resolve(cache, carrier->id, m, ml, "bus::Operation", &off_cmd, &sz_cmd) != 0;
            write_back_stat = fss_meta_resolve(cache, carrier->id, m, ml, "bus::Status", &off_status, &sz) != 0;

            uint32_t command = 1 /* default WRITE (a delivered frame) */, length = 0;
            uint64_t address = 0;
            uint8_t *payload = nullptr;
            /* Read bus::Operation at its resolved width (u8 for a CAN frame's rtr,
             * u32 for a TLM/register write) into the zeroed low bytes. */
            if (have_cmd) { command = 0; std::memcpy(&command, data + off_cmd, sz_cmd < 4 ? sz_cmd : 4); }
            std::memcpy(&address, data + off_addr, sizeof(address));
            std::memcpy(&length,  data + off_len,  sizeof(length));
            /* bus::Data is carried either BY POINTER (fss:local-ptr — the TLM GP
             * carrier: the slot holds the payload's address) or INLINE (e.g.
             * can::frame: classic CAN ≤8 bytes, the slot IS the payload).  The
             * meta says which (the field's layout); alias accordingly — no copy
             * either way.  Without this, an inline payload would be misread as a
             * pointer and dereferenced. */
            if (data_is_inline(m, ml))
                payload = data + off_data;
            else
                std::memcpy(&payload, data + off_data, sizeof(payload));

            local.set_command(static_cast<tlm::tlm_command>(command)); /* 0/1/2 map exactly */
            local.set_address(address);
            local.set_data_ptr(payload);            /* alias — no payload copy */
            local.set_data_length(length);

            /* Optional fields: populate when present, use sensible defaults otherwise. */
            uint32_t off_sw, off_be, off_be_len;
            bool have_sw     = fss_meta_resolve(cache, carrier->id, m, ml, "bus::StreamingWidth",   &off_sw,     &sz) != 0;
            bool have_be     = fss_meta_resolve(cache, carrier->id, m, ml, "bus::ByteEnable",       &off_be,     &sz) != 0;
            bool have_be_len = fss_meta_resolve(cache, carrier->id, m, ml, "bus::ByteEnableLength", &off_be_len, &sz) != 0;
            have_dmi         = fss_meta_resolve(cache, carrier->id, m, ml, "bus::DmiAllowed",       &off_dmi,    &sz) != 0;
            /* tlm::GpOption is not resolved: it has no meaning on b_transport. */

            if (have_sw) {
                uint32_t sw = 0; std::memcpy(&sw, data + off_sw, sizeof(sw));
                local.set_streaming_width(sw);
            } else {
                local.set_streaming_width(length);
            }

            if (have_be && have_be_len) {
                uint32_t be_len = 0; std::memcpy(&be_len, data + off_be_len, sizeof(be_len));
                uint8_t *be_ptr = nullptr; std::memcpy(&be_ptr, data + off_be, sizeof(be_ptr));
                local.set_byte_enable_ptr(be_len ? be_ptr : nullptr);
                local.set_byte_enable_length(be_len);
            } else {
                local.set_byte_enable_ptr(nullptr);
                local.set_byte_enable_length(0);
            }

            if (have_dmi) {
                uint8_t dv = 0; std::memcpy(&dv, data + off_dmi, sizeof(dv));
                local.set_dmi_allowed(dv != 0);
            } else {
                local.set_dmi_allowed(false);
            }
            local.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
            gp = &local;
        }

        /* SAME-THREAD FAST PATH: if endpoint_receive is running ON this artefact's
         * kernel thread, the sender is another model IN THIS SAME KERNEL (e.g. a
         * combined artefact that builds initiator+controller+device and wires them
         * through FSS endpoints).  We are already in kernel context, mid-b_transport
         * or mid-wait, which is exactly where the dispatch SC_THREAD would have run
         * this gp — so call b_transport DIRECTLY, skipping the queue+doorbell+cv.
         * A cross-thread send (a genuinely foreign kernel or the host thread) still
         * takes the doorbell path below: you cannot enter a kernel from a foreign
         * thread without it.  gp is already resolved (Path 1 cast or the foreign
         * build above); the write-back is identical to the doorbell path's. */
        if (art->kernel_running_.load(std::memory_order_acquire) &&
            std::this_thread::get_id() == art->kernel_id_) {
            sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
            entry->fw_if->b_transport(*gp, delay);
            if (write_back_stat) {
                int32_t st = static_cast<int32_t>(gp->get_response_status());
                std::memcpy(data + off_status, &st, sizeof(st));
            }
            if (have_dmi) {
                uint8_t d = gp->is_dmi_allowed() ? 1 : 0;
                std::memcpy(data + off_dmi, &d, sizeof(d));
            }
            return FSS_SEND_OK;
        }

        /* Dispatch gp on the kernel thread (cross-kernel hop), wait for it.
         *
         * NB: this hop exists only because the binding ALWAYS runs the kernel
         * on its own thread (run()), so endpoint_receive — called on the host's
         * sender thread — is necessarily a foreign thread w.r.t. this kernel.
         * If the caller were ON the kernel thread (the in-context case (1), and
         * what psystemc enables: a foreign thread entering the kernel safely
         * under lock), we could skip the queue+doorbell entirely and call
         * b_transport directly here — the PASSIVE/zero-handoff path.  For v0.1
         * we always spawn, so we always take the doorbell. */
        auto request = std::make_shared<request_state>();
        request->fw_if = entry->fw_if;
        request->gp = gp;
        {
            std::lock_guard<std::mutex> lk(art->queue_mu_);
            /* Do NOT enqueue once the kernel is tearing down.  dispatch_thread's
             * stop path drains the queue and wakes every waiter ONCE, then exits;
             * a request pushed after that would have no kernel thread to finish its
             * shared request state — the b_transport-vs-time teardown hang seen at
             * high N.  The check is under queue_mu_, the same lock
             * dispatch_thread's stop-drain holds, so it is exact: we either
             * enqueue before that drain (and it wakes us) or observe the flag and
             * bail.  A null doorbell means the kernel already stopped — same
             * bail (reject rather than block on a reply that will never come). */
            if (art->stop_flag_.load(std::memory_order_acquire) || !art->doorbell_)
                return FSS_SEND_NO_RECEIVER;
            /* Admission is atomic: every accepted request is in both the queue
             * and the owner-wide registry before the kernel can be woken. */
            art->outstanding_.push_back(request);
            art->queue_.push_back(request);
            /* queue_mu_ also protects doorbell_.  Set the durable wake level and
             * schedule its update before releasing it: teardown cannot destroy the
             * channel between this check and ring(), and the consumer cannot clear
             * a wake for work which is now in the queue. */
            art->doorbell_->ring();
        }

        {
            std::unique_lock<std::mutex> lk(request->mu);
            request->cv.wait(lk, [&request] {
                return request->state == request_state::phase::completed;
            });
        }

        if (write_back_stat) {
            int32_t st = static_cast<int32_t>(gp->get_response_status());
            std::memcpy(data + off_status, &st, sizeof(st));
        }
        if (have_dmi) {
            uint8_t d = gp->is_dmi_allowed() ? 1 : 0;
            std::memcpy(data + off_dmi, &d, sizeof(d));
        }
        return FSS_SEND_OK;
    }

    /* --- fss/sim standard control port ----------------------------------- */

    /* Minimal struct whose address serves as the endpoint's self pointer.
     * Kept separate from endpoint_entry to avoid confusion with data endpoints. */
    struct sim_port {
        fss_endpoint_t  endpoint;
        artefact       *owner = nullptr;
        std::string     name;            /* set for fss/sim/<tag> downstream sinks */
        fss_meta_cache_t *cache = nullptr;  /* (id,qname)→offset for the sim carrier */
    };

    /* sim::EventCode values (atoms/sim/EventCode.libsonnet default_encoding;
     * not emitted as C constants by fss_compile.py, so mirrored here). */
    enum : uint32_t {
        SIM_BOUND  = 0, SIM_ELABORATED = 1, SIM_STARTED = 2,
        SIM_PAUSED = 3, SIM_RESUMED    = 4, SIM_STOPPED = 5,
    };

    /* Receive callback for the fss/sim control port (self is sim_port *).
     * Dispatches the lifecycle chain (see START BARRIER in the header):
     *   Bound      → on_wired()           — "you are wired", arm the run gate;
     *   Elaborated → on_downstream_ready() — a downstream peer is running;
     *   Stopped    → signal_stop()         — stop without destroy.
     * Started is accepted as a wired-kick too, so a peer/orchestrator that has
     * not yet migrated to the wave still releases this node.
     * sim::EventCode is read by resolving its offset from the meta (the universal
     * match-or-walk rule) rather than a magic-offset cast. */
    static int sim_receive(void *self, fss_carrier_t *carrier)
    {
        auto *sp = static_cast<sim_port *>(self);
        if (!sp || !sp->owner || !carrier || !carrier->data_ptr) return FSS_SEND_NO_RECEIVER;

        uint32_t off = 0, sz = 0;
        if (!fss_field_offset(sp->cache, carrier, "sim::EventCode", &off, &sz))
            return FSS_SEND_NO_RECEIVER;
        uint32_t event_code = 0;
        std::memcpy(&event_code, static_cast<const uint8_t *>(carrier->data_ptr) + off,
                    sizeof(event_code));
        switch (event_code) {
        case SIM_BOUND:      sp->owner->on_wired();            break;
        case SIM_STARTED:    sp->owner->on_wired();            break;
        case SIM_ELABORATED: sp->owner->on_downstream_ready(); break;
        case SIM_STOPPED:    sp->owner->signal_stop();         break;
        default:             /* not handled in v0.1 */         break;
        }
        return FSS_SEND_OK;
    }

    /* --- fss/time standard control port ---------------------------------- */

    /* The window driver: a vendored sc_sync_windowed gates time advancement via
     * an sc_ob_event + suspend (see sc_sync_window.h).  Engaged only when fss/time
     * is bound; a node nobody time-syncs never constructs one and runs free. */
    using sync_window_t = sc_core::sc_sync_windowed<sc_core::sc_sync_policy_tlm_quantum>;
    using win_t         = sync_window_t::window;

    /* A node may have MORE THAN ONE timing peer (e.g. a controller with an
     * upstream initiator AND a downstream device).  Each gets its OWN fss/time
     * endpoint AND its own sc_sync_window: get_endpoint("fss/time") is peer 0;
     * any further name beginning "fss/time" (e.g. "fss/time/dev") allocates
     * another.
     *
     * The per-peer drivers COMPOSE through SystemC's GLOBAL suspend COUNT with no
     * extra logic: each posts an sc_ob_event at its peer's Upper and suspends when
     * the kernel reaches it.  The kernel stops at the EARLIEST Upper (= min over
     * peers) automatically — whichever ob_event fires first.  Crucially, suspend/
     * unsuspend is PER-PROCESS (sc_simcontext m_suspend_all_req), so one driver's
     * unsuspend never releases another's suspend: m_suspend is the count of peers
     * currently at-bound, and the kernel runs only at count 0.  That IS the "run
     * to the slowest, no faster than any" rule, for free. */
    static constexpr int MAX_TIME_PORTS = 4;
    struct time_port {
        fss_endpoint_t   endpoint;
        artefact        *owner = nullptr;
        int              index = 0;
        std::string      name;
        sync_window_t   *sync = nullptr;       /* this peer's driver (run-local) */
        bool             seen = false;         /* peer has sent a real window */
        bool             detached = false;     /* peer opened its window (done) */
        bool             downstream = false;   /* matched fss/sim/<tag> child at Bound */
        sc_core::sc_time last_from{};          /* its last-reported clock (skew) */
        fss_meta_cache_t *cache = nullptr;     /* (id,qname)→offset for time::window */
    };

    /* Packed layout we PRODUCE for an outbound time::window (send_peer_window).
     * The receive path no longer assumes this layout — it resolves time::Unit/
     * Lower/Upper from the meta (match-or-walk) so a foreign producer's layout is
     * honoured.  We only own the layout of what WE emit. */
    struct __attribute__((packed)) time_window_view {
        int32_t  unit;   /* time::Unit  @0  (sc_time_unit exponent) */
        uint32_t _pad;   /*             @4                          */
        uint64_t lower;  /* time::Lower @8                          */
        uint64_t upper;  /* time::Upper @16                         */
    };

    /* Receive a time::window on peer `tp->index` and hand it to that peer's driver.
     * A binding-provided control port like fss/sim and fss/config.  Resolve the
     * three fields by qname (universal match-or-walk) — no fixed-offset cast. */
    static int time_receive(void *self, fss_carrier_t *carrier)
    {
        auto *tp = static_cast<time_port *>(self);
        if (!tp || !tp->owner || !carrier || !carrier->data_ptr) return FSS_SEND_NO_RECEIVER;
        uint32_t o_unit = 0, o_lo = 0, o_hi = 0, sz = 0;
        if (!fss_field_offset(tp->cache, carrier, "time::Unit",  &o_unit, &sz) ||
            !fss_field_offset(tp->cache, carrier, "time::Lower", &o_lo,   &sz) ||
            !fss_field_offset(tp->cache, carrier, "time::Upper", &o_hi,   &sz))
            return FSS_SEND_NO_RECEIVER;
        const auto *d = static_cast<const uint8_t *>(carrier->data_ptr);
        int32_t  unit = 0; uint64_t lower = 0, upper = 0;
        std::memcpy(&unit,  d + o_unit, sizeof(unit));
        std::memcpy(&lower, d + o_lo,   sizeof(lower));
        std::memcpy(&upper, d + o_hi,   sizeof(upper));
        tp->owner->set_time_window(tp->index, unit, lower, upper);
        return FSS_SEND_OK;
    }

    /* A time::window carrier field (Unit) is the sc_time_unit exponent; the value
     * is a tick count at that unit, so sc_time(value, unit) reconstructs it. */
    static sc_core::sc_time time_from(int32_t unit, uint64_t value)
    {
        return sc_core::sc_time(static_cast<double>(value),
                                static_cast<sc_core::sc_time_unit>(unit));
    }

    static bool trace_on()
    {
        static const bool on = std::getenv("FSS_TIMESYNC_TRACE") != nullptr;
        return on;
    }

    /* Peer `idx` sent a window: hand it to that peer's own driver (thread-safe
     * cross-thread entry).  A real (bounded) window marks the peer seen and
     * records its clock for the skew check; an open_window (Upper==max) is a
     * RELEASE (the peer detached) and is excluded from skew. */
    void set_time_window(int idx, int32_t unit, uint64_t lower, uint64_t upper)
    {
        bool open;
        bool first_detach = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto &p = time_ports_[idx];
            /* UINT64_MAX Upper is the "open" (detach) sentinel — NOT round-tripped
             * through a double, which would corrupt sc_max_time() and let the open
             * window masquerade as a real {0, huge} window (polluting the skew). */
            open = (upper == UINT64_MAX);
            sc_core::sc_time lo = time_from(unit, lower);
            sc_core::sc_time hi = open ? sc_core::sc_max_time() : time_from(unit, upper);
            if (!open) { p.seen = true; p.last_from = lo; }
            else if (!p.detached) {
                p.detached = true;
                first_detach = true;
            }
            if (trace_on())
                std::cerr << "[ts] " << name_ << " recv peer#" << idx << " {"
                          << lo / sc_core::sc_time(1, sc_core::SC_NS) << ","
                          << (hi == sc_core::sc_max_time() ? -1.0
                              : hi / sc_core::sc_time(1, sc_core::SC_NS)) << "} ns\n";
            if (p.sync) p.sync->async_set_window({ lo, hi });
        }
        /* A peer DETACH must reach the model on the KERNEL thread (it may sc_stop):
         * queue it and ring the doorbell, the same cross-thread path b_transport
         * uses.  dispatch_thread drains detach_queue_ and calls fss_peer_detached. */
        if (open && first_detach) {
            {
                std::lock_guard<std::mutex> lk(queue_mu_);
                detach_queue_.push_back(idx);
                /* Keep the enqueue and durable wake atomic with respect to the
                 * dispatch thread's empty check and wrapper_hub teardown. */
                if (doorbell_) doorbell_->ring();
            }
        }
    }

    /* Sync callback for peer `idx` (kernel thread): our driver for that peer
     * reached its bound — send OUR window to THAT peer so it can extend.  Carried
     * as ticks at the default sc_time resolution (SC_PS, exponent 1). */
    void send_peer_window(int idx, const win_t &w)
    {
        /* Skew sample: w.from is OUR current simulated time; the gap to this peer's
         * last-known clock is the federation clock skew across this hop.  The
         * protocol holds it ≤ quantum (we never run past the peer's authorised
         * to = peer_time + quantum) — the point is it stays BOUNDED by the quantum,
         * not growing with the run.  Skip the open_window WE send at teardown. */
        if (w.to != sc_core::sc_max_time()) {
            std::lock_guard<std::mutex> lk(mu_);
            auto &p = time_ports_[idx];
            if (p.seen) {
                sc_core::sc_time gap = w.from > p.last_from
                                     ? w.from - p.last_from
                                     : p.last_from - w.from;
                if (gap > max_skew_) { max_skew_ = gap; max_skew_at_ = w.from; max_skew_peer_ = idx; }
                if (trace_on())
                    std::cerr << "[ts] " << name_ << " send peer#" << idx << " from="
                              << w.from / sc_core::sc_time(1, sc_core::SC_NS) << " gap="
                              << gap / sc_core::sc_time(1, sc_core::SC_NS) << " ns\n";
            }
        }
        auto &ep = time_ports_[idx].endpoint;
        if (!ep.send) return;
        time_window_view v;
        v.unit  = 1;                  /* SC_PS — the default sc_time resolution */
        v._pad  = 0;
        v.lower = static_cast<uint64_t>(w.from.value());
        /* Send the unbounded (open/detach) Upper as an explicit sentinel, NOT
         * sc_max_time()'s tick count: value()→double→value() loses precision, so a
         * round-tripped max would not compare equal to sc_max_time() at the peer. */
        v.upper = (w.to == sc_core::sc_max_time())
                      ? UINT64_MAX : static_cast<uint64_t>(w.to.value());
        fss_carrier_t c; std::memset(&c, 0, sizeof(c));
        c.id        = FSS_TIME_ID;
        c.data_ptr  = &v; c.data_size = sizeof(v);
        c.meta_ptr  = (void *)FSS_TIME_META; c.meta_size = sizeof(FSS_TIME_META);
        /* Pure fire-and-forget.  The upward readiness wave (see START BARRIER)
         * guarantees this peer is already RUNNING — its transport matched, its
         * driver armed — before we ever send, so a window is never lost to
         * startup discovery.  Readiness is a sim-status signal, NOT a retried
         * data window: fss/time carries time-sync and nothing else. */
        ep.send(ep.send_ctx, &c);
    }

    /* --- fss/config standard control port -------------------------------- */

    struct config_port {
        fss_endpoint_t  endpoint;
        artefact       *owner = nullptr;
        fss_meta_cache_t *cache = nullptr;  /* (id,qname)→offset for config carrier */
    };

    /* Static receive callback for the fss/config control port (self is
     * config_port *).  Applies a config::param_request carrier to the model's
     * CCI parameters by name.  Sent in the bind window (before Started), so the
     * value is set pre-elaboration.  Field offsets come from the meta (universal
     * match-or-walk).  See docs/spec/05-abi-conventions.md. */
    static int config_receive(void *self, fss_carrier_t *carrier)
    {
        auto *cp = static_cast<config_port *>(self);
        if (!cp || !cp->owner || !carrier || !carrier->data_ptr) return FSS_SEND_NO_RECEIVER;
        return cp->owner->apply_config(cp->cache, carrier);
    }

    enum : uint32_t { CFG_OP_GET = 0, CFG_OP_SET = 1, CFG_OP_DESCRIBE = 2 };
    enum : int32_t  { CFG_OK = 0, CFG_NOT_FOUND = 1, CFG_TYPE_MISMATCH = 2, CFG_READ_ONLY = 3 };

    /* Set a named CCI parameter from the carrier (member: has the broker).
     * v0.1: integer values only (ParamValue's leading 8 bytes, little-endian) —
     * typed config::ParamValue is deferred in the spec.  Writes Status back.
     * Offsets for config::{OpCode,ParamName,ParamValue,Status} are resolved by
     * qname from the meta (match-or-walk), not a fixed struct. */
    int apply_config(fss_meta_cache_t *cache, fss_carrier_t *carrier)
    {
        uint32_t o_op = 0, o_name = 0, o_val = 0, o_stat = 0, sz = 0;
        if (!fss_field_offset(cache, carrier, "config::OpCode",    &o_op,   &sz) ||
            !fss_field_offset(cache, carrier, "config::ParamName", &o_name, &sz) ||
            !fss_field_offset(cache, carrier, "config::ParamValue",&o_val,  &sz))
            return FSS_SEND_NO_RECEIVER;
        bool have_status = fss_field_offset(cache, carrier, "config::Status", &o_stat, &sz);

        auto *base = static_cast<uint8_t *>(carrier->data_ptr);
        uint32_t    opcode = 0; std::memcpy(&opcode, base + o_op, sizeof(opcode));
        /* ParamName is an fss:local-ptr-nul: the slot holds a char* (in-proc). */
        const char *param_name = nullptr; std::memcpy(&param_name, base + o_name, sizeof(param_name));
        const char *pv = reinterpret_cast<const char *>(base + o_val);

        auto write_status = [&](int32_t s) {
            if (have_status) std::memcpy(base + o_stat, &s, sizeof(s));
        };

        /* JSON ParamValue (the CAN platform's form): hand the raw blob to the
         * model hook, which parses the keys it understands.  Recognised by the
         * leading '{'.  No ParamName is named in this form. */
        if (opcode == CFG_OP_SET && pv[0] == '{') {
            if (hooks_) hooks_->fss_configure(pv);
            write_status(CFG_OK);
            return FSS_SEND_OK;
        }

        int32_t status = CFG_OK;
        if (opcode != CFG_OP_SET || !param_name || !broker_if_) {
            status = CFG_NOT_FOUND;   /* only Set is implemented in v0.1 */
        } else {
            /* Parameter name is relative to the model; CCI names are
             * hierarchical under the top module's name. */
            std::string full = std::string(name_) + "." + param_name;

            cci::cci_param_untyped_handle h =
                broker_if_->get_param_handle(full, cci::cci_originator("fss/config"));
            if (!h.is_valid()) {
                status = CFG_NOT_FOUND;
            } else {
                uint64_t param_value = 0; std::memcpy(&param_value, base + o_val, sizeof(param_value));
                h.set_cci_value(cci::cci_value(static_cast<sc_dt::int64>(param_value)));
            }
        }

        write_status(status);
        return FSS_SEND_OK;
    }

    /* Complete one request exactly once.  Do not hold request->mu while taking
     * queue_mu_: release each lock before taking the next one.  The registry
     * removal happens after the state transition, so stop can always find requests
     * that dispatch has removed from queue_ but has not yet completed. */
    void finish_request(const request_ptr &request, bool generic_error)
    {
        {
            std::lock_guard<std::mutex> lk(request->mu);
            if (request->state == request_state::phase::completed) return;
            if (generic_error || request->cancel)
                request->gp->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            request->state = request_state::phase::completed;
        }
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            for (auto it = outstanding_.begin(); it != outstanding_.end(); ++it) {
                if (it->get() == request.get()) {
                    outstanding_.erase(it);
                    break;
                }
            }
        }
        request->cv.notify_all();
    }

    /* Stop admission and cancel all outstanding work.  Queued work completes now;
     * dispatch-local work remains registered, is marked cancelled, and completes
     * on the dispatch thread.  Never notify a caller while holding queue_mu_. */
    void stop_and_cancel_pending(bool ring_doorbell, bool finish_running = false)
    {
        std::deque<request_ptr> queued;
        std::deque<request_ptr> outstanding;
        std::deque<request_ptr> finish_now;
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            stop_flag_.store(true, std::memory_order_release);
            queued.swap(queue_);
            outstanding = outstanding_;
            /* Admission puts a request in both collections.  Retain queue-only
             * entries too, so this stop snapshot is complete even if that invariant
             * changes in a later admission path. */
            for (const auto &request : queued) {
                bool present = false;
                for (const auto &saved : outstanding)
                    if (saved.get() == request.get()) { present = true; break; }
                if (!present) outstanding.push_back(request);
            }
            /* doorbell_ is protected by queue_mu_.  Ring only while its owner is
             * known to retain it; wrapper_hub teardown passes false. */
            if (ring_doorbell && doorbell_)
                doorbell_->ring();
        }

        start_cv_.notify_all();   /* unblock run() if still parked in the bind window */
        /* Dispatch may have already swapped a request into its local batch.  It is
         * still in outstanding, so decide from its state rather than from queue_
         * membership.  This is deliberately after queue_mu_ is released: queue_mu_
         * never nests request_state::mu. */
        for (const auto &request : outstanding) {
            std::lock_guard<std::mutex> lk(request->mu);
            if (request->state == request_state::phase::completed)
                continue;
            request->cancel = true;
            if (request->state == request_state::phase::queued || finish_running)
                finish_now.push_back(request);
        }
        for (const auto &request : finish_now)
            finish_request(request, true);
        /* A hub destructor can run while an exception unwinds dispatch_thread.
         * In that case no dispatcher remains to observe cancel, so finish the
         * dispatch-local states here as well. */
        if (finish_running)
            for (const auto &request : outstanding) {
                /* finish_request is idempotent.  This covers a request that moved
                 * from queued to running after the snapshot above. */
                finish_request(request, true);
            }
    }

    /* Signal the kernel to stop, without destroying. */
    void signal_stop()
    {
        stop_and_cancel_pending(true);
    }

    /* --- kernel thread ---------------------------------------------------- */

    /* Cross-thread doorbell.  A foreign (host) thread must NOT touch a kernel
     * sc_event — immediate/delta notification is not thread-safe.  The one
     * sanctioned cross-thread entry point is sc_prim_channel::async_request_update():
     * the host pushes a request (or sets stop_flag_) then rings the doorbell;
     * the kernel wakes and runs update() ON THE KERNEL THREAD, which delta-
     * notifies the dispatch thread's work_event (legal in the update phase).
     *
     * Attached as a SUSPENDING channel (async_attach_suspending, in the ctor):
     * at event starvation the kernel OS-blocks on the registry's suspend
     * semaphore waiting for the next ring, instead of falling out of sc_start.
     * That is what lets us run the kernel on a bare sc_start() with no keep-
     * alive spin — the host's ring is what advances it. */
    struct doorbell : sc_core::sc_prim_channel {
        sc_core::sc_event *evt;   /* the dispatch thread's work_event */
        /* async_request_update() coalesces requests.  Keep a separate level so
         * an update/event consumed in an unlucky delta cannot lose queued work. */
        std::atomic<bool> pending{false};
        explicit doorbell(sc_core::sc_event *e)
            : sc_core::sc_prim_channel("_fss_doorbell"), evt(e)
        {
            async_attach_suspending();   /* OS-block on starvation; wake on ring */
        }
        void ring()
        {
            /* Call only while the owner's queue_mu_ is held.  Set the durable
             * level before scheduling the kernel update. */
            pending.store(true, std::memory_order_release);
            async_request_update();
        }
        void update() override          /* kernel thread, update phase */
        {
            /* Do not consume pending here.  The dispatch thread consumes it only
             * after it has drained both queues and is ready to wait again. */
            if (pending.load(std::memory_order_acquire))
                evt->notify(sc_core::SC_ZERO_TIME);
        }
    };

    /* Small sc_module that owns the dispatch SC_THREAD, its work_event, and the
     * cross-thread doorbell.  Constructed in run() alongside Module top. */
    struct wrapper_hub : sc_core::sc_module {
        sc_core::sc_event   work_event;
        artefact           *owner;
        doorbell            bell;   /* declared after work_event: built last */

        explicit wrapper_hub(sc_core::sc_module_name n, artefact *a)
            : sc_core::sc_module(n), owner(a), bell(&work_event)
        {
            SC_THREAD(dispatch_thread);
        }

        /* owner->doorbell_ aliases &bell, which lives on the kernel thread's
         * stack (run()).  When the kernel thread leaves run() — normal stop or
         * exception unwind — the hub (and bell) are destroyed.  Clear the alias
         * under owner->queue_mu_ before that, so a concurrent producer or
         * signal_stop() can never ring a destroyed doorbell. */
        ~wrapper_hub() override
        {
            /* This also covers exception unwind and natural kernel completion.
             * Do it before clearing the alias so no request waiter can outlive the
             * hub.  Do not ring during destruction: an async update could outlive
             * bell itself. */
            owner->stop_and_cancel_pending(false, true);
            /* A producer checks doorbell_, makes work durable, and rings while it
             * holds queue_mu_.  Clearing the alias under that same lock keeps bell
             * alive until all such critical sections have exited. */
            std::lock_guard<std::mutex> lk(owner->queue_mu_);
            owner->doorbell_ = nullptr;
        }
        void dispatch_thread()
        {
            while (true) {
                /* Drain before waiting.  The doorbell level is cleared only after
                 * both queues are drained.  A producer that arrives after the clear
                 * sets the level before it requests an update; once wait() yields,
                 * the kernel runs that update and wakes this thread. */
                std::deque<request_ptr> local;
                std::deque<int>     detached;
                bool queues_empty = false;
                {
                    std::lock_guard<std::mutex> lk(owner->queue_mu_);
                    local.swap(owner->queue_);
                    detached.swap(owner->detach_queue_);
                    /* Clear only after observing BOTH shared queues empty while
                     * holding their producer lock.  In particular, never clear a
                     * producer wake after swapping nonempty work into local. */
                    if (local.empty() && detached.empty()) {
                        bell.pending.store(false, std::memory_order_release);
                        queues_empty = true;
                    }
                }

                /* Do not take a request lock under queue_mu_.  A concurrent stop
                 * snapshots outstanding after the swap, then may complete one of
                 * these states before we get here.  In that case leave it completed
                 * rather than reviving it as running. */
                for (const auto &request : local) {
                    std::lock_guard<std::mutex> request_lk(request->mu);
                    if (request->state == request_state::phase::queued)
                        request->state = request_state::phase::running;
                }

                if (owner->stop_flag_.load(std::memory_order_acquire)) {
                    /* Release every time-sync peer (each driver's detach() sends
                     * its peer an open_window) so none blocks waiting for a window
                     * we will never send again. */
                    for (int i = 0; i < owner->n_time_ports_; ++i)
                        if (owner->time_ports_[i].sync) owner->time_ports_[i].sync->detach();
                    /* local was removed from queue_ before observing stop.  Complete
                     * it here; stop_and_cancel_pending() only owns requests still in
                     * the shared queue. */
                    for (const auto &request : local)
                        owner->finish_request(request, true);
                    sc_core::sc_stop();
                    return;
                }

                if (queues_empty) {
                    /* No std::mutex is held across wait().  Keep the predicate
                     * loop: a stale event can wake us without new work.  A producer
                     * sets pending under queue_mu_ before it schedules the update,
                     * so an update that arrives before wait() is still observed. */
                    while (!owner->stop_flag_.load(std::memory_order_acquire) &&
                           !bell.pending.load(std::memory_order_acquire))
                        wait(work_event);
                    continue;
                }

                /* A time link has no inherent direction.  At Bound we matched each
                 * downstream fss/sim/<tag> sink to its fss/time/<tag> peer.  Count
                 * only those peers, and only their first detach (set_time_window
                 * filters repeats), so an upstream detach cannot tear down a relay
                 * while one of its consumers is still live. */
                bool downstream_quorum = false;
                int quorum_port_index = -1;
                for (int idx : detached) {
                    std::lock_guard<std::mutex> lk(owner->mu_);
                    if (idx >= 0 && idx < owner->n_time_ports_ &&
                        owner->time_ports_[idx].downstream) {
                        ++owner->downstream_detached_;
                        if (!owner->detach_hook_called_ &&
                            owner->n_downstream_time_ != 0 &&
                            owner->downstream_detached_ >= owner->n_downstream_time_) {
                            owner->detach_hook_called_ = true;
                            downstream_quorum = true;
                            quorum_port_index = idx;
                        }
                    }
                }

                /* Finish the batch already accepted by the binding.  This is a
                 * transaction boundary: after it, the model hook may stop safely.
                 * Requests arriving after stop_flag_ is set are rejected, and any
                 * race already in queue_ is completed below with an explicit error. */
                for (const auto &request : local) {
                    bool cancelled;
                    {
                        std::lock_guard<std::mutex> lk(request->mu);
                        cancelled = request->cancel;
                    }
                    bool failed = cancelled;
                    if (!cancelled) {
                        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
                        sc_core::sc_unsuspendable();
                        try {
                            request->fw_if->b_transport(*request->gp, delay);
                        } catch (...) {
                            failed = true;
                        }
                        sc_core::sc_suspendable();
                    }
                    owner->finish_request(request, failed);
                }

                if (downstream_quorum && owner->hooks_ &&
                    owner->hooks_->fss_peer_detached(quorum_port_index)) {
                        owner->stop_and_cancel_pending(false);
                        for (int i = 0; i < owner->n_time_ports_; ++i)
                            if (owner->time_ports_[i].sync) owner->time_ports_[i].sync->detach();
                        sc_core::sc_stop();
                        return;
                }
            }
        }
    };

    void run() noexcept
    {
        kernel_id_ = std::this_thread::get_id();  /* identify this kernel's thread for the same-thread fast path */
        try {
            /* === SystemC context: this binding CREATES its own ===============
             *
             * A transport instantiates an artefact by dlopen + fss_create (see
             * fss/artefact_abi.h).  For a SystemC model that means standing up a
             * simulation kernel.  Four deployment cases:
             *   (1) in-process, INSIDE an existing SystemC context (the artefact
             *       shares the host's SystemC runtime) — a kernel already exists.
             *   (2) in-process, NO SystemC context — need one.
             *   (3) in-machine (forked for SHM) — own process, need one.
             *   (4) remote (e.g. zenoh, far end) — own process, need one.
             * Cases 2/3/4 (almost everything) need a fresh context; only (1)
             * could reuse the host's.
             *
             * This binding ALWAYS creates its own — deliberately, for now: it is
             * correct for 2/3/4, and (1) requires sharing the host's SystemC
             * runtime, which the current per-DLL static-SystemC + RTLD_LOCAL
             * packaging does not do (each artefact gets its own simcontext).
             *
             * OPTIMISATION (deferred): in case (1) the binding could detect a
             * live host kernel and NOT create its own — skip the kernel thread,
             * the sc_start below, AND the broker registration (a second global
             * broker would clash), instead registering Module into the host's
             * hierarchy and riding the host's elaboration.  A best-effort check
             * is `sc_core::sc_is_running()` / `sc_core::sc_get_status()` once the
             * runtime is shared; doing this *safely* whether or not a context
             * exists is precisely what psystemc provides.  Vanilla + always-
             * create is the v0.1 decision; this is the seam to revisit.
             *
             * NB: there is no explicit "create context" call — vanilla SystemC's
             * sc_simcontext is a lazily-created global singleton, brought into
             * being by constructing the first sc_module (Module top, below) and
             * driven by sc_start().  The broker registration assumes we own the
             * context; under the case-(1) optimisation it must be gated too. */
            broker_if_ = new cci_utils::broker("global_broker");
            cci::cci_register_broker(broker_if_);

            Module top(name_.c_str());   /* first sc_module → implicitly creates our simcontext */

            /* Optional model hook: let a model that emits a custom carrier
             * declare its outbound endpoint(s) now (before the bind window so
             * get_endpoint can find them).  No-op for models that don't.  Capture
             * the hooks pointer so dispatch_thread can deliver peer-detach events
             * (the model lives on this kernel thread for the kernel's lifetime). */
            if (auto *h = dynamic_cast<fss_model_hooks *>(&top)) {
                hooks_ = h;
                h->fss_attach(*this);
            }

            wrapper_hub hub("_fss_hub", this);

            /* Bind window: publish top_ and the doorbell, signal host.  Do NOT
             * elaborate yet.  queue_mu_ serialises producer check/ring operations
             * with wrapper_hub teardown. */
            {
                std::lock_guard<std::mutex> qlk(queue_mu_);
                doorbell_ = &hub.bell;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                top_ = &top;
                constructed_ = true;
            }
            construct_cv_.notify_all();

            /* START BARRIER (see header): leave the bind window only once we are
             * wired AND every downstream time-peer is already running.  The sink
             * (no downstream) leaves as soon as it is wired; the wave then flows
             * up.  classify_chain_locked() (run on the Bound event) has set
             * n_downstream_, so this predicate is stable by the time wired_ holds. */
            {
                std::unique_lock<std::mutex> lk(mu_);
                start_cv_.wait(lk, [this] {
                    return (wired_ && downstream_ready_ >= n_downstream_)
                        || stop_flag_.load(std::memory_order_acquire);
                });
            }
            if (!stop_flag_.load(std::memory_order_acquire)) {
                /* If the host bound fss/time (known now — get_endpoint ran in the
                 * bind window), engage ONE window driver PER timing peer BEFORE
                 * elaboration: each is an sc_module gating time via sc_ob_event +
                 * suspend, orthogonal to this sc_start(), composing through the
                 * per-process suspend count.  Run()-local so they tear down with
                 * the kernel; each port's .sync aliases its driver under mu_. */
                std::vector<std::unique_ptr<sync_window_t>> wins;
                if (time_port_ready_) {
                    tlm_utils::tlm_quantumkeeper::set_global_quantum(
                        sc_core::sc_time(1, sc_core::SC_US));   /* quantum = skew bound/hop */
                    std::lock_guard<std::mutex> lk(mu_);
                    for (int i = 0; i < n_time_ports_; ++i) {
                        std::string nm = "_fss_time_window_" + std::to_string(i);
                        wins.emplace_back(new sync_window_t(nm.c_str()));
                        wins[i]->register_sync_cb(
                            [this, i](const win_t &w) { send_peer_window(i, w); });
                        time_ports_[i].sync = wins[i].get();
                    }
                }

                /* From here the kernel is on-thread and elaborating: a same-kernel
                 * FSS carrier (sender on THIS thread) may now dispatch directly. */
                kernel_running_.store(true, std::memory_order_release);
                sc_core::sc_start(sc_core::SC_ZERO_TIME);   /* complete elaboration */
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    elaborated_ = true;
                }
                elaborated_cv_.notify_all();

                /* Elaborated + drivers armed, downstream already running: announce
                 * "running" UP the chain on fss/sim (bound to our upstream peer, or
                 * the orchestrator if we are the source).  Our upstream starts only
                 * on this, so it never sends us a window/poll before we are in
                 * sc_start() below — the guarantee that makes fss/time
                 * fire-and-forget.  Done before sc_start() (which blocks here). */
                send_sim_up(SIM_ELABORATED);

                sc_core::sc_start();                        /* run until sc_stop */

                kernel_running_.store(false, std::memory_order_release);  /* kernel stopped: no more on-thread dispatch */

                /* Kernel stopped.  RELEASE every timing peer: fan out an
                 * open_window so a peer still waiting at its bound — for US to
                 * extend its window — runs free instead of hanging.  Covers NATURAL
                 * completion (the model called sc_stop() itself, e.g. an initiator
                 * that finished), which does NOT pass through dispatch_thread's
                 * stop-detach.  Done WITHOUT holding mu_ (send_peer_window takes mu_
                 * for skew); the open_window is excluded from skew, and re-sending
                 * it if dispatch_thread already detached is harmless.  Then drop the
                 * aliases under mu_ before the drivers destruct. */
                for (int i = 0; i < n_time_ports_; ++i)
                    if (time_ports_[i].sync) send_peer_window(i, sync_window_t::open_window);

                std::lock_guard<std::mutex> lk(mu_);
                for (int i = 0; i < n_time_ports_; ++i) time_ports_[i].sync = nullptr;

                /* Report the time-sync skew for this node: the max clock gap to ANY
                 * timing peer, observed over the run.  A diagnostic, NOT a verdict.
                 * With a correct observable bound (sc_ob_event re-evaluates so the
                 * forcing node lands AT the bound — see sc_ob_event.h) the gap holds
                 * at ~1 quantum: a node never runs past a peer's authorised Upper.
                 * An *unbounded* (run-growing) gap would be the signal something
                 * broke.  See fss-skew-bounded-transient. */
                bool peer_seen = false;
                for (int i = 0; i < n_time_ports_; ++i)
                    if (time_ports_[i].seen) peer_seen = true;
                if (peer_seen) {
                    const sc_core::sc_time ns(1, sc_core::SC_NS);
                    std::cout << "FSS TIMESYNC " << name_
                              << ": max_clock_gap=" << (max_skew_ / ns)
                              << " ns (at t=" << (max_skew_at_ / ns) << " ns peer#"
                              << max_skew_peer_ << ")"
                              << std::endl;
                }
            }

        } catch (const sc_core::sc_report &e) {
            std::cerr << "[fss::systemc_tlm] sc_report: " << e.what() << "\n";
        } catch (const std::exception &e) {
            std::cerr << "[fss::systemc_tlm] exception: " << e.what() << "\n";
        }
        /* Ensure host is never left blocking on either cv. */
        {
            std::lock_guard<std::mutex> lk(mu_);
            constructed_ = true;
            elaborated_  = true;
        }
        construct_cv_.notify_all();
        elaborated_cv_.notify_all();
    }

    /* --- state ------------------------------------------------------------ */

    std::string                 name_;

    /* Set in run() before constructed_ is set; cleared by ~wrapper_hub.
     * queue_mu_ protects this alias and every check/ring operation. */
    Module                     *top_        = nullptr;
    doorbell                   *doorbell_   = nullptr;   /* cross-thread kernel poke */
    cci::cci_broker_if         *broker_if_  = nullptr;   /* for fss/config */
    fss_model_hooks            *hooks_      = nullptr;   /* model hooks (peer-detach) */

    /* Shared dispatch queue and owner-wide registry.  The registry retains both
     * queued and dispatch-local requests until finish_request() completes them. */
    std::deque<request_ptr>     queue_;
    std::deque<request_ptr>     outstanding_;
    std::deque<int>             detach_queue_;   /* fss/time ports that peer-detached */
    std::mutex                  queue_mu_;
    std::atomic<bool>           stop_flag_{false};

    /* Endpoint cache; deque for stable element addresses. */
    std::deque<endpoint_entry>  endpoints_;

    /* fss/sim + fss/config standard control ports (binding-provided).  sim_port_ is
     * the UP-report port (send → upstream/orchestrator, recv the local Bound kick);
     * sim_down_ holds one inbound sink per DOWNSTREAM peer (fss/sim/<tag>). */
    sim_port                    sim_port_{};
    bool                        sim_port_ready_ = false;
    static constexpr int        MAX_SIM_DOWN = 4;
    sim_port                    sim_down_[MAX_SIM_DOWN]{};
    int                         n_sim_down_ = 0;
    config_port                 config_port_{};
    bool                        config_port_ready_ = false;

    /* fss/time: one port + one window driver per timing peer (see ready_time_port).
     * Each port's driver is aliased in time_ports_[i].sync, set under mu_ in run()
     * and cleared before the kernel thread leaves run().  time_port_ready_ arms
     * the drivers. */
    time_port                   time_ports_[MAX_TIME_PORTS]{};
    int                         n_time_ports_ = 0;
    bool                        time_port_ready_ = false;

    /* Chain role.  The tagged sim sinks identify downstream children.  The matched
     * time ports are the subset that can report a timing detach. */
    bool                        chain_classified_ = false;
    int                         n_downstream_   = 0;        /* fss/sim/<tag> peers */
    int                         n_downstream_time_ = 0;     /* matched fss/time peers */
    int                         downstream_detached_ = 0;  /* unique matched detaches */
    bool                        detach_hook_called_ = false;
    bool                        wired_            = false;   /* sim::event{Bound} seen */
    int                         downstream_ready_ = 0;       /* downstream Elaborated count */

    /* Max clock gap observed to ANY timing peer (under mu_; see send_peer_window),
     * with the sim-time + peer at which it occurred (to locate transients). */
    sc_core::sc_time            max_skew_{};
    sc_core::sc_time            max_skew_at_{};
    int                         max_skew_peer_ = -1;

    std::thread                 kernel_;
    std::thread::id             kernel_id_{};          /* set at run() entry; identifies THIS kernel's thread */
    std::atomic<bool>           kernel_running_{false}; /* true once run() is on-thread and elaboration is under way */
    std::mutex                  mu_;
    std::condition_variable     construct_cv_;
    bool                        constructed_ = false;
    bool                        elaborated_  = false;
    std::condition_variable     start_cv_;
    std::condition_variable     elaborated_cv_;
};

} // namespace fss::systemc_tlm

#endif
