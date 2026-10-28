/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * languages/fmi/fmi.c — generic FMI-3.0 Co-Simulation binding for FSS.
 *
 * Protocol- and carrier-agnostic FMI mechanics: open/close an FMU,
 * generic variable/clock/step accessors.  No protocol, no LS-BUS, no
 * carrier-specific VR constants.
 */

#include "fmi.h"

/* FMI-3.0 function-pointer types. */
#include "fmi3Functions.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* FMU entry-point table + instance                                   */
/* ------------------------------------------------------------------ */
struct fss_fmi {
    void                            *lib;
    fmi3InstantiateCoSimulationTYPE *Instantiate;
    fmi3FreeInstanceTYPE            *FreeInstance;
    fmi3EnterInitializationModeTYPE *EnterInit;
    fmi3ExitInitializationModeTYPE  *ExitInit;
    fmi3EnterEventModeTYPE          *EnterEvent;
    fmi3EnterStepModeTYPE           *EnterStep;
    fmi3UpdateDiscreteStatesTYPE    *UpdateDiscrete;
    fmi3DoStepTYPE                  *DoStep;
    fmi3TerminateTYPE               *Terminate;
    fmi3SetBinaryTYPE               *SetBinary;
    fmi3SetClockTYPE                *SetClock;
    fmi3GetClockTYPE                *GetClock;
    fmi3GetBinaryTYPE               *GetBinary;
    fmi3GetUInt32TYPE               *GetUInt32;
    fmi3GetUInt64TYPE               *GetUInt64;
    fmi3Instance                     inst;
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */
static void log_cb(fmi3InstanceEnvironment env, fmi3Status status,
                   fmi3String category, fmi3String message)
{
    (void)env; (void)status; (void)category;
    fprintf(stderr, "  [fss-fmi] %s\n", message ? message : "");
}

static fss_fmi_status_t normalize_status(fmi3Status status)
{
    switch (status) {
    case fmi3OK:      return FSS_FMI_STATUS_OK;
    case fmi3Warning: return FSS_FMI_STATUS_WARNING;
    case fmi3Discard: return FSS_FMI_STATUS_DISCARD;
    case fmi3Error:   return FSS_FMI_STATUS_ERROR;
    case fmi3Fatal:   return FSS_FMI_STATUS_FATAL;
    default:          return FSS_FMI_STATUS_ERROR;
    }
}

static int status_is_error(fss_fmi_status_t status)
{
    return status == FSS_FMI_STATUS_ERROR || status == FSS_FMI_STATUS_FATAL;
}

static int status_can_continue(fss_fmi_status_t status)
{
    return status == FSS_FMI_STATUS_OK || status == FSS_FMI_STATUS_WARNING;
}

/* Legacy accessors report both FMI success statuses as zero. */
static int legacy_status(fss_fmi_status_t status)
{
    return status_can_continue(status) ? 0 : (int)status;
}

static fss_fmi_step_result_t step_result(fss_fmi_status_t status)
{
    fss_fmi_step_result_t result;
    result.status = status;
    result.event_handling_needed = 0;
    result.terminate_simulation = 0;
    result.early_return = 0;
    result.last_successful_time = 0.0;
    return result;
}

static fss_fmi_status_t update_discrete(struct fss_fmi *h)
{
    fmi3Boolean a, b, c, d, e; fmi3Float64 t;
    return normalize_status(h->UpdateDiscrete(h->inst, &a, &b, &c, &d, &e, &t));
}

/* ------------------------------------------------------------------ */
/* fss_fmi_open                                                       */
/* ------------------------------------------------------------------ */
fss_fmi_t *fss_fmi_open(const char *fmu_path)
{
    if (!fmu_path) return NULL;

    fss_fmi_t *h = (fss_fmi_t *)calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->lib = dlopen(fmu_path, RTLD_NOW | RTLD_LOCAL);
    if (!h->lib) {
        fprintf(stderr, "fss-fmi: dlopen %s: %s\n", fmu_path, dlerror());
        free(h); return NULL;
    }

    h->Instantiate    = (fmi3InstantiateCoSimulationTYPE *)dlsym(h->lib, "fmi3InstantiateCoSimulation");
    h->FreeInstance   = (fmi3FreeInstanceTYPE *)           dlsym(h->lib, "fmi3FreeInstance");
    h->EnterInit      = (fmi3EnterInitializationModeTYPE *)dlsym(h->lib, "fmi3EnterInitializationMode");
    h->ExitInit       = (fmi3ExitInitializationModeTYPE *) dlsym(h->lib, "fmi3ExitInitializationMode");
    h->EnterEvent     = (fmi3EnterEventModeTYPE *)         dlsym(h->lib, "fmi3EnterEventMode");
    h->EnterStep      = (fmi3EnterStepModeTYPE *)          dlsym(h->lib, "fmi3EnterStepMode");
    h->UpdateDiscrete = (fmi3UpdateDiscreteStatesTYPE *)   dlsym(h->lib, "fmi3UpdateDiscreteStates");
    h->DoStep         = (fmi3DoStepTYPE *)                 dlsym(h->lib, "fmi3DoStep");
    h->Terminate      = (fmi3TerminateTYPE *)              dlsym(h->lib, "fmi3Terminate");
    h->SetBinary      = (fmi3SetBinaryTYPE *)              dlsym(h->lib, "fmi3SetBinary");
    h->SetClock       = (fmi3SetClockTYPE *)               dlsym(h->lib, "fmi3SetClock");
    h->GetClock       = (fmi3GetClockTYPE *)               dlsym(h->lib, "fmi3GetClock");
    h->GetBinary      = (fmi3GetBinaryTYPE *)              dlsym(h->lib, "fmi3GetBinary");
    h->GetUInt32      = (fmi3GetUInt32TYPE *)              dlsym(h->lib, "fmi3GetUInt32");
    h->GetUInt64      = (fmi3GetUInt64TYPE *)              dlsym(h->lib, "fmi3GetUInt64");

    /* Required = the lifecycle + step machinery every Co-Simulation FMU has.
     * The accessors (Set/Get Binary/Clock/UIntN) are DIRECTION-specific — a
     * sender FMU has no SetBinary/SetClock, a receiver no GetBinary/GetClock —
     * so they are optional here and null-checked at the call site. */
    if (!h->Instantiate || !h->FreeInstance || !h->EnterInit || !h->ExitInit ||
        !h->EnterEvent  || !h->EnterStep    || !h->UpdateDiscrete || !h->DoStep) {
        fprintf(stderr, "fss-fmi: %s: missing required fmi3 exports\n", fmu_path);
        dlclose(h->lib); free(h); return NULL;
    }

    h->inst = h->Instantiate(
        "fss-fmi",          /* instanceName */
        "",                 /* instantiationToken */
        NULL,               /* resourcePath */
        fmi3False,          /* visible */
        fmi3False,          /* loggingOn */
        fmi3True,           /* eventModeUsed */
        fmi3False,          /* earlyReturnAllowed */
        NULL, 0,            /* requiredIntermediateVariables, n */
        NULL,               /* instanceEnvironment */
        log_cb,             /* logMessage */
        NULL                /* intermediateUpdate */
    );
    if (!h->inst) {
        fprintf(stderr, "fss-fmi: InstantiateCoSimulation failed\n");
        dlclose(h->lib); free(h); return NULL;
    }

    if (!status_can_continue(normalize_status(
            h->EnterInit(h->inst, fmi3False, 0.0, 0.0, fmi3False, 0.0)))) {
        fprintf(stderr, "fss-fmi: EnterInitializationMode failed\n");
        h->FreeInstance(h->inst); dlclose(h->lib); free(h); return NULL;
    }
    if (!status_can_continue(normalize_status(h->ExitInit(h->inst)))) {
        fprintf(stderr, "fss-fmi: ExitInitializationMode failed\n");
        h->FreeInstance(h->inst); dlclose(h->lib); free(h); return NULL;
    }
    if (!status_can_continue(normalize_status(h->EnterStep(h->inst)))) {
        fprintf(stderr, "fss-fmi: EnterStepMode failed\n");
        h->FreeInstance(h->inst); dlclose(h->lib); free(h); return NULL;
    }

    return h;
}

/* ------------------------------------------------------------------ */
/* fss_fmi_close                                                      */
/* ------------------------------------------------------------------ */
void fss_fmi_close(fss_fmi_t *h)
{
    if (!h) return;
    if (h->inst) {
        if (h->Terminate) h->Terminate(h->inst);
        h->FreeInstance(h->inst);
    }
    if (h->lib) dlclose(h->lib);
    free(h);
}

/* ------------------------------------------------------------------ */
/* Variable/clock accessors                                           */
/* ------------------------------------------------------------------ */
int fss_fmi_set_binary(fss_fmi_t *h, unsigned vr,
                       const void *bytes, size_t len)
{
    if (!h || !h->SetBinary) return -1;
    const fmi3ValueReference ref = (fmi3ValueReference)vr;
    const fmi3Binary         val = (fmi3Binary)bytes;
    return legacy_status(normalize_status(
        h->SetBinary(h->inst, &ref, 1, &len, &val, 1)));
}

int fss_fmi_set_clock(fss_fmi_t *h, unsigned vr)
{
    if (!h || !h->SetClock) return -1;
    const fmi3ValueReference ref = (fmi3ValueReference)vr;
    const fmi3Clock          clk = fmi3ClockActive;
    return legacy_status(normalize_status(h->SetClock(h->inst, &ref, 1, &clk)));
}

fss_fmi_status_t fss_fmi_get_u32_status(fss_fmi_t *h, unsigned vr,
                                         uint32_t *value)
{
    if (value) *value = 0u;
    if (!h || !h->GetUInt32) return FSS_FMI_STATUS_ERROR;
    const fmi3ValueReference ref = (fmi3ValueReference)vr;
    fmi3UInt32 val = 0;
    fss_fmi_status_t status =
        normalize_status(h->GetUInt32(h->inst, &ref, 1, &val, 1));
    if (status_can_continue(status) && value) *value = (uint32_t)val;
    return status;
}

fss_fmi_status_t fss_fmi_get_u64_status(fss_fmi_t *h, unsigned vr,
                                         uint64_t *value)
{
    if (value) *value = 0u;
    if (!h || !h->GetUInt64) return FSS_FMI_STATUS_ERROR;
    const fmi3ValueReference ref = (fmi3ValueReference)vr;
    fmi3UInt64 val = 0;
    fss_fmi_status_t status =
        normalize_status(h->GetUInt64(h->inst, &ref, 1, &val, 1));
    if (status_can_continue(status) && value) *value = (uint64_t)val;
    return status;
}

fss_fmi_status_t fss_fmi_get_clock_status(fss_fmi_t *h, unsigned vr,
                                           int *active)
{
    if (active) *active = 0;
    if (!h || !h->GetClock) return FSS_FMI_STATUS_ERROR;
    const fmi3ValueReference ref = (fmi3ValueReference)vr;
    fmi3Clock val = fmi3ClockInactive;
    fss_fmi_status_t status = normalize_status(h->GetClock(h->inst, &ref, 1, &val));
    if (status_can_continue(status) && active)
        *active = val == fmi3ClockActive ? 1 : 0;
    return status;
}

fss_fmi_status_t fss_fmi_get_binary_status(fss_fmi_t *h, unsigned vr,
                                            const void **bytes, size_t *len)
{
    if (bytes) *bytes = NULL;
    if (len) *len = 0;
    if (!h || !h->GetBinary) return FSS_FMI_STATUS_ERROR;
    const fmi3ValueReference ref = (fmi3ValueReference)vr;
    size_t      sz  = 0;
    fmi3Binary  val = NULL;
    fss_fmi_status_t status =
        normalize_status(h->GetBinary(h->inst, &ref, 1, &sz, &val, 1));
    if (!status_can_continue(status)) return status;
    if (bytes) *bytes = (const void *)val;
    if (len)   *len   = sz;
    return status;
}

uint32_t fss_fmi_get_u32(fss_fmi_t *h, unsigned vr)
{
    uint32_t value = 0;
    (void)fss_fmi_get_u32_status(h, vr, &value);
    return value;
}

uint64_t fss_fmi_get_u64(fss_fmi_t *h, unsigned vr)
{
    uint64_t value = 0;
    (void)fss_fmi_get_u64_status(h, vr, &value);
    return value;
}

int fss_fmi_get_clock(fss_fmi_t *h, unsigned vr)
{
    int active = 0;
    (void)fss_fmi_get_clock_status(h, vr, &active);
    return active;
}

int fss_fmi_get_binary(fss_fmi_t *h, unsigned vr, const void **bytes, size_t *len)
{
    if (!h || !h->GetBinary) {
        if (bytes) *bytes = NULL;
        if (len) *len = 0;
        return -1;
    }
    return legacy_status(fss_fmi_get_binary_status(h, vr, bytes, len));
}

/* ------------------------------------------------------------------ */
/* Step helpers                                                       */
/* ------------------------------------------------------------------ */

fss_fmi_status_t fss_fmi_enter_event_status(fss_fmi_t *h)
{
    if (!h || !h->EnterEvent) return FSS_FMI_STATUS_ERROR;
    return normalize_status(h->EnterEvent(h->inst));
}

void fss_fmi_enter_event(fss_fmi_t *h)
{
    (void)fss_fmi_enter_event_status(h);
}

fss_fmi_step_result_t fss_fmi_advance_status(fss_fmi_t *h, double t, double dt)
{
    fss_fmi_step_result_t result = step_result(FSS_FMI_STATUS_ERROR);
    fss_fmi_status_t status;

    if (!h || !h->UpdateDiscrete || !h->EnterStep || !h->DoStep)
        return result;

    status = update_discrete(h);
    result.status = status;
    if (!status_can_continue(status)) return result;

    status = normalize_status(h->EnterStep(h->inst));
    if (status != FSS_FMI_STATUS_OK) result.status = status;
    if (!status_can_continue(status)) return result;

    fmi3Boolean evt = fmi3False, term = fmi3False, early = fmi3False;
    fmi3Float64 last = 0.0;
    status = normalize_status(h->DoStep(h->inst, (fmi3Float64)t, (fmi3Float64)dt,
                                        fmi3True, &evt, &term, &early, &last));
    if (status != FSS_FMI_STATUS_OK) result.status = status;
    result.event_handling_needed = evt == fmi3True;
    result.terminate_simulation = term == fmi3True;
    result.early_return = early == fmi3True;
    result.last_successful_time = (double)last;
    return result;
}

fss_fmi_step_result_t fss_fmi_step_status(fss_fmi_t *h, double t, double dt)
{
    fss_fmi_step_result_t result =
        step_result(fss_fmi_enter_event_status(h));
    if (!status_can_continue(result.status)) return result;
    fss_fmi_step_result_t advance = fss_fmi_advance_status(h, t, dt);
    if (advance.status == FSS_FMI_STATUS_OK)
        advance.status = result.status;
    return advance;
}

int fss_fmi_advance(fss_fmi_t *h, double t, double dt)
{
    return status_is_error(fss_fmi_advance_status(h, t, dt).status) ? -1 : 0;
}

int fss_fmi_step(fss_fmi_t *h, double t, double dt)
{
    return status_is_error(fss_fmi_step_status(h, t, dt).status) ? -1 : 0;
}
