#!/usr/bin/env python3
"""slotstrings.py UIM.pef [N]: for each function in ThePluginDispatchTable,
print its code address, which argument registers (r3-r10) it reads before
writing, and the log strings its body references (to name the slot)."""
import sys, os

ns = {}
exec(open(os.path.join(os.path.dirname(__file__), 'uimtable.py')).read().split('\ndef main')[0], ns)
secs, exps, rel, imps = ns['load'](open(sys.argv[1], 'rb').read())
code, data = secs[0]['body'], secs[1]['body']
drel = rel.get(1, {})
be = ns['be']

toc = None
for name, (sec, val) in exps.items():
    pass
# TOC from the first TVector in the table
t = exps['ThePluginDispatchTable'][1]

def word(o): return int.from_bytes(code[o:o + 4], 'big')
def sx(v): return v - 0x10000 if v & 0x8000 else v

def cstr(off):
    if not (0 <= off < len(data)): return None
    e = data.find(b'\0', off)
    s = data[off:e if e >= 0 else off + 90][:90]
    if len(s) >= 5 and all(32 <= c < 127 for c in s):
        return s.decode()
    return None

def func_end(o):
    """End of a function: first blr after o that is followed by a prologue
    (mflr r0) or by padding."""
    e = o
    while e < len(code) - 4:
        if word(e) == 0x4e800020 and (word(e + 4) == 0x7c0802a6 or word(e + 4) == 0):
            return e + 4
        e += 4
    return len(code)

def args_read(lo, hi):
    """Argument registers read before being written, in straight-line order
    (approximate: ignores branches)."""
    written, read = set(), []
    for o in range(lo, min(hi, lo + 4 * 80), 4):
        w = word(o); op = w >> 26
        rt, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        srcs, dst = [], None
        if op == 31:
            xo = (w >> 1) & 0x3ff
            if xo in (444, 28, 60, 124, 412, 316, 24, 536, 792, 824):  # or and andc nor orc xor slw srw sraw srawi
                srcs, dst = [rt, rb], ra
            elif xo in (151, 215, 407, 663, 150):  # stores
                srcs = [rt, ra, rb]
            else:
                srcs, dst = [ra, rb], rt
        elif op in (21, 20, 23):  # rlwinm/rlwimi/rlwnm: rs=rt field, ra dest
            srcs, dst = [rt], ra
        elif op in (24, 25, 26, 27, 28, 29):  # ori oris xori xoris andi. andis.
            srcs, dst = [rt], ra
        elif op in (36, 37, 38, 39, 44, 45, 47, 54, 55):  # stores / stmw
            srcs = [rt, ra] if op != 47 else [ra] + list(range(rt, 32))
        elif op in (32, 33, 34, 35, 40, 41, 42, 43, 46, 48, 50):  # loads
            srcs, dst = [ra], rt
        elif op in (14, 15):  # addi/addis
            srcs, dst = ([ra] if ra else []), rt
        elif op in (10, 11):  # cmpli/cmpi
            srcs = [ra]
        elif op == 18 and (w & 1):  # bl: callee may read args; stop
            break
        for r in srcs:
            if 3 <= r <= 10 and r not in written and r not in read:
                read.append(r)
        if dst is not None:
            written.add(dst)
    return sorted(read)

def strings_in(lo, hi):
    toc_val = None
    out, base = [], {}
    for o in range(lo, hi, 4):
        w = word(o); op = w >> 26
        rt, ra, d = (w >> 21) & 31, (w >> 16) & 31, sx(w & 0xffff)
        if op == 32 and ra == 2:
            out_base = None
            for tv in tocs:
                slot = tv + d
                if drel.get(slot) == ('sect', 1):
                    base[rt] = int.from_bytes(data[slot:slot + 4], 'big')
                    break
            else:
                base.pop(rt, None)
        elif op == 14 and ra in base:
            s = cstr(base[ra] + d)
            if s and s not in out: out.append(s)
    return out

# TOC: the second word of the table's TVectors
tocs = set()
for k in range(1, 40):
    o = t + 4 * k
    if drel.get(o) == ('sect', 1):
        tv = int.from_bytes(data[o:o + 4], 'big')
        if drel.get(tv + 4) == ('sect', 1):
            tocs.add(int.from_bytes(data[tv + 4:tv + 8], 'big'))

n = int(sys.argv[2]) if len(sys.argv) > 2 else 30
print('table version', int.from_bytes(data[t:t + 4], 'big'))
for k in range(1, n):
    o = t + 4 * k
    if drel.get(o) != ('sect', 1):
        print(f'[{k:2}] +{4*k:#04x}  (empty)')
        continue
    tv = int.from_bytes(data[o:o + 4], 'big')
    c = int.from_bytes(data[tv:tv + 4], 'big')
    e = func_end(c)
    s = [x.strip()[:70] for x in strings_in(c, e)]
    print(f'[{k:2}] +{4*k:#04x}  code+{c:#06x}..{e:#06x}  args {["r%d" % r for r in args_read(c, e)]}')
    for x in s[:4]:
        print('        ', x)
