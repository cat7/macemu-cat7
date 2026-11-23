#!/usr/bin/env python3
"""mkuimstub.py OUT.i [OUT.pef]

Generate SheepShaver's paravirtual USB UIM: a PEF 'ndrv' code fragment that
exports TheDriverDescription and ThePluginDispatchTable.  Every table slot is
a stub that loads the slot number into r11 and jumps to the emulator's
NATIVE_USB_UIM routine through the XLM_USB_UIM low-memory vector; the native
routine returns straight to the caller.  r2 is left alone: the caller's
pointer-call glue set it to this fragment's TOC (= data section base), which
tells the emulator where the dispatch table is.

The table version is patched at UIMInitialize time to match the USB stack
that loads the UIM: 4 for the USB 1.2.x stack in the Mac OS ROM file, 6 for
disk-based USB 1.4-1.5.9.  Slots 1-21 are at the same places in both; v4
also uses slot 22, v6 adds 26-29.

The fragment matches Name Registry entries named or compatible with
MATCH_NAME, which is unique to SheepShaver, so Apple's OHCI UIM never binds
to SheepShaver's USB entry.  Output: a C array (like VideoDriverStub.i)."""
import struct, sys

MATCH_NAME = b'SheepShaver,usb-uim'   # must match name_registry.cpp
DRIVER_NAME = b'SheepUIM'
VERSION = 0x01008000                   # NumVersion 1.0.0 final
XLM_USB_UIM = 0x28dc                   # must match include/xlowmem.h
TABLE_VERSION = 6                      # initial value; set again by UIMInitialize
NUM_SLOTS = 29
EMPTY_SLOTS = {5, 6, 9, 10, 13, 14, 17, 18}   # empty in Apple's v4 and v6 tables

def pstr(s, size):
    assert len(s) < size
    return bytes([len(s)]) + s + bytes(size - 1 - len(s))

def hashword(name):
    h = 0
    for c in name:
        h = ((h << 1) - (h >> 16)) & 0xffffffff
        if h & 0x80000000:          # keep it a signed 32-bit value
            h -= 1 << 32
        h ^= c
    h &= 0xffffffff
    if h & 0x80000000:
        h -= 1 << 32
    return (len(name) << 16) | ((h ^ (h >> 16)) & 0xffff)

# ---- code section: one stub per implemented slot ----
slots = [s for s in range(1, NUM_SLOTS + 1) if s not in EMPTY_SLOTS]
code = b''
code_off = {}
for s in slots:
    code_off[s] = len(code)
    code += struct.pack('>5I',
                        0x39600000 | s,              # li     r11,slot
                        0x80000000 | XLM_USB_UIM,    # lwz    r0,XLM_USB_UIM(0)
                        0x7c0903a6,                  # mtctr  r0
                        0x4e800420,                  # bctr
                        0x60000000)                  # nop (padding)

# ---- data section ----
desc = (b'mtej' + struct.pack('>I', 0) +                       # signature, desc version
        pstr(MATCH_NAME, 32) + struct.pack('>I', VERSION) +     # DriverType
        struct.pack('>I', 0x4) +                                # runtime: under expert control
        pstr(DRIVER_NAME, 32) + bytes(32) +                     # driver name, reserved
        struct.pack('>I', 1) + b'usb ' + bytes(4) + struct.pack('>I', VERSION))  # 1 service
assert len(desc) == 128
table_off = len(desc)
table_len = 4 * (NUM_SLOTS + 1)
tv_off = table_off + table_len
data = bytearray(desc + bytes(table_len) + bytes(8 * len(slots)))
struct.pack_into('>I', data, table_off, TABLE_VERSION)
relocs_c, relocs_d = [], []     # data offsets to relocate by code / data base
for k, s in enumerate(slots):
    tv = tv_off + 8 * k
    struct.pack_into('>I', data, table_off + 4 * s, tv)      # table -> TVector (data)
    relocs_d.append(table_off + 4 * s)
    struct.pack_into('>II', data, tv, code_off[s], 0)       # TVector: code, TOC = data base
    relocs_c.append(tv)
    relocs_d.append(tv + 4)

# ---- loader section ----
names = [b'TheDriverDescription', b'ThePluginDispatchTable']
values = [0, table_off]
strings = b''.join(n + b'\0' for n in names)
name_offs = [0, len(names[0]) + 1]

def reloc_instrs():
    marks = sorted([(o, 'C') for o in relocs_c] + [(o, 'D') for o in relocs_d])
    out, pos = [], 0
    for o, kind in marks:
        if o != pos:
            out += [0xA000 | ((o >> 16) & 0x3ff), o & 0xffff]   # RelocSetPosition
        out.append(0x4000 if kind == 'C' else 0x4200)          # RelocBySectC/D, run 1
        pos = o + 4
    return out

ri = reloc_instrs()
hdr_len = 56
reloc_hdr_off = hdr_len                   # no imported libraries / symbols
reloc_instr_off = reloc_hdr_off + 12
strings_off = reloc_instr_off + 2 * len(ri)
strings_off = (strings_off + 3) & ~3
hash_off = (strings_off + len(strings) + 3) & ~3
loader = bytearray(hash_off + 4 + 4 * len(names) + 10 * len(names))
struct.pack_into('>iIiIiIIIIIIIII', loader, 0,
                 -1, 0, -1, 0, -1, 0,           # main, init, term: none
                 0, 0,                          # imported libraries / symbols
                 1, reloc_instr_off,            # relocated sections, instructions
                 strings_off, hash_off, 0, len(names))
struct.pack_into('>HHII', loader, reloc_hdr_off, 1, 0, len(ri), 0)
for k, w in enumerate(ri):
    struct.pack_into('>H', loader, reloc_instr_off + 2 * k, w)
loader[strings_off:strings_off + len(strings)] = strings
struct.pack_into('>I', loader, hash_off, (len(names) << 18) | 0)   # 1 bucket: count, first index
for k, n in enumerate(names):
    struct.pack_into('>I', loader, hash_off + 4 + 4 * k, hashword(n))
    struct.pack_into('>IIh', loader, hash_off + 4 + 4 * len(names) + 10 * k,
                     (1 << 24) | name_offs[k], values[k], 1)        # data symbols in section 1

# ---- container ----
def align16(x): return (x + 15) & ~15
sec_hdr_off = 40
code_pos = align16(sec_hdr_off + 3 * 28)
data_pos = align16(code_pos + len(code))
loader_pos = align16(data_pos + len(data))
pef = bytearray(loader_pos + len(loader))
struct.pack_into('>4s4s4sIIIIIHHI', pef, 0, b'Joy!', b'peff', b'pwpc', 1, 0, 0, 0, 0, 3, 2, 0)
for k, (kind, share, pos, body) in enumerate([(0, 4, code_pos, code), (1, 1, data_pos, data), (4, 4, loader_pos, loader)]):
    l = len(body) if kind != 4 else 0
    struct.pack_into('>iIIIIIBBBB', pef, sec_hdr_off + 28 * k,
                     -1, 0, l, l, len(body), pos, kind, share, 4, 0)
    pef[pos:pos + len(body)] = body

with open(sys.argv[1], 'w') as f:
    f.write('// Generated by tools/usb/mkuimstub.py - do not edit\n')
    f.write(f'// SheepShaver paravirtual USB UIM: {len(slots)} slot stubs, table version {TABLE_VERSION}\n')
    for i in range(0, len(pef), 16):
        f.write('\t' + ' '.join(f'0x{b:02x},' for b in pef[i:i + 16]) + '\n')
if len(sys.argv) > 2:
    open(sys.argv[2], 'wb').write(pef)
print(f'{len(pef)} bytes, {len(slots)} stubs')
