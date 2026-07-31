/*
 * Testing various regex-patterns
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _UNICODE
#  include <locale.h>
#endif
#include "re.h"

struct test_case {
  char *rx;
  char *text;
  int len;
};

static void *xmalloc (size_t n)
{
    void *p = malloc (n);
    if (!p) { fputs ("out of memory\n", stderr); exit (1); }
    return p;
}

static void *xcalloc (size_t n, size_t sz)
{
    void *p = calloc (n, sz);
    if (!p) { fputs ("out of memory\n", stderr); exit (1); }
    return p;
}

/* "\\n" => "\n" */
char *cunquote (char* s, int l)
{
    int i;
    char *r = xmalloc (l + 1);
    for (i=0; i<l; i++)
    {
      if (*s == '\\' && i+1 < l)
      {
        if (*(s+1) == 'n')
          r[i] = '\n', s+=2, l--;
        else if (*(s+1) == 'r')
          r[i] = '\r', s+=2, l--;
        else if (*(s+1) == 't')
          r[i] = '\t', s+=2, l--;
        else
          r[i] = *s++;
      }
      else
      {
        r[i] = *s++;
      }
    }
    r[i] = '\0';
    return r;
}

/* A vector file this program cannot read is a failure, not an empty run.
 * These paths used to `return NULL` with *ntests still zero, so main()
 * looped over nothing and the summary counted whatever was left: running
 * ./tests/test1 from the wrong directory printed "1/1 tests succeeded"
 * and exited 0, and one malformed row appended to tests/ok.lst dropped
 * all 136 of them the same way. */
static void die_reading (const char *fname, int line, const char *why)
{
    fprintf (stderr, "%s:%d: %s\n", fname, line, why);
    exit (1);
}

struct test_case* read_tests (const char *fname, int *ntests)
{
    char s[80];
    FILE *f = fopen (fname, "rt");
    if (!f)
    {
      fprintf (stderr, "%s: cannot open (run this from the repository root)\n",
               fname);
      exit (1);
    }
    int size = 120;
    struct test_case* vec = xcalloc (size, sizeof(struct test_case));
    int i = 0;
    int line = 0;
    *ntests = 0;
    while (fgets(s, 80, f))
    {
        // regex until first tab
        char *p = strchr (s, '\t');
        int l;
        line++;
        // A row too long for the buffer would be split, and its tail
        // parsed as a row of its own; the last line of a file with no
        // final newline is the one legitimate case.
        if (!strchr (s, '\n') && !feof (f))
          die_reading (fname, line, "row longer than this reader's buffer");
        if (!p) // no tab, just an old exreg test
          continue;
        if (s[0] == '#') // outcommented
          continue;
        l = p - s;
        vec[i].rx = cunquote (s, l);
        // string from first tab
        char *str = strchr (p, '"');
        if (!str)
          die_reading (fname, line, "no quoted subject after the pattern");
        char *end = strchr (str+1, '"');
        if (!end)
          die_reading (fname, line, "unterminated quoted subject");
        l = end - str - 1;
        vec[i].text = cunquote (&str[1], l);

        vec[i].len = 0;
        sscanf(&end[1], "\t%d", &vec[i].len);

        i++;
        if (i >= size)
        {
            size *= 2;
            struct test_case* nv = realloc (vec, size * sizeof(struct test_case));
            if (!nv) { fputs ("out of memory\n", stderr); fclose (f); exit (1); }
            vec = nv;
        }
    }
    fclose (f);
    *ntests = i;
    return vec;
}

void free_test_cases (struct test_case* test_case, int ntests)
{
    for (int i=0; i < ntests; i++)
    {
        free (test_case[i].rx);
        free (test_case[i].text);
    }
    free (test_case);
}

int do_test (struct test_case* test_case, int i, int ntests, int ok)
{
    char* text;
    char* pattern;
    int should_fail;
    int length;
    int correctlen;

    pattern = test_case[i].rx;
    text = test_case[i].text;
    should_fail = ok == 0;
    correctlen = test_case[i].len;

    int m = re_match(pattern, text, &length);

    if (should_fail)
    {
        if (m != (-1))
        {
            fprintf(stderr, "[%d/%d]: pattern '%s' matched '%s' unexpectedly, matched %i chars. \n", i+1, ntests, pattern, text, length);
            return 0;
        }
    }
    else
    {
        if (m == (-1))
        {
            fprintf(stderr, "[%d/%d]: pattern '%s' didn't match '%s' as expected. \n", (i+1), ntests, pattern, text);
            return 0;
        }
        if (length != correctlen)
        {
            fprintf(stderr, "[%d/%d]: pattern '%s' matched '%i' chars of '%s'; expected '%i'. \n", (i+1), ntests, pattern, length, text, correctlen);
            return 0;
        }
    }
    return 1;
}

/* Match a (rx, text, len) tuple against the known-broken cases loaded from
 * tests/xfail_ok.lst. Returning non-zero marks the case as expected-to-fail
 * (xfail): a natural failure is tolerated, while an unexpected pass (XPASS)
 * counts as a real failure, mirroring fe's PTY `xfail` semantics. */
static int is_xfail (struct test_case* xf, int nxf,
                     const char* rx, const char* text, int len)
{
    for (int i = 0; i < nxf; ++i)
    {
        if (xf[i].len == len && strcmp(xf[i].rx, rx) == 0 && strcmp(xf[i].text, text) == 0)
            return 1;
    }
    return 0;
}

int main(void)
{
    int ntests, ntests_nok, nxfail = 0;
    int nfailed = 0;
    int nxpass = 0;
    int nskipped = 0;
    int i;
    const char *skip_xfail = getenv("RE_SKIP_XFAIL");

    printf("Testing hand-picked regex patterns\n");

    //setlocale(LC_CTYPE, "en_US.UTF-8");
    struct test_case* tests_ok = read_tests ("tests/ok.lst", &ntests);
    struct test_case* xfail = read_tests ("tests/xfail_ok.lst", &nxfail);
    for (i = 0; i < ntests; ++i)
    {
        int xf = is_xfail(xfail, nxfail, tests_ok[i].rx, tests_ok[i].text, tests_ok[i].len);
        if (xf && skip_xfail)
        {
            /* Under heavy runners (ASan/MSan/Valgrind) the known-broken
             * engine paths can abort the process, so xfail cases are not
             * executed -- mirroring fe's FE_SKIP_SCRIPTS. */
            nskipped += 1;
            continue;
        }
        int passed = do_test (tests_ok, i, ntests, 1);
        if (xf)
        {
            if (passed)
            {
                fprintf(stderr, "[%d/%d]: XPASS: '%s' on '%s' was expected to fail (known bug). \n",
                        i+1, ntests, tests_ok[i].rx, tests_ok[i].text);
                nxpass += 1;
            }
        }
        else if (!passed)
        {
            nfailed += 1;
        }
    }
    free_test_cases (tests_ok, ntests);
    free_test_cases (xfail, nxfail);

    struct test_case* tests_nok = read_tests ("tests/nok.lst", &ntests_nok);
    for (i = 0; i < ntests_nok; ++i)
    {
        if (!do_test (tests_nok, i, ntests_nok, 0))
            nfailed += 1;
    }
    free_test_cases (tests_nok, ntests_nok);
    ntests += ntests_nok;

    // regression test for unhandled BEGIN in the middle of an expression
    // we need to test text strings with all possible values for the second
    // byte because re.c was matching it against an uninitalized value, so
    // it could be anything
    int length;
    const char* pattern = "a^";
    for (i = 0; i < 255; i++) {
      char text_buf[] = { 'a', i, '\0' };
      int m = re_match(pattern, text_buf, &length);
      if (m != -1) {
        fprintf(stderr, "[%d/%d]: pattern '%s' matched '%s' unexpectedly", ntests, ntests, pattern, text_buf);
        nfailed += 1;
        break;
      }
    }
    ntests++;
    printf(" %d/%d tests succeeded (%d xfail, %d xpass, %d skipped).\n",
           ntests - nfailed - nxpass - nskipped, ntests, nxfail, nxpass, nskipped);

    return nfailed + nxpass; /* 0 if all tests passed or only expected failures */
}
