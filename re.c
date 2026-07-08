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

/* Bound on the number of times a quantified group ("(...)+" etc.) is
 * greedily expanded before backtracking; caps the fixed-size bookkeeping
 * arrays in matchgrouptimes() rather than allocating on the heap. */
#ifndef CPROVER
#define MAX_GROUP_REPEATS 256
#else
#define MAX_GROUP_REPEATS 8 /* faster formal proofs */
#endif

/* Bounds total backtracking work per top-level match attempt. Nested
 * quantified groups (e.g. "(a*)*") can force catastrophic/exponential
 * backtracking; rather than hang, matchpattern() fails the match once this
 * many steps have run. Not a formal ReDoS fix -- just a fail-fast ceiling,
 * the same spirit as fe's Lisp step budget for otherwise-unbounded input. */
#ifndef CPROVER
#define MAX_MATCH_STEPS 2000000
#else
#define MAX_MATCH_STEPS 1000 /* faster formal proofs */
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
   * but stepping *from* it via getnext() is not. A group whose recorded
   * size overshoots (reachable with deeply nested quantified groups, where
   * matchgroup()'s own num_patterns bookkeeping can be imprecise -- see
   * matchgroup()) would otherwise walk this past the end of the buffer;
   * clamp to the sentinel instead. */
  for (int i = 1; i <= index && (pattern->type & ~RE_TYPE_ICASE) != UNUSED; ++i)
    pattern = getnext(pattern);

  return pattern;
}

/* Backtracking step counter for the current top-level match attempt; reset
 * in re_matchp() and consumed by matchpattern(). See MAX_MATCH_STEPS. */
static long re_match_steps;

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
static int matchpattern(regex_t* pattern,
                        const char* text,
                        int* matchlength,
                        int* num_patterns,
                        const char* text_start,
                        re_match_result* out);
static int matchcharclass(char c, const char* str, int icase);
static int matchstar(regex_t* p,
                     regex_t* pattern,
                     const char* text,
                     int* matchlength,
                     const char* text_start,
                     re_match_result* out);
static int matchplus(regex_t* p,
                     regex_t* pattern,
                     const char* text,
                     int* matchlength,
                     const char* text_start,
                     re_match_result* out);
static int matchquestion(regex_t* p,
                         regex_t* pattern,
                         const char* text,
                         int* matchlength,
                         const char* text_start,
                         re_match_result* out);
static int matchbranch(regex_t* p,
                       regex_t* pattern,
                       const char* text,
                       int* matchlength,
                       const char* text_start,
                       re_match_result* out);
static int matchtimes(regex_t* p,
                      unsigned short n,
                      const char* text,
                      int* matchlength);
static int matchtimes_n(regex_t* p,
                        unsigned short n,
                        const char* text,
                        int* matchlength);
static int matchtimes_m(regex_t* p,
                        unsigned short m,
                        const char* text,
                        int* matchlength);
static int matchtimes_nm(regex_t* p,
                         unsigned short n,
                         unsigned short m,
                         const char* text,
                         int* matchlength);
static int matchgroup(regex_t* p,
                      const char* text,
                      int* matchlength,
                      const char* text_start,
                      re_match_result* out);
static int matchgrouptimes(regex_t* p,
                           regex_t* pattern,
                           const char* text,
                           int* matchlength,
                           unsigned short min,
                           unsigned short max,
                           const char* text_start,
                           re_match_result* out);
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

static int re_matchp_internal(re_t pattern,
                              const char* text,
                              int* matchlength,
                              const char* text_start,
                              re_match_result* out) {
  int num_patterns = 0;
  re_match_steps = 0;
  *matchlength = 0;
  if (pattern != 0) {
    if ((pattern->type & ~RE_TYPE_ICASE) == BEGIN) {
      return ((matchpattern(getnext(pattern), text, matchlength, &num_patterns,
                            text_start, out))
                  ? 0
                  : -1);
    } else {
      int idx = -1;

      do {
        idx += 1;
        num_patterns = 0;
        reset_spans(out);

        if (matchpattern(pattern, text, matchlength, &num_patterns, text_start,
                         out)) {
          // empty branch matches null (i.e. ok, but *matchlength == 0)
          if (*matchlength && text[0] == '\0')
            return -1;

          return idx;
        }

        //  Reset match length for the next starting point
        *matchlength = 0;

      } while (*text++ != '\0');
    }
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
    case GROUPEND:
      return 1;
    case BEGIN:
      return 0;
    default:
      if (icase) {
        return tolower((unsigned char)p->u.ch) == tolower((unsigned char)c);
      }
      return (p->u.ch == c);
  }
}

static unsigned matchcount(regex_t* p, const char* text, unsigned max) {
  unsigned count = 0;
  while (*text && count < max && matchone(p, *text)) {
    text++;
    count++;
  }
  return count;
}

static int matchstar(regex_t* p,
                     regex_t* pattern,
                     const char* text,
                     int* matchlength,
                     const char* text_start,
                     re_match_result* out) {
  int num_patterns = 0;
  re_span old_spans[RE_MAX_SPANS];
  save_spans(old_spans, out);
  if (matchplus(p, pattern, text, matchlength, text_start, out)) {
    return 1;
  }
  restore_spans(out, old_spans);
  if (matchpattern(pattern, text, matchlength, &num_patterns, text_start,
                   out)) {
    return 1;
  }
  restore_spans(out, old_spans);
  return 0;
}

static int matchplus(regex_t* p,
                     regex_t* pattern,
                     const char* text,
                     int* matchlength,
                     const char* text_start,
                     re_match_result* out) {
  int num_patterns = 0;
  const char* prepoint = text;
  text += matchcount(p, text, UINT_MAX);

  re_span old_spans[RE_MAX_SPANS];
  save_spans(old_spans, out);

  for (; text > prepoint; text--) {
    if (matchpattern(pattern, text, matchlength, &num_patterns, text_start,
                     out)) {
      *matchlength += text - prepoint;
      return 1;
    }
    restore_spans(out, old_spans);
    DEBUG_P("+ pattern does not match %s\n", &text[1]);
  }
  DEBUG_P("+ pattern did not match %s\n", prepoint);
  return 0;
}

static int matchquestion(regex_t* p,
                         regex_t* pattern,
                         const char* text,
                         int* matchlength,
                         const char* text_start,
                         re_match_result* out) {
  int num_patterns = 0;
  if ((p->type & ~RE_TYPE_ICASE) == UNUSED)
    return 1;

  re_span old_spans[RE_MAX_SPANS];
  save_spans(old_spans, out);

  if (matchpattern(pattern, text, matchlength, &num_patterns, text_start,
                   out)) {
#ifdef DEBUG
    DEBUG_P("? matched %s\n", text);
#endif
    return 1;
  }
  restore_spans(out, old_spans);
  if (*text && matchone(p, *text++)) {
    if (matchpattern(pattern, text, matchlength, &num_patterns, text_start,
                     out)) {
      (*matchlength)++;
#ifdef DEBUG
      DEBUG_P("? matched %s\n", text);
#endif
      return 1;
    }
  }
  restore_spans(out, old_spans);
  return 0;
}

static int matchtimes(regex_t* p,
                      unsigned short n,
                      const char* text,
                      int* matchlength) {
  unsigned count = matchcount(p, text, n);
  if (count != n)
    return 0;
  *matchlength += count;
  return 1;
}

static int matchtimes_n(regex_t* p,
                        unsigned short n,
                        const char* text,
                        int* matchlength) {
  unsigned count = matchcount(p, text, UINT_MAX);
  if (count < n)
    return 0;
  *matchlength += count;
  return 1;
}

static int matchtimes_m(regex_t* p,
                        unsigned short m,
                        const char* text,
                        int* matchlength) {
  *matchlength += matchcount(p, text, m);
  return 1;
}

static int matchtimes_nm(regex_t* p,
                         unsigned short n,
                         unsigned short m,
                         const char* text,
                         int* matchlength) {
  unsigned count = matchcount(p, text, m);
  if (count < n)
    return 0;
  *matchlength += count;
  return 1;
}

static int matchbranch(regex_t* p,
                       regex_t* pattern,
                       const char* text,
                       int* matchlength,
                       const char* text_start,
                       re_match_result* out) {
  int num_patterns = 0;
  const char* prepoint = text;
  if ((p->type & ~RE_TYPE_ICASE) == UNUSED)
    return 1;

  re_span old_spans[RE_MAX_SPANS];
  save_spans(old_spans, out);

  /* Match the current p (previous) */
  if (*text && matchone(p, *text++)) {
    (*matchlength)++;
    return 1;
  }
  if ((pattern->type & ~RE_TYPE_ICASE) == UNUSED)
    // empty branch "0|" allows NULL text
    return 1;

  restore_spans(out, old_spans);

  /* or the next branch */
  if (matchpattern(pattern, prepoint, matchlength, &num_patterns, text_start,
                   out))
    return 1;

  restore_spans(out, old_spans);
  return 0;
}

static int matchgroup(regex_t* p,
                      const char* text,
                      int* matchlength,
                      const char* text_start,
                      re_match_result* out) {
  int pre = *matchlength;
  int num_patterns = 0, length = pre;
  regex_t* groupstart = p;
  const regex_t* groupend =
      getindex(p, p->u.group_size + 1);  //&p[p->u.group_size + 1];
  DEBUG_P("does GROUP (%u) match %s?\n", (unsigned)p->u.group_size, text);

  int g_num = p->u.group_num;
  re_span old_spans[RE_MAX_SPANS];
  save_spans(old_spans, out);
  if (out) {
    out->spans[g_num].start = text - text_start;
  }

  p = getnext(p);
  while (p < groupend) {
    if ((p->type & ~RE_TYPE_ICASE) ==
        UNUSED)  // only with invalid external compiles
      return 0;
    regex_t* resume_from = p;
    if (!matchpattern(p, text, &length, &num_patterns, text_start, out)) {
      DEBUG_P("GROUP did not match %.*s (len %d, patterns %d)\n", length,
              text - *matchlength, *matchlength, num_patterns);
      *matchlength = pre;
      restore_spans(out, old_spans);
      return 0;
    }
    DEBUG_P("GROUP did match %.*s (len %d, patterns %d)\n", length,
            text - *matchlength, *matchlength, num_patterns);
    int delta = length - *matchlength;
    text += delta;
    p = getindex(groupstart, num_patterns);
    *matchlength += delta;
    /* matchquestion()/matchstar()/matchplus()/matchbranch() each recurse
     * into matchpattern() with their own fresh, local "num_patterns" rather
     * than this function's, so whenever the group's content resolves via
     * one of those (any '*', '+', '?' or '|' inside the group), the above
     * getindex() can fail to advance past "resume_from" -- re-matching the
     * same node forever and blowing the stack. Stop instead: the group is
     * considered fully matched at the length/text position reached so far. */
    if (p <= resume_from)
      break;
  }
  DEBUG_P("ENDGROUP did match %s (len %d, patterns %d)\n", text - *matchlength,
          *matchlength, num_patterns);
  if (out) {
    out->spans[g_num].end = text - text_start;
  }
  return 1;
}

/* Match a quantified group, e.g. "(ab)+", "(ab){2,4}": greedily expand the
 * group up to 'max' times (0 = unbounded), recording the text position and
 * accumulated matchlength reached after each repetition, then backtrack
 * from the greediest count down to 'min' until 'pattern' (whatever follows
 * the quantifier) matches at that point. Mirrors matchplus()/matchtimes_n()
 * for a single atom, but repeats matchgroup() instead of matchone(). */
static int matchgrouptimes(regex_t* p,
                           regex_t* pattern,
                           const char* text,
                           int* matchlength,
                           unsigned short min,
                           unsigned short max,
                           const char* text_start,
                           re_match_result* out) {
  const char* pos[MAX_GROUP_REPEATS + 1];
  int cum[MAX_GROUP_REPEATS + 1];
  re_span history[MAX_GROUP_REPEATS + 1][RE_MAX_SPANS];
  int reps = 0;
  const int base = *matchlength;

  pos[0] = text;
  cum[0] = base;
  save_spans(history[0], out);

  while (reps < MAX_GROUP_REPEATS && (max == 0 || reps < max)) {
    int ml = cum[reps];
    restore_spans(out, history[reps]);
    if (!matchgroup(p, pos[reps], &ml, text_start, out) || ml == cum[reps])
      break;
    reps++;
    cum[reps] = ml;
    pos[reps] = text + (ml - base);
    save_spans(history[reps], out);
  }

  if (reps < min) {
    *matchlength = base;
    restore_spans(out, history[0]);
    return 0;
  }

  for (int k = reps; k >= (int)min; k--) {
    int trial_matchlength = cum[k];
    int num_patterns = 0;
    restore_spans(out, history[k]);
    if (matchpattern(pattern, pos[k], &trial_matchlength, &num_patterns,
                     text_start, out)) {
      *matchlength = trial_matchlength;
      return 1;
    }
  }

  *matchlength = base;
  restore_spans(out, history[0]);
  return 0;
}

static inline int ismultimatch(unsigned short type) {
  type &= ~RE_TYPE_ICASE;
  return (unsigned)(type - TIMES) <= TIMES_NM - TIMES;
}

/* Iterative matching */
static int matchpattern(regex_t* pattern,
                        const char* text,
                        int* matchlength,
                        int* num_patterns,
                        const char* text_start,
                        re_match_result* out) {
  int pre = *matchlength;
  if (++re_match_steps > MAX_MATCH_STEPS)
    return 0;
  while (1) {
    if ((pattern->type & ~RE_TYPE_ICASE) == UNUSED) {
      return 1;
    }

    regex_t* next_pattern = getnext(pattern);

    /* GROUPEND always terminates the current pattern chain, even when a
     * quantifier for the enclosing group follows it. Checking this before
     * the next_pattern-based quantifier lookaheads keeps matchgroup()'s
     * internal matchpattern() call (matching the group's own contents)
     * from reading past its own GROUPEND and mistaking the group's own
     * quantifier for one that applies to the group's last inner atom. */
    if ((pattern->type & ~RE_TYPE_ICASE) == GROUPEND) {
      (*num_patterns)++;
      DEBUG_P("GROUPEND matches %.*s (len %d, patterns %d)\n", *matchlength,
              text - *matchlength, *matchlength, *num_patterns);
      return 1;
    } else if ((next_pattern->type & ~RE_TYPE_ICASE) == QUESTIONMARK) {
      return matchquestion(pattern, getnext(next_pattern), text, matchlength,
                           text_start, out);
    } else if ((next_pattern->type & ~RE_TYPE_ICASE) == STAR) {
      // int i = (pattern[1].type == GROUPEND) ? pattern[1].u.group_start : 0;
      return matchstar(pattern, getnext(next_pattern), text, matchlength,
                       text_start, out);
    } else if ((next_pattern->type & ~RE_TYPE_ICASE) == PLUS) {
      DEBUG_P("PLUS match %s?\n", text);
      // int i = (pattern[1].type == GROUPEND) ? pattern[1].u.group_start : 0;
      return matchplus(pattern, getnext(next_pattern), text, matchlength,
                       text_start, out);
    } else if (ismultimatch(next_pattern->type)) {
      const int beforelen = *matchlength;
      int retval = 0;
      if ((next_pattern->type & ~RE_TYPE_ICASE) == TIMES) {
        // int i = (pattern[1].type == GROUPEND) ? pattern[1].u.group_start : 0;
        retval = matchtimes(pattern, next_pattern->u.n, text, matchlength);
      } else if ((next_pattern->type & ~RE_TYPE_ICASE) == TIMES_N) {
        retval = matchtimes_n(pattern, next_pattern->u.n, text, matchlength);
      } else if ((next_pattern->type & ~RE_TYPE_ICASE) == TIMES_M) {
        retval = matchtimes_m(pattern, next_pattern->u.m, text, matchlength);
      } else if ((next_pattern->type & ~RE_TYPE_ICASE) == TIMES_NM) {
        // int i = (pattern[1].type == GROUPEND) ? pattern[1].u.group_start : 0;
        retval = matchtimes_nm(pattern, next_pattern->u.n, next_pattern->u.m,
                               text, matchlength);
      }

      if (!retval)
        return 0;
      else {
        const int consumed = *matchlength - beforelen;
        pre = *matchlength;
        (*num_patterns)++;
        pattern = getnext(next_pattern);
        text += consumed;
        continue;
      }

    } else if ((next_pattern->type & ~RE_TYPE_ICASE) == BRANCH) {
      // int i = (pattern[1].type == GROUPEND) ? pattern[1].u.group_start : 0;
      return matchbranch(pattern, getnext(next_pattern), text, matchlength,
                         text_start, out);
    } else if ((pattern->type & ~RE_TYPE_ICASE) == GROUP) {
      /* A quantifier following a group applies to the whole group, but it
       * sits after GROUPEND -- outside the span "next_pattern" (the node
       * right after the GROUP header) can see -- so it isn't caught by the
       * QUESTIONMARK/STAR/PLUS/ismultimatch checks above. */
      regex_t* after_group = getindex(pattern, pattern->u.group_size + 2);
      unsigned short qmin = 0, qmax = 0;
      int quantified = 1;
      switch (after_group->type & ~RE_TYPE_ICASE) {
        case QUESTIONMARK:
          qmin = 0;
          qmax = 1;
          break;
        case STAR:
          qmin = 0;
          qmax = 0;
          break;
        case PLUS:
          qmin = 1;
          qmax = 0;
          break;
        case TIMES:
          qmin = after_group->u.n;
          qmax = after_group->u.n;
          break;
        case TIMES_N:
          qmin = after_group->u.n;
          qmax = 0;
          break;
        case TIMES_M:
          qmin = 0;
          qmax = after_group->u.m;
          break;
        case TIMES_NM:
          qmin = after_group->u.n;
          qmax = after_group->u.m;
          break;
        default:
          quantified = 0;
          break;
      }

      if (quantified)
        return matchgrouptimes(pattern, getnext(after_group), text, matchlength,
                               qmin, qmax, text_start, out);

      const int beforelen = *matchlength;
      const int retval =
          matchgroup(pattern, text, matchlength, text_start, out);

      if (!retval)
        return 0;
      else {
        text += (*matchlength - beforelen);
        pre = *matchlength;
        (*num_patterns) += pattern->u.group_size + 2;
        pattern = getindex(pattern, pattern->u.group_size + 2);
        continue;
      }
    } else if (((pattern->type & ~RE_TYPE_ICASE) == END) &&
               (next_pattern->type & ~RE_TYPE_ICASE) == UNUSED) {
      return (text[0] == '\0');
    }
    (*matchlength)++;
    (*num_patterns)++;

    if (text[0] == '\0')
      break;
    if (!matchone(pattern, *(text++)))
      break;
    pattern = next_pattern;
  }

  *matchlength = pre;
  return 0;
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
