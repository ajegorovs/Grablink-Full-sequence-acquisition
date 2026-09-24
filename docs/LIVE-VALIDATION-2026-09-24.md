# Camera-assisted validation — September 24, 2026

## Scope and evidence

Operator used the connected Basler camera with the Debug|x64 build and reported that the camera was configured and released from external tooling before launch. The application's installed MultiCam channel reported 2040 × 1088 pixels and a 2040-byte source pitch. We observed the UI diagnostics and independently enumerated/read the BMP files on the local machine. The app was closed at the end; a native process query found no GrablinkSnapshot.exe process. The full raw captures are local scratch output under `%LOCALAPPDATA%\Temp\grablink-camera-validation-20260924`; they are not committed or accessible to an external reviewer. The observations below are a session record, not remotely reproducible raw evidence.

| Run/folder under scratch root | Operation | Observed result |
| --- | --- | --- |
| `20260924_150339_538` | Go, capacity 200 | 200 sequential BMPs, no extras/gaps; first/middle/last headers and hashes checked. Diagnostics: 0.569 s, 351.6 stored FPS; 200 written/0 failed, 444119600 bytes in 1.06 s (399.0 MiB/s reported). |
| `20260924_150741_291` | Go again, capacity 200 | Separate 200-BMP run; prior run's sampled hashes unchanged. Diagnostics: 0.568 s, 352.0 stored FPS while cumulative stored count was 400. |
| `20260924_151402_576` | Go, capacity 2000 | 2000 sequential BMPs; **every** file's length/header checked: 2040 × 1088 top-down 8-bit, 2220598 bytes/file, 4441196000 bytes total. Diagnostics: 5.687 s, 351.7 stored FPS; 2000 written/0 failed in 8.69 s, 487.5 MiB/s reported. Operator observed responsive menu-hover animations during capture. |
| `20260924_152031_391` | Go → Stop & Save | 1104 contiguous BMPs, valid checked headers, 2451540192 bytes. The preceding Go → Stop discarded immediately and produced no new output folder. |
| `retry-target/20260924_152444_190` | Missing selected base folder | App recreated the directory and saved 200 files; deleting the directory is **not** a save-refusal test. |
| `blocked-target/20260924_153211_736` | Selected base path replaced temporarily with a regular test file | Save refused after 200-frame capture; operator saw “file, not a folder” and “200 frames are kept.” Restored the empty directory; Stop & Save retry wrote 200 complete, sequential BMPs (444119600 bytes), all headers checked. No successful capture was touched to create the test. |
| `20260924_155135_792` | Later full Go run | Separate 2000-BMP sequence finished. |
| `20260924_155226_468` | Close while saving | Process exited; 439 sequential, valid BMPs (`Image_00000.bmp`–`Image_00438.bmp`), no malformed or partial file among those written. Remaining unsaved frames were not preserved. A separate close-while-recording attempt exited with no new save folder. |

The diagnostic counter snapshot after the first 2000-frame run reported 2400 cumulative stored frames, zero frame rejections, zero acquisition failures and zero preview drops. It counted 506003 admitted callbacks (mostly preview-only), 446.7 µs average, 8242 µs maximum. The maximum exceeds one nominal frame interval but is *cumulative*, not isolated to the capture window; neither it nor a complete saved sequence proves zero upstream loss.

## Separate physical-timing evidence

The operator reported an **external** earlier experiment filming an LED sequence and counting frames per LED light-up period to assess physical frame pace. No protocol, original footage, date, numerical result or report was provided to this repository. Do not present that experiment as a repo-verified result or require repeating it by default. Its question (physical cadence) differs from delivery continuity in this run (whether camera/grabber frames were skipped before the app stored them). Obtain and cite the external record if it becomes available; otherwise label its conclusion operator-reported.

## Next proof required

1. Check the installed SDK's exact `MC_TimeCode`, `MC_OverrunCount`, cluster-unavailable and related signal semantics, scope, reset and rollover, then instrument a bounded run so driver sequence/counter evidence can be correlated to stored frames **from that same run**. Do not introduce formatting, allocation, dialogs or filesystem work in the callback. Instrument capture-window callback latency separately from always-on preview latency.
2. Cross-check app preview-drop classification against `PreviewPublisher::Counters()`. Normal startup, capture and save worked live; driver refusal paths were not deliberately injected.
3. Re-run Debug/Release x64 solution builds and both core test binaries after edits; last combined hardware-free gate on this branch was 129/129 tests in each configuration. Review the Phase 2 branch commits and package reviewer-accessible evidence. Do not publish multi-gigabyte raw captures by accident.

Acceptance remains open for **zero camera-to-grabber delivery loss**. We did verify the bounded stored-frame throughput, save sequence integrity, retry ownership, UI responsiveness during recording, and clean process exit on the tested paths.
