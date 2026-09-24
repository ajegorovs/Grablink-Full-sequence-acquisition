# Scout: language / UI-environment options for this app

Question: can the application be developed in Python + Qt, or something else? All numbers below are measured
on the Windows development PC against the installed MultiCam 6.19.4, not estimated.

## 1. What the vendor actually supports

From `docs/.../Content/02_What_s_New/Multicam_Release_Notes/Release_Specification/development-tools.htm`:

| Environment | Vendor position |
|---|---|
| C / C++ | 32- and 64-bit libs, Windows + Linux. The path this app uses today. |
| Python | **Official bindings**, shipped as a wheel inside the MultiCam package (`Python/MultiCam-6.19.4.5806-py2.py3-none-any.whl`), compatible with Python 2.7 and 3.x, Windows and Linux. Installed via `python -m pip install <whl>`. |
| .NET | Supported historically; the page says the **.NET assembly is deprecated and removed**, yet `Clr/x86_64/MultiCam_NetApi.dll` (v1.0.9477, product 6.19.4.5806) still ships. The vendor C#/VB samples do **not** use it — they hand-write `[DllImport("MultiCam.dll")]` P/Invoke wrappers, targeting .NET **2.0** WinForms. Documentation inconsistency worth knowing. |
| DirectShow | Picolo series only (analog). Not our board. |
| Serial DLL (clseremc) | Grablink Camera Link serial. |

No GUI toolkit is prescribed or shipped — the samples use MFC (Windows C++), SDL/OpenGL (Linux),
WinForms (C#) — so **Qt is a free choice**, and nothing in the SDK constrains the UI language.

The offline documentation set contains **no Python pages**: the shipped `python/GrablinkSnapshot/GrablinkSnapshot.py`
is the documentation.

## 2. What the Python binding is (read from the wheel)

`MultiCam/MC/MultiCam.py`, 540 lines, a thin `ctypes` wrapper (dependency: `six`). It exposes essentially the whole
C API this app uses: `McCreate/McCreateNm/McDelete`, all `Get/SetParam{Int,Str,Float,Inst,Ptr,Int64}` (+ `Nm` by-name
variants), `McRegisterCallback` (Python callables allowed), `McWaitSignal`, `McGetSignalInfo`, `McConvertSurface`,
plus the `MC_SIG_*`, `MC_SignalEnable`, `MC_SignalHandling` constants. Consequence: **zero-copy surface registration
(`MC_SurfaceAddr` / `MC_SurfacePitch` / `MC_SurfaceSize` / `MC_Cluster`) is reachable from Python**, so the
registered-RAM design from `SCOUT-multicam-ram-to-ssd.md` §5 is expressible without C++.

Design detail worth knowing: each Mc function is wrapped by its own `RLock` (`MultiCam.py:90-112`), i.e. the
serialisation is **per function, not process-wide**. Measured: a thread blocked 1.51 s inside `McWaitSignal` while
another thread's `McGetParamStr` returned in **0 ms** — cross-function concurrency is fine. (Calling the *same*
function from the callback thread and the UI thread still serialises; the lock is re-entrant per thread.)

## 3. Measured cost per frame (deadline at 340 fps = 2.94 ms)

| Operation | median | p99 | max |
|---|---|---|---|
| Mc call via the Python wrapper | 4.7 µs | — | — |
| Mc call, raw ctypes (no errcheck) | 2.7 µs | — | — |
| Python callback invocation (driver → Python, empty body) | 1.0 µs | — | — |
| copy one 2.22 MB frame: `ctypes.memmove` into a reused buffer | 0.109 ms | 0.316 ms | 0.755 ms |
| copy one frame: numpy `dst[:] = view` | 0.111 ms | 0.378 ms | 0.578 ms |
| copy one frame: `bytes(ctypes.string_at(...))` (allocates) | 0.471 ms | **1.400 ms** | **1.879 ms** |
| zero-copy numpy view of a frame (no copy at all) | 0.0005 ms | — | — |

Read: the Python interpreter itself is not the problem (µs-scale per call). The tail of an *allocating* per-frame
copy is (1.4–1.9 ms of a 2.94 ms budget). **Rule: allocate once, copy with `memmove`/numpy, or better, take
zero-copy views** — then per-frame Python work is 0.3 ms worst case (~11 % of budget).

Also verified from Python: `VirtualAlloc(MEM_RESERVE|MEM_COMMIT)` of a 446 MB block returns immediately (the repo's
"commit the whole capacity up front, never zero it" invariant is expressible in Python), and the driver reports for
our camera **`ImageSizeX = 2040`, `BufferPitch = 2040`** (no row padding — so one frame is exactly 2,219,520 B,
and at 340 fps the acquisition is 755 MB/s). `SurfaceCount` reads 0 before activation: MultiCam decides the surface
count at activation.

## 4. Where Python is viable and where it breaks

- **Fine:** control/parameter work, state machine, path handling, UI, and the post-capture dump (the binding's
  overhead is irrelevant there). RAM buffering with an app-owned committed block; copy at 0.1–0.4 ms/frame is
  affordable even at 340 fps.
- **Breaks:** per-frame *allocation* (measured 1.9 ms worst case) and any unbounded Python work between two surface
  hand-backs. With SNAPSHOT's one frame per surface and a 3-surface cluster, a ~9 ms stall loses a frame
  (`MC_SIG_CLUSTER_UNAVAILABLE`, visible as a `TimeCode` gap). **HFR mode is therefore close to mandatory for a
  Python app**: with `PhaseLength_Fr = N` the deadline becomes `N × 2.94 ms` (N = 255 → 750 ms of slack at 340 fps,
  and the OS interrupt rate drops from 340 Hz to ~1.3 Hz).
- **Concurrency shape that follows:** one dedicated thread for `McWaitSignal` + memory work (the vendor's
  `multicam_advanced_waitsignal` sample shows the wait-driven model), a save thread, and Qt on the main thread.
  Python callbacks are available and cheap (1 µs) but a callback body must still be allocation-free and must not
  touch Qt objects.
- **Gotchas hit while smoke-testing** (15 minutes, worth budgeting for): `Create(MC.CHANNEL)` with the numeric model
  constant produces a bad handle — use the sample's `Create('CHANNEL')`; `SignalEnable` by-ident on a channel with no
  `CamFile`/`Connector` set returns `MC_BAD_PARAMETER` (configure first, then set it — numeric `ON` = 5); passing a
  string where an int param is expected raises a raw `ctypes.ArgumentError`, not a `MultiCamError`. No type stubs, no
  binding docs, and the wheel is not installed on this box yet.

## 5. Dump target, re-measured (system SATA SSD, 316 GB free)

Three consecutive rounds, source fully resident, one 446 MB file, `fsync` at the end: **356 / 343 / 345 MB/s**.
200 BMP-shaped files (header + one frame each) with ordinary buffering: 0.19–0.23 s (~2 GB/s, i.e. absorbed by the
page cache, not media-bound). Earlier runs on the same drive measured 86 MB/s (single file) and 22 MB/s (200 files)
while it was still draining ~5 GB of previous test writes, and **38 MB/s for 200 files with `fsync` per file**.
So: this budget SATA SSD gives ~350 MB/s when idle and collapses when busy, and per-file flushing is the thing that
must not be in the dump path. Capture at 755 MB/s still cannot be written live; the RAM buffer stays necessary.
The secondary HDD volume (`D:`) is not a candidate (29 GB free).

## 5b. Alternative UI implementations — findings

### The current UI surface is small and bounded

Inventory (`resource.h`, `GrablinkSnapshot.rc`, message maps):

- Menu: `Go!`, `Stop!`, `Stop & Save`, `Set Output Folder…`, `View > Fit to Window` (a checked toggle), `Help > About`.
- Dialogs: about box, folder dialog (edit control + browse). Accelerator table, 2 icons.
- Status bar: one 400 px pane (`ID_INDICATOR_CHANNEL`) written by `CMainFrame::WriteStatusBar`.
- View: one canvas, `OnDraw` (≈137 lines) with `StretchDIBits` (fit) / `SetDIBitsToDevice` (1:1) on the window DC,
  a frame lease on the preview slot for the duration of the paint, and three `WM_APP` handlers.
- Command/message glue in the doc: `OnGo`/`OnStop`/`OnStopSave`/`OnSetFolder`/`OnViewFitWindow`/`OnUpdate…` plus
  `OnCaptureCompleteMessage`/`OnAcquisitionErrorMessage` ≈ 530 lines, of which much is orchestration rather than UI.

So a port has to re-create ~6 commands, 1 toggle, 1 status pane, 2 dialogs and one image canvas. Everything
expensive (allocation, frame ownership, saving, path claiming, counters) already lives in `core/`, which contains
no MFC and no MultiCam.

### Preview cost: the toolkit is not on the critical path (measured)

One 2040×1088 8-bit grey frame from a 256-grey palette, painted 1:1 and scaled to 1200×800, median of 100 reps:

| Path | 1:1 (2040×1088) | scaled to 1200×800 |
|---|---|---|
| GDI `StretchDIBits` (8 bpp + palette → 32 bpp DIB section, validated by reading the destination bits back) | 0.393 ms | 1.645 ms (max 3.49) |
| Qt `QPainter::drawImage` into an offscreen `QImage(Format_RGB32)` | 0.571 ms | 1.151 ms (max 1.37) |
| Qt, same but with `SmoothPixmapTransform` (anisotropic filtering) | — | 2.672 ms (max 3.07) |
| Qt `QImage::scaled` + 1:1 blit (if a scaled cache is kept) | — | 2.09 ms |

Both toolkits land in the same 0.4–2.7 ms band for a repaint — Qt is even slightly cheaper than GDI for the scaled
case. At the app's coalesced refresh (one outstanding preview message; the status text updates at ~2 Hz) that is
irrelevant to a 340 fps acquisition: the UI toolkit choice cannot make or break the capture path. (An earlier run
reported a GDI blit of 0.011 ms; that harness was reading back from a device bitmap and was not valid — the numbers
above are from a destination DIB section with a readback check, and supersede it.)

Related zero-copy finding: PySide6 wraps the numpy view of a published slot **without copying** — `QImage(buf, 2040,
1088, 2040, Format_Grayscale8)` read back the exact expected pixel values, with `bytesPerLine` equal to the source
pitch. So the existing "hold the frame lease for the duration of the paint" discipline (and `PreviewPublisher`'s
three-slot pool) maps 1:1 onto Qt's implicit sharing; a Qt view needs no extra frame copy.

### Acquisition and display in one Python process (measured together)

Thread A = one framed acquisition move per frame at a 350 fps cadence (2.22 MB `ctypes.memmove` into a reused
buffer, 1750 frames); thread B = QPainter scaled repaint of the same frame. Acquisition deadline = 2.86 ms/frame.

| Scenario | copy p50 | p99 | max | frames over 2.86 ms | painter |
|---|---|---|---|---|---|
| A alone | 0.135 ms | 0.314 ms | 0.452 ms | **0 / 1750** | — |
| A + painter at 30 fps | 0.127 ms | 0.431 ms | 1.852 ms | **0 / 1750** | 1.374 ms, 30.0/s |
| A + painter flat out | 0.137 ms | 1.541 ms | 1.676 ms | **0 / 1750** | 1.353 ms, 200/s |

Painting at 30 fps did not move the acquisition median at all and only lifted the tail (p99 0.31 → 0.43 ms); even at
200 paints/s every frame stayed inside the budget. (The harness ran at ~297 fps pace rather than 350 — `sleep`
granularity, not a work limit: the per-frame work is ~0.13 ms of a ~3.4 ms iteration.)

### Per-option findings

- **Qt 6 (C++).** Keeps `core/`, the C++ acquisition/save path and the whole tested surface; replaces only the
  MFC glue and resources, and `QImage(Format_Grayscale8)` covers the preview directly. Qt also gives the AGENDA's
  Phase 2 diagnostics (tables, live counters, plots) for free, which MFC would require hand-drawing. Build becomes
  CMake/MSVC without MFC or the `v143` MFC dependency. Licensing: LGPLv3 with dynamic linking (or commercial).
  Risk: the ~1k lines of UI glue and the resource rewrite; no performance risk (measured above).
- **PySide6 (Python).** The UI half is as cheap as Qt C++; the finding that matters is that the *capture* half is
  feasible too (`SCOUT` §3–4: 0.1–0.4 ms/frame for copies, µs for calls, and HFR turns the deadline into
  `PhaseLength_Fr × 2.94 ms`), with the caveats: the AGENTS.md invariants must be re-implemented, `core/`'s 112 tests
  would need re-expressing in Python (or `core/` kept as a DLL and called, which adds an interface but preserves the
  tests), and deployment becomes CPython + PySide6 packaging (PyInstaller) instead of a single MSVC binary.
- **WPF / WinUI 3 (C#).** P/Invoke against `MultiCam.dll` is exactly what the vendor's own C# samples do, so the
  binding is proven — but the vendor's .NET *assembly* is documented as deprecated, there is no .NET SDK on this box
  (only a 3.1 runtime), and `core/` + its tests would be rewritten in C#. Preview would need a copy into
  `WriteableBitmap.BackBuffer` (no zero-copy wrap of an 8bpp native block) — cheap at this refresh rate. Weakest fit.
- **Dear ImGui.** Fits the tiny command surface (6 items) and would be the lightest UI code, but it is immediate-mode
  GPU rendering: you own the texture upload, the folder dialog, the status bar and every widget, and you lose
  accessibility and native menu conventions. Reasonable only if the UI is meant to be a lightweight overlay on an
  otherwise headless tool.
- **Electron / web UI.** Adds a process boundary plus frame IPC for a single-machine operator tool, with no
  compensating benefit; the capture and dump paths would still be native.
- **Stay on MFC.** Zero port cost and the current work continues there; the cost is that UI work (diagnostics,
  fit/zoom, configuration dialogs) stays slow to build, and the toolkit is the part of this codebase with no tests.

## 6. Options, with the honest cost

| Option | What it keeps | Cost / risk |
|---|---|---|
| **C++ + Qt 6** (MSVC 2022, CMake) | `core/` unchanged (10k lines, Win32+std only, no MFC/MultiCam, 112 tests), the proven acquisition path, the whole SDK surface | Rewrite ~2.7k lines of MFC UI/glue (`GrablinkSnapshotDoc.cpp` 1364 + `View` 361 + `MainFrm`/`rc`/`BmpHelper`). Qt LGPLv3 (dynamic) or commercial. Lowest performance risk. |
| **Python + PySide6** | Nothing verbatim; `core/` would be re-expressed or wrapped as a DLL | PySide6 6.11.2 (`cp310-abi3`, requires >=3.10,<3.15 → works on the 3.11/3.12/3.14 installed here; LGPLv3 option). Capture is measured-feasible with HFR + zero-copy, but the AGENTS.md invariants have to be re-implemented in Python and a live hardware run has to prove frame counts. Fastest for tooling/analysis, most re-validation for production. |
| **C# / .NET** (WPF or WinForms) | Nothing verbatim | Samples P/Invoke `MultiCam.dll` directly (they don't use the deprecated vendor assembly). No .NET SDK on this box (only a 3.1 runtime). `core/` and its 112 tests would be re-written. Weakest fit. |
| **Rust / other FFI** | Nothing verbatim | No vendor support; same rewrite cost as C#, plus unfamiliar build/ABI work. Not recommended. |
| **Stay on MFC** | Everything | Where the current RAM→SSD work is landing; no UI modernization. |

## 7. Recommendation

- **Product UI modernization:** C++ + Qt, keep `core/` as-is. The seam already exists (AGENTS.md forbids MFC and
  MultiCam in `core/`), so the work is a UI/glue port plus a thin adapter around the document's state machine,
  not a rewrite of tested logic. Do this if the app must stay the production capture path.
- **Tooling, instrumented runs, parameter studies, offline analysis:** Python + the official binding, with PySide6
  only if a GUI is genuinely needed (scripts first). Measured to fit the 340 fps deadline under the rules above.
- **If Python must become the production capture path:** build it exactly like the numbers dictate — HFR mode with a
  large `PhaseLength_Fr`, one app-owned committed block, zero-copy numpy views, `memmove`/numpy copies only,
  wait-driven signalling in a dedicated thread, save worker on its own thread, commit up-front, no per-file flush —
  and prove it on hardware against the driver's own `TimeCode`/`Elapsed_Fr`/`OverrunCount`.

## 7b. Benchmark hygiene on this box

The grabber/camera can be in use by another application at any time (the parallel implementation effort runs on the
same machine). The measurements above opened the driver read-only, but two of the probes went further than that:
they created a channel and set `CamFile` / `Connector` / `ColorFormat` — calls that reconfigure the card's camera
interface and can fail or interfere while another process holds the camera. Do not repeat that without clearance.
Safe-by-construction probes: `McOpenDriver` + a `Configuration` parameter read, plus all in-process memory and disk
measurements. Anything that sets a channel parameter, and anything that activates a channel, needs the card to be
free and should be coordinated with whoever is running the app.

## 8. Must-verify on hardware before committing

1. A live HFR capture from Python: files written vs `Elapsed_Fr`/`TimeCode` vs `OverrunCount` (dropped-frame proof).
2. Whether the driver's Python callback (or the wait thread) stays timely while Qt repaints at 340 fps.
3. The in-surface slice layout for HFR (frame k offset inside a surface) — undocumented, and the dump depends on it.
4. Binding behaviour under activation: channel activation, `MC_SurfaceAddr` registration of a >2 GB multi-surface
   block, and teardown (`IDLE` → `McWaitSignal(MC_SIG_END_CHANNEL_ACTIVITY)` → delete).

## Evidence

- Binding source: `%LOCALAPPDATA%\Temp\mcwhl\MultiCam\MC\MultiCam.py` (unpacked from the installed wheel).
- Benchmarks (re-runnable): `%LOCALAPPDATA%\Temp\mcwhl\bench_mc_python.py`, `bench_latency.py`, `bench_dump.py`,
  `bench_lock5.py`. They open the driver read-only, read one Configuration parameter, and never activate the board.
- Vendor docs: `development-tools.htm`, `software-tools.htm` (paths as in `SCOUT-multicam-ram-to-ssd.md`).
- LOC split: `wc -l` over the MFC layer (2659) vs `core/` + `tests/` (9905).
