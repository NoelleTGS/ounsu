#!/usr/bin/env python3
"""Print the undefined symbols of an ELF64 relocatable object that no relocation refers to.

    python3 tools/unused-undef.py OBJECT

Used by make-island.sh to drop debris such as the .NET runtime's "dst"/"val" (macro parameters left in
WriteBarriers.S.o's symbol table). Reads the symbol and relocation tables directly: `readelf -rW` took over 16
minutes on osu!'s island (millions of debug-info relocations).
"""
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
    sys.exit("unused-undef: not a little-endian ELF64 file")
shoff, = struct.unpack_from("<Q", data, 0x28)
shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
if shnum == 0:  # more than 65535 sections: the count is in section header 0's sh_size
    shnum = struct.unpack_from("<IIQQQQIIQQ", data, shoff)[5]
sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]

symtab_index = next(i for i, s in enumerate(sections) if s[1] == 2)  # SHT_SYMTAB
_, _, _, _, sym_off, sym_size, strtab_link, _, _, _ = sections[symtab_index]
str_off = sections[strtab_link][4]

referenced = set()
for name, stype, flags, addr, off, size, link, info, align, entsize in sections:
    if stype == 4 and link == symtab_index:  # SHT_RELA against the symbol table
        for r_offset, r_info, r_addend in struct.iter_unpack("<QQq", data[off:off + size]):
            referenced.add(r_info >> 32)

for k, (st_name, st_info, st_other, st_shndx, st_value, st_size) in enumerate(
        struct.iter_unpack("<IBBHQQ", data[sym_off:sym_off + sym_size])):
    if k and st_shndx == 0 and st_info >> 4 in (1, 2) and k not in referenced and st_name:  # undefined global/weak
        print(data[str_off + st_name:data.index(b"\0", str_off + st_name)].decode())
