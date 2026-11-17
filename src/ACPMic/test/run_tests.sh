#!/bin/sh
# Build and run the host tests for ACPMic's core (no kernel, no hardware).
#   ./test/run_tests.sh          quick
#   ./test/run_tests.sh --full   + exhaustive int32->float check (~30 s)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
xcrun clang -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=undefined,address \
    -o build/test_acp_core test/test_acp_core.c src/acp_core.c
./build/test_acp_core "$@"
