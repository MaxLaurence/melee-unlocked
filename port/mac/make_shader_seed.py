#!/usr/bin/env python3
# Records the GX pipelines the Mac app builds before a first launch's first frame (Resources/shader-seed).
# SPDX-License-Identifier: GPL-2.0-or-later
"""Plays hidden runs of the built app and merges the pipeline recipes they record into a seed.

    python3 port/mac/make_shader_seed.py --app build-mac-arm64/MeleeUnlocked.app --iso melee.iso --out build-mac-gen/shader-seed

The Metal renderer records every pipeline a run builds (renderer_metal.cpp, <cache>/gx-metal-v1/recipes.bin)
and the next start builds them all before the game runs. A first launch has no recipes of its own,
so each new effect was skipped for the frames its shaders took to build. The seed is those recipes
from a CPU match on every stage (all 26 characters among them, items on, through the results screen)
and from the menus, Classic and the online screens, so a first launch prepares them up front.

Runs are hidden, sound off, with no Slippi account (HOME points at an empty folder) and no adapter.
"""
import argparse
import concurrent.futures
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = "gx-metal-v1"
MAGIC = 0x3150524C544D5847   # renderer_metal.cpp RECIPE_MAGIC
RECIPE_BYTES = 4 + 256 * 4 + 0x58 * 4   # ProgramRecipe: components, BPMemory, XF registers
LIMIT = 16384


def hash_bytes(data):
    """gx::hash_bytes (gx_texture.cpp)."""
    mask = (1 << 64) - 1
    h = 0xcbf29ce484222325
    whole = len(data) // 8 * 8
    for (v,) in struct.iter_unpack("<Q", data[:whole]):
        h = ((h ^ v) * 0x100000001b3) & mask
        h ^= h >> 29
    for b in data[whole:]:
        h = ((h ^ b) * 0x100000001b3) & mask
    return h


def read_recipes(path):
    try:
        data = path.read_bytes()
    except OSError:
        return []
    if len(data) < 24:
        return []
    magic, count, digest = struct.unpack_from("<QQQ", data)
    body = data[24:]
    if magic != MAGIC or len(body) != count * RECIPE_BYTES or hash_bytes(body) != digest:
        print(f"  ignored {path}: not a recipe file of this version", file=sys.stderr)
        return []
    return [body[i * RECIPE_BYTES:(i + 1) * RECIPE_BYTES] for i in range(count)]


def write_recipes(path, records):
    body = b"".join(records)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack("<QQQ", MAGIC, len(records), hash_bytes(body)) + body)


def runs():
    """(name, arguments) for every run of the seed."""
    out = []
    # A CPU match on every stage (external stage ids 2 Fountain of Dreams .. 32 Final Destination), four
    # level 9 CPUs each, the 26 characters in turn. parity_vs.txt walks the VS menus; --match decides the
    # stage and fighters when the match starts. 9000 frames reach the results screen of the 2 minute match.
    # 21 and 26 are no VS stages: the x86 and arm64 builds alike crash on 21 and never start 26.
    for i, stage in enumerate(s for s in range(2, 33) if s not in (21, 26)):
        players = ":".join(f"{(4 * i + k) % 26}/c9/x{(i + k) % 4}" for k in range(4))
        out.append((f"stage{stage:02d}", ["--vanilla-game", "--script", "port/scripts/parity_vs.txt",
                                          "--match", f"{stage}:{players}", "--frames", "9000"]))
    # The menus as a player sees them (Slippi's), Classic, the online screens and the plain game's menus.
    for script in ("menu_probe", "vs_match", "vs_probe", "to_css", "online_unranked", "online_teams",
                   "classic_fox", "classic_run", "native_unlocked_vs", "video_background_probe"):
        out.append((script, ["--script", f"port/scripts/{script}.txt", "--frames", "6000"]))
    for script in ("classic_fox", "to_css", "parity_vs_items_off"):
        out.append((f"vanilla_{script}", ["--vanilla-game", "--script", f"port/scripts/{script}.txt", "--frames", "6000"]))
    return out


def play(exe, iso, work, name, args, timeout):
    folder = work / name
    home = work / "home"
    home.mkdir(parents=True, exist_ok=True)
    cache = folder / "cache"
    env = dict(os.environ, HOME=str(home), MELEE_NO_GC_ADAPTER="1")
    env.pop("MELEE_SHADER_SEED", None)
    env["MELEE_SHADER_SEED"] = ""   # record only what this run draws
    command = [str(exe), "--iso", str(iso), "--hidden", "--fast", "--volume", "0", "--time-base", "0",
               "--rng-seed", "0x5eed5eed", "--user-dir", str(folder / "user"), "--shader-cache", str(cache),
               "--log-file", str(folder / "melee.log")] + args
    command = [str(ROOT / a) if a.startswith("port/scripts/") else a for a in command]
    try:
        result = subprocess.run(command, cwd=folder.parent, env=env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=timeout)
        code = result.returncode
    except subprocess.TimeoutExpired:
        code = "timeout"
    recipes = read_recipes(cache / CACHE / "recipes.bin")
    return name, code, recipes, cache / CACHE


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", required=True, help="the built MeleeUnlocked.app")
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True, help="the seed folder (gets gx-metal-v1/recipes.bin)")
    ap.add_argument("--jobs", type=int, default=max(2, min(6, (os.cpu_count() or 4) // 3)))
    ap.add_argument("--timeout", type=float, default=600)
    ap.add_argument("--keep", help="keep the runs' folders here instead of a temporary folder")
    ap.add_argument("--only", help="comma-separated run names (for a quick check)")
    a = ap.parse_args()
    exe = Path(a.app).resolve() / "Contents/MacOS/MeleeUnlocked"
    iso = Path(a.iso).resolve()
    if not exe.is_file():
        sys.exit(f"no app at {a.app}")
    plan = runs()
    if a.only:
        wanted = set(a.only.split(","))
        plan = [r for r in plan if r[0] in wanted]
    work = Path(a.keep).resolve() if a.keep else Path(tempfile.mkdtemp(prefix="melee-seed-"))
    work.mkdir(parents=True, exist_ok=True)
    print(f"{len(plan)} runs, {a.jobs} at a time, in {work}")
    merged, seen = [], set()
    shaders = {}
    failed = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        futures = [pool.submit(play, exe, iso, work, name, args, a.timeout) for name, args in plan]
        for future in concurrent.futures.as_completed(futures):
            name, code, recipes, cache = future.result()
            added = 0
            for r in recipes:
                key = hash_bytes(r)
                if key not in seen and len(merged) < LIMIT:
                    seen.add(key)
                    merged.append(r)
                    added += 1
            for msl in cache.glob("*.msl"):
                shaders.setdefault(msl.name, msl)
            note = "" if code == 0 else f" (exit {code})"
            if code != 0 or not recipes:
                failed.append(name)
            print(f"  {name}: {len(recipes)} recipes, {added} new{note}", flush=True)
    out = Path(a.out).resolve() / CACHE
    if out.exists():
        shutil.rmtree(out)
    write_recipes(out / "recipes.bin", merged)
    # The translated shaders too: a first launch then only compiles them (no HLSL to MSL translation).
    for name, path in shaders.items():
        shutil.copyfile(path, out / name)
    # One recipe per pipeline: the app builds the merged seed the way a first launch does and writes
    # back the recipes it used (MELEE_SHADER_SEED_COMPACT, renderer_metal.cpp). That also proves every
    # pipeline in the seed builds, and times a first launch.
    compact = work / "compact.bin"
    folder = work / "compact"
    env = dict(os.environ, HOME=str(work / "home"), MELEE_NO_GC_ADAPTER="1",
               MELEE_SHADER_SEED=str(out.parent), MELEE_SHADER_SEED_COMPACT=str(compact))
    subprocess.run([str(exe), "--iso", str(iso), "--hidden", "--volume", "0", "--frames", "30",
                    "--user-dir", str(folder / "user"), "--shader-cache", str(folder / "cache"),
                    "--log-file", str(folder / "melee.log")],
                   cwd=work, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=a.timeout)
    pipelines = read_recipes(compact)
    if not pipelines:
        sys.exit(f"the compaction run wrote no recipes (see {folder / 'melee.log'})")
    for line in (folder / "melee.log").read_text(errors="replace").splitlines():
        if "prewarmed" in line or "failed to build" in line:
            print(f"  {line}")
    write_recipes(out / "recipes.bin", pipelines)
    print(f"seed: {len(pipelines)} pipelines (from {len(merged)} recorded recipes), {len(shaders)} shaders in {out}")
    if failed:
        print(f"runs that ended early or recorded nothing: {', '.join(sorted(failed))}")
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if merged else 1


if __name__ == "__main__":
    raise SystemExit(main())
