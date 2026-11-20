#!/usr/bin/env python3
"""Compiler launcher for the arm64 game: static data aligned as x86-64 GCC aligns it.

x86-64 GCC aligns every static array or struct of 32 bytes or more to 32 bytes (16 or more to 16).
The game's DMA paths (DevCom, DVD, ARAM) assert the console's 32-byte alignment on buffers the
decompilation declares without an alignment, and the x86 builds meet it through that rule alone.
AArch64 GCC aligns such data to 8 bytes and has no option for more. So the arm64 build compiles with
-fdata-sections (one section per variable) and this raises each data section's alignment after the
compile, in the object's section headers: the linker then places every variable as x86 would align it.

usage (CMAKE_C_COMPILER_LAUNCHER): align_like_x86.py <compiler> <args...>
"""
import struct
import subprocess
import sys

SHT_PROGBITS, SHT_NOBITS = 1, 8
SHF_MERGE = 0x10
PREFIXES = (b'.data', b'.bss', b'.rodata', b'.sdata', b'.sbss')


def align_object(path: str) -> int:
    with open(path, 'r+b') as f:
        data = bytearray(f.read())
        if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1 or struct.unpack_from('<H', data, 16)[0] != 1:
            return 0   # not a little-endian ELF64 relocatable object
        shoff = struct.unpack_from('<Q', data, 0x28)[0]
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x3A)
        strtab_off = struct.unpack_from('<Q', data, shoff + shstrndx * shentsize + 0x18)[0]
        changed = 0
        for i in range(shnum):
            h = shoff + i * shentsize
            name_off, sh_type, flags = struct.unpack_from('<IIQ', data, h)
            size, = struct.unpack_from('<Q', data, h + 0x20)
            align, = struct.unpack_from('<Q', data, h + 0x30)
            if sh_type not in (SHT_PROGBITS, SHT_NOBITS) or flags & SHF_MERGE or not (flags & 0x2):   # SHF_ALLOC
                continue
            end = data.index(b'\0', strtab_off + name_off)
            name = bytes(data[strtab_off + name_off:end])
            if not name.startswith(PREFIXES):
                continue
            want = 32 if size >= 32 else 16 if size >= 16 else 0
            if want > align:
                struct.pack_into('<Q', data, h + 0x30, want)
                changed += 1
        if changed:
            f.seek(0)
            f.write(data)
        return changed


def main():
    command = sys.argv[1:]
    result = subprocess.run(command)
    if result.returncode:
        sys.exit(result.returncode)
    if '-c' in command and '-o' in command:
        out = command[command.index('-o') + 1]
        if out.endswith(('.o', '.obj')):
            align_object(out)


if __name__ == '__main__':
    main()
