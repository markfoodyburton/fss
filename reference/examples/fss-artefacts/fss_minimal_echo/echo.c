/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * echo.c — minimal FSS artefact plugin in plain C.
 *
 * Exports fss_create / fss_destroy / fss_get_endpoint (fss/artefact_abi.h).
 * The single endpoint "in" receives any carrier, increments the first payload
 * byte, and returns FSS_SEND_OK.  Safe to instantiate many times (no SystemC,
 * no global state).
 */
#include "fss/artefact_abi.h"
#include "fss/endpoint.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    fss_endpoint_t in;
    unsigned       rx_count;
} echo_node;

static int echo_receive(void *self, fss_carrier_t *c)
{
    echo_node *n = (echo_node *)self;
    n->rx_count++;
    if (c && c->data_ptr && c->data_size >= 1)
        ((uint8_t *)c->data_ptr)[0]++;
    return FSS_SEND_OK;
}

FSS_EXPORT fss_node_t *fss_create(fss_transport_t *t, const char *name)
{
    (void)t; (void)name;
    echo_node *n = (echo_node *)calloc(1, sizeof(echo_node));
    if (!n) return NULL;
    n->in.self     = n;
    n->in.receive  = echo_receive;
    n->in.send     = NULL;
    n->in.send_ctx = NULL;
    return (fss_node_t *)n;
}

FSS_EXPORT fss_endpoint_t *fss_get_endpoint(fss_node_t *node, const char *endpoint_name)
{
    if (strcmp(endpoint_name, "in") == 0)
        return &((echo_node *)node)->in;
    return NULL;
}

FSS_EXPORT void fss_destroy(fss_node_t *node)
{
    free(node);
}
