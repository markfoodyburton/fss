/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * agent_core.c — the generic agent (see agent_core.h).  NON-NORMATIVE.
 *
 * Construction of every node goes through the model-loader vtable (the one
 * per-language seam); everything else — control interpretation, per-edge
 * channel attach on shm/zenoh, the readiness wave, teardown ordering — is
 * shared and knows nothing of any language.
 *
 * The agent runs two ways from the SAME core:
 *   - REMOTE (fss_agent_run): born with a home control channel, it blocks
 *     reading instructions off shm, then arms/waits/tears down and exits (its
 *     exit code is the delivery verdict when it hosts the counting sink).
 *   - IN-PROC (fss_agent_open/ctrl/arm/…): the orchestrator hosts it in its own
 *     process and feeds instructions by direct call, so a co-located node runs
 *     through exactly the same core — no privileged direct-hosted path.
 * Both share agent_open_transports / agent_do_instr / agent_arm / agent_stop /
 * agent_teardown below.
 */
#include "fss/runtime/agent_core.h"
#include "fss/runtime/orchestrator.h"        /* shared plumbing + taps */
#include "fss/runtime/control_protocol.h"
#include "fss/runtime/placement.h"           /* fss_place_resolve */
#include "fss/protocols/control_plan.h" /* FSS_CONTROL_PLAN_ID/META (R5) */
#include "fss/c/meta.h"                       /* fss_meta_find (PlanJson/Status offsets) */

#include <stdint.h>
#include <stdatomic.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <dlfcn.h>
#include <limits.h>

/* Private launcher barrier records.  They never cross an FSS transport ABI. */
#define FSS_BARRIER_MAGIC 0x46534252u
enum { FSS_BARRIER_ATTACHED = 1, FSS_BARRIER_READY = 2, FSS_BARRIER_NACK = 3,
       FSS_BARRIER_RELEASE = 4, FSS_BARRIER_STOP = 5, FSS_BARRIER_PROBE = 6,
       FSS_BARRIER_TRANSPORT_READY = 7 };
typedef struct { uint64_t nonce; uint32_t magic; uint32_t state; } fss_barrier_record;

static int barrier_write(int fd, uint64_t nonce, uint32_t state)
{
    fss_barrier_record r = { nonce, FSS_BARRIER_MAGIC, state };
    size_t put = 0;
    if (fd < 0) return 0;
    while (put < sizeof(r)) {
        ssize_t n = write(fd, (const char *)&r + put, sizeof(r) - put);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 1;
        put += (size_t)n;
    }
    return 0;
}

static int barrier_read_state(int fd, uint64_t nonce, uint32_t state)
{
    fss_barrier_record r; size_t got = 0;
    if (fd < 0) return 0;
    while (got < sizeof(r)) {
        ssize_t n = read(fd, (char *)&r + got, sizeof(r) - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 1;
        got += (size_t)n;
    }
    return r.magic != FSS_BARRIER_MAGIC || r.nonce != nonce || r.state != state;
}

/* After READY the same private parent->child pipe carries STOP.  It is not a
 * lifecycle transport: polling it lets a counting agent leave its wait without
 * waiting for a transport reader or a control send. */
static int barrier_poll_stop(int fd, uint64_t nonce, int timeout_ms)
{
    fss_barrier_record r;
    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int pr;
    ssize_t n;

    if (fd < 0) return 0;
    do { pr = poll(&pfd, 1, timeout_ms); } while (pr < 0 && errno == EINTR);
    if (pr <= 0) return pr < 0 ? -1 : 0;
    if (pfd.revents & (POLLERR | POLLNVAL)) return -1;
    if (!(pfd.revents & (POLLIN | POLLHUP))) return 0;
    do { n = read(fd, &r, sizeof(r)); } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n != (ssize_t)sizeof(r) || r.magic != FSS_BARRIER_MAGIC ||
        r.nonce != nonce || r.state != FSS_BARRIER_STOP) return -1;
    return 1;
}

static int64_t agent_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Startup has no control transport that is safe to rely on yet.  Report every
 * failure through the private barrier, then close both ends so the launcher
 * cannot wait for a state that this agent will never reach. */
static void barrier_nack_and_close(int status_fd, int release_fd, uint64_t nonce)
{
    (void)barrier_write(status_fd, nonce, FSS_BARRIER_NACK);
    if (status_fd >= 0) close(status_fd);
    if (release_fd >= 0) close(release_fd);
}

fss_node_t *fss_reg_find(fss_reg_entry *reg, int n, const char *name)
{
    for (int i = 0; i < n; i++) if (!strcmp(reg[i].name, name)) return reg[i].node;
    return NULL;
}

/* The agent's whole state.  Data-edge transports are loaded LAZILY: an
 * OP_SERVER/OP_CLIENT names a transport (control::Transport, e.g. "shm"/"zenoh"),
 * the agent dlopens libfss_<name>.<ext> from its transport dir on first use,
 * confirms the plugin via fss_transport_name(), and caches it.
 * It holds NO topology map: it executes whatever instructions arrive. */
#define MAX_CH 8
#define MAX_XPORTS 8
#define MAX_FANOUT 8   /* peers a single one-way source endpoint can fan out to */
/* These are optional Zenoh plugin extensions.  They are deliberately kept out
 * of the transport vtable: old and non-Zenoh transports retain the ABI. */
typedef int (*fss_zenoh_set_edge_fn)(fss_transport_t *, const char *, const char *, int);
typedef int (*fss_zenoh_wait_ready_fn)(fss_transport_t *, uint32_t);
#ifndef FSS_TRANSPORT_CAP_REMOTE_READY
#define FSS_TRANSPORT_CAP_REMOTE_READY UINT64_C(8)
#endif
typedef struct {
    char name[32]; fss_chan_lib lib; int loaded;
    fss_zenoh_set_edge_fn set_edge;
    fss_zenoh_wait_ready_fn wait_ready;
    int remote_ready;
} fss_xport;
typedef struct { fss_transport_t *transport; fss_zenoh_wait_ready_fn wait_ready; } fss_ready_check;

/* A data-plane FAN-OUT record: one local SOURCE endpoint feeding N peer sends
 * (the agent-core analogue of inproc.cpp's inproc_fanout).  The 1st peer on a
 * source is wired directly (the identity fast path, unchanged); a 2nd peer turns
 * the source into a shim (agent_fanout_send) that delivers to every peer in turn.
 * A peer is a (send, ctx) pair: for an OP_CLIENT source it is a cross channel's
 * INITIATOR send (shm can't share a segment, so each fan-out target is its OWN
 * shm channel — the SENDER shims across them); for an OP_BIND source it is the
 * target's receive, or the frame-count oracle when the target edge is counted. */
typedef struct {
    fss_endpoint_t *src;                    /* the local source endpoint being shimmed */
    fss_carrier_fn  peer_send[MAX_FANOUT];
    void           *peer_ctx[MAX_FANOUT];
    int             n;
} fss_fanout;
struct fss_agent {
    const fss_model_loader_t *loader;     /* the per-language model seam */
    const char      *xport_dir;           /* dir to resolve libfss_<name>.<ext> data plugins in */
    const char      *model_dir;           /* dir to resolve lib<name>.<ext> MODEL plugins in (by name) */
    fss_xport        xports[MAX_XPORTS]; int nxport;  /* lazily-loaded data-edge transport plugins */
    const char      *zrole, *zrv;         /* this process's zenoh rendezvous role + endpoint */
    fss_transport_t *ctl;                 /* TOPOLOGY control channel (remote only; NULL when in-proc) */
    fss_chan_lib    *ctl_lib;             /* the control transport's lib (a registry entry; NULL when in-proc) */
    fss_endpoint_t   control_ep;          /* R11: control INITIATOR send endpoint (the agent PULLS its plan) */
    fss_transport_t *lc;                  /* SIM-CONTROL lifecycle channel (remote only): receives stop */
    fss_endpoint_t   lc_ep;               /* lifecycle TARGET sink (sets A->stop on a stop push) */
    int              frames;              /* completion bound */
    fss_reg_entry    reg[FSS_MAX_NODES]; int nreg;
    fss_transport_t *inbound[MAX_CH];  fss_chan_lib *in_lib[MAX_CH];  int n_inbound;   /* servers: destroy BEFORE close(node) */
    fss_transport_t *outbound[MAX_CH]; fss_chan_lib *out_lib[MAX_CH]; int n_outbound;  /* clients: destroy AFTER close(node) */
    fss_ready_check  ready_checks[MAX_CH * 2]; int n_ready_checks;
    fss_counting_ctx cs[MAX_CH];          /* frame-count oracles (one per counted data sink) */
    fss_endpoint_t   count_sink[MAX_CH];  /* persistent sink endpoints wrapping each counted data ep */
    int              n_counted;           /* number of FLAG_COUNT sinks in THIS agent (fan-out: >1) */
    fss_fanout       fanout[MAX_CH]; int n_fanout;  /* one-way source fan-out shims (data-plane) */
    volatile int     count;               /* total frames across ALL counted sinks in this agent */
    volatile int     counting;            /* set once a FLAG_COUNT sink is installed: this agent hosts dev */
    volatile int     done;
    _Atomic int      stop;                /* OP_STOP arrived (non-counting agent teardown trigger) */
    int              barrier_fd;          /* private post-READY STOP pipe; -1 when standalone */
    uint64_t         barrier_nonce;
    char             cfg_json[64];        /* run config; each process configures its own nodes LOCALLY */
};

static void agent_teardown(struct fss_agent *A);

/* Resolve a data-edge transport by NAME (control::Transport), loading its plugin
 * lazily on first use.  The plugin file is libfss_<name>.<ext> in the agent's
 * transport dir; the loaded plugin is confirmed by matching fss_transport_name()
 * against the requested name (transport_abi.h).  Returns the cached channel lib,
 * or NULL on any failure (unknown name, missing file, name mismatch). */
static fss_chan_lib *agent_transport(struct fss_agent *A, const char *name)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < A->nxport; i++)
        if (!strcmp(A->xports[i].name, name)) return A->xports[i].loaded ? &A->xports[i].lib : NULL;
    if (A->nxport >= MAX_XPORTS) return NULL;
    fss_xport *x = &A->xports[A->nxport];
    snprintf(x->name, sizeof(x->name), "%s", name);
    if (fss_load_channel_named(A->xport_dir, name, &x->lib)) return NULL;
    if (!strcmp(name, "zenoh")) {
        fss_transport_capabilities_fn_t caps =
            (fss_transport_capabilities_fn_t)dlsym(x->lib.dl, "fss_transport_capabilities");
        x->set_edge = (fss_zenoh_set_edge_fn)dlsym(x->lib.dl, "fss_zenoh_set_edge");
        x->wait_ready = (fss_zenoh_wait_ready_fn)dlsym(x->lib.dl, "fss_zenoh_wait_ready");
        x->remote_ready = caps && ((caps() & FSS_TRANSPORT_CAP_REMOTE_READY) != 0);
    }
    x->loaded = 1; A->nxport++;
    return &x->lib;
}

static fss_xport *agent_xport_for(struct fss_agent *A, fss_chan_lib *lib)
{
    for (int i = 0; i < A->nxport; i++) if (&A->xports[i].lib == lib) return &A->xports[i];
    return NULL;
}

static int agent_set_zenoh_edge(struct fss_agent *A, fss_chan_lib *lib, fss_transport_t *t,
                                const char *local, const char *peer, fss_role role)
{
    fss_xport *x = agent_xport_for(A, lib);
    if (!x || !x->remote_ready) return 0;
    if (!x->set_edge || !x->wait_ready || !local || !local[0] || !peer || !peer[0]) return 1;
    if (x->set_edge(t, local, peer, (int)role) != FSS_SEND_OK) return 1;
    /* Targets publish their token during attach.  Only initiators wait for the
     * peer's token, so do not turn a valid target into a failing check. */
    if (role != FSS_ROLE_INITIATOR) return 0;
    if (A->n_ready_checks >= (int)(sizeof(A->ready_checks) / sizeof(A->ready_checks[0]))) return 1;
    A->ready_checks[A->n_ready_checks++] = (fss_ready_check){ t, x->wait_ready };
    return 0;
}

/* Does a "node/ep" locator name the standard fss/config control port?  A config
 * edge's TARGET is served by the config RESPONDER (fan-in: N pullers -> 1
 * responder), not by the model's own config-set sink — see OP_SERVER/OP_BIND. */
static int is_config_loc(const char *loc)
{
    const char *s = loc ? strstr(loc, "fss/config") : NULL;
    return s && s[10] == '\0';   /* the endpoint tail is exactly "fss/config" */
}

/* Resolve "node/ep" against the agent's registry via the placement facility.
 * Endpoint resolution goes through fss_place_endpoint for a cdylib node. */
static fss_endpoint_t *agent_resolve(struct fss_agent *A, const char *node_ep)
{
    char buf[128]; snprintf(buf, sizeof(buf), "%s", node_ep);
    char *slash = strchr(buf, '/');
    if (!slash) return NULL;
    *slash = '\0';
    fss_node_t *n = fss_reg_find(A->reg, A->nreg, buf);
    if (!n) return NULL;
    return A->loader->get_endpoint(A->loader->self, n, slash + 1);
}

/* Data-plane fan-out shim: deliver the carrier to every peer of a one-way source
 * in turn (mirrors inproc.cpp's inproc_fanout).  One-way only, so there is no
 * write-back to ferry; the per-peer status is aggregated as OK unless a peer
 * errors (remember the last non-OK). */
static int agent_fanout_send(void *ctx, fss_carrier_t *c)
{
    fss_fanout *f = (fss_fanout *)ctx;
    int agg = FSS_SEND_OK;
    for (int i = 0; i < f->n; i++) {
        int rc = f->peer_send[i] ? f->peer_send[i](f->peer_ctx[i], c) : FSS_SEND_NO_RECEIVER;
        if (rc != FSS_SEND_OK) agg = rc;
    }
    return agg;
}

/* Wire an outbound peer (send, ctx) onto a local SOURCE endpoint, fanning out
 * when a source feeds more than one target (1 source -> N targets on a one-way
 * channel).  1st peer: the identity fast path (src->send = the peer directly —
 * behaviour is byte-identical to the single-target path).  2nd peer: migrate the
 * existing wiring and the new peer into a fanout record and install the shim.
 * 3rd+: append to the record.  `prev_*` is the wiring present on src BEFORE this
 * edge (NULL if none) — an OP_CLIENT attach overwrites src->send, so the caller
 * captures it before attach and passes it here. */
static int agent_fanout_add(struct fss_agent *A, fss_endpoint_t *src,
                            fss_carrier_fn prev_send, void *prev_ctx,
                            fss_carrier_fn new_send, void *new_ctx)
{
    for (int i = 0; i < A->n_fanout; i++) {   /* already fanning out on this source? append */
        if (A->fanout[i].src == src) {
            fss_fanout *f = &A->fanout[i];
            if (f->n >= MAX_FANOUT) return 1;
            f->peer_send[f->n] = new_send; f->peer_ctx[f->n] = new_ctx; f->n++;
            src->send = agent_fanout_send; src->send_ctx = f;
            return 0;
        }
    }
    if (prev_send) {   /* 2nd peer on this source: start fanning out */
        if (A->n_fanout >= MAX_CH) return 1;
        fss_fanout *f = &A->fanout[A->n_fanout++];
        f->src = src; f->n = 0;
        f->peer_send[f->n] = prev_send; f->peer_ctx[f->n] = prev_ctx; f->n++;
        f->peer_send[f->n] = new_send;  f->peer_ctx[f->n]  = new_ctx;  f->n++;
        src->send = agent_fanout_send; src->send_ctx = f;
        return 0;
    }
    src->send = new_send; src->send_ctx = new_ctx;   /* 1st peer: identity fast path */
    return 0;
}

/* Interpret ONE control instruction against the agent's state.  Each op mutates
 * agent_state and writes in->status (0 = ok); shared by the remote reader thread
 * (ctrl_interpret) and the in-proc driver (fss_agent_ctrl). */
static int agent_do_instr(struct fss_agent *A, fss_ctrl_instr *in)
{
    in->status = 0;
    switch (in->op) {
    case FSS_OP_PING:
        break;
    case FSS_OP_CONSTRUCT: {   /* bring the node to life through the model loader */
        if (A->nreg >= FSS_MAX_NODES) { in->status = 1; break; }
        /* arg1 is a model NAME: resolve it to a plugin file against the model dir
         * (name -> plugin, never a path — the same discipline as transports). */
        char mpath[512];
        fss_model_plugin_path(A->model_dir, in->arg1, mpath, sizeof(mpath));
        fss_node_t *n = A->loader->construct(A->loader->self, in->arg0, mpath);
        if (!n) { in->status = 1; break; }
        A->reg[A->nreg].name = strdup(in->arg0);
        A->reg[A->nreg].node = n; A->nreg++;
        break;
    }
    case FSS_OP_SERVER: {   /* inbound channel: channel(arg1) owner, attach TARGET -> arg0 */
        fss_chan_lib *lib = agent_transport(A, in->transport);
        int zenoh = !strcmp(in->transport, "zenoh");
        if (!lib) { in->status = 4; break; }
        fss_endpoint_t *ep = agent_resolve(A, in->arg0);
        if (!ep || A->n_inbound >= MAX_CH ||
            ((in->flags & FSS_CFLAG_COUNT) && A->n_counted >= MAX_CH)) {
            in->status = 1; break;
        }
        fss_carrier_fn old_receive = ep->receive;
        void *old_self = ep->self;
        /* Config fan-in TARGET: init serves the run config to the pullers over this
         * round-trip channel.  Install the RESPONDER on init's fss/config sink
         * (self = cfg_json) so the channel's reader thread answers each puller's GET
         * in place.  fss_config_responder is stateless/reentrant (reads the request,
         * writes the reply from cfg_json, no shared mutable state), so the N shm
         * reader threads — one per per-edge round-trip segment — share it with NO
         * lock (mirror of the proven shm fan-in spike).  init's own model config
         * receive is unused (it PULLS its config via fss/config.send), so replacing
         * receive/self here is safe. */
        if (is_config_loc(in->arg0)) { ep->receive = fss_config_responder; ep->self = A->cfg_json; }
        char cfg[256];
        fss_chan_session_cfg(zenoh, A->zrole, A->zrv, cfg, sizeof(cfg));
        fss_transport_t *t = fss_chan_create_retry(lib, cfg);
        if (!t) { ep->receive = old_receive; ep->self = old_self; in->status = 2; break; }
        if (zenoh && agent_set_zenoh_edge(A, lib, t, in->arg0, in->arg2, FSS_ROLE_TARGET)) {
            ep->receive = old_receive; ep->self = old_self; lib->destroy(t); in->status = 2; break;
        }
        fss_channel_t *ch = fss_chan_open_retry(t, in->arg1, in->roundtrip, 0 /*attach: orchestrator provisioned it (R11)*/);
        if (!ch) { ep->receive = old_receive; ep->self = old_self; lib->destroy(t); in->status = 2; break; }
        if (in->flags & FSS_CFLAG_COUNT) {   /* wrap the data sink with the frame oracle */
            int ix = A->n_counted;
            A->cs[ix].inner = ep; A->cs[ix].count = (int *)&A->count;
            A->count_sink[ix].self = &A->cs[ix]; A->count_sink[ix].receive = fss_counting_receive;
            if (t->attach(t->self, ch, &A->count_sink[ix], FSS_ROLE_TARGET) != FSS_SEND_OK) {
                ep->receive = old_receive; ep->self = old_self;
                lib->destroy(t); in->status = 3; break;
            }
            A->n_counted++;
            A->counting = 1;
        } else {
            if (t->attach(t->self, ch, ep, FSS_ROLE_TARGET) != FSS_SEND_OK) {
                ep->receive = old_receive; ep->self = old_self;
                lib->destroy(t); in->status = 3; break;
            }
        }
        A->in_lib[A->n_inbound] = lib;
        A->inbound[A->n_inbound++] = t;
        break;
    }
    case FSS_OP_CLIENT: {   /* outbound channel: channel(arg1) attacher, attach INITIATOR arg0 */
        fss_chan_lib *lib = agent_transport(A, in->transport);
        int zenoh = !strcmp(in->transport, "zenoh");
        if (!lib) { in->status = 4; break; }
        fss_endpoint_t *ep = agent_resolve(A, in->arg0);
        if (!ep) { if (in->flags & FSS_CFLAG_OPTIONAL) break;   /* absent optional ep (e.g. no config pull) */
                   in->status = 1; break; }
        if (A->n_outbound >= MAX_CH) { in->status = 1; break; }
        char cfg[256];
        fss_chan_session_cfg(zenoh, A->zrole, A->zrv, cfg, sizeof(cfg));
        fss_transport_t *t = fss_chan_create_retry(lib, cfg);
        if (!t) { in->status = 2; break; }
        if (zenoh && agent_set_zenoh_edge(A, lib, t, in->arg0, in->arg2, FSS_ROLE_INITIATOR)) {
            lib->destroy(t); in->status = 2; break;
        }
        fss_channel_t *ch = fss_chan_open_retry(t, in->arg1, in->roundtrip, 0 /*attacher*/);
        if (!ch) { lib->destroy(t); in->status = 2; break; }
        /* Capture any wiring already on this source BEFORE attach overwrites
         * ep->send: a 2nd+ OP_CLIENT on the same source is a fan-out (1->N on a
         * one-way channel, e.g. shm's N independent segments) — the attach installs
         * THIS channel's INITIATOR send, and agent_fanout_add shims the source
         * across all peers' sends.  The 1-target case is the identity fast path. */
        fss_carrier_fn prev_send = ep->send; void *prev_ctx = ep->send_ctx;
        if (t->attach(t->self, ch, ep, FSS_ROLE_INITIATOR) != FSS_SEND_OK) {
            ep->send = prev_send; ep->send_ctx = prev_ctx;
            lib->destroy(t); in->status = 3; break;
        }
        fss_carrier_fn new_send = ep->send; void *new_ctx = ep->send_ctx;
        if (agent_fanout_add(A, ep, prev_send, prev_ctx, new_send, new_ctx)) {
            ep->send = prev_send; ep->send_ctx = prev_ctx;
            lib->destroy(t); in->status = 1; break;
        }
        A->out_lib[A->n_outbound] = lib;
        A->outbound[A->n_outbound++] = t;
        break;
    }
    case FSS_OP_BIND: {   /* in-process edge inside the agent: arg0/.send -> arg1/.receive */
        fss_endpoint_t *from = agent_resolve(A, in->arg0);
        fss_endpoint_t *to   = agent_resolve(A, in->arg1);
        if (!from || !to || ((in->flags & FSS_CFLAG_COUNT) && A->n_counted >= MAX_CH)) {
            in->status = 1; break;
        }
        /* The new peer is either the frame-count oracle (counted edge) or the
         * target's receive (direct identity wire).  agent_fanout_add installs it,
         * fanning `from` out when the same source feeds >1 target (1->N one-way —
         * e.g. an in-proc CAN broadcast to two devices in one agent). */
        fss_carrier_fn new_send; void *new_ctx;
        int counted = 0;
        if (is_config_loc(in->arg1)) {  /* in-proc config fan-in: bind the puller's
             * send straight to init's config RESPONDER (self = cfg_json), the same
             * server the cross-process OP_SERVER installs.  An inproc config edge
             * unions the puller and init into one group, so this is a direct
             * same-process round-trip call; N pullers binding the one responder IS
             * the in-proc N->1 fan-in (no shim: distinct sources, one sink fn). */
            new_send = fss_config_responder; new_ctx = A->cfg_json;
        } else if (in->flags & FSS_CFLAG_COUNT) {   /* tap the data edge with the frame oracle */
            int ix = A->n_counted++;
            A->cs[ix].inner = to; A->cs[ix].count = (int *)&A->count;
            new_send = fss_counting_receive; new_ctx = &A->cs[ix];
            A->counting = 1;
            counted = 1;
        } else {                        /* direct identity wire: send = peer->receive */
            new_send = to->receive; new_ctx = to->self;
        }
        if (agent_fanout_add(A, from, from->send, from->send_ctx, new_send, new_ctx)) {
            if (counted) {
                A->n_counted--;
                if (A->n_counted == 0) A->counting = 0;
            }
            in->status = 1;
        }
        break;
    }
    case FSS_OP_STOP:
        atomic_store_explicit(&A->stop, 1, memory_order_release);
        break;
    case FSS_OP_DONE:
        A->done = 1;
        break;
    default:
        in->status = 255u;
    }
    return FSS_SEND_OK;   /* round-trip: shm ships `in` (with status) back */
}

/* The remote control sink (runs on the shm control reader thread).  Dispatches by
 * carrier id: a control::plan carrier (R5) carries the agent's WHOLE build+wire
 * plan as a JSON PlanJson slot — read it and run every step in one go; a
 * control::instr carrier is a single lifecycle op (ARM/STOP/…). */
static int ctrl_interpret(void *self, fss_carrier_t *c)
{
    struct fss_agent *A = (struct fss_agent *)self;
    if (!c || !c->data_ptr) return FSS_SEND_NO_RECEIVER;

    if (c->id == FSS_CONTROL_PLAN_ID) {
        uint32_t pj_off, pj_size;
        if (!c->meta_ptr ||
            !fss_meta_find(c->meta_ptr, c->meta_size, "control::PlanJson", &pj_off, &pj_size) ||
            (size_t)pj_off + pj_size > c->data_size) return FSS_SEND_NO_RECEIVER;
        int rc = fss_agent_run_plan(A, (const char *)c->data_ptr + pj_off);
        uint32_t st_off, st_size;   /* write control::Status back over the round-trip */
        if (fss_meta_find(c->meta_ptr, c->meta_size, "control::Status", &st_off, &st_size)
            && st_size >= sizeof(uint32_t) && (size_t)st_off + st_size <= c->data_size) {
            uint32_t s = (uint32_t)rc; memcpy((uint8_t *)c->data_ptr + st_off, &s, sizeof(s));
        }
        return rc;
    }

    if (c->data_size < sizeof(fss_ctrl_instr)) return FSS_SEND_NO_RECEIVER;
    return agent_do_instr(A, (fss_ctrl_instr *)c->data_ptr);
}

/* ── shared lifecycle pieces (used by both the remote and in-proc entries) ──── */

/* Stash the transport-plugin search dir (data-edge transports load lazily by
 * name, see agent_transport) and the zenoh rendezvous facts, build the run
 * config.  The inproc_plugin arg is accepted but unused (kept for launcher
 * compatibility — the launcher still passes --inproc, and we just ignore it). */
static int agent_open_transports(struct fss_agent *A, const fss_model_loader_t *loader,
                                 const char *inproc_plugin, const char *xport_dir,
                                 const char *model_dir,
                                 const char *zrole, const char *zrv,
                                 int frames, int period_us)
{
    (void)inproc_plugin;   /* no longer used: construction is via the placement facility */
    if (!loader || frames <= 0) return 1;
    memset(A, 0, sizeof(*A));
    atomic_init(&A->stop, 0);
    A->loader = loader; A->xport_dir = xport_dir; A->model_dir = model_dir;
    A->zrole = zrole; A->zrv = zrv; A->frames = frames;
    A->barrier_fd = -1;
    fss_build_config_json(frames, period_us, A->cfg_json, sizeof(A->cfg_json));
    return 0;
}

/* PREPARE configures each hosted node locally.  BOUND is separate; the registry
 * is constructed in reverse plan order, so forward registry order delivers it
 * from sink to source.  Each process
 * configures ITS OWN nodes: the run JSON is identical everywhere (born-with
 * frames/period), so a node's config pull is served by a local responder and no
 * config channel crosses the process boundary.  Optional: absent on rust/fmi
 * controllers. */
static void agent_prepare(struct fss_agent *A)
{
    for (int i = 0; i < A->nreg; i++) {
        fss_endpoint_t *cfg_ep = A->loader->get_endpoint(A->loader->self, A->reg[i].node, "fss/config");
        /* Install the LOCAL config responder ONLY if nothing already wired this
         * node's config send.  A PULLER (config fan-in) already had its fss/config
         * send set to the cross-process channel INITIATOR by OP_CLIENT — or, in the
         * all-inproc cell, bound to init's responder by OP_BIND — so do NOT clobber
         * it, else the node would answer ITSELF locally instead of pulling from init.
         * A node with no config edge (the single-device path / the 576) has a NULL
         * config send here and gets the local responder exactly as before. */
        if (cfg_ep && !cfg_ep->send) { cfg_ep->send = fss_config_responder; cfg_ep->send_ctx = A->cfg_json; }
    }
}

static void agent_bound(struct fss_agent *A)
{
    for (int i = 0; i < A->nreg; i++)
        fss_local_sim(A->loader->get_endpoint(A->loader->self, A->reg[i].node, "fss/sim"), FSS_SIM_BOUND);
}

static int agent_ready_timeout_ms(void)
{
    const char *s = getenv("FSS_TRANSPORT_READY_TIMEOUT_MS");
    char *end = NULL;
    long v;
    if (!s || !s[0]) return 30000;
    errno = 0; v = strtol(s, &end, 10);
    return errno || !end || *end || v <= 0 || v > INT_MAX ? 30000 : (int)v;
}

static int agent_transport_ready(struct fss_agent *A)
{
    int timeout = agent_ready_timeout_ms();
    for (int i = 0; i < A->n_ready_checks; i++)
        if (!A->ready_checks[i].wait_ready ||
            A->ready_checks[i].wait_ready(A->ready_checks[i].transport, (uint32_t)timeout) != FSS_SEND_OK)
            return 1;
    return 0;
}

static void agent_arm(struct fss_agent *A) { agent_prepare(A); agent_bound(A); }

/* A counting agent (hosts dev) waits for the frames to land; a non-counting
 * agent (e.g. a lone ctrl in the shm/shm split) has no local frame count and
 * waits for STOP.  A remote agent observes its private post-READY pipe; the
 * lifecycle receiver remains available for the existing standalone behaviour.
 * Returns 1 on a clean completion. */
static int agent_wait_delivery(struct fss_agent *A)
{
    /* Fan-out in ONE agent (e.g. in-proc broadcast to 2 devices): each counted
     * sink contributes A->frames to the shared count, so the bound scales with
     * how many counted sinks this agent hosts (1 in the single-device path). */
    int target = A->frames * A->n_counted;
    int timeout = A->counting ? 15000 : 30000;
    int64_t deadline = agent_monotonic_ms();
    if (deadline < 0) return 0;
    deadline += timeout;

    /* Do not use fss_wait_for_count here.  It cannot observe the private STOP
     * pipe while blocked.  poll wakes immediately for STOP; in-proc agents have
     * no barrier fd and retain the former short sleep wait. */
    for (;;) {
        if (atomic_load_explicit(&A->stop, memory_order_acquire)) return 1;
        if (A->counting && A->count >= target) return 1;
        int64_t now = agent_monotonic_ms();
        if (now < 0 || now >= deadline) return 0;
        int64_t remain = deadline - now;
        int delay = remain < 5 ? (int)remain : 5;
        if (A->barrier_fd >= 0) {
            int stopped = barrier_poll_stop(A->barrier_fd, A->barrier_nonce, delay);
            if (stopped != 0) {
                atomic_store_explicit(&A->stop, 1, memory_order_release);
                return stopped > 0;
            }
        } else fss_msleep(delay);
    }
}

/* Fire SIM_STOPPED into every hosted node, then a short settle before teardown. */
static void agent_stop(struct fss_agent *A)
{
    for (int i = 0; i < A->nreg; i++)
        fss_local_sim(A->loader->get_endpoint(A->loader->self, A->reg[i].node, "fss/sim"), FSS_SIM_STOPPED);
    fss_msleep(200);
}

/* Teardown: inbound (readers sink INTO our nodes) BEFORE destroy; outbound (our
 * clock sends THROUGH them, destroy joins the clock) AFTER destroy; then the
 * control channel.  The loader's destroy calls fss_place_destroy which
 * fss_destroy's and dlcloses the artefact DLL. */
static void agent_teardown(struct fss_agent *A)
{
    for (int i = 0; i < A->n_inbound; i++)  A->in_lib[i]->destroy(A->inbound[i]);
    for (int i = 0; i < A->nreg; i++) {
        A->loader->destroy(A->loader->self, A->reg[i].node);
        free((void *)A->reg[i].name);
    }
    for (int i = 0; i < A->n_outbound; i++) A->out_lib[i]->destroy(A->outbound[i]);
    if (A->lc  && A->ctl_lib) A->ctl_lib->destroy(A->lc);    /* lifecycle channel */
    if (A->ctl && A->ctl_lib) A->ctl_lib->destroy(A->ctl);   /* topology channel */
}

/* ── JSON plan (R5) ───────────────────────────────────────────────────────────
 * The control carrier is a JSON PLAN document: an ordered array of step objects
 * `[{"op":N,"a0":..,"a1":..,"a2":..,"xport":..,"rt":N,"fl":N}, ...]` in sink-first
 * order (the orchestrator builds it; fss_build_plan_step appends).  An agent PULLS
 * its whole plan once and runs every step through the existing agent_do_instr —
 * the JSON is only the transport format; the executor is unchanged.  These are
 * fixed-schema scanners (not a general JSON parser): our step values are ints and
 * quoted strings with no nested objects. */

/* Required object-scoped uint field in [obj,end): "key": <int>. */
static int fss_json_int(const char *obj, const char *end, const char *key, uint32_t *out)
{
    char pat[24]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return 0;
    p = strchr(p + strlen(pat), ':'); if (!p || p >= end) return 0;
    p++; while (*p == ' ' || *p == '\t') p++;
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    char *tail;
    unsigned long v = strtoul(p, &tail, 10);
    if (tail == p || tail > end || v > UINT32_MAX) return 0;
    while (tail < end && (*tail == ' ' || *tail == '\t')) tail++;
    /* `end` is the closing brace.  A final member ends at that brace; every
     * other member must have a comma delimiter. */
    if (tail != end && *tail != ',') return 0;
    *out = (uint32_t)v;
    return 1;
}
/* Object-scoped string field in [obj,end): "key": "value".  Empty if absent. */
static int fss_json_str(const char *obj, const char *end, const char *key, char *out, size_t n)
{
    if (n) out[0] = '\0';
    char pat[24]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return 0;
    p = strchr(p + strlen(pat), ':'); if (!p || p >= end) return 0;
    p++; while (*p == ' ' || *p == '\t') p++;
    if (p >= end || *p != '"') return 0;
    p++;
    size_t i = 0;
    while (p < end && *p != '"') {
        if (*p == '\\') {
            if (p + 1 >= end) return 0;
            p++;
        }
        if (i + 1 >= n) return 0;
        out[i++] = *p++;
    }
    if (p >= end || *p != '"') return 0;
    out[i] = '\0';
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    /* As for integers, allow the final member to terminate at `}`. */
    if (p != end && *p != ',') return 0;
    return 1;
}

static const char *fss_json_object_end(const char *p)
{
    int quoted = 0;
    for (; *p; p++) {
        if (quoted && *p == '\\' && p[1]) { p++; continue; }
        if (*p == '"') quoted = !quoted;
        else if (!quoted && *p == '}') return p;
    }
    return NULL;
}

/* Run every step of a JSON plan through agent_do_instr, in order.  Returns 0 on
 * success, or the first failing step's status.  (Whatever steps the plan holds:
 * construct+wire[+DONE] for a remote agent; construct+wire only for the in-proc
 * p0 agent, whose arm the orchestrator drives after installing the root latch.) */
int fss_agent_run_plan(struct fss_agent *A, const char *plan_json)
{
    if (!A || !plan_json) return 1;
    const char *p = plan_json;
    while (isspace((unsigned char)*p)) p++;
    if (*p++ != '[') return 1;
    int nsteps = 0;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == ']') { p++; break; }
        if (*p != '{') return 1;
        const char *end = fss_json_object_end(p + 1);
        if (!end) return 1;
        fss_ctrl_instr in; memset(&in, 0, sizeof(in));
        if (!fss_json_int(p, end, "op", &in.op) ||
            !fss_json_int(p, end, "rt", &in.roundtrip) ||
            !fss_json_int(p, end, "fl", &in.flags) ||
            !fss_json_str(p, end, "a0", in.arg0, sizeof(in.arg0)) ||
            !fss_json_str(p, end, "a1", in.arg1, sizeof(in.arg1)) ||
             !fss_json_str(p, end, "a2", in.arg2, sizeof(in.arg2)) ||
             !fss_json_str(p, end, "xport", in.transport, sizeof(in.transport))) return 1;
        /* A comma needs another member.  The field scanners accept a comma as
         * a member delimiter, so reject a dangling comma here. */
        const char *last = end;
        while (last > p && isspace((unsigned char)last[-1])) last--;
        if (last == p || last[-1] == ',') return 1;
        agent_do_instr(A, &in);
        if (in.status != 0) return (int)in.status;
        p = end + 1;
        nsteps++;
        while (isspace((unsigned char)*p)) p++;
        if (*p == ',') {
            p++;
            while (isspace((unsigned char)*p)) p++;
            if (*p == ']') return 1;  /* no trailing array comma */
            continue;
        }
        if (*p == ']') { p++; break; }
        return 1;
    }
    while (isspace((unsigned char)*p)) p++;
    return nsteps > 0 && *p == '\0' ? 0 : 1;
}

/* Minimal breadcrumb reader: find "key" then copy the following "..."-quoted
 * string value into out.  NOT a general JSON parser — the breadcrumb is a flat
 * string->string map the orchestrator writes ({"transport":..,"control":..}). */
static int fss_bc_get(const char *json, const char *key, char *out, size_t n)
{
    if (!json || !out || n == 0) return 0;
    char pat[64]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':'); if (!p) return 0;
    p++; while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (*p == '"') ? 1 : 0;
}

/* Standard breadcrumb bootstrap: bring up the control transport the agent phones
 * home over, create its channel, and wire the agent's control sink.  This is the
 * in-proc dual of the launcher's --fss-parent flag; a dlopen'd model calls it
 * after reading its native config.  The breadcrumb is a flat JSON map:
 *   {"transport":"<name>","control":"<control channel name>"}
 * `transport` is a NAME (fss_transport_name(), e.g. "shm"), resolved to a plugin
 * through the SAME agent_transport() path a data edge uses (transport dir +
 * fss_transport_name() confirmation + cache), NOT a plugin path.  `control` is
 * the arbitrary channel name (an opaque token) the agent OWNS (round-trip: it
 * pulls its instructions and the orchestrator attaches as the initiator). */
/* Lifecycle (sim-control) sink: a stop push from the orchestrator on the SEPARATE
 * lifecycle channel sets the agent's stop flag.  That channel only ever carries
 * stop, so any arrival means "tear down".  Kept OFF the topology channel. */
static int lifecycle_stop_receive(void *self, fss_carrier_t *c)
{
    (void)c;
    atomic_store_explicit(&((struct fss_agent *)self)->stop, 1, memory_order_release);
    return FSS_SEND_OK;
}

/* PULL this agent's whole build+wire plan from the orchestrator (R11): send an
 * empty control::plan request on the topology channel (INITIATOR); the
 * orchestrator's responder fills PlanJson in the round-trip reply; run every
 * step (construct + attach — all channels are provisioned, so any order works). */
static int agent_pull_plan(struct fss_agent *A)
{
    uint32_t pj_off, pj_size, st_off, st_size;
    if (!fss_meta_find(FSS_CONTROL_PLAN_META, FSS_CONTROL_PLAN_META_LEN,
                       "control::PlanJson", &pj_off, &pj_size)) return 1;
    size_t need = (size_t)pj_off + pj_size;
    if (fss_meta_find(FSS_CONTROL_PLAN_META, FSS_CONTROL_PLAN_META_LEN,
                      "control::Status", &st_off, &st_size)
        && (size_t)st_off + st_size > need) need = (size_t)st_off + st_size;
    uint8_t buf[4096];
    if (need > sizeof(buf) || !A->control_ep.send) return 1;
    memset(buf, 0, need);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_CONTROL_PLAN_ID; c.data_ptr = buf; c.data_size = need;
    c.meta_ptr = (void *)FSS_CONTROL_PLAN_META; c.meta_size = FSS_CONTROL_PLAN_META_LEN;
    if (A->control_ep.send(A->control_ep.send_ctx, &c) != FSS_SEND_OK) return 1;  /* reply carries PlanJson */
    return fss_agent_run_plan(A, (const char *)buf + pj_off);
}

int fss_topology_transport_init(struct fss_agent *A, const char *breadcrumb_json)
{
    char name[32], ctl_name[64], lc_name[64];
    if (!fss_bc_get(breadcrumb_json, "transport", name, sizeof(name)) ||
        !fss_bc_get(breadcrumb_json, "control",   ctl_name, sizeof(ctl_name)) ||
        !fss_bc_get(breadcrumb_json, "lifecycle", lc_name, sizeof(lc_name))) {
        fprintf(stderr, "[FAIL] agent: bad breadcrumb: %s\n",
                breadcrumb_json ? breadcrumb_json : "(null)");
        return 1;
    }
    A->ctl_lib = agent_transport(A, name);
    if (!A->ctl_lib) { fprintf(stderr, "[FAIL] agent: control transport %s\n", name); return 1; }

    /* TOPOLOGY channel: the orchestrator OWNS it; the agent attaches as INITIATOR
     * and PULLS its plan through control_ep.send (round-trip). */
    A->ctl = fss_chan_create_retry(A->ctl_lib, "");
    if (!A->ctl) { fprintf(stderr, "[FAIL] agent: control create\n"); goto fail; }
    fss_channel_t *cch = fss_chan_open_retry(A->ctl, ctl_name, 1 /*round-trip*/, 0 /*attacher*/);
    if (!cch) { fprintf(stderr, "[FAIL] agent: control channel %s\n", ctl_name); goto fail; }
    memset(&A->control_ep, 0, sizeof(A->control_ep));
    if (A->ctl->attach(A->ctl->self, cch, &A->control_ep, FSS_ROLE_INITIATOR) != FSS_SEND_OK
        || !A->control_ep.send) {
        fprintf(stderr, "[FAIL] agent: control attach %s\n", ctl_name); goto fail; }

    /* SIM-CONTROL lifecycle channel (separate plane): attach TARGET; a stop push
     * from the orchestrator sets A->stop. */
    A->lc = fss_chan_create_retry(A->ctl_lib, "");
    if (!A->lc) { fprintf(stderr, "[FAIL] agent: lifecycle create\n"); goto fail; }
    fss_channel_t *lch = fss_chan_open_retry(A->lc, lc_name, 0 /*one-way*/, 0 /*attacher*/);
    if (!lch) { fprintf(stderr, "[FAIL] agent: lifecycle channel %s\n", lc_name); goto fail; }
    memset(&A->lc_ep, 0, sizeof(A->lc_ep));
    A->lc_ep.self = A; A->lc_ep.receive = lifecycle_stop_receive;
    if (A->lc->attach(A->lc->self, lch, &A->lc_ep, FSS_ROLE_TARGET) != FSS_SEND_OK) {
        fprintf(stderr, "[FAIL] agent: lifecycle attach %s\n", lc_name); goto fail;
    }
    return 0;

fail:
    agent_teardown(A);
    return 1;
}

/* ── the REMOTE agent (born with a home control channel) ─────────────────────
 * Loads its transports, creates a control endpoint on the shm back-channel to
 * home, executes the instructions that arrive, then arms + runs + waits + tears
 * down.  Exit code is the delivery verdict when it hosts the counting sink. */
int fss_agent_run(const fss_model_loader_t *loader,
                  const char *inproc_plugin, const char *xport_dir,
                  const char *model_dir,
                  const char *zrole, const char *zrv,
                   const char *breadcrumb, int frames, int period_us,
                    int status_fd, int release_fd, uint64_t barrier_nonce)
{
    struct fss_agent A;
    /* This includes the completion bound: the launcher must not mistake an
     * invalid invocation for an agent that is still attaching. */
    if (frames <= 0) {
        barrier_nack_and_close(status_fd, release_fd, barrier_nonce);
        return 1;
    }
    if (agent_open_transports(&A, loader, inproc_plugin, xport_dir, model_dir,
                               zrole, zrv, frames, period_us)) {
        barrier_nack_and_close(status_fd, release_fd, barrier_nonce);
        return 1;
    }

    if (fss_topology_transport_init(&A, breadcrumb)) {
        barrier_nack_and_close(status_fd, release_fd, barrier_nonce);
        return 1;
    }

    /* Pull and attach first.  The private barrier probes peer readiness before
     * it permits SIM_BOUND. */
    if (agent_pull_plan(&A)) {
        fprintf(stderr, "[FAIL] agent: pull/run plan\n");
        agent_teardown(&A);
        barrier_nack_and_close(status_fd, release_fd, barrier_nonce);
        return 1;
    }

    agent_prepare(&A);
    if (barrier_write(status_fd, barrier_nonce, FSS_BARRIER_ATTACHED) ||
        barrier_read_state(release_fd, barrier_nonce, FSS_BARRIER_PROBE)) {
        barrier_nack_and_close(status_fd, release_fd, barrier_nonce);
        agent_teardown(&A);
        return 1;
    }
    if (agent_transport_ready(&A) ||
        barrier_write(status_fd, barrier_nonce, FSS_BARRIER_TRANSPORT_READY) ||
        barrier_read_state(release_fd, barrier_nonce, FSS_BARRIER_RELEASE)) {
        barrier_nack_and_close(status_fd, release_fd, barrier_nonce);
        agent_teardown(&A);
        return 1;
    }
    if (release_fd >= 0) {
        int flags = fcntl(release_fd, F_GETFL);
        if (flags < 0 || fcntl(release_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            barrier_nack_and_close(status_fd, release_fd, barrier_nonce);
            agent_teardown(&A);
            return 1;
        }
        A.barrier_fd = release_fd;
        A.barrier_nonce = barrier_nonce;
    } else A.barrier_fd = -1;
    agent_bound(&A);
    if (barrier_write(status_fd, barrier_nonce, FSS_BARRIER_READY)) {
        barrier_nack_and_close(status_fd, A.barrier_fd, barrier_nonce);
        A.barrier_fd = -1;
        agent_teardown(&A);
        return 1;
    }
    if (status_fd >= 0) close(status_fd);
    int ok = agent_wait_delivery(&A);
    agent_stop(&A);
    agent_teardown(&A);
    if (A.barrier_fd >= 0) close(A.barrier_fd);
    if (!ok) fprintf(stderr, "[FAIL] agent: count=%d/%d\n", A.count, frames);
    return ok ? 0 : 1;
}

/* ── the IN-PROC agent (the orchestrator's own process-0 agent) ──────────────
 * No home control channel: the orchestrator drives it by direct fss_agent_ctrl
 * calls.  Same core, same arming/teardown; the orchestrator (not the agent)
 * waits for the whole federation, so the lifecycle is stepped rather than a
 * single blocking run. */
fss_agent_t *fss_agent_open(const fss_model_loader_t *loader,
                            const char *inproc_plugin, const char *xport_dir,
                            const char *model_dir,
                            const char *zrole, const char *zrv,
                            int frames, int period_us)
{
    struct fss_agent *A = (struct fss_agent *)malloc(sizeof(*A));
    if (!A) return NULL;
    if (agent_open_transports(A, loader, inproc_plugin, xport_dir, model_dir,
                              zrole, zrv, frames, period_us)) { free(A); return NULL; }
    return A;
}

int fss_agent_ctrl(fss_agent_t *A, fss_ctrl_instr *in) { return agent_do_instr(A, in); }

fss_endpoint_t *fss_agent_endpoint(fss_agent_t *A, const char *node, const char *ep)
{
    char buf[128]; snprintf(buf, sizeof(buf), "%s/%s", node, ep);
    return agent_resolve(A, buf);
}

void fss_agent_prepare(fss_agent_t *A)        { agent_prepare(A); }
int  fss_agent_transport_ready(fss_agent_t *A) { return A ? agent_transport_ready(A) : 1; }
void fss_agent_bound(fss_agent_t *A)          { agent_bound(A); }
void fss_agent_arm(fss_agent_t *A)            { agent_arm(A); }
int  fss_agent_is_counting(fss_agent_t *A)    { return A->counting; }
int  fss_agent_wait_delivery(fss_agent_t *A)  { return agent_wait_delivery(A); }

void fss_agent_stop_and_close(fss_agent_t *A)
{
    agent_stop(A);
    agent_teardown(A);
    free(A);
}
