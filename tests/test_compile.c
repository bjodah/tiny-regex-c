/*

This file tests two bug patterns reported by @DavidKorczynski in
https://github.com/kokke/tiny-regex-c/issues/44

And some structural issues with nested groups.

*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h> /* for NULL */
#include "re.h"

/* Compile 'pattern', run it over 'text', and compare the first 'nspans'
 * reported spans with 'spans'. Returns the number of checks that failed. */
static int check_spans(const char *pattern, const char *text,
                       int nspans, const int spans[][2])
{
  _Alignas(RE_STORAGE_ALIGNMENT) unsigned char storage[512];
  unsigned size = sizeof(storage);
  re_t regex = NULL;
  re_match_result res;
  int failed = 0;
  int i;

  if (re_compile_checked(pattern, RE_FLAG_NONE, storage, &size, &regex)
      != RE_STATUS_OK)
  {
    printf(" re_compile_checked(\"%s\") failed.\n", pattern);
    return 1;
  }
  if (re_exec(regex, text, 0, &res) != RE_STATUS_OK)
  {
    printf(" \"%s\" did not match \"%s\".\n", pattern, text);
    return 1;
  }
  if (res.nspans != nspans)
  {
    printf(" \"%s\" on \"%s\": nspans %d, expected %d.\n",
           pattern, text, res.nspans, nspans);
    failed++;
  }
  for (i = 0; i < nspans && i < res.nspans; i++)
  {
    if (res.spans[i].start != spans[i][0] || res.spans[i].end != spans[i][1])
    {
      printf(" \"%s\" on \"%s\": span %d [%d, %d), expected [%d, %d).\n",
             pattern, text, i, res.spans[i].start, res.spans[i].end,
             spans[i][0], spans[i][1]);
      failed++;
    }
  }
  return failed;
}

int main(void)
{
  size_t i;
  int failed = 0;
  int unexpected = 0;
  int ntests = 0;
  printf("Testing handling of invalid regex patterns:\n");
  const char *const tests[] = {
    /* Test 1: inverted set without a closing ']' */
    "\\\x01[^\\\xff][^",
    /* Test 2: set with an incomplete escape sequence and without a closing ']' */
    "\\\x01[^\\\xff][\\",
    /* Invalid escape. '\\' as last char without previous \\ */
    "\\",
    /* incomplete char classes */
    "[^", "[abc\\",
    /* overlong char classes */
    "[0123456789012345678901234567890123456789]",
    "[01234567890123456789\\0123456789012345678]",
    "[00000000000000000000000000000000000000][",
    /* quantifiers without context: nothing to repeat at position 0 */
    "+", "?", "*",
    /* Tests 7-12: invalid quantifiers. */
    /* note that python and perl allows these, and matches them exact. */
    // "{2}", "x{}", "x{1,2,}", "x{,2,}", "x{-2}",
    /* intervals Emacs rejects; they used to compile as the literal
     * characters of their own spelling */
    "a\\{2,1\\}", "a\\{65536\\}", "a\\{x\\}", "a\\{1,2,3\\}", "a\\{1", "\\{",
    /* POSIX class names Emacs rejects, or whose meaning a byte-oriented
     * matcher cannot honour; they used to become a set of the characters
     * spelling the name, so "[[:blank:]]" matched 'a' */
    "[[:foo:]]", "[[:multibyte:]]",
  };
  /* Indices 5,6,7 are overlong char-classes that re_compile() currently
   * accepts instead of rejecting; documented as a known bug (xfail). An
   * unexpected rejection there is an XPASS that fails CI. */
  const size_t ntests_invalid = sizeof(tests)/sizeof(*tests);
  const int xfail_invalid[] = {0,0,0,0,0,1,1,1,0,0,0,
                               0,0,0,0,0,0,
                               0,0};

  for (i = 0; i < ntests_invalid; i++)
  {
    const char *s = tests[i];
    re_t p = re_compile(s);
    int compiled = (p != NULL);
    ntests++;
    if (xfail_invalid[i])
    {
      /* expected to compile (bug): reject == XPASS */
      if (!compiled)
      {
        printf(" [%d] XPASS: re_compile(\"%s\") now correctly rejects.\n", ntests, s);
        unexpected++;
      }
    }
    else if (compiled)
    {
      printf(" [%d] re_compile(\"%s\") must not compile.\n", ntests, s);
      failed++;
    }
  }
  printf(" %d/%d tests succeeded.\n", ntests-failed-unexpected, ntests);

  /* Nested groups used to be checked by reading a private regex_t layout
   * copied into this file, which had gone stale -- three of those reads
   * were carried as xfails because they inspected the wrong bytes. What
   * the compiler owes its caller is the spans, so assert on those; GNU
   * Emacs 31 reports exactly these. */
  printf("Testing compilation of nested groups:\n");
  {
    static const int abbb[3][2] = {{0, 4}, {3, 4}, {0, 2}};
    static const int bab[3][2]  = {{0, 3}, {1, 3}, {1, 3}};
    static const int axb[3][2]  = {{0, 3}, {0, 1}, {2, 3}};
    static const int aXZb[3][2] = {{0, 5}, {0, 1}, {4, 5}};
    static const int acabc[3][2] = {{0, 5}, {2, 5}, {3, 4}};

    ntests++; failed += check_spans("\\(\\(ab\\)\\|b\\)+", "abbb", 3, abbb);
    ntests++; failed += check_spans("\\(\\(ab\\)\\|b\\)+", "bab", 3, bab);
    ntests++; failed += check_spans("\\(a\\)x\\(b\\)", "axb", 3, axb);
    /* The invalid "\x" fallback emits three nodes; when the node count
     * lagged behind, the *following* group's "\)" scan ran off the front
     * of the program and this pattern was rejected outright. */
    ntests++; failed += check_spans("\\(a\\)\\xZ\\(b\\)", "a\\xZb", 3, aXZb);
    ntests++; failed += check_spans("\\(a\\(b\\)?c\\)+", "acabc", 3, acabc);
  }

  printf(" %d/%d tests succeeded.\n", ntests-failed-unexpected, ntests);
  return (failed + unexpected) ? 1 : 0;
}

