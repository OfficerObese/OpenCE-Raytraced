# D3D12 renderer spike (M0)

A throwaway test, kept on the `d3d12-renderer-spike` branch. It checks the
riskiest assumption of the D3D12 renderer plan before the renderer is
written: that a 64-bit renderer process can present into the 32-bit game's
window, fed through shared memory, without taking the window's input.

| File | Contents |
| --- | --- |
| `spike_protocol.h` | The shared section: a header page, then a 64 MB ring of 16-byte-aligned messages. Fixed-width types, checked with `_Static_assert` |
| `spike_host.c` | The game's side, 32-bit (`i686-pc-windows-msvc`). Owns the window and input, makes the section, events and frame semaphore, and starts the renderer inside a job object (`KILL_ON_JOB_CLOSE`) |
| `spike_dcomp.cpp` | Presentation option B. The window shows a DirectComposition surface (`DCompositionCreateSurfaceHandle`) whose handle the renderer inherits. C++ only because `dcomp.h` has no C interface |
| `spike_renderer.c` | The renderer, 64-bit (`x86_64-pc-windows-msvc`), C with `COBJMACROS`. A D3D12 device, a swap chain made with `IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle`, frame latency 2 through the waitable object, and payloads copied through an upload heap to a default heap. Every length in the ring is bounds-checked |
| `build_spike.py` | Builds both into `build/spike` with clang and lld |

```
python port/windows/spike/build_spike.py
build\spike\spike_host.exe           (interactive: T sends 64 MB through the ring)
build\spike\spike_host.exe --test    (the check below; exit code 0 if it passed)
```

Set `SPIKE_D3D12_DEBUG=1` for the D3D12 debug layer.

## Results

Measured 2026-10-10 on Windows 11 (build 26300), an RTX 4070 SUPER with
driver 617.14, and a 2560x1440 display at 500 Hz:

| Check | Result |
| --- | --- |
| The renderer's frames reach the screen in the host's window | Yes. The screen pixel matches the cleared colour within 1/255 (two colours) |
| Frame pacing | 501 frames a second presented, which is the display's refresh rate. `Present(1, 0)` with the frame-latency waitable paces it |
| Window resize | `ResizeBuffers` after a resize message; the new size is presented |
| Input | The host's window still receives keys while the renderer presents into it. There is no child window, so the two processes' input queues are not joined |
| Ring throughput | 4 x 16 MB in 29 ms, about 2.2 GB/s. That includes the host's copy into the ring, the renderer's copy into the upload heap and the GPU copy. The last 16 MB took 4.9 ms from first byte to GPU copy done, 1.0 ms of it the GPU copy |
| Renderer lifetime | The renderer exits on the quit message; the job object ends it if the host dies |

Presentation option B (DirectComposition surface handle) works, so options
C (shared texture and a D3D11 presenter) and A (cross-process child window)
are not needed.

Things learned:

- `DCompositionCreateSurfaceHandle` returns `E_INVALIDARG` when its security
  attributes ask for an inheritable handle. Create the handle without them,
  then call `SetHandleInformation(HANDLE_FLAG_INHERIT)`.
- Clang finds the Visual Studio 2026 Build Tools and the Windows SDK
  10.0.26100 libraries for both targets without a developer command prompt.
- At 2.2 GB/s, the burst of first-use uploads after a level load (a few
  hundred MB) costs about 0.1 to 0.2 s. Mapping the ring section as an
  upload heap (`OpenExistingHeapFromFileMapping`, plan M5) would remove one
  of the two copies.

Not tested yet: Wine/Proton (vkd3d-proton), fullscreen through SDL, and the
debug layer's messages (they go to the debugger's output, not stderr).
