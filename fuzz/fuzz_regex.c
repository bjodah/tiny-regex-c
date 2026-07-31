#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "re.h"

enum { MaxFieldSize = 4096 };

/* Split the fuzzer input into a pattern and a subject text (separated by the
 * first '\n'), then exercise both the static-buffer entry point (re_match)
 * and the user-supplied-buffer entry point (re_compile_to) with a small,
 * input-derived buffer size to also cover its bounds checking.
 *
 * Input format: the first byte selects the small buffer's size, 1..64; the
 * rest splits at the first '\n' into pattern and subject text. */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1 || size > MaxFieldSize)
    return 0;

  const unsigned char buf_size_selector = data[0];
  data += 1;
  size -= 1;

  const uint8_t* sep = memchr(data, '\n', size);
  const size_t pat_len = sep ? (size_t)(sep - data) : size;
  const uint8_t* text_data = sep ? sep + 1 : data + size;
  const size_t text_len = sep ? size - pat_len - 1 : 0;

  char pattern[MaxFieldSize + 1];
  char text[MaxFieldSize + 1];
  memcpy(pattern, data, pat_len);
  pattern[pat_len] = '\0';
  memcpy(text, text_data, text_len);
  text[text_len] = '\0';

  int length;
  (void)re_match(pattern, text, &length);

  /* Caller storage at both alignments.  RE_STORAGE_ALIGNMENT-aligned
   * storage compiles as usual; storage one byte past it must be refused
   * outright rather than compiled into and then read through. */
  _Alignas(RE_STORAGE_ALIGNMENT) unsigned char small_raw[65];
  unsigned char* small_buf = small_raw;
  unsigned bufsize = (buf_size_selector % 64u) + 1;

  unsigned misaligned_size = bufsize;
  assert(re_compile_to(pattern, small_raw + 1, &misaligned_size) == NULL);

  re_t p = re_compile_to(pattern, small_buf, &bufsize);
  if (p) {
    int length2;
    (void)re_matchp(p, text, &length2);
  }

  return 0;
}
