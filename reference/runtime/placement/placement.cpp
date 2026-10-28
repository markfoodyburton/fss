/*
 * This file is part of fss
 * Copyright (c) Qualcomm Innovation Center, Inc. All Rights Reserved.
 * Author: Mark Burton
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * placement.cpp — process-level artefact placement facility.
 *
 * Lifted verbatim in spirit from transports/inproc/inproc.cpp (node_entry,
 * split_locator, resolve, inproc_open, inproc_close, inproc_get_endpoint).
 * The registry is a function-local static std::map guarded by a std::mutex.
 *
 * Key differences from the old inproc transport code:
 *   - fss_place_construct takes (node, locator) SEPARATELY (not "node=path").
 *   - fss_create is called with NULL as the transport arg (every artefact does
 *     (void)transport — the arg is unused).
 *   - Error messages carry the tag "fss_place:" instead of "fss_inproc:".
 */
#include "fss/runtime/placement.h"

#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace {

typedef fss_node_t     *(*fss_create_fn)      (fss_transport_t *, const char *);
typedef void            (*fss_destroy_fn)     (fss_node_t *);
typedef fss_endpoint_t *(*fss_get_endpoint_fn)(fss_node_t *, const char *);

/* One loaded artefact instance. */
struct node_entry {
    void                *dl              = nullptr;
    fss_create_fn        create_fn       = nullptr;
    fss_destroy_fn       destroy_fn      = nullptr;
    fss_get_endpoint_fn  get_endpoint_fn = nullptr;
    fss_node_t          *node            = nullptr;
};

/* Process-global registry: node-name -> loaded artefact. */
struct placement_registry {
    std::mutex                         mu;
    std::map<std::string, node_entry>  nodes;
};

static placement_registry &registry()
{
    static placement_registry reg;
    return reg;
}

/* Split a locator "<node>/<endpoint>" at the FIRST '/'.  Returns false if
 * there is no '/'.  `endpoint` keeps any further '/' (so "spi/fss/sim" ->
 * node "spi", endpoint "fss/sim"). */
static bool split_locator(const char *locator,
                           std::string &node, std::string &endpoint)
{
    if (!locator) return false;
    const char *slash = std::strchr(locator, '/');
    if (!slash) return false;
    node.assign(locator, slash - locator);
    endpoint.assign(slash + 1);
    return true;
}

} // namespace

extern "C" {

fss_node_t *fss_place_construct(const char *node_name, const char *locator)
{
    if (!node_name || !locator) return nullptr;

    auto &reg = registry();
    std::lock_guard<std::mutex> lk(reg.mu);

    if (reg.nodes.count(node_name)) {
        std::fprintf(stderr, "fss_place: construct: node '%s' already registered\n",
                     node_name);
        return nullptr;
    }

    node_entry e;
    e.dl = dlopen(locator, RTLD_NOW | RTLD_LOCAL);
    if (!e.dl) {
        std::fprintf(stderr, "fss_place: construct: dlopen %s: %s\n",
                     locator, dlerror());
        return nullptr;
    }
    e.create_fn       = reinterpret_cast<fss_create_fn>      (dlsym(e.dl, "fss_create"));
    e.destroy_fn      = reinterpret_cast<fss_destroy_fn>     (dlsym(e.dl, "fss_destroy"));
    e.get_endpoint_fn = reinterpret_cast<fss_get_endpoint_fn>(dlsym(e.dl, "fss_get_endpoint"));
    if (!e.create_fn || !e.destroy_fn || !e.get_endpoint_fn) {
        std::fprintf(stderr, "fss_place: construct: dlsym on %s: %s\n",
                     locator, dlerror());
        dlclose(e.dl);
        return nullptr;
    }

    /* Pass NULL as the transport arg: every artefact does (void)transport. */
    e.node = e.create_fn(nullptr, node_name);
    if (!e.node) {
        std::fprintf(stderr, "fss_place: construct: fss_create failed for '%s'\n",
                     node_name);
        dlclose(e.dl);
        return nullptr;
    }
    reg.nodes.emplace(node_name, e);
    return e.node;
}

fss_endpoint_t *fss_place_endpoint(fss_node_t *node, const char *ep)
{
    if (!node || !ep) return nullptr;
    auto &reg = registry();
    std::lock_guard<std::mutex> lk(reg.mu);
    for (auto &kv : reg.nodes) {
        if (kv.second.node == node)
            return kv.second.get_endpoint_fn(node, ep);
    }
    return nullptr;
}

fss_endpoint_t *fss_place_resolve(const char *node_slash_ep)
{
    std::string node_str, ep_str;
    if (!split_locator(node_slash_ep, node_str, ep_str)) return nullptr;
    auto &reg = registry();
    std::lock_guard<std::mutex> lk(reg.mu);
    auto it = reg.nodes.find(node_str);
    if (it == reg.nodes.end()) return nullptr;
    return it->second.get_endpoint_fn(it->second.node, ep_str.c_str());
}

void fss_place_destroy(fss_node_t *node)
{
    if (!node) return;
    auto &reg = registry();
    std::lock_guard<std::mutex> lk(reg.mu);
    for (auto it = reg.nodes.begin(); it != reg.nodes.end(); ++it) {
        if (it->second.node == node) {
            it->second.destroy_fn(node);
            if (it->second.dl) dlclose(it->second.dl);
            reg.nodes.erase(it);
            return;
        }
    }
}

} // extern "C"
