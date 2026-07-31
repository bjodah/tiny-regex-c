Falco Girgis's Changelist:
- merged several bugfixes that were just sitting as pending PRs
- merged several features that were just sitting as pending PRs
- added CMake support
- exposed configuration definitions as CMake options
- modified internal symbol representation to be far more compact
  - everything is byte-packed as tightly as humanly possible
  - character class substrings are now stored within the symbol 
  - far more cache coherent
- added support for user-supplied compiled pattern storage
  - allows you to heap allocate and maintain more than one compiled pattern
- Fixed multi matches `{n}`, `{,m}`, `{n,}`, `{n, m}`, which were only partially working previously
- Added stringifier to (sort of) go back to string form from a compiled regexp
- Broke print() function
- Broke recursive pattern matching

#
![CI](https://github.com/kokke/tiny-regex-c/workflows/CI/badge.svg)
# tiny-regex-c
# A small regex implementation in C
### Description
Small and portable [Regular Expression](https://en.wikipedia.org/wiki/Regular_expression) (regex) library written in C. 

Design is inspired by Rob Pike's regex-code for the book *"Beautiful Code"* [available online here](http://www.cs.princeton.edu/courses/archive/spr09/cos333/beautiful.html).

Supports a subset of the syntax and semantics of the Python standard library implementation (the `re`-module).

**I will gladly accept patches correcting bugs.**

### Design goals
The main design goal of this library is to be small, correct, self contained and use few resources while retaining acceptable performance and feature completeness. Clarity of the code is also highly valued.

### Notable features and omissions
- Small code and binary size: 500 SLOC, ~3kb binary for x86. Statically #define'd memory usage / allocation.
  - NOTE: support added for user-specified storage -- Falco
- No use of dynamic memory allocation (i.e. no calls to `malloc` / `free`).
- To avoid call-stack exhaustion, iterative searching is preferred over recursive by default (can be changed with a pre-processor flag).
- No support for capturing groups or named capture: `(^P<name>group)` etc.
- Thorough testing : [exrex](https://github.com/asciimoo/exrex) is used to randomly generate test-cases from regex patterns, which are fed into the regex code for verification. Try `make test` to generate a few thousand tests cases yourself. 
- Verification-harness for [KLEE Symbolic Execution Engine](https://klee.github.io), see [formal verification.md](https://github.com/kokke/tiny-regex-c/blob/master/formal_verification.md).
- Provides character length of matches.
- Compiled for x86 using GCC 7.2.0 and optimizing for size, the binary takes up ~2-3kb code space and allocates ~0.5kb RAM :
  ```
  > gcc -Os -c re.c
  > size re.o
      text     data     bss     dec     hex filename
      2404        0     304    2708     a94 re.o
      
  ```



### API
This is the public / exported API:
```C
/* Typedef'd pointer to hide implementation details. */
typedef struct regex_t* re_t;

/* Compiles regex string pattern to a regex_t-array. */
re_t re_compile(const char* pattern);

/* Finds matches of the compiled pattern inside text. */
int  re_matchp(re_t pattern, const char* text, int* matchlength);

/* Finds matches of pattern inside text (compiles first automatically). */
int  re_match(const char* pattern, const char* text, int* matchlength);
```

### Supported regex-operators
The following features / regex-operators are supported by this library.


  -  `.`         Dot, matches any character except newline
  -  `^`         Start anchor, matches beginning of string
  -  `$`         End anchor, matches end of string
  -  `*`         Asterisk, match zero or more (greedy)
  -  `+`         Plus, match one or more (greedy)
  -  `?`         Question, match zero or one (greedy)
  -  `[abc]`     Character class, match if one of {'a', 'b', 'c'}
  -  `[^abc]`   Inverted class, match if NOT one of {'a', 'b', 'c'}
  -  `[a-zA-Z]` Character ranges, the character set of the ranges { a-z | A-Z }
  -  `\s`       Whitespace, '\t' '\f' '\r' '\n' '\v' and spaces
  -  `\S`       Non-whitespace
  -  `\w`       Alphanumeric, [a-zA-Z0-9_]
  -  `\W`       Non-alphanumeric
  -  `\d`       Digits, [0-9]
  -  `\D`       Non-digits
  -  `\xXX`     Hex-encoded byte. Without two hex digits it is not an
     escape at all: `\xZ` is the three literal characters it is spelled
     with, `\x4X` the four, and a trailing `\x` the two
  -  `[[:digit:]]` POSIX bracket classes inside character classes: alnum,
     alpha, ascii, blank, cntrl, digit, graph, lower, nonascii, print, punct,
     space, upper, word, xdigit
  -  `\|`       Branch Or, e.g. `a\|A`, `ab\|cd`
  -  `\{n\}`    Exact quantifier
  -  `\{n,\}`   Match n or more times
  -  `\{,m\}`   Match m or less times
  -  `\{n,m\}`  Match n to m times
  -  `\(...\)`  Group

Bare `(`, `)`, `|`, `{`, and `}` are literal characters; grouping,
alternation, and intervals use the escaped Emacs-style forms above.

### Characters, not bytes
The matcher steps by UTF-8 character. `.` consumes a whole character, a
multi-byte literal in the pattern is a single atom (`å*` repeats `å`, not
its last byte), quantifiers and intervals count characters, and a bracket
expression holds characters -- `[åä]` matches either of them and not the
`0xC3` lead byte they share. A range compares codepoints, so `[à-é]`
matches `ç` but not `ê`, as in Emacs.

Reported spans, and `re_exec()`'s `start_offset`, stay **byte** offsets
into the subject.

Case folding and the ASCII classes (`\s`, `\S`, `\w`, `\W`, `\d`, `\D`
and every POSIX class except `[:ascii:]` and `[:nonascii:]`) remain
ASCII-only: a multi-byte character is in none of them, and in all of
their complements as one whole character. `[:ascii:]` and `[:nonascii:]`
ask whether the character is ASCII at all.

Input that is not well-formed UTF-8 is not an error: every stray byte is
a character of its own, which `.` consumes singly and which no real
character ever equals. `\xXX` above therefore still names a byte -- a
non-ASCII one matches only where it stands alone.

`\|` has the lowest precedence, as in Emacs: its alternatives are whole
concatenations (`ab\|cd` is `ab` or `cd`, not `a` followed by `b\|cd`),
bounded by the enclosing `\(...\)` if there is one. Quantified groups and
intervals backtrack, so `\(.*\),\(.*\)` and `.\{2,3\}c` behave as they do
in Emacs.  Every quantifier is greedy, `?` included: `a?` on `a` matches
one character, as it does in Emacs.

Constructs this library cannot honour are rejected rather than
reinterpreted.  An interval whose contents are not `n`, `n,`, `,m` or
`n,m` (counts up to 65535, `m` not below `n`), an unterminated `\{`, and
an unknown POSIX class name are all compile errors; they never fall back
to matching the literal characters they are spelled with.  Where there is
nothing to repeat -- the start of the pattern, of a group or of an
alternative -- `\{` is the literal `{`, as in Emacs.

`^` matches at the start of the subject handed to `re_exec()`, not at its
`start_offset`: that argument says where to resume scanning, so a
`^`-anchored pattern cannot match at a non-zero offset.

### Caller-supplied storage
`re_compile_checked()` and `re_compile_to()` compile into a buffer the
caller owns. A compiled program is not an opaque byte string -- the engine
reads its multi-byte fields in place -- so that buffer must be aligned to
`RE_STORAGE_ALIGNMENT`:

```C
alignas(RE_STORAGE_ALIGNMENT) unsigned char storage[RE_MAX_COMPILED_BYTES];
```

Anything `malloc()` returns is aligned enough; a bare `unsigned char`
array or an interior pointer into one need not be. Storage that is not
aligned is **refused** rather than written to and read back through:
`re_compile_checked()` returns `RE_STATUS_BAD_PATTERN` and
`re_compile_to()` returns `NULL`.

Passing `NULL` storage with `*storage_size == 0` to
`re_compile_checked()` is the supported way to ask how many bytes the
pattern needs; it reports `RE_STATUS_BUFFER_TOO_SMALL` and writes the
required size, and compiling into exactly that many bytes then works.

A buffer too small for the whole pattern is a failure, never a silently
compiled prefix. `re_compile_to()` writes straight into the caller's
buffer and wants one node's slack over the size it reports; callers that
must fit exactly should go through `re_compile_checked()`.

### Usage
Compile a regex from ASCII-string (char-array) to a custom pattern structure using `re_compile()`.

Search a text-string for a regex and get an index into the string, using `re_match()` or `re_matchp()`.

The returned index points to the first place in the string, where the regex pattern matches.

The integer pointer passed will hold the length of the match.

If the regular expression doesn't match, the matching function returns an index of -1 to indicate failure.

### Examples
Example of usage:
```C
/* Standard int to hold length of match */
int match_length;

/* Standard null-terminated C-string to search: */
const char* string_to_search = "ahem.. 'hello world !' ..";

/* Compile a simple regular expression using character classes, meta-char and greedy quantifiers: */
re_t pattern = re_compile("[Hh]ello [Ww]orld\\s*[!]?");

/* Check if the regex matches the text: */
int match_idx = re_matchp(pattern, string_to_search, &match_length);
if (match_idx != -1)
{
  printf("match at idx %i, %i chars long.\n", match_idx, match_length);
}
```

For more usage examples I encourage you to look at the code in the `tests`-folder.

### TODO
- Add `example.c` that demonstrates usage.
- Add `tests/test_perf.c` for performance and time measurements.
- Word boundary: \b \B
- Non-greedy, lazy quantifiers (??, +?, *?, {n,m}?)
- Backreferences in the matcher.

### FAQ
- *Q: What differentiates this library from other C regex implementations?*

  A: Well, the small size for one. 500 lines of C-code compiling to 2-3kb ROM, using very little RAM.

### License
All material in this repository is in the public domain.
