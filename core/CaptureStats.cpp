// CaptureStats.cpp - implementation of the capture diagnostics aggregator.
//
// Everything here is a relaxed atomic operation on a fixed set of counters:
// no lock, no allocation, no clock, no string. The only ordering discipline is
// around the capture window, where the "started"/"finished" flags are published
// with release stores and read with acquire loads so that a reader who sees a
// flag also sees the timestamp that goes with it. The counters themselves do not
// need ordering: they are independent totals, and a snapshot taken during live
// recording is documented to be a close, not a transactional, view.

#include "CaptureStats.h"

namespace grablinkcore
{

// The callback path must be lock free on the supported x64 target; if this ever
// stops holding, CaptureStats is no longer safe to call from the callback and
// the design has to change rather than silently take a lock.
static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
              "CaptureStats requires lock free 64-bit atomics");
static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "CaptureStats requires lock free 32-bit atomics");

namespace
{
const unsigned long long kMicrosecondsPerSecond = 1000000ULL;
}

// --- CaptureStatsSnapshot --------------------------------------------------

CaptureStatsSnapshot::CaptureStatsSnapshot()
    : surfacesReceived(0),
      framesStored(0),
      framesRejected(0),
      acquisitionFailures(0),
      previewPublished(0),
      previewDropped(0),
      callbackCount(0),
      callbackTotalMicroseconds(0),
      callbackMaxMicroseconds(0),
      captureStarted(false),
      captureFinished(false),
      captureBeginMicroseconds(0),
      captureEndMicroseconds(0),
      framesStoredAtCaptureBegin(0)
{
}

bool CaptureStatsSnapshot::HasCaptureWindow() const
{
    return captureStarted && captureFinished &&
           captureEndMicroseconds > captureBeginMicroseconds;
}

unsigned long long CaptureStatsSnapshot::CaptureDurationMicroseconds() const
{
    if (!HasCaptureWindow())
    {
        return 0;
    }
    return captureEndMicroseconds - captureBeginMicroseconds;
}

double CaptureStatsSnapshot::CaptureDurationSeconds() const
{
    return static_cast<double>(CaptureDurationMicroseconds()) /
           static_cast<double>(kMicrosecondsPerSecond);
}

double CaptureStatsSnapshot::EffectiveFps() const
{
    const unsigned long long duration = CaptureDurationMicroseconds();
    if (duration == 0)
    {
        return 0.0;
    }

    // Only the frames this window stored may be turned into a rate: the
    // cumulative framesStored total spans every capture this aggregator has
    // seen, so using it here would inflate the rate more with every run.
    return static_cast<double>(CaptureWindowFramesStored()) /
           (static_cast<double>(duration) / static_cast<double>(kMicrosecondsPerSecond));
}

unsigned long long CaptureStatsSnapshot::CaptureWindowFramesStored() const
{
    if (!captureStarted)
    {
        // No window: frames recorded here belong to no run.
        return 0;
    }
    if (framesStored <= framesStoredAtCaptureBegin)
    {
        // Nothing stored since the window began, or a baseline that a racing
        // Reset left ahead of the counter. Clamp rather than wrap around into
        // a huge frame count.
        return 0;
    }
    return framesStored - framesStoredAtCaptureBegin;
}

// --- CaptureStats ----------------------------------------------------------

CaptureStats::CaptureStats()
    : m_surfacesReceived(0),
      m_framesStored(0),
      m_framesRejected(0),
      m_acquisitionFailures(0),
      m_previewPublished(0),
      m_previewDropped(0),
      m_callbackCount(0),
      m_callbackTotalMicroseconds(0),
      m_callbackMaxMicroseconds(0),
      m_captureStarted(0),
      m_captureFinished(0),
      m_captureBeginMicroseconds(0),
      m_captureEndMicroseconds(0),
      m_framesStoredAtCaptureBegin(0)
{
}

void CaptureStats::Reset() noexcept
{
    m_surfacesReceived.store(0, std::memory_order_relaxed);
    m_framesStored.store(0, std::memory_order_relaxed);
    m_framesRejected.store(0, std::memory_order_relaxed);
    m_acquisitionFailures.store(0, std::memory_order_relaxed);
    m_previewPublished.store(0, std::memory_order_relaxed);
    m_previewDropped.store(0, std::memory_order_relaxed);

    m_callbackCount.store(0, std::memory_order_relaxed);
    m_callbackTotalMicroseconds.store(0, std::memory_order_relaxed);
    m_callbackMaxMicroseconds.store(0, std::memory_order_relaxed);

    // Clear the flags before the timestamps so that a reader racing the reset
    // can never observe a window made of a new flag and an old timestamp.
    m_captureStarted.store(0, std::memory_order_relaxed);
    m_captureFinished.store(0, std::memory_order_relaxed);
    m_captureBeginMicroseconds.store(0, std::memory_order_relaxed);
    m_captureEndMicroseconds.store(0, std::memory_order_relaxed);
    m_framesStoredAtCaptureBegin.store(0, std::memory_order_relaxed);
}

void CaptureStats::RecordSurfaceReceived() noexcept
{
    m_surfacesReceived.fetch_add(1, std::memory_order_relaxed);
}

void CaptureStats::RecordFrameStored() noexcept
{
    m_framesStored.fetch_add(1, std::memory_order_relaxed);
}

void CaptureStats::RecordFrameRejected() noexcept
{
    m_framesRejected.fetch_add(1, std::memory_order_relaxed);
}

void CaptureStats::RecordAcquisitionFailure() noexcept
{
    m_acquisitionFailures.fetch_add(1, std::memory_order_relaxed);
}

void CaptureStats::RecordPreviewPublished() noexcept
{
    m_previewPublished.fetch_add(1, std::memory_order_relaxed);
}

void CaptureStats::RecordPreviewDropped() noexcept
{
    m_previewDropped.fetch_add(1, std::memory_order_relaxed);
}

void CaptureStats::RecordCallback(unsigned long long durationMicroseconds) noexcept
{
    m_callbackCount.fetch_add(1, std::memory_order_relaxed);
    m_callbackTotalMicroseconds.fetch_add(durationMicroseconds,
                                          std::memory_order_relaxed);

    // Raise the maximum without a lock. compare_exchange_weak refreshes
    // "observed" on failure, so the loop converges on the current value and
    // never lowers a maximum another thread has already published.
    unsigned long long observed =
        m_callbackMaxMicroseconds.load(std::memory_order_relaxed);
    while (observed < durationMicroseconds &&
           !m_callbackMaxMicroseconds.compare_exchange_weak(
               observed, durationMicroseconds, std::memory_order_relaxed,
               std::memory_order_relaxed))
    {
    }
}

void CaptureStats::BeginCapture(unsigned long long beginMicroseconds) noexcept
{
    // A new capture must not inherit the previous one's end timestamp.
    m_captureFinished.store(0, std::memory_order_relaxed);
    m_captureEndMicroseconds.store(0, std::memory_order_relaxed);
    m_captureBeginMicroseconds.store(beginMicroseconds, std::memory_order_relaxed);

    // Freeze the cumulative frame count as this window's baseline. Every
    // BeginCapture() takes a fresh baseline, so a window that is started and
    // then abandoned without an EndCapture() still cannot leak its frames into
    // the next window's rate. Stored before the release below, so a reader who
    // acquires "started" also sees the baseline that belongs to it.
    m_framesStoredAtCaptureBegin.store(m_framesStored.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
    m_captureStarted.store(1, std::memory_order_release);
}

void CaptureStats::EndCapture(unsigned long long endMicroseconds) noexcept
{
    m_captureEndMicroseconds.store(endMicroseconds, std::memory_order_relaxed);
    m_captureFinished.store(1, std::memory_order_release);
}

CaptureStatsSnapshot CaptureStats::Snapshot() const noexcept
{
    CaptureStatsSnapshot snapshot;

    snapshot.surfacesReceived =
        m_surfacesReceived.load(std::memory_order_relaxed);
    snapshot.framesStored = m_framesStored.load(std::memory_order_relaxed);
    snapshot.framesRejected = m_framesRejected.load(std::memory_order_relaxed);
    snapshot.acquisitionFailures =
        m_acquisitionFailures.load(std::memory_order_relaxed);
    snapshot.previewPublished =
        m_previewPublished.load(std::memory_order_relaxed);
    snapshot.previewDropped = m_previewDropped.load(std::memory_order_relaxed);

    snapshot.callbackCount = m_callbackCount.load(std::memory_order_relaxed);
    snapshot.callbackTotalMicroseconds =
        m_callbackTotalMicroseconds.load(std::memory_order_relaxed);
    snapshot.callbackMaxMicroseconds =
        m_callbackMaxMicroseconds.load(std::memory_order_relaxed);

    // Acquire the flags before the timestamps they publish, so a window is
    // never assembled from a fresh flag and a stale timestamp.
    snapshot.captureStarted =
        m_captureStarted.load(std::memory_order_acquire) != 0;
    snapshot.captureFinished =
        m_captureFinished.load(std::memory_order_acquire) != 0;
    snapshot.captureBeginMicroseconds =
        m_captureBeginMicroseconds.load(std::memory_order_relaxed);
    snapshot.captureEndMicroseconds =
        m_captureEndMicroseconds.load(std::memory_order_relaxed);
    // Loaded after the flags with the timestamp the window began at, so a
    // window is never assembled from a fresh flag and a stale baseline.
    snapshot.framesStoredAtCaptureBegin =
        m_framesStoredAtCaptureBegin.load(std::memory_order_relaxed);

    return snapshot;
}

} // namespace grablinkcore
