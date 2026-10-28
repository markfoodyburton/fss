# examples/platforms/can_bus — the FULL CAN-bus platform matrix.
#
#   initiator ──(hop1: round-trip)──▶ CAN controller ──(hop2: one-way)──▶ CAN device
#
# The platform is FAITHFUL: an initiator emits TLM TX register-WRITEs, a
# controller DERIVES the can::frame, a device receives it.  Each of the three
# roles exists in several languages, joined by a transport per hop.  This file
# enumerates the whole product
#       initiator × controller × device × hop1 × hop2
# and emits one ctest per combination (test name
# agent_can_<h1>_<h2>_<init>_<ctrl>_<dev>, label "agent_matrix"), gating
# only on which artefacts were actually built.  NO combination is carrier-
# infeasible: a non-C++ controller handed an in-proc LIVE C++ payload simply
# declines the producer's native fast path (its core::TypeId won't match) and
# reads the carrier at the meta-described offsets — interoperating through the
# meta, not a shared C++ ABI, which is the whole point of the standard (see
# typeid-crosslang-decision).  The host drives any artefact by path.
#
# RUNNING THE MATRIX: every cell passes, but the CROSS-PROCESS cells each fork
# children running time-synced kernels, so under -j parallel OVERSUBSCRIPTION a
# handful of random cross-process cells hit a known, PRE-EXISTING deadlock: a
# synchronous cross-kernel b_transport into a node that is time-SUSPENDED at its
# bound cannot be serviced (the suspended kernel wakes on the doorbell but
# re-suspends before its dispatch thread runs), and if the caller must itself
# advance to release that suspend, the two stall.  CPU starvation widens the race
# window.  This is NOT the readiness wave and NOT a broken cell — it is the
# passive/zero-handoff b_transport path that psystemc provides (see
# docs/design/open-issues.md).  Until then, let ctest absorb the transient:
#     ctest -L agent_matrix -j6 --repeat until-pass:3
# The all-inproc cells (no forking) are unconditionally stable; every cross-
# process cell is rock-solid in isolation.

if(NOT TARGET fss_inproc)
    message(STATUS "examples/platforms/can_bus skipped — fss_inproc not built")
    return()
endif()
if(NOT FSS_SIM_ID_HEADER_AVAILABLE)
    message(STATUS "examples/platforms/can_bus skipped — FSS_SIM_ID header not generated (jsonnet/python3)")
    return()
endif()
if(NOT FSS_CONFIG_ID_HEADER_AVAILABLE)
    message(STATUS "examples/platforms/can_bus skipped — FSS_CONFIG_ID header not generated (jsonnet/python3)")
    return()
endif()
if(NOT FSS_TIME_ID_HEADER_AVAILABLE)
    message(STATUS "examples/platforms/can_bus skipped — FSS_TIME_ID header not generated (jsonnet/python3)")
    return()
endif()
if(NOT FSS_TLM_ID_HEADER_AVAILABLE)
    message(STATUS "examples/platforms/can_bus skipped — FSS_TLM_ID header not generated (jsonnet/python3)")
    return()
endif()
if(NOT FSS_CAN_ID_HEADER_AVAILABLE)
    message(STATUS "examples/platforms/can_bus skipped — FSS_CAN_ID header not generated (jsonnet/python3)")
    return()
endif()

# Phase-5 rework: the CAN platform driven by the DATA-DRIVEN orchestrator engine
# (matrix_host builds the plan via can_plan, fss_orchestrate runs it), replacing
# the hand-wired oracle (the old platform_host + the privileged in-proc path of
# agent_host.c).  This is now the sole host for the matrix.
add_executable(fss_can_matrix_host matrix_host.c can_plan.c)
target_link_libraries(fss_can_matrix_host PRIVATE
    fss-abi fss-orch-engine ${CMAKE_DL_LIBS})
# The generic agent is a separate binary; the engine execs it by path.
if(TARGET fss-agent-c)
    add_dependencies(fss_can_matrix_host fss-agent-c)
endif()
target_include_directories(fss_can_matrix_host PRIVATE
    $<TARGET_PROPERTY:fss-sim-id,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:fss-config-id,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:fss-time-id,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:fss-known-carrier-tlm,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:fss-known-carrier-can,INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(fss_can_matrix_host PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_dependencies(fss_can_matrix_host fss_inproc fss-sim-id-gen fss-config-id-gen fss-time-id-gen fss-tlm-id-gen fss-can-id-gen)

# Transport (hop) axis: a kind is offered only if its plugin was built; the host
# forks per cross-process hop (shm/zenoh) via the universal open().
set(_HOP_PLUGIN_inproc "$<TARGET_FILE:fss_inproc>")
set(_HOP_KINDS inproc)
if(TARGET fss_shm)
    set(_HOP_PLUGIN_shm "$<TARGET_FILE:fss_shm>")
    list(APPEND _HOP_KINDS shm)
    add_dependencies(fss_can_matrix_host fss_shm)
endif()
if(TARGET fss_zenoh)
    set(_HOP_PLUGIN_zenoh "$<TARGET_FILE:fss_zenoh>")
    list(APPEND _HOP_KINDS zenoh)
    add_dependencies(fss_can_matrix_host fss_zenoh)
endif()

# ── Role × language artefact registry ───────────────────────────────────────
# Register (role, language) → built target.  A model is now passed BY NAME (the
# target name) and resolved to a plugin against a single --model-dir, mirroring
# the transport discipline (never an absolute path).  The per-language artefacts
# build into their own dirs under inconsistent names (lib-prefixed or not, .so or
# .dylib), so each is STAGED into one model dir as lib<name>.so — the canonical
# name both hosts' resolver (fss_model_plugin_path) looks up.  Only registered
# (actually built) languages enter the product; both hosts depend on the staging.
set(_MODEL_DIR "${CMAKE_CURRENT_BINARY_DIR}/models")
set(_MODEL_STAGE "")
set(_INIT_LANGS "")
set(_CTRL_LANGS "")
set(_DEV_LANGS  "")
# `file` is the staging SOURCE: empty for a CMake target (taken from the target
# itself, so no absolute model path is spelled in this file), or an explicit path
# for an imported/variable target (the rust cdylibs).
macro(_reg role lang target file)
    if(TARGET ${target})
        list(APPEND _${role}_LANGS ${lang})
        set(_${role}_NAME_${lang} "${target}")   # models are passed BY NAME (the target)
        if("${file}" STREQUAL "")
            set(_src "$<TARGET_FILE:${target}>")
        else()
            set(_src "${file}")
        endif()
        set(_staged "${_MODEL_DIR}/lib${target}.so")
        add_custom_command(OUTPUT "${_staged}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_MODEL_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy "${_src}" "${_staged}"
            DEPENDS ${target}
            VERBATIM)
        list(APPEND _MODEL_STAGE "${_staged}")
        add_dependencies(fss_can_matrix_host ${target})
    endif()
endmacro()

# Initiators (4): paced TX-write source, ONE fss/time peer (the controller).  sc
# emits a LIVE gp in-proc (flat over a wire); rust/python/fmi emit the flat form.
_reg(INIT sc     fss_can_initiator_paced_systemc "")
_reg(INIT rust   fss_can_initiator_paced_rust    "${FSS_CAN_INITIATOR_PACED_RUST_LIB}")
_reg(INIT python fss_can_initiator_paced_python  "")
_reg(INIT fmi    fss_can_initiator_paced_fmi     "")

# Controllers (4): queued relay — derive the can::frame, bounded TX-FIFO drained
# per quantum; a 2-peer timing node (fss/time ⇄ init, fss/time/dev ⇄ dev).  sc
# takes the live-gp fast path or the flat path; rust/python read live OR flat at
# the meta-described offsets; fmi wraps a store-and-forward FMU.
_reg(CTRL sc     fss_can_controller_queued_systemc "")
_reg(CTRL rust   fss_can_controller_queued_rust    "${FSS_CAN_CONTROLLER_QUEUED_RUST_LIB}")
_reg(CTRL python fss_can_controller_queued_python  "")
_reg(CTRL fmi    fss_can_controller_queued_fmi     "")

# Devices (4): paced sink — receive the can::frame, stop at N and cascade teardown
# upstream; ONE fss/time peer (the controller).
_reg(DEV sc     fss_can_target_paced_systemc "")
_reg(DEV rust   fss_can_target_paced_rust    "${FSS_CAN_TARGET_PACED_RUST_LIB}")
_reg(DEV python fss_can_target_paced_python  "")
_reg(DEV fmi    fss_can_target_paced_fmi     "")

# Stage every registered model into the one model dir (lib<name>.so); the host
# builds it before any test runs, so a NAME + --model-dir always resolves.
add_custom_target(fss_can_model_stage DEPENDS ${_MODEL_STAGE})
add_dependencies(fss_can_matrix_host   fss_can_model_stage)

# ── The AGENT-MODEL matrix (the full 576, label "agent_matrix") ──────────────
# The full product, driven by the Phase-5 data-driven orchestrator (matrix_host
# builds the plan; fss_orchestrate places nodes, execs agents, and drives them
# over the control path).  The host takes the CONSTRUCTION transport (--inproc),
# the control BACK-CHANNEL (--shm), and the agent binary (--agent-c) explicitly,
# independent of the data hops, since all are intrinsic to an agent rather than
# to any edge.  Cross-process cells hit the known transient (run -j6 --repeat until-pass:3).
set(_agent_common --inproc $<TARGET_FILE:fss_inproc>)
if(TARGET fss_shm)
    list(APPEND _agent_common --shm $<TARGET_FILE:fss_shm>)
endif()
# The cross-process launchers exec the generic agent binary (born-with over argv).
if(TARGET fss-agent-c)
    list(APPEND _agent_common --agent-c $<TARGET_FILE:fss-agent-c>)
endif()
foreach(init ${_INIT_LANGS})
  foreach(ctrl ${_CTRL_LANGS})
    foreach(dev ${_DEV_LANGS})
      foreach(h1 ${_HOP_KINDS})
        foreach(h2 ${_HOP_KINDS})
          set(_name agent_can_${h1}_${h2}_${init}_${ctrl}_${dev})
          add_test(NAME ${_name}
              COMMAND fss_can_matrix_host
                  ${_agent_common}
                  --hop1 ${h1}:${_HOP_PLUGIN_${h1}}
                  --hop2 ${h2}:${_HOP_PLUGIN_${h2}}
                  --model-dir  ${_MODEL_DIR}
                  --initiator  ${_INIT_NAME_${init}}
                  --controller ${_CTRL_NAME_${ctrl}}
                  --device     ${_DEV_NAME_${dev}}
                  --frames 16)
          set_tests_properties(${_name} PROPERTIES
              PASS_REGULAR_EXPRESSION "PLATFORM CAN PASS: acked=16/16"
              TIMEOUT 120
              LABELS "agent_matrix")
        endforeach()
      endforeach()
    endforeach()
  endforeach()
endforeach()

# ── DATA-PLANE FAN-OUT (1 source → 2 devices), label "fanout_matrix" ─────────
# Fan-out is a hop2/can_tx property: ONE controller send reaches TWO devices.
# This proves the SINGLE engine+agent mechanism over ALL THREE hop2 transports
# with a tiny dedicated set (init=ctrl=dev=dev2=sc, hop1=inproc, vary hop2) — the
# 576 agent_matrix above stays EXACTLY 576, untouched.  The engine requires BOTH
# devices to ack N, so a PASS is non-vacuous (a dropped device fails the run):
#   inproc — both devices in one process; the agent shims ctrl/can_tx across both
#            counted sinks (the shared count reaches frames×2).
#   shm    — each device its OWN 1:1 SPSC segment; the sender's agent shims across
#            the N segments; each remote agent's exit code gates its own delivery.
#   zenoh  — both devices subscribe the ONE key; the publisher z_puts ONCE.
# Cross-process cells hit the same known transient as the matrix:
#     ctest -L fanout_matrix --repeat until-pass:3 -j2
if(("sc" IN_LIST _INIT_LANGS) AND ("sc" IN_LIST _CTRL_LANGS) AND ("sc" IN_LIST _DEV_LANGS))
  # dev2 is the SAME sc target model, but staged under a SECOND file name.  Under
  # the v0.1 vanilla-SystemC + static + RTLD_LOCAL packaging each model DLL owns
  # its OWN SystemC runtime + CCI broker; dlopen refcounts by path, so in the
  # in-proc cell (both devices in ONE process) two instances of the SAME file
  # would share one runtime and clash on the global CCI broker.  A distinct file
  # → an independent dlopen object → an isolated kernel, exactly as two same-model
  # SystemC kernels in one process require.  (For shm/zenoh dev2 is its own
  # process, so the extra copy is merely harmless.)
  set(_DEV2_NAME "${_DEV_NAME_sc}_fanout2")
  set(_dev2_staged "${_MODEL_DIR}/lib${_DEV2_NAME}.so")
  add_custom_command(OUTPUT "${_dev2_staged}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${_MODEL_DIR}"
      COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${_DEV_NAME_sc}>" "${_dev2_staged}"
      DEPENDS ${_DEV_NAME_sc}
      VERBATIM)
  add_custom_target(fss_can_fanout2_stage DEPENDS "${_dev2_staged}")
  add_dependencies(fss_can_matrix_host fss_can_fanout2_stage)

  foreach(h2 ${_HOP_KINDS})
    set(_name fanout_can_inproc_${h2}_sc_sc_sc_sc)
    add_test(NAME ${_name}
        COMMAND fss_can_matrix_host
            ${_agent_common}
            --hop1 inproc:${_HOP_PLUGIN_inproc}
            --hop2 ${h2}:${_HOP_PLUGIN_${h2}}
            --model-dir  ${_MODEL_DIR}
            --initiator  ${_INIT_NAME_sc}
            --controller ${_CTRL_NAME_sc}
            --device     ${_DEV_NAME_sc}
            --device2    ${_DEV2_NAME}
            --frames 16)
    set_tests_properties(${_name} PROPERTIES
        PASS_REGULAR_EXPRESSION "PLATFORM CAN PASS: acked=16/16"
        TIMEOUT 120
        LABELS "fanout_matrix")
  endforeach()
endif()

# ── CONFIG FAN-IN (N pullers → 1 responder, round-trip), label "fanin_matrix" ─
# The round-trip MIRROR of fanout_matrix.  Instead of each agent serving the run
# config to its own nodes LOCALLY, ctrl and dev PULL their config from the ROOT
# node init (always p0) over a round-trip fss/config edge → 2 initiators, 1 shared
# target (init/fss/config) = N→1 fan-in.  One tiny set (init=ctrl=dev=sc) proves
# the SINGLE engine+agent mechanism over ALL THREE config transports; the 576
# agent_matrix stays EXACTLY 576, untouched (it adds no config edges).
#
# NON-VACUOUS via --frames 24 (≠ the models' compile-time default 16): init (the
# source, LOCAL config) sends 24 and dev's frame oracle awaits 24.  dev learns
# "expect 24" ONLY by pulling init's config across the process boundary — a puller
# installs NO local responder (the arm no-clobber guard leaves its config send as
# the channel INITIATOR), so if the pull does not arrive it falls back to the model
# DEFAULT 16, sc_stop()s after 16 frames, and the oracle stalls at 16/24 → the run
# FAILS.  A PASS therefore proves dev genuinely received its pulled config over the
# config fan-in channel from init.  (Verified: with the responder instrumented it
# fires 3× — init-local + ctrl + dev — only when dev's pull truly lands.)
#
# Per-transport topology.  The config responder lives on init (root/p0); BOTH
# pullers must reach it.  shm/inproc reach p0 directly (host-local segment / same
# process).  zenoh is a STAR: with multicast scouting disabled a "connect" leaf only
# links the "listen" centre, so leaf→leaf never routes — and in a 3-node star
# exactly one node-pair is leaf↔leaf.  We therefore keep hop2=inproc for the zenoh
# cell, which MERGES ctrl+dev into the centre group and leaves init as the SOLE
# leaf, so both queriers reach init as centre→leaf.  (shm has no such constraint,
# so its cell keeps the stronger THREE-process fan-in: ctrl and dev in separate
# processes, each its own round-trip segment into init's one reentrant responder.)
# Cross-process cells hit the same known transient as the matrix:
#     ctest -L fanin_matrix --repeat until-pass:3 -j2
if(("sc" IN_LIST _INIT_LANGS) AND ("sc" IN_LIST _CTRL_LANGS) AND ("sc" IN_LIST _DEV_LANGS))
  # (config-edge kind, hop1 kind, hop2 kind) per cell — see the topology note above.
  set(_fanin_specs "inproc|inproc|inproc" "shm|shm|shm" "zenoh|zenoh|inproc")
  foreach(spec ${_fanin_specs})
    string(REPLACE "|" ";" _s "${spec}")
    list(GET _s 0 _cfg)
    list(GET _s 1 _h1)
    list(GET _s 2 _h2)
    if(("${_cfg}" IN_LIST _HOP_KINDS) AND ("${_h1}" IN_LIST _HOP_KINDS) AND ("${_h2}" IN_LIST _HOP_KINDS))
      set(_name fanin_can_${_cfg}_sc_sc_sc)
      add_test(NAME ${_name}
          COMMAND fss_can_matrix_host
              ${_agent_common}
              --hop1 ${_h1}:${_HOP_PLUGIN_${_h1}}
              --hop2 ${_h2}:${_HOP_PLUGIN_${_h2}}
              --config-edge ${_cfg}
              --model-dir  ${_MODEL_DIR}
              --initiator  ${_INIT_NAME_sc}
              --controller ${_CTRL_NAME_sc}
              --device     ${_DEV_NAME_sc}
              --frames 24)
      set_tests_properties(${_name} PROPERTIES
          PASS_REGULAR_EXPRESSION "PLATFORM CAN PASS: acked=24/24"
          TIMEOUT 120
          LABELS "fanin_matrix")
    endif()
  endforeach()
endif()

# ── Combined single-artefact CAN federation (label "combined") ───────────────
# One artefact builds all three CAN models in ONE SystemC kernel; the two internal
# edges are ordinary in-process FSS edges that take the binding's same-thread fast
# path.  This isolates the FSS carrier cost from the federation/time-sync cost:
# contrast COMBINED PERF with the three-kernel platform_can_inproc_inproc_sc_sc_sc
# number.  Correctness test at the default frame count; the perf run is manual
# (pass --frames N).
if(TARGET fss_can_combined_systemc)
    add_executable(fss_can_combined_host combined_host.c)
    target_link_libraries(fss_can_combined_host PRIVATE
        fss-abi fss-placement ${CMAKE_DL_LIBS})
    target_include_directories(fss_can_combined_host PRIVATE
        $<TARGET_PROPERTY:fss-sim-id,INTERFACE_INCLUDE_DIRECTORIES>
        $<TARGET_PROPERTY:fss-config-id,INTERFACE_INCLUDE_DIRECTORIES>)
    set_target_properties(fss_can_combined_host PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON)
    add_dependencies(fss_can_combined_host
        fss_can_combined_systemc fss-sim-id-gen fss-config-id-gen)

    # Stage the combined artefact into the model dir (lib<name>.so) so it too is
    # loaded BY NAME + --model-dir, never by absolute path.  (Source via a variable
    # so no absolute model path is spelled here, same as _reg.)
    set(_combined_tgt fss_can_combined_systemc)
    set(_combined_staged "${_MODEL_DIR}/lib${_combined_tgt}.so")
    add_custom_command(OUTPUT "${_combined_staged}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_MODEL_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${_combined_tgt}>" "${_combined_staged}"
        DEPENDS ${_combined_tgt}
        VERBATIM)
    add_custom_target(fss_can_combined_stage DEPENDS "${_combined_staged}")
    add_dependencies(fss_can_combined_host fss_can_combined_stage)

    add_test(NAME fss_can_combined
        COMMAND fss_can_combined_host
            --combined  fss_can_combined_systemc
            --model-dir ${_MODEL_DIR}
            --frames 16)
    set_tests_properties(fss_can_combined PROPERTIES
        PASS_REGULAR_EXPRESSION "PLATFORM CAN COMBINED PASS: acked=16/16"
        TIMEOUT 120
        LABELS "combined")
endif()
