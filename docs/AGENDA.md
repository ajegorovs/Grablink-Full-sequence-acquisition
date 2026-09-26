# Future work plan

## Next session — priorities (September 24, 2026 handoff)

1. **Close the delivery-continuity gap.** The LED-sequence timing experiment
   was an external validation reported by the operator; no raw footage, protocol,
   measured values, or report are in this repository. It assessed physical frame
   pace from images, not whether every camera frame reached this application's
   buffer in the present run. Do not call it missing repo work or repeat it by
   default. Record its date, setup, and result only if the external record becomes
   available. Design a bounded, camera-assisted readout of MultiCam surface
   `MC_TimeCode` continuity and `MC_OverrunCount`/cluster-unavailable signals
   during the *same* capture window; check SDK parameter scope and reset/rollover
   semantics before coding. Keep the callback POD-only and short. A complete BMP
   sequence proves every *stored* frame was saved, not that upstream frames were
   never skipped. The 8242 us callback maximum is cumulative over preview and
   capture; measure capture-window latency separately before claiming headroom.
2. **Reconcile diagnostics.** Compare preview-drop classification with
   `PreviewPublisher::Counters()`; audit remaining unchecked driver operations
   and exercise failure messages without disturbing a live capture. The normal
   startup/capture/save paths were live-tested, not injected driver failures.
3. **Prepare external review.** Phase 2 is packaged as separate commits on
   `build/multicam-root-and-setup-docs`. Re-run both x64 solution builds and
   both hardware-free test executables after any further code edit (last combined
   gate: 129/129 in Debug and Release). Review the branch diff and provide
   evidence accessible to the reviewer. The September 24 BMPs are local scratch
   output, not part of the repo or remote; do not claim the external reviewer
   can inspect them.
4. **Then operator-safety backlog:** persist and anchor the output folder,
   provide an explicit discard-pending action, and address modal ownership.
   Treat SDK upgrade, camera preflight automation and UI toolkit choice as
   separate decisions rather than prerequisites to the continuity check.

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
- the prior baseline had 121 core tests in both configurations; the Phase 2
  branch has 129 passing tests in each configuration;
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

- Git repository on `build/multicam-root-and-setup-docs` for the Phase 2
  diagnostics work. Remote repository:
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

## Phase 1 — Camera-connected validation (mostly completed)

The September 24, 2026 operator run completed the short/full/partial capture,
retry, repeat-run and shutdown flows below; measurements are recorded under
Phase 2 and in `docs/LIVE-VALIDATION-2026-09-24.md`. The camera had been
configured and released in external tooling before the session, but its exact
tap settings were not captured in this repo. Physical frame pace was previously
assessed by an external LED-sequence experiment, per operator report; no
protocol, footage or quantitative result is available here. Neither that
report nor the complete BMP sequences prove zero upstream drops in this run.
The outstanding acceptance item is driver delivery continuity (TimeCode,
overruns and related signals) measured on a bounded live run.

The following is the original live-validation checklist. Items 3–5 and the
stored-throughput/save portion of item 6 were exercised on September 24. The
operator confirmed the device was configured and released, but the exact Pylon
settings and a separate Euresys-tool test (items 1–2) were not documented here.
Driver loss evidence in item 6 remains open.

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

Acceptance status: stored-frame sequence, BMP integrity, responsive UI during
recording, retry and clean shutdown passed the tested paths. No acquisition
failures were reported, but zero upstream loss and callback-latency margin remain
unproven pending driver sequence/overrun and capture-window timing evidence.

## Phase 2 — Instrumentation and diagnostics

Implemented and exercised on the normal powered-camera path on September 24,
2026; fault-injection and delivery-continuity evidence are still pending:

- `CaptureStats` is fed by the admitted callback: surface signals (including
  preview-only), stored frames, refused appends, driver acquisition failures,
  preview publications/drops, and QPC-measured callback duration. Capture
  windows begin on Go and end once on the active-to-stopped transition. The
  snapshot accessor now has an on-demand reader, `View > Capture Diagnostics...`
  (see Remaining). The 129 hardware-free core tests pass
  in Debug and Release; they do not exercise the MultiCam callback.
- `SaveWorker::Progress()` reports successful BMP file bytes, monotonic elapsed
  time, and whole-job average bytes/second, including partial/failed/cancelled
  jobs. Save metrics have one owner, `SaveWorker`; they are not duplicated
  in `CaptureStats`.
- Semantics: `surfacesReceived` counts admitted surface-processing signals even
  outside a capture; `framesRejected` counts only failed appends (which also
  stop capture); `acquisitionFailures` counts driver failure signals, not append
  refusals. Preview publish refusals from invalid input do not count as UI lag.

Remaining:

- ~~Expose capture and save diagnostics to the operator, including dimensions,
  pitch, capacity, committed bytes, capture duration/FPS, callback latency and
  save duration/throughput.~~ Done: an on-demand `View > Capture Diagnostics...`
  snapshot reads the capture lock-protected frame-buffer geometry, count,
  capacity and committed bytes plus the cached source pitch, `CaptureSnapshot()`
  and `GetSaveProgress()`, and shows them in a modal snapshot dialog: a large
  read-only multiline edit that scrolls both ways and can be selected and
  copied, with a Close button, instead of a message box that clipped the
  roughly 40-line report at the desktop edge. The dialog holds the usual
  application-modal scope for exactly the span of its `DoModal()`, and it is
  deliberately a snapshot of the moment the command ran - there is no timer and
  no live refresh, so nothing reaches into the capture path while it is open.
  It separates the last capture window (duration/FPS, reported only when a
  window is usable) from the cumulative acquisition and preview counters.
  Save figures come only from `SaveWorker::Progress()`; the dialog also no
  longer claims the copies are one atomic snapshot across sources. The
  operator read the dialog during the September 24 live session; displayed
  capture/save values matched the checked files on the normal success path.
- Confirm the preview-drop classification against the publisher's own counters
  rather than relying on duplicate checks.
- **Partly done:** setup and activation `Mc*` return codes are checked and
  translated into UI-thread error messages. Every setup and activation call in
  `OnNewDocument()` is now checked: `McSetParamInt(MC_BoardTopology)`,
  `McCreate`, `McSetParamInt(MC_DriverIndex)`, `McSetParamStr(MC_Connector)`,
  `McSetParamStr(MC_CamFile)`, `McSetParamInt(MC_Expose_us)`,
  `McSetParamInt(MC_ColorFormat)`, `McSetParamInt(MC_AcquisitionMode)`,
  `McSetParamInt(MC_TrigMode)`, `McSetParamInt(MC_NextTrigMode)`,
  `McSetParamInt(MC_SeqLength_Fr)`, `McGetParamInt(MC_ImageSizeX/Y/BufferPitch)`,
  `McSetParamInt(MC_SurfaceCount)`, the two signal enables (`MC_SIG_SURFACE_PROCESSING`,
  `MC_SIG_ACQUISITION_FAILURE`), `McRegisterCallback` and the activation
  `McSetParamInt(MC_ChannelState, MC_ChannelState_ACTIVE)`. The first refusal
  aborts the open on the UI thread instead of driving a rejected handle: the
  failing call's label and status code are shown, with the driver's own
  description of the status when the SDK supplies one. The description comes
  from the documented configuration lookup `McGetParamStr(MC_CONFIGURATION,
  MC_ErrorDesc + |status|)`, the same call the vendor samples use to turn a
  status into words - there is no separate MultiCam error-text API. On failure
  `AbortChannelSetup()` first runs the drain-safe `ResetCaptureState()` teardown
  and only then shows the box (never with `m_captureLock` held), so a partial
  setup - a created channel, a registered callback, a configured preview pool -
  is released immediately rather than leaking until the document is destroyed.
  Not yet camera-validated. The application-level driver open/close is checked
  too, and the global error-handling policy is no longer `MSGBOX`:
  - `CGrablinkSnapshotApp::InitInstance()` now checks `McOpenDriver(NULL)` and
    aborts startup with an operator message when it is refused. The installed
    SDK documentation (`McOpenDriver` reference) states the call returns
    `MC_SERVICE_ERROR` (-25) when the MultiCam Service is not running, and that
    software should only touch MultiCam while the service is up; the same page
    suggests retrying in a loop until `MC_OK`, which this application
    deliberately does not do (a loop would block startup with no bound).
    `ExitInstance()` closes the driver only when the open really succeeded,
    because the same reference requires as many `McCloseDriver()` calls as
    successful `McOpenDriver()` calls.
  - `MC_ErrorHandling` is set to `MC_ErrorHandling_NONE`, the documented "Return"
    behavior (ErrorHandling parameter reference and the "API Errors" page; also
    the documented default): on error the driver returns the code and shows no
    dialog box. It is the only one of the four behaviors that both suppresses the
    vendor's own message box and keeps every status usable by this application -
    `MSGBOX` can force a failed function to return `MC_OK` when the operator picks
    *Ignore*, and `EXCEPTION`/`MSGEXCEPTION` throw a Win32 structured exception
    this build does not catch. The operator-facing errors therefore remain ours,
    reported on the UI thread by the document. If the policy cannot be set,
    startup is refused rather than continued on an unreliable policy.
  - The `ResetCaptureState()` teardown calls now pass their statuses to a
    debug-only logger (`LogTeardownStatus`). They never branch on a status and
    never show a dialog: the callback drain that follows is what makes releasing
    the channel safe, and a refused stop call (ordinary on a channel that never
    activated) is not a reason to skip it. With the policy at `NONE` these calls
    can no longer raise the vendor's message box.
  - The callback's `McGetParamPtr(MC_SurfaceAddr)` read is checked. A refused read
    used to leave a NULL surface that was passed into `Append()` and reported to
    the operator as a "frame store failed" - false, since nothing had reached the
    buffer. It is now its own POD status path: the callback stops the run once
    via `EndCaptureRunUnderLock()`, records the raw `MCSTATUS` and sets the
    acquisition-error flag, and posts `WM_APP_ACQUISITION_ERROR` - all under
    `m_captureLock`, with no text, no allocation and no driver description lookup
    on the signal thread. The UI handler reads the status under the same lock and
    reports it as a refused surface-address read (with the driver's own
    description from the documented `MC_ErrorDesc` lookup, built on the UI
    thread). The notification is latched to one post per capture run
    (`m_surfaceAddrErrorNotified`, re-armed by `Go!`), because the refusal repeats
    at frame rate and a post per frame would flood the UI thread with error boxes.
  Not yet camera-validated.
  The view's channel-state and FPS reads are checked, skip a null channel, and
  run only inside the ~2 Hz status update; a failed read shows unavailable.
- **Live validation on September 24, 2026 (Debug|x64, powered camera):**
  two consecutive 200-frame runs saved complete sequences into separate folders;
  the first run's sampled SHA-256 hashes remained unchanged after the second.
  Diagnostics reported 351.6 and 352.0 stored FPS, respectively, with the
  cumulative stored count advancing to 400 (per-run FPS did not inflate).
  A bounded 2000-frame run stored 2000 frames in 5.687 s (351.7 stored FPS),
  then saved 2000 BMPs in 8.69 s (4441196000 bytes, 487.5 MiB/s reported).
  All 2000 files had contiguous names and valid 2040 x 1088 top-down 8-bit BMP
  sizes and headers. The operator could hover over the menu and see its animation
  during recording. Go -> Stop discarded immediately without a new output folder;
  Go -> Stop & Save wrote 1104 contiguous BMPs with valid headers in a separate
  folder. A selected output path occupied by a regular file refused saving
  after a 200-frame capture and reported that all 200 frames were kept; after
  restoring the empty directory, Stop & Save wrote all 200 frames with no gaps
  or invalid headers (444119600 bytes). Merely removing the selected directory
  is not a refusal test because the application recreates it before saving.
  Closing while capture was active exited the process with no new run folder.
  A later 2000-frame run finished saving in a separate folder; closing
  during the following run's active save exited the process and left 439
  sequential, valid BMPs (`Image_00000.bmp` through `Image_00438.bmp`) in that
  run's folder, with no partial/invalid file among them. This validates clean
  process exit and consistent files already completed, not preservation of the
  remaining unsaved frames. The tested outputs are local, under the Hermes
  scratch directory, and are not reviewer-accessible or committed evidence.
  The cumulative counters reported zero rejected frames, driver acquisition
  failures and preview drops,
  but the callback maximum reached 8242 us (the average was 446.7 us across
  506003 callbacks, mostly preview-only). These are not capture-only latency
  measurements and do not prove zero camera/driver frame loss: the driver
  TimeCode/sequence and overrun counters were not captured. Inspect those before
  claiming no dropped frames or a latency margin at 300-350 FPS.
- Cross-check preview-drop classification against publisher counters rather than
  duplicate conditions; collect driver TimeCode/overrun evidence to assess
  camera-to-grabber loss independently of stored frame counts.

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

A first answer to "which controls could move into a side pane, and what would
that cost" is in `docs/SCOUT-sidebar-and-control-relocation.md` (research only,
no code): every existing command and dialog can be hosted in a pane with the
document's handlers and modal-scope discipline reused unchanged, the cheapest
carrier is a classic `CDialogBar` on the current `CFrameWnd`, and the resizable
`CDockablePane` route is the one that needs the `CFrameWndEx`/`CWinAppEx`
migration - i.e. it is the choice that should wait for the toolkit decision
above, because a port replaces it.

## Deferred by design

Do not replace RAM buffering with direct-to-disk streaming unless measurements show the storage device can sustain the camera stream with adequate margin. The current architecture intentionally absorbs the high-FPS sequence in RAM and saves only after capture.
