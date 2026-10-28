#!/usr/bin/env bash
# Builds in Release mode, runs the test suite, then the benchmarks.
# Results land in results/ (CSV) together with a note of the machine used.
#
#   scripts/run_benchmarks.sh [extra bench args, e.g. --n 500000]
#
# Set CPU=<id> to pin the benchmark to one core (default: no pinning).
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)"
./build/test_bplus_tree

mkdir -p results
{
  echo "date:     $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "cpu:      $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "kernel:   $(uname -sr)"
  echo "compiler: $(${CXX:-c++} --version | head -1)"
  echo "pinned:   ${CPU:-no}"
} > results/ENV.txt

if [[ -n "${CPU:-}" ]] && command -v taskset >/dev/null; then
  taskset -c "$CPU" ./build/bench --out results "$@"
else
  ./build/bench --out results "$@"
fi
