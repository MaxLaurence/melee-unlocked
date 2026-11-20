"""Wrap the AArch64 melee_game.so in a signed Mach-O so Apple silicon will run its code.

Apple silicon executes only pages whose code signature the kernel knows. The game is a Linux ELF that
the host loads itself (elf_loader.cpp), at its console addresses inside a window above 4 GB, which
dyld could never do. So the ELF goes, byte for byte, into a 16 KB-aligned __TEXT,__elf section of an
otherwise empty dylib that the linker signs (ad hoc). The loader registers that signature with the
kernel (F_ADDFILESIGS_RETURN) and maps the ELF's executable segment straight from the file.
"""
from pathlib import Path
import argparse, subprocess, tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path, help='melee_game.so built for AArch64 (port/mac/build-game.sh)')
    parser.add_argument('out', type=Path, help='the container to write (melee_game.dylib)')
    args = parser.parse_args()
    elf = args.elf.resolve()
    head = elf.read_bytes()[:20]
    if head[:4] != b'\x7fELF' or int.from_bytes(head[18:20], 'little') != 183:
        raise SystemExit(f'{elf} is not an AArch64 ELF')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        asm = Path(tmp) / 'container.s'
        quoted = str(elf).replace('\\', '\\\\').replace('"', '\\"')
        asm.write_text('.section __TEXT,__elf,regular,pure_instructions\n'
                       '.p2align 14\n'
                       '.globl _mu_game_elf\n'
                       '_mu_game_elf:\n'
                       f'.incbin "{quoted}"\n'
                       '.p2align 14\n')
        subprocess.run(['xcrun', 'clang', '-arch', 'arm64', '-dynamiclib', '-mmacosx-version-min=13.0',
                        '-install_name', '@rpath/melee_game.dylib', str(asm), '-o', str(args.out)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(args.out)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f'Game container: {args.out}')


if __name__ == '__main__':
    main()
