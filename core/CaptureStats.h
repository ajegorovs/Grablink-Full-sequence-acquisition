#pragma once

// CaptureStats.h - hardware-free, callback-safe aggregate capture diagnostics.
//
// The acquisition callback runs at frame rate on a driver thread and must stay
// short, deterministic and free of allocation, string formatting, file-system
// work and clock reads. CaptureStats is the diagnostics sink for that thread:
// every Record*() method is a handful of relaxed atomic operations, so it never
// allocates, never blocks, never throws and never reads a clock. That makes it
// safe to call from the callback, which is where the numbers have to be
// collected if they are to be trusted.
//
// The caller owns the clock. Capture start and end are handed in as integer
// microseconds through BeginCapture() and EndCapture(), and each callback
// reports its own measured duration through RecordCallback(). No time source is
// baked into this type, so the timing policy (which clock, which units, whether
// durations are measured at all) stays with the capture code, and the type is
// testable without hardware or a running clock.
//
// Snapshot() returns a plain, self-contained copy of every counter, so a
// reporting thread (UI or logger) can read a value that no longer moves while
// the callback keeps recording. Each counter is individually atomic: a snapshot
// taken while recording is live may mix values from slightly different
// instants, and a Reset() that races a concurrent Record*() may or may not
// count that record. Once recording has stopped, a snapshot is exact.
//
// The file contains nothing but the C++ standard library: no MFC, no MultiCam
// and no external dependency, so it can be unit tested without hardware.

#include <atomic>

namespace grablinkcore
{

// --- CaptureStatsSnapshot --------------------------------------------------

// Consistent-by-value copy of every counter, plus the derived capture window
// metrics. Plain data: copying it is a memcpy's worth of work and never
// allocates.
struct CaptureStatsSnapshot
{
    // Surfaces handed to the callback by the grabber.
    unsigned long long surfacesReceived;

    // Frames the callback copied into the capture buffer, and frames it had to
    // drop (buffer full, out of range, refused).
    unsigned long long framesStored;
    unsigned long long framesRejected;

    // Acquisition level failures: channel errors, missing surfaces, timeouts.
    unsigned long long acquisitionFailures;

    // Preview frames handed to the UI, and preview frames skipped because the
    // UI was not keeping up.
    unsigned long long previewPublished;
    unsigned long long previewDropped;

    // Callback cost. Count and total come from RecordCallback(); the maximum is
    // the worst single duration seen since the last Reset().
    unsigned long long callbackCount;
    unsigned long long callbackTotalMicroseconds;
    unsigned long long callbackMaxMicroseconds;

    // Capture window, exactly as supplied by the caller.
    bool captureStarted;
    bool captureFinished;
    unsigned long long captureBeginMicroseconds;
    unsigned long long captureEndMicroseconds;

    CaptureStatsSnapshot();

    // True when a usable window exists: the capture started, finished, and the
    // end is strictly after the begin. A zero length or inverted window is not
    // usable and every derived metric below reports zero for it, so a bad or
    // missing timestamp can never turn into a division by zero or a wrapped
    // around duration.
    bool HasCaptureWindow() const;

    // End - begin in integer microseconds, or 0 when there is no usable window.
    unsigned long long CaptureDurationMicroseconds() const;

    // Duration in seconds, or 0.0 when there is no usable window.
    double CaptureDurationSeconds() const;

    // Stored frames per second over the capture window, or 0.0 when there is no
    // usable window or no stored frame.
    double EffectiveFps() const;
};

// --- CaptureStats ----------------------------------------------------------

// Aggregate acquisition diagnostics. Thread safe: one or more recording threads
// (the callback) may call the Record*() methods while any other thread reads
// Snapshot() or calls Reset(). The type holds no resource other than its
// counters, so it can be embedded directly in the capture owner.
//
// Atomic counters, not a lock: the callback path must never contend with a
// reporting thread, and on x64 every counter here is lock free.
class CaptureStats
{
public:
    CaptureStats();

    // Returns the aggregator to its constructed state: every counter is zero
    // and no capture window exists. Safe to call while another thread records;
    // a record that races the reset may or may not survive it.
    void Reset() noexcept;

    // --- recording: called from the acquisition callback -------------------
    // None of these allocates, blocks, throws or reads a clock.

    void RecordSurfaceReceived() noexcept;
    void RecordFrameStored() noexcept;
    void RecordFrameRejected() noexcept;
    void RecordAcquisitionFailure() noexcept;
    void RecordPreviewPublished() noexcept;
    void RecordPreviewDropped() noexcept;

    // Counts one callback whose measured cost was "durationMicroseconds",
    // adds that cost to the running total and raises the maximum if it is the
    // worst seen so far.
    void RecordCallback(unsigned long long durationMicroseconds) noexcept;

    // --- capture window: supplied by the caller ----------------------------

    // Marks the capture as started at "beginMicroseconds" (any caller chosen
    // monotonic microsecond origin). Clears a previously finished window.
    void BeginCapture(unsigned long long beginMicroseconds) noexcept;

    // Marks the capture as finished at "endMicroseconds".
    void EndCapture(unsigned long long endMicroseconds) noexcept;

    // --- reading -----------------------------------------------------------

    // Race free copy of the current state. Safe to call at any time, including
    // from another thread while the callback records.
    CaptureStatsSnapshot Snapshot() const noexcept;

private:
    CaptureStats(const CaptureStats&);
    CaptureStats& operator=(const CaptureStats&);

    std::atomic<unsigned long long> m_surfacesReceived;
    std::atomic<unsigned long long> m_framesStored;
    std::atomic<unsigned long long> m_framesRejected;
    std::atomic<unsigned long long> m_acquisitionFailures;
    std::atomic<unsigned long long> m_previewPublished;
    std::atomic<unsigned long long> m_previewDropped;

    std::atomic<unsigned long long> m_callbackCount;
    std::atomic<unsigned long long> m_callbackTotalMicroseconds;
    std::atomic<unsigned long long> m_callbackMaxMicroseconds;

    std::atomic<unsigned int> m_captureStarted;
    std::atomic<unsigned int> m_captureFinished;
    std::atomic<unsigned long long> m_captureBeginMicroseconds;
    std::atomic<unsigned long long> m_captureEndMicroseconds;
};

} // namespace grablinkcore
