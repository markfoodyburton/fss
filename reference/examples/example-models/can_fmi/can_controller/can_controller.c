/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_controller — a clean FMI 3.0 / FMI-LS-BUS CAN *controller* Network FMU.
 *
 * No FSS.  An ordinary FMI 3.0 Co-Simulation FMU modelling the transmit path of
 * a CAN controller: it RECEIVES a transmit request (a CanTransmit op on the
 * `Rx_Data` binary input, clocked by `Rx_Clock`) — a CPU writing the controller's
 * transmit register — QUEUES it, lets simulation TIME PASS across a DoStep, then
 * EMITS the frame on its `Tx_Data` binary output (clocked by the triggered output
 * clock `Tx_Clock`).  This is the faithful "derive + time can pass" a passthrough
 * relay cannot express: the frame genuinely resides in the controller across a
 * step before it goes out on the bus.
 *
 * Variables (see modelDescription.xml):
 *   1  Rx_Data   binary  input   (clocked by Rx_Clock)  — incoming transmit request
 *   2  Rx_Clock  clock   input   (triggered)
 *   3  Tx_Data   binary  output  (clocked by Tx_Clock)  — outgoing CAN frame
 *   4  Tx_Clock  clock   output  (triggered)
 *   5  FwdCount  uint64  output  — frames forwarded so far
 */

#include "fmi3Functions.h"
#include "fmi3LsBusUtil.h"
#include "fmi3LsBusUtilCan.h"

#include <stdlib.h>
#include <string.h>

#define VR_RX_DATA   1u
#define VR_RX_CLOCK  2u
#define VR_TX_DATA   3u
#define VR_TX_CLOCK  4u
#define VR_FWD_COUNT 5u

#define BUF_SIZE   2048
#define QUEUE_CAP  64        /* pending transmit requests held across time steps */

typedef struct {
    fmi3String              name;
    fmi3LogMessageCallback  log;
    fmi3InstanceEnvironment env;

    /* Rx staging: SetBinary copies bytes in, SetClock raises rx_clock_active,
     * UpdateDiscreteStates enqueues the op. */
    fmi3UInt8   rx_buffer[BUF_SIZE];
    size_t      rx_len;
    fmi3Boolean rx_clock_active;

    /* Pending queue: each entry is one serialised CanTransmit op, held until a
     * later step processes it (so simulation time passes between in and out). */
    fmi3UInt8   queue[QUEUE_CAP][BUF_SIZE];
    size_t      queue_len[QUEUE_CAP];
    int         q_head, q_tail, q_count;

    /* Tx staging: the op popped for emission this cycle. */
    fmi3UInt8   tx_buffer[BUF_SIZE];
    size_t      tx_len;
    fmi3Boolean tx_clock_active;

    fmi3UInt64  fwd_count;
} can_controller_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

fmi3Instance fmi3InstantiateCoSimulation(
    fmi3String instanceName, fmi3String instantiationToken, fmi3String resourcePath,
    fmi3Boolean visible, fmi3Boolean loggingOn, fmi3Boolean eventModeUsed,
    fmi3Boolean earlyReturnAllowed, const fmi3ValueReference requiredIntermediateVariables[],
    size_t nRequiredIntermediateVariables, fmi3InstanceEnvironment instanceEnvironment,
    fmi3LogMessageCallback logMessage, fmi3IntermediateUpdateCallback intermediateUpdate)
{
    (void)instantiationToken; (void)resourcePath; (void)visible; (void)loggingOn;
    (void)eventModeUsed; (void)earlyReturnAllowed; (void)requiredIntermediateVariables;
    (void)nRequiredIntermediateVariables; (void)intermediateUpdate;

    can_controller_t *s = (can_controller_t *)calloc(1, sizeof(can_controller_t));
    if (!s) return NULL;
    s->name = instanceName;
    s->log  = logMessage;
    s->env  = instanceEnvironment;
    return (fmi3Instance)s;
}

void fmi3FreeInstance(fmi3Instance instance) { free(instance); }

fmi3Status fmi3EnterInitializationMode(fmi3Instance instance, fmi3Boolean toleranceDefined,
    fmi3Float64 tolerance, fmi3Float64 startTime, fmi3Boolean stopTimeDefined, fmi3Float64 stopTime)
{
    (void)instance; (void)toleranceDefined; (void)tolerance; (void)startTime;
    (void)stopTimeDefined; (void)stopTime;
    return fmi3OK;
}

fmi3Status fmi3ExitInitializationMode(fmi3Instance instance) { (void)instance; return fmi3OK; }
fmi3Status fmi3EnterEventMode(fmi3Instance instance)         { (void)instance; return fmi3OK; }
fmi3Status fmi3EnterStepMode(fmi3Instance instance)          { (void)instance; return fmi3OK; }
fmi3Status fmi3Terminate(fmi3Instance instance)              { (void)instance; return fmi3OK; }

/* ------------------------------------------------------------------ */
/* Input clock + binary: stage the incoming transmit request          */
/* ------------------------------------------------------------------ */

fmi3Status fmi3SetClock(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, const fmi3Clock values[])
{
    can_controller_t *s = (can_controller_t *)instance;
    for (size_t i = 0; i < nvr; ++i)
        if (vr[i] == VR_RX_CLOCK && values[i] == fmi3ClockActive)
            s->rx_clock_active = fmi3True;
    return fmi3OK;
}

fmi3Status fmi3SetBinary(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, const size_t sizes[], const fmi3Binary values[], size_t nValues)
{
    can_controller_t *s = (can_controller_t *)instance;
    (void)nValues;
    for (size_t i = 0; i < nvr; ++i) {
        if (vr[i] == VR_RX_DATA) {
            size_t len = sizes[i] < BUF_SIZE ? sizes[i] : BUF_SIZE;
            memcpy(s->rx_buffer, values[i], len);
            s->rx_len = len;
        }
    }
    return fmi3OK;
}

/* ------------------------------------------------------------------ */
/* Event handling: enqueue the staged request, then pop one to emit   */
/* (one cycle holds the frame; the DoStep in between is time passing). */
/* ------------------------------------------------------------------ */

fmi3Status fmi3UpdateDiscreteStates(fmi3Instance instance,
    fmi3Boolean *discreteStatesNeedUpdate, fmi3Boolean *terminateSimulation,
    fmi3Boolean *nominalsChanged, fmi3Boolean *valuesChanged,
    fmi3Boolean *nextEventTimeDefined, fmi3Float64 *nextEventTime)
{
    can_controller_t *s = (can_controller_t *)instance;

    /* Enqueue the staged transmit request (if Rx_Clock ticked). */
    if (s->rx_clock_active && s->rx_len > 0 && s->q_count < QUEUE_CAP) {
        memcpy(s->queue[s->q_tail], s->rx_buffer, s->rx_len);
        s->queue_len[s->q_tail] = s->rx_len;
        s->q_tail = (s->q_tail + 1) % QUEUE_CAP;
        s->q_count++;
    }
    s->rx_clock_active = fmi3False;

    /* Pop one queued request to emit on Tx this cycle. */
    s->tx_clock_active = fmi3False;
    if (s->q_count > 0) {
        size_t len = s->queue_len[s->q_head];
        memcpy(s->tx_buffer, s->queue[s->q_head], len);
        s->tx_len = len;
        s->q_head = (s->q_head + 1) % QUEUE_CAP;
        s->q_count--;
        s->tx_clock_active = fmi3True;
        s->fwd_count += 1u;
    }

    if (discreteStatesNeedUpdate) *discreteStatesNeedUpdate = fmi3False;
    if (terminateSimulation)      *terminateSimulation      = fmi3False;
    if (nominalsChanged)          *nominalsChanged           = fmi3False;
    if (valuesChanged)            *valuesChanged             = fmi3False;
    if (nextEventTimeDefined)     *nextEventTimeDefined      = fmi3False;
    if (nextEventTime)            *nextEventTime             = 0.0;
    return fmi3OK;
}

fmi3Status fmi3DoStep(fmi3Instance instance, fmi3Float64 currentCommunicationPoint,
    fmi3Float64 communicationStepSize, fmi3Boolean noSetFMUStatePriorToCurrentPoint,
    fmi3Boolean *eventHandlingNeeded, fmi3Boolean *terminateSimulation,
    fmi3Boolean *earlyReturn, fmi3Float64 *lastSuccessfulTime)
{
    can_controller_t *s = (can_controller_t *)instance;
    (void)noSetFMUStatePriorToCurrentPoint;
    /* Ask for event mode whenever there is queued work to drain. */
    if (eventHandlingNeeded)  *eventHandlingNeeded  = (s->q_count > 0) ? fmi3True : fmi3False;
    if (terminateSimulation)  *terminateSimulation  = fmi3False;
    if (earlyReturn)          *earlyReturn          = fmi3False;
    if (lastSuccessfulTime)   *lastSuccessfulTime   = currentCommunicationPoint + communicationStepSize;
    return fmi3OK;
}

/* ------------------------------------------------------------------ */
/* Output clock + binary: hand over the emitted frame                 */
/* ------------------------------------------------------------------ */

fmi3Status fmi3GetClock(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, fmi3Clock values[])
{
    can_controller_t *s = (can_controller_t *)instance;
    for (size_t i = 0; i < nvr; ++i) {
        if (vr[i] == VR_TX_CLOCK) {
            values[i] = s->tx_clock_active ? fmi3ClockActive : fmi3ClockInactive;
            s->tx_clock_active = fmi3False;   /* reading consumes the tick */
        } else {
            values[i] = fmi3ClockInactive;
        }
    }
    return fmi3OK;
}

fmi3Status fmi3GetBinary(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, size_t sizes[], fmi3Binary values[], size_t nValues)
{
    can_controller_t *s = (can_controller_t *)instance;
    (void)nValues;
    for (size_t i = 0; i < nvr; ++i) {
        if (vr[i] == VR_TX_DATA) {
            sizes[i]  = s->tx_len;
            values[i] = (fmi3Binary)s->tx_buffer;
        } else {
            sizes[i]  = 0;
            values[i] = NULL;
        }
    }
    return fmi3OK;
}

fmi3Status fmi3GetUInt64(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, fmi3UInt64 values[], size_t nValues)
{
    can_controller_t *s = (can_controller_t *)instance;
    (void)nValues;
    for (size_t i = 0; i < nvr; ++i)
        values[i] = (vr[i] == VR_FWD_COUNT) ? s->fwd_count : 0u;
    return fmi3OK;
}
