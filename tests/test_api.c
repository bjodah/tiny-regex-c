/*
 * Exercises the public API entry points (re_compile_to, re_size,
 * re_compare, re_string) that the hand-picked/random pattern suites never
 * touch, since those only call re_match()/re_compile().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re.h"

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
  unsigned char storage[512];
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
  unsigned char storage[256];
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

int main(void) {
  int failed = 0;

  unsigned char buf1[256];
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

  unsigned char buf2[256];
  unsigned size2 = sizeof(buf2);
  re_t p2 = re_compile_to("a[0-9]+", buf2, &size2);
  if (re_compare(p1, p2) != 0) {
    fprintf(stderr, "re_compare: identical patterns compared unequal\n");
    failed++;
  }

  unsigned char buf3[256];
  unsigned size3 = sizeof(buf3);
  re_t p3 = re_compile_to("b[0-9]+", buf3, &size3);
  if (re_compare(p1, p3) == 0) {
    fprintf(stderr, "re_compare: different patterns compared equal\n");
    failed++;
  }

  unsigned char buf4[256];
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
    unsigned char storage[256];
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
    unsigned char storage_invalid[256];
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
    unsigned char storage_small[5];
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
    unsigned char z_storage[256];
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
    unsigned char posix_storage[256];
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
    unsigned char dialect_storage[256];
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
    unsigned char icase_storage[256];
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
      unsigned char test10_storage[256];
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
          /* byte-oriented: this is the lead byte of the two-byte 'å'.
           * kg's wrapper widens such a span to the whole glyph. */
          {"[[:nonascii:]]", "a\xc3\xa5", 1, {{1, 2}}, 0},
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
  }

  printf("%d/%d tests succeeded.\n", failed == 0, 1);
  return failed ? 1 : 0;
}
