#!/usr/bin/env bash
# Linux / WSL / macOS build.   ./build.sh          -> optimised build
#                              ./build.sh tsan     -> tests with ThreadSanitizer
set -euo pipefail
mkdir -p build
FLAGS="-std=c++17 -O2 -Wall -Wextra -pthread -Iinclude"

if [[ "${1:-}" == "tsan" ]]; then
  g++ -std=c++17 -O1 -g -fsanitize=thread -pthread -Iinclude tests/test_main.cpp -o build/tests_tsan
  ./build/tests_tsan
  exit 0
fi

g++ $FLAGS tests/test_main.cpp        -o build/tests
g++ $FLAGS bench/benchmark.cpp        -o build/benchmark
g++ $FLAGS examples/basic_usage.cpp   -o build/basic_usage
g++ $FLAGS examples/deadlock_demo.cpp -o build/deadlock_demo
echo "Build OK. Try: ./build/tests && ./build/benchmark --quick"
