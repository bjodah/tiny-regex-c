/*
 * Exercises the public API entry points (re_compile_to, re_size,
 * re_compare, re_string) that the hand-picked/random pattern suites never
 * touch, since those only call re_match()/re_compile().
 */

#include <stdio.h>
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
    fprintf(stderr,
            "re_compare: patterns of different size compared equal\n");
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

  printf("%d/%d tests succeeded.\n", failed == 0, 1);
  return failed ? 1 : 0;
}
