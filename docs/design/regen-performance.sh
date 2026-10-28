#!/usr/bin/env bash
# regen-performance.sh — regenerate the numbers in docs/design/performance.md.
#
# Usage:
#   ./docs/design/regen-performance.sh [BUILD_DIR]
#
# BUILD_DIR defaults to ./build.  Override via $1 or the BUILD_DIR environment
# variable (the argument takes precedence if given).
#
# What this runs:
#   1. transport_bench inproc  — 1 000 000 frames; reports ns/send for inproc.
#   2. transport_bench shm     — 200 000 frames; reports ns/send for shm.
#   3. ctest -L platform_matrix -j6 with --output-on-failure; aggregates
#      mean test time by hop1×hop2 and by each language axis (init/ctrl/dev).
#
# Artefact locations assumed:
#   transport_bench:   <BUILD_DIR>/examples/tools/transport_bench/transport_bench
#   libfss_inproc.*:   somewhere under <BUILD_DIR>/
#   libfss_minimal_echo.*: somewhere under <BUILD_DIR>/
#   libfss_shm.*:      somewhere under <BUILD_DIR>/
#
# Test name pattern: platform_can_<hop1>_<hop2>_<init>_<ctrl>_<dev>
# Test label:        platform_matrix

set -euo pipefail

BUILD_DIR="${1:-${BUILD_DIR:-./build}}"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "ERROR: build dir not found: ${BUILD_DIR}" >&2
    exit 1
fi

BENCH="${BUILD_DIR}/examples/tools/transport_bench/transport_bench"
if [[ ! -x "${BENCH}" ]]; then
    echo "ERROR: transport_bench not found or not executable: ${BENCH}" >&2
    exit 1
fi

# Locate modules under the build tree.
find_module() {
    local name="$1"
    local result
    result=$(find "${BUILD_DIR}" -name "${name}.so" -o -name "${name}.dylib" 2>/dev/null | head -1)
    if [[ -z "${result}" ]]; then
        echo "ERROR: module ${name} not found under ${BUILD_DIR}" >&2
        exit 1
    fi
    echo "${result}"
}

INPROC_SO=$(find_module "libfss_inproc")
ECHO_SO=$(find_module "libfss_minimal_echo")
SHM_SO=$(find_module "libfss_shm")

echo "=== transport_bench: inproc (N=1000000) ==="
"${BENCH}" inproc "${INPROC_SO}" "${ECHO_SO}" 1000000

echo ""
echo "=== transport_bench: shm (N=200000) ==="
"${BENCH}" shm "${SHM_SO}" 200000

echo ""
echo "=== platform_matrix ctest aggregation ==="

# Run ctest, capture per-test timing lines.
# ctest prints: "X/Y Test #N: <name> ..... Passed  <secs> sec"
CTEST_OUTPUT=$(mktemp)
trap 'rm -f "${CTEST_OUTPUT}"' EXIT

(
    cd "${BUILD_DIR}"
    ctest -L platform_matrix -j6 --output-on-failure 2>&1
) | tee "${CTEST_OUTPUT}"

# Python aggregation: parse "Passed N.NN sec" lines, split test name by '_',
# aggregate by hop1×hop2 and by each language axis.
python3 - "${CTEST_OUTPUT}" <<'PYEOF'
import sys, re, collections

path = sys.argv[1]
# Match lines like:
#   N/M Test #K: platform_can_<h1>_<h2>_<init>_<ctrl>_<dev> ... Passed  1.23 sec
pattern = re.compile(
    r'platform_can_(\w+)_(\w+)_(\w+)_(\w+)_(\w+)\s+\.+\s+Passed\s+([\d.]+)\s+sec'
)

hop_times   = collections.defaultdict(list)
init_times  = collections.defaultdict(list)
ctrl_times  = collections.defaultdict(list)
dev_times   = collections.defaultdict(list)

with open(path) as f:
    for line in f:
        m = pattern.search(line)
        if not m:
            continue
        h1, h2, init, ctrl, dev, t = m.groups()
        t = float(t)
        hop_times[(h1, h2)].append(t)
        init_times[init].append(t)
        ctrl_times[ctrl].append(t)
        dev_times[dev].append(t)

if not hop_times:
    print("WARNING: no matching test results found in ctest output.")
    sys.exit(0)

print("\n--- Mean test time by hop1 x hop2 ---")
print(f"{'hop1 x hop2':<22}  {'mean (s)':>9}  {'n':>5}")
for (h1, h2), times in sorted(hop_times.items()):
    mean = sum(times) / len(times)
    print(f"{h1+' x '+h2:<22}  {mean:9.3f}  {len(times):>5}")

print("\n--- Mean test time by initiator language ---")
for lang, times in sorted(init_times.items()):
    print(f"  init={lang:<12}  mean={sum(times)/len(times):.3f} s  n={len(times)}")

print("\n--- Mean test time by controller language ---")
for lang, times in sorted(ctrl_times.items()):
    print(f"  ctrl={lang:<12}  mean={sum(times)/len(times):.3f} s  n={len(times)}")

print("\n--- Mean test time by device language ---")
for lang, times in sorted(dev_times.items()):
    print(f"  dev= {lang:<12}  mean={sum(times)/len(times):.3f} s  n={len(times)}")

total = sum(t for ts in hop_times.values() for t in ts)
count = sum(len(ts) for ts in hop_times.values())
print(f"\nTotal tests parsed: {count}  total CPU-s: {total:.1f}")
PYEOF
