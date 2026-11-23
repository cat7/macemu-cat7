#!/usr/bin/env python3
"""List code fragments of a classic Mac OS file (cfrg + PEF): names,
exports and imports.  Usage: pef.py FILE [FRAGMENT-NAME-SUBSTRING] [--dump-dir DIR]"""
import struct, sys, os

def be(fmt, b, o):
    return struct.unpack_from('>' + fmt, b, o)

def resources(rsrc):
    data_off, map_off = be('II', rsrc, 0)
    tl_off, nl_off = be('HH', rsrc, map_off + 24)
    tl = map_off + tl_off
    (ntypes,) = be('H', rsrc, tl)
    out = {}
    for i in range(ntypes + 1):
        t, cnt, ref_off = be('4sHH', rsrc, tl + 2 + 8 * i)
        for j in range(cnt + 1):
            rid, name_off, attr_off = be('hHI', rsrc, tl + ref_off + 12 * j)
            off = data_off + (attr_off & 0xffffff)
            (ln,) = be('I', rsrc, off)
            out.setdefault(t.decode('mac_roman'), {})[rid] = rsrc[off + 4: off + 4 + ln]
    return out

def cfrg_members(cfrg):
    (count,) = be('I', cfrg, 28)
    o, res = 32, []
    for _ in range(count):
        arch, = be('4s', cfrg, o)
        usage, where, offset, length = be('BBII', cfrg, o + 22)
        msize, = be('H', cfrg, o + 40)
        nlen = cfrg[o + 42]
        name = cfrg[o + 43:o + 43 + nlen].decode('mac_roman')
        res.append(dict(arch=arch.decode('mac_roman'), usage=usage, where=where,
                        offset=offset, length=length, name=name))
        o += msize
    return res

USAGE = {0: 'import library', 1: 'application', 2: 'drop-in/plugin', 3: 'stub', 4: 'weak stub'}
CLASS = {0: 'code', 1: 'data', 2: 'tvect', 3: 'toc', 4: 'glue'}

def pef_info(p):
    assert p[:8] == b'Joy!peff', 'not PEF'
    nsec, = be('H', p, 32)
    secs = []
    for i in range(nsec):
        so = 40 + 28 * i
        name_off, defaddr, total, unpacked, clen, coff = be('iIIIII', p, so)
        kind = p[so + 24]
        secs.append(dict(kind=kind, total=total, clen=clen, coff=coff))
    ld = next(s for s in secs if s['kind'] == 4)
    L = p[ld['coff']: ld['coff'] + ld['clen']]
    (mainS, mainO, initS, initO, termS, termO, nlib, nimp, nrel, relo,
     strs, hasho, hpow, nexp) = be('iIiIiIIIIIIIII', L, 0)
    def s(off):
        e = L.index(b'\0', strs + off)
        return L[strs + off:e].decode('mac_roman')
    libs = []
    for i in range(nlib):
        lo = 56 + 24 * i
        noff, oldv, curv, cnt, first = be('IIIII', L, lo)
        libs.append((s(noff), first, cnt))
    imps = []
    for i in range(nimp):
        (v,) = be('I', L, 56 + 24 * nlib + 4 * i)
        imps.append((CLASS.get((v >> 24) & 0xf, v >> 24), s(v & 0xffffff)))
    exps = []
    keys = hasho + 4 * (1 << hpow)
    syms = keys + 4 * nexp
    for i in range(nexp):
        (klen,) = be('I', L, keys + 4 * i)
        cn, val, sec = be('IIh', L, syms + 10 * i)
        n = L[strs + (cn & 0xffffff): strs + (cn & 0xffffff) + (klen >> 16)].decode('mac_roman')
        exps.append((CLASS.get(cn >> 24, cn >> 24), n, sec, val))
    return dict(sections=secs, libs=libs, imports=imps, exports=exps,
                main=(mainS, mainO), init=(initS, initO), term=(termS, termO))

def main():
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else None
    dump = sys.argv[sys.argv.index('--dump-dir') + 1] if '--dump-dir' in sys.argv else None
    data = open(path, 'rb').read()
    rsrc = open(path + '/..namedfork/rsrc', 'rb').read()
    for m in cfrg_members(resources(rsrc)['cfrg'][0]):
        if want and want.lower() not in m['name'].lower():
            print(f"- {m['name']!r}: {USAGE.get(m['usage'], m['usage'])}, {m['arch']}, data fork @{m['offset']:#x} len {m['length']}")
            continue
        print(f"== {m['name']!r}: {USAGE.get(m['usage'], m['usage'])}, {m['arch']}, data fork @{m['offset']:#x} len {m['length']}")
        length = m['length'] or len(data) - m['offset']
        p = data[m['offset']: m['offset'] + length]
        if dump:
            os.makedirs(dump, exist_ok=True)
            open(os.path.join(dump, m['name'].replace('/', '_') + '.pef'), 'wb').write(p)
        try:
            info = pef_info(p)
        except Exception as e:
            print('   (not parsed:', e, ')'); continue
        print('   sections:', [(s['kind'], s['total']) for s in info['sections']], 'init', info['init'], 'term', info['term'])
        print('   exports:')
        for c, n, sec, val in sorted(info['exports'], key=lambda x: x[1]):
            print(f'     {c:5} {n}  (section {sec} +{val:#x})')
        print('   imports:')
        for lib, first, cnt in info['libs']:
            names = [n for _, n in info['imports'][first:first + cnt]]
            print(f'     {lib}: {len(names)}: ' + ', '.join(names))

main()
