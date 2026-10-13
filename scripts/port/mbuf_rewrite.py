#!/usr/bin/env python3
"""Rewrite OpenBSD mbuf field accesses in qwz into macOS mbuf KPI calls.

macOS mbufs are opaque, so `m->m_len` and friends become accessor macros that
qwz_compat.h defines. Run once on the upstream files; the result is committed.
Usage: mbuf_rewrite.py file...
"""
import re
import sys

# An mbuf expression: identifier chains like m, xfer->m, msdu->m, rx_data->m.
E = r"([A-Za-z_][A-Za-z0-9_]*(?:(?:->|\.)[A-Za-z_][A-Za-z0-9_]*)*)"

RULES = [
    # a->m_len = a->m_pkthdr.len = expr;  (expr may span lines)
    (re.compile(E + r"->m_len\s*=\s*\1->m_pkthdr\.len\s*=\s*([^;]+);", re.S),
     r"M_SETLEN(\1, \2);"),
    (re.compile(E + r"->m_pkthdr\.ph_cookie"),
     r"((struct ieee80211_node *)mbuf_pkthdr_rcvif(\1))"),
    (re.compile(E + r"->m_pkthdr\.len"), r"M_PKTLEN(\1)"),
    (re.compile(E + r"->m_len"), r"M_LEN(\1)"),
    (re.compile(E + r"->m_flags"), r"M_FLAGS(\1)"),
    (re.compile(E + r"->m_next"), r"mbuf_next(\1)"),
]

for fn in sys.argv[1:]:
    s = open(fn).read()
    n = 0
    for rx, rep in RULES:
        s, k = rx.subn(rep, s)
        n += k
    open(fn, "w").write(s)
    print(f"{fn}: {n} rewrites")
