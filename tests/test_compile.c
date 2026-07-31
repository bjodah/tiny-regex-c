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
    /* unterminated bracket expressions. A ']' in the first position is a
     * member, so "[]" and "[^]" are unterminated too -- Emacs' reading. */
    "[^", "[abc\\", "[a", "[]", "[^]",
    "[00000000000000000000000000000000000000][",
    /* intervals Emacs rejects; they used to compile as the literal
     * characters of their own spelling */
    "a\\{2,1\\}", "a\\{65536\\}", "a\\{x\\}", "a\\{1,2,3\\}", "a\\{1", "\\{",
    /* quantifiers applied to an already-quantified atom. Emacs folds the
     * two into one; this engine has no faithful spelling for that and
     * used to compile a node the matcher could only ever fail on. */
    "a++", "a**", "a*+", "a?*", "a\\{2\\}*", "a\\{2\\}\\{3\\}",
    /* groups that are never closed, or closed without being opened */
    "\\(", "\\(a", "\\(\\(a\\)", "a\\)", "\\)", "\\(a\\)\\)",
    /* POSIX class names Emacs rejects, or whose meaning a byte-oriented
     * matcher cannot honour; they used to become a set of the characters
     * spelling the name, so "[[:blank:]]" matched 'a' */
    "[[:foo:]]", "[[:multibyte:]]",
  };
  const size_t ntests_invalid = sizeof(tests)/sizeof(*tests);

  for (i = 0; i < ntests_invalid; i++)
  {
    const char *s = tests[i];
    ntests++;
    if (re_compile(s) != NULL)
    {
      printf(" [%d] re_compile(\"%s\") must not compile.\n", ntests, s);
      failed++;
    }
  }
  printf(" %d/%d tests succeeded.\n", ntests-failed-unexpected, ntests);

  /* Patterns the invalid list used to carry as expected-to-compile bugs.
   * The class buffer they were too long for has been gone since class data
   * moved inline with its own bounds checks; they are ordinary patterns. */
  printf("Testing acceptance of long bracket expressions:\n");
  {
    static const char *const valid[] = {
      "[0123456789012345678901234567890123456789]",
      "[01234567890123456789\\0123456789012345678]",
    };
    size_t v;
    for (v = 0; v < sizeof(valid)/sizeof(*valid); v++)
    {
      ntests++;
      if (re_compile(valid[v]) == NULL)
      {
        printf(" [%d] re_compile(\"%s\") must compile.\n", ntests, valid[v]);
        failed++;
      }
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

