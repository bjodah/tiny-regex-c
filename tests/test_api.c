/*
 * Exercises the public API entry points (re_compile_to, re_size,
 * re_compare, re_string) that the hand-picked/random pattern suites never
 * touch, since those only call re_match()/re_compile().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RE_TEST_PTHREADS
#include <pthread.h>
#endif

#include "re.h"

/* A pattern that backtracks catastrophically, and a subject with no 'b'
 * in it, so a match attempt can only end by running out of budget. */
#define RE_SLOW_PATTERN "\\(a*\\)*b"
#define RE_SLOW_TEXT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

/* A pattern, a subject and the spans GNU Emacs reports for it.  nspans 0
 * means "must not match"; a span of {-1, -1} is a group that did not
 * participate.  'offset' is re_exec()'s start_offset, i.e. where the scan
 * resumes -- never where the subject begins. */
struct span_case {
  const char* pattern;
  const char* text;
  int nspans;
  int spans[3][2];
  int offset;
};

/* A pattern and the status re_compile_checked() must report for it. */
struct compile_case {
  const char* pattern;
  re_status status;
};

static int check_compile(const struct compile_case* c) {
  _Alignas(RE_STORAGE_ALIGNMENT) unsigned char storage[512];
  unsigned size = sizeof(storage);
  re_t regex = NULL;
  re_status status =
      re_compile_checked(c->pattern, RE_FLAG_NONE, storage, &size, &regex);

  if (status == c->status)
    return 0;
  fprintf(stderr, "FAIL: re_compile_checked(\"%s\") returned %d, expected %d\n",
          c->pattern, status, c->status);
  return 1;
}

/* Run one span_case and report how many checks it failed. */
static int check_spans(const struct span_case* c) {
  _Alignas(RE_STORAGE_ALIGNMENT) unsigned char storage[256];
  unsigned size = sizeof(storage);
  re_t regex = NULL;
  re_match_result res;
  re_status status;
  int len = (int)strlen(c->text);
  int failed = 0;
  int i;

  if (re_compile_checked(c->pattern, RE_FLAG_NONE, storage, &size, &regex) !=
      RE_STATUS_OK) {
    fprintf(stderr, "FAIL: re_compile_checked(\"%s\") failed\n", c->pattern);
    return 1;
  }

  status = re_exec(regex, c->text, c->offset, &res);
  if (c->nspans == 0) {
    if (status == RE_STATUS_OK)
      fprintf(stderr, "FAIL: \"%s\" matched \"%s\" [%d, %d)\n", c->pattern,
              c->text, res.spans[0].start, res.spans[0].end);
    return status == RE_STATUS_OK;
  }
  if (status != RE_STATUS_OK) {
    fprintf(stderr, "FAIL: \"%s\" did not match \"%s\" (status %d)\n",
            c->pattern, c->text, status);
    return 1;
  }
  if (res.nspans != c->nspans) {
    fprintf(stderr, "FAIL: \"%s\" on \"%s\": nspans %d, expected %d\n",
            c->pattern, c->text, res.nspans, c->nspans);
    failed++;
  }
  for (i = 0; i < c->nspans && i < res.nspans; i++) {
    if (res.spans[i].start != c->spans[i][0] ||
        res.spans[i].end != c->spans[i][1]) {
      fprintf(stderr, "FAIL: \"%s\" on \"%s\": span %d [%d, %d), expected "
                      "[%d, %d)\n",
              c->pattern, c->text, i, res.spans[i].start, res.spans[i].end,
              c->spans[i][0], c->spans[i][1]);
      failed++;
    }
    /* The invariant behind all of this: a match lies inside its subject. */
    if (res.spans[i].end > len) {
      fprintf(stderr, "FAIL: \"%s\" on \"%s\": span %d ends at %d, past the "
                      "%d-byte subject\n",
              c->pattern, c->text, i, res.spans[i].end, len);
      failed++;
    }
  }
  return failed;
}

/* Compile 'pattern' into 'storage', aborting the test on failure. */
static re_t compile_or_die(const char* pattern, unsigned char* storage,
                           unsigned bytes) {
  re_t regex = NULL;
  unsigned size = bytes;

  if (re_compile_checked(pattern, RE_FLAG_NONE, storage, &size, &regex) !=
      RE_STATUS_OK) {
    fprintf(stderr, "FAIL: could not compile \"%s\"\n", pattern);
    exit(1);
  }
  return regex;
}

/* A cancel callback that runs a whole re_exec() of its own, with a budget
 * so small the nested attempt is always abandoned, and then lets the outer
 * attempt continue.  With the budget in file-scope statics, the nested run
 * both reset the outer's step count and left it poisoned on the way out. */
struct nesting_probe {
  re_t nested;
  int calls;
  int nested_status;
};

static int cancel_running_nested_exec(void* data) {
  struct nesting_probe* probe = data;
  re_exec_options tiny = {0};
  re_match_result res;

  tiny.max_steps = 50;
  probe->calls++;
  probe->nested_status =
      (int)re_exec_with_options(probe->nested, RE_SLOW_TEXT, 0, &tiny, &res);
  return 0;
}

static int cancel_always(void* data) {
  ++*(int*)data;
  return 1;
}

#ifdef RE_TEST_PTHREADS
struct racer {
  re_t regex;
  const char* text;
  re_exec_options options;
  re_status expected;
  int failed;
  int rounds;
};

/* Both racers wait here, so the two matching loops really do overlap:
 * without it the first thread can finish before the second starts and the
 * test proves nothing.  Blocking rather than spinning -- under Valgrind,
 * where one thread runs at a time, a spin wait costs minutes. */
static pthread_barrier_t racers_start;

static void* race(void* data) {
  struct racer* r = data;
  int i;

  pthread_barrier_wait(&racers_start);
  for (i = 0; i < r->rounds; i++) {
    re_match_result res;
    re_status status =
        re_exec_with_options(r->regex, r->text, 0, &r->options, &res);
    if (status != r->expected)
      r->failed++;
    if (status == RE_STATUS_OK && res.spans[0].start != 0)
      r->failed++;
  }
  return NULL;
}

/* Run two racers concurrently and report how many checks failed. */
static int run_racers(struct racer* a, struct racer* b) {
  pthread_t ta, tb;

  if (pthread_barrier_init(&racers_start, NULL, 2))
    return 1;
  if (pthread_create(&ta, NULL, race, a) || pthread_create(&tb, NULL, race, b))
    return 1;
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);
  pthread_barrier_destroy(&racers_start);
  return a->failed + b->failed;
}
#endif

/* Everything the matcher remembers between calls -- which, since the work
 * budget moved into the per-execution context, is nothing. */
static int test_execution_state(void) {
  _Alignas(RE_STORAGE_ALIGNMENT) static unsigned char slow_storage[256];
  _Alignas(RE_STORAGE_ALIGNMENT) static unsigned char fast_storage[256];
  re_t slow = compile_or_die(RE_SLOW_PATTERN, slow_storage,
                             sizeof(slow_storage));
  re_t fast = compile_or_die("a\\{3\\}", fast_storage, sizeof(fast_storage));
  re_exec_options opts;
  re_match_result res;
  re_status status;
  int failed = 0;

  /* A small budget is spent, and reported as such rather than as a plain
   * no-match. */
  memset(&opts, 0, sizeof(opts));
  opts.max_steps = 200;
  status = re_exec_with_options(slow, RE_SLOW_TEXT, 0, &opts, &res);
  if (status != RE_STATUS_TOO_COMPLEX) {
    fprintf(stderr, "FAIL: a 200-step budget gave %d, expected TOO_COMPLEX\n",
            status);
    failed++;
  }
  /* ... and it belongs to that call alone: the next one starts fresh. */
  if (re_exec(fast, "aaa", 0, &res) != RE_STATUS_OK || res.spans[0].end != 3) {
    fprintf(stderr, "FAIL: a spent budget leaked into the next execution\n");
    failed++;
  }
  memset(&opts, 0, sizeof(opts));
  opts.max_depth = 4;
  if (re_exec_with_options(slow, RE_SLOW_TEXT, 0, &opts, &res) !=
      RE_STATUS_TOO_COMPLEX) {
    fprintf(stderr, "FAIL: a 4-frame depth budget was not reported\n");
    failed++;
  }
  if (re_exec(fast, "aaa", 0, &res) != RE_STATUS_OK) {
    fprintf(stderr, "FAIL: a spent depth budget leaked into the next run\n");
    failed++;
  }

  /* Cancellation is TOO_COMPLEX -- the attempt was abandoned -- and the
   * compiled pattern is reusable afterwards. */
  {
    int calls = 0;
    memset(&opts, 0, sizeof(opts));
    opts.cancel = cancel_always;
    opts.cancel_data = &calls;
    if (re_exec_with_options(fast, "aaa", 0, &opts, &res) !=
        RE_STATUS_TOO_COMPLEX) {
      fprintf(stderr, "FAIL: cancellation did not abandon the attempt\n");
      failed++;
    }
    if (calls == 0) {
      fprintf(stderr, "FAIL: the cancel callback was never polled\n");
      failed++;
    }
    if (re_exec(fast, "aaa", 0, &res) != RE_STATUS_OK) {
      fprintf(stderr, "FAIL: a cancelled pattern did not run again\n");
      failed++;
    }
  }

  /* A callback that runs a nested execution of its own must not spend the
   * outer attempt's budget. */
  {
    struct nesting_probe probe;
    probe.nested = slow;
    probe.calls = 0;
    probe.nested_status = (int)RE_STATUS_OK;
    memset(&opts, 0, sizeof(opts));
    opts.cancel = cancel_running_nested_exec;
    opts.cancel_data = &probe;
    status = re_exec_with_options(fast, "aaa", 0, &opts, &res);
    if (status != RE_STATUS_OK || res.spans[0].end != 3) {
      fprintf(stderr,
              "FAIL: a nested execution inside the cancel callback broke the "
              "outer one: status %d\n",
              status);
      failed++;
    }
    if (probe.calls == 0 ||
        probe.nested_status != (int)RE_STATUS_TOO_COMPLEX) {
      fprintf(stderr,
              "FAIL: the nested execution did not run bounded by its own "
              "budget (%d calls, status %d)\n",
              probe.calls, probe.nested_status);
      failed++;
    }
  }

  /* re_compile()'s static buffer holds MAX_REGEXP_OBJECTS nodes and says
   * nothing when a pattern outgrows it: NULL is indistinguishable from a
   * bad pattern.  re_compile_checked() is the entry point that reports
   * the difference, and the one every caller here uses. */
  {
    char atoms[64];
    unsigned needed = 0;
    re_t sized = NULL;

    memset(atoms, 'a', sizeof(atoms));
    atoms[29] = '\0';
    if (re_compile(atoms) == NULL) {
      fprintf(stderr, "FAIL: re_compile() rejected 29 literal atoms\n");
      failed++;
    }
    atoms[29] = 'a';
    atoms[30] = '\0';
    if (re_compile(atoms) != NULL) {
      fprintf(stderr, "FAIL: re_compile() took 30 literal atoms\n");
      failed++;
    }
    if (re_compile_checked(atoms, RE_FLAG_NONE, NULL, &needed, &sized) !=
        RE_STATUS_BUFFER_TOO_SMALL) {
      fprintf(stderr,
              "FAIL: re_compile_checked() did not report 30 atoms as a size "
              "problem\n");
      failed++;
    }
  }

#ifdef RE_TEST_PTHREADS
  /* Two executions at once share no mutable state: neither on one compiled
   * program nor across two, and a budget spent on one thread is not spent
   * on the other. */
  {
    struct racer same_a, same_b, mixed_slow, mixed_fast;

    memset(&same_a, 0, sizeof(same_a));
    same_a.regex = fast;
    same_a.text = "aaa";
    same_a.expected = RE_STATUS_OK;
    same_a.rounds = 400;
    same_b = same_a;
    failed += run_racers(&same_a, &same_b);

    memset(&mixed_slow, 0, sizeof(mixed_slow));
    mixed_slow.regex = slow;
    mixed_slow.text = RE_SLOW_TEXT;
    mixed_slow.options.max_steps = 3000;
    mixed_slow.expected = RE_STATUS_TOO_COMPLEX;
    mixed_slow.rounds = 40;

    memset(&mixed_fast, 0, sizeof(mixed_fast));
    mixed_fast.regex = fast;
    mixed_fast.text = "aaa";
    mixed_fast.expected = RE_STATUS_OK;
    mixed_fast.rounds = 400;
    failed += run_racers(&mixed_slow, &mixed_fast);

    if (failed)
      fprintf(stderr, "FAIL: concurrent executions disagreed with serial "
                      "ones\n");
  }
#endif

  return failed;
}

int main(void) {
  int failed = 0;

  _Alignas(RE_STORAGE_ALIGNMENT) unsigned char buf1[256];
  unsigned size1 = sizeof(buf1);
  re_t p1 = re_compile_to("a[0-9]+", buf1, &size1);
  if (!p1 || size1 == 0) {
    fprintf(stderr, "re_compile_to(\"a[0-9]+\") failed\n");
    failed++;
  }

  unsigned sz = re_size(p1);
  if (sz != size1) {
    fprintf(stderr, "re_size() == %u, expected %u\n", sz, size1);
    failed++;
  }

  _Alignas(RE_STORAGE_ALIGNMENT) unsigned char buf2[256];
  unsigned size2 = sizeof(buf2);
  re_t p2 = re_compile_to("a[0-9]+", buf2, &size2);
  if (re_compare(p1, p2) != 0) {
    fprintf(stderr, "re_compare: identical patterns compared unequal\n");
    failed++;
  }

  _Alignas(RE_STORAGE_ALIGNMENT) unsigned char buf3[256];
  unsigned size3 = sizeof(buf3);
  re_t p3 = re_compile_to("b[0-9]+", buf3, &size3);
  if (re_compare(p1, p3) == 0) {
    fprintf(stderr, "re_compare: different patterns compared equal\n");
    failed++;
  }

  _Alignas(RE_STORAGE_ALIGNMENT) unsigned char buf4[256];
  unsigned size4 = sizeof(buf4);
  re_t p4 = re_compile_to("a[0-9]+longer", buf4, &size4);
  if (re_compare(p1, p4) == 0) {
    fprintf(stderr, "re_compare: patterns of different size compared equal\n");
    failed++;
  }

  char rendered[256];
  unsigned rsize = sizeof(rendered);
  re_string(p1, rendered, &rsize);
  if (rendered[0] == '\0') {
    fprintf(stderr, "re_string() produced an empty string\n");
    failed++;
  }
  printf("re_string(\"a[0-9]+\") -> \"%s\"\n", rendered);

  rsize = sizeof(rendered);
  rendered[0] = 'x';
  re_string(NULL, rendered, &rsize);
  if (rendered[0] != '\0' || rsize != 0) {
    fprintf(stderr, "re_string(NULL, ...) did not produce an empty string\n");
    failed++;
  }

  /* Checked API tests */
  {
    /* Test 1: Compile valid pattern */
    _Alignas(RE_STORAGE_ALIGNMENT) unsigned char storage[256];
    unsigned storage_size = sizeof(storage);
    re_t regex = NULL;
    re_status status = re_compile_checked("a[0-9]+", RE_FLAG_NONE, storage,
                                          &storage_size, &regex);
    if (status != RE_STATUS_OK) {
      fprintf(stderr, "FAIL: re_compile_checked (valid pattern) returned %d\n",
              status);
      failed++;
    }
    if (!regex) {
      fprintf(
          stderr,
          "FAIL: re_compile_checked (valid pattern) output regex is NULL\n");
      failed++;
    }

    /* Test 2: Compile invalid pattern */
    _Alignas(RE_STORAGE_ALIGNMENT) unsigned char storage_invalid[256];
    unsigned storage_size_invalid = sizeof(storage_invalid);
    re_t regex_invalid = NULL;
    status = re_compile_checked("\\", RE_FLAG_NONE, storage_invalid,
                                &storage_size_invalid, &regex_invalid);
    if (status != RE_STATUS_BAD_PATTERN) {
      fprintf(stderr,
              "FAIL: re_compile_checked (invalid pattern) expected "
              "BAD_PATTERN, got %d\n",
              status);
      failed++;
    }

    /* Test 3: Buffer too small handling and storage_size updating */
    unsigned storage_size_small = 5; /* way too small */
    _Alignas(RE_STORAGE_ALIGNMENT) unsigned char storage_small[5];
    re_t regex_small = NULL;
    status = re_compile_checked("a[0-9]+", RE_FLAG_NONE, storage_small,
                                &storage_size_small, &regex_small);
    if (status != RE_STATUS_BUFFER_TOO_SMALL) {
      fprintf(stderr,
              "FAIL: re_compile_checked (small buffer) expected "
              "BUFFER_TOO_SMALL, got %d\n",
              status);
      failed++;
    }
    /* Let's verify storage_size_small was updated to the required size */
    if (storage_size_small <= 5) {
      fprintf(stderr,
              "FAIL: re_compile_checked (small buffer) did not update required "
              "size correctly: %u\n",
              storage_size_small);
      failed++;
    }
    /* Now compile with the newly returned required size */
    unsigned char* storage_fit = malloc(storage_size_small);
    if (!storage_fit) {
      fprintf(stderr, "FAIL: malloc failed in test\n");
      failed++;
    } else {
      unsigned storage_size_fit = storage_size_small;
      re_t regex_fit = NULL;
      status = re_compile_checked("a[0-9]+", RE_FLAG_NONE, storage_fit,
                                  &storage_size_fit, &regex_fit);
      if (status != RE_STATUS_OK) {
        fprintf(stderr,
                "FAIL: re_compile_checked after resizing buffer returned %d\n",
                status);
        failed++;
      }
      free(storage_fit);
    }

    /* Test 4: Matching with whole-match spans */
    if (regex) {
      re_match_result match_res;
      status = re_exec(regex, "xyz a1234 abc", 0, &match_res);
      if (status != RE_STATUS_OK) {
        fprintf(stderr, "FAIL: re_exec (matching case) returned %d\n", status);
        failed++;
      } else {
        if (match_res.nspans != 1) {
          fprintf(stderr, "FAIL: re_exec expected nspans == 1, got %d\n",
                  match_res.nspans);
          failed++;
        }
        if (match_res.spans[0].start != 4) {
          fprintf(stderr,
                  "FAIL: re_exec expected spans[0].start == 4, got %d\n",
                  match_res.spans[0].start);
          failed++;
        }
        if (match_res.spans[0].end != 9) {
          fprintf(stderr, "FAIL: re_exec expected spans[0].end == 9, got %d\n",
                  match_res.spans[0].end);
          failed++;
        }
      }
    }

    /* Test 5: No-match */
    if (regex) {
      re_match_result match_res;
      status = re_exec(regex, "xyz a abc", 0, &match_res);
      if (status != RE_STATUS_NO_MATCH) {
        fprintf(stderr, "FAIL: re_exec (no match) expected NO_MATCH, got %d\n",
                status);
        failed++;
      }
    }

    /* Test 6: Zero-length matches */
    _Alignas(RE_STORAGE_ALIGNMENT) unsigned char z_storage[256];
    unsigned z_size = sizeof(z_storage);
    re_t z_regex = NULL;
    status =
        re_compile_checked("a*", RE_FLAG_NONE, z_storage, &z_size, &z_regex);
    if (status != RE_STATUS_OK) {
      fprintf(stderr, "FAIL: re_compile_checked (\"a*\") returned %d\n",
              status);
      failed++;
    } else {
      re_match_result match_res;
      /* "a*" on "b" should match empty string at index 0 */
      status = re_exec(z_regex, "b", 0, &match_res);
      if (status != RE_STATUS_OK) {
        fprintf(stderr, "FAIL: re_exec (\"a*\" on \"b\") returned %d\n",
                status);
        failed++;
      } else {
        if (match_res.nspans != 1) {
          fprintf(stderr, "FAIL: re_exec expected nspans == 1, got %d\n",
                  match_res.nspans);
          failed++;
        }
        if (match_res.spans[0].start != 0 || match_res.spans[0].end != 0) {
          fprintf(stderr,
                  "FAIL: re_exec expected spans[0] == [0, 0], got [%d, %d]\n",
                  match_res.spans[0].start, match_res.spans[0].end);
          failed++;
        }
      }

      /* "a*" on "b" starting at offset 1 should match empty string at index 1
       */
      status = re_exec(z_regex, "b", 1, &match_res);
      if (status != RE_STATUS_OK) {
        fprintf(stderr,
                "FAIL: re_exec (\"a*\" on \"b\" at offset 1) returned %d\n",
                status);
        failed++;
      } else {
        if (match_res.nspans != 1) {
          fprintf(stderr, "FAIL: re_exec expected nspans == 1, got %d\n",
                  match_res.nspans);
          failed++;
        }
        if (match_res.spans[0].start != 1 || match_res.spans[0].end != 1) {
          fprintf(stderr,
                  "FAIL: re_exec expected spans[0] == [1, 1], got [%d, %d]\n",
                  match_res.spans[0].start, match_res.spans[0].end);
          failed++;
        }
      }
    }

    /* Test 7: POSIX bracket classes */
    _Alignas(RE_STORAGE_ALIGNMENT) unsigned char posix_storage[256];
    unsigned posix_size = sizeof(posix_storage);
    re_t posix_regex = NULL;
    status = re_compile_checked("[[:digit:]]+", RE_FLAG_NONE, posix_storage,
                                &posix_size, &posix_regex);
    if (status != RE_STATUS_OK) {
      fprintf(stderr,
              "FAIL: re_compile_checked (\"[[:digit:]]+\") returned %d\n",
              status);
      failed++;
    } else {
      re_match_result match_res;
      status = re_exec(posix_regex, "abc 1234 def", 0, &match_res);
      if (status != RE_STATUS_OK) {
        fprintf(stderr,
                "FAIL: re_exec (\"[[:digit:]]+\" on \"abc 1234 def\") returned "
                "%d\n",
                status);
        failed++;
      } else {
        if (match_res.spans[0].start != 4 || match_res.spans[0].end != 8) {
          fprintf(stderr, "FAIL: re_exec expected [4, 8], got [%d, %d]\n",
                  match_res.spans[0].start, match_res.spans[0].end);
          failed++;
        }
      }
    }

    /* Test 8: Bare vs Escaped constructs (Emacs-like dialect) */
    _Alignas(RE_STORAGE_ALIGNMENT) unsigned char dialect_storage[256];
    unsigned dialect_size = sizeof(dialect_storage);
    re_t dialect_regex = NULL;
    /* bare ( ) and | should be literal characters, not grouping or alternation
     */
    status = re_compile_checked("(a|b)", RE_FLAG_NONE, dialect_storage,
                                &dialect_size, &dialect_regex);
    if (status != RE_STATUS_OK) {
      fprintf(stderr, "FAIL: re_compile_checked (\"(a|b)\") returned %d\n",
              status);
      failed++;
    } else {
      re_match_result match_res;
      status = re_exec(dialect_regex, "(a|b)", 0, &match_res);
      if (status != RE_STATUS_OK) {
        fprintf(stderr, "FAIL: re_exec (\"(a|b)\" on \"(a|b)\") returned %d\n",
                status);
        failed++;
      } else {
        if (match_res.spans[0].start != 0 || match_res.spans[0].end != 5) {
          fprintf(stderr, "FAIL: re_exec expected [0, 5], got [%d, %d]\n",
                  match_res.spans[0].start, match_res.spans[0].end);
          failed++;
        }
      }
    }

    /* Test 9: Case Folding (RE_FLAG_ICASE) */
    _Alignas(RE_STORAGE_ALIGNMENT) unsigned char icase_storage[256];
    unsigned icase_size = sizeof(icase_storage);
    re_t icase_regex = NULL;
    status = re_compile_checked("aB[c-e]F", RE_FLAG_ICASE, icase_storage,
                                &icase_size, &icase_regex);
    if (status != RE_STATUS_OK) {
      fprintf(stderr, "FAIL: re_compile_checked with ICASE returned %d\n",
              status);
      failed++;
    } else {
      re_match_result match_res;
      status = re_exec(icase_regex, "AbDf", 0, &match_res);
      if (status != RE_STATUS_OK) {
        fprintf(stderr, "FAIL: re_exec (case-insensitive) returned %d\n",
                status);
        failed++;
      } else {
        if (match_res.spans[0].start != 0 || match_res.spans[0].end != 4) {
          fprintf(stderr, "FAIL: re_exec expected [0, 4], got [%d, %d]\n",
                  match_res.spans[0].start, match_res.spans[0].end);
          failed++;
        }
      }
    }
    /* Test 10: Nested groups, optional unmatched groups, and repeated captures
     */
    {
      _Alignas(RE_STORAGE_ALIGNMENT) unsigned char test10_storage[256];
      unsigned test10_size = sizeof(test10_storage);
      re_t test10_regex = NULL;
      status = re_compile_checked("\\(a\\(b\\)?c\\)+", RE_FLAG_NONE,
                                  test10_storage, &test10_size, &test10_regex);
      if (status != RE_STATUS_OK) {
        fprintf(stderr,
                "FAIL: re_compile_checked (\"\\\\(a\\\\(b\\\\)?c\\\\)+\") "
                "returned %d\n",
                status);
        failed++;
      } else {
        re_match_result match_res;
        status = re_exec(test10_regex, "acabc", 0, &match_res);
        if (status != RE_STATUS_OK) {
          fprintf(stderr, "FAIL: re_exec for Test 10 returned %d\n", status);
          failed++;
        } else {
          if (match_res.nspans != 3) {
            fprintf(stderr, "FAIL: Test 10 expected nspans == 3, got %d\n",
                    match_res.nspans);
            failed++;
          }
          if (match_res.spans[0].start != 0 || match_res.spans[0].end != 5) {
            fprintf(stderr,
                    "FAIL: Test 10 spans[0] expected [0, 5], got [%d, %d]\n",
                    match_res.spans[0].start, match_res.spans[0].end);
            failed++;
          }
          if (match_res.spans[1].start != 2 || match_res.spans[1].end != 5) {
            fprintf(stderr,
                    "FAIL: Test 10 spans[1] expected [2, 5] (last repetition), "
                    "got [%d, %d]\n",
                    match_res.spans[1].start, match_res.spans[1].end);
            failed++;
          }
          if (match_res.spans[2].start != 3 || match_res.spans[2].end != 4) {
            fprintf(stderr,
                    "FAIL: Test 10 spans[2] expected [3, 4], got [%d, %d]\n",
                    match_res.spans[2].start, match_res.spans[2].end);
            failed++;
          }
        }

        /* test unmatched optional group */
        status = re_exec(test10_regex, "ac", 0, &match_res);
        if (status != RE_STATUS_OK) {
          fprintf(
              stderr,
              "FAIL: re_exec for Test 10 (unmatched optional) returned %d\n",
              status);
          failed++;
        } else {
          if (match_res.spans[0].start != 0 || match_res.spans[0].end != 2) {
            fprintf(stderr,
                    "FAIL: Test 10 (unmatched optional) spans[0] expected [0, "
                    "2], got [%d, %d]\n",
                    match_res.spans[0].start, match_res.spans[0].end);
            failed++;
          }
          if (match_res.spans[1].start != 0 || match_res.spans[1].end != 2) {
            fprintf(stderr,
                    "FAIL: Test 10 (unmatched optional) spans[1] expected [0, "
                    "2], got [%d, %d]\n",
                    match_res.spans[1].start, match_res.spans[1].end);
            failed++;
          }
          if (match_res.spans[2].start != -1 || match_res.spans[2].end != -1) {
            fprintf(stderr,
                    "FAIL: Test 10 (unmatched optional) spans[2] expected [-1, "
                    "-1], got [%d, %d]\n",
                    match_res.spans[2].start, match_res.spans[2].end);
            failed++;
          }
        }
      }
    }

    /* Test 11: spans, including capture spans, for the three defects this
     * matcher was rebuilt around.  Every expectation is what GNU Emacs
     * reports for the same pattern and subject. */
    {
      static const struct span_case cases[] = {
          /* the reported end used to run past the end of the subject */
          {"a*.c+", "ac", 1, {{0, 2}}, 0},
          {"\\(.*.a\\{2\\}\\)", "baa", 2, {{0, 3}, {0, 3}}, 0},
          {"[a-c]+\\w\\{2\\}b", "baab", 1, {{0, 4}}, 0},
          {"\\w*.\\{3\\}a?[a-c]\\{1\\}", "b11bZZZ", 1, {{0, 4}}, 0},
          /* "\|" separates whole alternatives, not just the atom before it */
          {"foo\\|bar", "bar", 1, {{0, 3}}, 0},
          {"ab\\|cd", "cd", 1, {{0, 2}}, 0},
          {"za\\|b", "zb", 1, {{1, 2}}, 0},
          {"\\(ab\\)\\|\\(cd\\)", "cd", 3, {{0, 2}, {-1, -1}, {0, 2}}, 0},
          {"x\\(ab\\|cd\\)y", "xcdy", 2, {{0, 4}, {1, 3}}, 0},
          {"\\(a\\|ab\\)c", "abc", 2, {{0, 3}, {0, 2}}, 0},
          {"ab\\|cd", "ac", 0, {{0, 0}}, 0},
          /* groups and intervals give input back when what follows needs it */
          {"^\\(.*\\),\\(.*\\)$", "a,b", 3, {{0, 3}, {0, 1}, {2, 3}}, 0},
          {".\\{2,3\\}c", "abc", 1, {{0, 3}}, 0},
          {"a\\{2,3\\}a", "aaa", 1, {{0, 3}}, 0},
          {"a\\{2,\\}a", "aaa", 1, {{0, 3}}, 0},
          {"a\\{,3\\}a", "aaa", 1, {{0, 3}}, 0},
          {"\\(.*\\)x", "yx", 2, {{0, 2}, {0, 1}}, 0},
          {"\\(a*\\)a", "aa", 2, {{0, 2}, {0, 1}}, 0},
          {"\\(a+\\)a", "aa", 2, {{0, 2}, {0, 1}}, 0},
          {"\\(a\\{2\\}\\)a", "aaa", 2, {{0, 3}, {0, 2}}, 0},
          {"\\(a\\{2\\}\\)a", "aa", 0, {{0, 0}}, 0},
          {"\\(.*\\)x", "y", 0, {{0, 0}}, 0},
      };
      size_t i;
      for (i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        failed += check_spans(&cases[i]);
    }

    /* Test 12: intervals, POSIX class names, '^' anchoring and greedy
     * '?'.  Spans are GNU Emacs' again. */
    {
      static const struct span_case cases[] = {
          /* an interval bound may be zero, and the two may be equal */
          {"x\\{0,1\\}", "x", 1, {{0, 1}}, 0},
          {"a\\{0\\}", "aaa", 1, {{0, 0}}, 0},
          {"a\\{,0\\}", "aaa", 1, {{0, 0}}, 0},
          {"a\\{\\}", "aaa", 1, {{0, 0}}, 0},
          {"a\\{,\\}", "aaa", 1, {{0, 3}}, 0},
          {"a\\{2,2\\}", "aaa", 1, {{0, 2}}, 0},
          {"a\\{2,2\\}", "a", 0, {{0, 0}}, 0},
          {"\\(a\\)\\{0\\}", "aa", 2, {{0, 0}, {-1, -1}}, 0},
          {"\\(a\\)\\{0,1\\}", "aa", 2, {{0, 1}, {0, 1}}, 0},
          {"\\(ab\\)\\{0\\}c", "abc", 2, {{2, 3}, {-1, -1}}, 0},
          {"\\(a\\)\\{2,3\\}", "aaaa", 2, {{0, 3}, {2, 3}}, 0},
          /* nothing to repeat: Emacs reads "\{" as a literal '{' */
          {"\\{2\\}", "{2}", 1, {{0, 3}}, 0},
          {"\\(\\{2\\}\\)", "{2}", 2, {{0, 3}, {0, 3}}, 0},
          {"^\\{2\\}", "{2}", 1, {{0, 3}}, 0},
          {"^\\{2\\}a", "a", 0, {{0, 0}}, 0},
          {"a\\{2\\}", "{2}", 0, {{0, 0}}, 0},
          /* POSIX class names, which used to degrade to a set of the
           * characters spelling them */
          {"[[:blank:]]", "a\tb", 1, {{1, 2}}, 0},
          {"[[:blank:]]", "a", 0, {{0, 0}}, 0},
          {"[[:word:]]", "_a", 1, {{1, 2}}, 0},
          {"[[:ascii:]]", "a", 1, {{0, 1}}, 0},
          /* '^' holds at the start of the subject, not where the scan
           * resumes */
          {"^a", "aba", 1, {{0, 1}}, 0},
          {"^a", "aba", 0, {{0, 0}}, 1},
          {"^b", "ab", 0, {{0, 0}}, 1},
          {"^a\\|b", "ab", 1, {{1, 2}}, 1},
          /* '$' is unchanged: it holds at the subject's terminator */
          {"a$", "aba", 1, {{2, 3}}, 1},
          /* '?' is greedy, like Emacs' */
          {"a?", "a", 1, {{0, 1}}, 0},
          {"x?x", "x", 1, {{0, 1}}, 0},
          {"\\(a\\)?", "a", 2, {{0, 1}, {0, 1}}, 0},
          {"ab?", "ab", 1, {{0, 2}}, 0},
          {"[ab][ab]?", "baab1acc", 1, {{0, 2}}, 0},
          {"c[^a]?", "bc1cacc.", 1, {{1, 3}}, 0},
      };
      size_t i;
      for (i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        failed += check_spans(&cases[i]);
    }

    /* Test 12b: the matcher steps by character, not by byte.  Spans stay
     * byte offsets, so the expected numbers below are byte counts of
     * whole glyphs.  Every case was checked against "emacs -Q --batch"
     * first, converting its character offsets to byte offsets. */
    {
      static const struct span_case cases[] = {
          /* '.' consumes a whole glyph, and an interval counts glyphs */
          {".", "\xc3\xa5" "bc", 1, {{0, 2}}, 0},
          {".\\{2\\}", "\xc3\xa5" "bc", 1, {{0, 3}}, 0},
          {".\\{3\\}", "\xc3\xa5\xe6\x97\xa5\xf0\x9f\x99\x82", 1, {{0, 9}}, 0},
          {"a.b", "a\xf0\x9f\x99\x82" "b", 1, {{0, 6}}, 0},
          {".*", "\xc3\xa5" "b", 1, {{0, 3}}, 0},
          {"x.\\{2,3\\}y", "x\xc3\xa5\xe6\x97\xa5y", 1, {{0, 7}}, 0},
          /* a multi-byte literal is one atom, quantified as one */
          {"\xc3\xa5+", "\xc3\xa5\xc3\xa5\xc3\xa5", 1, {{0, 6}}, 0},
          {"\xc3\xa5*b", "b", 1, {{0, 1}}, 0},
          {"\\(\xc3\xa5\\)\\{2\\}b", "\xc3\xa5\xc3\xa5" "b", 2,
           {{0, 5}, {2, 4}}, 0},
          {"\xc3\xb6\\|\xc3\xa4", "\xc3\xa4", 1, {{0, 2}}, 0},
          /* class members are glyphs: 'ä' matches, the shared 0xc3 lead
           * byte of 'ö' does not */
          {"[\xc3\xa5\xc3\xa4]", "\xc3\xa4", 1, {{0, 2}}, 0},
          {"[\xc3\xa5\xc3\xa4]", "\xc3\xb6", 0, {{0, 0}}, 0},
          {"[\xc3\xa5\xc3\xa4]", "\xc3\xb6\xc3\xa4", 1, {{2, 4}}, 0},
          {"[^\xc3\xa5]", "\xc3\xa5\xc3\xa4", 1, {{2, 4}}, 0},
          /* a range with a multi-byte endpoint compares codepoints, as
           * Emacs does: "[à-é]" holds ç but not ê */
          {"[\xc3\xa0-\xc3\xa9]", "\xc3\xa7", 1, {{0, 2}}, 0},
          {"[\xc3\xa0-\xc3\xa9]", "\xc3\xaa", 0, {{0, 0}}, 0},
          {"[a-\xc3\xbf]", "\xc3\xa5", 1, {{0, 2}}, 0},
          /* "[:ascii:]" and "[:nonascii:]" now mean what they say */
          {"[[:ascii:]]", "\xc3\xa5" "a", 1, {{2, 3}}, 0},
          {"[[:nonascii:]]", "a\xc3\xa5", 1, {{1, 3}}, 0},
          {"[^[:ascii:]]", "a\xc3\xa5", 1, {{1, 3}}, 0},
          /* invalid UTF-8 is not an error: each stray byte is its own
           * glyph, so '.' takes exactly one of them and a stray lead byte
           * is not the character it would have led */
          {".", "\xc3", 1, {{0, 1}}, 0},
          {".\\{2\\}", "\xc3\xc3", 1, {{0, 2}}, 0},
          {".\\{2\\}", "\xc3\xc3\xa5", 1, {{0, 3}}, 0},
          {"[\xc3\xa5]", "\xc3", 0, {{0, 0}}, 0},
          {"[[:nonascii:]]", "\xc3", 1, {{0, 1}}, 0},
          /* "\xXX" still spells a byte: a non-ASCII one only matches
           * where it stands alone */
          {"\\xc3", "\xc3", 1, {{0, 1}}, 0},
          {"\\xc3", "\xc3\xa5", 0, {{0, 0}}, 0},
          {"\\x61", "a", 1, {{0, 1}}, 0},
          /* A "\x" that is not a hex escape after all is the literal
           * characters it is spelled with -- three nodes, or four.  The
           * compiler used to count that as one node, so anything after it
           * that scans back over node indices (a group's "\)") landed in
           * the middle of the program. */
          {"\\xZ", "\\xZ", 1, {{0, 3}}, 0},
          {"\\x4", "\\x4", 1, {{0, 3}}, 0},
          {"\\x4X", "\\x4X", 1, {{0, 4}}, 0},
          {"\\x", "\\x", 1, {{0, 2}}, 0},
          {"\\xZ\\xY", "\\xZ\\xY", 1, {{0, 6}}, 0},
          /* before, inside and after a group, and nested */
          {"\\xZ\\(a\\)", "\\xZa", 2, {{0, 4}, {3, 4}}, 0},
          {"\\(\\xZ\\)a", "\\xZa", 2, {{0, 4}, {0, 3}}, 0},
          {"\\(a\\)\\xZ\\(b\\)", "a\\xZb", 3, {{0, 5}, {0, 1}, {4, 5}}, 0},
          {"\\(\\(a\\)\\xZ\\)b", "a\\xZb", 3, {{0, 5}, {0, 4}, {0, 1}}, 0},
          /* an interval after the fallback repeats its *last* node */
          {"\\xZ\\{2\\}", "\\xZZ", 1, {{0, 4}}, 0},
          {"\\xZ\\{2\\}", "\\xZ", 0, {{0, 0}}, 0},
      };
      size_t i;
      for (i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        failed += check_spans(&cases[i]);
    }

    /* Test 12c: the capture register left by a repeated group whose body
     * can match empty.  An empty repetition ends the loop only once the
     * minimum count is behind us (see match_rep); while the minimum is
     * still being filled, and for the one repetition just past it, an
     * empty body still lets the group run again, so a later repetition
     * can consume and own the register.  Emacs' spans throughout. */
    {
      static const struct span_case cases[] = {
          /* the empty branch comes first, so repetition 1 matches empty
           * and repetition 2 matches "a" -- the register is "a" */
          {"\\(x*\\|a\\)\\{2\\}b", "ab", 2, {{0, 2}, {0, 1}}, 0},
          {"\\(x*\\|a\\)\\{1,2\\}b", "ab", 2, {{0, 2}, {0, 1}}, 0},
          {"\\(x*\\|a\\)\\{2,3\\}b", "ab", 2, {{0, 2}, {0, 1}}, 0},
          {"\\(x*\\|a\\)\\{2\\}$", "a", 2, {{0, 1}, {0, 1}}, 0},
          {"\\(\\|a\\)\\{2\\}b", "ab", 2, {{0, 2}, {0, 1}}, 0},
          {"\\(x*\\|ab\\)\\{2\\}c", "abc", 2, {{0, 3}, {0, 2}}, 0},
          {"\\(x*\\|a\\)\\{3\\}b", "aab", 2, {{0, 3}, {1, 2}}, 0},
          {"\\(x*\\|a\\|b\\)\\{3\\}c", "abc", 2, {{0, 3}, {1, 2}}, 0},
          {"\\(x*\\|a\\)\\{2\\}\\(b\\)", "ab", 3, {{0, 2}, {0, 1}, {1, 2}}, 0},
          {"\\(\\(x*\\|a\\)\\{2\\}\\)b", "ab", 3, {{0, 2}, {0, 1}, {0, 1}}, 0},
          /* enough slack above the minimum and the trailing empty
           * repetition is reachable again, so it owns the register */
          {"\\(x*\\|a\\)\\{1,5\\}$", "aaaa", 2, {{0, 4}, {3, 4}}, 0},
          {"\\(x*\\|a\\)\\{1,6\\}$", "aaaa", 2, {{0, 4}, {4, 4}}, 0},
          {"\\(x*\\|a\\)\\{2,4\\}b", "ab", 2, {{0, 2}, {1, 1}}, 0},
          {"\\(x*\\|a\\)\\{2,\\}b", "ab", 2, {{0, 2}, {1, 1}}, 0},
          /* an empty-matching branch that comes second is never reached
           * before the input runs out, so nothing changed for these */
          {"\\(a\\|x*\\)\\{2\\}b", "ab", 2, {{0, 2}, {1, 1}}, 0},
          {"\\(x*\\|a\\)\\{0,2\\}b", "ab", 2, {{0, 2}, {1, 1}}, 0},
          {"\\(x*\\|a\\)*b", "ab", 2, {{0, 2}, {1, 1}}, 0},
          {"\\(x*\\|a\\)+b", "ab", 2, {{0, 2}, {1, 1}}, 0},
          {"\\(x*\\)\\{3\\}b", "xb", 2, {{0, 2}, {1, 1}}, 0},
          {"\\(a*\\)\\{2\\}b", "ab", 2, {{0, 2}, {1, 1}}, 0},
      };
      size_t i;
      for (i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        failed += check_spans(&cases[i]);
    }

    /* Test 13: patterns that must be reported as bad rather than quietly
     * reinterpreted as literal text.  Emacs signals an error for every
     * BAD_PATTERN entry here and accepts every OK one. */
    {
      static const struct compile_case cases[] = {
          {"x\\{0,1\\}", RE_STATUS_OK},
          {"a\\{0\\}", RE_STATUS_OK},
          {"a\\{2,2\\}", RE_STATUS_OK},
          {"a\\{65535\\}", RE_STATUS_OK},
          {"a\\{2,1\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{3,1\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{65536\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{,65536\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{100000\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{99999999999999\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{x\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{ 1\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{-1\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{1,2,3\\}", RE_STATUS_BAD_PATTERN},
          {"a\\{1", RE_STATUS_BAD_PATTERN},
          {"\\{", RE_STATUS_BAD_PATTERN},
          {"[[:blank:]]", RE_STATUS_OK},
          {"[[:word:]]", RE_STATUS_OK},
          {"[[:nonascii:]]", RE_STATUS_OK},
          {"[[:foo:]]", RE_STATUS_BAD_PATTERN},
          {"[[:Alpha:]]", RE_STATUS_BAD_PATTERN},
          {"[[:digitx:]]", RE_STATUS_BAD_PATTERN},
          {"[[::]]", RE_STATUS_BAD_PATTERN},
          /* Emacs accepts these two, but their meaning is a property of
           * the string's representation, which a byte matcher cannot
           * answer; rejecting beats answering wrongly. */
          {"[[:multibyte:]]", RE_STATUS_BAD_PATTERN},
          {"[[:unibyte:]]", RE_STATUS_BAD_PATTERN},
      };
      size_t i;
      for (i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        failed += check_compile(&cases[i]);
    }

    /* Test 13b: the rest of the acceptance contract -- groups, bracket
     * expressions and quantifiers with nothing (or too much) to repeat.
     * Emacs agrees with every row but "\(?\)", where its rejection is an
     * artifact of reserving "\(?" for shy groups, which this engine does
     * not have. */
    {
      static const struct compile_case cases[] = {
          /* a group has to be closed, and closed only once */
          {"\\(a\\)", RE_STATUS_OK},
          {"\\(\\(a\\)\\)", RE_STATUS_OK},
          {"\\(", RE_STATUS_BAD_PATTERN},
          {"\\(a", RE_STATUS_BAD_PATTERN},
          {"\\(\\(a\\)", RE_STATUS_BAD_PATTERN},
          {"\\(a\\|b", RE_STATUS_BAD_PATTERN},
          {"a\\)", RE_STATUS_BAD_PATTERN},
          {"\\)", RE_STATUS_BAD_PATTERN},
          {"\\(a\\)\\)", RE_STATUS_BAD_PATTERN},
          /* a bracket expression has to be closed; a ']' in the first
           * position is a member, so "[]" is unterminated */
          {"[a]", RE_STATUS_OK},
          {"[]a]", RE_STATUS_OK},
          {"[]]", RE_STATUS_OK},
          {"[^]]", RE_STATUS_OK},
          {"[a", RE_STATUS_BAD_PATTERN},
          {"[^a", RE_STATUS_BAD_PATTERN},
          {"[]", RE_STATUS_BAD_PATTERN},
          {"[^]", RE_STATUS_BAD_PATTERN},
          {"[", RE_STATUS_BAD_PATTERN},
          {"[^", RE_STATUS_BAD_PATTERN},
          {"[a-", RE_STATUS_BAD_PATTERN},
          {"[[:digit:]", RE_STATUS_BAD_PATTERN},
          /* nothing to repeat: the quantifier is the literal character */
          {"*", RE_STATUS_OK},
          {"+", RE_STATUS_OK},
          {"?", RE_STATUS_OK},
          {"^*", RE_STATUS_OK},
          {"$*", RE_STATUS_OK},
          {"\\(*\\)", RE_STATUS_OK},
          {"\\(?\\)", RE_STATUS_OK},
          {"\\|*", RE_STATUS_OK},
          {"a\\|*b", RE_STATUS_OK},
          /* too much to repeat: the atom already carries a quantifier */
          {"a++", RE_STATUS_BAD_PATTERN},
          {"a**", RE_STATUS_BAD_PATTERN},
          {"a*+", RE_STATUS_BAD_PATTERN},
          {"a?*", RE_STATUS_BAD_PATTERN},
          {"a*?", RE_STATUS_BAD_PATTERN},
          {"a\\{2\\}*", RE_STATUS_BAD_PATTERN},
          {"a\\{2\\}\\{3\\}", RE_STATUS_BAD_PATTERN},
          {"\\(a\\)**", RE_STATUS_BAD_PATTERN},
          /* ... but a quantifier on a closed group is ordinary */
          {"\\(a\\)*", RE_STATUS_OK},
          {"\\(a\\)\\{2\\}", RE_STATUS_OK},
      };
      size_t i;
      for (i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        failed += check_compile(&cases[i]);
    }

    /* Test 13c: what the newly accepted literal quantifiers and bracket
     * members actually match.  GNU Emacs 31 reports each of these spans. */
    {
      static const struct span_case cases[] = {
          {"*", "*", 1, {{0, 1}}, 0},
          {"+", "+", 1, {{0, 1}}, 0},
          {"?", "?", 1, {{0, 1}}, 0},
          {"^*", "*", 1, {{0, 1}}, 0},
          {"\\(*\\)", "*", 2, {{0, 1}, {0, 1}}, 0},
          {"a\\|*b", "*b", 1, {{0, 2}}, 0},
          {"[]a]", "]", 1, {{0, 1}}, 0},
          {"[]a]", "a", 1, {{0, 1}}, 0},
          {"[]]", "]", 1, {{0, 1}}, 0},
          {"[^]]", "a", 1, {{0, 1}}, 0},
          {"[^]]", "]", 0, {{0, 0}}, 0},
      };
      size_t i;
      for (i = 0; i < sizeof(cases) / sizeof(*cases); i++)
        failed += check_spans(&cases[i]);
    }

    /* Test 14: the caller-storage alignment contract.
     *
     * A compiled program is read in place through the private node type,
     * so storage that is not RE_STORAGE_ALIGNMENT-aligned has to be
     * refused.  Compiling into it and executing from it used to be
     * undefined behavior that UBSan reports inside re_exec(). */
    {
      static _Alignas(RE_STORAGE_ALIGNMENT) unsigned char raw[512];
      const char* pat = "\\(a\\)b";
      int round;

      unsigned odd_size = sizeof(raw) - 1;
      re_t odd_regex = (re_t)(void*)raw; /* must be cleared */
      status =
          re_compile_checked(pat, RE_FLAG_NONE, raw + 1, &odd_size, &odd_regex);
      if (status != RE_STATUS_BAD_PATTERN) {
        fprintf(stderr,
                "FAIL: re_compile_checked into misaligned storage expected "
                "BAD_PATTERN, got %d\n",
                status);
        failed++;
      }
      if (odd_regex) {
        fprintf(stderr,
                "FAIL: re_compile_checked into misaligned storage left a "
                "non-NULL regex\n");
        failed++;
      }

      unsigned odd_to_size = sizeof(raw) - 1;
      if (re_compile_to(pat, raw + 1, &odd_to_size)) {
        fprintf(stderr,
                "FAIL: re_compile_to into misaligned storage compiled "
                "anyway\n");
        failed++;
      }

      /* Aligned storage still works, twice into the same buffer. */
      for (round = 0; round < 2; round++) {
        unsigned even_size = sizeof(raw) - 2;
        re_t even_regex = NULL;
        re_match_result res;

        status = re_compile_checked(pat, RE_FLAG_NONE, raw + 2, &even_size,
                                    &even_regex);
        if (status != RE_STATUS_OK || !even_regex) {
          fprintf(stderr,
                  "FAIL: re_compile_checked into aligned storage (round %d) "
                  "returned %d\n",
                  round, status);
          failed++;
          continue;
        }
        status = re_exec(even_regex, "xab", 0, &res);
        if (status != RE_STATUS_OK || res.spans[0].start != 1 ||
            res.spans[0].end != 3 || res.spans[1].start != 1 ||
            res.spans[1].end != 2) {
          fprintf(stderr,
                  "FAIL: re_exec from aligned storage (round %d): status %d, "
                  "spans [%d, %d) [%d, %d)\n",
                  round, status, res.spans[0].start, res.spans[0].end,
                  res.spans[1].start, res.spans[1].end);
          failed++;
        }
      }

      /* NULL storage with *storage_size == 0 is the size query, and the
       * size it reports is enough to compile into. */
      unsigned query_size = 0;
      re_t query_regex = NULL;
      status =
          re_compile_checked(pat, RE_FLAG_NONE, NULL, &query_size, &query_regex);
      if (status != RE_STATUS_BUFFER_TOO_SMALL || query_size == 0 ||
          query_regex) {
        fprintf(stderr,
                "FAIL: size query expected BUFFER_TOO_SMALL with a non-zero "
                "size, got %d / %u\n",
                status, query_size);
        failed++;
      } else {
        unsigned exact_size = query_size;
        re_t exact_regex = NULL;
        status = re_compile_checked(pat, RE_FLAG_NONE, raw, &exact_size,
                                    &exact_regex);
        if (status != RE_STATUS_OK || exact_size != query_size) {
          fprintf(stderr,
                  "FAIL: compiling into the queried size returned %d (%u vs "
                  "%u)\n",
                  status, exact_size, query_size);
          failed++;
        }
      }
    }

    /* Test 15: the storage boundary, swept one byte at a time.
     *
     * "\xZ" is the interesting pattern: its fallback emits three nodes
     * from a single loop iteration, and those extra nodes used to sidestep
     * the loop's own bounds check.  For every capacity the invariants are:
     * nothing is written at or past the caller's limit; a compile that
     * succeeds compiled the *whole* pattern, never a prefix of it, and so
     * reports the same size and the same spans as a roomy one; and no
     * capacity below the reported size can succeed.  re_compile_to() wants
     * one node's slack over that size, which is why the sweep runs past
     * it (re_compile_checked() has no such slack -- it compiles into its
     * own buffer and copies the exact bytes out). */
    {
      static const char* const pats[] = {"\\xZ", "\\x4X", "\\(a\\)\\xZ\\(b\\)",
                                         "a[0-9]+", "[[:digit:]]\\{2,4\\}"};
      size_t pi;

      for (pi = 0; pi < sizeof(pats) / sizeof(*pats); pi++) {
        const char* pat = pats[pi];
        unsigned need = 0;
        re_t sized = NULL;
        unsigned cap;
        int accepted = 0;

        if (re_compile_checked(pat, RE_FLAG_NONE, NULL, &need, &sized) !=
                RE_STATUS_BUFFER_TOO_SMALL ||
            need == 0 || need > 64) {
          fprintf(stderr, "FAIL: size query for \"%s\" gave %u\n", pat, need);
          failed++;
          continue;
        }
        /* The size the query reports is enough for re_compile_checked. */
        {
          _Alignas(RE_STORAGE_ALIGNMENT) unsigned char exact[64];
          unsigned exact_size = need;
          re_t exact_regex = NULL;
          if (re_compile_checked(pat, RE_FLAG_NONE, exact, &exact_size,
                                 &exact_regex) != RE_STATUS_OK) {
            fprintf(stderr,
                    "FAIL: re_compile_checked(\"%s\") into its own reported "
                    "%u bytes failed\n",
                    pat, need);
            failed++;
          }
        }

        for (cap = 1; cap <= need + sizeof(void*) * 2; cap++) {
          _Alignas(RE_STORAGE_ALIGNMENT) unsigned char buf[128];
          unsigned sz = cap;
          re_t got;
          unsigned k;

          memset(buf, 0xAA, sizeof(buf));
          got = re_compile_to(pat, buf, &sz);
          if (got && cap < need) {
            fprintf(stderr,
                    "FAIL: re_compile_to(\"%s\") compiled into %u bytes, "
                    "under the %u it needs\n",
                    pat, cap, need);
            failed++;
          }
          if (got && sz != need) {
            fprintf(stderr,
                    "FAIL: re_compile_to(\"%s\") into %u bytes reported size "
                    "%u, expected %u -- a prefix of the pattern?\n",
                    pat, cap, sz, need);
            failed++;
          }
          accepted += (got != NULL);
          for (k = cap; k < sizeof(buf); k++) {
            if (buf[k] != 0xAA) {
              fprintf(stderr,
                      "FAIL: re_compile_to(\"%s\") into %u bytes wrote past "
                      "the buffer, at offset %u\n",
                      pat, cap, k);
              failed++;
              break;
            }
          }
        }
        if (!accepted) {
          fprintf(stderr,
                  "FAIL: re_compile_to(\"%s\") never compiled, even with "
                  "slack over its %u bytes\n",
                  pat, need);
          failed++;
        }
      }
    }
  }

  failed += test_execution_state();

  printf("%d/%d tests succeeded.\n", failed == 0, 1);
  return failed ? 1 : 0;
}
