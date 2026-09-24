# Scout: MultiCam 6.19.5 vendor drop — what it offers for "capture to RAM, dump to SSD"

Scope: read-only survey of `vendor/multicam-6.19.5/` plus the installed runtime and this machine's
hardware/storage. No production code touched. Prepared for the parallel implementation effort.

## 1. What is in the vendor drop, and where it was unpacked

| File | Purpose | State |
|---|---|---|
| `multicam-win-sample-programs-6.19.5.6064.zip` | all Windows samples (C++, C#, VB) | extracted → `vendor/multicam-6.19.5/samples-win/` (4.2 MB) |
| `multicam-linux-sample-programs-6.19.5.6060.tar.gz` | Linux/SDL samples + CUDA | extracted → `vendor/multicam-6.19.5/samples-linux/` (172 KB) |
| `multicam-linux-offline-documentation-6.19.5.4061.tar.gz` | **full HTML doc set + all PDF guides** | extracted → `vendor/multicam-6.19.5/docs/` (298 MB) |
| `multicam-win-offline-documentation-6.19.5.4061.exe` | same doc set, Windows installer | **not extracted** — 7-Zip 23.00 classifies it as `PE` and the embedded 7z payload at offset 146416 is not a readable 7z archive. The Linux tarball carries the identical content; use it. |
| `multicam-win10-6.19.5.6064.exe` | MultiCam **runtime** installer 6.19.5 | not installed |
| `multicam-linux-x86_64-6.19.5.6060.tar.gz` | Linux runtime | n/a on this box |

`vendor/` is gitignored (`.gitignore:8`), so none of this is committed — the extractions are scratch.

Key starting point: the repo's original C++ files are the vendor sample. `vendor/.../MsVc/GrablinkSnapshot/GrablinkSnapshot.{h,MainFrm.h,StdAfx.h}`
are byte-identical to the repo's; `GrablinkSnapshotDoc.cpp` differs by ~1150 lines, i.e. everything in
`core/` + doc/view is ours.

## 2. Runtime version: no reason to move to 6.19.5 (on Windows)

- Installed: **6.19.4.5806** (`Bin/x86_64/MultiCamStudio.exe`, `CameraLinkValidationTool.exe`; python wheel `MultiCam-6.19.4.5806-py2.py3-none-any.whl`).
- Vendored: **6.19.5.6064** (not installed).
- Release notes (`docs/.../Content/11_Pdf/D401EN-MultiCam_Release_Notes-6.19.5.4061.pdf`, §4.2/§4.3): **6.19.5 for Windows contains nothing** — added/improved features are Linux installation scripts and Clang/LLVM kernel builds; solved issues are Linux kernel installs. The one change is a Linux uninstall-script change.
- Relevant fixes are already in the installed 6.19.4: *"Fixed channel activation failure (MC_IO_ERROR) that could follow right after changing the value of parameters affecting the cluster (e.g. ColorFormat, GrabField, SurfaceCount…)"* (6.19.4), and *"Fixed incrementation of the OverrunCount parameter"* (6.19.3).
- Breaking change to be aware of only if we ever move: `StrobeDur` range is `[1,100]` since 6.19.3 (we don't use strobe).
- 6.19 is the last release supporting 32-bit development → consistent with AGENDA "production path is x64".

Conclusion: AGENDA Phase 6's "validate 6.19.5 before upgrading" can be answered from the release notes — the upgrade buys this project nothing on Windows.

## 3. The card and camera on this box (facts, not assumptions)

- PCI device: `GRABLINK Full` (`PCI\VEN_1805&DEV_030A`, i.e. PC1622).
- PC1622 Grablink Full has a **64-bit DMA engine** (`02_What_s_New/.../Important_Notices/64-bit-DMA-capable-products.htm`) — this is the precondition for manual surface allocation on x64 Windows (see §5).
- CAM file in use: `acA2000-340km_P340SC`, supplied by the vendor at
  `C:\Users\Public\Documents\Euresys\MultiCam\Cameras\BASLER\acA2000-340km\acA2000-340km_P340SC.cam`
  (2122 `.cam` files live under `%PUBLIC%\Documents\Euresys\MultiCam\Cameras\<VENDOR>\<MODEL>\`).
  That CAM file declares `Imaging = AREA; TapConfiguration = DECA_10T8; TapGeometry = 1X10_1Y; Hactive_Px = 2040; Vactive_Ln = 1088; ExposeOverlap = FORBID`, which matches the repo's `MC_BoardTopology_MONO_DECA` and the 2040×1088 geometry in AGENDA.
- Frame arithmetic: 2040 × 1088 = 2,219,520 B packed; with a 2048 pitch = 2,228,224 B. At 340 fps that is **~755 MB/s** of DMA traffic.

## 4. Does the SDK help us write to RAM and then dump to SSD? — No, it stops at RAM

- Nothing in the 586-file sample set writes a sequence to disk except:
  - `MsVc/PicoloDirectShow` → AVI via the MultiCam **DirectShow** filters; Picolo analog boards only, needs the ffdshow codec (`docs/.../Sample_Programs/Picolo_Sample_Programs.htm`).
  - `python/GrablinkSnapshot/GrablinkSnapshot.py` → one BMP via Pillow (`image.save`).
  - MultiCam Studio has an image "Save" function (see Known Issues caveat about 16-bit/BMP).
- The documentation contains **no** guidance on SSD/disk streaming, throughput targets, or sequence-file formats. `Multicam_Storage_Formats` is about pixel formats, not file storage.
- Hardware metadata insertion (per-line LVAL/GPPC/Q counters written into the pixel stream) is documented for **LINE/TDI cameras only** and needs `MEDIUM_4T8`/`DECA_10T8` **line-scan** configurations → **not applicable to this area-scan Basler** (`04_Grablink/Functional_Guide/Advanced_Features/Metadata_Insertion/*`).
- The vendored `Grablink_Migration_Guide.pdf` covers Grablink-generation → Grablink-generation moves only; there is no GenTL/eGrabber/Coaxlink material in this package (that stack is the CoaXPress product line, not Grablink).

So the RAM→SSD design stays ours; what the SDK *does* give us is (a) DMA into memory we can own, and (b) driver-side counters/signals to prove nothing was dropped (§6).

## 5. The three RAM-buffering shapes the SDK actually supports

Hard limits first (`04_Grablink/Functional_Guide/.../surface-allocation-rules.htm`, `02_What_s_New/.../buffer-size-limit.htm`):

- **max 2 GiB per surface** — above that MultiCam returns `MC_IO_ERROR` at activation. At 2,228,224 B/frame that is **≈ 964 frames per surface**.
- **max 4096 surfaces** per channel / per application / per board (DualBase with two channels: ~2048/channel practical).
- **max ~4,000,000 descriptors per surface** on Grablink Full (~2,000,000 for Base/DualBase); descriptors ≈ SurfaceSize/4096 for fragmented memory. For a 2 GiB surface this is the *non*-binding constraint, but it is why RGB-planar and `ImageFlipY` surfaces are expensive (a descriptor boundary per line/component). "Cropping in hardware" relaxes this for monochrome packed formats with `ImageFlipY = OFF`.
- Manual allocation (application-owned buffers) is prohibited **only** when the board lacks 64-bit DMA — our Grablink Full is fine, and `MC_SurfaceAllocation_ANYWHERE` is legal.

**Shape A — today's design (keep):** MultiCam auto-allocates a small cluster (`MC_SurfaceCount`), the callback copies each surface into our own pre-committed block. Our own block is *not* a surface, so the 2 GiB cap does not apply to the capture capacity. Cost: one memcpy of ~2.2 MB per frame (~755 MB/s of memory bandwidth at 340 fps) and one driver interrupt/callback per frame.

**Shape B — zero-copy via manual surfaces:** give MultiCam our memory. Working vendor code for the exact call sequence:
- `samples-win/.../MsVc/GrablinkDualFull/GrablinkDualFullGrabber.cpp:173-227` — `new BYTE[size]`, `McCreate(MC_DEFAULT_SURFACE_HANDLE, &h)`, `McSetParamPtr(h, MC_SurfaceAddr, buf)`, `MC_SurfacePitch`, `MC_SurfaceSize`, then `McSetParamInst(channel, MC_Cluster + i, h)` for i in 0..N-1.
- `samples-win/.../MsVc/GrablinkMultiBase/GrablinkMultiBaseGrabber.cpp:173-200` — same, with `MC_MinBufferPitch` used to place one module's surface inside a shared allocation (useful if we ever build one big block holding N ≤2 GiB chunks — note the *surface* must still be ≤2 GiB).
- `samples-win/.../MsVc/grablink-cuda/src/main.cpp` — the same pattern with **`cudaMallocHost` pinned memory** and `MC_SurfaceState_RESERVED` to hold a surface out of the rotation; README documents ~6 GB/s host→GPU on the pinned buffer.

Consequence for us: a >2 GiB RAM sequence has to be registered as **several surfaces** (each ≤2 GiB), all registered before `ChannelState = ACTIVE` (the cluster is fixed while running; `MC_CLUSTER_BUSY` exists as an error code).

**Shape C — HFR mode, the one piece of "new" vendor tooling for this goal:** `MC_AcquisitionMode_HFR` divides the sequence into phases that each acquire `PhaseLength_Fr` frames **into a single surface**, so the OS interruption rate is divided by `PhaseLength_Fr` (`Multicam_Acquisition_Principles/High_Frame_Rate.htm`, `04_Grablink/Functional_Guide/Acquisition/hfr-acquisition-mode.htm`).
- `PhaseLength_Fr` range **1..255** (the Principles page's "2..256" is inconsistent with both the parameter page and the functional guide — verify on hardware).
- `SeqLength_Fr` up to `PhaseLength_Fr × 65,534`; SNAPSHOT alone caps at 65,534.
- Manual stop = `ChannelState = IDLE`; the doc guarantees the sequence ends **on a frame boundary**, so the surface is whole.
- Monitoring: `Elapsed_Fr`, `Remaining_Fr`, `PerSecond_Fr`.
- Important: in SNAPSHOT mode `PhaseLength_Fr` is *enforced to 1* — one frame per surface, i.e. 340 callbacks/s is inherent to the current mode. HFR at 255 turns that into ~1.3 phases/s, but the surface now holds 255 frames (slices), so the SSD dump must slice frames out of a surface and the UI/counter/save-progress path must become phase-based rather than frame-based. That is the main design decision to hand whoever implements the RAM→SSD work; verify the in-surface slice layout (offset = k × pitch × lines?) on hardware before relying on it.

## 6. Driver-provided evidence for "no frame was lost" (for the instrumentation phase)

Already available, no app-side bookkeeping needed:

- `MC_OverrunCount` (get-only, writable to reset) — "incremented each time a transfer overrun occurs… when the data transfer between the frame grabber and the host computer saturates the PCI bus". This is the driver-truth counter for the RAM-capture failure mode.
- `MC_TimeCode` (surface, get-only) — sequence position of each surface; **incremented even when an acquisition is not signalled** (e.g. cluster unavailable), reset at each ACTIVE. So `signalled frames` vs `TimeCode` is exactly the dropped-frame measure.
- `MC_Elapsed_Fr` / `MC_Remaining_Fr` / `MC_PerSecond_Fr` / `MC_FillCount`.
- Signals to enable: `MC_SIG_SURFACE_PROCESSING`, `MC_SIG_CLUSTER_UNAVAILABLE`, `MC_SIG_ACQUISITION_FAILURE`, `MC_SIG_FRAMETRIGGER_VIOLATION`, `MC_SIG_END_CHANNEL_ACTIVITY`; `MC_AcquisitionCleanup_ENABLED` suppresses spoiled images (their surfaces go straight to FREE).
- Teardown pattern the vendor samples use, and which matches our "unregister the callback before channel teardown" invariant: `ChannelState = IDLE` → `McWaitSignal(channel, MC_SIG_END_CHANNEL_ACTIVITY, …)` → then delete surfaces/channel (`grablink-cuda/src/main.cpp`, `GrablinkMultiBaseGrabber.cpp:289-292`).
- `MC_SignalHandling` is per-signal (CALLBACK / WAITING / OS_EVENT), so end-of-activity can be waited on while frames stay callback-driven — exactly the mixed use the CUDA sample demonstrates.
- Memento (Euresys trace/analysis tool): drivers support it, traces land in `C:\Users\Public\Documents\Euresys\Memento\traces\MultiCam`, and 6.18+ provides **Sequence / Waiting / Acquisition / TimeCode probes** for Grablink; the analyzer itself is **not installed** on this box (separate Euresys download).

## 7. Target storage, measured (dump target reality check)

- The only SSD is the system SATA volume `C:` (316 GB free). The secondary `D:` HDD (2 TB, 29 GB free) is not a capture target.
- Measured on `C:` (Python, 8 MiB writes, files deleted afterwards):
  - 1 GiB single file: 2524 MB/s write loop (page cache), then **4.3 s fsync flush → ~227 MB/s media-sustained**.
  - 300 × 2.22 MB files, no per-file flush: 522 MB/s (cache-backed).
  - 300 × 2.22 MB files **with fsync per file: 38 MB/s** (17.4 s) — per-file flushing is the thing to avoid.
  - Re-measured later in three consecutive rounds with a fully resident source: a single 446 MB file writes at **356 / 343 / 345 MB/s** (fsync included), while 200 BMP-shaped files with ordinary buffering return in 0.19–0.23 s because the page cache absorbs them. Earlier runs on the same drive gave 86 MB/s (single file) and 22 MB/s (200 files) while it was still draining ~5 GB of previous test writes.
  - Honest range for this budget, DRAM-less SATA SSD: **~350 MB/s when idle, collapsing to 20–90 MB/s while the drive is busy/collecting**.
- Consequences: the 755 MB/s capture rate cannot be written live to this SSD, which independently confirms AGENDA's "RAM buffering is intentional". A 200-frame capture (≈444 MB) dumps in ~2 s at media speed (~12 s if each file were flushed); a 10,000-frame capture (≈22 GB) would take ~1.5–2.5 min to dump.

## 8. What is missing / open

- No vendor sample or doc for long-sequence capture with a post-capture dump — nothing to port, only constraints to respect.
- In-surface slice layout for HFR (frame k offset inside a surface) is not spelled out in the docs → hardware item.
- `MC_BufferPitch` value for this CAM file (2040 vs 2048) is a runtime read; AGENDA already asks for it to be recorded.
- Memento Analyzer is not installed, so trace-based drop analysis needs a separate download.

## Evidence index (all paths relative to the repo root)

- Samples: `vendor/multicam-6.19.5/samples-win/multicam-sample-programs/MsVc/{GrablinkSnapshot,GrablinkHfr,GrablinkDualFull,GrablinkMultiBase,grablink-cuda,MulticamAdvancedWaitSignal,PicoloDirectShow}`, `.../MsCs/*`, `.../python/GrablinkSnapshot/GrablinkSnapshot.py`
- Docs: `vendor/multicam-6.19.5/docs/multicam-linux-offline-documentation/Content/`
  - `02_What_s_New/Multicam_Release_Notes/Important_Notices/{buffer-size-limit,memory-allocation,64-bit-DMA-capable-products}.htm`
  - `03_MultiCam/Multicam_Acquisition_Principles/{Memory_buffers,High_Frame_Rate,Snapshot}.htm`
  - `03_MultiCam/Multicam_User_Guide/{surface-allocation-rules,cluster-of-surfaces,image-sequence-acquisition,surface-states,callback-signaling,enabling-signals,multicam-signals}.htm`
  - `04_Grablink/Functional_Guide/Acquisition/{snapshot,hfr}-acquisition-mode.htm`, `04_Grablink/Functional_Guide/Advanced_Features/Metadata_Insertion/*`
  - `04_Grablink/Parameters_Reference/parameters/{surfacecount,surfaceaddr,surfaceallocation,surfacestate,surfacesize,bufferpitch,buffersize,minbufferpitch,phaselength_fr,seqlength_fr,signalhandling,timecode,overruncount,elapsed_fr,persecond_fr,acquisitioncleanup}.htm`
  - PDFs: `Content/11_Pdf/D401EN-…Release_Notes…pdf`, `D402EN-…User_Guide…pdf`, `D405EN-…Acquisition_Principles…pdf`, `D412EN-…Parameters…pdf`
- Installed runtime/header evidence: `C:\Program Files (x86)\Euresys\MultiCam\{Bin\Bin\x86_64,Include\McParams.h,Python\*.whl}`
- CAM file: `C:\Users\Public\Documents\Euresys\MultiCam\Cameras\BASLER\acA2000-340km\acA2000-340km_P340SC.cam`
