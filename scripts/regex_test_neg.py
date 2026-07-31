#!/usr/bin/env python3

"""Random negative testing against Python's own matcher.

For every pattern in the given file this generates random subjects that
Python's `re` does *not* accept and asserts that tests/test_rand_neg
rejects them too.

Like the positive driver, this reads a Python-compatible subset
(tests/pynok.lst), not tests/nok.lst: several rows there are patterns
this engine refuses and Python happily reads as literals -- "\\(a", "a++"
and "a\\{2\\}\\{3\\}" among them -- so a subject "Python does not match"
says nothing about what the C engine should do with it. tests/nok.lst is
checked in full by tests/test1.c.

The same three defects fixed in the positive driver were here: the walk
stopped at the first repeated pattern (nok.lst:15), the repeat budget was
one counter for the whole file, and both arguments were passed to the C
program wrapped in literal double quotes. It also always exited 0.
"""

import random
import re
import string
import subprocess
import sys

PROG = "./tests/test_rand_neg"


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


def gen_no_match(pattern, rng, minlen=1, maxlen=50, maxattempts=500):
	compiled = re.compile(pattern)
	for _ in range(maxattempts):
		candidate = "".join(rng.choice(string.printable)
				    for _ in range(rng.randint(minlen, maxlen)))
		if not compiled.search(candidate):
			return candidate
	return None


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

	per_pattern = max(1, total // len(patterns))
	rng = random.Random(20260731)
	checks = 0
	failures = 0

	print(f"Testing {len(patterns)} patterns from {path} against "
	      f"{per_pattern} random non-matching subject(s) each:")
	for pattern in patterns:
		try:
			compiled_ok = re.compile(pattern) is not None
		except re.error as exc:
			print(f"FAIL: {pattern!r} is not a Python pattern: {exc}",
			      file=sys.stderr)
			return 1
		assert compiled_ok
		for _ in range(per_pattern):
			example = gen_no_match(pattern, rng)
			if example is None:
				print(f"  SKIP  {pattern!r}: no non-matching "
				      "subject found in 500 attempts")
				break
			checks += 1
			proc = subprocess.run([PROG, pattern, example])
			if proc.returncode != 0:
				octets = ", ".join("0x%02x" % b for b in
						   example.encode("utf-8"))
				print(f"  FAIL  {pattern!r} matched "
				      f"{example!r} [{octets}]")
				failures += 1

	print(f" {checks - failures}/{checks} subjects rejected as expected.\n")
	if failures:
		print(f"FAIL: {failures} subject(s) accepted by {PROG}",
		      file=sys.stderr)
		return 1
	if checks == 0:
		print("FAIL: no subject was generated at all", file=sys.stderr)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv))
