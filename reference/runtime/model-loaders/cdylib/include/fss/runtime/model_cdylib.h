/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* fss/runtime/model_cdylib.h — the C/SystemC/Rust cdylib model loader. */
#ifndef FSS_RUNTIME_MODEL_CDYLIB_H
#define FSS_RUNTIME_MODEL_CDYLIB_H

#include "fss/runtime/agent_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Delegates construction to the in-proc transport (the model is an artefact DLL
 * it already knows how to open/get_endpoint/close). */
extern const fss_model_loader_t fss_model_cdylib;

#ifdef __cplusplus
}
#endif
#endif
