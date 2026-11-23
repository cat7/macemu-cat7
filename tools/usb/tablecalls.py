#!/usr/bin/env python3
"""tablecalls.py FRAGMENT.pef GLUE_ADDR: find indirect calls
`lwz r12,OFF(rX)` ... `bl GLUE` in a PEF fragment's code and print, per call,
the table offset, the enclosing function (prologue scan back) and the log
strings referenced near it (TOC-relative string bases)."""
import sys, struct, os, re, collections
ns = {}
src = open(os.path.join(os.path.dirname(__file__), 'uimtable.py')).read().split('\ndef main')[0]
exec(src, ns)
secs, exps, rel, imps = ns['load'](open(sys.argv[1], 'rb').read())
code, data = secs[0]['body'], secs[1]['body']
drel = rel.get(1, {})
glue = int(sys.argv[2], 0)

# TOC: second word of any TVector export
toc = None
for name, (sec, val) in exps.items():
    if sec == 1 and drel.get(val + 4) == ('sect', 1):
        toc = int.from_bytes(data[val + 4:val + 8], 'big'); break

def word(o): return int.from_bytes(code[o:o + 4], 'big')
def sx(v): return v - 0x10000 if v & 0x8000 else v

def cstr(off):
    if not (0 <= off < len(data)): return None
    e = data.find(b'\0', off)
    s = data[off:e if e >= 0 else off + 80][:90]
    if len(s) >= 4 and all(32 <= c < 127 for c in s):
        return s.decode()
    return None

# exported function starts, for naming enclosing functions
starts = {}
for name, (sec, val) in exps.items():
    if sec == 1 and drel.get(val) == ('sect', 0):
        starts[int.from_bytes(data[val:val + 4], 'big')] = name

def func_start(o):
    while o > 0:
        if word(o) == 0x7c0802a6:  # mflr r0
            return o
        o -= 4
    return 0

def strings_near(lo, hi):
    """Strings formed by lwz rN,d(r2) followed by addi rM,rN,k."""
    out, base = [], {}
    for o in range(lo, hi, 4):
        w = word(o); op = w >> 26
        rt, ra, d = (w >> 21) & 31, (w >> 16) & 31, sx(w & 0xffff)
        if op == 32 and ra == 2:  # lwz rt,d(r2)
            slot = toc + d
            if drel.get(slot) == ('sect', 1):
                base[rt] = int.from_bytes(data[slot:slot + 4], 'big')
                s = cstr(base[rt])
                if s: out.append(s)
            else:
                base.pop(rt, None)
        elif op == 14 and ra in base:  # addi rt,ra,d
            s = cstr(base[ra] + d)
            if s: out.append(s)
        elif op == 14 and ra == 2:     # addi rt,r2,d (string in TOC area)
            s = cstr(toc + d)
            if s: out.append(s)
    return out

calls = collections.defaultdict(list)
for o in range(0, len(code) - 4, 4):
    w = word(o)
    if (w >> 26) == 18 and (w & 3) == 1:  # bl
        tgt = o + ((w & 0x3fffffc) - (0x4000000 if w & 0x2000000 else 0))
        if tgt != glue: continue
        for b in range(o - 4, max(o - 40, 0), -4):
            p = word(b)
            if p >> 26 == 32 and (p >> 21) & 31 == 12:  # lwz r12,d(ra)
                off = sx(p & 0xffff); ra = (p >> 16) & 31
                fs = func_start(o)
                calls[off].append((o, fs, ra))
                break

for off in sorted(calls):
    print(f'table +{off:#05x} (slot {off // 4}): {len(calls[off])} call(s)')
    for o, fs, ra in calls[off]:
        fname = starts.get(fs, f'func@{fs:#x}')
        near = strings_near(max(fs, o - 400), min(len(code), o + 400))
        near = [s.strip() for s in near if 'UIM' in s or 'root' in s.lower() or len(near) <= 3][:4]
        print(f'    at {o:#06x} in {fname} via r{ra}: {near}')
