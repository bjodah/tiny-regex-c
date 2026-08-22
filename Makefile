# Overridable from the environment as well as the command line: the .ci
# sanitizer stages export CC/CFLAGS and then run make, and a ":=" here
# would silently win over that -- every sanitizer stage was in fact
# building plain "cc -O3 -Wall -Wextra".
CC ?= cc
CFLAGS ?= -O3 -Wall -Wextra
#CFLAGS := -g -Wall -Wextra -std=c99 -DDEBUG

# Number of random text expressions to generate, for random testing
NRAND_TESTS := 1000

# Prefix used to invoke every test binary, e.g. valgrind under CI.
TEST_RUNNER ?=

PYTHON != if (python --version 2>&1 | grep -q 'Python 3\..*'); then \
            echo 'python';                                          \
          elif command -v python3 >/dev/null 2>&1; then             \
            echo 'python3';                                         \
          else                                                      \
            echo 'Error: no compatible python 3 version found.' >&2;  \
            exit 1;                                                 \
          fi
TEST_BINS = tests/test1 tests/test2 tests/test_compile tests/test_rand \
            tests/test_rand_neg tests/test_api tests/test_end_anchor

all: $(TEST_BINS)

tests/test1: re.c tests/test1.c
	@$(CC) -I. $(CFLAGS) re.c tests/test1.c         -o $@
tests/test2: re.c tests/test2.c
	@$(CC) -I. $(CFLAGS) re.c tests/test2.c         -o $@
tests/test_compile: re.c tests/test_compile.c
	@$(CC) -I. $(CFLAGS) re.c tests/test_compile.c  -o $@
tests/test_rand: re.c tests/test_rand.c
	@$(CC) -I. $(CFLAGS) re.c tests/test_rand.c     -o $@
tests/test_rand_neg: re.c tests/test_rand_neg.c
	@$(CC) -I. $(CFLAGS) re.c tests/test_rand_neg.c -o $@
# test_api.c runs two executions at once to show they share no state.
# Clear TEST_PTHREAD_FLAGS to build it without threads.
# _POSIX_C_SOURCE: the sanitizer stages build with -std=c2x, under which
# glibc hides pthread_barrier_* unless a feature macro asks for it.
TEST_PTHREAD_FLAGS ?= -DRE_TEST_PTHREADS -D_POSIX_C_SOURCE=200809L -pthread

tests/test_api: re.c tests/test_api.c
	@$(CC) -I. $(CFLAGS) $(TEST_PTHREAD_FLAGS) re.c tests/test_api.c -o $@
tests/test_end_anchor: re.c tests/test_end_anchor.c
	@$(CC) -I. $(CFLAGS) re.c tests/test_end_anchor.c -o $@

clean:
	@rm -f $(TEST_BINS)
	@rm -f a.out
	@rm -f *.o

# These two compare against *Python's* re, which reads this dialect's
# escaped operators ("\\(", "\\|", "\\{") as literals, so they run the
# generated Python-compatible subsets rather than ok.lst/nok.lst
# themselves. Add a row to tests/ok.lst or tests/nok.lst -- tests/test1.c
# executes every one of them -- and run `make pysubset` to let the row
# into these drivers if both dialects agree on it.
test-pyok: tests/test_rand tests/pyok.lst
	@$(PYTHON) ./scripts/regex_test.py tests/pyok.lst $(NRAND_TESTS)

test-pynok: tests/test_rand_neg tests/pynok.lst
	@$(PYTHON) ./scripts/regex_test_neg.py tests/pynok.lst $(NRAND_TESTS)

# Regenerate those subsets from ok.lst/nok.lst. Checked in, because the
# drivers must not silently start testing a different set of rows than
# the one that was reviewed.
pysubset:
	@$(PYTHON) ./scripts/select_python_subset.py

# Every leaf here is a *run* of one already-built binary (or one of the two
# Python drivers), and they are prerequisites rather than recipe lines
# because a recipe's lines are a sequence make may not overlap: `make -j`
# runs prerequisites concurrently and recipe lines one after another.  The
# two Python drivers were $(MAKE) recursions for the same reason and are
# ordinary prerequisites now, so the jobserver schedules them beside the
# rest instead of after it.
test: all verify-syntax test-pyok test-pynok run-test1 run-test-compile \
	run-test2 run-test-api run-test-end-anchor

run-test1: tests/test1
	$(TEST_RUNNER) ./tests/test1

run-test-compile: tests/test_compile
	$(TEST_RUNNER) ./tests/test_compile

run-test2: tests/test2
	$(TEST_RUNNER) ./tests/test2

run-test-api: tests/test_api
	$(TEST_RUNNER) ./tests/test_api

run-test-end-anchor: tests/test_end_anchor
	$(TEST_RUNNER) ./tests/test_end_anchor

check: test

CBMC ?= cbmc
CBMC_FLAGS ?= -DCPROVER --unwind 16 --depth 16 --bounds-check \
	--pointer-check --memory-leak-check --div-by-zero-check \
	--signed-overflow-check --unsigned-overflow-check \
	--pointer-overflow-check --conversion-check --undefined-shift-check
# --enum-range-check not with cbmc 5.10 on ubuntu-latest.
# One proof per harness: re.c has no main() under -DCPROVER, so cbmc has
# to be told its entry point, and the old recipe (no --function, and an
# "--unwindset 8" that is missing the loop name the option takes) could
# not have run even when the harness still compiled.
verify:
	$(CBMC) $(CBMC_FLAGS) --function verify_re_compile $(CBMC_ARGS) re.c
	$(CBMC) $(CBMC_FLAGS) --function verify_re_match $(CBMC_ARGS) re.c

# What keeps the CPROVER harness from rotting on a machine without cbmc:
# it is code, and an ordinary compiler can say whether it is still valid.
# It had not compiled for a long time -- it called re_match() with a
# compiled program, read a union member that no longer exists, and used a
# bare assume().  Part of `make test`, costs a fraction of a second.
VERIFY_SYNTAX_CC ?= $(CC)
verify-syntax:
	$(VERIFY_SYNTAX_CC) -DCPROVER -I. -Wall -Wextra -Werror -fsyntax-only re.c

# Project metrics
SCC ?= scc
SCC_PATHS ?= re.c re.h $(FUZZ_SRCS)
SCC_COMPLEXITY_PATHS ?= re.c re.h
# The measured actual, with no slack: raising either is an explicit
# decision, and the rationale and the before/after measurement live in the
# commit that raises it.
SCC_COMPLEXITY_MAX ?= 361
SCC_FILE_COMPLEXITY_MAX ?= 361
PMCCABE ?= pmccabe
PMCCABE_PATHS ?= re.c
PMCCABE_FUNCTION_COMPLEXITY_MAX ?= 60

complexity:
	$(SCC) --ci --by-file --sort complexity $(SCC_PATHS)

complexity-check:
	$(SCC) --ci --by-file --format json $(SCC_COMPLEXITY_PATHS) | \
		$(PYTHON) utils/check_scc_complexity.py \
			--max-total $(SCC_COMPLEXITY_MAX) \
			--max-file $(SCC_FILE_COMPLEXITY_MAX)

pmccabe:
	$(PMCCABE) $(PMCCABE_PATHS) | sort -nr

pmccabe-check:
	$(PMCCABE) $(PMCCABE_PATHS) | \
		$(PYTHON) utils/check_pmccabe_complexity.py \
			--max-function $(PMCCABE_FUNCTION_COMPLEXITY_MAX)

# Coverage
COVERAGE_DIR ?= coverage
COVERAGE_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x -O0 -g --coverage
COVERAGE_LCOV_ARGS ?= --quiet --branch-coverage --ignore-errors inconsistent,gcov
COVERAGE_GENHTML_ARGS ?= --quiet
COVERAGE_MIN_LINES ?= 80

coverage: coverage-clean
	$(MAKE) clean
	mkdir -p $(COVERAGE_DIR)
	$(MAKE) check CFLAGS="$(COVERAGE_CFLAGS)"
	lcov $(COVERAGE_LCOV_ARGS) --capture --directory . \
		--output-file $(COVERAGE_DIR)/run.info
	lcov $(COVERAGE_LCOV_ARGS) --extract $(COVERAGE_DIR)/run.info \
		'$(CURDIR)/re.c' --output-file $(COVERAGE_DIR)/re.info
	genhtml $(COVERAGE_GENHTML_ARGS) $(COVERAGE_DIR)/re.info \
		--output-directory $(COVERAGE_DIR)/html
	lcov --branch-coverage --summary $(COVERAGE_DIR)/re.info \
		--fail-under-lines $(COVERAGE_MIN_LINES)

coverage-clean:
	rm -rf $(COVERAGE_DIR)
	find . -maxdepth 1 \( -name '*.gcda' -o -name '*.gcno' \) -delete
	find tests -maxdepth 1 \( -name '*.gcda' -o -name '*.gcno' \) -delete

# Formatting -- scoped to the maintained library sources, not the
# third-party test fixtures (tests/test2.c alone is a ~2000-line binary
# data blob) or the Python helper scripts.
CLANG_FORMAT ?= clang-format
FORMAT_FILES = re.c re.h $(FUZZ_SRCS)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_FILES)

# Static analysis
BEAR ?= bear
CLANG_CC ?= clang
COMPILE_DB_FILE ?= compile_commands.json
# Found on PATH; the absolute path is one developer box's layout, kept
# only as a last resort.  Override with `make IWYU=... IWYU_TOOL=...`.
IWYU_FALLBACK_DIR ?= /opt-3/iwyu-21/bin
IWYU ?= $(shell command -v include-what-you-use 2>/dev/null || \
	echo $(IWYU_FALLBACK_DIR)/include-what-you-use)
IWYU_TOOL ?= $(shell command -v iwyu_tool.py 2>/dev/null || \
	echo $(IWYU_FALLBACK_DIR)/iwyu_tool.py)
IWYU_ARGS ?= -Xiwyu --error=1
IWYU_FILES = $(addprefix $(CURDIR)/,re.c $(FUZZ_SRCS))

COMPILE_DB_CFLAGS ?= -I. -std=c2x -Wall -Wextra

# Build isolated, single-source "-c" compile commands for the files iwyu
# analyzes. The normal test targets link "re.c" together with each
# tests/testN.c in one multi-source invocation, which bear/iwyu can't turn
# back into a clean per-file compilation database entry.
compile-db:
	rm -f $(COMPILE_DB_FILE)
	$(BEAR) --output $(COMPILE_DB_FILE) -- \
		$(CLANG_CC) $(COMPILE_DB_CFLAGS) -c re.c -o /tmp/re-compiledb.o
	$(BEAR) --append --output $(COMPILE_DB_FILE) -- \
		$(CLANG_CC) $(COMPILE_DB_CFLAGS) -fsanitize=fuzzer \
			-c $(FUZZ_DIR)/fuzz_regex.c -o /tmp/fuzz-regex-compiledb.o

iwyu:
	@test -f $(COMPILE_DB_FILE) || { \
		echo "$(COMPILE_DB_FILE) missing; run 'make compile-db' first"; \
		exit 2; \
	}
	@command -v "$(IWYU)" >/dev/null 2>&1 || { \
		echo "include-what-you-use not found (tried '$(IWYU)');" \
		     "install it, or set IWYU=/path/to/include-what-you-use" >&2; \
		exit 2; \
	}
	@command -v "$(IWYU_TOOL)" >/dev/null 2>&1 || { \
		echo "iwyu_tool.py not found (tried '$(IWYU_TOOL)');" \
		     "install it, or set IWYU_TOOL=/path/to/iwyu_tool.py" >&2; \
		exit 2; \
	}
	PATH="$$(dirname "$(IWYU)"):$${PATH}" \
		$(IWYU_TOOL) -p . $(IWYU_FILES) -- $(IWYU_ARGS)

# Fuzzing
FUZZ_DIR ?= fuzz
FUZZ_CC ?= clang
FUZZ_CFLAGS ?= -Wall -Wextra -Werror -pedantic -std=c2x -O1 -g \
	-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer
FUZZ_RUNS ?= 2000
FUZZ_MAX_LEN ?= 512
FUZZ_TIMEOUT ?= 2
FUZZ_RSS_LIMIT_MB ?= 512
FUZZ_VERBOSITY ?= 0
FUZZ_CORPUS_DIR ?= $(FUZZ_DIR)/corpus
FUZZ_SEED_DIR ?= $(FUZZ_DIR)/seeds
FUZZ_ARTIFACT_DIR ?= $(FUZZ_DIR)/artifacts
FUZZ_SRCS = $(FUZZ_DIR)/fuzz_regex.c
FUZZ_BIN = $(FUZZ_DIR)/fuzz_regex

fuzz: $(FUZZ_BIN)

$(FUZZ_BIN): $(FUZZ_DIR)/fuzz_regex.c re.c re.h
	$(FUZZ_CC) $(FUZZ_CFLAGS) -I. -o $@ $(FUZZ_DIR)/fuzz_regex.c re.c

fuzz-smoke: $(FUZZ_BIN)
	mkdir -p $(FUZZ_CORPUS_DIR)/regex $(FUZZ_ARTIFACT_DIR)/regex
	./$(FUZZ_BIN) -runs=$(FUZZ_RUNS) -max_len=$(FUZZ_MAX_LEN) \
		-timeout=$(FUZZ_TIMEOUT) -rss_limit_mb=$(FUZZ_RSS_LIMIT_MB) \
		-verbosity=$(FUZZ_VERBOSITY) \
		-artifact_prefix=$(FUZZ_ARTIFACT_DIR)/regex/ \
		$(FUZZ_CORPUS_DIR)/regex $(FUZZ_SEED_DIR)/regex

fuzz-clean:
	rm -rf $(FUZZ_CORPUS_DIR) $(FUZZ_ARTIFACT_DIR) $(FUZZ_BIN)

.PHONY: all clean test-pyok test-pynok pysubset test check verify verify-syntax \
	run-test1 run-test-compile run-test2 run-test-api run-test-end-anchor \
	complexity \
	complexity-check pmccabe pmccabe-check coverage coverage-clean \
	format format-check compile-db iwyu fuzz fuzz-smoke fuzz-clean
