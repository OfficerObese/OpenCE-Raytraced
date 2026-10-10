# Windows

`ninja windows` compiles the game with clang for 32-bit x86 Windows. The
result is a native executable, `build/windows/halo.exe`, with the renderer
`halo_renderer.exe` (64-bit x86) and `SDL3.dll` next to it. You must build
on Windows.

The Windows build uses the platform layer of the Linux build
(`port/linux/src`). The sound, the input, the settings and the multiplayer
functions are the same as on Linux. Refer to
[port/linux/README.md](../linux/README.md). The graphics are drawn with
Direct3D 12 instead of OpenGL, and look the same: refer to "The Direct3D 12
renderer". The game needs Windows 10 or 11 and a GPU with Direct3D 12
(feature level 11_0).

## Requirements

You do not need the Xbox SDK.

- Visual Studio or the Visual Studio Build Tools, with the C++ workload.
  The build uses the 32-bit (x86) MSVC libraries for the game, the 64-bit
  (x64) MSVC libraries for the renderer, and a Windows 10 or 11 SDK. It does not use the MSVC compiler.
- LLVM (clang and lld), Python 3 and ninja in the `PATH`. For example,
  enter `scoop install llvm python ninja`.
- A network connection for the first build. `configure.py` downloads the
  Visual C++ development package of SDL 3.4.16 to
  `build/windows/third_party`.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja windows`.

`configure.py` generates the Windows build only on Windows.

The build uses all the instructions of the processor of the computer that
builds it (`-march=native`). Such a build does not always start on a
different computer. To make a build for other computers, enter
`python configure.py --portable`. Refer to the build options in the main
[README](../../README.md#build-options).

For a new optimization profile (`--pgo=train`), the build compiles the
profile runtime of LLVM for 32-bit x86 (`pgo/halo_profile_runtime.c`).
LLVM for Windows supplies this runtime only for x86-64.

## Start the game

Enter `build\windows\halo.exe`.

The game finds the game data as on Linux. Refer to "Start the game" in
[port/linux/README.md](../linux/README.md#start-the-game).

| Item | Location |
| --- | --- |
| Settings | `config.toml` next to `halo.exe` |
| Saved games | `%APPDATA%\halo`, or `paths.saves` in `config.toml` |
| Log | `debug.txt` in the data root (the folder that contains `maps\`) |
| Log of the port | The console. The release build has no console: `halo.log` next to `halo.exe` |
| Crash reports | `crashes\` next to `halo.exe`, until the game sends them |

## How the port operates

The game is 32-bit code, as on Linux, because its data contains 32-bit
pointers. The clang target `i686-pc-windows-msvc` gives the ABI of MSVC:
the structure layout, the 16-bit `wchar_t` and the calling conventions.
Thus the Windows build needs fewer changes than the Linux build.

The executable is large-address-aware, because the platform layer reserves
the Xbox memory at `0x80000000`.

### Headers

- The game and the platform layer use the Xbox SDK declarations of
  `port/include/xdk`, not the Windows SDK declarations with the same names.
  The compiler reads `port/include/xdk` before the Windows SDK.
- The C runtime is the static UCRT of Windows.
- `include/halo_windows_prefix.h` is the first header of each file. It gives
  Windows names to the Xbox SDK functions of the platform layer (for example
  `CreateFileA`, `ReadFile`, `Sleep`). Thus the code for Windows gets to
  Windows. The list of names is in `include/halo_windows_api_names.h`.
- `include/crt` changes some C runtime headers. The file functions of the
  game (`fopen`, `open`, `mkdir`) go through the translation of Xbox paths
  (`src/windows_crt.c`).
- `include/posix` declares the POSIX functions that the platform layer uses.

### Code for Windows

These files use only the Windows SDK:

| File | Contents |
| --- | --- |
| `src/win32_files.c`, `src/win32_net.c` | The file and socket functions of `port/linux/src/posix.h`. |
| `src/win32_posix.c` | The POSIX functions on Windows threads, critical sections, condition variables, `VirtualAlloc` and the performance counter. |
| `src/win32_memory_watch.c` | The write tracking of textures, with a vectored exception handler. |
| `src/win32_crash.c` | The crash reports. Refer to "Crash reports". |

`port.json` gives the Linux files that these files replace, and the Windows
libraries of the link.

### Inline functions

MSVC makes a COMDAT copy of an external `__inline` function where it does
not inline a call. Some files call such functions through a prototype.
clang inlines more calls. Thus a function can have no copy. The build
prevents this:

- `port/linux/game/msvc_comdat.c` gives one weak definition of each header
  inline function.
- A wrapper takes the address of each `__inline` function in a `.c` file.
  Thus the compiler makes an external copy (`inline_export_wrapper` in
  `tools/windows_build.py`).

MSVC gives file scope to a structure tag in a prototype. clang does not.
Thus the build also includes the declarations of the Linux build
(`build/windows/halo_msvc_tags.h`).

## The Direct3D 12 renderer

The Windows build draws with Direct3D 12 and uses no OpenGL.
`python configure.py --windows-renderer opengl` makes a build that draws
with OpenGL 4.5 in `halo.exe` instead, as the Linux build does. The tests
below compare the two.

The game stays 32-bit, because its data contains 32-bit pointers. The
renderer is a second program, `build/windows/halo_renderer.exe`, for 64-bit
x86. The game starts it and sends it what to draw:

| Part | Files | Contents |
| --- | --- | --- |
| The device of the game | `d3d12/d3d8_d3d12.c` | The Xbox Direct3D 8 device. It makes the same decisions as `port/linux/src/d3d8_gl.c` (the size of the screen, the render targets, the copy of the contiguous memory, the visibility tests), and sends messages instead of OpenGL calls. |
| The messages | `d3d12/xgpu_protocol.h`, `d3d12/xgpu_ring.c` | A ring of messages in memory that the two programs share. The game copies all the data that the GPU uses into the ring. The renderer does not read the memory of the game. |
| The shaders | `d3d12/xgpu_hlsl.c` | The GLSL of `port/linux/src/nv2a_vsh.c` and `nv2a_psh.c`, changed to HLSL. The renderer compiles the HLSL with `D3DCompile`. |
| The textures | `d3d12/xgpu_gl_textures.c` | The texture cache, the high-res HUD and text, and the art of the menus (`port/linux/src`) keep their twelve OpenGL texture calls. These calls send the textures to the renderer. |
| The window | `d3d12/win32_renderer_host.c`, `d3d12/win32_dcomp.cpp` | The game starts the renderer in a job object, so the renderer stops when the game stops. The window of the game shows a DirectComposition surface, and the renderer presents into it. Thus the window of the game keeps its input. |
| The renderer | `renderer/*.c` | Direct3D 12: the device, the textures and the render targets, the pipelines, the draws, the visibility tests, FXAA and SMAA (`port/third_party/smaa`), the presentation and the screenshots. |

The renderer writes its log to `halo_renderer.log` next to `halo.exe` (or
to the file that the environment variable `HALO_RENDERER_LOG` gives). The
setting `debug.d3d12_debug_layer` turns on the D3D12 debug layer, which
writes its errors and warnings to that log.

When something goes wrong with the renderer:

- **The GPU is lost.** The driver can reset the GPU after a hang, or the
  driver can be updated while the game runs. The renderer then makes its
  device again and restores all the textures, the render targets and the
  copy of the contiguous memory. It keeps a copy of each in its own memory
  for this. The frame being drawn is lost, and the game plays on. The log of
  the game says `the renderer lost the GPU and made it again`. The setting
  `debug.scene_test_device_reset` tests this.
- **The renderer crashes.** It writes `halo_renderer.dmp` (a minidump) next
  to its log. The game then shows the error and stops.
- **The renderer stops.** If the renderer exits, or draws no frame for 30
  seconds, the game shows the reason and stops.
- **The renderer cannot start.** The game shows the reason and stops.

Three tests compare the renderer with the OpenGL build. They need the game
data:

| Test | What it does |
| --- | --- |
| `python tools/test_d3d12_parity.py compare --game <folder>` | Plays fixed scenes (the main menu, five levels, each setting that changes the drawing: MSAA, SSAA, FXAA, SMAA, lighting for each pixel, and two scenes where the renderer loses its GPU device) and compares each frame with the frame of the OpenGL build (`record` makes those frames, with `--exe` an OpenGL build). The game skips the cutscenes, ignores the mouse and the keyboard, plays no sound and uses no internet play (`debug.scene_test`). |
| `python tools/test_hlsl_shaders.py` | Changes the GLSL of each shader that the game made (`debug.gpu_dump_shaders`) to HLSL and compiles it. |
| `python tools/test_d3d12_performance.py --game <folder>` | Counts the frames that each build draws each second, with no vsync and no limit. |

## Crash reports

A crash writes the faulting address and the calls that led to it to
`debug.txt` and to the log. The builds of the workflow (the releases)
also send a crash report to the Sentry project of the developers:

1. The game starts a second copy of `halo.exe` (`halo.exe --crash-report`).
   This copy writes a minidump of the game to `crashes\` next to
   `halo.exe`, and the game stops. A minidump contains the stacks and the
   registers of the threads, and the list of the modules. It does not
   contain the other memory of the game.
2. At the first crash, the copy asks the player whether to send crash
   reports. `crash_reports.upload` in `config.toml` keeps the answer:
   `"yes"` sends this report and all the reports after it, and `"no"` sends
   no reports and does not ask again. The answer `"no"` deletes the
   minidump.
3. The copy sends the minidump and a copy of `halo.log` to Sentry, then
   deletes them. If it cannot send them (for example, with no network
   connection), the game sends them when it starts the next time
   (`halo.exe --crash-upload`, in the background). `crashes\` keeps at
   most 8 reports.

Sentry finds the function names and the source lines of the minidump in
the PDBs (`halo.pdb`, `SDL3.pdb`). The `release` job of the workflow
uploads the PDBs and the executables of each build of main to Sentry. This
needs the `SENTRY_AUTH_TOKEN` secret of the repository: an organization
auth token from the settings of the Sentry organization (the token gives
the organization and its region). The `SENTRY_PROJECT` variable of the
repository can give the slug of the project; without it, the job uses the
id of the project in the DSN. For a personal token instead, also set the
`SENTRY_ORG` variable (the slug of the organization) and the `SENTRY_URL`
variable (`https://de.sentry.io`).

Each release also has the PDBs as `halo-windows-<configuration>-symbols.zip`.
`tools/symbolize_crash.py` uses them to add the function names and the
source lines to the crash lines of a `debug.txt` or a `halo.log`:

```
python tools/symbolize_crash.py debug.txt halo.exe
```

Give the `halo.exe` of the build that crashed, with its `halo.pdb` next to
it. The tool needs `llvm-symbolizer` (LLVM).

Builds without a build number (a local build, or a build of a branch other
than main) send no crash reports. To test the crash reports with such a
build, set the `HALO_CRASH_REPORTS_ANY_BUILD` environment variable.

The crash reports also include the crashes of `abort()` and of an invalid
argument to a function of the C runtime.

## Limits

- Bink video is not available. The game skips the movies.
- The game uses only the ANSI file functions. A path to the data or the
  saved games with characters that are not in the code page of the system
  does not always operate.
