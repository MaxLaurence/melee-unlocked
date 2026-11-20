"""Package the Metal Mac host into MeleeUnlocked.app (ad-hoc signed, for local use).

The layout is the earlier Mac spike's (package_full.py): read-only assets in Contents/Resources, the
player's settings, saves, replays and shader cache in ~/.local/share/melee-unlocked (full_launch.h).
Files derived from the disc are never bundled; their deltas are, and the host rebuilds them from the
player's own ISO (runtime/host/sysgen.cpp).
"""
from pathlib import Path
import argparse, hashlib, json, plistlib, shutil, struct, subprocess, sys

ROOT = Path(__file__).resolve().parents[2]


def copy(src, dst):
    if not src.exists():
        raise SystemExit(f'Missing packaging input: {src}')
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_dir():
        shutil.copytree(src, dst, symlinks=False, dirs_exist_ok=True, ignore=shutil.ignore_patterns('.git*', 'README.md'))
    else:
        shutil.copy2(src, dst, follow_symlinks=True)


def shader_pak():
    """The CRT/slang shader presets as one file, exactly as tools/package_linux_release.py packs them."""
    shaders = ROOT / 'port/third_party/slang-shaders'
    if not (shaders / 'crt').is_dir():
        raise SystemExit(f'missing shader presets: {shaders}')
    entries = sorted((p.relative_to(shaders).as_posix(), p.read_bytes()) for p in shaders.rglob('*') if p.is_file())
    digest = hashlib.sha256()
    for entry, data in entries:
        digest.update(entry.encode('utf-8') + b'\0' + data)
    pak = bytearray(b'MUSHPAK1' + digest.hexdigest()[:16].encode('ascii') + struct.pack('<I', len(entries)))
    for entry, data in entries:
        raw = entry.encode('utf-8')
        pak += struct.pack('<H', len(raw)) + raw + struct.pack('<I', len(data)) + data
    return bytes(pak)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--game', type=Path, required=True, help='melee_game.so (its .snapexcl beside it)')
    parser.add_argument('--arch', choices=('arm64', 'x86_64'), default='arm64')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seed', type=Path, help='the shader seed (make_shader_seed.py --out), when there is one')
    args = parser.parse_args()
    for needed in (args.host, args.game, args.game.with_suffix('.snapexcl')):
        if not needed.is_file():
            raise SystemExit(f'Missing packaging input: {needed}')
    version = (ROOT / 'VERSION').read_text().strip()
    app = args.out
    staging = app.with_name(app.name + '.partial')
    if staging.exists():
        shutil.rmtree(staging)
    contents = staging / 'Contents'
    res = contents / 'Resources'
    copy(args.host, contents / 'MacOS/MeleeUnlocked')
    if args.arch == 'arm64':
        # Apple silicon runs code only from signed pages: the ELF goes in a signed container.
        subprocess.run([sys.executable, '-B', str(Path(__file__).with_name('game_container.py')), str(args.game),
                        str(res / 'melee_game.dylib')], check=True, stdout=subprocess.DEVNULL)
    else:
        copy(args.game, res / 'melee_game.so')
    copy(args.game.with_suffix('.snapexcl'), res / 'melee_game.snapexcl')
    for src, dst in (('port/slippi_sys', 'Sys'), ('port/slippi_sys_playback', 'SysPlayback'),
                     ('port/slippi_sys_general', 'slippi_sys_general'), ('lang', 'lang')):
        copy(ROOT / src, res / dst)
    coef = ROOT / 'port/slippi_sys/GC/dsp_coef.bin'
    for folder in ('Sys', 'SysPlayback', 'slippi_sys_general'):
        copy(coef, res / folder / 'GC/dsp_coef.bin')
    deltas = json.loads((ROOT / 'port/sys_deltas/manifest.json').read_text())
    disc_derived = {f['target'] for f in deltas['files']}
    for folder in ('Sys', 'SysPlayback', 'slippi_sys_general'):
        for name in disc_derived:
            (res / folder / 'GameFiles/GALE01' / name).unlink(missing_ok=True)
    copy(ROOT / 'port/sys_deltas/manifest.json', res / 'deltas/manifest.json')
    for f in deltas['files']:
        copy(ROOT / 'port/sys_deltas' / f['delta'], res / 'deltas' / f['delta'])
    (res / 'Shaders').mkdir(parents=True, exist_ok=True)
    (res / 'Shaders/crt-shaders.pak').write_bytes(shader_pak())
    copy(ROOT / 'port/app/bailey_cut.bmp', res / 'ui/bailey.bmp')
    # The pipelines a first launch prepares before its first frame (full_launch.h sets MELEE_SHADER_SEED).
    if args.seed and (args.seed / 'gx-metal-v1/recipes.bin').is_file():
        copy(args.seed, res / 'shader-seed')
    for source in ('gd_melee', 'mockups', 'menu_previews', 'vendor'):
        copy(ROOT / 'port/runtime/gx/ui_sources' / source / 'assets', res / 'ui_sources' / source)
    copy(ROOT / 'docs/training-mods.md', res / 'TrainingMods.md')
    licenses = [(ROOT / 'LICENSE', 'COPYING.txt'), (ROOT / 'NOTICE', 'NOTICE.txt'),
                (ROOT / 'port/third_party/dht/LICENCE', 'dht.txt'),
                (ROOT / 'port/third_party/monocypher/LICENCE.md', 'monocypher.md'),
                (ROOT / 'port/third_party/enet/LICENSE', 'enet.txt'),
                (ROOT / 'port/third_party/imgui/LICENSE.txt', 'imgui.txt'),
                (ROOT / 'port/third_party/stb/LICENSE', 'stb.txt'),
                (ROOT / 'port/third_party/zstd/LICENSE', 'zstd.txt'),
                (ROOT / 'port/third_party/bzip2/LICENSE', 'bzip2.txt'),
                (ROOT / 'port/third_party/lzma/lzma-sdk.txt', 'lzma-sdk.txt'),
                (ROOT / 'port/third_party/earcut/LICENSE', 'earcut.txt'),
                (ROOT / 'docs/licenses/dsp-coefficients.txt', 'dsp-coefficients.txt'),
                (ROOT / 'docs/licenses/slippilab-MIT.txt', 'slippilab.txt'),
                (ROOT / 'docs/licenses/slippi-ai-MIT.txt', 'phillip-slippi-ai-MIT.txt'),
                (ROOT / 'port/runtime/gx/ui_sources/gd_melee/assets/kit/SourceSans3-OFL.md', 'source-sans-3-OFL.md'),
                (ROOT / 'docs/third-party-ui-attribution.md', 'ui-attribution.md'),
                (ROOT / 'docs/third-party-training-mode-ce.txt', 'training-mode-ce.txt')]
    for src, dst in licenses:
        if src.is_file():
            copy(src, res / 'licenses' / dst)
    (res / 'README.txt').write_text(
        f'Melee Unlocked {version} for macOS (Metal renderer, {"Apple silicon" if args.arch == "arm64" else "x86-64"}).\n'
        'Settings, saves, replays and the shader cache live in ~/.local/share/melee-unlocked/User/Slippi.\n',
        encoding='utf-8')
    info = dict(CFBundleIdentifier='io.github.meleeunlocked.mac', CFBundleName='Melee Unlocked',
                CFBundleDisplayName='Melee Unlocked', CFBundleExecutable='MeleeUnlocked',
                CFBundlePackageType='APPL', CFBundleShortVersionString=version, CFBundleVersion=version,
                CFBundleInfoDictionaryVersion='6.0',
                # x86-64: AVX2 and FMA under Rosetta need macOS 15.
                LSMinimumSystemVersion='13.0' if args.arch == 'arm64' else '15.0', LSArchitecturePriority=[args.arch],
                LSRequiresNativeExecution=args.arch == 'arm64',
                NSHighResolutionCapable=True, NSPrincipalClass='NSApplication',
                NSSupportsAutomaticGraphicsSwitching=False,
                CFBundleDocumentTypes=[dict(CFBundleTypeName='GameCube disc image', CFBundleTypeRole='Viewer',
                                            CFBundleTypeExtensions=['iso', 'gcm', 'rvz'], LSHandlerRank='Alternate')])
    with (contents / 'Info.plist').open('wb') as f:
        plistlib.dump(info, f)
    assert not any(p.is_symlink() for p in staging.rglob('*')), 'Bundle contains symlinks'
    shipped = sorted(str(p.relative_to(staging)) for p in staging.rglob('*') if p.name in disc_derived)
    assert not shipped, f'Bundle carries disc-derived Sys files: {shipped}'
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', '--timestamp=none', str(staging)], check=True,
                   stdout=subprocess.DEVNULL)
    if app.exists():
        shutil.rmtree(app)
    staging.rename(app)
    print('App bundle:', app)


if __name__ == '__main__':
    main()
