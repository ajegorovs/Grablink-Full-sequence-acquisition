// CaptureStatsTests.cpp - behavioural tests for the capture diagnostics
// aggregation core.
//
// Every test was added one at a time following RED -> GREEN: the test was
// compiled and executed against the current implementation first (RED), then
// the implementation was completed until the test passed (GREEN).
//
// The standard headers are taken in before the harness pulls in <windows.h>,
// so that no Win32 macro (min/max in particular) can disturb the library.

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <sstream>
#include <thread>
#include <vector>

#include "TestHarness.h"

#include "CaptureStats.h"

using grablinkcore::CaptureStats;
using grablinkcore::CaptureStatsSnapshot;

// ---------------------------------------------------------------------------
// Global allocation counting.
//
// The capture callback must not allocate, so CaptureStats::Record*() must not
// allocate either. Replacing the global operators is the only way to observe
// that from a test without changing the type under test: every C++ allocation
// in this executable is routed through the counting forms below. The count
// itself lives in the test harness (TestHarness.h, test::AllocationCount()) so
// that every test file can read one and the same observation; the counting
// increment is a relaxed atomic add, so it cannot perturb a measured window.
// ---------------------------------------------------------------------------

void* operator new(std::size_t size)
{
    test::AllocationCount().fetch_add(1, std::memory_order_relaxed);
    void* memory = std::malloc(size != 0 ? size : 1);
    if (memory == 0)
    {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    ::operator delete(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
// Floating point comparison that reports through the harness. Exact equality is
// used wherever the expected value is exactly representable, so this helper is
// only for the derived rate values where a tolerance documents the intent.
void CheckClose(const char* file, int line, double actual, double expected,
                double tolerance)
{
    const double difference = actual - expected;
    if (difference > tolerance || difference < -tolerance)
    {
        std::ostringstream stream;
        stream << "expected " << expected << " (+/- " << tolerance
               << ") but got " << actual;
        test::ReportFailure(file, line, stream.str());
    }
}
}

#define CHECK_CLOSE(actual, expected, tolerance)                               \
    CheckClose(__FILE__, __LINE__, (actual), (expected), (tolerance))

// ---------------------------------------------------------------------------
// Test 1: a freshly constructed aggregator reports nothing and has no window.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsStartsEmpty)
{
    CaptureStats stats;
    const CaptureStatsSnapshot snapshot = stats.Snapshot();

    CHECK_EQ(snapshot.surfacesReceived, 0ULL);
    CHECK_EQ(snapshot.framesStored, 0ULL);
    CHECK_EQ(snapshot.framesRejected, 0ULL);
    CHECK_EQ(snapshot.acquisitionFailures, 0ULL);
    CHECK_EQ(snapshot.previewPublished, 0ULL);
    CHECK_EQ(snapshot.previewDropped, 0ULL);
    CHECK_EQ(snapshot.callbackCount, 0ULL);
    CHECK_EQ(snapshot.callbackTotalMicroseconds, 0ULL);
    CHECK_EQ(snapshot.callbackMaxMicroseconds, 0ULL);
    CHECK_EQ(snapshot.captureStarted, false);
    CHECK_EQ(snapshot.captureFinished, false);
    CHECK_EQ(snapshot.captureBeginMicroseconds, 0ULL);
    CHECK_EQ(snapshot.captureEndMicroseconds, 0ULL);
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 0ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 0ULL);
    CHECK_EQ(snapshot.HasCaptureWindow(), false);
    CHECK_EQ(snapshot.CaptureDurationMicroseconds(), 0ULL);
    CHECK_CLOSE(snapshot.CaptureDurationSeconds(), 0.0, 1e-12);
    CHECK_CLOSE(snapshot.EffectiveFps(), 0.0, 1e-12);
}

// ---------------------------------------------------------------------------
// Test 2: every counter records exactly the events it was told about, and the
// counters are independent of one another.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsCountsEachEventExactly)
{
    CaptureStats stats;

    stats.RecordSurfaceReceived();
    stats.RecordSurfaceReceived();
    stats.RecordSurfaceReceived();
    stats.RecordFrameStored();
    stats.RecordFrameStored();
    stats.RecordFrameRejected();
    stats.RecordAcquisitionFailure();
    stats.RecordPreviewPublished();
    stats.RecordPreviewPublished();
    stats.RecordPreviewPublished();
    stats.RecordPreviewDropped();

    const CaptureStatsSnapshot snapshot = stats.Snapshot();

    CHECK_EQ(snapshot.surfacesReceived, 3ULL);
    CHECK_EQ(snapshot.framesStored, 2ULL);
    CHECK_EQ(snapshot.framesRejected, 1ULL);
    CHECK_EQ(snapshot.acquisitionFailures, 1ULL);
    CHECK_EQ(snapshot.previewPublished, 3ULL);
    CHECK_EQ(snapshot.previewDropped, 1ULL);
    // Recording events must not fabricate callback timings.
    CHECK_EQ(snapshot.callbackCount, 0ULL);
    CHECK_EQ(snapshot.callbackTotalMicroseconds, 0ULL);
    CHECK_EQ(snapshot.callbackMaxMicroseconds, 0ULL);
}

// ---------------------------------------------------------------------------
// Test 3: the callback reports count, running total and worst case duration.
// The maximum must survive later, faster callbacks.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsTracksCallbackCountTotalAndMax)
{
    CaptureStats stats;

    stats.RecordCallback(100);
    stats.RecordCallback(250);
    stats.RecordCallback(40);
    stats.RecordCallback(250);

    const CaptureStatsSnapshot snapshot = stats.Snapshot();

    CHECK_EQ(snapshot.callbackCount, 4ULL);
    CHECK_EQ(snapshot.callbackTotalMicroseconds, 640ULL);
    CHECK_EQ(snapshot.callbackMaxMicroseconds, 250ULL);
}

// ---------------------------------------------------------------------------
// Test 4: a zero length callback is counted like any other, and the maximum
// starts from the very first duration reported.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsCountsZeroLengthCallback)
{
    CaptureStats stats;

    stats.RecordCallback(0);
    stats.RecordCallback(0);

    CaptureStatsSnapshot snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.callbackCount, 2ULL);
    CHECK_EQ(snapshot.callbackTotalMicroseconds, 0ULL);
    CHECK_EQ(snapshot.callbackMaxMicroseconds, 0ULL);

    stats.RecordCallback(7);
    snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.callbackCount, 3ULL);
    CHECK_EQ(snapshot.callbackTotalMicroseconds, 7ULL);
    CHECK_EQ(snapshot.callbackMaxMicroseconds, 7ULL);
}

// ---------------------------------------------------------------------------
// Test 5: Reset returns the aggregator to its constructed state, including the
// capture window, and the aggregator is usable again afterwards.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsResetClearsEverything)
{
    CaptureStats stats;

    stats.RecordSurfaceReceived();
    stats.RecordFrameStored();
    stats.RecordFrameRejected();
    stats.RecordAcquisitionFailure();
    stats.RecordPreviewPublished();
    stats.RecordPreviewDropped();
    stats.RecordCallback(900);
    stats.BeginCapture(1000);
    stats.EndCapture(2000);

    stats.Reset();

    const CaptureStatsSnapshot snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.surfacesReceived, 0ULL);
    CHECK_EQ(snapshot.framesStored, 0ULL);
    CHECK_EQ(snapshot.framesRejected, 0ULL);
    CHECK_EQ(snapshot.acquisitionFailures, 0ULL);
    CHECK_EQ(snapshot.previewPublished, 0ULL);
    CHECK_EQ(snapshot.previewDropped, 0ULL);
    CHECK_EQ(snapshot.callbackCount, 0ULL);
    CHECK_EQ(snapshot.callbackTotalMicroseconds, 0ULL);
    CHECK_EQ(snapshot.callbackMaxMicroseconds, 0ULL);
    CHECK_EQ(snapshot.captureStarted, false);
    CHECK_EQ(snapshot.captureFinished, false);
    CHECK_EQ(snapshot.captureBeginMicroseconds, 0ULL);
    CHECK_EQ(snapshot.captureEndMicroseconds, 0ULL);
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 0ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 0ULL);

    // Still usable, and recording after a reset counts from zero again.
    stats.RecordFrameStored();
    stats.RecordCallback(5);
    stats.BeginCapture(3000);
    stats.EndCapture(4000);

    const CaptureStatsSnapshot second = stats.Snapshot();
    CHECK_EQ(second.framesStored, 1ULL);
    CHECK_EQ(second.callbackCount, 1ULL);
    CHECK_EQ(second.callbackMaxMicroseconds, 5ULL);
    CHECK_EQ(second.CaptureDurationMicroseconds(), 1000ULL);
}

// ---------------------------------------------------------------------------
// Test 6: a snapshot is an independent copy. Later recording must not change a
// snapshot that was already taken.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsSnapshotIsIndependentCopy)
{
    CaptureStats stats;
    stats.RecordFrameStored();

    const CaptureStatsSnapshot before = stats.Snapshot();
    stats.RecordFrameStored();
    stats.RecordCallback(11);

    CHECK_EQ(before.framesStored, 1ULL);
    CHECK_EQ(before.callbackCount, 0ULL);
    CHECK_EQ(stats.Snapshot().framesStored, 2ULL);
    CHECK_EQ(stats.Snapshot().callbackCount, 1ULL);
}

// ---------------------------------------------------------------------------
// Test 7: the capture window is whatever the caller supplies. Duration is the
// exact difference in integer microseconds.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsComputesCaptureDurationFromCallerTimestamps)
{
    CaptureStats stats;

    stats.BeginCapture(1000000);
    CaptureStatsSnapshot snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.captureStarted, true);
    CHECK_EQ(snapshot.captureFinished, false);
    CHECK_EQ(snapshot.captureBeginMicroseconds, 1000000ULL);
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 0ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 0ULL);
    CHECK_EQ(snapshot.HasCaptureWindow(), false);
    CHECK_EQ(snapshot.CaptureDurationMicroseconds(), 0ULL);

    stats.EndCapture(3000000);
    snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.captureFinished, true);
    CHECK_EQ(snapshot.captureEndMicroseconds, 3000000ULL);
    CHECK_EQ(snapshot.HasCaptureWindow(), true);
    CHECK_EQ(snapshot.CaptureDurationMicroseconds(), 2000000ULL);
    CHECK_CLOSE(snapshot.CaptureDurationSeconds(), 2.0, 1e-12);
}

// ---------------------------------------------------------------------------
// Test 8: effective fps is the frames stored during the window over the caller
// supplied duration. Frames stored before BeginCapture() belong to an earlier
// run: they stay in the cumulative counter but not in the window.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsComputesEffectiveFps)
{
    CaptureStats stats;
    stats.BeginCapture(1000000);
    for (int index = 0; index < 120; ++index)
    {
        stats.RecordFrameStored();
    }
    stats.EndCapture(3000000);

    const CaptureStatsSnapshot snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 0ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 120ULL);
    CHECK_CLOSE(snapshot.CaptureDurationSeconds(), 2.0, 1e-12);
    CHECK_CLOSE(snapshot.EffectiveFps(), 60.0, 1e-9);

    // A fractional duration must not be truncated to whole seconds.
    CaptureStats other;
    other.BeginCapture(0);
    other.RecordFrameStored();
    other.RecordFrameStored();
    other.RecordFrameStored();
    other.EndCapture(1500000);

    const CaptureStatsSnapshot otherSnapshot = other.Snapshot();
    CHECK_CLOSE(otherSnapshot.CaptureDurationSeconds(), 1.5, 1e-12);
    CHECK_CLOSE(otherSnapshot.EffectiveFps(), 2.0, 1e-9);

    // Frames recorded before the window began are outside it.
    CaptureStats carried;
    for (int index = 0; index < 5; ++index)
    {
        carried.RecordFrameStored();
    }
    carried.BeginCapture(0);
    carried.RecordFrameStored();
    carried.RecordFrameStored();
    carried.EndCapture(1000000);

    const CaptureStatsSnapshot carriedSnapshot = carried.Snapshot();
    CHECK_EQ(carriedSnapshot.framesStored, 7ULL);
    CHECK_EQ(carriedSnapshot.framesStoredAtCaptureBegin, 5ULL);
    CHECK_EQ(carriedSnapshot.CaptureWindowFramesStored(), 2ULL);
    CHECK_CLOSE(carriedSnapshot.EffectiveFps(), 2.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Test 9: the zero guards. No window, a zero length window, an inverted window
// and an empty capture all report zero instead of dividing by zero or
// underflowing.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsGuardsAgainstZeroAndInvertedWindows)
{
    // Never started.
    CaptureStats never;
    never.RecordFrameStored();
    CHECK_EQ(never.Snapshot().CaptureDurationMicroseconds(), 0ULL);
    CHECK_CLOSE(never.Snapshot().CaptureDurationSeconds(), 0.0, 1e-12);
    CHECK_CLOSE(never.Snapshot().EffectiveFps(), 0.0, 1e-12);

    // Started but never finished.
    CaptureStats unfinished;
    unfinished.RecordFrameStored();
    unfinished.BeginCapture(500);
    CHECK_EQ(unfinished.Snapshot().HasCaptureWindow(), false);
    CHECK_EQ(unfinished.Snapshot().CaptureDurationMicroseconds(), 0ULL);
    CHECK_CLOSE(unfinished.Snapshot().EffectiveFps(), 0.0, 1e-12);

    // Zero length window.
    CaptureStats zeroLength;
    zeroLength.RecordFrameStored();
    zeroLength.BeginCapture(500);
    zeroLength.EndCapture(500);
    CHECK_EQ(zeroLength.Snapshot().HasCaptureWindow(), false);
    CHECK_EQ(zeroLength.Snapshot().CaptureDurationMicroseconds(), 0ULL);
    CHECK_CLOSE(zeroLength.Snapshot().CaptureDurationSeconds(), 0.0, 1e-12);
    CHECK_CLOSE(zeroLength.Snapshot().EffectiveFps(), 0.0, 1e-12);

    // Inverted window must not wrap around into a huge duration.
    CaptureStats inverted;
    inverted.RecordFrameStored();
    inverted.BeginCapture(900);
    inverted.EndCapture(400);
    CHECK_EQ(inverted.Snapshot().HasCaptureWindow(), false);
    CHECK_EQ(inverted.Snapshot().CaptureDurationMicroseconds(), 0ULL);
    CHECK_CLOSE(inverted.Snapshot().EffectiveFps(), 0.0, 1e-12);

    // A valid window with no stored frames is zero fps, not an error.
    CaptureStats empty;
    empty.BeginCapture(0);
    empty.EndCapture(1000000);
    CHECK_EQ(empty.Snapshot().HasCaptureWindow(), true);
    CHECK_CLOSE(empty.Snapshot().CaptureDurationSeconds(), 1.0, 1e-12);
    CHECK_CLOSE(empty.Snapshot().EffectiveFps(), 0.0, 1e-12);
}

// ---------------------------------------------------------------------------
// Test 10: the record methods, Reset and Snapshot must not allocate. This is
// what makes them safe to call from the acquisition callback.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsRecordMethodsDoNotAllocate)
{
    CaptureStats stats;

    const long before = test::AllocationCount().load(std::memory_order_relaxed);

    for (int index = 0; index < 1000; ++index)
    {
        stats.RecordSurfaceReceived();
        stats.RecordFrameStored();
        stats.RecordFrameRejected();
        stats.RecordAcquisitionFailure();
        stats.RecordPreviewPublished();
        stats.RecordPreviewDropped();
        stats.RecordCallback(static_cast<unsigned long long>(index));
    }
    stats.BeginCapture(10);
    stats.EndCapture(20);
    CaptureStatsSnapshot snapshot = stats.Snapshot();
    stats.Reset();
    snapshot = stats.Snapshot();

    const long after = test::AllocationCount().load(std::memory_order_relaxed);

    CHECK_EQ(after - before, 0L);
    CHECK_EQ(snapshot.framesStored, 0ULL);
}

// ---------------------------------------------------------------------------
// Test 11: concurrent increments must lose nothing. Four threads record the
// same number of every event; after joining, every counter holds the exact
// total and nothing else.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsConcurrentIncrementsAreExact)
{
    const int kThreads = 4;
    const int kIterations = 20000;

    CaptureStats stats;
    std::vector<std::thread> workers;
    for (int thread = 0; thread < kThreads; ++thread)
    {
        workers.push_back(std::thread([&stats, kIterations]()
        {
            for (int index = 0; index < kIterations; ++index)
            {
                stats.RecordSurfaceReceived();
                stats.RecordFrameStored();
                stats.RecordFrameRejected();
                stats.RecordAcquisitionFailure();
                stats.RecordPreviewPublished();
                stats.RecordPreviewDropped();
            }
        }));
    }
    for (std::size_t index = 0; index < workers.size(); ++index)
    {
        workers[index].join();
    }

    const unsigned long long expected =
        static_cast<unsigned long long>(kThreads) *
        static_cast<unsigned long long>(kIterations);
    const CaptureStatsSnapshot snapshot = stats.Snapshot();

    CHECK_EQ(snapshot.surfacesReceived, expected);
    CHECK_EQ(snapshot.framesStored, expected);
    CHECK_EQ(snapshot.framesRejected, expected);
    CHECK_EQ(snapshot.acquisitionFailures, expected);
    CHECK_EQ(snapshot.previewPublished, expected);
    CHECK_EQ(snapshot.previewDropped, expected);
}

// ---------------------------------------------------------------------------
// Test 12: concurrent callback durations must give the exact count, the exact
// total and the true maximum, even when the worst case is recorded by the
// thread that runs last.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsConcurrentCallbackDurationsAreExact)
{
    const int kThreads = 4;
    const int kIterations = 5000;

    CaptureStats stats;
    std::vector<std::thread> workers;
    for (int thread = 0; thread < kThreads; ++thread)
    {
        workers.push_back(std::thread([&stats, thread, kIterations]()
        {
            const unsigned long long duration =
                static_cast<unsigned long long>(thread + 1) * 1000ULL;
            for (int index = 0; index < kIterations; ++index)
            {
                stats.RecordCallback(duration);
            }
        }));
    }
    for (std::size_t index = 0; index < workers.size(); ++index)
    {
        workers[index].join();
    }

    const unsigned long long count =
        static_cast<unsigned long long>(kThreads) *
        static_cast<unsigned long long>(kIterations);
    // Each thread contributes duration * kIterations to the total.
    unsigned long long total = 0;
    for (int thread = 0; thread < kThreads; ++thread)
    {
        total += static_cast<unsigned long long>(thread + 1) * 1000ULL *
                 static_cast<unsigned long long>(kIterations);
    }

    const CaptureStatsSnapshot snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.callbackCount, count);
    CHECK_EQ(snapshot.callbackTotalMicroseconds, total);
    CHECK_EQ(snapshot.callbackMaxMicroseconds,
             static_cast<unsigned long long>(kThreads) * 1000ULL);
}

// ---------------------------------------------------------------------------
// Test 13: a Reset racing live recording must not corrupt anything. Counters
// may lose increments that straddle the reset, but never exceed what was
// recorded, and a final reset must leave an empty, still usable aggregator.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsResetIsRaceSafe)
{
    const int kThreads = 4;
    const int kIterations = 20000;

    CaptureStats stats;
    std::vector<std::thread> workers;
    for (int thread = 0; thread < kThreads; ++thread)
    {
        workers.push_back(std::thread([&stats, kIterations]()
        {
            for (int index = 0; index < kIterations; ++index)
            {
                stats.RecordFrameStored();
                stats.RecordCallback(3);
                // Read concurrently as well: Snapshot must stay safe to call
                // from another thread while the callback records.
                const CaptureStatsSnapshot observed = stats.Snapshot();
                (void)observed;
            }
        }));
    }

    for (int index = 0; index < 200; ++index)
    {
        stats.Reset();
    }

    for (std::size_t index = 0; index < workers.size(); ++index)
    {
        workers[index].join();
    }

    const unsigned long long recorded =
        static_cast<unsigned long long>(kThreads) *
        static_cast<unsigned long long>(kIterations);
    const CaptureStatsSnapshot snapshot = stats.Snapshot();
    CHECK(snapshot.framesStored <= recorded);
    CHECK(snapshot.callbackCount <= recorded);

    stats.Reset();
    const CaptureStatsSnapshot cleared = stats.Snapshot();
    CHECK_EQ(cleared.framesStored, 0ULL);
    CHECK_EQ(cleared.callbackCount, 0ULL);
    CHECK_EQ(cleared.callbackTotalMicroseconds, 0ULL);
    CHECK_EQ(cleared.callbackMaxMicroseconds, 0ULL);

    // Usable again.
    stats.RecordFrameStored();
    CHECK_EQ(stats.Snapshot().framesStored, 1ULL);
}

// ---------------------------------------------------------------------------
// Test 19: one run's reported rate must not be inflated by an earlier run's
// stored frames. framesStored stays cumulative - it is a per-document
// diagnostic total - but EffectiveFps() describes one window, so it may only
// count the frames stored during the window it reports.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsEffectiveFpsCountsOnlyFramesStoredSinceWindowBegan)
{
    CaptureStats stats;

    // Run 1: 100 frames over 1 s -> 100 fps.
    stats.BeginCapture(1000000);
    for (int index = 0; index < 100; ++index)
    {
        stats.RecordFrameStored();
    }
    stats.EndCapture(2000000);

    CaptureStatsSnapshot snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.framesStored, 100ULL);
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 0ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 100ULL);
    CHECK_EQ(snapshot.CaptureDurationMicroseconds(), 1000000ULL);
    CHECK_CLOSE(snapshot.EffectiveFps(), 100.0, 1e-9);

    // Run 2: 200 frames over 2 s -> 100 fps. The cumulative total is now 300,
    // so dividing the whole of it by this window would report 150 fps.
    stats.BeginCapture(2000000);
    // The baseline moves with BeginCapture(): the new window starts empty even
    // though 100 frames are already in the cumulative counter.
    snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 100ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 0ULL);
    for (int index = 0; index < 200; ++index)
    {
        stats.RecordFrameStored();
    }
    stats.EndCapture(4000000);

    snapshot = stats.Snapshot();
    // The cumulative counter itself is preserved: it is a diagnostic total
    // for the whole document, not a per-window figure.
    CHECK_EQ(snapshot.framesStored, 300ULL);
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 100ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 200ULL);
    CHECK_EQ(snapshot.CaptureDurationMicroseconds(), 2000000ULL);
    CHECK_CLOSE(snapshot.EffectiveFps(), 100.0, 1e-9);

    // A third run of 300 frames over 3 s is 100 fps again, whatever the
    // cumulative total has grown to.
    stats.BeginCapture(5000000);
    for (int index = 0; index < 300; ++index)
    {
        stats.RecordFrameStored();
    }
    stats.EndCapture(8000000);

    snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.framesStored, 600ULL);
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 300ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 300ULL);
    CHECK_CLOSE(snapshot.EffectiveFps(), 100.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Test 20: a capture that is started and then stopped without ever finishing
// is still a window boundary. The run that follows must measure its rate from
// its own begin, not from the last run that happened to end, and the abandoned
// window itself reports no rate at all because it has no usable end.
// ---------------------------------------------------------------------------
TEST_CASE(CaptureStatsEffectiveFpsUsesItsOwnBaselineAfterAbandonedCapture)
{
    CaptureStats stats;

    // Run 1: 100 frames over 1 s -> 100 fps.
    stats.BeginCapture(0);
    for (int index = 0; index < 100; ++index)
    {
        stats.RecordFrameStored();
    }
    stats.EndCapture(1000000);
    CHECK_CLOSE(stats.Snapshot().EffectiveFps(), 100.0, 1e-9);

    // A discarded run: started, 50 frames stored, never ended. It has no
    // usable window, so it reports no rate even though frames were stored.
    stats.BeginCapture(2000000);
    for (int index = 0; index < 50; ++index)
    {
        stats.RecordFrameStored();
    }

    CaptureStatsSnapshot snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.framesStored, 150ULL);
    // The discarded run opened a window of its own: its 50 frames are the
    // window's frames, they are simply not turned into a rate because the
    // window never finished.
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 100ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 50ULL);
    CHECK_EQ(snapshot.HasCaptureWindow(), false);
    CHECK_EQ(snapshot.CaptureDurationMicroseconds(), 0ULL);
    CHECK_CLOSE(snapshot.EffectiveFps(), 0.0, 1e-12);

    // Run 2: 200 frames over 2 s -> 100 fps. Its window began after the
    // discarded run stored its frames, so those frames are outside it: both
    // the run 1 total (100) and the discarded 50 would otherwise be counted.
    stats.BeginCapture(3000000);
    for (int index = 0; index < 200; ++index)
    {
        stats.RecordFrameStored();
    }
    stats.EndCapture(5000000);

    snapshot = stats.Snapshot();
    CHECK_EQ(snapshot.framesStored, 350ULL);
    CHECK_EQ(snapshot.framesStoredAtCaptureBegin, 150ULL);
    CHECK_EQ(snapshot.CaptureWindowFramesStored(), 200ULL);
    CHECK_EQ(snapshot.HasCaptureWindow(), true);
    CHECK_EQ(snapshot.CaptureDurationMicroseconds(), 2000000ULL);
    CHECK_CLOSE(snapshot.EffectiveFps(), 100.0, 1e-9);
}
