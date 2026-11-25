#!/usr/bin/env python3
"""mksndhwstub.py OUT

Write a stand-in "SndHWUtilitiesLib" for SheepShaver: a Mac OS shared
library (file type 'shlb', a PEF fragment in the data fork, described by a
'cfrg' resource) that exports AudioNotify as a function returning 0.

Mac OS 9's Sound control panel (8.5.x) imports AudioNotify from
SndHWUtilitiesLib, strictly (not weak). That library is part of Apple Audio
Extension, whose startup code gives up under SheepShaver (it only runs on
Apple's own sound hardware), so the library is never registered and the
control panel refuses to open: "SndHWUtilitiesLib AudioNotify could not be
found". AudioNotify tells Apple's sound hardware plug-ins about changed
settings; SheepShaver's sound driver needs no such notice, so doing nothing
is enough. Put the file in the System Folder's Extensions folder.

Run on macOS: the resource fork goes to OUT/..namedfork/rsrc and the type
and creator to the com.apple.FinderInfo attribute. Check the result with
tools/usb/pef.py OUT."""
import struct, sys, subprocess

LIB_NAME = b'SndHWUtilitiesLib'
CURRENT_VERSION = 0x01018000        # what the Sound control panel was linked against (1.0.1)
OLD_DEF_VERSION = 0                 # like the real one: any older version
EXPORTS = [b'AudioNotify']

def hashword(name):
    h = 0
    for c in name:
        h = ((h << 1) - (h >> 16)) & 0xffffffff
        if h & 0x80000000:
            h -= 1 << 32
        h ^= c
    h &= 0xffffffff
    if h & 0x80000000:
        h -= 1 << 32
    return (len(name) << 16) | ((h ^ (h >> 16)) & 0xffff)

def align16(x): return (x + 15) & ~15

# ---- code: li r3,0 ; blr ----
code = struct.pack('>2I', 0x38600000, 0x4e800020)

# ---- data: one TVector per export (code address, TOC = data base) ----
data = bytearray(8 * len(EXPORTS))
relocs = []                         # (offset, 'C' or 'D')
for k in range(len(EXPORTS)):
    struct.pack_into('>II', data, 8 * k, 0, 0)
    relocs += [(8 * k, 'C'), (8 * k + 4, 'D')]

# ---- loader section ----
ri, pos = [], 0
for o, kind in relocs:
    if o != pos:
        ri += [0xA000 | ((o >> 16) & 0x3ff), o & 0xffff]  # RelocSetPosition
    ri.append(0x4000 if kind == 'C' else 0x4200)         # RelocBySectC / RelocBySectD, 1 word
    pos = o + 4
strings = b''.join(n + b'\0' for n in EXPORTS)
name_offs, o = [], 0
for n in EXPORTS:
    name_offs.append(o)
    o += len(n) + 1
reloc_hdr_off = 56                  # no imports
reloc_instr_off = reloc_hdr_off + 12
strings_off = (reloc_instr_off + 2 * len(ri) + 3) & ~3
hash_off = (strings_off + len(strings) + 3) & ~3
n = len(EXPORTS)
loader = bytearray(hash_off + 4 + 4 * n + 10 * n)
struct.pack_into('>iIiIiIIIIIIIII', loader, 0,
                 -1, 0, -1, 0, -1, 0,           # main, init, term: none
                 0, 0,                          # imported libraries / symbols
                 1, reloc_instr_off,            # relocated sections, instructions
                 strings_off, hash_off, 0, n)   # hash table power 0: one bucket
struct.pack_into('>HHII', loader, reloc_hdr_off, 1, 0, len(ri), 0)  # data section's relocations
for k, w in enumerate(ri):
    struct.pack_into('>H', loader, reloc_instr_off + 2 * k, w)
loader[strings_off:strings_off + len(strings)] = strings
struct.pack_into('>I', loader, hash_off, (n << 18) | 0)
for k, name in enumerate(EXPORTS):
    struct.pack_into('>I', loader, hash_off + 4 + 4 * k, hashword(name))
    struct.pack_into('>IIh', loader, hash_off + 4 + 4 * n + 10 * k,
                     (2 << 24) | name_offs[k], 8 * k, 1)   # TVector symbol in section 1

# ---- PEF container ----
sec_hdr_off = 40
code_pos = align16(sec_hdr_off + 3 * 28)
data_pos = align16(code_pos + len(code))
loader_pos = align16(data_pos + len(data))
pef = bytearray(loader_pos + len(loader))
struct.pack_into('>4s4s4sIIIIIHHI', pef, 0, b'Joy!', b'peff', b'pwpc', 1, 0,
                 OLD_DEF_VERSION, OLD_DEF_VERSION, CURRENT_VERSION, 3, 2, 0)
for k, (kind, share, p, body) in enumerate([(0, 4, code_pos, code), (1, 1, data_pos, data), (4, 4, loader_pos, loader)]):
    l = len(body) if kind != 4 else 0
    struct.pack_into('>iIIIIIBBBB', pef, sec_hdr_off + 28 * k,
                     -1, 0, l, l, len(body), p, kind, share, 4, 0)
    pef[p:p + len(body)] = body

# ---- 'cfrg' 0: one import library, the whole data fork ----
name = bytes([len(LIB_NAME)]) + LIB_NAME
member_size = (42 + len(name) + 3) & ~3
member = bytearray(member_size)
struct.pack_into('>4sHBBIIIHBBIIIHHH', member, 0, b'pwpc', 0, 0, 0,
                 CURRENT_VERSION, OLD_DEF_VERSION, 0, 0,
                 0, 1, 0, 0,                    # usage: import library; where: data fork; offset, length (all)
                 0, 0, 0, member_size)
member[42:42 + len(name)] = name
cfrg = struct.pack('>III16sI', 0, 0, 1, bytes(16), 1) + bytes(member)

# ---- resource fork with that one resource ----
res_data = struct.pack('>I', len(cfrg)) + cfrg
type_list = struct.pack('>H4sHH', 0, b'cfrg', 0, 10)          # 1 type, 1 resource, refs at +10
ref_list = struct.pack('>hHI4s', 0, 0xffff, 0, bytes(4))      # id 0, no name, data offset 0
map_body = type_list + ref_list
map_len = 28 + len(map_body)
data_off = 256
map_off = data_off + len(res_data)
header = struct.pack('>IIII', data_off, map_off, len(res_data), map_len)
res_map = header + bytes(4) + struct.pack('>hHHH', 0, 0, 28, map_len) + map_body
rsrc = header + bytes(data_off - 16) + res_data + res_map

out = sys.argv[1]
open(out, 'wb').write(pef)
open(out + '/..namedfork/rsrc', 'wb').write(rsrc)
finder = b'shlb' + b'????' + bytes(24)
subprocess.run(['/usr/bin/xattr', '-wx', 'com.apple.FinderInfo', finder.hex(), out], check=True)
print(f'{out}: PEF {len(pef)} bytes, cfrg {len(cfrg)} bytes, exports ' + ', '.join(e.decode() for e in EXPORTS))
