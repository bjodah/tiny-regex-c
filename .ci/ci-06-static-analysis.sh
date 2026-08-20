#!/bin/bash
set -euxo pipefail

cd "$(dirname "$0")/.."
source .ci/ci-env.sh

compile_db_files() {
	python3 - <<'PY'
import json
with open("compile_commands.json", "r", encoding="utf-8") as fp:
    for entry in json.load(fp):
        print(entry["file"])
PY
}

run_clang_check() {
	local file=$1
	local out

	cd "$(dirname "${file}")"
	out=$(mktemp)
	trap 'rm -f "${out}"' RETURN
	clang-check -analyze --analyzer-output-path=/dev/null -p "${COMPILE_DB}" \
		"$(basename "${file}")" 2>&1 | tee "${out}"
	if rg -q "warning:" "${out}"; then
		exit 1
	fi
}

run_clang_tidy() {
	local file=$1
	local out

	out=$(mktemp)
	trap 'rm -f "${out}"' RETURN
	if ! clang-tidy --quiet -p "${COMPILE_DB}" "${file}" >"${out}" 2>&1; then
		cat "${out}"
		exit 1
	fi
}

export -f run_clang_check run_clang_tidy
export CC="ccache clang"

"${MAKE_PARALLEL[@]}" compile-db
export COMPILE_DB=$(/bin/pwd)

compile_db_files | "${PARALLEL}" --halt soon,fail=1 --jobs "${JOBS}" --line-buffer run_clang_check

# CPROVER guards the CBMC-only formal-verification harness at the bottom of
# re.c; cppcheck otherwise analyzes both configurations of that #ifdef and
# flags its symbolic "assume()" reads as uninitialized-variable errors.
cppcheck --quiet --error-exitcode=1 --std=c23 \
	--enable=warning,style,performance,portability --check-level=exhaustive \
	--inline-suppr --suppress=preprocessorErrorDirective:auto.h -UCPROVER \
	-j "${JOBS}" ./re.c ./fuzz/*.c

compile_db_files | "${PARALLEL}" --halt soon,fail=1 --jobs "${JOBS}" --line-buffer run_clang_tidy
