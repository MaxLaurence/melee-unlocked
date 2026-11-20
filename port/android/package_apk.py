"""Package the Android host into MeleeUnlocked.apk with the SDK's own tools (javac, d8, aapt2, zipalign,
apksigner): no Gradle. Debug-signed with a key kept in the build folder, for installing with adb.

    python3 port/android/package_apk.py --main build-android/port/android/libmain.so \
        --game build-android-game/melee_game.so --deps build-android-deps/arm64 --out build-android/MeleeUnlocked.apk

The assets are the Mac bundle's Resources (port/mac/package_metal.py): Sys and its siblings without the
files derived from the disc (the host rebuilds them from the player's own ISO), the shader presets, the
settings panel's art, licences, and the game's sound ranges. The launcher unpacks them into the app's
files folder (LauncherActivity), the folder the host treats as its own (android_paths.h).
"""
from pathlib import Path
import argparse, json, os, re, shutil, subprocess, sys, zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'port/mac'))
from package_metal import copy, shader_pak  # noqa: E402  (the Mac packager's helpers)

SDK = Path(os.environ.get('ANDROID_HOME', '/opt/homebrew/share/android-commandlinetools'))
NDK = Path(os.environ.get('ANDROID_NDK_HOME', SDK / 'ndk/27.1.12297006'))
MIN_SDK, TARGET_SDK = 30, 34


def newest(folder):
    versions = sorted((p for p in folder.iterdir() if p.is_dir()), key=lambda p: [int(x) for x in re.findall(r'\d+', p.name)])
    if not versions:
        raise SystemExit(f'nothing in {folder}')
    return versions[-1]


def run(*command, **kw):
    subprocess.run([str(c) for c in command], check=True, **kw)


def stage_assets(res, game):
    for src, dst in (('port/slippi_sys', 'Sys'), ('port/slippi_sys_playback', 'SysPlayback'),
                     ('port/slippi_sys_general', 'slippi_sys_general'), ('lang', 'lang')):
        copy(ROOT / src, res / dst)
    coef = ROOT / 'port/slippi_sys/GC/dsp_coef.bin'
    for folder in ('Sys', 'SysPlayback', 'slippi_sys_general'):
        copy(coef, res / folder / 'GC/dsp_coef.bin')
    deltas = json.loads((ROOT / 'port/sys_deltas/manifest.json').read_text())
    for folder in ('Sys', 'SysPlayback', 'slippi_sys_general'):
        for f in deltas['files']:
            (res / folder / 'GameFiles/GALE01' / f['target']).unlink(missing_ok=True)
    copy(ROOT / 'port/sys_deltas/manifest.json', res / 'deltas/manifest.json')
    for f in deltas['files']:
        copy(ROOT / 'port/sys_deltas' / f['delta'], res / 'deltas' / f['delta'])
    (res / 'Shaders').mkdir(parents=True, exist_ok=True)
    (res / 'Shaders/crt-shaders.pak').write_bytes(shader_pak())
    copy(ROOT / 'port/app/bailey_cut.bmp', res / 'ui/bailey.bmp')
    for source in ('gd_melee', 'mockups', 'menu_previews', 'vendor'):
        copy(ROOT / 'port/runtime/gx/ui_sources' / source / 'assets', res / 'ui_sources' / source)
    copy(game.with_suffix('.snapexcl'), res / 'melee_game.snapexcl')
    for src, dst in ((ROOT / 'LICENSE', 'COPYING.txt'), (ROOT / 'NOTICE', 'NOTICE.txt'),
                     (ROOT / 'port/third_party/imgui/LICENSE.txt', 'imgui.txt'),
                     (ROOT / 'port/third_party/enet/LICENSE', 'enet.txt'),
                     (ROOT / 'port/third_party/monocypher/LICENCE.md', 'monocypher.md')):
        if src.is_file():
            copy(src, res / 'licenses' / dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--main', type=Path, required=True, help='libmain.so')
    ap.add_argument('--game', type=Path, required=True, help='melee_game.so (its .snapexcl beside it)')
    ap.add_argument('--deps', type=Path, required=True, help='build-deps.sh prefix (libSDL2.so)')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    for needed in (a.main, a.game, a.game.with_suffix('.snapexcl'), a.deps / 'lib/libSDL2.so'):
        if not needed.is_file():
            raise SystemExit(f'Missing packaging input: {needed}')
    tools = newest(SDK / 'build-tools')
    platform = SDK / f'platforms/android-{TARGET_SDK}/android.jar'
    if not platform.is_file():
        platform = newest(SDK / 'platforms') / 'android.jar'
    strip = next((NDK / 'toolchains/llvm/prebuilt').iterdir()) / 'bin/llvm-strip'
    version = (ROOT / 'VERSION').read_text().strip()
    parts = (re.findall(r'\d+', version) + ['0', '0', '0'])[:3]
    code = int(parts[0]) * 1000000 + int(parts[1]) * 1000 + int(parts[2])
    work = a.out.parent / 'apk'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    # Java: the app's activities and SDL's own Android glue (SDL2 2.32's android-project sources).
    sdl_java = ROOT / 'build-android-deps/src/SDL2/android-project/app/src/main/java'
    sources = [str(p) for p in (ROOT / 'port/android/java').rglob('*.java')] + [str(p) for p in sdl_java.rglob('*.java')]
    classes = work / 'classes'
    classes.mkdir()
    run('javac', '-nowarn', '-encoding', 'UTF-8', '-source', '11', '-target', '11', '-Xlint:-options',
        '-classpath', platform, '-d', classes, *sources, stdout=subprocess.DEVNULL)
    dex = work / 'dex'
    dex.mkdir()
    run(tools / 'd8', '--release', '--min-api', MIN_SDK, '--lib', platform, '--output', dex,
        *[str(p) for p in classes.rglob('*.class')])

    # Resources and manifest.
    compiled = work / 'res.zip'
    run(tools / 'aapt2', 'compile', '--dir', ROOT / 'port/android/res', '-o', compiled)
    assets = work / 'assets'
    stage_assets(assets / 'res', a.game)
    unsigned = work / 'unsigned.apk'
    run(tools / 'aapt2', 'link', '-I', platform, '--manifest', ROOT / 'port/android/AndroidManifest.xml',
        '--min-sdk-version', MIN_SDK, '--target-sdk-version', TARGET_SDK, '--version-code', code,
        '--version-name', version, '--debug-mode', '-A', assets, '-o', unsigned, compiled)   # debuggable: adb run-as

    # Code: classes.dex and the native libraries (the game as libmelee_game.so: only lib*.so is installed).
    with zipfile.ZipFile(unsigned, 'a', zipfile.ZIP_DEFLATED) as z:
        z.write(dex / 'classes.dex', 'classes.dex')
        libs = work / 'lib'
        libs.mkdir()
        run(strip, '--strip-unneeded', '-o', libs / 'libmain.so', a.main)
        run(strip, '--strip-unneeded', '-o', libs / 'libSDL2.so', a.deps / 'lib/libSDL2.so')
        shutil.copy2(a.game, libs / 'libmelee_game.so')   # unstripped: the loader reads its symbol table
        for lib in ('libSDL2.so', 'libmain.so', 'libmelee_game.so'):
            z.write(libs / lib, f'lib/arm64-v8a/{lib}')
    aligned = work / 'aligned.apk'
    run(tools / 'zipalign', '-f', '-p', '4', unsigned, aligned)

    # Debug signature, with a key kept beside the build so reinstalls keep the app's data.
    key = a.out.parent / 'debug.keystore'
    if not key.is_file():
        run('keytool', '-genkeypair', '-keystore', key, '-storepass', 'android', '-keypass', 'android', '-alias', 'debug',
            '-keyalg', 'RSA', '-keysize', '2048', '-validity', '10000', '-dname', 'CN=Melee Unlocked debug',
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run(tools / 'apksigner', 'sign', '--ks', key, '--ks-pass', 'pass:android', '--key-pass', 'pass:android',
        '--ks-key-alias', 'debug', '--out', a.out, aligned)
    print(f'APK: {a.out} ({a.out.stat().st_size / 1e6:.1f} MB, version {version} / {code})')


if __name__ == '__main__':
    main()
