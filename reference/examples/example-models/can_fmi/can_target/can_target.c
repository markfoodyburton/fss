/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * can_target — a clean FMI 3.0 / FMI-LS-BUS CAN *receiver* Network FMU.
 *
 * No FSS.  This is an ordinary FMI 3.0 Co-Simulation FMU implementing the
 * FMI-LS-BUS low-cut CAN convention: it receives CAN bus operations as a
 * byte buffer on the `Rx_Data` binary input, clocked by the `Rx_Clock`
 * input clock.  On each clock activation it iterates the operations out of
 * the buffer (FMI3_LS_BUS_READ_NEXT_OPERATION), decodes CanTransmit frames,
 * and exposes the last-received frame + a running count as plain FMI outputs
 * so an importer can observe what arrived.
 *
 * Variables (see modelDescription.xml):
 *   1  Rx_Data   binary  input   (clocked by Rx_Clock)
 *   2  Rx_Clock  clock   input   (triggered)
 *   3  LastId    uint32  output
 *   4  RxCount   uint64  output
 *   5  LastByte0 uint32  output  (first payload byte of the last frame)
 *   6  LastDlc   uint32  output  (payload length of the last frame)
 */

#include "fmi3Functions.h"
#include "fmi3LsBusUtil.h"
#include "fmi3LsBusUtilCan.h"

#include <stdlib.h>
#include <string.h>

#define VR_RX_DATA    1u
#define VR_RX_CLOCK   2u
#define VR_LAST_ID    3u
#define VR_RX_COUNT   4u
#define VR_LAST_BYTE0 5u
#define VR_LAST_DLC   6u

#define RX_BUFFER_SIZE 2048

typedef struct {
    fmi3String              name;
    fmi3LogMessageCallback  log;
    fmi3InstanceEnvironment env;

    /* Rx staging: bytes copied in by fmi3SetBinary, clock raised by
     * fmi3SetClock, decoded during fmi3UpdateDiscreteStates. */
    fmi3UInt8  rx_buffer[RX_BUFFER_SIZE];
    size_t     rx_len;
    fmi3Boolean rx_clock_active;

    /* Observable state. */
    fmi3UInt32 last_id;
    fmi3UInt64 rx_count;
    fmi3UInt32 last_byte0;
    fmi3UInt32 last_dlc;
} can_target_t;

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

    can_target_t *s = (can_target_t *)calloc(1, sizeof(can_target_t));
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
/* Clocked input: stage the buffer, raise the clock, decode on update */
/* ------------------------------------------------------------------ */

fmi3Status fmi3SetBinary(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, const size_t sizes[], const fmi3Binary values[], size_t nValues)
{
    can_target_t *s = (can_target_t *)instance;
    (void)nValues;
    for (size_t i = 0; i < nvr; ++i) {
        if (vr[i] == VR_RX_DATA) {
            size_t n = sizes[i];
            if (n > RX_BUFFER_SIZE) n = RX_BUFFER_SIZE;
            memcpy(s->rx_buffer, values[i], n);
            s->rx_len = n;
        }
    }
    return fmi3OK;
}

fmi3Status fmi3SetClock(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, const fmi3Clock values[])
{
    can_target_t *s = (can_target_t *)instance;
    for (size_t i = 0; i < nvr; ++i) {
        if (vr[i] == VR_RX_CLOCK && values[i] == fmi3ClockActive)
            s->rx_clock_active = fmi3True;
    }
    return fmi3OK;
}

fmi3Status fmi3UpdateDiscreteStates(fmi3Instance instance,
    fmi3Boolean *discreteStatesNeedUpdate, fmi3Boolean *terminateSimulation,
    fmi3Boolean *nominalsChanged, fmi3Boolean *valuesChanged,
    fmi3Boolean *nextEventTimeDefined, fmi3Float64 *nextEventTime)
{
    can_target_t *s = (can_target_t *)instance;

    if (s->rx_clock_active) {
        fmi3LsBusUtilBufferInfo info;
        FMI3_LS_BUS_BUFFER_INFO_INIT(&info, s->rx_buffer, RX_BUFFER_SIZE);
        /* Mark the staged bytes as readable content. */
        info.writePos = s->rx_buffer + s->rx_len;

        fmi3LsBusOperationHeader *op;
        while (FMI3_LS_BUS_READ_NEXT_OPERATION(&info, op)) {
            if (op->opCode == FMI3_LS_BUS_CAN_OP_CAN_TRANSMIT) {
                fmi3LsBusCanOperationCanTransmit *tx =
                    (fmi3LsBusCanOperationCanTransmit *)op;
                s->last_id    = tx->id;
                s->last_dlc   = tx->dataLength;
                s->last_byte0 = (tx->dataLength > 0) ? tx->data[0] : 0u;
                s->rx_count  += 1u;
            }
        }
        s->rx_clock_active = fmi3False;
        s->rx_len = 0;
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
    (void)instance; (void)noSetFMUStatePriorToCurrentPoint;
    if (eventHandlingNeeded)  *eventHandlingNeeded  = fmi3False;
    if (terminateSimulation)  *terminateSimulation  = fmi3False;
    if (earlyReturn)          *earlyReturn          = fmi3False;
    if (lastSuccessfulTime)   *lastSuccessfulTime   = currentCommunicationPoint + communicationStepSize;
    return fmi3OK;
}

/* ------------------------------------------------------------------ */
/* Observable outputs                                                 */
/* ------------------------------------------------------------------ */

fmi3Status fmi3GetUInt32(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, fmi3UInt32 values[], size_t nValues)
{
    can_target_t *s = (can_target_t *)instance;
    (void)nValues;
    for (size_t i = 0; i < nvr; ++i) {
        switch (vr[i]) {
            case VR_LAST_ID:    values[i] = s->last_id;    break;
            case VR_LAST_BYTE0: values[i] = s->last_byte0; break;
            case VR_LAST_DLC:   values[i] = s->last_dlc;   break;
            default:            values[i] = 0u;            break;
        }
    }
    return fmi3OK;
}

fmi3Status fmi3GetUInt64(fmi3Instance instance, const fmi3ValueReference vr[],
    size_t nvr, fmi3UInt64 values[], size_t nValues)
{
    can_target_t *s = (can_target_t *)instance;
    (void)nValues;
    for (size_t i = 0; i < nvr; ++i)
        values[i] = (vr[i] == VR_RX_COUNT) ? s->rx_count : 0u;
    return fmi3OK;
}
