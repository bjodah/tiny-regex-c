/*
 * Exercises the public API entry points (re_compile_to, re_size,
 * re_compare, re_string) that the hand-picked/random pattern suites never
 * touch, since those only call re_match()/re_compile().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "re.h"

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
  }

  printf("%d/%d tests succeeded.\n", failed == 0, 1);
  return failed ? 1 : 0;
}
