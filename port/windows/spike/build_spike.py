"""Builds the D3D12 renderer spike (README.md) into build/spike:
spike_host.exe (32-bit, as the game) and spike_renderer.exe (64-bit).

    python port/windows/spike/build_spike.py
"""

import subprocess
import sys
from pathlib import Path

SPIKE = Path(__file__).resolve().parent
ROOT = SPIKE.parents[2]
OUT = ROOT / "build" / "spike"

COMMON = ["-O2", "-g", "-gcodeview", "-Wall", "-Wno-unused-function", "-DUNICODE", "-D_UNICODE",
          "-D_CRT_SECURE_NO_WARNINGS"]
LINK = ["-fuse-ld=lld", "-g", "-Wl,/SUBSYSTEM:CONSOLE"]


def run(command):
    print(" ".join(str(part) for part in command))
    subprocess.run([str(part) for part in command], check=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    host = "--target=i686-pc-windows-msvc"
    renderer = "--target=x86_64-pc-windows-msvc"

    run(["clang", host, "-std=c11", *COMMON, "-c", SPIKE / "spike_host.c", "-o", OUT / "spike_host.o"])
    run(["clang++", host, "-std=c++17", *COMMON, "-c", SPIKE / "spike_dcomp.cpp", "-o", OUT / "spike_dcomp.o"])
    run(["clang", host, *LINK, OUT / "spike_host.o", OUT / "spike_dcomp.o", "-o", OUT / "spike_host.exe",
         "-luser32", "-lgdi32", "-ldcomp", "-lole32"])

    run(["clang", renderer, "-std=c11", *COMMON, "-c", SPIKE / "spike_renderer.c", "-o", OUT / "spike_renderer.o"])
    run(["clang", renderer, *LINK, OUT / "spike_renderer.o", "-o", OUT / "spike_renderer.exe",
         "-ld3d12", "-ldxgi", "-ldxguid", "-luser32"])
    print(f"built {OUT / 'spike_host.exe'} and {OUT / 'spike_renderer.exe'}")


if __name__ == "__main__":
    sys.exit(main())
