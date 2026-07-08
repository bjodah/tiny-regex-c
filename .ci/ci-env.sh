#!/bin/bash

JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}
PARALLEL=${PARALLEL:-parallel}
VALGRIND=${VALGRIND:-valgrind --quiet --tool=memcheck --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,possible --error-exitcode=1}

MAKE_PARALLEL=(make -j "${JOBS}" --output-sync=target)
