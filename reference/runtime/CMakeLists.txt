# runtime/ — the ring-3 FSS runtime: the parts that DRIVE a federation into
# existence (launcher, orchestrator, agents) but are NOT the FSS standard.
#
# The standard fixes what a federation is: endpoints, carriers, transports (the
# four ABIs).  This directory holds a reference implementation of the machinery
# that BUILDS one: an agent per language (a loadable unit that loads its own
# model and its transports by the standard ABI), an orchestrator that holds the
# plan and drives agents over a private control protocol, and a launcher that
# spawns agents and hands them their born-with facts.  None of this is
# normative; a deployment may replace any of it.  It lives here (not in abi/,
# not under examples/) because it is reusable across platforms.
#
# Reference C is compiled per-consumer via INTERFACE + target_sources (the
# fss-lang-c-meta convention), not shipped as a shared library.

add_subdirectory(control-protocol)
add_subdirectory(orchestrator)
add_subdirectory(placement)
add_subdirectory(agent-core)
add_subdirectory(model-loaders/cdylib)
add_subdirectory(launcher)
add_subdirectory(agents/agent_c)

# The data-driven orchestrator engine (fss_orchestrate; orchestrator/engine.c).
# Defined here, AFTER its dependencies, so the INTERFACE links resolve: it drives
# agents into a federation, so it depends on agent-core + launcher, which in turn
# depend on the fss-orchestrator plumbing.  Kept apart from fss-orchestrator (the
# plumbing itself) to avoid a dependency cycle.  engine.h lives in
# orchestrator/include, carried by fss-orchestrator.
add_library(fss-orch-engine INTERFACE)
target_sources(fss-orch-engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/orchestrator/engine.c)
target_link_libraries(fss-orch-engine INTERFACE
    fss-orchestrator fss-agent-core fss-model-cdylib fss-launcher)
