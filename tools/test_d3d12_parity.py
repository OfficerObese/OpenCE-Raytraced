"""Compares the Windows D3D12 renderer's frames with the OpenGL build's.

``record`` runs a build (the OpenGL one) over a fixed list of scenes and keeps
one frame of each in build/reference; ``compare`` runs another build over the
same scenes and compares its frames with those, by PSNR, with each 8 by 8
block of pixels averaged (what moved by a pixel between runs then barely
differs, while a wrong colour or a missing pass still does).

A level's scene is taken by debug.scene_test (port/linux/game/scene_test.c):
the level starts (init.txt's map_name), every cutscene is skipped, and a
fixed number of the game's ticks after the player has control, where no
enemy is near, the frame is saved and the game exits. Without interpolation
(display.interpolation false) that frame is the same moment in every run,
however fast the build draws. The main menu has no player: its frame is a
fixed frame number.

    python tools/test_d3d12_parity.py record  --game <maps folder's parent> [--exe build/windows/halo.exe]
    python tools/test_d3d12_parity.py compare --game <maps folder's parent> [--exe ...] [--floor <dB>]

Each scene runs in its own data root under build/reference_data, whose maps
folder links to the game's, so that nothing is written into the game's folder.
The game runs in a 1280x960 window and silent (audio.volume 0: the sound
code still runs, as in play), ignores the mouse, keyboard and gamepads, and
has internet play off.
"""

import argparse
import concurrent.futures
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / "build" / "reference"
DATA = ROOT / "build" / "reference_data"
EXE = ROOT / "build" / "windows" / "halo.exe"

WINDOW_SIZE = "1280x960"
# ticks (30 a second) after the player has control that a level's frame is saved
SCENE_TICKS = 60
# the main menu's frame number (30 frames a second without interpolation)
MENU_FRAME = 450
DOWNSCALE = 8
EXACT, MOVING = 45.0, 30.0

# name: (level, or None for the main menu; the least PSNR that passes, dB).
# Two runs of the OpenGL build (2026-10-10): c20 62, c40 and d40 identical,
# d20 56 dB, where a frame 5% too dark is 38-43 dB (EXACT); a30 44 dB
# (particles, the checkpoint text's fade), where a missing colour channel is
# 13-23 dB (MOVING). The menu's ship moves by the clock: two runs of one
# build agree (84 dB), but another build reaches the frame at another moment
# (the D3D12 build: 40 dB, the logo and text exact, the ship's hull and the
# stars a pixel apart), so it is MOVING too. Only levels whose opening
# cutscene can be skipped: not a10 (its waking waits for the player to look up
# and down, and the scenes take no input), nor a50, b30, b40 and c10, whose
# openings make the player wait 23 to 37 seconds.
SCENES = {
    "menu": (None, MOVING),
    "a30": ("a30", MOVING),
    "c20": ("c20", EXACT),
    "c40": ("c40", EXACT),
    "d20": ("d20", EXACT),
    "d40": ("d40", EXACT),
}
# scenes run at once
JOBS = 3


class SceneError(Exception):
    pass


def link_maps(data_root, game):
    maps = data_root / "maps"
    if maps.exists():
        return
    target = Path(game) / "maps"
    if not target.is_dir():
        sys.exit(f"{target} is not a folder")
    subprocess.run(["cmd", "/c", "mklink", "/J", str(maps), str(target)], check=True, stdout=subprocess.DEVNULL)


def run_scene(exe, game, name):
    """runs one scene; returns the path of its frame"""
    level, _ = SCENES[name]
    data_root = DATA / name
    data_root.mkdir(parents=True, exist_ok=True)
    link_maps(data_root, game)
    for folder in ("shots", "screenshots"):
        if (data_root / folder).exists():
            shutil.rmtree(data_root / folder)
    (data_root / "shots").mkdir()
    (data_root / "init.txt").write_text(f"map_name levels\\{level}\\{level}\n" if level else "")
    environment = dict(os.environ)
    environment.update({
        "HALO_DATA_ROOT": str(data_root),
        # (a save root of its own: the game's other drives, its map cache
        # among them, are folders there, which copies of the game running
        # together would otherwise write over each other's; and the player's
        # own saves are not touched)
        "HALO_SAVE_ROOT": str(data_root / "saves"),
        "HALO_UPDATE_ANSWER": "never",
        "HALO_FULLSCREEN": "false",
        "HALO_WINDOW_SIZE": WINDOW_SIZE,
        "HALO_INTERPOLATION": "false",
        "HALO_VOLUME": "0",
        "HALO_VOICE_VOLUME": "0",
        # (also what makes the game ignore the mouse, keyboard and gamepads,
        # in the main menu too, where it does nothing else)
        "HALO_SCENE_TEST": str(SCENE_TICKS),
        # no internet play: no invite on the clipboard joins a game, no
        # router is asked to forward a port, nothing is listed or announced
        "HALO_NET_ONLINE": "false",
        "HALO_NET_JOIN_FROM_CLIPBOARD": "false",
        "HALO_NET_ALLOW_UPNP": "false",
        "HALO_NET_PUBLIC_LOBBY": "false",
        "HALO_NET_HOST_PUBLIC": "false",
        "HALO_DISCORD_APPLICATION": "",
        # (the D3D12 build's renderer log, with the scene's)
        "HALO_RENDERER_LOG": str(data_root / "renderer.txt"),
    })
    if level:
        # (exit_after only in case the scene never comes)
        environment.update({"HALO_EXIT_AFTER": "120"})
    else:
        # (the game is ended once the frame is written: a first run builds
        # its map cache first, which takes a while)
        environment.update({"HALO_SCREENSHOT_DIR": str(data_root / "shots"),
                            "HALO_SCREENSHOT_EVERY": str(MENU_FRAME),
                            "HALO_EXIT_AFTER": "120"})
    log = data_root / "stderr.txt"
    menu_frame = data_root / "shots" / f"frame{MENU_FRAME:05d}.bmp"
    with open(log, "wb") as stderr:
        process = subprocess.Popen([str(exe)], cwd=data_root, env=environment, stdout=subprocess.DEVNULL,
                                   stderr=stderr)
        started = time.monotonic()
        while process.poll() is None:
            if time.monotonic() - started > 180:
                process.kill()
                raise SceneError(f"{name}: the game did not exit; its log is {log}")
            if not level and menu_frame.exists():
                # (written whole: its size stays)
                size = menu_frame.stat().st_size
                time.sleep(0.5)
                if size > 54 and menu_frame.stat().st_size == size:
                    process.kill()
                    process.wait()
                    break
            time.sleep(0.25)
        result = process
    if level:
        frames = sorted((data_root / "screenshots").glob("*.png")) if (data_root / "screenshots").exists() else []
    else:
        frames = [data_root / "shots" / f"frame{MENU_FRAME:05d}.bmp"]
    if not frames or not frames[-1].exists():
        raise SceneError(f"{name}: no frame was saved (exit code {result.returncode}); its log is {log}")
    return frames[-1]


def psnr(a, b):
    if a.size != b.size:
        return 0.0
    mean = sum(value * value for value in ImageStat.Stat(ImageChops.difference(a, b)).rms) / 3.0
    return float("inf") if mean == 0 else 10.0 * math.log10(255.0 * 255.0 / mean)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("record", "compare"))
    parser.add_argument("--game", required=True, help="the folder that holds the game's maps folder")
    parser.add_argument("--exe", default=str(EXE))
    parser.add_argument("--floor", type=float, help="the least PSNR (dB) that passes, for every scene")
    parser.add_argument("--scene", action="append", choices=sorted(SCENES), help="only these scenes")
    parser.add_argument("--jobs", type=int, default=JOBS, help=f"scenes run at once (default {JOBS})")
    arguments = parser.parse_args()
    exe = Path(arguments.exe).resolve()
    scenes = arguments.scene or list(SCENES)
    REFERENCE.mkdir(parents=True, exist_ok=True)

    # (each scene its own data root and copy of the game; their frames are
    # counted in the game's ticks, so running together changes none)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, arguments.jobs)) as pool:
        runs = {name: pool.submit(run_scene, exe, arguments.game, name) for name in scenes}
    failures = 0
    for name in scenes:
        try:
            frame = runs[name].result()
        except SceneError as error:
            print(error)
            failures += 1
            continue
        reference = REFERENCE / f"{name}.png"
        if arguments.mode == "record":
            Image.open(frame).convert("RGB").save(reference)
            print(f"{name}: recorded {frame}")
            continue
        if not reference.exists():
            sys.exit(f"{name}: no reference; run record first")
        expected = Image.open(reference).convert("RGB")
        drawn = Image.open(frame).convert("RGB")
        full = psnr(expected, drawn)
        value = psnr(expected.reduce(DOWNSCALE), drawn.reduce(DOWNSCALE)) if expected.size == drawn.size else 0.0
        floor = SCENES[name][1] if arguments.floor is None else arguments.floor
        passed = value >= floor
        failures += not passed
        print(f"{name}: {value:.1f} dB at 1/{DOWNSCALE} scale ({full:.1f} dB full size) "
              f"{'passed' if passed else 'FAILED'} (floor {floor:.0f}, {frame})")
    if arguments.mode == "compare":
        print("passed" if not failures else f"FAILED: {failures} of {len(scenes)} scenes")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
