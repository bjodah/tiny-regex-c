#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

# tests/test_api.c runs two executions of one compiled pattern, and of two
# different ones, on two threads. Nothing in the matcher may be shared
# between them; ThreadSanitizer is what says so.
export CC="ccache clang"
export CFLAGS="-Werror -Wall -Wextra -pedantic -std=c2x -fsanitize=thread -fno-omit-frame-pointer -O1 -g"
export TSAN_OPTIONS="halt_on_error=1:exitcode=1"

"${MAKE_PARALLEL[@]}" -B tests/test_api
./tests/test_api
