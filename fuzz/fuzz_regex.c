#include <stdint.h>
#include <string.h>

#include "re.h"

enum { MaxFieldSize = 4096 };

/* Split the fuzzer input into a pattern and a subject text (separated by the
 * first '\n'), then exercise both the static-buffer entry point (re_match)
 * and the user-supplied-buffer entry point (re_compile_to) with a small,
 * input-derived buffer size to also cover its bounds checking. */
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

  unsigned char small_buf[64];
  unsigned bufsize = (buf_size_selector % sizeof(small_buf)) + 1;
  re_t p = re_compile_to(pattern, small_buf, &bufsize);
  if (p) {
    int length2;
    (void)re_matchp(p, text, &length2);
  }

  return 0;
}
