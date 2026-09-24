# GrablinkSnapshot

Windows MFC application for capturing high-frame-rate 8-bit monochrome image sequences from an Euresys Grablink/MultiCam capture card.

The application records the sequence into a precommitted RAM block so camera bandwidth is not limited by SSD write speed. After capture, ownership of that block is transferred without copying to a background worker that saves the frames as BMP files.

## Repository layout

```text
GrablinkSnapshot.sln   solution: Debug|x64 and Release|x64, application + tests
src/                   MFC application and its resources
core/                  hardware-independent units; no MFC, no MultiCam
docs/                  work plan, investigation records, historical checklist
tests/                 hardware-free test project
```

## Current architecture

```text
MultiCam surface
    |
    | pitch-aware frame copy; no allocation or disk I/O
    v
Precommitted FrameBuffer in RAM
    |
    | zero-copy ownership transfer
    v
FrameSnapshot -> SaveWorker -> 8-bit BMP sequence
```

Important behavior:

- The complete capture capacity is reserved and committed before acquisition starts.
- Physical pages are touched as frames arrive; the application does not zero the full block.
- `MC_BufferPitch` is respected when copying grabber surfaces.
- The MultiCam callback does not write files or perform MFC/UI work.
- Saving runs asynchronously and reports progress and failures in the UI.
- Stop discards the current capture; Stop & Save saves a partial capture.
- If saving cannot start, captured frames are retained for retry.

## Requirements

- Windows and Visual Studio 2022 with platform toolset `v143`
- MFC components installed
- Euresys MultiCam installed on the machine. The project resolves it from `$(MulticamRoot)`,
  which defaults to `$(MSBuildProgramFiles32)\Euresys\MultiCam`, so the SDK header
  (`Include\multicam.h`) and the import library (`Lib\amd64\MultiCam.lib`) are found without
  editing the project when MultiCam is in its standard location
- Production build target: `x64`
- A compatible Grablink card and camera for end-to-end acquisition

The current camera configuration uses:

- connector `M`;
- CAM file `acA2000-340km_P340SC`;
- `MC_ColorFormat_Y8`;
- repeating snapshot acquisition.

For a Basler camera that does not retain transport settings, configure the correct tap/transport mode in Pylon Viewer after power-up and verify the stream in the Euresys/MultiCam tools first.

## Setting up on another machine

The repository does not ship the Euresys SDK. `vendor/` is gitignored, and the MultiCam
installation is not redistributable, so a clone contains the project, the code and the tests,
but no headers, no import library, no runtime and no camera files. Per machine:

1. Install the Euresys MultiCam runtime from Euresys (the Windows installer, for example
   `multicam-win10-6.19.5.6064.exe`). It provides the headers and `MultiCam.lib` under
   `C:\Program Files (x86)\Euresys\MultiCam`, the runtime `multicam.dll` with the **MultiCam
   Service**, the camera-file library under
   `C:\Users\Public\Documents\Euresys\MultiCam\Cameras`, and the MultiCamStudio and
   CameraLinkValidationTool utilities used to verify the link before running this application.
2. Install Visual Studio 2022 with the MFC components and the `v143` toolset, and build `x64`.
3. Install the grabber driver as described by Euresys for the card, and verify the camera
   stream in MultiCamStudio first.
4. Check the camera configuration in `src/GrablinkSnapshotDoc.cpp`: the connector, the CAM file
   (`acA2000-340km_P340SC` in the current configuration), the colour format and the acquisition
   mode are set there. The CAM file must exist in the installed camera-file library.

If MultiCam is installed anywhere other than the default location, point the build at it instead
of editing the project — a command-line property, or an environment variable of the same name:

```text
MSBuild.exe GrablinkSnapshot.sln -p:Configuration=Debug -p:Platform=x64 -p:MulticamRoot="D:\SDK\Euresys\MultiCam"
```

The application was developed and validated against MultiCam **6.19.4.5806**. The `6.19.5`
Windows package changes nothing functional on Windows — its release notes cover Linux
installation and Clang/LLVM kernel builds; see `docs/SCOUT-multicam-ram-to-ssd.md`.

## Build

From a Visual Studio developer environment, or by invoking MSBuild directly:

```text
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" GrablinkSnapshot.sln -p:Configuration=Debug -p:Platform=x64

"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" GrablinkSnapshot.sln -p:Configuration=Release -p:Platform=x64
```

The solution builds the application and the tests. The application executable is produced under
`x64\Debug` and `x64\Release`, the test executable under `tests\x64\Debug` and
`tests\x64\Release`. `Win32` configurations still exist in the application project but are not
part of the solution and are not a supported target: the high-capacity capture path is x64.

## Hardware-free tests

The `core/` capture-buffer, BMP-writer, ownership, and asynchronous-save behavior can be tested without a powered camera:

```text
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" GrablinkSnapshot.sln -p:Configuration=Debug -p:Platform=x64

tests\x64\Debug\GrablinkSnapshotCoreTests.exe
```

Run the same commands with `Configuration=Release` and `tests\x64\Release\GrablinkSnapshotCoreTests.exe`.

Current baseline: **121 tests pass in both Debug and Release**.

## Operation

1. Start the application and confirm the live preview.
2. Use **Set Output Folder...** before capturing if the default is not appropriate.
3. Select **Go!** to begin filling the RAM buffer.
4. Use:
   - **Stop** to discard the current partial capture;
   - **Stop & Save** to save the frames captured so far;
   - or allow the buffer to fill and start saving automatically.
5. Wait for background saving to finish before beginning another capture.

The current defaults—frame count, CAM file, exposure, and output folder—are still configured in `CGrablinkSnapshotDoc` and are scheduled to move into persistent configuration.

## Memory behavior

Required capture memory is approximately:

```text
image width × image height × frame count
```

For a 2048 × 1088 Y8 image and 20,000 frames, the capture block is about 41.5 GiB. `VirtualAlloc(MEM_RESERVE | MEM_COMMIT)` charges that amount against the Windows commit limit before capture, ensuring the recording does not fail halfway through due to insufficient commit capacity. The pages are not needlessly zeroed by the application.

## Documentation

- `AGENTS.md` — repository rules, invariants, and verification requirements.
- `docs/AGENDA.md` — ordered future work and camera-connected validation plan.
- `docs/SCOUT-multicam-ram-to-ssd.md`, `docs/SCOUT-language-and-ui-options.md` — investigations
  that measured the vendor SDK, the dump target, and the host-language/UI options.
- `docs/TASK_v1.md` — historical improvement checklist from the earlier implementation.

## Verification status

Validated on a connected Basler camera and the MultiCam runtime: `Debug|x64` and `Release|x64`
builds, 121 core tests in both configurations, a 200-frame capture writing exactly
`Image_00000.bmp`–`Image_00199.bmp`, a 2000-frame capacity run, consecutive captures claiming
separate run folders, clean shutdown while the live callback stream was active, and interactive
modal dialogs during live preview.

Still open, and the reason no sustained-rate claim is made here: measured dropped frames at
300–350 FPS (the application counts its own frames; the driver's `TimeCode`/`OverrunCount`
counters are not read yet), the invalid-output-folder retry flow, and a full-buffer capture
followed by a complete disk-write cycle. `docs/AGENDA.md` is the authoritative list.
