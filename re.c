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
 *   '\|'       Branch Or; the alternatives are whole concatenations and
 *              a group bounds them, e.g. ab\|cd, x\(ab\|cd\)y
 *   '\{n\}'    Match n times
 *   '\{n,\}'   Match n or more times
 *   '\{,m\}'   Match m or less times
 *   '\{n,m\}'  Match n to m times
 *   '\(...\)'  Group, including a trailing quantifier applied to the group
 *
 * TODO:
 *   - multibyte support (mbtowc, esp. UTF-8. maybe hardcode UTF-8 without libc
 * locale insanity)
 *   - \b word boundary support
 */

#include "re.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#ifdef _UNICODE
#include <locale.h>
#include <stdlib.h>
#endif

/* Definitions: */

#define MAX_CHAR_CLASS_LEN 40 /* Max length of character-class buffer in. */
#ifndef CPROVER
#define MAX_REGEXP_OBJECTS 30 /* Max number of regex symbols in expression. */
#else
#define MAX_REGEXP_OBJECTS 8 /* faster formal proofs */
#endif

#define MAX_REGEXP_LEN 70

/* Bound on the number of times a quantified group ("\(...\)+" etc.) is
 * expanded. Each repetition is one more frame on the C stack, so this caps
 * how far a single group can drive the matcher down. */
#ifndef CPROVER
#define MAX_GROUP_REPEATS 256
#else
#define MAX_GROUP_REPEATS 8 /* faster formal proofs */
#endif

/* Bounds total backtracking work per top-level match attempt. Nested
 * quantified groups (e.g. "\(a*\)*") can force catastrophic/exponential
 * backtracking; rather than hang, match_seq() fails the match once this
 * many steps have run. Not a formal ReDoS fix -- just a fail-fast ceiling,
 * the same spirit as fe's Lisp step budget for otherwise-unbounded input. */
#ifndef CPROVER
#define MAX_MATCH_STEPS 2000000
#else
#define MAX_MATCH_STEPS 1000 /* faster formal proofs */
#endif

/* Bounds the matcher's recursion depth. The matcher keeps its pending work
 * on the C stack (see the continuation frames below), so a pattern that
 * both repeats a lot and nests can grow the stack without ever running out
 * of steps; overrunning this is reported as RE_STATUS_TOO_COMPLEX rather
 * than smashing the stack. One unit is one match_seq() frame, a few hundred
 * bytes of stack, so the ceiling stays comfortably under a megabyte. */
#ifndef CPROVER
#define MAX_MATCH_DEPTH 4096
#else
#define MAX_MATCH_DEPTH 64 /* faster formal proofs */
#endif

#ifdef DEBUG
#define DEBUG_P(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_P(...)
#endif

enum regex_type_e {
  UNUSED,
  DOT,
  BEGIN,
  END,
  QUESTIONMARK,
  STAR,
  PLUS,
  CHAR,
  CHAR_CLASS,
  INV_CHAR_CLASS,
  DIGIT,
  NOT_DIGIT,
  ALPHA,
  NOT_ALPHA,
  WHITESPACE,
  NOT_WHITESPACE,
  BRANCH,
  GROUP,
  GROUPEND,
  TIMES,
  TIMES_N,
  TIMES_M,
  TIMES_NM
};

typedef struct regex_t {
  unsigned short type; /* CHAR, STAR, etc.                      */
  union {
    char ch; /*      the character itself             */
    struct {
      unsigned char group_size; /*  OR the number of group patterns. */
      unsigned char group_num;
    };
    struct {
      unsigned char
          group_start; /*  OR for GROUPEND, the start index of the group. */
      unsigned char group_num_end;
    };
    struct {
      unsigned short n; /* match n times */
      unsigned short m; /* match n to m times */
    };
  } u;
} regex_t;

/*
 * Character-class (and inline) data is stored as a trailing string that
 * overlays the union, beginning at the `ch` byte (offset 0 of `u`).  The
 * storage extends past this object into the surrounding compiled-pattern
 * byte buffer.  RE_CCL_STR(p) points at the first class char (= old
 * &p->u.data[-1]); RE_CCL_DAT(p) matches the old `data` base (= old
 * &p->u.data[0]).  Both are byte-identical to the pre-refactor layout.
 */
#define RE_CCL_STR(p) ((char*)&(p)->u)
#define RE_CCL_DAT(p) (RE_CCL_STR(p) + 1)

#define RE_TYPE_ICASE (1 << 15)

static unsigned getsize(const regex_t* pattern) {
  static const unsigned char payload_size[] = {
      [CHAR] = sizeof(unsigned short) * 2,
      [CHAR_CLASS] = sizeof(unsigned short),
      [INV_CHAR_CLASS] = sizeof(unsigned short),
      [GROUP] = sizeof(unsigned short) * 2,
      [GROUPEND] = sizeof(unsigned short) * 2,
      [TIMES] = sizeof(unsigned short) * 2,
      [TIMES_N] = sizeof(unsigned short) * 2,
      [TIMES_M] = sizeof(unsigned short) * 2,
      [TIMES_NM] = sizeof(unsigned short) * 2,
  };
  unsigned type = pattern->type & ~RE_TYPE_ICASE;
  unsigned size = sizeof(unsigned short);
  if (type <= TIMES_NM)
    size += payload_size[type];
  if (type == CHAR_CLASS || type == INV_CHAR_CLASS)
    size += strlen(RE_CCL_STR(pattern));

  if (size % 2)
    ++size;

  return size;
}

static re_t getnext(regex_t* pattern) {
  return (re_t)(((unsigned char*)pattern) + getsize(pattern));
}

static re_t getindex(regex_t* pattern, int index) {
  /* UNUSED terminates the compiled buffer; it is always safely in-bounds,
   * but stepping *from* it via getnext() is not. An index that overshoots
   * the pattern -- re_compile_to()'s "\)" scan walks back over indices it
   * has not finished writing -- would otherwise walk this past the end of
   * the buffer; clamp to the sentinel instead. */
  for (int i = 1; i <= index && (pattern->type & ~RE_TYPE_ICASE) != UNUSED; ++i)
    pattern = getnext(pattern);

  return pattern;
}

/* Backtracking budget for the current top-level match attempt; both are
 * reset in re_matchp_internal() and consumed by match_seq(). See
 * MAX_MATCH_STEPS and MAX_MATCH_DEPTH. */
static long re_match_steps;
static long re_match_depth;

static void save_spans(re_span spans[RE_MAX_SPANS],
                       const re_match_result* out) {
  if (out)
    memcpy(spans, out->spans, sizeof(out->spans));
}

static void restore_spans(re_match_result* out,
                          const re_span spans[RE_MAX_SPANS]) {
  if (out)
    memcpy(out->spans, spans, sizeof(out->spans));
}

static void reset_spans(re_match_result* out) {
  if (!out)
    return;
  for (int i = 0; i < RE_MAX_SPANS; i++) {
    out->spans[i].start = -1;
    out->spans[i].end = -1;
  }
}

/* Private function declarations: */
static int matchcharclass(char c, const char* str, int icase);
static int matchone(regex_t* p, char c);
static int matchdigit(char c);
static int matchalpha(char c);
static int matchwhitespace(char c);
static int matchmetachar(char c, const char* str);
static int matchrange(char c, const char* str, int icase);
static int matchdot(char c);
static int ismetachar(char c);
static int hex(char c);
static int re_matchp_internal(re_t pattern,
                              const char* text,
                              int* matchlength,
                              const char* text_start,
                              re_match_result* out);

/* Public functions: */
int re_match(const char* pattern, const char* text, int* matchlength) {
  re_t regex = re_compile(pattern);
  if (!regex) {
    return -1;
  }
  return re_matchp(regex, text, matchlength);
}

int re_matchp(re_t pattern, const char* text, int* matchlength) {
  re_match_result out;
  re_status status = re_exec(pattern, text, 0, &out);
  if (status == RE_STATUS_OK) {
    if (matchlength) {
      *matchlength = out.spans[0].end - out.spans[0].start;
    }
    return out.spans[0].start;
  }
  return -1;
}

re_status re_compile_checked(const char* pattern,
                             re_flags flags,
                             unsigned char* storage,
                             unsigned* storage_size,
                             re_t* out) {
  if (out) {
    *out = NULL;
  }
  if (!pattern || !storage_size) {
    return RE_STATUS_BAD_PATTERN;
  }

  unsigned char temp_buffer[RE_MAX_COMPILED_BYTES];
  unsigned temp_size = sizeof(temp_buffer);
  re_t compiled = re_compile_to(pattern, temp_buffer, &temp_size);
  if (!compiled) {
    return RE_STATUS_BAD_PATTERN;
  }

  if (flags & RE_FLAG_ICASE) {
    re_t p = (re_t)temp_buffer;
    while (p) {
      p->type |= RE_TYPE_ICASE;
      if ((p->type & ~RE_TYPE_ICASE) == UNUSED)
        break;
      p = getnext(p);
    }
  }

  unsigned required_size = temp_size;

  if (!storage || *storage_size < required_size) {
    *storage_size = required_size;
    return RE_STATUS_BUFFER_TOO_SMALL;
  }

  memcpy(storage, temp_buffer, required_size);
  *storage_size = required_size;
  if (out) {
    *out = (re_t)storage;
  }
  return RE_STATUS_OK;
}

re_status re_exec(re_t regex,
                  const char* text,
                  int start_offset,
                  re_match_result* out) {
  if (!regex) {
    return RE_STATUS_BAD_PATTERN;
  }
  if (!text) {
    return RE_STATUS_NO_MATCH;
  }
  int len = (int)strlen(text);
  if (start_offset < 0 || start_offset > len) {
    return RE_STATUS_NO_MATCH;
  }

  int num_groups = 0;
  re_t p_node = regex;
  while (p_node) {
    if ((p_node->type & ~RE_TYPE_ICASE) == GROUP) {
      if (p_node->u.group_num > num_groups) {
        num_groups = p_node->u.group_num;
      }
    }
    if ((p_node->type & ~RE_TYPE_ICASE) == UNUSED)
      break;
    p_node = getnext(p_node);
  }

  if (out) {
    out->nspans = num_groups + 1;
  }
  reset_spans(out);

  int matchlength = 0;
  int res =
      re_matchp_internal(regex, text + start_offset, &matchlength, text, out);
  if (res >= 0) {
    if (out) {
      out->spans[0].start = start_offset + res;
      out->spans[0].end = start_offset + res + matchlength;
    }
    return RE_STATUS_OK;
  }

  if (re_match_steps > MAX_MATCH_STEPS) {
    return RE_STATUS_TOO_COMPLEX;
  }

  return RE_STATUS_NO_MATCH;
}

static int compile_charclass(const char* pattern,
                             int* pattern_index,
                             regex_t* compiled,
                             const char* storage_end) {
  int i = *pattern_index;
  int char_index = -1;

  if (pattern[i + 1] == '^') {
    compiled->type = INV_CHAR_CLASS;
    i++;
    if (pattern[i + 1] == '\0')
      return 0;
  } else {
    compiled->type = CHAR_CLASS;
  }

  while (pattern[++i] != ']' && pattern[i] != '\0') {
    if (pattern[i] == '[' && pattern[i + 1] == ':') {
      const char* end = strstr(&pattern[i + 2], ":]");
      if (end) {
        int length = (end + 2) - &pattern[i];
        if (RE_CCL_DAT(compiled) + char_index + length >= storage_end)
          return 0;
        memcpy(RE_CCL_DAT(compiled) + char_index, &pattern[i], length);
        char_index += length;
        i += length - 1;
        continue;
      }
    }
    if (pattern[i] == '\\') {
      if (RE_CCL_DAT(compiled) + char_index >= storage_end)
        return 0;
      if (pattern[i + 1] == '\0')
        return 0;
      RE_CCL_DAT(compiled)[char_index++] = pattern[i++];
    }

    if (RE_CCL_DAT(compiled) + char_index >= storage_end)
      return 0;
    RE_CCL_DAT(compiled)[char_index++] = pattern[i];
  }

  if (RE_CCL_DAT(compiled) + char_index >= storage_end)
    return 0;
  RE_CCL_DAT(compiled)[char_index] = '\0';
  *pattern_index = i;
  return 1;
}

re_t re_compile_to(const char* pattern,
                   unsigned char* re_data,
                   unsigned* size) {
  memset(re_data, 0, *size);

  int i = 0; /* index into pattern        */
  int j = 0; /* index into re_data    */
  int num_groups = 0;
  unsigned bytes = *size;
  *size = 0;

  regex_t* re_compiled = (regex_t*)(re_data);

  /* "< re_data + bytes" (rather than "re_data + bytes - sizeof(regex_t)")
   * avoids computing bytes - sizeof(regex_t): with a caller-supplied buffer
   * smaller than sizeof(regex_t), that subtraction underflows the unsigned
   * "bytes" and the resulting pointer addition is undefined behavior. */
#define RE_HAS_ROOM(p) ((char*)(p) + sizeof(regex_t) < (char*)re_data + bytes)

  /* Bound the scan by the pattern length rather than re-reading past the
   * terminator: some escape handlers (e.g. '\x') land `i` on the NUL and
   * the trailing `i += 1` then steps one byte past the allocation. */
  const int plen = pattern ? (int)strlen(pattern) : 0;
  while (i < plen && RE_HAS_ROOM(re_compiled)) {
    char c = pattern[i];

    switch (c) {
      /* Meta-characters: */
      case '^': {
        re_compiled->type = BEGIN;
      } break;
      case '$': {
        re_compiled->type = END;
      } break;
      case '.': {
        re_compiled->type = DOT;
      } break;
      case '*': {
        if (j > 0)
          re_compiled->type = STAR;
        else  // nothing to repeat at position 0
          return 0;
      } break;
      case '+': {
        if (j > 0)
          re_compiled->type = PLUS;
        else  // nothing to repeat at position 0
          return 0;
      } break;
      case '?': {
        if (j > 0)
          re_compiled->type = QUESTIONMARK;
        else  // nothing to repeat at position 0
          return 0;
      } break;

      /* Escaped character-classes (\s \S \w \W \d \D \*): */
      case '\\': {
        if (pattern[i + 1] != '\0') {
          /* Skip the escape-char '\\' */
          i += 1;
          /* ... and check the next */
          switch (pattern[i]) {
            /* Meta-characters: */
            case 'd': {
              re_compiled->type = DIGIT;
            } break;
            case 'D': {
              re_compiled->type = NOT_DIGIT;
            } break;
            case 'w': {
              re_compiled->type = ALPHA;
            } break;
            case 'W': {
              re_compiled->type = NOT_ALPHA;
            } break;
            case 's': {
              re_compiled->type = WHITESPACE;
            } break;
            case 'S': {
              re_compiled->type = NOT_WHITESPACE;
            } break;
            case '(': {
              const char* p = &pattern[i + 1];
              int found = 0;
              while (*p != '\0') {
                if (*p == '\\') {
                  if (*(p + 1) == '\0') {
                    break;
                  }
                  if (*(p + 1) == ')') {
                    found = 1;
                    break;
                  }
                  p += 2;
                } else {
                  p++;
                }
              }
              if (found) {
                num_groups++;
                if (num_groups >= RE_MAX_SPANS) {
                  return 0;
                }
                re_compiled->type = GROUP;
                re_compiled->u.group_size = 0;
                re_compiled->u.group_num = num_groups;
              } else {
                return 0;
              }
            } break;
            case ')': {
              int nestlevel = 0;
              int k = j - 1;
              for (; k >= 0; k--) {
                regex_t* cur = getindex((regex_t*)re_data, k);
                if (k < j && (cur->type & ~RE_TYPE_ICASE) == GROUPEND)
                  nestlevel++;
                else if ((cur->type & ~RE_TYPE_ICASE) == GROUP) {
                  if (nestlevel == 0) {
                    cur->u.group_size = j - k - 1;
                    re_compiled->type = GROUPEND;
                    re_compiled->u.group_start = k;
                    re_compiled->u.group_num_end = cur->u.group_num;
                    break;
                  }
                  nestlevel--;
                }
              }
              if (k < 0)
                return 0;
            } break;
            case '|': {
              re_compiled->type = BRANCH;
            } break;
            case '{': {
              unsigned short n, m;
              const char* p = &pattern[i + 1];
              while (*p != '\0') {
                if (*p == '\\' && *(p + 1) == '}') {
                  break;
                }
                p++;
              }
              re_compiled->type = CHAR;
              re_compiled->u.ch = '{';
              if (*p != '\0' && j > 0) {
                char buf[64];
                int len = p - &pattern[i];
                if (len > 0 && len < (int)sizeof(buf) - 1) {
                  memcpy(buf, &pattern[i], len);
                  buf[len] = '}';
                  buf[len + 1] = '\0';
                  if (2 == sscanf(buf, "{%hu,%hu}", &n, &m)) {
                    if (!(n == 0 || m == 0 || n > 32767 || m > 32767 ||
                          m <= n || buf[len - 1] == ',')) {
                      re_compiled->type = TIMES_NM;
                      re_compiled->u.n = n;
                      re_compiled->u.m = m;
                    }
                  } else {
                    int o = -1;
                    if (1 == sscanf(buf, "{%hu,}%n", &n, &o) && o >= 0 &&
                        n > 0 && n <= 32767) {
                      re_compiled->type = TIMES_N;
                      re_compiled->u.n = n;
                    } else if (1 == sscanf(buf, "{,%hu}", &m) &&
                               buf[len - 1] != ',' && m > 0 && m <= 32767) {
                      re_compiled->type = TIMES_M;
                      re_compiled->u.m = m;
                    } else if (1 == sscanf(buf, "{%hu}", &n) && n > 0 &&
                               n <= 32767) {
                      re_compiled->type = TIMES;
                      re_compiled->u.n = n;
                    }
                  }
                }
              }
              if (re_compiled->type != CHAR) {
                i = (p - pattern) + 1;
              } else {
                if (RE_HAS_ROOM(getnext(re_compiled))) {
                  re_compiled->type = CHAR;
                  re_compiled->u.ch = '\\';
                  re_compiled = getnext(re_compiled);
                  re_compiled->type = CHAR;
                  re_compiled->u.ch = '{';
                  j += 1;
                } else {
                  return 0;
                }
              }
            } break;
            case 'x': {
              /* \xXX. An invalid escape here falls back to emitting the
               * literal characters seen so far as separate CHAR nodes,
               * bypassing the main loop's own per-iteration bounds check
               * -- each extra node needs its own RE_HAS_ROOM() check. */
              re_compiled->type = CHAR;
              i++;
              int h = hex(pattern[i]);
              if (h == -1) {
                re_compiled->u.ch = '\\';
                re_compiled->type = CHAR;

                re_compiled = getnext(re_compiled);
                if (!RE_HAS_ROOM(re_compiled))
                  return 0;
                re_compiled->u.ch = 'x';
                re_compiled->type = CHAR;

                re_compiled = getnext(re_compiled);
                if (!RE_HAS_ROOM(re_compiled))
                  return 0;
                re_compiled->u.ch = pattern[i];
                re_compiled->type = CHAR;
                break;
              }
              re_compiled->u.ch = h << 4;
              h = hex(pattern[++i]);
              if (h != -1)
                re_compiled->u.ch += h;
              else {
                re_compiled->u.ch = '\\';
                re_compiled->type = CHAR;

                re_compiled = getnext(re_compiled);
                if (!RE_HAS_ROOM(re_compiled))
                  return 0;
                re_compiled->u.ch = 'x';
                re_compiled->type = CHAR;

                re_compiled = getnext(re_compiled);
                if (!RE_HAS_ROOM(re_compiled))
                  return 0;
                re_compiled->u.ch = pattern[i - 1];
                re_compiled->type = CHAR;

                if (pattern[i]) {
                  re_compiled = getnext(re_compiled);
                  if (!RE_HAS_ROOM(re_compiled))
                    return 0;
                  re_compiled->u.ch = pattern[i];
                  re_compiled->type = CHAR;
                }
              }
            } break;

            /* Escaped character, e.g. '.', '$' or '\\' */
            default: {
              re_compiled->type = CHAR;
              re_compiled->u.ch = pattern[i];
            } break;
          }
        }
        /* '\\' as last char without previous \\ -> invalid regular expression.
         */
        else
          return 0;
      } break;

      /* Character class: */
      case '[': {
        if (!compile_charclass(pattern, &i, re_compiled,
                               (char*)re_data + bytes))
          return 0;
      } break;

      case '\0':  // EOL (dead-code)
        return 0;

      /* Other characters: */
      default: {
        re_compiled->type = CHAR;
        // cbmc: arithmetic overflow on signed to unsigned type conversion in c
        re_compiled->u.ch = c;
      } break;
    }
    i += 1;
    j += 1;
    re_compiled = getnext(re_compiled);
  }
  /* 'UNUSED' is a sentinel used to indicate end-of-pattern. The main loop's
   * bounds check only guarantees room for a full regex_t before *starting*
   * an iteration; if it instead exits because the buffer ran out (rather
   * than because the pattern did), "re_compiled" can already sit within
   * sizeof(unsigned short) of the buffer end, and writing the sentinel's
   * type field here would overflow a small caller-supplied buffer. */
  if ((char*)re_compiled + sizeof(unsigned short) > (char*)re_data + bytes)
    return 0;
  re_compiled->type = UNUSED;

  /* Calculate final, compressed actual size. */
  *size = (unsigned char*)getnext(re_compiled) - re_data;

#undef RE_HAS_ROOM
  return (re_t)re_data;
}

re_t re_compile(const char* pattern) {
  static unsigned char buffer[MAX_REGEXP_OBJECTS * sizeof(regex_t)];
  unsigned size = sizeof(buffer);
  re_t out = NULL;
  re_status status =
      re_compile_checked(pattern, RE_FLAG_NONE, buffer, &size, &out);
  if (status == RE_STATUS_OK) {
    return out;
  }
  return NULL;
}

unsigned re_size(re_t pattern) {
  unsigned bytes = 0;

  while (pattern) {
    bytes += getsize(pattern);

    if ((pattern->type & ~RE_TYPE_ICASE) == UNUSED)
      break;

    pattern = getnext(pattern);
  }

  return bytes;
}

int re_compare(re_t pattern1, re_t pattern2) {
  int result = 0;

  const unsigned totalSize1 = re_size(pattern1);
  const unsigned totalSize2 = re_size(pattern2);

  result = (totalSize1 > totalSize2) - (totalSize1 < totalSize2);
  if (result)
    return result;

  while (pattern1 && pattern2) {
    unsigned size1 = getsize(pattern1);
    unsigned size2 = getsize(pattern2);

    result = (size1 > size2) - (size1 < size2);
    if (result)
      return result;

    result = memcmp(pattern1, pattern2, size1);

    if (result != 0)
      return result;

    if ((pattern1->type & ~RE_TYPE_ICASE) == UNUSED)
      break;

    pattern1 = getnext(pattern1);
    pattern2 = getnext(pattern2);
  }

  return result;
}

#define re_string_cat_fmt_(buff, ...)           \
  do {                                          \
    sprintf(tmp_buff, __VA_ARGS__);             \
    strncat(buff, tmp_buff, count - *size - 1); \
    *size = strlen(buff);                       \
    if (*size >= count)                         \
      return;                                   \
  } while (0)

void re_string(regex_t* pattern, char* buffer, unsigned* size) {
#if 0
  const char *const types[] = { "UNUSED", "DOT", "BEGIN", "END", "QUESTIONMARK", "STAR", "PLUS", "CHAR", "CHAR_CLASS", "INV_CHAR_CLASS", "DIGIT", "NOT_DIGIT", "ALPHA", "NOT_ALPHA", "WHITESPACE", "NOT_WHITESPACE", "BRANCH", "GROUP", "GROUPEND", "TIMES", "TIMES_N", "TIMES_M", "TIMES_NM" };
#endif
  unsigned count = *size;
  unsigned char i = 0;
  int j;
  unsigned char group_end;
  char c;
  char tmp_buff[128];

  *size = 0;
  buffer[0] = '\0';

  if (!pattern)
    return;
  while (*size < count) {
    if ((pattern->type & ~RE_TYPE_ICASE) == UNUSED) {
      break;
    }

    // if (group_end && i == group_end)
    //   printf("      )\n");
#if 0
    if ((pattern->type & ~RE_TYPE_ICASE) <= TIMES_NM)
      re_string_cat_fmt_(buffer, "type: %s", types[pattern->type & ~RE_TYPE_ICASE]);
    else
      re_string_cat_fmt_(buffer, "invalid type: %d", pattern->type & ~RE_TYPE_ICASE);
#endif
    switch (pattern->type & ~RE_TYPE_ICASE) {
      case CHAR_CLASS:
      case INV_CHAR_CLASS:
        re_string_cat_fmt_(buffer, "[");
        if ((pattern->type & ~RE_TYPE_ICASE) == INV_CHAR_CLASS)
          re_string_cat_fmt_(buffer, "^");
        j = -1;
        while ((c = RE_CCL_DAT(pattern)[j])) {
          if (c == ']')
            break;
          re_string_cat_fmt_(buffer, "%c", c);
          ++j;
        }
        re_string_cat_fmt_(buffer, "]");
        break;
      case CHAR:
        re_string_cat_fmt_(buffer, "%c", pattern->u.ch);
        break;
      case TIMES:
        re_string_cat_fmt_(buffer, "{%hu}", pattern->u.n);
        break;
      case TIMES_N:
        re_string_cat_fmt_(buffer, "{%hu,}", pattern->u.n);
        break;
      case TIMES_M:
        re_string_cat_fmt_(buffer, "{,%hu}", pattern->u.m);
        break;
      case TIMES_NM:
        re_string_cat_fmt_(buffer, "{%hu,%hu}", pattern->u.n, pattern->u.m);
        break;
      case GROUP:
        group_end = i + pattern->u.group_size;
        if (group_end >= MAX_REGEXP_OBJECTS)
          return;
        re_string_cat_fmt_(buffer, " (");
        break;
      case GROUPEND:
        re_string_cat_fmt_(buffer, " )");
        break;
      case BEGIN:
        re_string_cat_fmt_(buffer, "^");
        break;
      case END:
        re_string_cat_fmt_(buffer, "$");
        break;
      case QUESTIONMARK:
        re_string_cat_fmt_(buffer, "?");
        break;
      case DIGIT:
        re_string_cat_fmt_(buffer, "\\d");
        break;
      default:
        break;
    }
    // re_string_cat_fmt_(buffer, "\n");
    ++i;
    pattern = getnext(pattern);
  }
}

static int hex(char c) {
  static const char digits[] = "0123456789abcdef";
  const char* digit =
      memchr(digits, tolower((unsigned char)c), sizeof(digits) - 1);
  return digit ? (int)(digit - digits) : -1;
}

/* Private functions: */
static int matchdigit(char c) {
  return isdigit((unsigned char)c);
}
static int matchalpha(char c) {
  return isalpha((unsigned char)c);
}
static int matchwhitespace(char c) {
  return isspace((unsigned char)c);
}
static int matchalphanum(char c) {
  return ((c == '_') || matchalpha(c) || matchdigit(c));
}
static int matchposixalnum(char c) {
  return isalnum((unsigned char)c);
}
static int matchcontrol(char c) {
  return iscntrl((unsigned char)c);
}
static int matchgraph(char c) {
  return isgraph((unsigned char)c);
}
static int matchprint(char c) {
  return isprint((unsigned char)c);
}
static int matchpunct(char c) {
  return ispunct((unsigned char)c);
}
static int matchxdigit(char c) {
  return isxdigit((unsigned char)c);
}
static int matchlower(char c) {
  return islower((unsigned char)c);
}
static int matchupper(char c) {
  return isupper((unsigned char)c);
}

typedef int (*char_matcher)(char);

static const char_matcher metachar_matchers[256] = {
    ['d'] = matchdigit,    ['D'] = matchdigit,      ['w'] = matchalphanum,
    ['W'] = matchalphanum, ['s'] = matchwhitespace, ['S'] = matchwhitespace,
};

struct named_class {
  const char* name;
  unsigned char length;
  char_matcher match[2];
};

static int matchnamedclass(char c, const char** str, int icase) {
  static const struct named_class classes[] = {
      {"[:digit:]", 9, {matchdigit, matchdigit}},
      {"[:alpha:]", 9, {matchalpha, matchalpha}},
      {"[:alnum:]", 9, {matchposixalnum, matchposixalnum}},
      {"[:space:]", 9, {matchwhitespace, matchwhitespace}},
      {"[:cntrl:]", 9, {matchcontrol, matchcontrol}},
      {"[:graph:]", 9, {matchgraph, matchgraph}},
      {"[:print:]", 9, {matchprint, matchprint}},
      {"[:punct:]", 9, {matchpunct, matchpunct}},
      {"[:xdigit:]", 10, {matchxdigit, matchxdigit}},
      {"[:lower:]", 9, {matchlower, matchalpha}},
      {"[:upper:]", 9, {matchupper, matchalpha}},
  };

  for (unsigned i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
    if (strncmp(*str, classes[i].name, classes[i].length) != 0)
      continue;
    *str += classes[i].length - 1;
    return classes[i].match[!!icase](c) * 2 - 1;
  }
  return 0;
}

static int matchrange(char c, const char* str, int icase) {
  char normalized[3] = {str[0], str[1], str[2]};
  if (icase) {
    c = (char)tolower((unsigned char)c);
    normalized[0] = (char)tolower((unsigned char)str[0]);
    normalized[2] = (char)tolower((unsigned char)str[2]);
  }
  str = normalized;
  return ((c != '-') && (str[0] != '\0') && (str[0] != '-') &&
          (str[1] == '-') && (str[2] != '\0') &&
          ((c >= str[0]) && (c <= str[2])));
}
static int matchdot(char c) {
#if defined(RE_DOT_MATCHES_NEWLINE) && (RE_DOT_MATCHES_NEWLINE == 1)
  (void)c;
  return 1;
#else
  return memchr("\n\r", c, 2) == NULL;
#endif
}
static int ismetachar(char c) {
  return metachar_matchers[(unsigned char)c] != NULL;
}

static int matchmetachar(char c, const char* str) {
  char_matcher matcher = metachar_matchers[(unsigned char)str[0]];
  if (!matcher)
    return c == str[0];
  return !!matcher(c) == !!islower((unsigned char)str[0]);
}

static int matchcharclass(char c, const char* str, int icase) {
  do {
    if (matchrange(c, str, icase)) {
      DEBUG_P("%c matches %s\n", c, str);
      return 1;
    } else if (str[0] == '\\') {
      /* Escape-char: increment str-ptr and match on next char */
      str += 1;
      if (matchmetachar(c, str)) {
        return 1;
      }
      if (icase) {
        if (tolower((unsigned char)c) == tolower((unsigned char)str[0]) &&
            !ismetachar(str[0])) {
          return 1;
        }
      } else {
        if ((c == str[0]) && !ismetachar(str[0])) {
          return 1;
        }
      }
    } else {
      int named = matchnamedclass(c, &str, icase);
      if (named > 0)
        return 1;
      if (named < 0)
        continue;

      if (!(icase
                ? (tolower((unsigned char)c) == tolower((unsigned char)str[0]))
                : (c == str[0])))
        continue;
      if (str[0] == '-') {
        if ((str[-1] == '\0') || (str[1] == '\0'))
          return 1;
        // else continue
      } else {
        return 1;
      }
    }
  } while (*str++ != '\0');

  DEBUG_P("%c did not match prev. ccl\n", c);
  return 0;
}

static int matchone(regex_t* p, char c) {
  DEBUG_P("ONE %d matches %c?\n", p->type, c);
  int icase = (p->type & RE_TYPE_ICASE) != 0;
  switch (p->type & ~RE_TYPE_ICASE) {
    case DOT:
      return matchdot(c);
    case CHAR_CLASS:
      return matchcharclass(c, (const char*)RE_CCL_STR(p), icase);
    case INV_CHAR_CLASS:
      return !matchcharclass(c, (const char*)RE_CCL_STR(p), icase);
    case DIGIT:
      return matchdigit(c);
    case NOT_DIGIT:
      return !matchdigit(c);
    case ALPHA:
      return matchalphanum(c);
    case NOT_ALPHA:
      return !matchalphanum(c);
    case WHITESPACE:
      return matchwhitespace(c);
    case NOT_WHITESPACE:
      return !matchwhitespace(c);
    default:
      if (icase) {
        return tolower((unsigned char)p->u.ch) == tolower((unsigned char)c);
      }
      return (p->u.ch == c);
  }
}

/* ------------------------------------------------------------------------
 * Backtracking matcher
 *
 * The compiled pattern is a flat node list; its structure -- groups,
 * alternation, which quantifier belongs to which atom -- is recovered by
 * walking it. Matching is stated as "match the node run [p, stop) at
 * 'text', then run the continuation k", and a successful match returns the
 * position in the subject it reached. Two properties follow from that
 * shape:
 *
 *   - No match length is accumulated by hand on the way out, so a reported
 *     span cannot end past the subject.
 *   - Every construct backtracks, because a choice only succeeds if the
 *     continuation it installed succeeds too.
 *
 * Continuation frames live on the C stack and are chained through 'next',
 * so matching still allocates nothing. Total work is bounded by
 * MAX_MATCH_STEPS and stack depth by MAX_MATCH_DEPTH.
 * ---------------------------------------------------------------------- */

enum cont_kind { CONT_SEQ, CONT_REP };

/* CONT_SEQ resumes a node run; CONT_REP closes one repetition of the group
 * headed by 'p' (whose body ends at 'stop') and decides whether to repeat
 * it again. */
typedef struct re_cont {
  const struct re_cont* next;
  unsigned char kind;
  regex_t* p;
  regex_t* stop;
  const char* iter;        /* CONT_REP: where this repetition began */
  unsigned short min, max; /* CONT_REP: bounds; max == 0 is unbounded */
  unsigned short done;     /* CONT_REP: repetitions completed */
} re_cont;

typedef struct {
  const char* text_start; /* offset 0 for reported spans */
  const char* anchor;     /* the only place '^' can match */
  regex_t* prog_end;      /* the UNUSED sentinel */
  re_match_result* out;
  int has_branch; /* whether the pattern contains '\|' at all */
} re_ctx;

static const char* match_seq(regex_t* p,
                             regex_t* stop,
                             const char* text,
                             const re_cont* k,
                             re_ctx* ctx);
static const char* match_cont(const re_cont* k, const char* text, re_ctx* ctx);
static const char* match_rep(const re_cont* k, const char* text, re_ctx* ctx);
static const char* match_group_iter(regex_t* g,
                                    regex_t* gend,
                                    const char* text,
                                    unsigned short done,
                                    unsigned short min,
                                    unsigned short max,
                                    const re_cont* k,
                                    re_ctx* ctx);

static unsigned short node_type(const regex_t* p) {
  return p->type & ~RE_TYPE_ICASE;
}

static int isquantifier(unsigned short type) {
  return type == QUESTIONMARK || type == STAR || type == PLUS ||
         (unsigned)(type - TIMES) <= TIMES_NM - TIMES;
}

/* The GROUPEND closing the group headed by 'g'. Found by walking the
 * nesting rather than by trusting GROUP's own 8-bit group_size, which a
 * group of more than 255 nodes overflows. */
static regex_t* group_end(regex_t* g, const re_ctx* ctx) {
  regex_t* p = getnext(g);
  int depth = 1;

  while (p < ctx->prog_end) {
    unsigned short type = node_type(p);
    if (type == GROUP)
      depth++;
    else if (type == GROUPEND && --depth == 0)
      break;
    p = getnext(p);
  }
  return p;
}

/* One past the atom starting at 'p', where a whole group is one atom. */
static regex_t* atom_end(regex_t* p, regex_t* stop, const re_ctx* ctx) {
  regex_t* end = node_type(p) == GROUP ? group_end(p, ctx) : p;

  if (end >= ctx->prog_end)
    return stop;
  end = getnext(end);
  return end < stop ? end : stop;
}

/* The first '\|' in [p, stop) that belongs to this level: alternation
 * separates whole concatenations, so a BRANCH inside a nested group is
 * that group's business, not ours. NULL when there is none. */
static regex_t* find_branch(regex_t* p, regex_t* stop, const re_ctx* ctx) {
  while (p < stop) {
    if (node_type(p) == BRANCH)
      return p;
    p = atom_end(p, stop, ctx);
  }
  return NULL;
}

/* Repetition bounds of the quantifier node 'p'; max 0 means unbounded. */
static void quant_bounds(const regex_t* p,
                         unsigned short* min,
                         unsigned short* max) {
  switch (node_type(p)) {
    case QUESTIONMARK:
      *min = 0, *max = 1;
      break;
    case STAR:
      *min = 0, *max = 0;
      break;
    case PLUS:
      *min = 1, *max = 0;
      break;
    case TIMES:
      *min = p->u.n, *max = p->u.n;
      break;
    case TIMES_N:
      *min = p->u.n, *max = 0;
      break;
    case TIMES_M:
      *min = 0, *max = p->u.m;
      break;
    default: /* TIMES_NM */
      *min = p->u.n, *max = p->u.m;
      break;
  }
}

/* The capture slot of group 'g', or NULL when it has none to record. */
static re_span* group_span(const re_ctx* ctx, const regex_t* g) {
  unsigned num = g->u.group_num;

  if (!ctx->out || num == 0 || num >= RE_MAX_SPANS)
    return NULL;
  return &ctx->out->spans[num];
}

/* "a\|b": try the alternatives left to right, leftmost-first like Emacs.
 * [p, br) is the first alternative and [br+1, stop) is everything after
 * it, which recursion splits again at the next '\|'. */
static const char* match_alt(regex_t* p,
                             regex_t* br,
                             regex_t* stop,
                             const char* text,
                             const re_cont* k,
                             re_ctx* ctx) {
  re_span saved[RE_MAX_SPANS];
  const char* end;

  save_spans(saved, ctx->out);
  end = match_seq(p, br, text, k, ctx);
  if (end)
    return end;
  restore_spans(ctx->out, saved);
  end = match_seq(getnext(br), stop, text, k, ctx);
  if (end)
    return end;
  restore_spans(ctx->out, saved);
  return NULL;
}

/* A quantified single-node atom. '*', '+' and the intervals are greedy:
 * take as much as the atom allows, then hand bytes back one at a time
 * until the rest of the pattern fits. '?' is non-greedy (see re.h) and
 * grows instead of shrinking. */
static const char* match_atom(regex_t* p,
                              const char* text,
                              unsigned short min,
                              unsigned short max,
                              int greedy,
                              const re_cont* k,
                              re_ctx* ctx) {
  re_span saved[RE_MAX_SPANS];
  unsigned n = 0;
  unsigned i;

  while ((max == 0 || n < max) && text[n] && matchone(p, text[n]))
    n++;
  if (n < min)
    return NULL;

  save_spans(saved, ctx->out);
  for (i = min; i <= n; i++) {
    const char* end = match_cont(k, text + (greedy ? min + n - i : i), ctx);
    if (end)
      return end;
    restore_spans(ctx->out, saved);
  }
  return NULL;
}

/* '^' and '$' consume nothing, so a quantifier on one only decides
 * whether the assertion has to hold at all. */
static const char* match_anchor(const regex_t* p,
                                const char* text,
                                unsigned short min,
                                const re_cont* k,
                                re_ctx* ctx) {
  int holds = node_type(p) == BEGIN ? text == ctx->anchor : text[0] == '\0';

  if (!holds && min > 0)
    return NULL;
  return match_cont(k, text, ctx);
}

/* A group, quantified or not (an unquantified one is just min == max == 1).
 * Greedy: entering the group is tried before skipping it. */
static const char* match_group(regex_t* g,
                               const char* text,
                               unsigned short min,
                               unsigned short max,
                               const re_cont* k,
                               re_ctx* ctx) {
  re_span saved[RE_MAX_SPANS];
  const char* end;

  save_spans(saved, ctx->out);
  end = match_group_iter(g, group_end(g, ctx), text, 0, min, max, k, ctx);
  if (end)
    return end;
  restore_spans(ctx->out, saved);
  if (min == 0)
    return match_cont(k, text, ctx);
  return NULL;
}

/* Start repetition number 'done' + 1 of the group headed by 'g'. */
static const char* match_group_iter(regex_t* g,
                                    regex_t* gend,
                                    const char* text,
                                    unsigned short done,
                                    unsigned short min,
                                    unsigned short max,
                                    const re_cont* k,
                                    re_ctx* ctx) {
  re_span* span = group_span(ctx, g);
  re_cont rep;

  if (done >= MAX_GROUP_REPEATS)
    return NULL;
  if (span)
    span->start = (int)(text - ctx->text_start);

  rep.next = k;
  rep.kind = CONT_REP;
  rep.p = g;
  rep.stop = gend;
  rep.iter = text;
  rep.min = min;
  rep.max = max;
  rep.done = done;
  return match_seq(getnext(g), gend, text, &rep, ctx);
}

/* One repetition of a group finished at 'text': close its capture, then
 * prefer repeating the group over leaving it. */
static const char* match_rep(const re_cont* k, const char* text, re_ctx* ctx) {
  re_span* span = group_span(ctx, k->p);
  unsigned short done = (unsigned short)(k->done + 1);
  int grew = text != k->iter;

  if (span)
    span->end = (int)(text - ctx->text_start);

  if (grew && (k->max == 0 || done < k->max)) {
    re_span saved[RE_MAX_SPANS];
    const char* end;
    save_spans(saved, ctx->out);
    end = match_group_iter(k->p, k->stop, text, done, k->min, k->max, k->next,
                           ctx);
    if (end)
      return end;
    restore_spans(ctx->out, saved);
  }
  /* A repetition that consumed nothing gets no further by repeating, so
   * an empty body satisfies whatever is left of 'min'. */
  if (done >= k->min || !grew)
    return match_cont(k->next, text, ctx);
  return NULL;
}

static const char* match_cont(const re_cont* k, const char* text, re_ctx* ctx) {
  if (!k)
    return text;
  if (k->kind == CONT_SEQ)
    return match_seq(k->p, k->stop, text, k->next, ctx);
  return match_rep(k, text, ctx);
}

/* Match the node run [p, stop) at 'text', then the continuation 'k'. */
static const char* match_seq_body(regex_t* p,
                                  regex_t* stop,
                                  const char* text,
                                  const re_cont* k,
                                  re_ctx* ctx) {
  unsigned short type, min = 1, max = 1;
  regex_t* after;
  re_cont rest;
  int greedy = 1;

  if (p >= stop || node_type(p) == UNUSED)
    return match_cont(k, text, ctx);

  if (ctx->has_branch) {
    regex_t* br = find_branch(p, stop, ctx);
    if (br)
      return match_alt(p, br, stop, text, k, ctx);
  }

  after = atom_end(p, stop, ctx);
  if (after < stop && isquantifier(node_type(after))) {
    quant_bounds(after, &min, &max);
    /* '?' is documented non-greedy for a plain atom; on a group it has
     * always been greedy here. D-1 will settle both on greedy. */
    greedy = node_type(after) != QUESTIONMARK || node_type(p) == GROUP;
    after = atom_end(after, stop, ctx);
  }

  rest.next = k;
  rest.kind = CONT_SEQ;
  rest.p = after;
  rest.stop = stop;

  type = node_type(p);
  if (type == GROUP)
    return match_group(p, text, min, max, &rest, ctx);
  if (type == BEGIN || type == END)
    return match_anchor(p, text, min, &rest, ctx);
  if (isquantifier(type))
    return NULL; /* a stray quantifier, e.g. "a*?", matches nothing */
  return match_atom(p, text, min, max, greedy, &rest, ctx);
}

static const char* match_seq(regex_t* p,
                             regex_t* stop,
                             const char* text,
                             const re_cont* k,
                             re_ctx* ctx) {
  const char* end;

  if (++re_match_steps > MAX_MATCH_STEPS)
    return NULL;
  if (++re_match_depth > MAX_MATCH_DEPTH) {
    /* Out of stack budget. Spend the step budget too, so the attempt
     * unwinds at once and re_exec() reports it as TOO_COMPLEX. */
    re_match_steps = MAX_MATCH_STEPS + 1;
    re_match_depth--;
    return NULL;
  }
  end = match_seq_body(p, stop, text, k, ctx);
  re_match_depth--;
  return end;
}

static void init_ctx(re_ctx* ctx,
                     re_t pattern,
                     const char* text,
                     const char* text_start,
                     re_match_result* out) {
  regex_t* p = pattern;

  ctx->text_start = text_start;
  ctx->anchor = text;
  ctx->out = out;
  ctx->has_branch = 0;
  while (node_type(p) != UNUSED) {
    if (node_type(p) == BRANCH)
      ctx->has_branch = 1;
    p = getnext(p);
  }
  ctx->prog_end = p;
}

static int re_matchp_internal(re_t pattern,
                              const char* text,
                              int* matchlength,
                              const char* text_start,
                              re_match_result* out) {
  re_ctx ctx;
  int anchored;
  int idx = 0;

  re_match_steps = 0;
  re_match_depth = 0;
  *matchlength = 0;
  if (!pattern)
    return -1;

  init_ctx(&ctx, pattern, text, text_start, out);
  /* A leading '^' can only hold where the scan started, so trying later
   * offsets is pointless -- unless a '\|' means the anchor governs the
   * first alternative alone. */
  anchored = node_type(pattern) == BEGIN && !ctx.has_branch;

  do {
    const char* end;
    reset_spans(out);
    end = match_seq(pattern, ctx.prog_end, text + idx, NULL, &ctx);
    if (end) {
      *matchlength = (int)(end - (text + idx));
      return idx;
    }
    if (anchored)
      break;
  } while (text[idx++] != '\0');

  return -1;
}

#ifdef CPROVER
#define N 24

/* Formal verification with cbmc: */
/* cbmc -DCPROVER --64 --depth 200 --bounds-check --pointer-check
 * --memory-leak-check --div-by-zero-check --signed-overflow-check
 * --unsigned-overflow-check --pointer-overflow-check --conversion-check
 * --undefined-shift-check --enum-range-check --pointer-primitive-check -trace
 * re.c
 */

void verify_re_compile() {
  /* test input - ten chars used as a regex-pattern input */
  char arr[N];
  /* make input symbolic, to search all paths through the code */
  /* i.e. the input is checked for all possible ten-char combinations */
  for (int i = 0; i < sizeof(arr) - 1; i++) {
    // arr[i] = nondet_char();
    assume(arr[i] > -127 && arr[i] < 128);
  }
  /* assume proper NULL termination */
  assume(arr[sizeof(arr) - 1] == 0);
  /* verify abscence of run-time errors - go! */
  re_compile(arr);
}

void verify_re_match() {
  int length;
  regex_t pattern[MAX_REGEXP_OBJECTS];
  char arr[N];

  for (unsigned char i = 0; i < MAX_REGEXP_OBJECTS; i++) {
    // pattern[i].type = nondet_uchar();
    // pattern[i].u.ch = nondet_int();
    assume(pattern[i].type >= 0 && pattern[i].type <= 255);
    assume(pattern[i].u.ccl >= 0 && pattern[i].u.ccl <= ~1);
  }
  for (int i = 0; i < sizeof(arr) - 1; i++) {
    assume(arr[i] > -127 && arr[i] < 128);
  }
  /* assume proper NULL termination */
  assume(arr[sizeof(arr) - 1] == 0);

  re_match(&pattern, arr, &length);
}

int main(int argc, char* argv[]) {
  verify_re_compile();
  verify_re_match();
  return 0;
}
#endif
