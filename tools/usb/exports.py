#!/usr/bin/env python3
"""exports.py FRAGMENT.pef [OUT.code [OUT.data]]: export name -> code offset
(resolving TVectors), and optionally write the code / unpacked data sections."""
import sys, importlib.util, os
spec = importlib.util.spec_from_file_location('uimtable', os.path.join(os.path.dirname(__file__), 'uimtable.py'))
src = open(spec.origin).read().split('\ndef main')[0]
ns = {}
exec(src, ns)
secs, exps, rel, imps = ns['load'](open(sys.argv[1], 'rb').read())
code, data = secs[0]['body'], secs[1]['body']
drel = rel.get(1, {})
for name, (sec, val) in sorted(exps.items(), key=lambda x: x[1][1]):
    if sec == 1 and drel.get(val) == ('sect', 0):
        c = int.from_bytes(data[val:val + 4], 'big')
        print(f'{name:40} tvector data+{val:#06x} -> code+{c:#06x}')
    else:
        print(f'{name:40} section {sec} +{val:#x}')
if len(sys.argv) > 2:
    open(sys.argv[2], 'wb').write(code)
if len(sys.argv) > 3:
    open(sys.argv[3], 'wb').write(data)
# glue: data words relocated to imports (for following bl -> glue -> import)
if '--imports' in sys.argv:
    for off, r in sorted(drel.items()):
        if r[0] == 'import':
            print(f'  data+{off:#06x} = import {r[1]}')
