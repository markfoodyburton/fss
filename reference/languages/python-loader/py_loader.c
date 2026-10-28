/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * py_loader.c — the FSS Python-loader binding: a C cdylib that embeds CPython so
 * a Python model can be a dlopen'd FSS artefact, just like a C/Rust/SystemC one.
 *
 * This closes the one gap in the open() story: a Python interpreter is not a
 * cdylib, so a transport can't dlopen a .py directly.  Instead a transport
 * dlopens THIS loader (a real cdylib exporting fss_create / fss_get_endpoint /
 * fss_destroy); the loader embeds CPython, imports a model module, and bridges
 * its endpoints to Python.  The model module + its directory are baked in at
 * compile time (FSS_PY_MODULE / FSS_PY_DIR) — the same pattern the FMI artefact
 * uses for its FMU path — so the loader is opened with a plain "<node>=<path>"
 * locator and needs no new locator grammar.
 *
 * The loader is carrier-AGNOSTIC: it ferries opaque (id, data, meta) bytes; all
 * carrier layout lives in the Python model (an example), never here.
 *
 * The model module must expose:
 *   fss_create(name) -> object
 * and the object MAY expose:
 *   object.endpoints() -> [(name, "in"|"out"), ...]   # default [("can_rx","in")]
 *   object.receive(carrier_id:int, data:bytes, meta:bytes)  # for each "in" endpoint
 *        -> int                                         #   terminal status (0=OK), OR
 *        -> (out_name:str, id:int, data:bytes, meta:bytes)  # forward on an "out" ep
 *   object.start() -> iterable[(out_name, id, data, meta)]  # fired on sim Started
 *   object.report()                                    # optional teardown summary
 *
 * Threading: Py_Initialize acquires the GIL on the init thread; we release it
 * (PyEval_SaveThread) so a transport's reader/handler thread can re-acquire via
 * PyGILState_Ensure for each call — a foreign C thread safely entering Python.
 * The interpreter is process-global (Py_IsInitialized guards init), so several
 * Python artefacts can coexist in one process (e.g. an all-Python in-proc chain).
 */
#include "fss/artefact_abi.h"
#include "fss/endpoint.h"
#include "fss/c/meta.h"                  /* fss_meta_resolve — the walk primitive */
#include "fss/protocols/config.h"   /* generated: FSS_CONFIG_ID  */
#include "fss/protocols/sim.h"      /* generated: FSS_SIM_ID/META */
#include "fss/protocols/time_sync.h"/* generated: FSS_TIME_ID/META */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FSS_PY_MODULE
#  error "FSS_PY_MODULE must be defined (the python model module name)"
#endif
#ifndef FSS_PY_DIR
#  error "FSS_PY_DIR must be defined (directory containing the module .py)"
#endif

#define MAX_EP        4
#define EP_NAME_CAP   32
#define SIM_BOUND      0u  /* sim::EventCode::Bound      */
#define SIM_ELABORATED 1u  /* sim::EventCode::Elaborated */
#define SIM_STARTED    2u  /* sim::EventCode::Started    */
#define SIM_STOPPED    5u  /* sim::EventCode::Stopped    */

/* Time-sync (the non-C++ analogue of the SystemC sc_sync_window — native C in the
 * loader).  A Python model has no kernel, so the LOADER owns an invented clock
 * that honours the time::window protocol on fss/time, making a Python node a
 * first-class federation timing peer.  Layout: int32 Unit@0, uint64 Lower@8,
 * uint64 Upper@16 (24 bytes); ticks are picoseconds (Unit=1); quantum 1 µs
 * matches the SystemC global quantum. */
#define MAX_TIME_PEERS  4
#define MAX_SIM_DOWN    4
#define TIME_QUANTUM_PS 1000000ULL
#define TIME_UNIT_PS    1
#define TIME_OPEN_UPPER 0xFFFFFFFFFFFFFFFFULL   /* time::Upper detach sentinel */
#define TW_SIZE         24
/* time::window carrier id/meta come from the generated fss/protocols/
 * time_sync.h (FSS_TIME_ID / FSS_TIME_META), included above — the loader attaches
 * the meta on every window send so a peer can walk it. */

typedef struct py_node py_node;
typedef struct { py_node *node; int idx; } ep_ctx;

struct py_node {
    fss_endpoint_t eps[MAX_EP];
    ep_ctx         ctx[MAX_EP];
    char           names[MAX_EP][EP_NAME_CAP];
    int            is_out[MAX_EP];
    int            n_eps;
    fss_endpoint_t sim;        /* fss/sim UP-report port (Elaborated sent here) */
    /* fss/sim/<tag> — one inbound sink per DOWNSTREAM peer; n_sim_down IS this
     * node's n_downstream.  Decoupled from the (symmetric) time peers. */
    fss_endpoint_t sim_down[MAX_SIM_DOWN];
    char           sim_down_names[MAX_SIM_DOWN][EP_NAME_CAP];
    int            n_sim_down;
    fss_endpoint_t config;     /* standard fss/config control port */
    PyObject      *obj;        /* the python model instance      */
    int            has_start;
    int            has_done;   /* model exposes done() (sink completion) */
    int            has_config; /* model exposes config(json_str) */
    int            has_tick;   /* model exposes tick(now_ps) (producer/relay pacing) */
    int            stop_on_detach; /* a peer detach stops us (source/relay teardown) */
    char           name[EP_NAME_CAP];

    /* --- time-sync: a windowed invented clock (engaged only if fss/time bound) -- */
    fss_endpoint_t time_eps[MAX_TIME_PEERS];   /* one per timing peer            */
    ep_ctx         time_ctx[MAX_TIME_PEERS];
    char           time_names[MAX_TIME_PEERS][EP_NAME_CAP];
    int            n_time;
    uint64_t       peer_last_from[MAX_TIME_PEERS]; /* peer's last clock (skew)   */
    uint64_t       peer_upper[MAX_TIME_PEERS];     /* peer's latest Upper (ps)   */
    int            peer_seen[MAX_TIME_PEERS];
    uint64_t       now_ps, granted_upper_ps, max_skew_ps;
    int            ts_stop, ts_running;
    pthread_t      clock_thread;
    pthread_mutex_t ts_mu;
    pthread_cond_t  ts_cv;

    /* --- upward readiness wave (START BARRIER) ----------------------------- */
    int            wired;              /* Bound (or Started) received           */
    int            downstream_ready;  /* count of Elaborated received          */
    int            n_downstream;      /* == n_sim_down; peers we await on start */
    int            chain_classified;  /* classify done once on wired           */
    int            start_fired;       /* try_start ran (guard against re-entry) */
    int            config_pulled;     /* one-shot guard for the wired-time config pull */

    /* --- carrier-meta walk (the "get me the xyz atom" primitive) ----------- *
     * Every control interface resolves field offsets from the carrier's own
     * meta via fss_meta_resolve (NOT hard-coded offsets); the (id,qname) cache
     * makes repeats O(1).  Receivers run on several threads, so the cache is
     * guarded by meta_mu (fss_meta_resolve is not itself thread-safe). */
    fss_meta_cache_t *meta_cache;
    pthread_mutex_t   meta_mu;
};

/* Resolve (byte_offset[,byte_size]) of `qname` in carrier c's own meta.  Returns
 * 1 on success, 0 if the meta is absent (the carrier contract requires it present
 * at the artefact), foreign, or malformed.  Mirrors the SC binding's
 * fss_field_offset: id is only the cache key, never a gate. */
static int py_field(py_node *n, const fss_carrier_t *c, const char *qname,
                    uint32_t *off, uint32_t *sz)
{
    if (!c->meta_ptr || !c->meta_size) return 0;   /* meta MUST be present */
    pthread_mutex_lock(&n->meta_mu);
    int ok = fss_meta_resolve(n->meta_cache, c->id, c->meta_ptr, c->meta_size,
                              qname, off, sz);
    pthread_mutex_unlock(&n->meta_mu);
    return ok;
}

/* Time-sync forward declarations (defined below, used by the endpoint callbacks). */
static void  ts_request_stop(py_node *n);
static void *ts_clock_thread(void *arg);
static void  try_start(py_node *n);   /* START BARRIER: fire clock + model start when gate opens */

/* Find an outbound endpoint by name, or NULL. */
static fss_endpoint_t *find_out(py_node *n, const char *name)
{
    for (int i = 0; i < n->n_eps; ++i)
        if (n->is_out[i] && strcmp(n->names[i], name) == 0)
            return &n->eps[i];
    return NULL;
}

/* Send one (out_name, id, data, meta) spec on the named outbound endpoint.
 * Called with the GIL held; returns the transport status. */
static int send_spec(py_node *n, PyObject *spec)
{
    if (!PyTuple_Check(spec) || PyTuple_Size(spec) != 4) {
        PyErr_SetString(PyExc_TypeError, "send spec must be (out_name, id, data, meta)");
        return FSS_SEND_NO_RECEIVER;
    }
    const char *out_name = PyUnicode_AsUTF8(PyTuple_GetItem(spec, 0));
    unsigned long long cid = PyLong_AsUnsignedLongLong(PyTuple_GetItem(spec, 1));
    Py_buffer data, meta;
    if (!out_name ||
        PyObject_GetBuffer(PyTuple_GetItem(spec, 2), &data, PyBUF_SIMPLE) != 0)
        return FSS_SEND_NO_RECEIVER;
    int have_meta = (PyObject_GetBuffer(PyTuple_GetItem(spec, 3), &meta, PyBUF_SIMPLE) == 0);

    fss_endpoint_t *ep = find_out(n, out_name);
    int rc = FSS_SEND_NO_RECEIVER;
    if (ep && ep->send) {
        fss_carrier_t c;
        memset(&c, 0, sizeof(c));
        c.id        = (uint64_t)cid;
        c.data_ptr  = data.buf;
        c.data_size = (uint64_t)data.len;
        c.meta_ptr  = have_meta ? meta.buf : NULL;
        c.meta_size = have_meta ? (uint64_t)meta.len : 0;
        /* RELEASE the GIL across the outbound send: it is a foreign, possibly
         * blocking call that may RE-ENTER Python on another thread (e.g. a
         * downstream Python node reached through a kernel-bearing controller's
         * cross-thread dispatch).  Holding the GIL here would deadlock that node's
         * PyGILState_Ensure against ours.  The buffers stay pinned (we hold the
         * Py_buffer refs), so data_ptr/meta_ptr remain valid without the GIL. */
        Py_BEGIN_ALLOW_THREADS
        rc = ep->send(ep->send_ctx, &c);
        Py_END_ALLOW_THREADS
    }
    PyBuffer_Release(&data);
    if (have_meta) PyBuffer_Release(&meta);
    return rc;
}

/* Inbound: hand (carrier_id, flat data, meta) to the python model's receive().
 * The model returns either an int status (terminal device) or a send spec to
 * forward on an outbound endpoint (controller).  Runs on a possibly-foreign
 * transport thread — reacquire the GIL.
 *
 * The carrier's own CBOR meta travels with it (a transport strips/restores it on
 * the wire), so the model's receive is the endpoint SHIM: it meta-resolves the
 * carrier's fields by qname (fss.meta_find) and hands its own logic a native
 * value — the id-agnostic reuse path that lets ANY framework's carrier be read
 * with no bridge.  The loader stays carrier-agnostic (it ferries the bytes). */
static int py_receive(void *self, fss_carrier_t *c)
{
    ep_ctx  *e = (ep_ctx *)self;
    py_node *n = e->node;
    if (!n->obj || !c || !c->data_ptr) return FSS_SEND_NO_RECEIVER;

    PyGILState_STATE gil = PyGILState_Ensure();
    int rc = FSS_SEND_NO_RECEIVER;
    /* Hand the model a WRITABLE memoryview over the carrier buffer (not a bytes
     * copy): in-proc this IS the producer's flat gp, so a round-trip target can
     * write a response back — e.g. bus::Status=Ok for a poll/ack (the analogue of
     * the Rust controller's c.data_mut()).  Read-only models are unaffected
     * (struct.unpack_from / len / indexing all accept a memoryview).  The view is
     * valid only for this call; the model must not retain it. */
    PyObject *mv = PyMemoryView_FromMemory((char *)c->data_ptr,
                                           (Py_ssize_t)c->data_size, PyBUF_WRITE);
    /* The carrier's meta as a READ-ONLY view so the model can meta-resolve fields
     * by qname; empty bytes if absent (the contract requires it present, but never
     * trust the sender). */
    PyObject *meta = (c->meta_ptr && c->meta_size)
        ? PyMemoryView_FromMemory((char *)c->meta_ptr, (Py_ssize_t)c->meta_size, PyBUF_READ)
        : PyBytes_FromStringAndSize(NULL, 0);
    PyObject *res = (mv && meta) ? PyObject_CallMethod(n->obj, "receive", "KOO",
                                                       (unsigned long long)c->id, mv, meta)
                                 : NULL;
    Py_XDECREF(mv);
    Py_XDECREF(meta);
    if (res) {
        if (PyLong_Check(res))      rc = (int)PyLong_AsLong(res);
        else if (res == Py_None)    rc = FSS_SEND_OK;
        else                        rc = send_spec(n, res);   /* forward */
        Py_DECREF(res);
    } else {
        PyErr_Print();
    }
    /* Sink-driven teardown: if the model is time-synced (clock running) and now
     * reports done() (e.g. it received everything), stop the clock — it sends an
     * open_window (detach) to its peer, cascading the federation stop. */
    int stop_now = 0;
    if (n->ts_running && n->has_done) {
        PyObject *d = PyObject_CallMethod(n->obj, "done", NULL);
        if (d) { stop_now = PyObject_IsTrue(d); Py_DECREF(d); }
        else PyErr_Clear();
    }
    PyGILState_Release(gil);
    if (stop_now) ts_request_stop(n);
    return rc;
}

/* fss/config: JSON form pushed before SIM_STARTED (host thread, pre-clock — no
 * race with tick()).  Carrier layout (param_request.jsonnet): OpCode u32@0,
 * ParamName(char*)@8, ParamValue 40B@16, Status u32@56.  The host puts a JSON
 * C-string in ParamValue ("frames"/"period_us"); hand it to the model's optional
 * config(json_str) hook, which parses the keys it understands.  Runs on the host
 * thread — acquire the GIL to call into Python. */
static int config_receive(void *self, fss_carrier_t *c)
{
    py_node *n = (py_node *)self;
    if (!c || !c->data_ptr) return FSS_SEND_NO_RECEIVER;
    uint32_t pv_off, st_off, st_sz;
    if (!py_field(n, c, "config::ParamValue", &pv_off, NULL)) return FSS_SEND_NO_RECEIVER;
    if (n->obj && n->has_config) {
        const char *json = (const char *)c->data_ptr + pv_off;   /* ParamValue (flat JSON) */
        PyGILState_STATE gil = PyGILState_Ensure();
        PyObject *res = PyObject_CallMethod(n->obj, "config", "s", json);
        if (res) Py_DECREF(res); else PyErr_Print();
        PyGILState_Release(gil);
    }
    if (py_field(n, c, "config::Status", &st_off, &st_sz) && st_sz >= 4) {
        int32_t st = 0; memcpy((char *)c->data_ptr + st_off, &st, 4);   /* Status = Ok */
    }
    return FSS_SEND_OK;
}

/* PULL config at wired time: the orchestrator installed our fss/config port's send
 * at bind (its single responder is the fan-in sink).  Send a config::param_request
 * {Get}; the round-trip reply lands in-place (synchronous for inproc; the transport
 * blocks for shm/zenoh), then apply it via config_receive (the model's config() hook)
 * — the path the old push used, inverted to pull.  One-shot; no-op if nothing bound
 * our send (a node nobody configures runs on model defaults).  Mirrors the SC
 * artefact's pull_config. */
static void pull_config(py_node *n)
{
    if (n->config_pulled) return;
    n->config_pulled = 1;
    fss_endpoint_t *cfg = &n->config;
    if (!cfg->send) return;
    /* RETRY across an async transport's route setup: immediate for inproc/shm (first
     * pass, no sleep), loops only while a freshly bound zenoh querier waits for its
     * peer queryable.  Bounded; falls back to model defaults if never reachable. */
    for (int attempt = 0; attempt < 300; ++attempt) {
        uint8_t buf[60]; memset(buf, 0, sizeof(buf));   /* OpCode u32@0 = Get(0); ParamValue@16; Status@56 */
        fss_carrier_t c; memset(&c, 0, sizeof(c));
        c.id = FSS_CONFIG_ID; c.data_ptr = buf; c.data_size = sizeof(buf);
        c.meta_ptr = (void *)FSS_CONFIG_META; c.meta_size = FSS_CONFIG_META_LEN;
        uint32_t pv_off;
        if (cfg->send(cfg->send_ctx, &c) == FSS_SEND_OK &&
            py_field(n, &c, "config::ParamValue", &pv_off, NULL) && buf[pv_off] == '{') {
            config_receive(n, &c);
            return;
        }
        struct timespec ts = { 0, 10 * 1000000L }; nanosleep(&ts, NULL);
    }
}

/* fss/sim: upward readiness wave dispatcher.
 *   Bound|Started → mark wired + classify chain + try_start;
 *   Elaborated    → count downstream peer running + try_start;
 *   Stopped       → existing stop behaviour. */
static int sim_receive(void *self, fss_carrier_t *c)
{
    py_node *n = (py_node *)self;
    if (!n->obj || !c || !c->data_ptr) return FSS_SEND_OK;
    uint32_t ec_off, ec_sz, code;
    if (!py_field(n, c, "sim::EventCode", &ec_off, &ec_sz) || ec_sz < 4 ||
        c->data_size < (uint64_t)ec_off + 4)
        return FSS_SEND_OK;
    memcpy(&code, (const char *)c->data_ptr + ec_off, 4);

    if (code == SIM_BOUND || code == SIM_STARTED) {
        /* Fix n_downstream once: the fss/sim/<tag> sinks (allocated by the
         * orchestrator's get_endpoint, all done before Bound) ARE the downstream
         * peers whose Elaborated the run gate awaits.  Time peers are symmetric —
         * no head to detect, no peer to classify. */
        if (!n->chain_classified) {
            n->n_downstream     = n->n_sim_down;
            n->chain_classified = 1;
        }
        n->wired = 1;
        pull_config(n);   /* in-band config BEFORE the clock arms */
        try_start(n);
    } else if (code == SIM_ELABORATED) {
        ++n->downstream_ready;
        try_start(n);
    } else if (code == SIM_STOPPED) {
        ts_request_stop(n);
    }
    return FSS_SEND_OK;
}

/* Add the model's directory (and any generated-constants dir) to sys.path
 * (GIL held). */
static void add_sys_path(void)
{
    PyObject *sys  = PyImport_ImportModule("sys");
    PyObject *path = sys ? PyObject_GetAttrString(sys, "path") : NULL;
    if (path) {
        const char *dirs[] = {
            FSS_PY_DIR,
#ifdef FSS_PY_GEN_DIR
            FSS_PY_GEN_DIR,   /* generated flat-carrier constants (tlm_flat.py, …) */
#endif
#ifdef FSS_PY_LIB_DIR
            FSS_PY_LIB_DIR,   /* shared Python binding (fss.py: meta_find, …)      */
#endif
        };
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
            PyObject *d = PyUnicode_FromString(dirs[i]);
            PyList_Append(path, d);
            Py_DECREF(d);
        }
    }
    Py_XDECREF(path);
    Py_XDECREF(sys);
}

/* Read the model's endpoint declaration into the node (default can_rx/in). */
static void load_endpoints(py_node *n)
{
    n->n_eps = 0;
    PyObject *eps = PyObject_HasAttrString(n->obj, "endpoints")
                        ? PyObject_CallMethod(n->obj, "endpoints", NULL) : NULL;
    if (eps && PySequence_Check(eps)) {
        Py_ssize_t k = PySequence_Size(eps);
        for (Py_ssize_t i = 0; i < k && n->n_eps < MAX_EP; ++i) {
            PyObject *pair = PySequence_GetItem(eps, i);
            const char *nm = NULL, *dir = NULL;
            if (pair && PyTuple_Check(pair) && PyTuple_Size(pair) == 2) {
                nm  = PyUnicode_AsUTF8(PyTuple_GetItem(pair, 0));
                dir = PyUnicode_AsUTF8(PyTuple_GetItem(pair, 1));
            }
            if (nm && dir) {
                int idx = n->n_eps++;
                strncpy(n->names[idx], nm, EP_NAME_CAP - 1);
                n->is_out[idx] = (strcmp(dir, "out") == 0);
            }
            Py_XDECREF(pair);
        }
    } else {
        /* Default: a single inbound "can_rx" (the original device contract). */
        strncpy(n->names[0], "can_rx", EP_NAME_CAP - 1);
        n->is_out[0] = 0;
        n->n_eps = 1;
    }
    Py_XDECREF(eps);
    if (PyErr_Occurred()) PyErr_Clear();
}

/* ======================================================================== *
 *  Time-sync: a windowed invented clock (native C; mirrors the SC binding).  *
 * ======================================================================== */

/* Convert a (unit, value) tick count to picoseconds.  fs = value * 1000^unit
 * (window.jsonnet); ps = fs/1000.  SC sends unit=1 (PS) → identity. */
static uint64_t ts_to_ps(int32_t unit, uint64_t value)
{
    if (unit >= 1) {
        unsigned __int128 v = value;
        for (int i = 0; i < unit - 1; ++i) v *= 1000u;
        return (uint64_t)(v > (unsigned __int128)UINT64_MAX ? UINT64_MAX : v);
    }
    return value / 1000u;   /* unit 0 = femtoseconds */
}

/* granted_upper = min over ALL configured peers (SC "anding": every peer must
 * grant).  A peer caps the clock by: seen → its Upper; detached (Upper=MAX) →
 * drops out of the min; UNSEEN (not yet joined) → the bootstrap ceiling (one
 * quantum of absolute time) — we take the single step needed to emit our first
 * window but MUST NOT advance past it until the peer joins, so the federation
 * never races ahead of (laps) a late wall-clock joiner (e.g. a cross-process
 * initiator started after a connect/settle delay; without this the rest of the
 * federation runs to teardown before the joiner delivers a frame).  Under ts_mu. */
static void ts_recompute_granted(py_node *n)
{
    if (n->n_time <= 0) { n->granted_upper_ps = n->now_ps + TIME_QUANTUM_PS; return; }
    uint64_t lo = UINT64_MAX;
    for (int i = 0; i < n->n_time; ++i) {
        uint64_t cap;
        if (n->peer_seen[i])                       cap = n->peer_upper[i];       /* real, or MAX once detached */
        else if (n->peer_upper[i] == UINT64_MAX)   cap = UINT64_MAX;             /* detached unseen → drops out */
        else                                       cap = TIME_QUANTUM_PS;        /* unseen & live → hold */
        if (cap < lo) lo = cap;
    }
    n->granted_upper_ps = lo;
}

/* Inbound on an fss/time port: fold peer idx's window into the conjunction. */
static int time_receive(void *self, fss_carrier_t *c)
{
    ep_ctx  *e = (ep_ctx *)self;
    py_node *n = e->node;
    int idx = e->idx;
    if (!c || !c->data_ptr) return FSS_SEND_NO_RECEIVER;
    /* Walk the carrier's own meta for the window fields (no id gate, no hard-coded
     * offsets); a foreign/meta-less window simply fails to resolve and is dropped. */
    uint32_t uo, lo_off, up_off, usz, lsz, upsz;
    if (!py_field(n, c, "time::Unit",  &uo,     &usz)  || usz  < 4 ||
        !py_field(n, c, "time::Lower", &lo_off, &lsz)  || lsz  < 8 ||
        !py_field(n, c, "time::Upper", &up_off, &upsz) || upsz < 8 ||
        c->data_size < (uint64_t)up_off + 8)
        return FSS_SEND_NO_RECEIVER;
    const uint8_t *d = (const uint8_t *)c->data_ptr;
    int32_t  unit;  memcpy(&unit,  d + uo,     4);
    uint64_t lower; memcpy(&lower, d + lo_off, 8);
    uint64_t upper; memcpy(&upper, d + up_off, 8);

    pthread_mutex_lock(&n->ts_mu);
    if (upper != TIME_OPEN_UPPER) {
        n->peer_seen[idx]      = 1;
        n->peer_last_from[idx] = ts_to_ps(unit, lower);
        n->peer_upper[idx]     = ts_to_ps(unit, upper);
    } else {
        n->peer_upper[idx] = UINT64_MAX;   /* detached: drops out of the min */
        if (n->stop_on_detach) n->ts_stop = 1; /* source/relay teardown cascade */
    }
    ts_recompute_granted(n);
    pthread_cond_broadcast(&n->ts_cv);
    pthread_mutex_unlock(&n->ts_mu);
    return FSS_SEND_OK;
}

/* Send a time::window {lower_ps, upper} to peer idx (upper==MAX → open). */
static int ts_send(py_node *n, int idx, uint64_t lower_ps, uint64_t upper)
{
    fss_endpoint_t *ep = &n->time_eps[idx];
    if (!ep->send) return FSS_SEND_OK;
    uint8_t buf[TW_SIZE];
    int32_t unit = TIME_UNIT_PS; uint32_t pad = 0;
    memcpy(buf + 0,  &unit,     4);
    memcpy(buf + 4,  &pad,      4);
    memcpy(buf + 8,  &lower_ps, 8);
    memcpy(buf + 16, &upper,    8);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_TIME_ID; c.data_ptr = buf; c.data_size = TW_SIZE;
    c.meta_ptr = (void *)FSS_TIME_META; c.meta_size = FSS_TIME_META_LEN;
    return ep->send(ep->send_ctx, &c);
}

/* Announce Elaborated UP the chain on our dedicated fss/sim port, whose send the
 * orchestrator bound to our upstream (a peer's fss/sim/<tag> sink, or the
 * orchestrator if we are the source).  Retry until delivered (readiness handshake;
 * NOT a data window), mirroring the SC send_sim_up helper. */
static void ts_send_elaborated_up(py_node *n)
{
    fss_endpoint_t *up = &n->sim;
    if (!up->send) return;
    uint32_t code = SIM_ELABORATED;
    uint8_t  buf[4]; memcpy(buf, &code, 4);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_SIM_ID; c.data_ptr = buf; c.data_size = 4;
    c.meta_ptr = (void *)FSS_SIM_META; c.meta_size = FSS_SIM_META_LEN;
    while (up->send(up->send_ctx, &c) != FSS_SEND_OK) {
        pthread_mutex_lock(&n->ts_mu);
        int stop = n->ts_stop;
        pthread_mutex_unlock(&n->ts_mu);
        if (stop) return;
        struct timespec ts = { 0, 2 * 1000000L }; nanosleep(&ts, NULL);
    }
}

/* Per-quantum model work (a producer's pacing, a relay's drain): call the model's
 * tick(now_ps) under the GIL and send each (out_name,id,data,meta) spec it yields.
 * The analogue of an SC SC_THREAD's wait()-paced body / the Rust on_tick hook. */
static void ts_model_tick(py_node *n, uint64_t now_ps)
{
    if (!n->has_tick || !n->obj) return;
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *seq = PyObject_CallMethod(n->obj, "tick", "K", (unsigned long long)now_ps);
    if (seq) {
        PyObject *it = PyObject_GetIter(seq);
        if (it) {
            PyObject *spec;
            while ((spec = PyIter_Next(it)) != NULL) {
                send_spec(n, spec);   /* releases the GIL across each outbound send */
                Py_DECREF(spec);
            }
            Py_DECREF(it);
        }
        Py_DECREF(seq);
    }
    if (PyErr_Occurred()) PyErr_Print();
    PyGILState_Release(gil);
}

/* The clock thread: advance now toward granted_upper in quantum steps, sending
 * our window after each; block at the bound; on stop send open_window + report. */
static void *ts_clock_thread(void *arg)
{
    py_node *n = (py_node *)arg;
    const uint64_t q = TIME_QUANTUM_PS;
    /* Readiness wave: announce Elaborated UP before advancing time so the upstream
     * peer is guaranteed to be running before it receives any window (pure fire-
     * and-forget thereafter — no first-window retry needed). */
    ts_send_elaborated_up(n);
    for (;;) {
        uint64_t now;
        pthread_mutex_lock(&n->ts_mu);
        /* Channels confirmed live: a plain wait suffices — a peer's window WILL
         * arrive and broadcast ts_cv. */
        while (!n->ts_stop && n->granted_upper_ps <= n->now_ps)
            pthread_cond_wait(&n->ts_cv, &n->ts_mu);
        if (n->ts_stop) { pthread_mutex_unlock(&n->ts_mu); break; }
        now = n->now_ps + q;
        if (now > n->granted_upper_ps) now = n->granted_upper_ps;
        n->now_ps = now;
        for (int i = 0; i < n->n_time; ++i) {
            if (n->peer_seen[i] && n->peer_upper[i] != UINT64_MAX) {
                uint64_t a = now, b = n->peer_last_from[i];
                uint64_t gap = a > b ? a - b : b - a;
                if (gap > n->max_skew_ps) n->max_skew_ps = gap;
            }
        }
        pthread_mutex_unlock(&n->ts_mu);
        ts_model_tick(n, now);   /* producer/relay acts for this quantum */
        for (int i = 0; i < n->n_time; ++i) ts_send(n, i, now, now + q);
    }
    /* Stopped: release every peer (open_window) + report skew. */
    uint64_t now, skew; int any = 0;
    pthread_mutex_lock(&n->ts_mu);
    now = n->now_ps; skew = n->max_skew_ps;
    for (int i = 0; i < n->n_time; ++i) if (n->peer_seen[i]) any = 1;
    pthread_mutex_unlock(&n->ts_mu);
    for (int i = 0; i < n->n_time; ++i) ts_send(n, i, now, TIME_OPEN_UPPER);
    if (any) {
        /* Diagnostic, NOT a verdict: the bounded multi-quantum reading is real
         * processing-latency skew (a pacing wait lands a few quanta past the peer's
         * last checkpoint), not a lockstep violation, so there is no fixed bound to
         * PASS/FAIL against.  See fss-skew-bounded-transient. */
        double gap_ns = (double)skew / 1000.0;
        printf("FSS TIMESYNC %s: max_clock_gap=%g ns\n", n->name, gap_ns);
        fflush(stdout);
    }
    return NULL;
}

/* Signal the clock to stop (e.g. the sink received everything, or destroy). */
static void ts_request_stop(py_node *n)
{
    pthread_mutex_lock(&n->ts_mu);
    n->ts_stop = 1;
    pthread_cond_broadcast(&n->ts_cv);
    pthread_mutex_unlock(&n->ts_mu);
}

/* START BARRIER: start the clock (and run the model's start() once) only when
 * this node is wired AND all its downstream time-peers have reported Elaborated.
 * The sink (n_downstream==0) fires as soon as wired.  Called from sim_receive. */
static void try_start(py_node *n)
{
    if (!n->wired || n->downstream_ready < n->n_downstream) return;
    if (n->start_fired || n->ts_stop) return;
    n->start_fired = 1;

    /* Spawn the clock thread (pure C, no GIL): it will announce Elaborated UP
     * then run the steady-state window loop. */
    if (n->n_time > 0) {
        n->ts_running = 1;
        pthread_create(&n->clock_thread, NULL, ts_clock_thread, n);
    }

    /* Fire the model's start() produce block (same as the old SIM_STARTED path,
     * now deferred until the run gate opens).  n_time==0 nodes have no clock but
     * still need to produce on start.  Acquire the GIL — Python C-API. */
    if (!n->has_start) return;

    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *seq = PyObject_CallMethod(n->obj, "start", NULL);
    if (seq) {
        PyObject *it = PyObject_GetIter(seq);
        if (it) {
            PyObject *spec;
            while ((spec = PyIter_Next(it)) != NULL) {
                send_spec(n, spec);   /* releases GIL across each outbound send */
                Py_DECREF(spec);
            }
            Py_DECREF(it);
        }
        Py_DECREF(seq);
    }
    if (PyErr_Occurred()) PyErr_Print();
    PyGILState_Release(gil);
}

FSS_EXPORT fss_node_t *fss_create(fss_transport_t *transport, const char *name)
{
    (void)transport;
    py_node *n = (py_node *)calloc(1, sizeof(py_node));
    if (!n) return NULL;
    strncpy(n->name, (name && *name) ? name : "py", EP_NAME_CAP - 1);
    n->granted_upper_ps = TIME_QUANTUM_PS;   /* bootstrap: free to run to 1 quantum */
    pthread_mutex_init(&n->ts_mu, NULL);
    pthread_cond_init(&n->ts_cv, NULL);
    pthread_mutex_init(&n->meta_mu, NULL);
    n->meta_cache = fss_meta_cache_create();

    /* The interpreter is process-global: the first loader initialises it and
     * releases the GIL; later loaders just add their module dir under the GIL. */
    if (!Py_IsInitialized()) {
        Py_Initialize();          /* acquires the GIL on this thread */
        add_sys_path();
        PyEval_SaveThread();      /* release the GIL for other threads */
    } else {
        PyGILState_STATE g = PyGILState_Ensure();
        add_sys_path();
        PyGILState_Release(g);
    }

    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *mod = PyImport_ImportModule(FSS_PY_MODULE);
    PyObject *obj = mod ? PyObject_CallMethod(mod, "fss_create", "s", name ? name : "") : NULL;
    Py_XDECREF(mod);
    if (!obj) {
        PyErr_Print();
        PyGILState_Release(gil);
        free(n);
        return NULL;
    }
    n->obj       = obj;
    n->has_start = PyObject_HasAttrString(obj, "start");
    n->has_done  = PyObject_HasAttrString(obj, "done");
    n->has_config = PyObject_HasAttrString(obj, "config");
    /* A model with tick() is a producer/relay: it acts each quantum and tears down
     * when a timing peer detaches (a sink instead has done() and stops itself). */
    n->has_tick  = PyObject_HasAttrString(obj, "tick");
    n->stop_on_detach = n->has_tick;
    load_endpoints(n);
    PyGILState_Release(gil);

    /* Wire the C endpoints: inbound endpoints dispatch to py_receive; outbound
     * endpoints carry no receive (the transport installs send at bind). */
    for (int i = 0; i < n->n_eps; ++i) {
        n->ctx[i].node = n;
        n->ctx[i].idx  = i;
        n->eps[i].self    = &n->ctx[i];
        n->eps[i].receive = n->is_out[i] ? NULL : py_receive;
    }
    n->sim.self    = n;
    n->sim.receive = sim_receive;
    n->config.self    = n;
    n->config.receive = config_receive;
    return (fss_node_t *)n;
}

FSS_EXPORT fss_endpoint_t *fss_get_endpoint(fss_node_t *node, const char *endpoint_name)
{
    py_node *n = (py_node *)node;
    if (strcmp(endpoint_name, "fss/sim") == 0) return &n->sim;
    /* "fss/sim/<tag>" — one inbound sink per DOWNSTREAM peer; its Elaborated feeds
     * the run gate.  Allocated on first request; n_sim_down IS n_downstream. */
    if (strncmp(endpoint_name, "fss/sim/", 8) == 0) {
        for (int i = 0; i < n->n_sim_down; ++i)
            if (strcmp(n->sim_down_names[i], endpoint_name) == 0) return &n->sim_down[i];
        if (n->n_sim_down >= MAX_SIM_DOWN) return NULL;
        int i = n->n_sim_down++;
        strncpy(n->sim_down_names[i], endpoint_name, EP_NAME_CAP - 1);
        n->sim_down[i].self    = n;             /* sim_receive recovers py_node from self */
        n->sim_down[i].receive = sim_receive;   /* inbound only — never sends */
        return &n->sim_down[i];
    }
    if (strcmp(endpoint_name, "fss/config") == 0) return &n->config;
    /* Any "fss/time"-prefixed name is a timing-peer port (the loader's invented
     * clock); "fss/time" = peer 0, "fss/time/<tag>" = further peers.  Allocating
     * one arms the clock thread (started on fss/sim Started). */
    if (strncmp(endpoint_name, "fss/time", 8) == 0) {
        for (int i = 0; i < n->n_time; ++i)
            if (strcmp(n->time_names[i], endpoint_name) == 0) return &n->time_eps[i];
        if (n->n_time >= MAX_TIME_PEERS) return NULL;
        int i = n->n_time++;
        strncpy(n->time_names[i], endpoint_name, EP_NAME_CAP - 1);
        n->time_ctx[i].node    = n;
        n->time_ctx[i].idx     = i;
        n->time_eps[i].self    = &n->time_ctx[i];
        n->time_eps[i].receive = time_receive;   /* send installed by the transport */
        return &n->time_eps[i];
    }
    for (int i = 0; i < n->n_eps; ++i)
        if (strcmp(n->names[i], endpoint_name) == 0) return &n->eps[i];
    return NULL;
}

FSS_EXPORT void fss_destroy(fss_node_t *node)
{
    py_node *n = (py_node *)node;
    /* Stop + join the clock thread first (it touches no Python, so no GIL). */
    if (n->ts_running) {
        ts_request_stop(n);
        pthread_join(n->clock_thread, NULL);
        n->ts_running = 0;
    }
    pthread_mutex_destroy(&n->ts_mu);
    pthread_cond_destroy(&n->ts_cv);
    fss_meta_cache_destroy(n->meta_cache);
    pthread_mutex_destroy(&n->meta_mu);
    if (n->obj) {
        PyGILState_STATE gil = PyGILState_Ensure();
        if (PyObject_HasAttrString(n->obj, "report")) {
            PyObject *res = PyObject_CallMethod(n->obj, "report", NULL);
            if (res) Py_DECREF(res); else PyErr_Clear();
        }
        Py_DECREF(n->obj);
        PyGILState_Release(gil);
    }
    free(n);
    /* Intentionally NOT Py_Finalize: other instances may share the interpreter,
     * and finalize/fork interplay is fragile.  Process exit reclaims it. */
}
