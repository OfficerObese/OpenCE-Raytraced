"""Measures how fast the Windows builds draw: the OpenGL build and the D3D12
one, on the same level and view, one after the other.

Each build plays a level as the parity test's scenes do (debug.scene_test:
its cutscene skipped, the player taking no input, in a 1280x960 window), but
with nothing holding the frames back - vsync off, no frame limit, a frame for
every one the display could show (interpolation) - and the frames it draws
counted (debug.gpu_stats logs the frame's number every 60) over the seconds
after the player has control.

    python tools/test_d3d12_performance.py --game <maps folder's parent> [--level d40] [--seconds 20]
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_d3d12_parity as parity  # noqa: E402

ROOT = parity.ROOT
BUILDS = {
    "OpenGL": ROOT / "build" / "windows_gl" / "halo.exe",
    "D3D12": ROOT / "build" / "windows" / "halo.exe",
}
FRAME_LINE = re.compile(r"frame (\d+): (\d+) draws")


def measure(name, exe, game, level, seconds, settings):
    """frames a second the build draws, and its draws a frame"""
    data_root = ROOT / "build" / "performance" / name
    data_root.mkdir(parents=True, exist_ok=True)
    parity.link_maps(data_root, game)
    (data_root / "init.txt").write_text(f"map_name levels\\{level}\\{level}\n")
    environment = parity.scene_environment(data_root)
    environment.update({
        # (the player stands for the measurement and a little more)
        "HALO_SCENE_TEST": str(int((seconds + 3) * 30)),
        "HALO_INTERPOLATION": "true",
        "HALO_NO_VSYNC": "1",
        "HALO_MAX_FPS": "-1",
        "HALO_GPU_STATS": "1",
        "HALO_EXIT_AFTER": "180",
    })
    environment.update(settings)
    log = data_root / "stderr.txt"
    samples = []
    draws = []
    with open(log, "wb") as stderr:
        process = subprocess.Popen([str(exe)], cwd=data_root, env=environment, stdout=subprocess.DEVNULL, stderr=stderr)
        control = None
        read = 0
        while process.poll() is None:
            time.sleep(0.02)
            text = log.read_text(errors="replace")
            for line in text[read:].splitlines(keepends=True):
                if not line.endswith("\n"):
                    break
                read += len(line)
                now = time.monotonic()
                if control is None and "the player has control" in line:
                    control = now
                match = FRAME_LINE.search(line)
                if match and control is not None and now - control > 2.0 and now - control < 2.0 + seconds:
                    samples.append((now, int(match.group(1))))
                    draws.append(int(match.group(2)))
        process.wait()
    if len(samples) < 2:
        sys.exit(f"{name}: too few frames counted; its log is {log}")
    (first_time, first_frame), (last_time, last_frame) = samples[0], samples[-1]
    return (last_frame - first_frame) / (last_time - first_time), sum(draws) / len(draws)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", required=True, help="the folder that holds the game's maps folder")
    parser.add_argument("--level", default="d40")
    parser.add_argument("--seconds", type=float, default=20.0)
    parser.add_argument("--anti-aliasing", help="display.anti_aliasing for both builds")
    arguments = parser.parse_args()
    settings = {"HALO_ANTI_ALIASING": arguments.anti_aliasing} if arguments.anti_aliasing else {}
    results = {}
    for name, exe in BUILDS.items():
        if not exe.exists():
            sys.exit(f"{name}: {exe} is not built")
        results[name] = measure(name, exe, arguments.game, arguments.level, arguments.seconds, settings)
        print(f"{name}: {results[name][0]:.0f} frames a second ({results[name][1]:.0f} draws a frame)")
    print(f"D3D12 / OpenGL: {results['D3D12'][0] / results['OpenGL'][0]:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
