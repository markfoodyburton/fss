/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_initiator — a clean FMI 3.0 / FMI-LS-BUS CAN *sender* Network FMU.
 *
 * No FSS.  An ordinary FMI 3.0 Co-Simulation FMU implementing the
 * FMI-LS-BUS low-cut CAN convention on the sending side: it emits one CAN
 * frame per tick of its periodic `Tx_Clock` output clock, serialised as a
 * CanTransmit bus operation into the `Tx_Data` binary output buffer using
 * the LS-BUS utility macros.
 *
 * The frame carries a fixed CAN id and an 8-byte payload whose first byte is
 * an incrementing counter, so a receiver/importer can verify exactly what
 * was sent.
 *
 * Variables (see modelDescription.xml):
 *   1  Tx_Data   binary  output  (clocked by Tx_Clock)
 *   2  Tx_Clock  clock   output  (periodic, time-based)
 *   3  TxCount   uint64  output  (frames emitted so far)
 */

#include "fmi3Functions.h"
#include "fmi3LsBusUtil.h"
#include "fmi3LsBusUtilCan.h"

#include <stdlib.h>
#include <string.h>

#define VR_TX_DATA   1u
#define VR_TX_CLOCK  2u
#define VR_TX_COUNT  3u

#define TX_BUFFER_SIZE  2048
#define CAN_FRAME_ID    0x100u
#define CAN_PAYLOAD_LEN 8u
#define TX_PERIOD       0.001  /* seconds between frames */

typedef struct {
    fmi3String              name;
    fmi3LogMessageCallback  log;
    fmi3InstanceEnvironment env;

    fmi3UInt8   tx_buffer[TX_BUFFER_SIZE];
    size_t      tx_len;
    fmi3Boolean tx_clock_active;
    fmi3UInt64  tx_count;
} can_initiator_t;

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

    can_initiator_t *s = (can_initiator_t *)calloc(1, sizeof(can_initiator_t));
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
/* Periodic Tx_Clock: announce its interval to the importer           */
/* ------------------------------------------------------------------ */

fmi3Status fmi3GetIntervalDecimal(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, fmi3Float64 intervals[], fmi3IntervalQualifier qualifiers[])
{
    (void)instance;
    for (size_t i = 0; i < nvr; ++i) {
        if (vr[i] == VR_TX_CLOCK) {
            intervals[i]  = TX_PERIOD;
            qualifiers[i] = fmi3IntervalChanged;
        } else {
            intervals[i]  = 0.0;
            qualifiers[i] = fmi3IntervalNotYetKnown;
        }
    }
    return fmi3OK;
}

/* ------------------------------------------------------------------ */
/* Event handling: build the next frame on each Tx_Clock tick         */
/* ------------------------------------------------------------------ */

fmi3Status fmi3UpdateDiscreteStates(fmi3Instance instance,
    fmi3Boolean *discreteStatesNeedUpdate, fmi3Boolean *terminateSimulation,
    fmi3Boolean *nominalsChanged, fmi3Boolean *valuesChanged,
    fmi3Boolean *nextEventTimeDefined, fmi3Float64 *nextEventTime)
{
    can_initiator_t *s = (can_initiator_t *)instance;

    /* One Tx_Clock tick per event-mode entry: serialise a CanTransmit op. */
    fmi3UInt8 payload[CAN_PAYLOAD_LEN];
    payload[0] = (fmi3UInt8)(s->tx_count & 0xFFu);  /* incrementing marker */
    for (unsigned b = 1; b < CAN_PAYLOAD_LEN; ++b) payload[b] = (fmi3UInt8)b;

    fmi3LsBusUtilBufferInfo info;
    FMI3_LS_BUS_BUFFER_INFO_INIT(&info, s->tx_buffer, TX_BUFFER_SIZE);
    FMI3_LS_BUS_CAN_CREATE_OP_CAN_TRANSMIT(&info, CAN_FRAME_ID,
        /*ide=*/0, /*rtr=*/0, CAN_PAYLOAD_LEN, payload);

    s->tx_len         = (size_t)FMI3_LS_BUS_BUFFER_LENGTH(&info);
    s->tx_clock_active = fmi3True;
    s->tx_count      += 1u;

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
    (void)instance; (void)noSetFMUStatePriorToCurrentPoint;
    /* The periodic Tx_Clock ticks every step; ask the importer for event mode. */
    if (eventHandlingNeeded)  *eventHandlingNeeded  = fmi3True;
    if (terminateSimulation)  *terminateSimulation  = fmi3False;
    if (earlyReturn)          *earlyReturn          = fmi3False;
    if (lastSuccessfulTime)   *lastSuccessfulTime   = currentCommunicationPoint + communicationStepSize;
    return fmi3OK;
}

/* ------------------------------------------------------------------ */
/* Clocked output: report activation + hand over the serialised frame */
/* ------------------------------------------------------------------ */

fmi3Status fmi3GetClock(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, fmi3Clock values[])
{
    can_initiator_t *s = (can_initiator_t *)instance;
    for (size_t i = 0; i < nvr; ++i) {
        if (vr[i] == VR_TX_CLOCK) {
            values[i] = s->tx_clock_active ? fmi3ClockActive : fmi3ClockInactive;
            s->tx_clock_active = fmi3False;  /* reading consumes the tick */
        } else {
            values[i] = fmi3ClockInactive;
        }
    }
    return fmi3OK;
}

fmi3Status fmi3GetBinary(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, size_t sizes[], fmi3Binary values[], size_t nValues)
{
    can_initiator_t *s = (can_initiator_t *)instance;
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
    can_initiator_t *s = (can_initiator_t *)instance;
    (void)nValues;
    for (size_t i = 0; i < nvr; ++i)
        values[i] = (vr[i] == VR_TX_COUNT) ? s->tx_count : 0u;
    return fmi3OK;
}
