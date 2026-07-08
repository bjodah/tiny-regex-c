/*
 *
 * Mini regex-module inspired by Rob Pike's regex code described in:
 *
 * http://www.cs.princeton.edu/courses/archive/spr09/cos333/beautiful.html
 *
 *
 *
 * Supports:
 * ---------
 *   '.'        Dot, matches any byte except newline
 *   '^'        Start anchor, matches beginning of string
 *   '$'        End anchor, matches end of string
 *   '*'        Asterisk, match zero or more (greedy)
 *   '+'        Plus, match one or more (greedy)
 *   '?'        Question, match zero or one (non-greedy)
 *   '[abc]'    Character class, match if one of {'a', 'b', 'c'}
 *   '[^abc]'   Inverted class, match if NOT one of {'a', 'b', 'c'}
 *   '[a-zA-Z]' Character ranges, the character set of the ranges { a-z | A-Z }
 *   '\s'       Whitespace, \t \f \r \n \v and spaces
 *   '\S'       Non-whitespace
 *   '\w'       Alphanumeric, [a-zA-Z0-9_]
 *   '\W'       Non-alphanumeric
 *   '\d'       Digits, [0-9]
 *   '\D'       Non-digits
 *   '\xXX'     Hex-encoded byte
 *   '\|'       Branch Or, e.g. a\|A, \w\|\s
 *   '\{n\}'    Match n times
 *   '\{n,\}'   Match n or more times
 *   '\{,m\}'   Match m or less times
 *   '\{n,m\}'  Match n to m times
 *   '\(...\)'  Group, including a trailing quantifier applied to the group
 *
 */

#ifndef TINY_REGEX_C_RE_H
#define TINY_REGEX_C_RE_H

#ifndef RE_DOT_MATCHES_NEWLINE
/* Define to 1 if you want '.' to match '\r' and '\n'. */
#define RE_DOT_MATCHES_NEWLINE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Typedef'd pointer to get abstract datatype. */
typedef struct regex_t* re_t;

typedef enum {
  RE_STATUS_OK = 0,
  RE_STATUS_NO_MATCH,
  RE_STATUS_BAD_PATTERN,
  RE_STATUS_TOO_COMPLEX,
  RE_STATUS_BUFFER_TOO_SMALL
} re_status;

typedef enum { RE_FLAG_NONE = 0, RE_FLAG_ICASE = 1 << 0 } re_flags;

typedef struct {
  int start;
  int end;
} re_span;

#define RE_MAX_SPANS 10

typedef struct {
  int nspans;
  re_span spans[RE_MAX_SPANS];
} re_match_result;

#define RE_MAX_COMPILED_BYTES 8192

/* Checked compile and execute APIs.  On RE_STATUS_BUFFER_TOO_SMALL,
 * *storage_size is set to the required byte count.  Passing NULL storage with
 * *storage_size == 0 is the supported way to query that size. */
re_status re_compile_checked(const char* pattern,
                             re_flags flags,
                             unsigned char* storage,
                             unsigned* storage_size,
                             re_t* out);

re_status re_exec(re_t regex,
                  const char* text,
                  int start_offset,
                  re_match_result* out);

/* Compile regex string pattern to custom buffer, returning # of bytes used */
re_t re_compile_to(const char* pattern,
                   unsigned char* re_data,
                   unsigned* bytes);

/* Compile regex string pattern to a regex_t-array, using internal buffer */
re_t re_compile(const char* pattern);

/* Reconstruct a regex string from a compiled pattern */
void re_string(re_t pattern, char* buffer, unsigned* size);

/* Returns the size in bytes of a compiled pattern */
unsigned re_size(re_t pattern);

/* Compares two compiled patterns for equality */
int re_compare(re_t pattern1, re_t pattern2);

/* Find matches of the compiled pattern inside text. */
int re_matchp(re_t pattern, const char* text, int* matchlength);

/* Find matches of the txt pattern inside text (will compile automatically
 * first). */
int re_match(const char* pattern, const char* text, int* matchlength);

#ifdef __cplusplus
}
#endif

#endif /* TINY_REGEX_C_RE_H */
