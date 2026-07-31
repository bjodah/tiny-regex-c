#!/usr/bin/env python3

"""Random positive testing against Python's own matcher.

For every pattern in the given file this generates random subjects that
Python's `re` accepts (via the vendored exrex) and asserts that
tests/test_rand accepts them too.

The list this reads has to be a *Python-compatible* subset. tests/ok.lst
is the Emacs dialect this engine implements, where "\\(", "\\|" and "\\{"
are operators that Python reads as literals; those rows are checked by
tests/test1.c, which runs every row of ok.lst and nok.lst. This driver
reads tests/pyok.lst: the rows on which both dialects agree, machine
selected by comparing Python's leftmost match length with the length
recorded in ok.lst.

History, because it explains why the row count moves so much here: this
script used to stop at the first pattern equal to the one above it
(ok.lst:17), and its repeat budget was one counter for the whole file
rather than per pattern, so the first pattern consumed all of it and
every later pattern ran zero subjects. It also wrapped both arguments in
literal double quotes before handing them to the C program, so what got
matched was "\\d" against "5", quotes included -- and it exited 0 no
matter how many subjects failed.
"""

import subprocess
import sys

import exrex

PROG = "./tests/test_rand"


def unquote(text):
	"""The list files spell a newline as the two characters "\\n".

	tests/test1.c does this same conversion before handing a row to the
	engine (cunquote()), and Python reads "\\n" in a pattern as a newline
	too, so both sides have to see the converted form or they are not
	being asked the same question.
	"""
	return (text.replace("\\n", "\n").replace("\\t", "\t")
		.replace("\\r", "\r"))


def read_patterns(path):
	patterns = []
	with open(path, "rt", encoding="utf-8") as handle:
		for line in handle:
			if not line.strip() or line.startswith("#"):
				continue
			patterns.append(unquote(line.split("\t")[0]))
	return patterns


def main(argv):
	if len(argv) < 2:
		print(f"\nusage: {argv[0]} pattern-file [total-subjects]\n")
		return 2

	path = argv[1]
	total = int(argv[2]) if len(argv) > 2 else 10
	patterns = read_patterns(path)
	if not patterns:
		print(f"FAIL: no patterns in {path}", file=sys.stderr)
		return 1

	# The budget is spread over the patterns instead of being spent on
	# the first one, so the file costs what the Makefile asked for.
	per_pattern = max(1, total // len(patterns))
	checks = 0
	failures = 0

	print(f"Testing {len(patterns)} patterns from {path} against "
	      f"{per_pattern} random matching subject(s) each:")
	for pattern in patterns:
		for _ in range(per_pattern):
			try:
				example = exrex.getone(pattern)
			except Exception as exc:  # exrex cannot build this one
				print(f"  SKIP  {pattern!r}: exrex: {exc}")
				break
			checks += 1
			proc = subprocess.run([PROG, pattern, example])
			if proc.returncode != 0:
				octets = ", ".join("0x%02x" % b for b in
						   example.encode("utf-8"))
				print(f"  FAIL  {pattern!r} did not match "
				      f"{example!r} [{octets}]")
				failures += 1

	print(f" {checks - failures}/{checks} subjects matched as expected.\n")
	if failures:
		print(f"FAIL: {failures} subject(s) rejected by {PROG}",
		      file=sys.stderr)
		return 1
	if checks == 0:
		print("FAIL: no subject was generated at all", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
