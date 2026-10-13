#!/usr/bin/env python3
"""Insert the casts C++ requires where qwz relies on C's implicit conversions.

qwz is C; itl80211's net80211 headers are C++, so qwz is compiled as C++.
This reads clang diagnostics (with -fdiagnostics-print-source-range-info) and
rewrites, in place:
  - implicit void * -> T * and integer -> enum conversions: wrap the
    expression in (T)(...);
  - arithmetic on void *: cast the left operand to (uint8_t *).
Anything it cannot handle is printed for manual fixing.
Usage: cxx_casts.py <diagnostics file> <source file>
"""
import re
import sys
from collections import defaultdict

diag_file, src_file = sys.argv[1], sys.argv[2]
lines = open(src_file).read().split("\n")

CONV = re.compile(
    r"^(?P<f>[^:]+):(?P<l>\d+):(?P<c>\d+):\{(?P<l1>\d+):(?P<c1>\d+)-(?P<l2>\d+):(?P<c2>\d+)\}.*error: "
    r"(?:cannot initialize a (?:variable|parameter|member subobject|return object) of type"
    r"|assigning to) '(?P<t>[^']+)'.* (?:from incompatible type|with an [lr]value of type) '(?P<from>[^']+)'")
VOIDARITH = re.compile(r"^(?P<f>[^:]+):(?P<l>\d+):(?P<c>\d+):.*error: arithmetic on a pointer to void")
OPERAND = re.compile(r"(\(\s*[^()]*\)|[A-Za-z_]\w*(?:(?:->|\.)[A-Za-z_]\w*|\[[^\]]*\])*)\s*$")

edits = defaultdict(list)   # line -> [(col_start, col_end, prefix, suffix)] (0-based, end exclusive)
manual = []
seen = set()
for d in open(diag_file):
    m = CONV.match(d)
    if m and m["f"] == src_file:
        t = m["t"]
        if "unnamed" in t or "typeof" in t:
            manual.append(d.strip()); continue
        key = (int(m["l1"]), int(m["c1"]))
        if key in seen:
            continue
        seen.add(key)
        l1, c1, l2, c2 = int(m["l1"]), int(m["c1"]) - 1, int(m["l2"]), int(m["c2"]) - 1
        if l1 == l2:
            edits[l1].append((c1, c2, f"({t})(", ")"))
        else:   # wrap across lines: prefix on the first, suffix on the last
            edits[l1].append((c1, c1, f"({t})(", ""))
            edits[l2].append((c2, c2, "", ")"))
        continue
    m = VOIDARITH.match(d)
    if m and m["f"] == src_file:
        ln, col = int(m["l"]), int(m["c"]) - 1
        left = lines[ln - 1][:col]
        om = OPERAND.search(left)
        if not om:
            manual.append(d.strip()); continue
        key = (ln, om.start(1))
        if key in seen:
            continue
        seen.add(key)
        edits[ln].append((om.start(1), om.end(1), "((uint8_t *)", ")"))

n = 0
for ln, es in edits.items():
    s = lines[ln - 1]
    es.sort(key=lambda e: e[0], reverse=True)
    last = None
    for a, b, pre, suf in es:
        if last is not None and b > last and a != b:   # overlapping: leave for the next pass
            continue
        s = s[:a] + pre + s[a:b] + suf + s[b:]
        last = a
        n += 1
    lines[ln - 1] = s
open(src_file, "w").write("\n".join(lines))
print(f"{src_file}: {n} casts inserted, {len(manual)} left for manual fixing")
for d in manual:
    print("  MANUAL", d[:200])
