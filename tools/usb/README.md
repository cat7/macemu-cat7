# USB passthrough spike tools

Tools for recovering the Mac OS 9 USB Interface Module (UIM) interface from
Apple's stripped PEF code fragments. See the USB passthrough proposal for
context; the Apple binaries themselves are not part of this repository.

| Tool | Use |
| --- | --- |
| `pef.py FILE [NAME] [--dump-dir DIR]` | List a classic Mac OS file's code fragments (`cfrg`), with exports and imports. Reads the resource fork through `FILE/..namedfork/rsrc`, so run it on macOS against an HFS+ volume. `--dump-dir` writes the matching fragments as `.pef` files |
| `uimtable.py FRAGMENT.pef [N]` | Decode `TheDriverDescription` and the first N words of `ThePluginDispatchTable` (unpacks pattern-initialized data, applies PEF relocations, follows TVectors into code) |
| `exports.py FRAGMENT.pef [OUT.code [OUT.data]]` | Map exports to code offsets; optionally write the code section and the unpacked data section |
| `tablecalls.py FRAGMENT.pef GLUE` | Find indirect calls (`lwz r12,OFF(rX)` + `bl GLUE`, the pointer-call glue) and print each table offset with the log strings near the call |
| `slotstrings.py UIM.pef [N]` | Per dispatch-table slot: code range, argument registers read, and the log strings in its body (names the slot) |
| `ppcdis FILE START END` | Disassemble big-endian PowerPC code from a raw file (`make` builds it) |

Typical run, with the Mac OS 9.2 USB DDK disk image attached read-only
(`hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage IMAGE`):

```sh
V="/Volumes/<volume>"
python3 -I pef.py "$V/System Folder/Extensions/USB Device Extension" USBOpenHCIDriver --dump-dir frags
python3 -I uimtable.py frags/USBOpenHCIDriver.pef 32
python3 -I exports.py frags/USBOpenHCIDriver.pef ohci.code ohci.data
make && ./ppcdis ohci.code 0x10ac 0x1100
```

The USB Services Library and USB Expert are `nlib` resources -20776 and
-20777 in `USB Support`; extract them with the `resources()` helper in
`pef.py`. The pointer-call glue is the code sequence
`800c0000 90410014 7c0903a6 804c0004 4e800420`.
