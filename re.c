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
 *   '\xXX'     Hex-encoded byte
 *   '\|'       Branch Or; the alternatives are whole concatenations and
 *              a group bounds them, e.g. ab\|cd, x\(ab\|cd\)y
 *   '\{n\}'    Match n times
 *   '\{n,\}'   Match n or more times
 *   '\{,m\}'   Match m or less times
 *   '\{n,m\}'  Match n to m times; see re.h for what makes an interval or a
 *              POSIX class name a bad pattern rather than literal text
 *   '\(...\)'  Group, including a trailing quantifier applied to the group
 *   '\(?:...\)' Shy group: groups without consuming a capture register
 *
 * TODO:
 *   - \b word boundary support
 */

#include "re.h"
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _UNICODE
#include <locale.h>
#include <stdlib.h>
#endif

/* Definitions: */

#ifndef CPROVER
#define MAX_REGEXP_OBJECTS 30 /* Max number of regex symbols in expression. */
#else
#define MAX_REGEXP_OBJECTS 8 /* faster formal proofs */
#endif

/* Largest count a "\{n,m\}" interval may spell.  This is the value Emacs
 * accepts up to (its RE_DUP_MAX, a name <limits.h> also defines, hence the
 * spelling here), and it is also the widest value the compiled node's
 * 16-bit count fields hold, so an accepted count is always representable. */
#define RE_INTERVAL_MAX 65535

/* quant_bounds()' "no upper bound" marker.  A spelled count never reaches
 * it, RE_INTERVAL_MAX being the largest one a pattern can express. */
#define RE_REP_INF UINT_MAX

/* Bound on the number of times a quantified group ("\(...\)+" etc.) is
 * expanded. Each repetition is two more match_seq() frames on the C stack
 * -- measured at 768 bytes per repetition with gcc -O2, 1072 with gcc -O0
 * and about 1.5 KiB under clang's AddressSanitizer -- so this caps how far
 * a single group can drive the matcher down.
 *
 * Reaching it is RE_STATUS_TOO_COMPLEX, not a no-match and not a shorter
 * match than the pattern asked for; see match_rep(). Raising it is not a
 * matter of changing the number: MAX_MATCH_DEPTH stops a counted group at
 * 2047 repetitions whatever this says, and the megabytes of C stack that
 * takes are why the pending work is to move the continuation chain off the
 * stack rather than to raise either constant. */
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
 * than smashing the stack. One unit is one match_seq() frame; measured
 * against a counted group, 4096 of them are 1.5 MB of stack at gcc -O2,
 * 2.2 MB at -O0 and over 3 MB under AddressSanitizer. That is the real
 * worst case a caller must have stack for, and it is the number to bring
 * down by moving the continuation chain off the C stack. */
#ifndef CPROVER
#define MAX_MATCH_DEPTH 4096
#else
#define MAX_MATCH_DEPTH 64 /* faster formal proofs */
#endif

/* How often re_exec_options' cancel callback is polled, in work units.
 * A power of two, and the phase is chosen so the very first unit polls:
 * a callback that always cancels then cancels immediately, however small
 * the pattern. */
#define RE_CANCEL_POLL_STEPS 256u

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
    struct {
      /* CHAR's codepoint, in halves: the union is two-byte aligned and
       * stays that way, so a 21-bit codepoint does not fit one member. */
      unsigned short cp_lo;
      unsigned short cp_hi;
    };
    struct {
      unsigned char group_size; /*  OR the number of group patterns. */
      /* The capture register this group fills, or 0 for a shy group
       * ("\(?:...\)"), which fills none -- group_span() reads 0 as "no
       * slot", so a shy group is a group everywhere else in the matcher
       * and a capture nowhere. */
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

/* RE_STORAGE_ALIGNMENT is the public spelling of what caller storage has to
 * satisfy for the nodes above to be read in place; the two must not drift. */
_Static_assert(_Alignof(regex_t) == RE_STORAGE_ALIGNMENT,
               "RE_STORAGE_ALIGNMENT must equal _Alignof(regex_t)");

/* Whether 'storage' may hold a compiled program.  Casting an insufficiently
 * aligned buffer to regex_t* and reading through it is undefined behavior
 * (and UBSan reports it), so the compilers refuse the buffer instead. */
static int storage_aligned(const void* storage) {
  return !((uintptr_t)storage % RE_STORAGE_ALIGNMENT);
}

/*
 * Character-class (and inline) data is stored as a trailing string that
 * overlays the union, beginning at offset 0 of `u`.  The storage extends
 * past this object into the surrounding compiled-pattern byte buffer.
 * RE_CCL_STR(p) points at the first class char (= old &p->u.data[-1]);
 * RE_CCL_DAT(p) matches the old `data` base (= old &p->u.data[0]).  Both
 * are byte-identical to the pre-refactor layout.
 */
#define RE_CCL_STR(p) ((char*)&(p)->u)
#define RE_CCL_DAT(p) (RE_CCL_STR(p) + 1)

#define RE_TYPE_ICASE (1 << 15)

/* ------------------------------------------------------------------------
 * UTF-8 glyphs
 *
 * The matcher steps by character, not by byte: '.' consumes a whole
 * glyph, a multi-byte literal in the pattern is one atom, and quantifiers
 * and intervals count glyphs -- Emacs' semantics.  Reported spans stay
 * byte offsets into the subject.
 *
 * A byte sequence that is not well-formed UTF-8 is not an error; each
 * stray byte is a glyph of its own, the rule kg's utf8_glyph_span_at()
 * uses.  Such a glyph is given a stand-in codepoint above anything UTF-8
 * can encode, so it never compares equal to a real character and never
 * falls inside a range.
 * ---------------------------------------------------------------------- */

#define RE_CP_STRAY 0x200000u /* + the byte, for a byte that stands alone */

typedef struct {
  unsigned cp;  /* the codepoint, or RE_CP_STRAY + the byte */
  unsigned len; /* bytes it occupies, 1 to 4 */
} re_glyph;

/* The glyph at 's', which must be NUL-terminated: the terminator is not a
 * continuation byte, so an unfinished sequence at the end of the subject
 * decodes as stray bytes rather than reading past it. */
static re_glyph glyph_at(const char* s) {
  unsigned char lead = (unsigned char)s[0];
  re_glyph g = {RE_CP_STRAY + lead, 1};
  unsigned need, cp, i;

  if (lead < 0x80) {
    g.cp = lead;
    return g;
  } else if (lead >= 0xC2 && lead <= 0xDF) {
    need = 1, cp = lead & 0x1Fu;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    need = 2, cp = lead & 0x0Fu;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    need = 3, cp = lead & 0x07u;
  } else {
    return g;
  }

  for (i = 1; i <= need; i++) {
    if (((unsigned char)s[i] & 0xC0) != 0x80)
      return g;
    cp = (cp << 6) | ((unsigned char)s[i] & 0x3Fu);
  }
  g.cp = cp;
  g.len = need + 1;
  return g;
}

/* Where the glyph ending at 'pos' begins, 'pos' being a glyph boundary at
 * or after 'start'.  Each candidate is decoded forward again, so this
 * agrees with glyph_at() on stray sequences too. */
static const char* glyph_prev(const char* start, const char* pos) {
  const char* p = pos - 1;
  const char* limit = pos - start > 4 ? pos - 4 : start;

  while (p > limit && ((unsigned char)*p & 0xC0) == 0x80)
    p--;
  return glyph_at(p).len == (unsigned)(pos - p) ? p : pos - 1;
}

/* A literal byte read as a glyph.  A non-ASCII one stands alone, so it is
 * the stray byte of that value rather than the character U+00XX. */
static unsigned byte_cp(unsigned char b) {
  return b < 0x80 ? b : RE_CP_STRAY + b;
}

static void set_char_cp(regex_t* p, unsigned cp) {
  p->u.cp_lo = (unsigned short)cp;
  p->u.cp_hi = (unsigned short)(cp >> 16);
}

static unsigned char_cp(const regex_t* p) {
  return p->u.cp_lo | ((unsigned)p->u.cp_hi << 16);
}

/* Whether 'type' is one of the repetition operators. */
static int isquantifier(unsigned short type) {
  return type == QUESTIONMARK || type == STAR || type == PLUS ||
         (unsigned)(type - TIMES) <= TIMES_NM - TIMES;
}

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

/* getnext() for the execute path, which never writes to the program: the
 * only pass that does is the compile-time ICASE marking. */
static const regex_t* next_node(const regex_t* pattern) {
  return (const regex_t*)((const unsigned char*)pattern + getsize(pattern));
}

/* ------------------------------------------------------------------------
 * Compile-time emitter
 *
 * One cursor over the program being compiled: where the next node goes,
 * and how many nodes precede it.  Those two facts used to live in separate
 * variables, and the invalid-"\x" fallback advanced the byte cursor by up
 * to three nodes while the count advanced by one.  Since the "\)" handler
 * scans back over node *indices*, a later group was then misparsed:
 * "\(a\)x\(b\)" compiled and "\(a\)\xZ\(b\)" was rejected.  One cursor
 * cannot disagree with itself.
 * ---------------------------------------------------------------------- */

struct re_emitter {
  unsigned char* base; /* the caller's buffer */
  unsigned char* next; /* where the node under construction starts */
  unsigned char* end;  /* one past the buffer */
  unsigned nodes;      /* nodes already finished */
};

/* Room for one more whole node.  Spelled as an addition rather than
 * "end - sizeof(regex_t)": with a caller buffer smaller than one node that
 * subtraction underflows the unsigned size and the pointer arithmetic
 * built on it is undefined. */
static int emitter_room(const struct re_emitter* em) {
  return em->next + sizeof(regex_t) < em->end;
}

/* The node under construction.  It joins the program at emitter_commit(),
 * which reads its type to learn how wide it is. */
static regex_t* emitter_node(const struct re_emitter* em) {
  return (regex_t*)em->next;
}

static void emitter_commit(struct re_emitter* em) {
  em->next += getsize(emitter_node(em));
  em->nodes++;
}

/* The already-finished node at logical index 'index'. */
static regex_t* emitter_node_at(const struct re_emitter* em, unsigned index) {
  regex_t* p = (regex_t*)em->base;
  unsigned n;

  for (n = 0; n < index; n++)
    p = getnext(p);
  return p;
}

/* Emit one literal-character node, or fail for want of room. */
static int emit_char(struct re_emitter* em, unsigned cp) {
  regex_t* node;

  if (!emitter_room(em))
    return 0;
  node = emitter_node(em);
  node->type = CHAR;
  set_char_cp(node, cp);
  emitter_commit(em);
  return 1;
}

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
static int matchcharclass(const re_glyph* g, const char* str, int icase);
static int matchone(const regex_t* p, const re_glyph* g);
static int matchdigit(int c);
static int matchalpha(int c);
static int matchwhitespace(int c);
static int matchmetachar(const re_glyph* g, char m);
static int matchdot(unsigned cp);
static int ismetachar(char c);
static int named_class_len(const char* str);
static int hex(char c);
static int re_matchp_internal(re_t pattern,
                              const char* text,
                              int* matchlength,
                              const char* text_start,
                              const char* text_limit,
                              const re_exec_options* options,
                              re_match_result* out,
                              int* exhausted);

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
  if (storage && !storage_aligned(storage)) {
    return RE_STATUS_BAD_PATTERN;
  }

  _Alignas(regex_t) unsigned char temp_buffer[RE_MAX_COMPILED_BYTES];
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
  return re_exec_with_options(regex, text, start_offset, NULL, out);
}

re_status re_exec_with_options(re_t regex,
                               const char* text,
                               int start_offset,
                               const re_exec_options* options,
                               re_match_result* out) {
  return re_exec_bounded(regex, text, start_offset, RE_LIMIT_NONE, options,
                         out);
}

re_status re_exec_bounded(re_t regex,
                          const char* text,
                          int start_offset,
                          int limit,
                          const re_exec_options* options,
                          re_match_result* out) {
  int exhausted = 0;

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
  /* A limit at or past the subject's end is no limit at all, and is the
   * one RE_LIMIT_NONE folds onto: past that point the subject's own end
   * is the tighter bound, and the two spellings must not answer
   * differently. */
  if (limit == RE_LIMIT_NONE || limit > len) {
    limit = len;
  }
  if (limit < 0 || start_offset > limit) {
    return RE_STATUS_NO_MATCH;
  }

  int num_groups = 0;
  const regex_t* p_node = regex;
  while (p_node) {
    if ((p_node->type & ~RE_TYPE_ICASE) == GROUP) {
      if (p_node->u.group_num > num_groups) {
        num_groups = p_node->u.group_num;
      }
    }
    if ((p_node->type & ~RE_TYPE_ICASE) == UNUSED)
      break;
    p_node = next_node(p_node);
  }

  if (out) {
    out->nspans = num_groups + 1;
  }
  reset_spans(out);

  int matchlength = 0;
  int res = re_matchp_internal(regex, text + start_offset, &matchlength, text,
                               text + limit, options, out, &exhausted);
  if (res >= 0) {
    if (out) {
      out->spans[0].start = start_offset + res;
      out->spans[0].end = start_offset + res + matchlength;
    }
    return RE_STATUS_OK;
  }

  /* Out of budget, or cancelled: the attempt was abandoned, which is not
   * the same answer as "this text does not match". */
  if (exhausted) {
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
  int first = 1;

  if (pattern[i + 1] == '^') {
    compiled->type = INV_CHAR_CLASS;
    i++;
  } else {
    compiled->type = CHAR_CLASS;
  }

  /* A ']' in the first position is a member rather than the terminator,
   * as it is in Emacs and POSIX: "[]a]" holds ']' and 'a'.  That also
   * makes "[]" and "[^]" what Emacs calls them -- unterminated, and so a
   * bad pattern rather than a class nothing can be in. */
  while (pattern[++i] != '\0') {
    if (pattern[i] == ']' && !first)
      break;
    first = 0;
    if (pattern[i] == '[' && pattern[i + 1] == ':') {
      const char* end = strstr(&pattern[i + 2], ":]");
      if (end) {
        int length = (end + 2) - &pattern[i];
        /* A class name this engine does not know is a bad pattern, as it
         * is in Emacs -- never the literal characters of its spelling,
         * which is how "[[:blank:]]" came to match 'a'. */
        if (named_class_len(&pattern[i]) != length)
          return 0;
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

  /* Reaching the end of the pattern instead of a ']' used to compile the
   * bracket expression anyway, so "[a" quietly meant the same as "[a]". */
  if (pattern[i] != ']')
    return 0;
  if (RE_CCL_DAT(compiled) + char_index >= storage_end)
    return 0;
  RE_CCL_DAT(compiled)[char_index] = '\0';
  *pattern_index = i;
  return 1;
}

/* Read a decimal interval count from [*s, end).  Advances *s past the
 * digits and reports through *have whether there were any.  Returns 0 for
 * a count past RE_INTERVAL_MAX, which Emacs rejects. */
static int parse_count(const char** s,
                       const char* end,
                       unsigned* val,
                       int* have) {
  const char* p = *s;
  unsigned v = 0;

  while (p < end && *p >= '0' && *p <= '9') {
    v = v * 10 + (unsigned)(*p - '0');
    if (v > RE_INTERVAL_MAX)
      return 0;
    p++;
  }
  *have = p != *s;
  *val = v;
  *s = p;
  return 1;
}

/* Compile the interval whose contents run [s, end) -- the text between
 * "\{" and "\}" -- into 'node'.  The accepted forms are Emacs': "{n}",
 * "{n,}", "{,m}" and "{n,m}", an absent bound meaning 0 below and
 * unbounded above, so "{}" is exactly zero and "{,}" is "any number".
 * Returns 0 for what Emacs rejects: anything but digits and one comma, or
 * an upper bound below the lower one. */
static int compile_interval(const char* s, const char* end, regex_t* node) {
  unsigned n = 0, m = 0;
  int have_n = 0, have_m = 0, comma = 0;

  if (!parse_count(&s, end, &n, &have_n))
    return 0;
  if (s < end && *s == ',') {
    comma = 1;
    s++;
    if (!parse_count(&s, end, &m, &have_m))
      return 0;
  }
  if (s != end)
    return 0;

  if (!comma) {
    node->type = TIMES;
    node->u.n = (unsigned short)n;
  } else if (!have_m) {
    node->type = TIMES_N;
    node->u.n = (unsigned short)n;
  } else if (!have_n) {
    node->type = TIMES_M;
    node->u.m = (unsigned short)m;
  } else {
    if (m < n)
      return 0;
    node->type = TIMES_NM;
    node->u.n = (unsigned short)n;
    node->u.m = (unsigned short)m;
  }
  return 1;
}

/* What a quantifier written at this point in the program can mean. */
enum quant_ctx {
  /* Nothing to repeat -- the start of the pattern, of a group or of an
   * alternative, or just after an anchor.  Emacs reads the quantifier
   * character literally there, and so does this engine. */
  QUANT_LITERAL,
  /* The preceding atom already carries a quantifier.  Emacs folds the two
   * into one; this engine has no faithful spelling for the composition
   * ("a\{2\}\{2,3\}" is 4 or 6 repetitions, not 4 to 6), and it used to
   * compile a node the matcher could only ever fail on.  Saying so is
   * better than either. */
  QUANT_BAD,
  QUANT_OK
};

static enum quant_ctx quant_context(const struct re_emitter* em) {
  unsigned short type;

  if (em->nodes == 0)
    return QUANT_LITERAL;
  type = emitter_node_at(em, em->nodes - 1)->type & ~RE_TYPE_ICASE;
  if (type == GROUP || type == BRANCH || type == BEGIN || type == END)
    return QUANT_LITERAL;
  return isquantifier(type) ? QUANT_BAD : QUANT_OK;
}

/* One "\(" still open while compiling.  The stack replaces two scans over
 * the half-written program: a forward one through the *pattern* that only
 * asked whether some later "\)" existed (so "\(\(a\)" compiled), and a
 * backward one over node indices. */
struct parse_frame {
  unsigned group_start;   /* node index of the GROUP */
  unsigned capture_index; /* its group_num */
};

/* The opening of a group: "\(", or Emacs' shy spelling "\(?:".  *i is on
 * the '(' and is left on the last pattern byte the opener spells.  The
 * answer is the capture number the group records -- 0 for a shy group,
 * which consumes none, so the numbering of the capturing groups around it
 * is exactly what it would be if the shy group were not there.  -1 is a
 * spelling this engine refuses.
 *
 * Emacs reserves "\(?" for the shy and the explicitly numbered group, and
 * only the shy one is honoured here.  "\(?1:...\)" would have to place a
 * group at a capture number the pattern names rather than at the one its
 * position gives it, which nothing in this compiler can express, so it is
 * refused -- as is every other "\(?", which Emacs rejects too.  None of
 * them falls back to the literal characters it is spelled with: that
 * fallback is how "\(?:a\)" came to match "?:a". */
static int group_open(const char* pattern, int* i, int* num_groups) {
  if (pattern[*i + 1] == '?') {
    if (pattern[*i + 2] != ':')
      return -1;
    *i += 2;
    return 0;
  }
  if (++*num_groups >= RE_MAX_SPANS)
    return -1;
  return *num_groups;
}

/* The "\xXX" escape, *i on the 'x'.  A well-formed one is a single CHAR
 * node holding the byte.  A malformed one is the literal characters it is
 * spelled with -- '\', 'x' and whatever followed -- each its own CHAR node,
 * which is the reading tests/ok.lst has recorded all along.  Leaves *i on
 * the last pattern byte consumed; returns 0 only for want of room. */
static int compile_hex_escape(const char* pattern,
                              int* i,
                              struct re_emitter* em) {
  int hi = hex(pattern[*i + 1]);
  int lo = hi < 0 ? -1 : hex(pattern[*i + 2]);

  if (lo >= 0) {
    *i += 2;
    return emit_char(em, byte_cp((unsigned char)((hi << 4) + lo)));
  }
  if (!emit_char(em, '\\') || !emit_char(em, 'x'))
    return 0;
  if (!pattern[*i + 1])
    return 1;
  *i += 1;
  if (!emit_char(em, byte_cp((unsigned char)pattern[*i])))
    return 0;
  if (hi < 0 || !pattern[*i + 1])
    return 1;
  *i += 1;
  return emit_char(em, byte_cp((unsigned char)pattern[*i]));
}

re_t re_compile_to(const char* pattern,
                   unsigned char* re_data,
                   unsigned* size) {
  if (!storage_aligned(re_data))
    return 0;
  memset(re_data, 0, *size);

  int i = 0; /* index into pattern */
  int num_groups = 0;
  unsigned bytes = *size;
  *size = 0;

  struct re_emitter em = {re_data, re_data, re_data + bytes, 0};
  struct parse_frame open_groups[RE_MAX_SPANS];
  unsigned depth = 0;
  regex_t* re_compiled;

  /* Bound the scan by the pattern length rather than re-reading past the
   * terminator: some escape handlers (e.g. '\x') land `i` on the NUL and
   * the trailing `i += 1` then steps one byte past the allocation. */
  const int plen = pattern ? (int)strlen(pattern) : 0;
  while (i < plen && emitter_room(&em)) {
    char c = pattern[i];

    re_compiled = emitter_node(&em);
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
      case '*':
      case '+':
      case '?': {
        enum quant_ctx q = quant_context(&em);
        if (q == QUANT_BAD)
          return 0;
        if (q == QUANT_LITERAL) {
          /* Nothing to repeat: Emacs reads the character literally, as it
           * already does for "\{" in the same position. */
          re_compiled->type = CHAR;
          set_char_cp(re_compiled, (unsigned char)c);
          break;
        }
        re_compiled->type = c == '*' ? STAR : c == '+' ? PLUS : QUESTIONMARK;
      } break;

      /* Escaped character-classes (\s \S \w \W \d \D \*), the operators
       * spelled with a backslash (\( \) \| \{), and the subject anchors
       * (\` \'): */
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
              int capture = group_open(pattern, &i, &num_groups);

              if (capture < 0 || depth >= RE_MAX_SPANS)
                return 0;
              open_groups[depth].group_start = em.nodes;
              open_groups[depth].capture_index = (unsigned)capture;
              depth++;
              re_compiled->type = GROUP;
              re_compiled->u.group_size = 0;
              re_compiled->u.group_num = (unsigned char)capture;
            } break;
            case ')': {
              const struct parse_frame* f;
              regex_t* open;

              if (depth == 0)
                return 0; /* "a\)" -- Emacs' "Unmatched ) or \)" */
              f = &open_groups[--depth];
              open = emitter_node_at(&em, f->group_start);
              open->u.group_size =
                  (unsigned char)(em.nodes - f->group_start - 1);
              re_compiled->type = GROUPEND;
              re_compiled->u.group_start = (unsigned char)f->group_start;
              re_compiled->u.group_num_end = (unsigned char)f->capture_index;
            } break;
            case '|': {
              re_compiled->type = BRANCH;
            } break;
            case '`': {
              /* Emacs' subject-start anchor.  It is the same node as '^'
               * because '^' already asserts exactly this here -- the start
               * of the whole subject, never a line's -- so the spelling is
               * an alias and not a second semantics.  Without these two
               * cases the default below turned them into the literal
               * characters '`' and '\'', which is a pattern quietly
               * matching the wrong thing rather than one that fails. */
              re_compiled->type = BEGIN;
            } break;
            case '\'': {
              /* Emacs' subject-end anchor; the same relationship to '$'. */
              re_compiled->type = END;
            } break;
            case '{': {
              /* An interval, up to the closing "\}".  An unterminated one
               * is a bad pattern ("Unmatched \{" in Emacs), and so are
               * contents Emacs rejects -- never the literal characters of
               * the interval's own spelling.  With nothing to repeat, the
               * "\{" is Emacs' literal '{' and parsing resumes after it. */
              const char* p = &pattern[i + 1];
              enum quant_ctx q = quant_context(&em);
              while (*p != '\0' && !(*p == '\\' && *(p + 1) == '}'))
                p++;
              if (*p == '\0' || q == QUANT_BAD)
                return 0;
              if (q == QUANT_LITERAL) {
                re_compiled->type = CHAR;
                set_char_cp(re_compiled, '{');
                break;
              }
              if (!compile_interval(&pattern[i + 1], p, re_compiled))
                return 0;
              i = (p - pattern) + 1;
            } break;
            case 'x': {
              /* Emits its own nodes -- one, or the three or four of the
               * malformed spelling -- so the loop must not commit again. */
              if (!compile_hex_escape(pattern, &i, &em))
                return 0;
              i += 1;
              continue;
            }

            /* Escaped character, e.g. '.', '$' or '\\' */
            default: {
              re_glyph g = glyph_at(&pattern[i]);
              re_compiled->type = CHAR;
              set_char_cp(re_compiled, g.cp);
              i += (int)g.len - 1;
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
        if (!compile_charclass(pattern, &i, re_compiled, (char*)em.end))
          return 0;
      } break;

      case '\0':  // EOL (dead-code)
        return 0;

      /* Other characters: a multi-byte one is a single atom, so "å*"
       * repeats the character rather than its last byte. */
      default: {
        re_glyph g = glyph_at(&pattern[i]);
        re_compiled->type = CHAR;
        set_char_cp(re_compiled, g.cp);
        i += (int)g.len - 1;
      } break;
    }
    i += 1;
    emitter_commit(&em);
  }
  /* The loop also stops when the buffer runs out. Compiling a prefix of
   * the pattern and calling that a success is worse than failing: the
   * caller gets a regex that quietly means something else. */
  if (i < plen)
    return 0;
  /* A "\(" that was never closed. The old forward scan only asked whether
   * *some* later "\)" existed, so "\(\(a\)" satisfied both groups with the
   * one closing bracket and compiled. */
  if (depth != 0)
    return 0;
  /* 'UNUSED' is a sentinel used to indicate end-of-pattern. The loop's
   * bounds check only guarantees room for a full regex_t before *starting*
   * an iteration, and a character class is as wide as its contents, so the
   * emitter can sit within sizeof(unsigned short) of the buffer end here
   * and writing the sentinel's type field would overflow the buffer. */
  if ((char*)em.next + sizeof(unsigned short) > (char*)em.end)
    return 0;
  re_compiled = emitter_node(&em);
  re_compiled->type = UNUSED;

  /* Calculate final, compressed actual size. */
  *size = (unsigned)((unsigned char*)getnext(re_compiled) - em.base);

  return (re_t)re_data;
}

re_t re_compile(const char* pattern) {
  static _Alignas(
      regex_t) unsigned char buffer[MAX_REGEXP_OBJECTS * sizeof(regex_t)];
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

/* Spell 'cp' back out as UTF-8 in 'buff' (5 bytes are always enough); a
 * stray-byte codepoint becomes the single byte it stands for. */
static void cp_to_utf8(unsigned cp, char* buff) {
  static const unsigned char lead[] = {0, 0xC0, 0xE0, 0xF0};
  int extra = cp < 0x80 || cp >= RE_CP_STRAY ? 0
              : cp < 0x800                   ? 1
              : cp < 0x10000                 ? 2
                                             : 3;
  int i = 0;

  if (cp >= RE_CP_STRAY)
    cp -= RE_CP_STRAY;
  buff[i++] = (char)(lead[extra] | (cp >> (6 * extra)));
  while (extra--)
    buff[i++] = (char)(0x80 | ((cp >> (6 * extra)) & 0x3F));
  buff[i] = '\0';
}

void re_string(regex_t* pattern, char* buffer, unsigned* size) {
#if 0
  const char *const types[] = { "UNUSED", "DOT", "BEGIN", "END", "QUESTIONMARK", "STAR", "PLUS", "CHAR", "CHAR_CLASS", "INV_CHAR_CLASS", "DIGIT", "NOT_DIGIT", "ALPHA", "NOT_ALPHA", "WHITESPACE", "NOT_WHITESPACE", "BRANCH", "GROUP", "GROUPEND", "TIMES", "TIMES_N", "TIMES_M", "TIMES_NM" };
#endif
  unsigned count = *size;
  unsigned char i = 0;
  int j;
  unsigned char group_end;
  char c;
  char cp_buff[5];
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
        cp_to_utf8(char_cp(pattern), cp_buff);
        re_string_cat_fmt_(buffer, "%s", cp_buff);
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

/* The value every class predicate below is asked about: a glyph's ASCII
 * character, or EOF for a glyph that has none.  "\w", "\d", "\s" and the
 * POSIX classes stay ASCII-only, and <ctype.h> answers "no" for EOF, so
 * that holds for multi-byte and stray glyphs without a test in each
 * predicate.  "[:ascii:]" and "[:nonascii:]" are the two that read the
 * sentinel itself, which is what makes them meaningful here. */
static int ascii_cp(unsigned cp) {
  return cp < 0x80 ? (int)cp : EOF;
}

/* ASCII-only case folding, the only folding this engine does. */
static unsigned fold_cp(unsigned cp, int icase) {
  return icase && cp < 0x80 ? (unsigned)tolower((int)cp) : cp;
}

static int matchdigit(int c) {
  return isdigit(c);
}
static int matchalpha(int c) {
  return isalpha(c);
}
static int matchwhitespace(int c) {
  return isspace(c);
}
static int matchalphanum(int c) {
  return ((c == '_') || matchalpha(c) || matchdigit(c));
}
static int matchposixalnum(int c) {
  return isalnum(c);
}
static int matchcontrol(int c) {
  return iscntrl(c);
}
static int matchgraph(int c) {
  return isgraph(c);
}
static int matchprint(int c) {
  return isprint(c);
}
static int matchpunct(int c) {
  return ispunct(c);
}
static int matchxdigit(int c) {
  return isxdigit(c);
}
static int matchblank(int c) {
  return c == ' ' || c == '\t';
}
/* Emacs' "[:word:]" is the buffer's word syntax; for ASCII text that is
 * the alphanumerics, and unlike "\w" it excludes '_'. */
static int matchword(int c) {
  return isalnum(c);
}
static int matchascii(int c) {
  return c != EOF;
}
static int matchnonascii(int c) {
  return c == EOF;
}
static int matchlower(int c) {
  return islower(c);
}
static int matchupper(int c) {
  return isupper(c);
}

typedef int (*char_matcher)(int);

static const char_matcher metachar_matchers[256] = {
    ['d'] = matchdigit,    ['D'] = matchdigit,      ['w'] = matchalphanum,
    ['W'] = matchalphanum, ['s'] = matchwhitespace, ['S'] = matchwhitespace,
};

struct named_class {
  const char* name;
  unsigned char length;
  char_matcher match[2];
};

/* The POSIX class names this engine honours.  Emacs also accepts
 * "[:multibyte:]" and "[:unibyte:]", whose meaning is a property of the
 * string's representation rather than of the character in it; nothing
 * here can answer them, so compile_charclass() rejects those (and every
 * unknown name) instead of quietly reading it as a set of characters. */
static const struct named_class named_classes[] = {
    {"[:digit:]", 9, {matchdigit, matchdigit}},
    {"[:alpha:]", 9, {matchalpha, matchalpha}},
    {"[:alnum:]", 9, {matchposixalnum, matchposixalnum}},
    {"[:space:]", 9, {matchwhitespace, matchwhitespace}},
    {"[:blank:]", 9, {matchblank, matchblank}},
    {"[:cntrl:]", 9, {matchcontrol, matchcontrol}},
    {"[:graph:]", 9, {matchgraph, matchgraph}},
    {"[:print:]", 9, {matchprint, matchprint}},
    {"[:punct:]", 9, {matchpunct, matchpunct}},
    {"[:xdigit:]", 10, {matchxdigit, matchxdigit}},
    {"[:lower:]", 9, {matchlower, matchalpha}},
    {"[:upper:]", 9, {matchupper, matchalpha}},
    {"[:word:]", 8, {matchword, matchword}},
    {"[:ascii:]", 9, {matchascii, matchascii}},
    {"[:nonascii:]", 12, {matchnonascii, matchnonascii}},
};

/* The spelled-out length of the named class at 'str', or 0 when the name
 * is not one this engine knows. */
static int named_class_len(const char* str) {
  for (unsigned i = 0; i < sizeof(named_classes) / sizeof(named_classes[0]);
       i++) {
    if (strncmp(str, named_classes[i].name, named_classes[i].length) == 0)
      return named_classes[i].length;
  }
  return 0;
}

/* The named class starting at *str, if any: +1 when 'c' is in it, -1 when
 * it is not, 0 when *str does not spell one this engine knows.  A hit
 * advances *str past the whole "[:name:]". */
static int matchnamedclass(int c, const char** str, int icase) {
  for (unsigned i = 0; i < sizeof(named_classes) / sizeof(named_classes[0]);
       i++) {
    if (strncmp(*str, named_classes[i].name, named_classes[i].length) != 0)
      continue;
    *str += named_classes[i].length;
    return named_classes[i].match[!!icase](c) * 2 - 1;
  }
  return 0;
}

static int matchdot(unsigned cp) {
#if defined(RE_DOT_MATCHES_NEWLINE) && (RE_DOT_MATCHES_NEWLINE == 1)
  (void)cp;
  return 1;
#else
  return cp != '\n' && cp != '\r';
#endif
}
static int ismetachar(char c) {
  return metachar_matchers[(unsigned char)c] != NULL;
}

/* "\d" and friends inside a bracket expression.  'm' is the letter after
 * the backslash, and must be one ismetachar() accepts; the upper-case
 * spelling of each is its complement. */
static int matchmetachar(const re_glyph* g, char m) {
  char_matcher matcher = metachar_matchers[(unsigned char)m];

  return !!matcher(ascii_cp(g->cp)) == !!islower((unsigned char)m);
}

/* Match 'g' against the body of a bracket expression -- the text between
 * '[' and ']', as compile_charclass() stored it.  Members are whole
 * glyphs, so "[åä]" holds two of them rather than three shared bytes, and
 * a range's endpoints are glyphs compared by codepoint, which is what
 * Emacs does: "[à-é]" matches ç but not ê. */
static int matchcharclass(const re_glyph* g, const char* str, int icase) {
  unsigned c = fold_cp(g->cp, icase);

  while (*str != '\0') {
    re_glyph lo;

    if (str[0] == '[') {
      int named = matchnamedclass(ascii_cp(g->cp), &str, icase);
      if (named > 0)
        return 1;
      if (named < 0)
        continue;
    } else if (str[0] == '\\' && ismetachar(str[1])) {
      if (matchmetachar(g, str[1]))
        return 1;
      str += 2;
      continue;
    } else if (str[0] == '\\' && str[1] != '\0') {
      /* An escaped ordinary character stands for itself. */
      str++;
    }

    lo = glyph_at(str);
    str += lo.len;
    if (str[0] == '-' && str[1] != '\0') {
      re_glyph hi = glyph_at(str + 1);
      str += 1 + hi.len;
      if (c >= fold_cp(lo.cp, icase) && c <= fold_cp(hi.cp, icase))
        return 1;
    } else if (c == fold_cp(lo.cp, icase)) {
      return 1;
    }
  }

  DEBUG_P("%u did not match prev. ccl\n", g->cp);
  return 0;
}

static int matchone(const regex_t* p, const re_glyph* g) {
  DEBUG_P("ONE %d matches %u?\n", p->type, g->cp);
  int icase = (p->type & RE_TYPE_ICASE) != 0;
  int c = ascii_cp(g->cp);
  switch (p->type & ~RE_TYPE_ICASE) {
    case DOT:
      return matchdot(g->cp);
    case CHAR_CLASS:
      return matchcharclass(g, RE_CCL_STR(p), icase);
    case INV_CHAR_CLASS:
      return !matchcharclass(g, RE_CCL_STR(p), icase);
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
      return fold_cp(char_cp(p), icase) == fold_cp(g->cp, icase);
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
  const regex_t* p;
  const regex_t* stop;
  const char* iter;  /* CONT_REP: where this repetition began */
  unsigned min, max; /* CONT_REP: bounds; RE_REP_INF max is unbounded */
  unsigned done;     /* CONT_REP: repetitions completed */
} re_cont;

typedef struct {
  const char* text_start;  /* offset 0 for reported spans, and where '^'
                            * holds -- re_exec()'s start_offset says where
                            * to resume scanning, not where the line begins */
  const char* text_limit;  /* one past the last byte a match may CONSUME.
                            * Not the subject's end and not a stand-in for
                            * it: '$' still asks about the terminator, so a
                            * limit short of it excludes a match without
                            * making the anchor hold early. */
  const regex_t* prog_end; /* the UNUSED sentinel */
  re_match_result* out;
  int has_branch; /* whether the pattern contains '\|' at all */

  /* The budget for this attempt, and only this one.  These used to be
   * file-scope statics, so a second execution -- on another thread, or
   * started from a cancellation callback -- reset and then spent the
   * first one's allowance. */
  unsigned long steps, max_steps;
  unsigned long depth, max_depth;
  int exhausted; /* budget spent or cancelled: not an honest no-match */
  int (*cancel)(void*);
  void* cancel_data;
} re_ctx;

static const char* match_seq(const regex_t* p,
                             const regex_t* stop,
                             const char* text,
                             const re_cont* k,
                             re_ctx* ctx);
static const char* match_cont(const re_cont* k, const char* text, re_ctx* ctx);
static const char* match_rep(const re_cont* k, const char* text, re_ctx* ctx);
static const char* match_group_iter(const regex_t* g,
                                    const regex_t* gend,
                                    const char* text,
                                    unsigned done,
                                    unsigned min,
                                    unsigned max,
                                    const re_cont* k,
                                    re_ctx* ctx);

static unsigned short node_type(const regex_t* p) {
  return p->type & ~RE_TYPE_ICASE;
}

/* The GROUPEND closing the group headed by 'g'. Found by walking the
 * nesting rather than by trusting GROUP's own 8-bit group_size, which a
 * group of more than 255 nodes overflows. */
static const regex_t* group_end(const regex_t* g, const re_ctx* ctx) {
  const regex_t* p = next_node(g);
  int depth = 1;

  while (p < ctx->prog_end) {
    unsigned short type = node_type(p);
    if (type == GROUP)
      depth++;
    else if (type == GROUPEND && --depth == 0)
      break;
    p = next_node(p);
  }
  return p;
}

/* One past the atom starting at 'p', where a whole group is one atom. */
static const regex_t* atom_end(const regex_t* p,
                               const regex_t* stop,
                               const re_ctx* ctx) {
  const regex_t* end = node_type(p) == GROUP ? group_end(p, ctx) : p;

  if (end >= ctx->prog_end)
    return stop;
  end = next_node(end);
  return end < stop ? end : stop;
}

/* The first '\|' in [p, stop) that belongs to this level: alternation
 * separates whole concatenations, so a BRANCH inside a nested group is
 * that group's business, not ours. NULL when there is none. */
static const regex_t* find_branch(const regex_t* p,
                                  const regex_t* stop,
                                  const re_ctx* ctx) {
  while (p < stop) {
    if (node_type(p) == BRANCH)
      return p;
    p = atom_end(p, stop, ctx);
  }
  return NULL;
}

/* Repetition bounds of the quantifier node 'p'; RE_REP_INF max means
 * unbounded.  A finite max of 0 is a real bound ("a\{0\}" matches empty),
 * so the two cannot share a spelling. */
static void quant_bounds(const regex_t* p, unsigned* min, unsigned* max) {
  switch (node_type(p)) {
    case QUESTIONMARK:
      *min = 0, *max = 1;
      break;
    case STAR:
      *min = 0, *max = RE_REP_INF;
      break;
    case PLUS:
      *min = 1, *max = RE_REP_INF;
      break;
    case TIMES:
      *min = p->u.n, *max = p->u.n;
      break;
    case TIMES_N:
      *min = p->u.n, *max = RE_REP_INF;
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
static const char* match_alt(const regex_t* p,
                             const regex_t* br,
                             const regex_t* stop,
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
  end = match_seq(next_node(br), stop, text, k, ctx);
  if (end)
    return end;
  restore_spans(ctx->out, saved);
  return NULL;
}

/* A quantified single-node atom. Every quantifier is greedy, '?' included
 * (Emacs': "a?" on "a" matches [0,1), not the empty string): take as much
 * as the atom allows, then hand glyphs back one at a time until the rest
 * of the pattern fits.  The count is in glyphs, so "å\{2\}" wants two
 * characters, but 'pos' stays a byte pointer. */
static const char* match_atom(const regex_t* p,
                              const char* text,
                              unsigned min,
                              unsigned max,
                              const re_cont* k,
                              re_ctx* ctx) {
  re_span saved[RE_MAX_SPANS];
  const char* pos = text;
  unsigned n = 0;

  while (n < max && *pos != '\0') {
    re_glyph g = glyph_at(pos);
    /* The one place this engine consumes, so the one place the match
     * limit has to hold.  A character that starts under the limit and
     * ends past it is not half-consumed: it is not consumed. */
    if (pos + g.len > ctx->text_limit)
      break;
    if (!matchone(p, &g))
      break;
    pos += g.len;
    n++;
  }
  if (n < min)
    return NULL;

  save_spans(saved, ctx->out);
  for (;; n--) {
    const char* end = match_cont(k, pos, ctx);
    if (end)
      return end;
    restore_spans(ctx->out, saved);
    if (n == min)
      break;
    pos = glyph_prev(text, pos);
  }
  return NULL;
}

/* '^' and '$' consume nothing, so a quantifier on one only decides
 * whether the assertion has to hold at all. */
static const char* match_anchor(const regex_t* p,
                                const char* text,
                                unsigned min,
                                const re_cont* k,
                                re_ctx* ctx) {
  int holds = node_type(p) == BEGIN ? text == ctx->text_start : text[0] == '\0';

  if (!holds && min > 0)
    return NULL;
  return match_cont(k, text, ctx);
}

/* A group, quantified or not (an unquantified one is just min == max == 1).
 * Greedy: entering the group is tried before skipping it. */
static const char* match_group(const regex_t* g,
                               const char* text,
                               unsigned min,
                               unsigned max,
                               const re_cont* k,
                               re_ctx* ctx) {
  re_span saved[RE_MAX_SPANS];

  if (max > 0) {
    const char* end;
    save_spans(saved, ctx->out);
    end = match_group_iter(g, group_end(g, ctx), text, 0, min, max, k, ctx);
    if (end)
      return end;
    restore_spans(ctx->out, saved);
  }
  if (min == 0)
    return match_cont(k, text, ctx);
  return NULL;
}

/* Start repetition number 'done' + 1 of the group headed by 'g'. */
static const char* match_group_iter(const regex_t* g,
                                    const regex_t* gend,
                                    const char* text,
                                    unsigned done,
                                    unsigned min,
                                    unsigned max,
                                    const re_cont* k,
                                    re_ctx* ctx) {
  re_span* span = group_span(ctx, g);
  re_cont rep;

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
  return match_seq(next_node(g), gend, text, &rep, ctx);
}

/* One repetition of a group finished at 'text': close its capture, then
 * prefer repeating the group over leaving it.
 *
 * A repetition that consumed nothing normally ends the loop -- repeating it
 * could only match empty again, forever.  Emacs stops one repetition later
 * than that, and the difference is visible in the capture register: it
 * compiles the mandatory head of a repeat as a counted loop with no
 * empty-match check at all, so "\(x*\|a\)\{2\}b" against "ab" spends
 * repetition 1 on the empty branch at 0 and repetition 2 on "a", leaving
 * group 1 as [0,1).  Only from repetition 'min' + 1 -- the first one the
 * loop is free to skip -- does an empty body stop it, which is where a
 * plain '*' (min 0) has been all along.  Reproducing that is what keeps
 * "\1" after a "\{n\}" or "\{n,m\}" agreeing with Emacs; the whole-match
 * span is the same either way.  See utils/regex_differential.py. */
static const char* match_rep(const re_cont* k, const char* text, re_ctx* ctx) {
  re_span* span = group_span(ctx, k->p);
  unsigned done = k->done + 1;
  int grew = text != k->iter;

  if (span)
    span->end = (int)(text - ctx->text_start);

  if ((grew || done <= k->min) && done < k->max) {
    /* Out of repetitions for this group.  A repetition that consumed
     * leaves only one answer this engine could still give -- a match
     * *shorter* than the pattern asks for, "\(a\)*" over 300 a's
     * stopping at 256 -- so report the ceiling rather than that answer.
     * An empty body has nothing left to consume and can still satisfy
     * whatever is left of 'min', which is the fallback below and what
     * makes "\(a*\)\{300\}" match the empty string, as Emacs does. */
    if (done >= MAX_GROUP_REPEATS) {
      if (grew) {
        ctx->exhausted = 1;
        return NULL;
      }
    } else {
      re_span saved[RE_MAX_SPANS];
      const char* end;
      save_spans(saved, ctx->out);
      end = match_group_iter(k->p, k->stop, text, done, k->min, k->max, k->next,
                             ctx);
      if (end)
        return end;
      restore_spans(ctx->out, saved);
    }
  }
  /* Leaving early on an empty body is the fallback, not the first choice:
   * it only matters once repeating is out of reach -- a 'min' past
   * MAX_GROUP_REPEATS, say -- where an empty body has to satisfy whatever
   * is left of 'min' or nothing can. */
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
static const char* match_seq_body(const regex_t* p,
                                  const regex_t* stop,
                                  const char* text,
                                  const re_cont* k,
                                  re_ctx* ctx) {
  unsigned short type;
  unsigned min = 1, max = 1;
  const regex_t* after;
  re_cont rest;

  if (p >= stop || node_type(p) == UNUSED)
    return match_cont(k, text, ctx);

  if (ctx->has_branch) {
    const regex_t* br = find_branch(p, stop, ctx);
    if (br)
      return match_alt(p, br, stop, text, k, ctx);
  }

  after = atom_end(p, stop, ctx);
  if (after < stop && isquantifier(node_type(after))) {
    quant_bounds(after, &min, &max);
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
  return match_atom(p, text, min, max, &rest, ctx);
}

/* Charge one unit of work and one frame of depth to this attempt's budget.
 * Returns 0 -- and latches ctx->exhausted, so the whole attempt unwinds at
 * once rather than reporting an honest no-match -- when the budget is spent
 * or the caller's cancel callback says to stop. */
static int budget_enter(re_ctx* ctx) {
  if (ctx->exhausted)
    return 0;
  if (++ctx->steps > ctx->max_steps || ctx->depth >= ctx->max_depth ||
      (ctx->cancel && (ctx->steps & (RE_CANCEL_POLL_STEPS - 1)) == 1 &&
       ctx->cancel(ctx->cancel_data))) {
    ctx->exhausted = 1;
    return 0;
  }
  ctx->depth++;
  return 1;
}

static const char* match_seq(const regex_t* p,
                             const regex_t* stop,
                             const char* text,
                             const re_cont* k,
                             re_ctx* ctx) {
  const char* end;

  if (!budget_enter(ctx))
    return NULL;
  end = match_seq_body(p, stop, text, k, ctx);
  ctx->depth--;
  return end;
}

static void init_ctx(re_ctx* ctx,
                     re_t pattern,
                     const char* text_start,
                     const char* text_limit,
                     const re_exec_options* options,
                     re_match_result* out) {
  const regex_t* p = pattern;

  ctx->text_start = text_start;
  ctx->text_limit = text_limit;
  ctx->out = out;
  ctx->has_branch = 0;
  ctx->steps = 0;
  ctx->depth = 0;
  ctx->exhausted = 0;
  ctx->max_steps = MAX_MATCH_STEPS;
  ctx->max_depth = MAX_MATCH_DEPTH;
  ctx->cancel = NULL;
  ctx->cancel_data = NULL;
  if (options) {
    if (options->max_steps)
      ctx->max_steps = options->max_steps;
    if (options->max_depth)
      ctx->max_depth = options->max_depth;
    ctx->cancel = options->cancel;
    ctx->cancel_data = options->cancel_data;
  }
  while (node_type(p) != UNUSED) {
    if (node_type(p) == BRANCH)
      ctx->has_branch = 1;
    p = next_node(p);
  }
  ctx->prog_end = p;
}

static int re_matchp_internal(re_t pattern,
                              const char* text,
                              int* matchlength,
                              const char* text_start,
                              const char* text_limit,
                              const re_exec_options* options,
                              re_match_result* out,
                              int* exhausted) {
  re_ctx ctx;
  int anchored;
  int idx = 0;

  *matchlength = 0;
  if (!pattern)
    return -1;

  init_ctx(&ctx, pattern, text_start, text_limit, options, out);
  /* A leading '^' can only hold at the start of the subject, so no offset
   * past the first one is worth trying -- and when the scan resumes past
   * it, not even that one can match.  Unless a '\|' means the anchor
   * governs the first alternative alone. */
  anchored = node_type(pattern) == BEGIN && !ctx.has_branch;

  /* The scan advances a glyph at a time, so a match can only start on a
   * character boundary and an empty match is still tried at the end. */
  for (;;) {
    const char* end;
    reset_spans(out);
    end = match_seq(pattern, ctx.prog_end, text + idx, NULL, &ctx);
    if (end) {
      *matchlength = (int)(end - (text + idx));
      return idx;
    }
    if (anchored || ctx.exhausted || text[idx] == '\0')
      break;
    idx += (int)glyph_at(text + idx).len;
    /* A start past the limit has no match to report: even the empty one
     * would end past it.  The start exactly AT the limit was tried above,
     * which is what makes an empty match there an ordinary result. */
    if (text + idx > ctx.text_limit)
      break;
  }

  *exhausted = ctx.exhausted;
  return -1;
}

#ifdef CPROVER
#define N 24

/* Formal verification with cbmc: `make verify`, which runs one proof per
 * harness below.  This block had rotted -- it called a re_match() whose
 * first parameter became a pattern string, read a union member (u.ccl)
 * that no longer exists, and used a bare assume() that is not a CBMC
 * builtin -- so `make verify` had not compiled, let alone verified,
 * for as long as the node layout has looked like this.  `make
 * verify-syntax` is what keeps that from happening again: it type-checks
 * this block with an ordinary compiler on every `make check`. */
#ifdef __CPROVER
#define ASSUME(x) __CPROVER_assume(x)
#else
/* Not building under cbmc: keep the harness compilable so its rot is
 * caught by a compiler rather than by whoever next installs cbmc. */
#define ASSUME(x) ((void)(x))
#endif

void verify_re_compile(void);
void verify_re_match(void);

void verify_re_compile(void) {
  /* test input - N chars used as a regex-pattern input */
  char arr[N];
  /* the array is uninitialized, which is what makes it symbolic: cbmc
   * searches every value it could hold.  The original per-byte range
   * assumption is gone: on an 8-bit char it excluded nothing, and it
   * made every compiler that is not cbmc warn about a tautology. */
  ASSUME(arr[sizeof(arr) - 1] == 0); /* proper NUL termination */
  /* verify abscence of run-time errors - go! */
  re_compile(arr);
}

void verify_re_match(void) {
  int length;
  regex_t pattern[MAX_REGEXP_OBJECTS];
  char arr[N];

  for (unsigned char i = 0; i < MAX_REGEXP_OBJECTS; i++) {
    ASSUME(pattern[i].type <= TIMES_NM);
    ASSUME(pattern[i].u.n <= 0xFFFF);
  }
  ASSUME(arr[sizeof(arr) - 1] == 0); /* proper NUL termination */

  /* re_match() takes a pattern string; the compiled-program entry point
   * is re_matchp(), which is what this harness has always meant. */
  re_matchp(pattern, arr, &length);
}
#endif
