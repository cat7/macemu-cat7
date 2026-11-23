#!/usr/bin/env python3
"""Decode a PEF driver's TheDriverDescription and ThePluginDispatchTable:
unpack the data section, apply relocations, follow TVectors into code and
name each function from its traceback table.  Usage: uimtable.py FRAGMENT.pef"""
import struct, sys, re

def be(fmt, b, o):
    return struct.unpack_from('>' + fmt, b, o)

def varint(b, i):
    v = 0
    while True:
        c = b[i]; i += 1
        v = (v << 7) | (c & 0x7f)
        if not c & 0x80:
            return v, i

def unpack_pidata(src, size):
    out, i = bytearray(), 0
    while i < len(src):
        op = src[i] >> 5; cnt = src[i] & 0x1f; i += 1
        if cnt == 0:
            cnt, i = varint(src, i)
        if op == 0:
            out += bytes(cnt)
        elif op == 1:
            out += src[i:i + cnt]; i += cnt
        elif op == 2:
            rep, i = varint(src, i)
            blk = src[i:i + cnt]; i += cnt
            out += blk * (rep + 1)
        elif op in (3, 4):
            custom, i = varint(src, i)
            rep, i = varint(src, i)
            if op == 3:
                common = src[i:i + cnt]; i += cnt
            else:
                common = bytes(cnt)
            out += common
            for _ in range(rep):
                out += src[i:i + custom]; i += custom
                out += common
        else:
            raise ValueError(f'pidata opcode {op}')
    out += bytes(max(0, size - len(out)))
    return bytes(out)

def load(p):
    nsec, = be('H', p, 32)
    secs = []
    for k in range(nsec):
        so = 40 + 28 * k
        _, defaddr, total, unpacked, clen, coff = be('iIIIII', p, so)
        kind = p[so + 24]
        raw = p[coff:coff + clen]
        if kind == 2:
            body = unpack_pidata(raw, total)
        else:
            body = raw + bytes(max(0, total - len(raw)))
        secs.append(dict(kind=kind, body=bytearray(body)))
    ld = next(s for s in secs if s['kind'] == 4)['body']
    (mainS, mainO, initS, initO, termS, termO, nlib, nimp, nrel, relo,
     strs, hasho, hpow, nexp) = be('iIiIiIIIIIIIII', ld, 0)
    def s(off):
        e = ld.index(b'\0', strs + off)
        return ld[strs + off:e].decode('mac_roman')
    imps = [s(be('I', ld, 56 + 24 * nlib + 4 * k)[0] & 0xffffff) for k in range(nimp)]
    # exports
    keys = hasho + 4 * (1 << hpow); syms = keys + 4 * nexp
    exps = {}
    for k in range(nexp):
        klen, = be('I', ld, keys + 4 * k)
        cn, val, sec = be('IIh', ld, syms + 10 * k)
        n = ld[strs + (cn & 0xffffff): strs + (cn & 0xffffff) + (klen >> 16)].decode('mac_roman')
        exps[n] = (sec, val)
    # relocations: record what each relocated word in each section refers to
    rel = {}
    hdr0 = 56 + 24 * nlib + 4 * nimp
    for r in range(nrel):
        sidx, _, cnt, first = be('HHII', ld, hdr0 + 12 * r)
        ins = [be('H', ld, relo + first + 2 * k)[0] for k in range(cnt)]
        tags = rel.setdefault(sidx, {})
        addr, imp_i, secC, secD = 0, 0, 0, 1
        k = 0
        def put(kind):
            nonlocal addr
            tags[addr] = kind; addr += 4
        hist = []
        while k < len(ins):
            w = ins[k]; k += 1
            if w >> 14 == 0:
                addr += 4 * ((w >> 6) & 0xff)
                for _ in range(w & 0x3f): put(('sect', secD))
            elif w >> 13 == 2:
                sub, run = (w >> 9) & 0xf, (w & 0x1ff) + 1
                for _ in range(run):
                    if sub == 0: put(('sect', secC))
                    elif sub == 1: put(('sect', secD))
                    elif sub == 2: put(('sect', secC)); put(('sect', secD)); addr += 4
                    elif sub == 3: put(('sect', secC)); put(('sect', secD))
                    elif sub == 4: put(('sect', secD)); addr += 4
                    elif sub == 5: put(('import', imps[imp_i])); imp_i += 1
            elif w >> 13 == 3:
                sub, idx = (w >> 9) & 0xf, w & 0x1ff
                if sub == 0: put(('import', imps[idx])); imp_i = idx + 1
                elif sub == 1: secC = idx
                elif sub == 2: secD = idx
                elif sub == 3: put(('sect', idx))
            elif w >> 12 == 8:
                addr += (w & 0xfff) + 1
            elif w >> 12 == 9:
                blocks, rep = ((w >> 8) & 0xf) + 1, (w & 0xff) + 1
                seq = ins[k - 1 - blocks:k - 1]
                ins[k:k] = seq * rep
            elif w >> 10 == 0x28:
                addr = ((w & 0x3ff) << 16) | ins[k]; k += 1
            elif w >> 10 == 0x29:
                idx = ((w & 0x3ff) << 16) | ins[k]; k += 1
                put(('import', imps[idx])); imp_i = idx + 1
            else:
                raise ValueError(f'reloc {w:#06x}')
    return secs, exps, rel, imps

def tb_name(code, off):
    """Name from the traceback table after the function at off."""
    for z in range(off, min(len(code) - 8, off + 0x20000), 4):
        if be('I', code, z)[0] == 0 and code[z + 4] == 0 and code[z + 5] in (0, 1, 9, 12):
            region = code[z + 8:z + 8 + 64]
            m = re.search(rb'\x00([\x02-\x3f])([A-Za-z_][A-Za-z0-9_]+)', region)
            if m and len(m.group(2)) >= m.group(1)[0]:
                return m.group(2)[:m.group(1)[0]].decode()
    return '?'

def p_str(b, o, n=32):
    return b[o + 1:o + 1 + b[o]].decode('mac_roman')

def main():
    p = open(sys.argv[1], 'rb').read()
    secs, exps, rel, imps = load(p)
    code = secs[0]['body']; data = secs[1]['body']; drel = rel.get(1, {})
    dd = exps['TheDriverDescription'][1]
    sig, ver = be('4sI', data, dd)
    print('TheDriverDescription:', sig, 'version', ver)
    print('  match name:', repr(p_str(data, dd + 8)), ' version', data[dd + 40:dd + 44].hex())
    rt, = be('I', data, dd + 44)
    print('  runtime flags', hex(rt), ' driver name:', repr(p_str(data, dd + 48)))
    nsv, = be('I', data, dd + 112)
    for k in range(nsv):
        cat, typ = be('4s4s', data, dd + 116 + 12 * k)
        print('  service', cat, typ, data[dd + 124 + 12 * k:dd + 128 + 12 * k].hex())
    t = exps['ThePluginDispatchTable'][1]
    print('ThePluginDispatchTable @ data+%#x' % t)
    for k in range(int(sys.argv[2]) if len(sys.argv) > 2 else 40):
        o = t + 4 * k
        v, = be('I', data, o)
        r = drel.get(o)
        if r and r[0] == 'sect' and r[1] == 1:
            tv_code, = be('I', data, v)       # TVector: code address, TOC
            name = tb_name(code, tv_code) if drel.get(v) == ('sect', 0) else '(data)'
            print(f'  [{k:2}] +{4*k:#04x}  -> tvector data+{v:#x} -> code+{tv_code:#x}  {name}')
        elif r:
            print(f'  [{k:2}] +{4*k:#04x}  {r}')
        else:
            print(f'  [{k:2}] +{4*k:#04x}  value {v:#010x}')

main()
