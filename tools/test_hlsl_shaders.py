"""Checks that every shader the game generates becomes HLSL that compiles.

The OpenGL build writes the GLSL its NV2A translators generate to a folder
(debug.gpu_dump_shaders); this converts each program with the D3D12
renderer's conversion (port/windows/d3d12/xgpu_hlsl.c) and compiles it with
D3DCompile, as the renderer does, through a checker built here
(port/windows/tests/hlsl_check.c). Windows only.

    python tools/test_hlsl_shaders.py [<corpus folder>...]

Without folders, the folders under build/shader_corpus. To make a corpus,
run the OpenGL build over the parity scenes with HALO_GPU_DUMP_SHADERS set:

    HALO_GPU_DUMP_SHADERS=build/shader_corpus/default python tools/test_d3d12_parity.py compare --game <...> --floor 0
"""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "port/windows/d3d12/xgpu_hlsl.c", ROOT / "port/windows/tests/hlsl_check.c"]
BUILD = ROOT / "build" / "hlsl_check"
CHECKER = BUILD / "hlsl_check.exe"


def build() -> None:
    BUILD.mkdir(parents=True, exist_ok=True)
    if CHECKER.exists() and all(CHECKER.stat().st_mtime >= source.stat().st_mtime
                                for source in SOURCES + [ROOT / "port/windows/d3d12/xgpu_hlsl.h"]):
        return
    subprocess.run(["clang", "--target=x86_64-pc-windows-msvc", "-std=gnu11", "-O2", "-g", "-gcodeview", "-Wall",
                    "-D_CRT_SECURE_NO_WARNINGS", f"-I{ROOT / 'port/windows/d3d12'}", "-fuse-ld=lld",
                    *map(str, SOURCES), "-o", str(CHECKER), "-ld3dcompiler"], check=True)


def main() -> int:
    if sys.platform != "win32":
        print("the HLSL check runs on Windows only")
        return 0
    folders = [Path(argument) for argument in sys.argv[1:]] or \
        sorted(path for path in (ROOT / "build" / "shader_corpus").glob("*") if path.is_dir())
    if not folders:
        print("no corpus: see this file's docstring")
        return 1
    build()
    return subprocess.run([str(CHECKER), str(BUILD / "failed"), *map(str, folders)]).returncode


if __name__ == "__main__":
    sys.exit(main())
