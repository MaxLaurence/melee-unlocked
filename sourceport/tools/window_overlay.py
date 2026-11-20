#!/usr/bin/env python3
"""Copy a source tree, moving fixed console addresses into the arm64 Mac's address window.

Code written for the console (Training Mode CE) names game data by its console address:
`(void *)0x803f58e0`, `(u8 *)0x804db6a0`. The native game keeps those addresses meaningful in MEM1,
which sits at 0x80000000 on x86-64 but at MU_WINDOW_BASE + 0x80000000 on arm64 macOS, where nothing
may be mapped below 4 GB (include/mu_disc.h). This rewrites each such cast of a MEM1-range literal to
the same address inside the window, as a constant expression, so static initialisers keep working.
Only the arm64 build uses the copy; the sources themselves are left alone.

usage: window_overlay.py SOURCE_DIR OUTPUT_DIR --base 0x10000000000
"""
import argparse
from pathlib import Path
import re
import shutil

# A cast to a pointer type applied directly to a literal in MEM1's range (0x80000000-0x81FFFFFF).
CAST = re.compile(r'\((?P<type>[A-Za-z_][^()]*?\*)\s*\)\s*(?P<addr>0x8[01][0-9a-fA-F]{6})(?P<suffix>[uUlL]*)\b')


def rewrite(text: str, base: int) -> tuple[str, int]:
    return CAST.subn(lambda m: f'(({m.group("type")})(0x{base:x}ull | {m.group("addr")}u))', text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--base', required=True, help='MU_WINDOW_BASE (4 GB aligned)')
    args = parser.parse_args()
    base = int(args.base, 0)
    if base & 0xFFFFFFFF:
        raise SystemExit('the window base must be 4 GB aligned')
    total = 0
    for src in sorted(p for p in args.source.rglob('*') if p.is_file()):
        dst = args.output / src.relative_to(args.source)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.suffix in ('.c', '.h'):
            text, n = rewrite(src.read_text(encoding='utf-8', errors='surrogateescape'), base)
            total += n
            data = text.encode('utf-8', errors='surrogateescape')
        else:
            data = src.read_bytes()
        if not dst.exists() or dst.read_bytes() != data:   # untouched files keep their timestamps
            dst.write_bytes(data)
            shutil.copystat(src, dst)
    print(f'window_overlay: {total} console addresses moved to the window at 0x{base:x}')


if __name__ == '__main__':
    main()
