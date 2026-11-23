#!/usr/bin/env python3
"""importcalls.py FRAGMENT.pef SYMBOL...: find the calls to imported symbols.
Locates each import's TOC slot (data word relocated to the import), the
glue routine that loads it (lwz r12,d(r2)), and every bl to that glue."""
import sys, os
ns = {}
exec(open(os.path.join(os.path.dirname(__file__), 'uimtable.py')).read().split('\ndef main')[0], ns)
secs, exps, rel, imps = ns['load'](open(sys.argv[1], 'rb').read())
code, data = secs[0]['body'], secs[1]['body']
drel = rel.get(1, {})

toc = None
for name, (sec, val) in exps.items():
    if sec == 1 and drel.get(val + 4) == ('sect', 1):
        toc = int.from_bytes(data[val + 4:val + 8], 'big'); break

def word(o): return int.from_bytes(code[o:o + 4], 'big')
def sx(v): return v - 0x10000 if v & 0x8000 else v

def func_start(o):
    while o > 0 and word(o) != 0x7c0802a6:
        o -= 4
    return o

for sym in sys.argv[2:]:
    slots = [off for off, r in drel.items() if r == ('import', sym)]
    if not slots:
        print(f'{sym}: not imported'); continue
    for slot in slots:
        glues = [o for o in range(0, len(code) - 4, 4)
                 if word(o) >> 16 == 0x8182 and toc + sx(word(o) & 0xffff) == slot]   # lwz r12,d(r2)
        for g in glues:
            callers = []
            for o in range(0, len(code) - 4, 4):
                w = word(o)
                if w >> 26 == 18 and (w & 3) == 1:
                    t = o + ((w & 0x3fffffc) - (0x4000000 if w & 0x2000000 else 0))
                    if t == g:
                        callers.append((o, func_start(o)))
            print(f'{sym}: TOC data+{slot:#x}, glue code+{g:#x}, called at ' +
                  ', '.join(f'{o:#x} (func {f:#x})' for o, f in callers))
