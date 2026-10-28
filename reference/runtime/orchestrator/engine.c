/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * engine.c — the data-driven orchestrator engine (see engine.h).  NON-NORMATIVE.
 *
 * One engine, no per-topology launcher.  Placement falls out of the plan: nodes
 * joined by in-proc edges land in one process; the group holding the root node
 * is hosted in-proc by the orchestrator, the rest are exec'd agents.  Every node
 * — co-located or remote — is then driven through the SAME control protocol, so
 * `drive()` is byte-identical but for the last hop (a direct call for the in-proc
 * agent, a round-trip over shm/zenoh for a remote one).
 */
#include "fss/runtime/engine.h"
#include "fss/runtime/agent_core.h"
#include "fss/runtime/launcher.h"
#include "fss/runtime/orchestrator.h"
#include "fss/runtime/control_protocol.h"
#include "fss/runtime/model_cdylib.h"        /* the in-proc agent's model loader */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <limits.h>
#include <sys/wait.h>

/* The control-channel transport for a local fork: the orchestrator<->agent
 * bootstrap medium, named (not a path) and resolved through the standard
 * name->plugin resolver.  A networked launch would name a wire transport here. */
#define FSS_CONTROL_XPORT "shm"

/* One process group: a set of nodes joined by in-proc edges.  Exactly one group
 * (the one holding the root node) is hosted in-proc; the rest are exec'd. */
typedef struct {
    int   rep;          /* union-find canonical node index */
    int   is_p0;        /* hosts the root node -> in-proc agent, the orchestrator's process */
    int   depth;        /* max node index in the group (plan lists root-first: bigger = more downstream) */
    int   uses_zenoh;   /* an incident cross edge rides zenoh -> born with the zenoh plugin */
    int   hosts_ctrl;   /* the zenoh star centre -> rendezvous role "listen" */
    int   needs_listener; /* this zenoh group must open the rendezvous listener first */
    const char *zrole;  /* session role passed when this group's agent is born */
    fss_agent_t     *local;   /* p0: the in-proc agent */
    pid_t            pid;     /* remote: the exec'd agent */
    fss_transport_t *ctl;     /* remote: orchestrator's TOPOLOGY control channel (pull responder) */
    fss_endpoint_t   cep;     /* remote: control responder endpoint (fss_plan_responder sink) */
    fss_transport_t *lc;      /* remote: orchestrator's SIM-CONTROL lifecycle channel (push) */
    fss_endpoint_t   lcep;    /* remote: lifecycle initiator endpoint (orchestrator -> agent stop) */
    char             plan[3000];  /* R11: this group's FULL build+wire steps, comma-separated JSON */
    size_t           plen;        /* bytes used in plan[] */
    char             planbuf[3072]; /* bracketed plan, held as the pull responder's ctx (must outlive) */
    int              status_fd;     /* child -> parent private barrier */
    int              release_fd;    /* parent -> child private barrier */
    uint64_t         barrier_nonce;
    int              attached;      /* ATTACHED was received by the parent */
} group_t;

/* Private launcher barrier record.  Keep this in step with agent-core: a single
 * fixed record is smaller than PIPE_BUF and is written atomically by the child. */
#define FSS_BARRIER_MAGIC 0x46534252u
enum { FSS_BARRIER_ATTACHED = 1, FSS_BARRIER_READY = 2, FSS_BARRIER_NACK = 3,
       FSS_BARRIER_RELEASE = 4, FSS_BARRIER_STOP = 5, FSS_BARRIER_PROBE = 6,
       FSS_BARRIER_TRANSPORT_READY = 7 };
typedef struct { uint64_t nonce; uint32_t magic; uint32_t state; } fss_barrier_record;

static int barrier_pipe(int p[2])
{
    if (pipe(p) != 0) return 1;
    int a = fcntl(p[0], F_GETFD), b = fcntl(p[1], F_GETFD);
    if (a < 0 || b < 0 || fcntl(p[0], F_SETFD, a | FD_CLOEXEC) < 0 ||
        fcntl(p[1], F_SETFD, b | FD_CLOEXEC) < 0) {
        close(p[0]); close(p[1]); return 1;
    }
    return 0;
}

/* Barrier records are fixed-size and smaller than PIPE_BUF. */
static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* An environment value is useful for slow debuggers and test failures, but
 * invalid values never make a phase wait unbounded. */
static int barrier_deadline_ms(void)
{
    const char *s = getenv("FSS_BARRIER_READY_TIMEOUT_MS");
    char *end = NULL;
    long v;
    if (!s || !*s) return 30000;
    errno = 0; v = strtol(s, &end, 10);
    return errno || !end || *end || v <= 0 || v > INT_MAX ? 30000 : (int)v;
}

/* Each sequential readiness phase gets its own full configured timeout. */
static int64_t barrier_phase_deadline(void)
{
    int64_t now = monotonic_ms();
    return now < 0 ? -1 : now + barrier_deadline_ms();
}

/* Read a fixed record without a blocking read after a partial pipe read.  poll
 * wakes for input, HUP, or the monotonic deadline.  waitpid(WNOHANG) catches a
 * dead child even if another inherited pipe writer wrongly keeps the pipe open. */
static int barrier_wait_record(int fd, pid_t *pid, uint64_t nonce, uint32_t want,
                               int64_t deadline_ms)
{
    fss_barrier_record r;
    size_t got = 0;
    if (fd < 0 || !pid || *pid <= 0) return 1;
    while (got < sizeof(r)) {
        int ws;
        pid_t w;
        do { w = waitpid(*pid, &ws, WNOHANG); } while (w < 0 && errno == EINTR);
        if (w == *pid || (w < 0 && errno == ECHILD)) { *pid = -1; return 1; }
        if (w < 0) return 1;

        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline_ms) return 1;
        int64_t remain = deadline_ms - now;
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, remain > INT_MAX ? INT_MAX : (int)remain);
        if (pr < 0 && errno == EINTR) continue; /* recheck child state and deadline */
        if (pr <= 0) return 1;
        if (pfd.revents & (POLLERR | POLLNVAL)) return 1;
        if (!(pfd.revents & (POLLIN | POLLHUP))) continue;
        ssize_t n;
        do { n = read(fd, (char *)&r + got, sizeof(r) - got); } while (n < 0 && errno == EINTR);
        if (n <= 0) return 1;
        got += (size_t)n;
    }
    return r.magic != FSS_BARRIER_MAGIC || r.nonce != nonce || r.state != want;
}

static int barrier_command(int fd, uint64_t nonce, uint32_t state)
{
    fss_barrier_record r = { nonce, FSS_BARRIER_MAGIC, state };
    size_t put = 0;
    while (put < sizeof(r)) {
        ssize_t n = write(fd, (const char *)&r + put, sizeof(r) - put);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 1;
        put += (size_t)n;
    }
    return 0;
}

static int barrier_release(int fd, uint64_t nonce)
{
    return barrier_command(fd, nonce, FSS_BARRIER_RELEASE);
}

/* STOP is best effort and must never block cleanup.  A record is smaller than
 * PIPE_BUF, so one nonblocking write is all-or-nothing. */
static int barrier_stop(int fd, uint64_t nonce)
{
    fss_barrier_record r = { nonce, FSS_BARRIER_MAGIC, FSS_BARRIER_STOP };
    int flags;
    ssize_t n;
    if (fd < 0) return 1;
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return 1;
    do { n = write(fd, &r, sizeof(r)); } while (n < 0 && errno == EINTR);
    (void)fcntl(fd, F_SETFL, flags);
    return n == (ssize_t)sizeof(r) ? 0 : 1;
}

/* Reap only until deadline.  No normal or cleanup path may wait forever for a
 * launched agent. */
static int reap_child_until(pid_t pid, int *ws, int64_t deadline_ms)
{
    for (;;) {
        pid_t w;
        do { w = waitpid(pid, ws, WNOHANG); } while (w < 0 && errno == EINTR);
        if (w == pid || (w < 0 && errno == ECHILD)) return 0;
        if (w < 0 || monotonic_ms() < 0 || monotonic_ms() >= deadline_ms) return 1;
        (void)poll(NULL, 0, 10);
    }
}

static void close_barrier_fds(group_t *g)
{
    if (g->release_fd >= 0) { close(g->release_fd); g->release_fd = -1; }
    if (g->status_fd >= 0) { close(g->status_fd); g->status_fd = -1; }
}

/* Stop through the private pipe, then use bounded signal/reap fallback.  This
 * works both before and after READY and never sends on lifecycle transport. */
static void stop_and_reap_child(group_t *g)
{
    int ws;
    int64_t deadline;

    if (g->pid <= 0) return;
    (void)barrier_stop(g->release_fd, g->barrier_nonce);
    close_barrier_fds(g);
    deadline = monotonic_ms();
    if (deadline >= 0 && !reap_child_until(g->pid, &ws, deadline + 2000)) {
        g->pid = -1; return;
    }
    (void)kill(g->pid, SIGTERM);
    deadline = monotonic_ms();
    if (deadline >= 0 && !reap_child_until(g->pid, &ws, deadline + 2000)) {
        g->pid = -1; return;
    }
    (void)kill(g->pid, SIGKILL);
    deadline = monotonic_ms();
    if (deadline >= 0 && !reap_child_until(g->pid, &ws, deadline + 2000)) g->pid = -1;
}

/* Append a UTF-8 string as one JSON string.  Plan data comes from topology
 * inputs, so it must not be allowed to change the JSON document's structure.
 * JSON permits raw UTF-8, but not malformed UTF-8. */
static int json_plan_string(char *out, size_t out_size, size_t *used,
                            const char *value)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)(value ? value : "");

    if (*used + 1 >= out_size) return 0;
    out[(*used)++] = '"';

    /* All callers use fixed-size local strings or plan fields.  This bound
     * makes the scan finite even if a malformed plan field lacks a terminator. */
    for (size_t input = 0; input < 1024; ) {
        unsigned char c = p[input];
        size_t n = 1;

        if (c == '\0') {
            if (*used + 2 > out_size) return 0;
            out[(*used)++] = '"';
            out[*used] = '\0';
            return 1;
        }
        if (c >= 0x80) {
            if (c >= 0xc2 && c <= 0xdf) n = 2;
            else if (c >= 0xe0 && c <= 0xef) n = 3;
            else if (c >= 0xf0 && c <= 0xf4) n = 4;
            else return 0;
            if (n > 1024 - input) return 0;
            if ((n > 1 && (p[input + 1] & 0xc0) != 0x80) ||
                (n > 2 && (p[input + 2] & 0xc0) != 0x80) ||
                (n > 3 && (p[input + 3] & 0xc0) != 0x80) ||
                (c == 0xe0 && p[input + 1] < 0xa0) ||
                (c == 0xed && p[input + 1] >= 0xa0) ||
                (c == 0xf0 && p[input + 1] < 0x90) ||
                (c == 0xf4 && p[input + 1] >= 0x90)) return 0;
        }

        if (c == '"' || c == '\\') {
            if (*used + 2 >= out_size) return 0;
            out[(*used)++] = '\\'; out[(*used)++] = (char)c;
        } else if (c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') {
            static const char escapes[] = "bfnrt";
            unsigned int which = c == '\b' ? 0 : c == '\f' ? 1 : c == '\n' ? 2 :
                                 c == '\r' ? 3 : 4;
            if (*used + 2 >= out_size) return 0;
            out[(*used)++] = '\\'; out[(*used)++] = escapes[which];
        } else if (c < 0x20) {
            if (*used + 6 >= out_size) return 0;
            out[(*used)++] = '\\'; out[(*used)++] = 'u';
            out[(*used)++] = '0';  out[(*used)++] = '0';
            out[(*used)++] = hex[c >> 4]; out[(*used)++] = hex[c & 15];
        } else {
            if (*used + n >= out_size) return 0;
            memcpy(out + *used, p + input, n);
            *used += n;
        }
        input += n;
    }
    return 0;
}

static int json_plan_literal(char *out, size_t out_size, size_t *used,
                             const char *literal)
{
    size_t n = strlen(literal);
    if (n >= out_size - *used) return 0;
    memcpy(out + *used, literal, n);
    *used += n;
    return 1;
}

/* Append one build+wire step to a group's JSON plan (R5).  Steps are emitted as
 * comma-separated objects; the delivery wraps them in [ ... ].  Empty string
 * fields (a0/a1/a2/xport) are emitted as "" (the agent's parser defaults them
 * harmlessly). */
static int plan_add(group_t *g, uint32_t op, uint32_t rt, uint32_t flags,
                    const char *a0, const char *a1, const char *a2, const char *xport)
{
    char step[640];
    int n = snprintf(step, sizeof(step), "%s{\"op\":%u,\"rt\":%u,\"fl\":%u,\"a0\":",
                     g->plen ? "," : "", op, rt, flags);
    size_t used = n > 0 ? (size_t)n : 0;
    if (n < 0 || used >= sizeof(step)) {
        fprintf(stderr, "[FAIL] engine: plan buffer overflow\n");
        return 1;
    }
    if (!json_plan_string(step, sizeof(step), &used, a0) ||
        !json_plan_literal(step, sizeof(step), &used, ",\"a1\":") ||
        !json_plan_string(step, sizeof(step), &used, a1) ||
        !json_plan_literal(step, sizeof(step), &used, ",\"a2\":") ||
        !json_plan_string(step, sizeof(step), &used, a2) ||
        !json_plan_literal(step, sizeof(step), &used, ",\"xport\":") ||
        !json_plan_string(step, sizeof(step), &used, xport) ||
        !json_plan_literal(step, sizeof(step), &used, "}")) {
        fprintf(stderr, "[FAIL] engine: invalid or unrepresentable plan string\n");
        return 1;
    }
    if (g->plen + used >= sizeof(g->plan)) {
        fprintf(stderr, "[FAIL] engine: plan buffer overflow\n");
        return 1;
    }
    memcpy(g->plan + g->plen, step, used);
    g->plen += used;
    return 0;
}

static int node_index(const fss_plan_t *p, const char *name)
{
    if (!p || !name) return -1;
    for (int i = 0; i < p->n_nodes; i++) if (!strcmp(p->nodes[i].name, name)) return i;
    return -1;
}

static int nonempty(const char *s) { return s && s[0] != '\0'; }

/* Check only facts represented by fss_plan_t, before it can launch an agent.
 *
 * A plan carries the compiler-derived round-trip bit, but not carrier metadata.
 * Therefore this engine cannot generically derive whether a carrier is one-way
 * or round-trip from an endpoint name.  That semantic derivation occurs when
 * the compiler builds the plan; a metadata-bearing future plan schema is needed
 * for generic revalidation here. */
static int validate_plan_structure(const fss_plan_t *p)
{
    if (!p || p->frames <= 0 || p->n_nodes <= 0 || p->n_nodes > FSS_PLAN_MAX_NODES ||
        p->n_edges < 0 || p->n_edges > FSS_PLAN_MAX_EDGES) {
        fprintf(stderr, "[FAIL] engine: invalid plan size\n");
        return 1;
    }

    for (int i = 0; i < p->n_nodes; i++) {
        if (!nonempty(p->nodes[i].name) || !nonempty(p->nodes[i].model)) {
            fprintf(stderr, "[FAIL] engine: invalid node %d\n", i);
            return 1;
        }
        for (int prior = 0; prior < i; prior++) {
            if (!strcmp(p->nodes[i].name, p->nodes[prior].name)) {
                fprintf(stderr, "[FAIL] engine: duplicate node %s\n", p->nodes[i].name);
                return 1;
            }
        }
    }
    if (!nonempty(p->root_node) || node_index(p, p->root_node) < 0) {
        fprintf(stderr, "[FAIL] engine: invalid root node\n");
        return 1;
    }
    /* A controller is optional for non-zenoh plans.  If supplied, it must name a
     * node; zenoh requires one listener, so it cannot be omitted there. */
    if (p->ctrl_node && (!p->ctrl_node[0] || node_index(p, p->ctrl_node) < 0)) {
        fprintf(stderr, "[FAIL] engine: invalid control node\n");
        return 1;
    }

    for (int e = 0; e < p->n_edges; e++) {
        const fss_edge_spec *ed = &p->edges[e];

        if (!nonempty(ed->from_node) || !nonempty(ed->from_ep) ||
            !nonempty(ed->to_node) || !nonempty(ed->to_ep) ||
            node_index(p, ed->from_node) < 0 || node_index(p, ed->to_node) < 0) {
            fprintf(stderr, "[FAIL] engine: invalid edge %d endpoint\n", e);
            return 1;
        }
        if (ed->kind != FSS_XK_INPROC && ed->kind != FSS_XK_SHM &&
            ed->kind != FSS_XK_ZENOH) {
            fprintf(stderr, "[FAIL] engine: invalid edge %d transport\n", e);
            return 1;
        }
        if (ed->kind == FSS_XK_ZENOH && !nonempty(p->ctrl_node)) {
            fprintf(stderr, "[FAIL] engine: zenoh edge without control node\n");
            return 1;
        }

        if (ed->roundtrip > 1) {
            fprintf(stderr, "[FAIL] engine: edge %d round-trip is not normalized\n", e);
            return 1;
        }

        for (int prior = 0; prior < e; prior++) {
            const fss_edge_spec *old = &p->edges[prior];
            if (!strcmp(ed->from_node, old->from_node) &&
                !strcmp(ed->from_ep, old->from_ep) &&
                !strcmp(ed->to_node, old->to_node) &&
                !strcmp(ed->to_ep, old->to_ep) &&
                ed->roundtrip != old->roundtrip) {
                fprintf(stderr, "[FAIL] engine: edge %d conflicts with edge %d mode\n",
                        e, prior);
                return 1;
            }
            /* chan_ident() uses the target for round-trip and the source for
             * one-way.  Thus either kind of edge can claim the same logical
             * channel endpoint; it must not claim it with the other mode. */
            if (!strcmp(ed->roundtrip ? ed->to_node : ed->from_node,
                        old->roundtrip ? old->to_node : old->from_node) &&
                !strcmp(ed->roundtrip ? ed->to_ep : ed->from_ep,
                        old->roundtrip ? old->to_ep : old->from_ep) &&
                ed->roundtrip != old->roundtrip) {
                fprintf(stderr, "[FAIL] engine: channel use %d conflicts with edge %d mode\n",
                        e, prior);
                return 1;
            }
        }

        /* A source endpoint may fan out only over a one-way carrier.  The plan
         * has no carrier metadata, so this is the safe structural restriction:
         * do not create a shared source channel when any branch says round-trip.
         * Repeated declarations of the same directed edge are not fan-out. */
        for (int other = e + 1; other < p->n_edges; other++) {
            const fss_edge_spec *next = &p->edges[other];
            if (strcmp(ed->from_node, next->from_node) ||
                strcmp(ed->from_ep, next->from_ep)) continue;
            if (!strcmp(ed->to_node, next->to_node) &&
                !strcmp(ed->to_ep, next->to_ep)) continue;
            if (ed->roundtrip || next->roundtrip) {
                fprintf(stderr, "[FAIL] engine: round-trip fan-out from %s/%s\n",
                        ed->from_node, ed->from_ep);
                return 1;
            }
        }
    }
    return 0;
}

/* The IDENTITY of a cross edge's channel: the endpoint whose sharing defines the
 * rendezvous.  A round-trip edge keys on its TARGET (fan-in: N initiators -> 1
 * target); a one-way edge keys on its SOURCE (fan-out: 1 initiator -> N targets).
 * Edges with the same identity share ONE channel.  (This is only for uniqueness
 * comparison; the endpoint string is NOT embedded in the POSIX channel name.) */
static void chan_ident(char *out, size_t n, const fss_edge_spec *ed)
{
    if (ed->roundtrip) snprintf(out, n, "%s/%s", ed->to_node,   ed->to_ep);
    else               snprintf(out, n, "%s/%s", ed->from_node, ed->from_ep);
}

static int uf_find(int *uf, int x) { while (uf[x] != x) { uf[x] = uf[uf[x]]; x = uf[x]; } return x; }

/* Drive one control instruction into a group's agent, uniformly: a direct call
 * for the in-proc agent, a round-trip over the control channel for a remote one.
 * `xport` names the data-edge transport (SERVER/CLIENT; "" or NULL otherwise). */
static int drive(group_t *g, uint32_t op, uint32_t rt, uint32_t flags,
                 const char *a0, const char *a1, const char *a2, const char *xport)
{
    fss_ctrl_instr in; memset(&in, 0, sizeof(in));
    in.op = op; in.roundtrip = rt; in.flags = flags;
    if (a0) snprintf(in.arg0, sizeof(in.arg0), "%s", a0);
    if (a1) snprintf(in.arg1, sizeof(in.arg1), "%s", a1);
    if (a2) snprintf(in.arg2, sizeof(in.arg2), "%s", a2);
    if (xport) snprintf(in.transport, sizeof(in.transport), "%s", xport);
    if (g->is_p0) return fss_agent_ctrl(g->local, &in);
    return fss_ctrl_send(&g->cep, &in);
}

/* Run p0's local plan only after any required remote zenoh listener has proved
 * its session is open and its local channels are attached. */
static int start_p0(group_t *p0, const fss_plan_t *plan, fss_fed_status *fed)
{
    char full[3072];
    snprintf(full, sizeof(full), "[%s]", p0->plan);
    if (fss_agent_run_plan(p0->local, full) != 0) return 1;
    fss_endpoint_t *rs = fss_agent_endpoint(p0->local, plan->root_node, "fss/sim");
    if (rs) { rs->send = fss_fed_status_receive; rs->send_ctx = fed; }
    fss_agent_prepare(p0->local);
    return 0;
}

int fss_orchestrate(const fss_plan_t *plan)
{
    int rc = 1;
    int N;
    long runkey = (long)getpid();          /* per-run key for channel names */
    fss_chan_lib shm; int have_shm = 0;    /* the orchestrator's control-channel lib */
    fss_fed_status fed = { 0 };
    fss_transport_t *prov[FSS_PLAN_MAX_EDGES]; int nprov = 0;  /* R11: orchestrator-provisioned data channels */
    struct sigaction old_pipe, ignore_pipe;

    if (validate_plan_structure(plan)) return 1;
    memset(&ignore_pipe, 0, sizeof(ignore_pipe));
    ignore_pipe.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe) != 0) return 1;
    N = plan->n_nodes;

    /* ── placement: union nodes joined by an in-proc edge ──────────────────── */
    int uf[FSS_PLAN_MAX_NODES];
    for (int i = 0; i < N; i++) uf[i] = i;
    for (int e = 0; e < plan->n_edges; e++) {
        if (plan->edges[e].kind != FSS_XK_INPROC) continue;
        int a = node_index(plan, plan->edges[e].from_node);
        int b = node_index(plan, plan->edges[e].to_node);
        if (a < 0 || b < 0) { fprintf(stderr, "[FAIL] engine: edge names\n"); return 1; }
        uf[uf_find(uf, a)] = uf_find(uf, b);
    }

    group_t g[FSS_PLAN_MAX_NODES]; int ng = 0;
    int gof[FSS_PLAN_MAX_NODES];           /* node index -> group index */
    for (int i = 0; i < N; i++) {
        int r = uf_find(uf, i), gi = -1;
        for (int k = 0; k < ng; k++) if (g[k].rep == r) { gi = k; break; }
        if (gi < 0) {
            gi = ng++; memset(&g[gi], 0, sizeof(g[gi]));
            g[gi].rep = r; g[gi].depth = -1;
            g[gi].pid = -1; g[gi].status_fd = -1; g[gi].release_fd = -1;
        }
        gof[i] = gi;
        if (i > g[gi].depth) g[gi].depth = i;   /* node index doubles as depth (root-first plan) */
    }

    int root_i = node_index(plan, plan->root_node);
    if (root_i < 0) {
        fprintf(stderr, "[FAIL] engine: no root node\n");
        (void)sigaction(SIGPIPE, &old_pipe, NULL);
        return 1;
    }
    int p0 = gof[root_i];
    g[p0].is_p0 = 1;
    if (plan->ctrl_node) { int c = node_index(plan, plan->ctrl_node); g[gof[c]].hosts_ctrl = 1; }
    for (int e = 0; e < plan->n_edges; e++) {   /* who needs the zenoh plugin */
        if (plan->edges[e].kind != FSS_XK_ZENOH) continue;
        g[gof[node_index(plan, plan->edges[e].from_node)]].uses_zenoh = 1;
        g[gof[node_index(plan, plan->edges[e].to_node)]].uses_zenoh   = 1;
    }
    {
        int n_listeners = 0;
        for (int k = 0; k < ng; k++) {
            g[k].zrole = g[k].hosts_ctrl ? "listen" : "connect";
            g[k].needs_listener = g[k].uses_zenoh && g[k].hosts_ctrl;
            n_listeners += g[k].needs_listener;
        }
        /* One plan has one zenoh rendezvous endpoint.  A controller outside the
         * zenoh graph cannot open that endpoint, and more than one listener is
         * ambiguous. */
        if (n_listeners != 0 && n_listeners != 1) {
            fprintf(stderr, "[FAIL] engine: ambiguous zenoh listeners\n");
            goto done;
        }
        for (int e = 0; e < plan->n_edges; e++) {
            if (plan->edges[e].kind == FSS_XK_ZENOH && n_listeners != 1) {
                fprintf(stderr, "[FAIL] engine: zenoh edge without listener group\n");
                goto done;
            }
        }
    }

    /* ── bring up each group's agent ───────────────────────────────────────── */
    g[p0].local = fss_agent_open(&fss_model_cdylib, plan->inproc_plugin,
                                  plan->transport_dir, plan->model_dir,
                                  g[p0].zrole, plan->zrv,
                                 plan->frames, plan->period_us);
    if (!g[p0].local) { fprintf(stderr, "[FAIL] engine: in-proc agent open\n"); goto done; }

    if (ng > 1) {
        if (!plan->agent_binary) { fprintf(stderr, "[FAIL] engine: cross-process needs an agent binary\n"); goto done; }
        /* The orchestrator's control-channel transport is resolved BY NAME
         * (FSS_CONTROL_XPORT) from the transport dir, the same resolver the agents
         * and data edges use, so no plugin path crosses the control bootstrap. */
        if (fss_load_channel_named(plan->transport_dir, FSS_CONTROL_XPORT, &shm)) goto done;
        have_shm = 1;
    }
    /* ── build each group's FULL plan: constructs sink-first, then its edges ──
     * R11: channels are provisioned up front (below), so an agent's whole
     * construct+attach plan is ONE pulled reply — no ordering constraint.  A
     * cross edge is a SERVER in the receiver's plan + a CLIENT in the sender's;
     * an in-proc edge is a BIND. */
    for (int i = N - 1; i >= 0; i--)
        if (plan_add(&g[gof[i]], FSS_OP_CONSTRUCT, 0, 0,
                     plan->nodes[i].name, plan->nodes[i].model, NULL, NULL)) goto done;

    /* ── channel identity -> shared channel index (ci) ────────────────────────
     * Name each cross channel by the ENDPOINT it rendezvous on (chan_ident),
     * not by the edge, so edges sharing that endpoint share ONE channel: fan-in
     * keys on the target, fan-out on the source (see chan_ident).  We collapse
     * each distinct identity to a small first-seen index ci, then keep TODAY's
     * short POSIX name format "e<runkey>_<ci>" — the identity string itself is
     * too long for the shm/sem name cap and is used for comparison only.  For
     * the current plan every identity is distinct, so ci is a 1:1 renumbering of
     * the edges -> same channel count and wiring (matrix stays byte-identical). */
    int  ci[FSS_PLAN_MAX_EDGES];
    char idents[FSS_PLAN_MAX_EDGES][192]; int n_ident = 0;
    for (int e = 0; e < plan->n_edges; e++) {
        char id[192];
        /* FAN-OUT per transport: zenoh/inproc realise "1 source -> N targets" on
         * ONE channel (a broadcast key / a shim), so their fan-out edges SHARE a
         * channel (source ident).  shm is 1-consumer SPSC — it CANNOT share a
         * segment, so each shm fan-out target needs its OWN channel; give every
         * shm edge a per-edge ident so it never collapses (the sender then shims
         * across the N segments, agent OP_CLIENT).  For every non-fan-out plan
         * (all idents already distinct, e.g. the 576 matrix) this is a no-op. */
        if (plan->edges[e].kind == FSS_XK_SHM) snprintf(id, sizeof(id), "shm#%d", e);
        else                                   chan_ident(id, sizeof(id), &plan->edges[e]);
        int found = -1;
        for (int j = 0; j < n_ident; j++) if (!strcmp(idents[j], id)) { found = j; break; }
        if (found < 0) { found = n_ident; snprintf(idents[n_ident++], sizeof(idents[0]), "%s", id); }
        ci[e] = found;
    }

    int client_ci[FSS_PLAN_MAX_EDGES]; for (int j = 0; j < FSS_PLAN_MAX_EDGES; j++) client_ci[j] = 0;
    int server_ci[FSS_PLAN_MAX_EDGES]; for (int j = 0; j < FSS_PLAN_MAX_EDGES; j++) server_ci[j] = 0;
    for (int e = 0; e < plan->n_edges; e++) {
        const fss_edge_spec *ed = &plan->edges[e];
        int fgi = gof[node_index(plan, ed->from_node)];
        int tgi = gof[node_index(plan, ed->to_node)];
        char fromloc[96], toloc[96];
        int fromloc_n = snprintf(fromloc, sizeof(fromloc), "%s/%s", ed->from_node, ed->from_ep);
        int toloc_n = snprintf(toloc, sizeof(toloc), "%s/%s", ed->to_node, ed->to_ep);
        if (fromloc_n < 0 || fromloc_n >= (int)sizeof(fromloc) ||
            toloc_n < 0 || toloc_n >= (int)sizeof(toloc)) {
            fprintf(stderr, "[FAIL] engine: unrepresentable edge endpoint\n");
            goto done;
        }
        if (fgi == tgi) {
            if (plan_add(&g[fgi], FSS_OP_BIND, 0, ed->flags, fromloc, toloc, NULL, NULL)) goto done;
        } else {
            const char *xport = fss_xk_name(ed->kind);
            char ch[64]; snprintf(ch, sizeof(ch), "e%ld_%d", runkey, ci[e]);
            /* TARGET side attaches a server.  Round-trip FAN-IN (N initiators -> 1
             * shared target on zenoh/inproc, e.g. the pullers' config edges into
             * init/fss/config) collapses the N edges onto ONE target ci, so the
             * target declares its server ONCE — a 2nd OP_SERVER would re-declare the
             * same queryable / re-attach the same sink.  Emit it on the FIRST edge of
             * a shared round-trip ci: this is the exact MIRROR of the one-way
             * source-side OP_CLIENT dedup below (fan-out).  shm gives each round-trip
             * edge its OWN ci (per-edge shm# ident above), so this naturally emits a
             * server per segment — the responder sink attaches to each, one reader
             * thread per segment into the one reentrant sink (the fan-in spike). */
            if (ed->roundtrip && server_ci[ci[e]]) { /* target already serves this shared round-trip channel */ }
            else { if (plan_add(&g[tgi], FSS_OP_SERVER, ed->roundtrip, ed->flags & FSS_CFLAG_COUNT, toloc, ch, fromloc, xport)) goto done;
                    server_ci[ci[e]] = 1; }
            /* One-way FAN-OUT: when N targets share ONE channel (zenoh/inproc, same
             * ci), the source attaches INITIATOR to that channel ONCE — a 2nd
             * OP_CLIENT would rebind the same source to the same channel.  Emit the
             * source-side client only on the FIRST edge of a shared one-way ci; the
             * broadcast reaches all subscribers.  shm gives each fan-out edge its
             * OWN ci (above), so this naturally emits a client per shm segment (the
             * sender's agent shims across them).  Round-trip edges never dedupe here
             * (a fan-in's N INITIATORS are distinct sources, each its own client). */
            if (ed->roundtrip == 0 && client_ci[ci[e]]) { /* source already attached to this shared channel */ }
            else { if (plan_add(&g[fgi], FSS_OP_CLIENT, ed->roundtrip, ed->flags & FSS_CFLAG_OPTIONAL, fromloc, ch, toloc, xport)) goto done;
                    client_ci[ci[e]] = 1; }
        }
    }

    /* ── R11: the orchestrator PROVISIONS every cross SHM channel up front ────
     * so nodes only ATTACH — no node creates a channel, so no create-race and no
     * ordering dependency.  zenoh is a symmetric keyed medium (no create), so
     * only shm channels are provisioned; a zenoh edge's two ends just declare.
     * Channels are shared by identity (ci), so provision each DISTINCT ci once. */
    int prov_ci[FSS_PLAN_MAX_EDGES]; for (int j = 0; j < FSS_PLAN_MAX_EDGES; j++) prov_ci[j] = 0;
    for (int e = 0; e < plan->n_edges; e++) {
        const fss_edge_spec *ed = &plan->edges[e];
        if (ed->kind != FSS_XK_SHM) continue;
        if (gof[node_index(plan, ed->from_node)] == gof[node_index(plan, ed->to_node)]) continue;
        if (prov_ci[ci[e]]) continue;   /* a shared channel is provisioned once */
        prov_ci[ci[e]] = 1;
        char ch[64]; snprintf(ch, sizeof(ch), "e%ld_%d", runkey, ci[e]);
        fss_transport_t *pt = fss_chan_create_retry(&shm, "");
        if (!pt) { fprintf(stderr, "[FAIL] engine: provision create %s\n", ch); goto done; }
        if (!fss_chan_open_retry(pt, ch, ed->roundtrip, 1 /*owner: orchestrator creates the medium*/)) {
            fprintf(stderr, "[FAIL] engine: provision channel %s\n", ch); shm.destroy(pt); goto done; }
        prov[nprov++] = pt;
    }

    /* Launch in two waves.  Zenoh's listener session must exist before a
     * connector can z_open: ATTACHED proves both the session open and local
     * channel attachment, without adding a public lifecycle phase. */
    int p0_started = 0;
    for (int wave = 0; wave < 2; wave++) {
      for (int k = 0; k < ng; k++) {
        if (g[k].is_p0) continue;
        if (g[k].needs_listener != (wave == 0)) continue;
        char ctl_name[64]; snprintf(ctl_name, sizeof(ctl_name), "ctl%ld_%d", runkey, k);
        char lc_name[64];  snprintf(lc_name,  sizeof(lc_name),  "lc%ld_%d",  runkey, k);
        snprintf(g[k].planbuf, sizeof(g[k].planbuf), "[%s]", g[k].plan);   /* pull responder ctx (must outlive) */

        /* topology channel: orchestrator creates it + installs the pull responder. */
        g[k].ctl = fss_chan_create_retry(&shm, "");
        if (!g[k].ctl) { fprintf(stderr, "[FAIL] engine: control create %d\n", k); goto done; }
        fss_channel_t *cch = fss_chan_open_retry(g[k].ctl, ctl_name, 1 /*round-trip*/, 1 /*owner*/);
        if (!cch) { fprintf(stderr, "[FAIL] engine: control channel %d\n", k); goto done; }
        memset(&g[k].cep, 0, sizeof(g[k].cep));
        g[k].cep.self = g[k].planbuf; g[k].cep.receive = fss_plan_responder;
        if (g[k].ctl->attach(g[k].ctl->self, cch, &g[k].cep, FSS_ROLE_TARGET) != FSS_SEND_OK) {
            fprintf(stderr, "[FAIL] engine: control attach %d\n", k); goto done;
        }

        /* lifecycle channel: orchestrator creates it + is the initiator that
         * pushes the stop signal (one-way; sim-control plane, NOT topology). */
        g[k].lc = fss_chan_create_retry(&shm, "");
        if (!g[k].lc) { fprintf(stderr, "[FAIL] engine: lifecycle create %d\n", k); goto done; }
        fss_channel_t *lch = fss_chan_open_retry(g[k].lc, lc_name, 0 /*one-way*/, 1 /*owner*/);
        if (!lch) { fprintf(stderr, "[FAIL] engine: lifecycle channel %d\n", k); goto done; }
        memset(&g[k].lcep, 0, sizeof(g[k].lcep));
        if (g[k].lc->attach(g[k].lc->self, lch, &g[k].lcep, FSS_ROLE_INITIATOR) != FSS_SEND_OK
            || !g[k].lcep.send) {
            fprintf(stderr, "[FAIL] engine: lifecycle attach %d\n", k); goto done;
        }

        char bc[384];
        snprintf(bc, sizeof(bc),
                 "{\"transport\":\"%s\",\"control\":\"%s\",\"lifecycle\":\"%s\"}",
                 FSS_CONTROL_XPORT, ctl_name, lc_name);
        int status[2] = { -1, -1 }, release[2] = { -1, -1 };
        if (barrier_pipe(status) || barrier_pipe(release)) {
            if (status[0] >= 0) { close(status[0]); close(status[1]); }
            if (release[0] >= 0) { close(release[0]); close(release[1]); }
            fprintf(stderr, "[FAIL] engine: barrier pipe %d\n", k); goto done;
        }
        g[k].barrier_nonce = ((uint64_t)(uint32_t)getpid() << 32) ^
                             ((uint64_t)(uint32_t)k << 16) ^ (uint64_t)(uint32_t)runkey;
        fss_born_with bw = {
            .inproc_plugin = plan->inproc_plugin, .transport_dir = plan->transport_dir,
            .model_dir = plan->model_dir, .zrole = g[k].zrole,
            .zrv = plan->zrv, .breadcrumb = bc, .frames = plan->frames,
            .period_us = plan->period_us, .status_fd = status[1], .release_fd = release[0],
            .barrier_nonce = g[k].barrier_nonce
        };
        g[k].pid = fss_launch_agent(plan->agent_binary, &bw);
        close(status[1]); close(release[0]);
        g[k].status_fd = status[0]; g[k].release_fd = release[1];
        if (g[k].pid < 0) {
            close(g[k].status_fd); close(g[k].release_fd);
            g[k].status_fd = g[k].release_fd = -1;
            fprintf(stderr, "[FAIL] engine: launch group %d\n", k);
            goto done;
        }
      }

      /* Do not await listener groups again in the connector wave. */
      int64_t attached_deadline = barrier_phase_deadline();
      if (attached_deadline < 0) goto done;
      for (int k = 0; k < ng; k++) if (!g[k].is_p0 &&
          g[k].needs_listener == (wave == 0)) {
          if (barrier_wait_record(g[k].status_fd, &g[k].pid, g[k].barrier_nonce,
                                  FSS_BARRIER_ATTACHED, attached_deadline)) {
              fprintf(stderr, "[FAIL] engine: group %d did not attach\n", k); goto done;
          }
          g[k].attached = 1;
      }

      /* A local listener must run its plan before remote connectors start. */
      if (wave == 0 && g[p0].needs_listener) {
          if (start_p0(&g[p0], plan, &fed)) {
              fprintf(stderr, "[FAIL] engine: run p0 plan\n"); goto done;
          }
          p0_started = 1;
      }
    }

    /* For a connector p0, retain the old point: all remotes are attached before
     * its plan installs config responders. */
    if (!p0_started && start_p0(&g[p0], plan, &fed)) {
        fprintf(stderr, "[FAIL] engine: run p0 plan\n"); goto done;
    }

    /* Probe after every endpoint has attached and p0 has installed its config
     * responders.  Zenoh transports use this private phase to wait for their
     * actual remote peer; no fixed sleep is part of startup. */
    int64_t transport_ready_deadline = barrier_phase_deadline();
    if (transport_ready_deadline < 0) goto done;
    for (int k = 0; k < ng; k++) if (!g[k].is_p0) {
        if (barrier_command(g[k].release_fd, g[k].barrier_nonce, FSS_BARRIER_PROBE)) {
            fprintf(stderr, "[FAIL] engine: probe group %d\n", k); goto done;
        }
    }
    if (fss_agent_transport_ready(g[p0].local)) {
        fprintf(stderr, "[FAIL] engine: p0 transport not ready\n"); goto done;
    }
    for (int k = 0; k < ng; k++) if (!g[k].is_p0) {
        if (barrier_wait_record(g[k].status_fd, &g[k].pid, g[k].barrier_nonce,
                                FSS_BARRIER_TRANSPORT_READY, transport_ready_deadline)) {
            fprintf(stderr, "[FAIL] engine: group %d transport not ready\n", k); goto done;
        }
    }

    /* Release the readiness wave sink-to-source.  Each remote confirms READY
     * before the next group can be released; p0 BOUND occupies its depth turn. */
    int released[FSS_PLAN_MAX_NODES] = { 0 };
    for (int turn = 0; turn < ng; turn++) {
        int k = -1;
        for (int i = 0; i < ng; i++) if (!released[i] &&
            (k < 0 || g[i].depth > g[k].depth)) k = i;
        if (k < 0) { fprintf(stderr, "[FAIL] engine: release order\n"); goto done; }
        released[k] = 1;
        if (g[k].is_p0) { fss_agent_bound(g[k].local); continue; }
        int64_t ready_deadline = barrier_phase_deadline();
        if (ready_deadline < 0) goto done;
        if (barrier_release(g[k].release_fd, g[k].barrier_nonce)) {
            fprintf(stderr, "[FAIL] engine: release group %d\n", k); goto done;
        }
        /* Keep the write end for the post-READY private STOP command. */
        if (barrier_wait_record(g[k].status_fd, &g[k].pid, g[k].barrier_nonce,
                                FSS_BARRIER_READY, ready_deadline)) {
            fprintf(stderr, "[FAIL] engine: group %d did not become ready\n", k); goto done;
        }
        close(g[k].status_fd); g[k].status_fd = -1;
    }

    if (!fss_wait_for_flag(&fed.ready, 30000)) fprintf(stderr, "[WARN] engine: federation not ready\n");

    /* ── completion: EVERY group hosting a COUNT edge's sink is the verdict ────
     * Fan-out (1 source -> N counted targets) yields N count sinks: with N in
     * separate processes (shm/zenoh) each remote agent's EXIT CODE is its own
     * delivery verdict, and with N in ONE process (in-proc) that agent counts
     * frames*n_counted (agent_wait_delivery).  The run PASSES only when every
     * distinct count-sink group completes — so a target that silently received
     * nothing FAILS the run (no vacuous pass).  The single-device path has one
     * count edge -> one group, exactly as before. */
    { int cgi[FSS_PLAN_MAX_NODES]; int ncg = 0;
      for (int e = 0; e < plan->n_edges; e++) {
          if (!(plan->edges[e].flags & FSS_CFLAG_COUNT)) continue;
          int gi = gof[node_index(plan, plan->edges[e].to_node)];
          int seen = 0; for (int j = 0; j < ncg; j++) if (cgi[j] == gi) { seen = 1; break; }
          if (!seen) cgi[ncg++] = gi;
      }
      if (ncg == 0) cgi[ncg++] = p0;

      struct timespec ta, tb; clock_gettime(CLOCK_MONOTONIC, &ta);
      int ok = 1;
      for (int j = 0; j < ncg; j++) {
           group_t *cg = &g[cgi[j]];
           if (cg->is_p0) { if (!fss_agent_wait_delivery(cg->local)) ok = 0; }
            else { int ws = 0;
                   int64_t deadline = monotonic_ms();
                   if (deadline < 0 || reap_child_until(cg->pid, &ws, deadline + 30000) ||
                       !(WIFEXITED(ws) && WEXITSTATUS(ws) == 0)) ok = 0;
                   else cg->pid = -1; }
      }
      clock_gettime(CLOCK_MONOTONIC, &tb);

      const char *lbl = plan->label ? plan->label : "FSS";
      if (ok) {
          double ns = (double)(tb.tv_sec - ta.tv_sec) * 1e9 + (double)(tb.tv_nsec - ta.tv_nsec);
          printf("PLATFORM %s PASS: acked=%d/%d\n", lbl, plan->frames, plan->frames);
          printf("PLATFORM %s PERF: frames=%d ns_per_txn=%.1f txns_per_sec=%.0f\n",
                 lbl, plan->frames, ns / plan->frames, ns > 0 ? plan->frames * 1e9 / ns : 0);
          rc = 0;
      } else fprintf(stderr, "[FAIL] engine: delivery incomplete\n");
    }

done:
    /* Stop and reap every still-live remote group through the private barrier.
     * The public lifecycle channel remains unchanged, but cleanup must not use a
     * transport send because a broken reader can block the parent. */
    for (int k = 0; k < ng; k++) {
        if (g[k].is_p0) continue;
        if (g[k].pid > 0) {
            stop_and_reap_child(&g[k]);
        }
        close_barrier_fds(&g[k]);
    }
    if (g[p0].local) { fss_agent_stop_and_close(g[p0].local); g[p0].local = NULL; }
    /* Destroy per-group control + lifecycle channels, then the provisioned data
     * channels last: the creator's shm_unlink frees the names, agent mappings
     * persist until they exit. */
    for (int k = 0; k < ng; k++) {
        if (g[k].is_p0) continue;
        if (g[k].ctl) { shm.destroy(g[k].ctl); g[k].ctl = NULL; }
        if (g[k].lc)  { shm.destroy(g[k].lc);  g[k].lc  = NULL; }
    }
    for (int i = 0; i < nprov; i++) shm.destroy(prov[i]);
    (void)have_shm;
    (void)sigaction(SIGPIPE, &old_pipe, NULL);
    return rc;
}
