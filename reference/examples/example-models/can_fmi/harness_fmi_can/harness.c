/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * harness_fmi_can — a minimal FMI 3.0 co-simulation importer that runs the
 * can_initiator and can_target FMUs as a trivial CAN bus.  No FSS.
 *
 * Like the other example-models harnesses, it wires two clean models together
 * and runs them, proving they work standalone before any FSS adapter exists.
 * The harness itself plays the "bus simulation": when the
 * initiator's Tx_Clock ticks it harvests the serialised CAN operation from
 * Tx_Data and delivers it to the target's Rx_Data, activating Rx_Clock.
 *
 * Both FMUs are loaded with RTLD_LOCAL so their identically-named, unprefixed
 * fmi3* exports stay private per handle (same trick the in-proc transport
 * uses for FSS artefacts).
 *
 * Exit 0 iff the target received exactly the frames the initiator sent.
 */

#include "fmi3Functions.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_STEPS      16          /* frames to send */
#define CAN_FRAME_ID 0x100u

/* The fmi3 entry points the harness uses, resolved per FMU. */
typedef struct {
    void *lib;
    fmi3InstantiateCoSimulationTYPE *Instantiate;
    fmi3FreeInstanceTYPE            *FreeInstance;
    fmi3EnterInitializationModeTYPE *EnterInit;
    fmi3ExitInitializationModeTYPE  *ExitInit;
    fmi3EnterEventModeTYPE          *EnterEvent;
    fmi3EnterStepModeTYPE           *EnterStep;
    fmi3UpdateDiscreteStatesTYPE    *UpdateDiscrete;
    fmi3DoStepTYPE                  *DoStep;
    fmi3TerminateTYPE               *Terminate;
    fmi3GetIntervalDecimalTYPE      *GetIntervalDecimal;
    fmi3GetClockTYPE                *GetClock;
    fmi3SetClockTYPE                *SetClock;
    fmi3GetBinaryTYPE               *GetBinary;
    fmi3SetBinaryTYPE               *SetBinary;
    fmi3GetUInt32TYPE               *GetUInt32;
    fmi3GetUInt64TYPE               *GetUInt64;
    fmi3Instance                     inst;
} fmu_t;

static void log_cb(fmi3InstanceEnvironment env, fmi3Status status,
                   fmi3String category, fmi3String message)
{
    (void)env; (void)status; (void)category;
    fprintf(stderr, "  [fmu] %s\n", message ? message : "");
}

#define SYM(f, name) \
    do { f->name = (void *)dlsym(f->lib, "fmi3" #name); } while (0)

static int load(fmu_t *f, const char *path)
{
    memset(f, 0, sizeof(*f));
    f->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!f->lib) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); return -1; }

    /* dlsym names need the fmi3 prefix; resolve each. */
    f->Instantiate        = (fmi3InstantiateCoSimulationTYPE *)dlsym(f->lib, "fmi3InstantiateCoSimulation");
    f->FreeInstance       = (fmi3FreeInstanceTYPE *)           dlsym(f->lib, "fmi3FreeInstance");
    f->EnterInit          = (fmi3EnterInitializationModeTYPE *)dlsym(f->lib, "fmi3EnterInitializationMode");
    f->ExitInit           = (fmi3ExitInitializationModeTYPE *) dlsym(f->lib, "fmi3ExitInitializationMode");
    f->EnterEvent         = (fmi3EnterEventModeTYPE *)         dlsym(f->lib, "fmi3EnterEventMode");
    f->EnterStep          = (fmi3EnterStepModeTYPE *)          dlsym(f->lib, "fmi3EnterStepMode");
    f->UpdateDiscrete     = (fmi3UpdateDiscreteStatesTYPE *)   dlsym(f->lib, "fmi3UpdateDiscreteStates");
    f->DoStep             = (fmi3DoStepTYPE *)                 dlsym(f->lib, "fmi3DoStep");
    f->Terminate          = (fmi3TerminateTYPE *)              dlsym(f->lib, "fmi3Terminate");
    f->GetIntervalDecimal = (fmi3GetIntervalDecimalTYPE *)     dlsym(f->lib, "fmi3GetIntervalDecimal");
    f->GetClock           = (fmi3GetClockTYPE *)               dlsym(f->lib, "fmi3GetClock");
    f->SetClock           = (fmi3SetClockTYPE *)               dlsym(f->lib, "fmi3SetClock");
    f->GetBinary          = (fmi3GetBinaryTYPE *)              dlsym(f->lib, "fmi3GetBinary");
    f->SetBinary          = (fmi3SetBinaryTYPE *)              dlsym(f->lib, "fmi3SetBinary");
    f->GetUInt32          = (fmi3GetUInt32TYPE *)              dlsym(f->lib, "fmi3GetUInt32");
    f->GetUInt64          = (fmi3GetUInt64TYPE *)              dlsym(f->lib, "fmi3GetUInt64");

    if (!f->Instantiate || !f->DoStep || !f->EnterEvent || !f->UpdateDiscrete) {
        fprintf(stderr, "%s: missing required fmi3 exports\n", path);
        return -1;
    }
    return 0;
}

static int instantiate(fmu_t *f, const char *name)
{
    f->inst = f->Instantiate(name, /*token=*/"", /*resource=*/NULL,
                             fmi3False, fmi3False, /*eventModeUsed=*/fmi3True,
                             fmi3False, NULL, 0, NULL, log_cb, NULL);
    if (!f->inst) { fprintf(stderr, "%s: instantiate failed\n", name); return -1; }
    if (f->EnterInit(f->inst, fmi3False, 0.0, 0.0, fmi3False, 0.0) != fmi3OK) return -1;
    if (f->ExitInit(f->inst) != fmi3OK) return -1;
    return 0;
}

static void update_states(fmu_t *f)
{
    fmi3Boolean a, b, c, d, e; fmi3Float64 t;
    f->UpdateDiscrete(f->inst, &a, &b, &c, &d, &e, &t);
}

int main(void)
{
    fmu_t ini, tgt;
    int rc = 1;

    if (load(&ini, CAN_INITIATOR_LIB) != 0) return 1;
    if (load(&tgt, CAN_TARGET_LIB)    != 0) { dlclose(ini.lib); return 1; }

    if (instantiate(&ini, "initiator") != 0) goto done;
    if (instantiate(&tgt, "target")    != 0) goto done;

    /* Learn the initiator's Tx_Clock period; use it as the step size. */
    fmi3Float64 dt = 0.001;
    if (ini.GetIntervalDecimal) {
        const fmi3ValueReference tx_clock = 2u;
        fmi3Float64 iv; fmi3IntervalQualifier q;
        if (ini.GetIntervalDecimal(ini.inst, &tx_clock, 1, &iv, &q) == fmi3OK &&
            q == fmi3IntervalChanged && iv > 0.0)
            dt = iv;
    }

    /* Both FMUs begin in Step Mode after init. */
    ini.EnterStep(ini.inst);
    tgt.EnterStep(tgt.inst);

    fmi3Float64 t = 0.0;
    for (int step = 0; step < N_STEPS; ++step) {
        fmi3Boolean evt = fmi3False, term = fmi3False, early = fmi3False;
        fmi3Float64 last = 0.0;

        /* --- initiator advances; its periodic Tx_Clock ticks --- */
        ini.DoStep(ini.inst, t, dt, fmi3True, &evt, &term, &early, &last);

        size_t       frame_size = 0;
        const fmi3UInt8 *frame   = NULL;
        if (evt) {
            ini.EnterEvent(ini.inst);
            update_states(&ini);                 /* serialise the frame */

            const fmi3ValueReference tx_clock = 2u, tx_data = 1u;
            fmi3Clock active = fmi3ClockInactive;
            ini.GetClock(ini.inst, &tx_clock, 1, &active);
            if (active == fmi3ClockActive && ini.GetBinary) {
                size_t sz = 0; fmi3Binary val = NULL;
                ini.GetBinary(ini.inst, &tx_data, 1, &sz, &val, 1);
                frame_size = sz; frame = (const fmi3UInt8 *)val;
            }
            ini.EnterStep(ini.inst);
        }

        /* --- bus: deliver the frame to the target's Rx_Clock/Rx_Data --- */
        if (frame && frame_size > 0) {
            tgt.EnterEvent(tgt.inst);
            const fmi3ValueReference rx_clock = 2u, rx_data = 1u;
            const fmi3Clock active = fmi3ClockActive;
            tgt.SetClock(tgt.inst, &rx_clock, 1, &active);
            const size_t   sz  = frame_size;
            const fmi3Binary val = (fmi3Binary)frame;
            tgt.SetBinary(tgt.inst, &rx_data, 1, &sz, &val, 1);
            update_states(&tgt);                 /* decode the frame */
            tgt.EnterStep(tgt.inst);
        }

        /* --- target advances over the same interval --- */
        tgt.DoStep(tgt.inst, t, dt, fmi3True, &evt, &term, &early, &last);
        t += dt;
    }

    /* --- verify the target received exactly what the initiator sent --- */
    fmi3UInt64 tx_count = 0, rx_count = 0;
    fmi3UInt32 last_id = 0, last_byte0 = 0;
    const fmi3ValueReference vr_tx_count = 3u, vr_rx_count = 4u,
                             vr_last_id = 3u, vr_last_byte0 = 5u;
    ini.GetUInt64(ini.inst, &vr_tx_count, 1, &tx_count, 1);
    tgt.GetUInt64(tgt.inst, &vr_rx_count, 1, &rx_count, 1);
    tgt.GetUInt32(tgt.inst, &vr_last_id, 1, &last_id, 1);
    tgt.GetUInt32(tgt.inst, &vr_last_byte0, 1, &last_byte0, 1);

    printf("sent=%llu received=%llu last_id=0x%X last_byte0=%u\n",
           (unsigned long long)tx_count, (unsigned long long)rx_count,
           last_id, last_byte0);

    if (tx_count == (fmi3UInt64)N_STEPS &&
        rx_count == (fmi3UInt64)N_STEPS &&
        last_id  == CAN_FRAME_ID &&
        last_byte0 == (fmi3UInt32)((N_STEPS - 1) & 0xFF)) {
        printf("[OK] can-fmi round-trip: %d CAN frames delivered\n", N_STEPS);
        rc = 0;
    } else {
        printf("[FAIL] can-fmi round-trip mismatch\n");
    }

done:
    if (ini.inst) { ini.Terminate(ini.inst); ini.FreeInstance(ini.inst); }
    if (tgt.inst) { tgt.Terminate(tgt.inst); tgt.FreeInstance(tgt.inst); }
    if (ini.lib) dlclose(ini.lib);
    if (tgt.lib) dlclose(tgt.lib);
    return rc;
}
