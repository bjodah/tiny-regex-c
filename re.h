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
 *   '.'        Dot, matches any character except newline
 *   '^'        Start anchor, matches beginning of string
 *   '$'        End anchor, matches end of string
 *   '*'        Asterisk, match zero or more (greedy)
 *   '+'        Plus, match one or more (greedy)
 *   '?'        Question, match zero or one (greedy)
 *   '[abc]'    Character class, match if one of {'a', 'b', 'c'}
 *   '[^abc]'   Inverted class, match if NOT one of {'a', 'b', 'c'}
 *   '[a-zA-Z]' Character ranges, the character set of the ranges { a-z | A-Z }
 *   '\s'       Whitespace, \t \f \r \n \v and spaces
 *   '\S'       Non-whitespace
 *   '\w'       Alphanumeric, [a-zA-Z0-9_]
 *   '\W'       Non-alphanumeric
 *   '\d'       Digits, [0-9]
 *   '\D'       Non-digits
 *              ('\s', '\S', '\w', '\W', '\d' and '\D' are ASCII-only, so a
 *              multi-byte character is in none of them and in all their
 *              complements -- as one whole character)
 *   '\xXX'     Hex-encoded byte; a non-ASCII one is a byte that stands
 *              alone, so it matches only where the subject has no valid
 *              sequence around it.  Where the two hex digits are not
 *              there, the escape is not one: '\xZ' is the three literal
 *              characters '\', 'x' and 'Z', '\x4X' the four it is spelled
 *              with, and a trailing '\x' the two.  A quantifier after
 *              such a spelling therefore repeats its last character.
 *   '\|'       Branch Or; the alternatives are whole concatenations and
 *              a group bounds them, e.g. ab\|cd, x\(ab\|cd\)y
 *   '\{n\}'    Match n times
 *   '\{n,\}'   Match n or more times
 *   '\{,m\}'   Match m or less times
 *   '\{n,m\}'  Match n to m times; counts run 0 to 65535 and m may not be
 *              below n.  Anything else between the braces, and an
 *              unterminated '\{', is a bad pattern -- it is never read as
 *              the literal characters it is spelled with.  Where there is
 *              nothing to repeat (pattern, group or alternative start),
 *              '\{' is a literal '{', as in Emacs.
 *   '[[:x:]]'  POSIX class inside a bracket expression: alnum, alpha,
 *              ascii, blank, cntrl, digit, graph, lower, nonascii, print,
 *              punct, space, upper, word, xdigit.  Any other name is a bad
 *              pattern rather than a set of the characters spelling it.
 *              All but '[:ascii:]' and '[:nonascii:]' are ASCII-only;
 *              those two ask whether the character is ASCII at all.
 *   '\(...\)'  Group, including a trailing quantifier applied to the group
 *
 * Characters, not bytes:
 * ----------------------
 * The matcher steps by UTF-8 character.  '.' consumes a whole character,
 * a multi-byte literal in the pattern is one atom ('å*' repeats 'å', not
 * its last byte), quantifiers and intervals count characters, and a
 * bracket expression holds characters -- '[åä]' matches either of them
 * and not the 0xC3 lead byte they share.  A range compares codepoints, so
 * '[à-é]' matches ç but not ê, as in Emacs.  Case folding stays ASCII.
 *
 * Reported spans are byte offsets into the subject, and so is
 * re_exec()'s start_offset.
 *
 * A subject or pattern that is not well-formed UTF-8 is not an error:
 * every stray byte is a character of its own, which '.' consumes singly
 * and which no real character ever equals.
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

/* Alignment every caller-supplied storage buffer must satisfy.
 *
 * A compiled program is not an opaque byte string: the engine reads its
 * multi-byte fields in place through `struct regex_t`, so storage that is
 * not aligned for that type cannot be used without undefined behavior.
 * Rather than read it anyway, the compilers refuse such a buffer --
 * re_compile_checked() returns RE_STATUS_BAD_PATTERN and re_compile_to()
 * returns NULL, neither of them writing to it.
 *
 * Anything malloc() returns satisfies this, as does any object declared
 * `alignas(RE_STORAGE_ALIGNMENT)` or with a stricter alignment.  A
 * `char`/`unsigned char` array with no alignment specifier does not, and
 * neither does an interior pointer into one. */
#define RE_STORAGE_ALIGNMENT 2u

/* Checked compile and execute APIs.  On RE_STATUS_BUFFER_TOO_SMALL,
 * *storage_size is set to the required byte count.  Passing NULL storage with
 * *storage_size == 0 is the supported way to query that size.  `storage` must
 * be aligned to RE_STORAGE_ALIGNMENT; if it is not, the call fails with
 * RE_STATUS_BAD_PATTERN without touching the buffer. */
re_status re_compile_checked(const char* pattern,
                             re_flags flags,
                             unsigned char* storage,
                             unsigned* storage_size,
                             re_t* out);

/* Search `text` for the leftmost match at or after `start_offset`.
 *
 * `start_offset` says where to resume scanning; it does not redefine the
 * start of the subject.  '^' holds at `text` itself and nowhere else, so a
 * '^'-anchored pattern cannot match at a non-zero `start_offset`, and '$'
 * holds at `text`'s terminator.  Callers matching line by line therefore
 * pass the whole line as `text` and use `start_offset` only to advance
 * within it.  Reported spans are offsets from `text`. */
re_status re_exec(re_t regex,
                  const char* text,
                  int start_offset,
                  re_match_result* out);

/* Compile regex string pattern to custom buffer, returning # of bytes used.
 * `re_data` must be aligned to RE_STORAGE_ALIGNMENT; a buffer that is not is
 * rejected (NULL return) rather than written to.  A buffer too small for the
 * whole pattern is also a NULL return -- never a silently compiled prefix of
 * it -- and this entry point wants one node's slack over the size it ends up
 * reporting, so a caller that must fit exactly should compile through
 * re_compile_checked(), which reports the size it needs and needs no more. */
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
