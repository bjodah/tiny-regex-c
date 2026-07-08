#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache gcc"
export CFLAGS="-Wall -Wextra -Werror -pedantic -std=c2x -O0 -g -fanalyzer"
export TEST_RUNNER="${VALGRIND}"
# tests/test2.c's worst-case-backtracking sweep runs up to a 32KB buffer;
# under Valgrind that is minutes per size. Cap it so the interesting (small)
# sizes still run under the interpreter.
export RE_TEST2_MAX_BYTES=2048

"${MAKE_PARALLEL[@]}" -B check
