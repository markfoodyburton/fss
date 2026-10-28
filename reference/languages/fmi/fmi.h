/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * languages/fmi/fmi.h — FMI 3.0 Co-Simulation binding for FSS.
 *
 * SCOPE: imports and drives ONE Co-Simulation FMU (plays the importer /
 * per-FMU master driver).  NOT an FMU exporter, NOT an SSP/system
 * importer (a multi-FMU "simulator"), NOT Model Exchange / Scheduled
 * Execution.  A full FSS federation composes many such FMU nodes via the
 * transport.  See README.md §Scope.
 *
 * Protocol- and carrier-agnostic.  Provides:
 *   fss_fmi_open  — dlopen an FMU, resolve fmi3* entry points, init.
 *   fss_fmi_close — terminate + free.
 *   Accessors:    fss_fmi_set_binary, fss_fmi_set_clock,
 *                 status-aware and legacy u32/u64/clock/binary getters.
 *   Step helpers: fss_fmi_step (no-set convenience),
 *                 fss_fmi_enter_event + fss_fmi_advance (two-phase,
 *                 for callers that set clocks/vars between the phases).
 *
 * FMI-3.0 Co-Simulation event/step sequencing:
 *   fss_fmi_enter_event(h)          — EnterEventMode
 *   fss_fmi_set_clock(h, vr)        — SetClock (must be in event mode)
 *   fss_fmi_set_binary(h, vr, ...)  — SetBinary (must be in event mode)
 *   fss_fmi_advance(h, t, dt)       — UpdateDiscreteStates +
 *                                     EnterStepMode + DoStep(t, dt)
 *
 * fss_fmi_step(h, t, dt) is a convenience wrapper for callers that have
 * no variables to set: EnterEventMode + UpdateDiscreteStates +
 * EnterStepMode + DoStep(t, dt).
 *
 * Callers bring their own protocol logic
 * and use these primitives to drive the FMU.
 */
#ifndef FSS_LANGUAGES_FMI_H
#define FSS_LANGUAGES_FMI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque FMI handle. */
typedef struct fss_fmi fss_fmi_t;

/*
 * Normalized FMI status values.  These deliberately have the FMI 3.0 numeric
 * values, but do not require an FMI header in users of this public header.
 */
typedef enum fss_fmi_status {
    FSS_FMI_STATUS_OK      = 0,
    FSS_FMI_STATUS_WARNING = 1,
    FSS_FMI_STATUS_DISCARD = 2,
    FSS_FMI_STATUS_ERROR   = 3,
    FSS_FMI_STATUS_FATAL   = 4
} fss_fmi_status_t;

/*
 * Result from a status-aware step operation.  The boolean fields are the
 * outputs from fmi3DoStep.  event_handling_needed is observed, not handled:
 * callers that need a complete event-mode lifecycle must drive it themselves.
 */
typedef struct fss_fmi_step_result {
    fss_fmi_status_t status;
    int event_handling_needed;
    int terminate_simulation;
    int early_return;
    double last_successful_time;
} fss_fmi_step_result_t;

/*
 * fss_fmi_open — dlopen `fmu_path`, resolve the fmi3 entry points,
 * InstantiateCoSimulation, EnterInitializationMode,
 * ExitInitializationMode, EnterStepMode.
 * Returns a handle on success, NULL on any failure (including `fmu_path == NULL`).
 */
fss_fmi_t *fss_fmi_open(const char *fmu_path);

/* fss_fmi_close — Terminate + FreeInstance + dlclose + free. */
void fss_fmi_close(fss_fmi_t *h);

/*
 * fss_fmi_set_binary — SetBinary(vr, bytes, len).
 * Must be called while in event mode (after fss_fmi_enter_event).
 * Returns 0 for fmi3OK or fmi3Warning; otherwise returns the normalized
 * fmi3Status.
 */
int fss_fmi_set_binary(fss_fmi_t *h, unsigned vr,
                       const void *bytes, size_t len);

/*
 * fss_fmi_set_clock — SetClock(vr, fmi3ClockActive).
 * Must be called while in event mode (after fss_fmi_enter_event).
 * Returns 0 for fmi3OK or fmi3Warning; otherwise returns the normalized
 * fmi3Status.
 */
int fss_fmi_set_clock(fss_fmi_t *h, unsigned vr);

/*
 * Status-aware getters.  They return the normalized FMI status and write a
 * zero/inactive/NULL result before every call.  Thus all supplied output
 * parameters have a safe value on failure.  OK and Warning both provide a
 * valid result.
 */
fss_fmi_status_t fss_fmi_get_u32_status(fss_fmi_t *h, unsigned vr,
                                         uint32_t *value);
fss_fmi_status_t fss_fmi_get_u64_status(fss_fmi_t *h, unsigned vr,
                                         uint64_t *value);
fss_fmi_status_t fss_fmi_get_clock_status(fss_fmi_t *h, unsigned vr,
                                           int *active);
fss_fmi_status_t fss_fmi_get_binary_status(fss_fmi_t *h, unsigned vr,
                                            const void **bytes, size_t *len);

/* Legacy value-only wrappers.  They cannot distinguish a failed read from a
 * valid zero value; use the status-aware variants in new code. */
uint32_t fss_fmi_get_u32(fss_fmi_t *h, unsigned vr);
uint64_t fss_fmi_get_u64(fss_fmi_t *h, unsigned vr);

/*
 * fss_fmi_get_clock — GetClock(vr): returns 1 if the (output) clock is active,
 * else 0.  Reading an output clock typically CONSUMES the tick.  Returns 0 if
 * the FMU exports no GetClock or the read fails.
 */
int fss_fmi_get_clock(fss_fmi_t *h, unsigned vr);

/*
 * fss_fmi_get_binary — GetBinary(vr): hand back the FMU-owned byte buffer and
 * its length via *bytes / *len.  The pointer is valid only until the next FMU
 * call.  This legacy wrapper returns 0 for fmi3OK or fmi3Warning, a normalized
 * failure status, or -1 if the FMU exports no GetBinary.
 */
int fss_fmi_get_binary(fss_fmi_t *h, unsigned vr, const void **bytes, size_t *len);

/*
 * fss_fmi_enter_event_status — status-aware EnterEventMode.  The legacy void
 * fss_fmi_enter_event symbol remains available for source and ABI compatibility.
 */
fss_fmi_status_t fss_fmi_enter_event_status(fss_fmi_t *h);

/*
 * fss_fmi_enter_event — transition to event mode: EnterEventMode.
 * Call before fss_fmi_set_clock / fss_fmi_set_binary in each step cycle.
 */
void fss_fmi_enter_event(fss_fmi_t *h);

/*
 * Status-aware variants of the step helpers.  They preserve the legacy
 * sequencing: advance performs one UpdateDiscreteStates call, then enters
 * step mode and calls DoStep.  In particular, event_handling_needed does not
 * cause this binding to run a full event iteration.
 */
fss_fmi_step_result_t fss_fmi_advance_status(fss_fmi_t *h, double t, double dt);
fss_fmi_step_result_t fss_fmi_step_status(fss_fmi_t *h, double t, double dt);

/*
 * fss_fmi_advance — complete one step cycle from event mode:
 * UpdateDiscreteStates + EnterStepMode + DoStep(t, dt).
 * Call after fss_fmi_set_clock / fss_fmi_set_binary.
 * Returns 0 unless the status-aware operation reports fmi3Error or fmi3Fatal.
 */
int fss_fmi_advance(fss_fmi_t *h, double t, double dt);

/*
 * fss_fmi_step — convenience: EnterEventMode + UpdateDiscreteStates +
 * EnterStepMode + DoStep(t, dt).  Use when no variables need to be
 * set in event mode.
 * Returns 0 unless the status-aware operation reports fmi3Error or fmi3Fatal.
 */
int fss_fmi_step(fss_fmi_t *h, double t, double dt);

#ifdef __cplusplus
}
#endif
#endif /* FSS_LANGUAGES_FMI_H */
