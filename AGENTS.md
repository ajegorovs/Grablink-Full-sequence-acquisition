# Repository guidance

## Purpose

This is a Windows MFC application derived from the Euresys Grablink Snapshot example. It captures a high-frame-rate 8-bit monochrome sequence from a MultiCam grabber into one precommitted RAM block, then saves the sequence as BMP files.

Correctness at the acquisition boundary is more important than stylistic modernization. Keep the MultiCam callback short, deterministic, and free of disk or UI work.

## Build environment

- Visual Studio 2022, platform toolset `v143`
- MFC application; supported production target is `x64`
- Euresys MultiCam headers and libraries under `C:\Program Files (x86)\Euresys\MultiCam`
- Solution: `GrablinkSnapshot.sln` (`Debug|x64`, `Release|x64`; builds both projects below)
- Application project: `src\GrablinkSnapshotMSVC110.vcxproj`
- Hardware-free tests: `tests\GrablinkSnapshotCoreTests.vcxproj`

Example build command:

```text
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" GrablinkSnapshot.sln -p:Configuration=Debug -p:Platform=x64
```

## Repository layout

```text
GrablinkSnapshot.sln   solution: Debug|x64 and Release|x64, application + tests
src/                   MFC application and its resources
core/                  hardware-independent units; no MFC, no MultiCam
docs/                  notes, work plan, investigation records
tests/                 hardware-free test project
```

## Required verification

For code changes, run all four checks:

1. Application `Debug|x64` build.
2. Application `Release|x64` build.
3. Core tests `Debug|x64`.
4. Core tests `Release|x64`.

The core test executables are:

```text
tests\x64\Debug\GrablinkSnapshotCoreTests.exe
tests\x64\Release\GrablinkSnapshotCoreTests.exe
```

A powered camera is required before claiming end-to-end acquisition verification. Without it, clearly distinguish build/unit-test results from hardware validation.

## Capture invariants

- The complete configured capture capacity is reserved and committed before capture starts. This is deliberate: acquisition must not run out of commit capacity halfway through.
- Do not zero the multi-gigabyte capture block. Every captured frame overwrites its own slot.
- No per-frame heap allocation.
- Respect `MC_BufferPitch`; grabber rows may be padded.
- The callback may retrieve the surface, copy one frame, change POD state under `m_captureLock`, and call `PostMessage` to the cached view handle.
- The callback must not perform file-system operations, create threads, show dialogs, manipulate `CString`, or traverse MFC document/view collections.
- Never write image files from the callback or UI thread.
- Transfer the full buffer to `FrameSnapshot`/`SaveWorker` without copying it.
- A failed save start must leave captured frames available for retry.
- Do not start another capture while a save is running or a pending snapshot is waiting to be saved.

## Threading and ownership

- Capture state and the cached view `HWND` are guarded by `m_captureLock`.
- `SaveWorker` owns and synchronizes save progress.
- `m_pendingSnapshot` is UI-thread-only.
- Use `WM_APP+n` messages for callback-to-UI notifications.
- Unregister the MultiCam callback before channel teardown.
- Preserve RAII ownership in `FrameBuffer`, `PixelStore`, `FrameSnapshot`, and `SaveWorker`; do not restore raw `malloc`/`free` ownership in the document.

## Scope and compatibility

- Keep the hardware-independent `core/` code free of MFC and MultiCam dependencies.
- Add behavioral tests for changes to `core/` before changing production implementation.
- Keep code compatible with the existing MSVC/MFC project; avoid dependency additions unless explicitly approved.
- Do not commit build products from `Debug/`, `Release/`, `x64/`, or `tests/x64/`.
- Do not commit, push, or rewrite history unless explicitly requested.

## Current limitations

See `docs/AGENDA.md`. In particular, real callback shutdown behavior, preview-surface lifetime, sustained FPS, and dropped-frame behavior remain hardware-validation items.
