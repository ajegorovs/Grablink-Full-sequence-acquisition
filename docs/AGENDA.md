# Future work plan

## Current baseline

The hardware-independent refactor is complete locally:

- pitch-aware capture into a precommitted RAM block;
- zero-copy handoff to an asynchronous saver;
- corrected 8-bit BMP generation and checked file I/O;
- callback-to-UI dispatch through cached-window `WM_APP+n` messages;
- retry-safe handling when a save cannot start;
- hardware-free test coverage for allocation, frame copying, ownership transfer, BMP output, cancellation, failures, worker exceptions, run-folder claiming, and modal scopes.

Verified on the connected Basler camera and MultiCam runtime:

- application builds in `Debug|x64` and `Release|x64`;
- 121 core tests pass in both configurations;
- application-owned preview is correct at approximately 351 FPS;
- a 200-frame capture saved exactly `Image_00000.bmp` through `Image_00199.bmp`;
- sampled first/middle/last BMPs are valid 2040 x 1088, 8-bit, top-down images with matching declared and actual sizes;
- closing the application while the live callback stream is active is immediate and leaves no process running;
- consecutive captures claim separate timestamped run folders and never overwrite each other;
- a 2000-frame capacity was run live and manual `Stop & Save` wrote partial
  sequences (652 and 1028 frames) with no gaps and valid headers;
- modal dialogs stay visible and interactive while the live preview stream runs;
- fit-to-window scaling shows no grid artifact at the native image size.

## Repository state

- Git repository on branch `master`, remote
  `https://github.com/ajegorovs/Grablink-Full-sequence-acquisition` (public).
- Layout: `GrablinkSnapshot.sln` at the root with `Debug|x64` and `Release|x64`
  building both projects, application sources in `src/`, hardware-independent units
  in `core/`, tests in `tests/`, notes in `docs/`.
- The refactor is committed in reviewable groups and pushed to the remote. The
  pre-restart history of the remote is preserved as tag `pre-restart-2023`
  (`c0e478d`).
- A recovery snapshot of the pre-restructure working tree (git bundle, tracked-tree
  patch, full source archive, SHA-256 checksums) was taken before the layout change
  and lives outside the repository in `..\_backup_20260924\`.

## Phase 1 — Camera-connected validation

Do this first when the camera is available.

1. Configure the Basler camera in Pylon Viewer and verify the selected tap/transport settings.
2. Confirm the stream works in the Euresys/MultiCam tooling with CAM file `acA2000-340km_P340SC`.
3. Start this application and record:
   - `MC_ImageSizeX` and `MC_ImageSizeY`;
   - `MC_BufferPitch`;
   - reported FPS;
   - requested capture capacity and committed bytes.
4. Capture a short sequence and compare several saved BMPs with the live image:
   - orientation;
   - row alignment/pitch;
   - dimensions;
   - first, middle, and last frame integrity.
5. Test all operator flows:
   - Go → automatic full-buffer save;
   - Go → Stop (discard);
   - Go → Stop & Save;
   - repeated capture after save;
   - invalid/unavailable output folder followed by retry;
   - close application during capture and during save.
6. Run a sustained 300–350 FPS capture and measure dropped frames, callback failures, callback-copy time, and save throughput.

Acceptance: no acquisition failures attributable to callback latency, exact frame count, valid BMP sequence, responsive UI during save, and clean shutdown.

## Phase 2 — Instrumentation and diagnostics

This is the largest remaining item. `core/CaptureStats` is already implemented and
covered by 13 tests, and it is registered in both projects, but **no production
code calls it** — the counters exist and nothing feeds or reads them.

- Wire `CaptureStats` into the callback and the save path: surfaces received,
  frames stored, frames rejected, acquisition failures, files failed.
- Settle the semantics first: the only current refusal also stops capture, so it is
  arguably an acquisition failure rather than a recoverable rejection; define
  whether surfaces received counts signals or stored frames, and whether the
  preview-only stream counts while not capturing.
- Measure callback duration without logging per frame; the application has no clock
  on that path today, so the time source has to be chosen and injected.
- Record capture duration, effective FPS, save duration, and disk throughput.
- Include the actual dimensions, pitch, capacity, and committed bytes in a
  diagnostics panel or capture summary.
- Check every relevant `Mc*` return code and translate failures into UI-thread error messages.

## Phase 3 — Remaining lifecycle risks

Completed:

- application-owned three-slot preview publication replaces drawing from the grabber-owned surface;
- a tested callback admission/drain barrier protects channel and synchronization-object teardown;
- a lock-free refresh coalescer limits the UI queue to one outstanding preview message;
- the unused `_bStopped` state has been removed.

## Phase 4 — Configuration and operator safety

Completed and camera-validated:

- Each saved capture claims an atomic timestamped subfolder under the chosen
  output folder; collisions receive numeric suffixes and retries preserve their
  pending frames and run-folder ownership.
- Two consecutive live 200-frame captures created separate run folders with
  complete `Image_00000.bmp`–`Image_00199.bmp` sequences. Sampled first, middle,
  and last files were valid 2040 x 1088, 8-bit BMPs; the first run remained
  unchanged after the second.
- The number of recorded frames is settable from the UI. A `Capture Settings...`
  command opens a dialog showing the committed size recomputed live as the count
  is edited, refuses while capturing, saving or holding a pending snapshot,
  persists the count through the MFC profile API, and `Go!` re-allocates when the
  buffer capacity does not match the configured count. The operator ran a
  2000-frame capacity and confirmed the dialog, the memory label, persistence and
  a visibly longer capture.

Completed and live-validated:

- Fit-to-window versus 1:1 preview is exposed as a checked `View` command, and the
  folder chooser resolves the configured output folder to an absolute path and
  preselects it when it exists as a directory.
- Fit-to-window scaling showed banding, dark-gray isolines, and — when the window
  was resized close to the image's native 2040 x 1088, i.e. near a 1:1 scale — a
  fine grey grid of roughly 10 px period whose cells were taller than wide. That
  periodic grid is the signature of a nearest-neighbour stretch: rows and columns
  are dropped at a regular interval, with period 1/|1 - scale|, and the
  taller-than-wide cells follow from the two axes being scaled independently
  because the destination is the full client rect. `GrablinkSnapshotView` now sets
  `HALFTONE` for the scaled `StretchDIBits` call only, restoring the previous
  stretch mode and brush origin through an RAII guard; the 1:1
  `SetDIBitsToDevice` path is unchanged. The operator confirmed the grid is gone.
- The status bar stopped refreshing in fit mode because its throttle counted
  repaints instead of elapsed time, so the update rate fell as the repaint got
  more expensive. It is now time-based at approximately 2 Hz, independent of
  paint cost.
- With the live preview running, an application modal dialog could end up
  foreground, onscreen, enabled and non-hung while `WS_VISIBLE` was false behind a
  disabled owner, leaving the application unusable and requiring a kill.
  Reproduced for both `Set Output Folder...` and `Help > About`, which placed the
  cause in the continuous preview-refresh post/invalidate stream entering the
  modal message loop rather than in the folder dialog's initialization callback
  (changing that callback from `SendMessage` to `PostMessage` did not change it).
  `core/ModalScopeCounter` now holds a counted, nestable modal scope and the driver
  callback suppresses only the preview-refresh post while a scope is held; preview
  pixels still publish every frame and the capture-complete and acquisition-error
  posts are untouched. The document's 22 live `MessageBox` call sites now go
  through one helper that holds a scope for the span of the call. Operator
  confirmed the About box, the folder chooser and the message boxes all appear
  visible and interactive, repeatedly, with the preview resuming on close.

Open:

- The message boxes are still shown with a `NULL` owner, so they are modal to the
  thread rather than to the application: the frame is not disabled, the box can
  end up behind the frame, and a command can be clicked behind it. Passing the
  main frame handle is the correct fix and is a one-line change at the helper,
  but it is an operator-visible behaviour change and is deliberately not made yet.
- The nested case (a modal opened from inside another modal) is covered by the
  counter's tests but not by a live run, because the UI does not currently
  produce one.
- Persist the chosen output folder. Only the frame count is persisted today, so
  the folder reverts to the built-in default on every restart.
- Make the default output folder absolute. It is the relative name `SavedImages`,
  so it resolves against whatever working directory the process was started in:
  launching the executable from `x64\Debug` wrote every capture into
  `x64\Debug\SavedImages` instead of the repository folder, and the operator had
  to discover that from the filesystem. A default anchored to the executable's
  location, or to a real user folder, would remove the surprise.
- Move the CAM file, connector and exposure out of the constructor into persistent configuration.
- Refuse unsupported Win32 capture configurations; the production high-capacity path is x64.
- Add an explicit “discard pending capture” action instead of relying on File > New/Ctrl+N.
- Consider changing the project to Unicode end-to-end; the save core already supports UTF-16 paths.

## Phase 5 — Project maintenance

- ~~Add `tests\GrablinkSnapshotCoreTests.vcxproj` to the solution~~ — done: the solution
  builds the application and the tests in `Debug|x64` and `Release|x64`.
- ~~Remove deprecated `/Gm` (`MinimalRebuild`) from the Debug configuration~~ — done.
- ~~Update or archive `TASK_v1.md`~~ — moved to `docs/TASK_v1.md` as a historical record.
- Retire `BmpHelper`; `core/BmpWriter` is the tested writer. `BmpHelper::Init8bppHeaders()`
  is still called from `CGrablinkSnapshotDoc::OnNewDocument()`, so this is a code change, not
  a deletion: move the palette/header setup into `core/BmpWriter` first.
- Add CI if a Windows runner with MFC and the MultiCam SDK is available.

## Phase 6 — SDK and camera configuration investigation

- Install and validate the downloaded MultiCam 6.19.5 package on a controlled machine before changing the production runtime; the currently installed runtime is 6.19.4.5806.
- Compare release notes, supported grabber firmware, CAM-file compatibility, callback behavior, and migration requirements before upgrading.
- Determine whether the Basler camera exposes its parameters through GenApi/GenICam over the current transport and whether MultiCam can issue those writes through the frame grabber.
- If MultiCam cannot configure the camera, prototype a preflight helper using the Basler pylon API or `PylonC` to load a known camera configuration before opening the MultiCam channel.
- Verify ownership constraints: pylon configuration must complete and release the camera before MultiCam acquisition starts unless the transport explicitly permits shared control.
- Read back and log the effective camera parameters after configuration; never assume a successful write, because the camera loses settings when power is removed.
- Keep manual Pylon Viewer configuration as the documented fallback until the scripted path is proven with this camera/card combination.

## Phase 7 — UI toolkit decision (open)

The scout in `docs/SCOUT-language-and-ui-options.md` measured the options on this
machine. Recorded position:

- The preview and acquisition cost is not the deciding factor: GDI and Qt both
  repaint a frame in the 0.4–2.7 ms band, and the refresh is already coalesced.
- The MFC surface to port is small (~2.7k lines: 6 commands, 1 toggle, 1 status
  pane, 2 dialogs, 1 canvas). All tested logic already lives in `core/`, which is
  MFC- and MultiCam-free, so a port replaces glue rather than behaviour.
- The modal-dialog defect was a message-flood/coalescing problem, not an MFC
  problem: any toolkit's modal loop that keeps dispatching a 351 Hz post stream
  needs the same suppression discipline. A port would not have fixed it.
- Qt 6 (C++) is the only option that keeps `core/` and its tests verbatim; it
  would also give the Phase 2 diagnostics UI that MFC requires hand-drawing.
- Python + the official binding is measured-feasible for capture, but it means
  re-expressing the AGENTS.md invariants and re-proving frame counts on
  hardware. Keep it for offline tooling and parameter studies, not the product
  capture path.
- C#/.NET and Rust are the weakest fits and are not recommended.

Revisit the port when the diagnostics/configuration UI is actually specified;
until then keep the MFC capture path and keep moving UI-independent decisions
into `core/` so the port stays cheap.

## Deferred by design

Do not replace RAM buffering with direct-to-disk streaming unless measurements show the storage device can sustain the camera stream with adequate margin. The current architecture intentionally absorbs the high-FPS sequence in RAM and saves only after capture.
