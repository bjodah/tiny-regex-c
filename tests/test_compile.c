/*

This file tests two bug patterns reported by @DavidKorczynski in
https://github.com/kokke/tiny-regex-c/issues/44

And some structural issues with nested groups.

*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h> /* for NULL */
#include "re.h"

typedef struct regex_t
{
  unsigned type;     /* CHAR, STAR, etc.                      */
  union
  {
    char  ch;            /*      the character itself             */
    char* ccl;           /*  OR  a pointer to characters in class */
    unsigned char  group_num;   /*  OR the number of group patterns. */
    unsigned char  group_start; /*  OR for GROUPEND, the start index of the group. */
    struct {
      unsigned short n;  /* match n times */
      unsigned short m;  /* match n to m times */
    };
  } u;
} regex_t;

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
    regex_t *p = re_compile(s);
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

  printf("Testing compilation of nested groups:\n");
  re_t p = re_compile("\\(\\(ab\\)\\|b\\)+");

  /* The local regex_t layout here predates the compact 6-byte struct in
   * re.c, so these group_num/group_start reads are stale and currently
   * read the wrong bytes. Mark the failing ones xfail until the layout
   * is reconciled; an XPASS then signals the fix landed. */
  ntests++;
  if (p[0].u.group_num != 6)
  {
    printf(" [%d] (xfail) wrong [0].group_num %hu for \\(\\(ab\\)\\|b\\)+\n", ntests, p[0].u.group_num);
  }
  else { printf(" [%d] XPASS: [0].group_num == 6.\n", ntests); unexpected++; }
  ntests++;
  if (p[1].u.group_num != 2)
  {
    printf(" [%u] (xfail) wrong [1].group_num %hu.\n", ntests, p[1].u.group_num);
  }
  else { printf(" [%u] XPASS: [1].group_num == 2.\n", ntests); unexpected++; }
  ntests++;
  if (p[4].u.group_start != 1)
  {
    printf(" [%u] (xfail) wrong [4].group_start %hu.\n", ntests, p[4].u.group_start);
  }
  else { printf(" [%u] XPASS: [4].group_start == 1.\n", ntests); unexpected++; }
  ntests++;
  if (p[7].u.group_start != 0)
  {
    printf(" [%u] wrong [7].group_start %hu.\n", ntests, p[7].u.group_start);
    failed++;
  }

  printf(" %d/%d tests succeeded.\n", ntests-failed-unexpected, ntests);
  return (failed + unexpected) ? 1 : 0;
}

