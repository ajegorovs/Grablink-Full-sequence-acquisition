# GrablinkSnapshot - Improvement Tasks

## Overview
Current implementation accumulates images in RAM then writes to disk. Issues: no thread safety, blocking save, poor state handling, no progress feedback.

---

## Goal 1: Add Progress Feedback to UI
**Priority**: High  
**Risk**: Low  
**Status**: ✅ COMPLETED

**Task**: Update view with `_numImagesCounter` on each frame

**Files**:
- `GrablinkSnapshotDoc.cpp` - already calls `UpdateAllViews(NULL)` on each frame
- `GrablinkSnapshotView.cpp` - modify `OnDraw` to display counter

**Implementation**:
- In `CGrablinkSnapshotView::OnDraw`, display `_numImagesCounter / _numImages` in status bar
- Throttle updates to avoid overload: update every N frames where N = camera_fps / target_hz
- Current target: 2 Hz (configurable via `targetHz` variable)

**Verification**:
- Run "Go!" and see counter in status bar updating at ~2 Hz
- ✅ Works at 350 fps

---

## Goal 2: Reset State on Each "Go!"
**Priority**: High  
**Risk**: Low  
**Status**: Pending

**Task**: Clear image buffer and reset state at start of each capture

**Files**: `GrablinkSnapshotDoc.cpp`

**Implementation**:
- In `OnGo()`, before starting:
  - `memset(imageData, 0, totalSize)`
  - `_numImagesCounter = 0`
  - `dataSaved = false`
- Also: Show live camera feed on startup (not just on "Go!")

**Bonus Feature - Live Preview on Startup**:
- Channel now starts ACTIVE in `OnNewDocument()` for immediate live preview
- Added `_bCapturing` flag to distinguish preview mode from capture mode
- On "Go!": `_bCapturing = true` starts capturing to buffer
- On "Stop": `_bCapturing = false` stops capture but keeps channel ACTIVE for preview
- ✅ IMPLEMENTED

**Verification**:
1. Start capture, click Stop mid-way
2. Start capture again
3. Verify: buffer cleared, starts fresh (not appending to previous)

---

## Goal 3: Handle Mid-Capture Stop Properly
**Priority**: High  
**Risk**: Medium

**Task**: Don't save partial data when user stops early

**Files**: `GrablinkSnapshotDoc.cpp`

**Implementation**:
- In `OnStop()`:
  - Check if `_numImagesCounter > 0` and `dataSaved == false`
  - Either auto-save what's captured, or discard - decide behavior
  - Call `UpdateAllViews` to reflect stopped state

**Verification**:
1. Start capture, let it run ~500 frames
2. Click Stop
3. Verify: no incomplete BMP files in output folder (or partial save handled correctly)

---

## Goal 4: Add Thread Safety (Critical Section)
**Priority**: High  
**Risk**: Medium

**Task**: Protect shared data from concurrent access

**Files**:
- `GrablinkSnapshotDoc.h` - add `CRITICAL_SECTION` member
- `GrablinkSnapshotDoc.cpp` - initialize in constructor, lock in callback

**Implementation**:
- Add `CRITICAL_SECTION m_cs;` to class
- Initialize: `InitializeCriticalSection(&m_cs);` in constructor
- In `Callback`: `EnterCriticalSection(&m_cs);` ... `LeaveCriticalSection(&m_cs);`
- Delete in destructor: `DeleteCriticalSection(&m_cs);`

**Verification**:
- Stress test with high frame rate camera - no crashes, no image corruption

---

## Goal 5: Async File Saving (Background Thread)
**Priority**: High  
**Risk**: High

**Task**: Move BMP writing to background thread to avoid blocking callback

**Files**: `GrablinkSnapshotDoc.cpp`, `GrablinkSnapshotDoc.h`

**Implementation**:
- After all frames captured, spawn worker thread
- Thread writes BMPs, posts message when done
- UI shows "Saving..." state

**Verification**:
1. Capture 1500 images
2. After capture, UI remains responsive while files write
3. All BMPs saved correctly

---

## Goal 6: Error Handling and User Feedback
**Priority**: Medium  
**Risk**: Low

**Task**: Add robust error checking and user notifications

**Files**: `GrablinkSnapshotDoc.cpp`

**Implementation**:
- Check `malloc` return - show error if NULL
- Check `fopen` return - skip failed files, report count at end
- Show MessageBox for: allocation failure, save completion (X of Y saved)

**Verification**:
- Test: fill disk, use invalid path - graceful failure with message

---

## Notes
- Goals should be implemented in order - each builds on previous
- After each goal, verify expected behavior before moving on
- Goal 4 (thread safety) is prerequisite for Goal 5 (async save)
- Goal 1 + Live Preview feature: Both completed in single session