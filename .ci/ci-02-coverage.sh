#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

export CC="ccache gcc"

make coverage
