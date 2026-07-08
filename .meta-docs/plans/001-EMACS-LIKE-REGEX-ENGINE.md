## Plan: Emacs-like regex dialect and capture-span API

### Goal

Evolve `tiny-regex-c` into the shared regex engine for both:

* `fe` Lisp regex functions;
* `kg` editor regex search and replacement.

The engine should provide an Emacs-like regex dialect directly. Callers should not need to translate patterns before compilation.

---

## Current situation

The current public API exposes:

```c
re_t re_compile_to(const char *pattern, unsigned char *re_data, unsigned *bytes);
re_t re_compile(const char *pattern);
int re_matchp(re_t pattern, const char *text, int *matchlength);
int re_match(const char *pattern, const char *text, int *matchlength);
```

Problems to address:

1. `re_compile()` uses internal static storage, which is unsuitable for multiple compiled patterns or editor/language objects.
2. Match results currently expose only start offset and match length.
3. No capture-span API exists yet.
4. Compile failure, no-match, and complexity-limit failure need clearer status reporting.
5. The dialect is not yet Emacs-like enough for kg commands named after Emacs regexp commands.

---

## Phase 1: Add capture-ready result API

### Objectives

Add a new API that can represent:

* compile success/failure;
* match/no-match;
* whole-match span;
* future capture spans;
* complexity-limit failures.

Do this before integrating many callers.

### Proposed types

Names are provisional.

```c
#define RE_MAX_CAPTURES 16

typedef enum {
    RE_STATUS_OK = 0,
    RE_STATUS_NO_MATCH,
    RE_STATUS_BAD_PATTERN,
    RE_STATUS_TOO_COMPLEX,
    RE_STATUS_BUFFER_TOO_SMALL
} re_status;

typedef struct {
    int start;
    int end;
} re_span;

typedef struct {
    re_status status;
    unsigned nspans;
    re_span spans[RE_MAX_CAPTURES + 1]; /* span 0 = whole match */
} re_match_result;
```

### Proposed API

```c
re_status re_compile_checked(
    const char *pattern,
    unsigned char *storage,
    unsigned *storage_size,
    re_t *out);

re_status re_exec(
    re_t regex,
    const char *text,
    int start_offset,
    re_match_result *out);
```

Rules:

* `RE_STATUS_OK` means `out->spans[0]` is valid.
* `RE_STATUS_NO_MATCH` means no match was found.
* `RE_STATUS_BAD_PATTERN` means the pattern is invalid.
* `RE_STATUS_TOO_COMPLEX` means the match budget was exceeded.
* `RE_STATUS_BUFFER_TOO_SMALL` means caller-provided compiled-regex storage was insufficient.

Initially, `nspans` may be `1`, with only the whole match populated.

### Compatibility

Keep existing API as wrappers during migration:

```c
re_t re_compile(const char *pattern);
int re_matchp(re_t pattern, const char *text, int *matchlength);
int re_match(const char *pattern, const char *text, int *matchlength);
```

But new integrations should use the checked API.

---

## Phase 2: Define zero-length match behavior

Zero-length matches are valid, but all iterators must avoid rediscovering the same match forever.

Engine-level rule:

* `re_exec` reports the match it finds at or after `start_offset`.
* It is caller responsibility to advance after zero-length matches when iterating.

Helper iterator rule, if added:

* forward iterator advances by one byte after a zero-length match;
* backward iterator only considers strictly earlier candidate positions after a zero-length match.

Tests:

* pattern matching empty string;
* `a*` on `bbb`;
* `^` on non-empty text;
* `$` on non-empty text;
* repeated search over a row.

---

## Phase 3: Emacs-like dialect MVP

### Objective

Make the regex dialect more Emacs-like inside the engine.

No kg-side or fe-side dialect adapters.

### MVP syntax

Support and document:

* `.` matches any byte except newline.
* `^` matches beginning of string/row.
* `$` matches end of string/row.
* Character classes:

  * `[abc]`
  * `[^abc]`
  * `[a-z]`
  * escaped `]`, `-`, and `\` behavior explicitly tested.
* Repetition:

  * `*`
  * `+`
  * `?`
* Emacs-like grouping:

  * `\(...\)`
* Emacs-like alternation:

  * `\|`
* Emacs-like intervals:

  * `\{n\}`
  * `\{n,\}`
  * `\{n,m\}`

Optional MVP, if cheap:

* POSIX bracket classes:

  * `[[:digit:]]`
  * `[[:alpha:]]`
  * `[[:alnum:]]`
  * `[[:space:]]`
  * `[[:lower:]]`
  * `[[:upper:]]`

### Compatibility choices

Because this is intentionally moving toward Emacs-like syntax:

* bare `(...)` should become literal parentheses or an error; do not treat bare parentheses as grouping in the final dialect;
* bare `|` should become literal `|` or an error; do not treat bare `|` as alternation in the final dialect;
* bare `{...}` should become literal braces or an error; use `\{...\}` for intervals.

Prefer literal interpretation where Emacs would treat the character literally. Reject only when continuing would hide likely bugs.

---

## Phase 4: Case folding

Add match flags rather than caller-side pattern rewriting.

Proposed API addition:

```c
typedef enum {
    RE_FLAG_NONE = 0,
    RE_FLAG_ICASE = 1u << 0
} re_flags;
```

Use flags in compile or exec, but prefer compile-time flags if they affect compiled representation.

kg will use this for smart-case search.

---

## Phase 5: Capture spans

### Objective

Record capture spans for `\(...\)` groups.

### Rules

* span 0 is the whole match;
* span N is the Nth capture group by opening group order;
* unmatched optional captures get `start = -1`, `end = -1`;
* repeated captures use “last successful capture wins”;
* exceeding `RE_MAX_CAPTURES` is a compile error unless a dynamically sized result API is introduced.

### Tests

Add tests for:

* one group;
* nested groups;
* alternation with captures;
* optional unmatched capture;
* repeated capture;
* exceeding capture limit;
* captures with zero-length submatches.

---

## Phase 6: Complexity and safety

The engine already has match-step limiting. Keep this principle and expose budget failure as `RE_STATUS_TOO_COMPLEX`.

Add tests for:

* pathological nested repetition;
* large but valid input;
* invalid pattern does not read past end;
* too-small compile buffer;
* zero-length matching at end of string.

---

## Phase 7: Documentation

Document:

* supported syntax;
* differences from full Emacs regex;
* byte-oriented indexing;
* capture numbering;
* zero-length behavior;
* error statuses;
* compile-storage requirements.

---

## Non-goals

* Full Emacs regex compatibility in one step.
* Unicode-aware matching.
* Syntax-table-aware Emacs constructs.
* Backreferences in the matcher.
* Multi-line matching.
* Replacement expansion.
