#!/usr/bin/env python3
"""List the functions in an ELF64 aarch64 relocatable object that contain an `svc` instruction.

    python3 tools/svc-scan.py OBJECT [ALLOWED_FUNCTION...]

Prints one function name per line (the symbol covering the instruction, or SECTION+offset) and exits 1 if any is
not in the allowed list. Used by make-island.sh: a raw Linux syscall left in the island would be a Horizon SVC 0.
Reads the section and symbol tables directly (objdump -d took many minutes on an island with OpenSSL in it).
"""
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
allowed = set(sys.argv[2:])
if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
    sys.exit("svc-scan: not a little-endian ELF64 file")

shoff, = struct.unpack_from("<Q", data, 0x28)
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
if shnum == 0 or shstrndx == 0xFFFF:  # more than 65535 sections: the real values are in section header 0
    _sh0 = struct.unpack_from("<IIQQQQIIQQ", data, shoff)
    shnum = shnum or _sh0[5]
    shstrndx = _sh0[6] if shstrndx == 0xFFFF else shstrndx
if shnum == 0:
    sys.exit("svc-scan: no sections")
sections = []
for i in range(shnum):
    name, stype, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
        "<IIQQQQIIQQ", data, shoff + i * shentsize)
    sections.append((name, stype, flags, off, size, link, entsize))
shstr_off = sections[shstrndx][3]


def cstr(off):
    return data[off:data.index(b"\0", off)].decode("ascii", "replace")


# Function symbols per section: (value, size, name)
funcs = {}
for name, stype, flags, off, size, link, entsize in sections:
    if stype != 2:  # SHT_SYMTAB
        continue
    strtab = sections[link][3]
    for k in range(size // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, off + k * 24)
        if st_info & 0xF in (0, 1, 2) and st_name and data[strtab + st_name] != 0x24:  # named, not a $x/$d mapping symbol
            funcs.setdefault(st_shndx, []).append((st_value, st_size, cstr(strtab + st_name)))

found = set()
SHF_EXECINSTR = 0x4
for idx, (name, stype, flags, off, size, link, entsize) in enumerate(sections):
    if stype != 1 or not flags & SHF_EXECINSTR:  # SHT_PROGBITS, executable
        continue
    words = struct.unpack_from(f"<{size // 4}I", data, off)
    for k, w in enumerate(words):
        if w & 0xFFE0001F == 0xD4000001:  # svc #imm
            at = k * 4
            who = f"{cstr(shstr_off + name)}+{at:#x}"
            # The symbol whose range covers it; assembly without .size: the nearest one before it.
            best = None
            for value, fsize, fname in funcs.get(idx, ()):
                if value <= at < value + fsize:
                    best = (value, fname)
                    break
                if fsize == 0 and value <= at and (best is None or value > best[0]):
                    best = (value, fname)
            if best:
                who = best[1]
            found.add(who)

bad = sorted(found - allowed)
for f in sorted(found):
    print(f + ("" if f in allowed else "   <- not allowed"))
sys.exit(1 if bad else 0)
