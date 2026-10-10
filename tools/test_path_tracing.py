"""Checks the Windows D3D12 renderer's path tracer (display.path_tracing)
against the game's own drawing of the same scenes.

``depth_parity`` plays each level scene of tools/test_d3d12_parity.py with
path tracing on and the depth parity view (debug.path_tracing_view): the
renderer traces a ray from the camera through each pixel of the 3D view and
compares where it hits with the game's depth buffer, and logs the share of
the pixels that agree, every 30 frames. The pixels behind the game's own
surfaces are left out: they are behind the objects (the weapon, characters,
vehicles), which the path tracer's scene does not have yet. A scene passes
when the last count agrees on at least the floor (99%), and the D3D12 debug
layer (on for these runs) reported no error.

    python tools/test_path_tracing.py depth_parity --game <maps folder's parent> [--floor 99]

The scenes run as tools/test_d3d12_parity.py runs them (windowed, silent,
taking no input, with no internet play), in data roots of their own.
"""

import argparse
import concurrent.futures
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_d3d12_parity as parity  # noqa: E402

LEVELS = ["a30", "c20", "c40", "d20", "d40"]
FLOOR = 99.0
PARITY_LINE = re.compile(r"depth parity over \d+ frames: ([0-9.]+)% agree of the pixels judged \((.*)\)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("depth_parity",))
    parser.add_argument("--game", required=True, help="the folder that holds the game's maps folder")
    parser.add_argument("--exe", default=str(parity.EXE))
    parser.add_argument("--floor", type=float, default=FLOOR, help=f"the least share that agrees (default {FLOOR}%%)")
    parser.add_argument("--level", action="append", choices=LEVELS, help="only these levels")
    arguments = parser.parse_args()
    exe = Path(arguments.exe).resolve()

    names = []
    for level in arguments.level or LEVELS:
        name = f"{level}_pt_depth"
        parity.SCENES[name] = (level, parity.EXACT)
        parity.SETTINGS[name] = {"HALO_PATH_TRACING": "true", "HALO_PATH_TRACING_VIEW": "depth_parity",
                                 "HALO_D3D12_DEBUG_LAYER": "1"}
        names.append(name)
    with concurrent.futures.ThreadPoolExecutor(max_workers=parity.JOBS) as pool:
        runs = {name: pool.submit(parity.run_scene, exe, arguments.game, name) for name in names}
    failures = 0
    for name in names:
        try:
            runs[name].result()
        except parity.SceneError as error:
            print(error)
            failures += 1
            continue
        log = parity.DATA / name / "renderer.txt"
        text = log.read_text(errors="replace")
        counts = PARITY_LINE.findall(text)
        errors = text.count("D3D12 error")
        if not counts:
            print(f"{name}: FAILED: no depth parity was logged; its log is {log}")
            failures += 1
            continue
        agree, detail = float(counts[-1][0]), counts[-1][1]
        passed = agree >= arguments.floor and not errors
        failures += not passed
        print(f"{name}: {agree:.2f}% agree ({detail}){f', {errors} debug layer errors' if errors else ''} "
              f"{'passed' if passed else 'FAILED'}")
    print("passed" if not failures else f"FAILED: {failures} of {len(names)} scenes")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
