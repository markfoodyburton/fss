/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * orchestrator.c — reference implementation of the ring-3 orchestrator
 * scaffolding + shared transport plumbing (see orchestrator.h).  NON-NORMATIVE.
 */
#include "fss/runtime/orchestrator.h"
#include "fss/runtime/control_protocol.h"
#include "fss/protocols/sim.h"        /* FSS_SIM_ID/META */
#include "fss/protocols/control.h" /* FSS_CONTROL_ID/META */
#include "fss/protocols/control_plan.h" /* FSS_CONTROL_PLAN_ID/META (R5) */
#include "fss/c/meta.h"                     /* fss_meta_find (config reply offsets) */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>   /* access, F_OK (fss_model_plugin_path) */

void fss_msleep(long ms) { struct timespec ts = { ms/1000, (ms%1000)*1000000L }; nanosleep(&ts, NULL); }

/* ── transport plugin loading ─────────────────────────────────────────────── */
/* Do this before resolving or calling a factory.  Pre-negotiation plugins may
 * expose the old open/bind object ABI under the same factory names; creating one
 * and reading it as fss_transport_t would be unsafe. */
static int fss_check_transport_abi(void *dl, const char *plugin)
{
    fss_transport_abi_version_fn_t version =
        (fss_transport_abi_version_fn_t)dlsym(dl, "fss_transport_abi_version");
    fss_transport_capabilities_fn_t capabilities =
        (fss_transport_capabilities_fn_t)dlsym(dl, "fss_transport_capabilities");
    if (!version || !capabilities) {
        fprintf(stderr, "[FAIL] transport %s: missing ABI query symbols\n", plugin);
        return 1;
    }

    uint32_t got_version = version();
    uint64_t got_capabilities = capabilities();
    if (!fss_transport_abi_compatible(got_version, got_capabilities)) {
        fprintf(stderr, "[FAIL] transport %s: incompatible ABI %u.%u or missing channel capabilities\n",
                plugin, (unsigned)FSS_TRANSPORT_ABI_VERSION_MAJOR_OF(got_version),
                (unsigned)FSS_TRANSPORT_ABI_VERSION_MINOR_OF(got_version));
        return 1;
    }
    return 0;
}

int fss_load_inproc(const char *plugin, fss_inproc_lib *lib)
{
    memset(lib, 0, sizeof(*lib));
    lib->dl = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
    if (!lib->dl) { fprintf(stderr, "[FAIL] dlopen %s: %s\n", plugin, dlerror()); return 1; }
    if (fss_check_transport_abi(lib->dl, plugin)) {
        dlclose(lib->dl);
        memset(lib, 0, sizeof(*lib));
        return 1;
    }
    lib->create  = (fss_create_fn_t) dlsym(lib->dl, "fss_transport_create");
    lib->destroy = (fss_destroy_fn_t)dlsym(lib->dl, "fss_transport_destroy");
    if (!lib->create || !lib->destroy) {
        fprintf(stderr, "[FAIL] dlsym factory: %s\n", dlerror());
        dlclose(lib->dl);
        memset(lib, 0, sizeof(*lib));
        return 1;
    }
    return 0;
}

/* Only the standard factory symbols are resolved: the inbound sink is installed
 * through the transport's own vtable slot (t->attach with FSS_ROLE_TARGET), so
 * the runtime knows no per-transport symbol name and no transport-kind enum.
 * New transport = new plugin, zero recompile. */
int fss_load_channel(const char *plugin, fss_chan_lib *lib)
{
    memset(lib, 0, sizeof(*lib));
    lib->dl = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
    if (!lib->dl) { fprintf(stderr, "[FAIL] dlopen %s: %s\n", plugin, dlerror()); return 1; }
    if (fss_check_transport_abi(lib->dl, plugin)) {
        dlclose(lib->dl);
        memset(lib, 0, sizeof(*lib));
        return 1;
    }
    lib->create  = (fss_create_fn_t) dlsym(lib->dl, "fss_transport_create");
    lib->destroy = (fss_destroy_fn_t)dlsym(lib->dl, "fss_transport_destroy");
    if (!lib->create || !lib->destroy) {
        fprintf(stderr, "[FAIL] dlsym channel factory: %s\n", dlerror());
        dlclose(lib->dl);
        memset(lib, 0, sizeof(*lib));
        return 1;
    }
    return 0;
}

fss_transport_t *fss_chan_create_retry(fss_chan_lib *lib, const char *cfg)
{
    fss_transport_t *t = NULL;
    for (int i = 0; i < 400 && !t; i++) { t = lib->create(cfg); if (!t) fss_msleep(5); }
    return t;
}

/* Resolve a transport NAME to its plugin file: libfss_<name><suffix> in `dir`
 * (or on the loader search path when `dir` is NULL/empty).  The suffix is the
 * platform's dlopen'able MODULE suffix (".so" even on macOS for MODULE plugins);
 * FSS_MODULE_SUFFIX is set by the build. */
#ifndef FSS_MODULE_SUFFIX
#define FSS_MODULE_SUFFIX ".so"
#endif
void fss_transport_plugin_path(const char *dir, const char *name, char *out, size_t n)
{
    if (dir && dir[0]) snprintf(out, n, "%s/libfss_%s%s", dir, name, FSS_MODULE_SUFFIX);
    else               snprintf(out, n, "libfss_%s%s", name, FSS_MODULE_SUFFIX);
}

/* Resolve a model NAME to its plugin file, mirroring fss_transport_plugin_path
 * (a model is passed BY NAME, never as a path).  A model may come from any
 * toolchain, so the suffix is not fixed: in `dir`, probe lib<name>.dylib then
 * lib<name>.so; with no dir, hand the loader the bare lib<name><suffix> to find
 * on its search path. */
void fss_model_plugin_path(const char *dir, const char *name, char *out, size_t n)
{
    if (dir && dir[0]) {
        snprintf(out, n, "%s/lib%s.dylib", dir, name);
        if (access(out, F_OK) == 0) return;
        snprintf(out, n, "%s/lib%s.so", dir, name);
    } else {
        snprintf(out, n, "lib%s%s", name, FSS_MODULE_SUFFIX);
    }
}

/* Load a channel transport BY NAME: resolve the plugin (fss_transport_plugin_path),
 * dlopen its factory symbols (fss_load_channel), then CONFIRM the loaded plugin is
 * the one asked for by matching its self-reported fss_transport_name() against
 * `name` (transport_abi.h).  This is the one resolver both a data edge
 * (control::Transport) and the control bootstrap (breadcrumb `transport`) go
 * through, so selecting a transport is always name -> plugin, never a path. */
int fss_load_channel_named(const char *dir, const char *name, fss_chan_lib *lib)
{
    char path[512];
    fss_transport_plugin_path(dir, name, path, sizeof(path));
    if (fss_load_channel(path, lib)) return 1;
    const char *(*rep)(void) = (const char *(*)(void))dlsym(lib->dl, "fss_transport_name");
    const char *reported = rep ? rep() : NULL;
    if (!reported || strcmp(reported, name)) {
        fprintf(stderr, "[FAIL] transport %s: name mismatch/absent (%s)\n",
                name, reported ? reported : "(none)");
        dlclose(lib->dl);
        memset(lib, 0, sizeof(*lib));
        return 1;
    }
    return 0;
}

/* ── kind-aware channel wiring (shm OR zenoh) ─────────────────────────────────
 * create-vs-attach and round-trip-vs-one-way are now channel() parameters
 * (owner, round_trip), not a config dialect.  What remains kind-specific is the
 * SESSION config a transport is create()d with: shm has none (the medium is
 * provisioned per-channel); zenoh needs the process rendezvous (its role +
 * endpoint) to open the shared session.  The channel NAME + shape then go to
 * channel(). */
void fss_chan_session_cfg(int zenoh, const char *role, const char *zrv, char *out, size_t n)
{
    if (!zenoh || !role || !zrv) { if (n) out[0] = '\0'; return; }
    snprintf(out, n, "%s=%s", role, zrv);   /* e.g. "listen=tcp/..." / "connect=tcp/..." */
}

/* Open a named channel on a transport, retrying briefly for an attacher
 * (owner = 0) whose owner has not yet provisioned the medium.  An owner
 * (owner = 1) creates it up front, so a single try suffices. */
fss_channel_t *fss_chan_open_retry(fss_transport_t *t, const char *name, int rt, int owner)
{
    if (!t || !t->channel) return NULL;
    fss_channel_t *ch = t->channel(t->self, name, rt, owner, NULL, 0);
    if (ch || owner) return ch;
    for (int i = 0; i < 400 && !ch; i++) { fss_msleep(5); ch = t->channel(t->self, name, rt, owner, NULL, 0); }
    return ch;
}

/* ── orchestrator taps (NOT artefact endpoints) ─────────────────────────────── */
int fss_counting_receive(void *self, fss_carrier_t *c)
{
    fss_counting_ctx *cs = (fss_counting_ctx *)self;
    int rc = (cs->inner && cs->inner->receive)
                 ? cs->inner->receive(cs->inner->self, c) : FSS_SEND_NO_RECEIVER;
    if (rc == FSS_SEND_OK) (*cs->count)++;
    return rc;
}
int fss_wait_for_count(volatile int *count, int target, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (*count >= target) return 1;
        fss_msleep(5);
    }
    return *count >= target;
}
void fss_local_sim(fss_endpoint_t *sim, uint32_t code)
{
    if (!sim || !sim->receive) return;
    uint8_t b[4]; memcpy(b, &code, 4);
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_SIM_ID; c.data_ptr = b; c.data_size = sizeof(b);
    c.meta_ptr = (void *)FSS_SIM_META; c.meta_size = FSS_SIM_META_LEN;
    sim->receive(sim->self, &c);
}
int fss_fed_status_receive(void *self, fss_carrier_t *c)
{
    fss_fed_status *f = (fss_fed_status *)self;
    if (!f || !c || c->id != FSS_SIM_ID || !c->data_ptr) return FSS_SEND_NO_RECEIVER;
    uint32_t code = 0; memcpy(&code, c->data_ptr, sizeof(code) <= c->data_size ? sizeof(code) : c->data_size);
    if (code == FSS_SIM_ELABORATED) f->ready = 1;
    return FSS_SEND_OK;
}
int fss_wait_for_flag(volatile int *flag, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        if (*flag) return 1;
        fss_msleep(5);
    }
    return *flag;
}
void fss_build_config_json(int frames, int period_us, char *out, size_t n)
{
    if (period_us > 0) snprintf(out, n, "{\"frames\":%d,\"period_us\":%d}", frames, period_us);
    else               snprintf(out, n, "{\"frames\":%d}", frames);
}
int fss_config_responder(void *self, fss_carrier_t *c)
{
    const char *json = (const char *)self;
    if (!c || !c->data_ptr || !c->meta_ptr || !json) return FSS_SEND_NO_RECEIVER;
    /* Resolve the reply fields from the carrier's OWN meta — the same walk every
     * consumer does to read them back (see the artefacts' node_field /
     * py_field), so no offset is hard-coded on either side. */
    uint32_t pv_off, pv_size;
    if (!fss_meta_find(c->meta_ptr, c->meta_size, "config::ParamValue", &pv_off, &pv_size)) return FSS_SEND_NO_RECEIVER;
    if (pv_size == 0 || (size_t)pv_off + pv_size > c->data_size) return FSS_SEND_NO_RECEIVER;
    uint8_t *base = (uint8_t *)c->data_ptr;
    size_t len = strlen(json);
    if (len >= pv_size) len = pv_size - 1;   /* NUL-terminated flat JSON in the fixed value slot */
    memset(base + pv_off, 0, pv_size); memcpy(base + pv_off, json, len);
    uint32_t st_off, st_size;                /* Status = Ok(0), if the carrier has one */
    if (fss_meta_find(c->meta_ptr, c->meta_size, "config::Status", &st_off, &st_size)
        && st_size >= sizeof(uint32_t) && (size_t)st_off + sizeof(uint32_t) <= c->data_size) {
        uint32_t ok = 0; memcpy(base + st_off, &ok, sizeof(ok));
    }
    return FSS_SEND_OK;
}

/* ── control driver ─────────────────────────────────────────────────────────
 * Round-trip one control instruction to a remote agent: the agent interprets it
 * and writes status back in place, which the round-trip channel returns.  (An
 * in-proc agent is driven by a direct fss_agent_ctrl call, in engine.c — the
 * kind-aware channel building both an orchestrator and an agent need lives in
 * fss_chan_* above, so a cross edge is now served/attached inside whichever
 * agent owns its endpoint, not by the orchestrator directly.) */
int fss_ctrl_send(fss_endpoint_t *cep, fss_ctrl_instr *in)
{
    if (!cep || !cep->send || !in) return -1;
    fss_carrier_t c; memset(&c, 0, sizeof(c));
    c.id = FSS_CONTROL_ID; c.data_ptr = in; c.data_size = sizeof(*in);
    c.meta_ptr = (void *)FSS_CONTROL_META; c.meta_size = FSS_CONTROL_META_LEN;
    if (cep->send(cep->send_ctx, &c) != FSS_SEND_OK) return -1;
    return (int)in->status;
}

/* Stateful topology RESPONDER (R11 pull): the orchestrator owns the control
 * channel and installs this as its TARGET sink, with `self` = the calling
 * agent's whole build+wire plan (a JSON step array).  The agent CALLS BACK with
 * an empty control::plan request; this writes the plan into the PlanJson slot +
 * Status=Ok, returned over the round-trip.  Per-agent 1:1 control channel, so the
 * channel identifies the asker — no self-id needed.  (Topology only; lifecycle
 * such as stop rides a SEPARATE sim-control channel, never this one.) */
int fss_plan_responder(void *self, fss_carrier_t *c)
{
    const char *plan = (const char *)self;
    if (!c || !c->data_ptr || !c->meta_ptr || !plan) return FSS_SEND_NO_RECEIVER;
    uint32_t pj_off, pj_size, st_off, st_size;
    if (!fss_meta_find(c->meta_ptr, c->meta_size, "control::PlanJson", &pj_off, &pj_size))
        return FSS_SEND_NO_RECEIVER;
    if (pj_size == 0 || (size_t)pj_off + pj_size > c->data_size) return FSS_SEND_NO_RECEIVER;
    uint8_t *base = (uint8_t *)c->data_ptr;
    size_t len = strlen(plan);
    if (len >= pj_size) len = pj_size - 1;
    memset(base + pj_off, 0, pj_size);
    memcpy(base + pj_off, plan, len);
    if (fss_meta_find(c->meta_ptr, c->meta_size, "control::Status", &st_off, &st_size)
        && st_size >= sizeof(uint32_t) && (size_t)st_off + st_size <= c->data_size) {
        uint32_t ok = 0; memcpy(base + st_off, &ok, sizeof(ok));
    }
    return FSS_SEND_OK;
}
