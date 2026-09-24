// CallbackDrainTests.cpp - behavioural tests for the callback admission gate.
//
// Every test in this file was written before the implementation existed and was
// executed against it: RED first (the first CallbackDrain.cpp was a stub that
// admitted everything, never closed the gate and always claimed a drain had
// completed), then GREEN once CallbackDrain.cpp implemented the ordering
// documented in CallbackDrain.h. The tests cover the interleavings a teardown
// actually has to survive, not just the happy path:
//
//   * a callback that is already running when Disable() arrives keeps the drain
//     from reporting success until it has left;
//   * a callback that arrives after Disable() is refused and never runs;
//   * a bounded drain fails on time, succeeds on a later attempt, and is
//     reusable for the next capture generation;
//   * the RAII guard leaves exactly once on every path: scope exit, early
//     release, move construction, move assignment, refused entry;
//   * an unbalanced Leave() is reported instead of underflowing the counter.
//
// The standard headers are taken in before the harness pulls in <windows.h>, so
// that no Win32 macro can disturb the library.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "TestHarness.h"

#include "CallbackDrain.h"

using grablinkcore::CallbackDrain;
using grablinkcore::CallbackDrainSnapshot;

// The gate is meant to be usable from the acquisition callback: no heap, no
// mutex, no virtual dispatch, nothing to destroy. These hold at compile time.
static_assert(std::is_standard_layout<CallbackDrain>::value,
              "CallbackDrain must stay a plain aggregate of atomics");
static_assert(std::is_trivially_destructible<CallbackDrain>::value,
              "CallbackDrain must need no teardown work of its own");
static_assert(std::is_standard_layout<CallbackDrainSnapshot>::value,
              "the diagnostic snapshot must stay plain old data");
static_assert(std::is_trivially_copyable<CallbackDrainSnapshot>::value,
              "the diagnostic snapshot must stay plain old data");

namespace
{
// Waits, with a bound, for "flag" to be set by another thread, so a test can
// never hang on a scheduling accident.
bool WaitUntil(const std::atomic<bool>& flag, unsigned long timeoutMs)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!flag.load())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

// Milliseconds elapsed since "start".
long long MillisecondsSince(const std::chrono::steady_clock::time_point& start)
{
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
}

std::chrono::steady_clock::time_point Now()
{
    return std::chrono::steady_clock::now();
}

// Joins a thread on scope exit, so a failing REQUIRE (which unwinds the test)
// can never leave a joinable std::thread behind - that would call
// std::terminate and take the whole test executable with it. Worker threads in
// these tests also wait with a bound, so joining can never hang.
class Joiner
{
public:
    explicit Joiner(std::thread& thread) : m_thread(&thread)
    {
    }

    ~Joiner()
    {
        if (m_thread != 0 && m_thread->joinable())
        {
            m_thread->join();
        }
    }

    Joiner(Joiner&& other) : m_thread(other.m_thread)
    {
        other.m_thread = 0;
    }

private:
    Joiner(const Joiner&) = delete;
    Joiner& operator=(const Joiner&) = delete;
    Joiner& operator=(Joiner&&) = delete;

    std::thread* m_thread;
};

// The lock-free level the platform promises for the atomic types the gate uses
// (2 means "always lock-free, no lock ever taken"). Read through a function so
// the checks that use it are not compile-time constant conditions, which MSVC
// warns about at /W4.
int AtomicLockFreeLevels()
{
    int levels = 0;
    levels += (ATOMIC_BOOL_LOCK_FREE == 2) ? 1 : 0;
    levels += (ATOMIC_LONG_LOCK_FREE == 2) ? 1 : 0;
    levels += (ATOMIC_LLONG_LOCK_FREE == 2) ? 1 : 0;
    return levels;
}

std::size_t EntryGuardBytes()
{
    return sizeof(CallbackDrain::Entry);
}
}

// ---------------------------------------------------------------------------
// Test 1: a closed gate admits nothing, and says so.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainAdmitsNothingBeforeItIsEnabled)
{
    CallbackDrain drain;

    CHECK(!drain.IsEnabled());
    CHECK(!drain.TryEnter());

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK(!snapshot.enabled);
    CHECK_EQ(snapshot.admitted, 0ULL);
    CHECK_EQ(snapshot.left, 0ULL);
    CHECK_EQ(snapshot.rejected, 1ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.maxInFlight, 0LL);

    // Nothing was ever enabled, so this refusal is not an "after disable" one.
    CHECK_EQ(snapshot.rejectedAfterDisable, 0ULL);
}

// ---------------------------------------------------------------------------
// Test 2: an open gate admits callbacks and tracks them while they run.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainAdmitsCallbacksAndTracksInFlightWork)
{
    CallbackDrain drain;

    CHECK(drain.Enable());
    CHECK(drain.IsEnabled());

    CHECK(drain.TryEnter());
    CHECK(drain.TryEnter());

    CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK(snapshot.enabled);
    CHECK_EQ(snapshot.admitted, 2ULL);
    CHECK_EQ(snapshot.inFlight, 2LL);
    CHECK_EQ(snapshot.maxInFlight, 2LL);
    CHECK_EQ(snapshot.left, 0ULL);

    drain.Leave();
    drain.Leave();

    snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.left, 2ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.unbalancedLeaves, 0ULL);

    // The high-water mark survives the callbacks it measured.
    CHECK_EQ(snapshot.maxInFlight, 2LL);
}

// ---------------------------------------------------------------------------
// Test 3: Disable() stops new admissions without disturbing the running ones.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainDisablePreventsNewAdmissionsButKeepsRunningWork)
{
    CallbackDrain drain;
    drain.Enable();

    CHECK(drain.TryEnter());          // a callback admitted before the teardown

    CHECK(drain.Disable());
    CHECK(!drain.IsEnabled());

    // The callback that is already inside is still counted...
    CHECK_EQ(drain.Snapshot().inFlight, 1LL);

    // ...but the gate admits nothing new, and records the refusal.
    CHECK(!drain.TryEnter());
    CHECK(!drain.TryEnter());

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.admitted, 1ULL);
    CHECK_EQ(snapshot.rejected, 2ULL);
    CHECK_EQ(snapshot.rejectedAfterDisable, 2ULL);
    CHECK_EQ(snapshot.inFlight, 1LL);

    drain.Leave();
    CHECK_EQ(drain.Snapshot().inFlight, 0LL);
    CHECK_EQ(drain.Snapshot().left, 1ULL);
}

// ---------------------------------------------------------------------------
// Test 4: Enable() and Disable() report the transition exactly once.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainEnableAndDisableReportTheirTransitionOnce)
{
    CallbackDrain drain;

    CHECK(drain.Enable());
    CHECK(!drain.Enable());           // already open: no transition
    CHECK_EQ(drain.Snapshot().enables, 1ULL);

    CHECK(drain.Disable());
    CHECK(!drain.Disable());          // already closed: no transition
    CHECK_EQ(drain.Snapshot().disables, 1ULL);
    CHECK(!drain.IsEnabled());
}

// ---------------------------------------------------------------------------
// Test 5: an idle, disabled gate drains immediately.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainDrainsImmediatelyWhenNoCallbackIsInFlight)
{
    CallbackDrain drain;
    drain.Enable();
    drain.Disable();

    CHECK(drain.WaitForDrain(1000));

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.drainAttempts, 1ULL);
    CHECK_EQ(snapshot.drainCompleted, 1ULL);
    CHECK_EQ(snapshot.drainTimedOut, 0ULL);
    CHECK_EQ(snapshot.drainRefusedEnabled, 0ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
}

// ---------------------------------------------------------------------------
// Test 6: a bounded drain fails on time while a callback is still running, and
//         succeeds once that callback has left.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainTimesOutWhileACallbackIsInFlightThenSucceeds)
{
    CallbackDrain drain;
    drain.Enable();

    CHECK(drain.TryEnter());          // never left until the test says so

    CHECK(drain.Disable());

    const std::chrono::steady_clock::time_point start = Now();
    CHECK(!drain.WaitForDrain(80));
    const long long elapsed = MillisecondsSince(start);

    CHECK(elapsed >= 60);             // the budget was actually spent
    CHECK(elapsed < 5000);            // ...and bounded

    CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.drainAttempts, 1ULL);
    CHECK_EQ(snapshot.drainTimedOut, 1ULL);
    CHECK_EQ(snapshot.drainCompleted, 0ULL);
    CHECK_EQ(snapshot.inFlight, 1LL);

    drain.Leave();

    CHECK(drain.WaitForDrain(5000));

    snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.drainAttempts, 2ULL);
    CHECK_EQ(snapshot.drainTimedOut, 1ULL);
    CHECK_EQ(snapshot.drainCompleted, 1ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
}

// ---------------------------------------------------------------------------
// Test 7: an open gate refuses to report a completed drain, and does not burn
//         the caller's timeout doing it.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainRefusesToDrainWhileTheGateIsOpen)
{
    CallbackDrain drain;
    drain.Enable();

    const std::chrono::steady_clock::time_point start = Now();
    CHECK(!drain.WaitForDrain(1000));
    const long long elapsed = MillisecondsSince(start);

    CHECK(elapsed < 500);             // refused at once, not timed out

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.drainRefusedEnabled, 1ULL);
    CHECK_EQ(snapshot.drainCompleted, 0ULL);
    CHECK_EQ(snapshot.drainTimedOut, 0ULL);

    drain.Disable();
    CHECK(drain.WaitForDrain(1000));
    CHECK_EQ(drain.Snapshot().drainCompleted, 1ULL);
}

// ---------------------------------------------------------------------------
// Test 8: reopening the gate during a drain makes the drain give up instead of
//         reporting success.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainGivesUpWhenTheGateIsReopenedDuringADrain)
{
    CallbackDrain drain;
    drain.Enable();

    std::atomic<bool> callbackEntered(false);
    std::atomic<bool> releaseCallback(false);
    std::thread callback([&]()
    {
        if (drain.TryEnter())
        {
            callbackEntered.store(true);
            WaitUntil(releaseCallback, 10000);
            drain.Leave();
        }
    });
    Joiner callbackJoiner(callback);

    REQUIRE(WaitUntil(callbackEntered, 5000));
    CHECK(drain.Disable());

    // A second thread re-arms the gate while the drain below is waiting.
    std::atomic<bool> reopened(false);
    std::thread opener([&]()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        reopened.store(drain.Enable());
    });
    Joiner openerJoiner(opener);

    const std::chrono::steady_clock::time_point start = Now();
    const bool drained = drain.WaitForDrain(5000);
    const long long elapsed = MillisecondsSince(start);

    CHECK(reopened.load());
    CHECK(!drained);
    CHECK(elapsed >= 50);             // it did wait for the reopen
    CHECK(elapsed < 4000);            // and then gave up, well inside the budget
    CHECK_EQ(drain.Snapshot().drainRefusedEnabled, 1ULL);
    CHECK_EQ(drain.Snapshot().drainCompleted, 0ULL);

    releaseCallback.store(true);
    callback.join();
    opener.join();

    // The callback that was admitted before the teardown still left cleanly.
    CHECK_EQ(drain.Snapshot().left, 1ULL);
    CHECK_EQ(drain.Snapshot().inFlight, 0LL);
}

// ---------------------------------------------------------------------------
// Test 9: the guard leaves exactly once, on scope exit.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainGuardLeavesExactlyOnceOnScopeExit)
{
    CallbackDrain drain;
    drain.Enable();

    {
        CallbackDrain::Entry entry(drain);
        CHECK(entry.Entered());
        CHECK_EQ(drain.Snapshot().inFlight, 1LL);
    }

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.admitted, 1ULL);
    CHECK_EQ(snapshot.left, 1ULL);
    CHECK_EQ(snapshot.unbalancedLeaves, 0ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
}

// ---------------------------------------------------------------------------
// Test 10: an early release leaves once and disarms the destructor.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainGuardReleaseLeavesOnceAndDisarmsTheDestructor)
{
    CallbackDrain drain;
    drain.Enable();

    {
        CallbackDrain::Entry entry(drain);
        REQUIRE(entry.Entered());

        CHECK(entry.Release());
        CHECK_EQ(drain.Snapshot().left, 1ULL);
        CHECK_EQ(drain.Snapshot().inFlight, 0LL);

        CHECK(!entry.Release());      // the second release is a no-op
        CHECK(!entry.Entered());
        CHECK_EQ(drain.Snapshot().left, 1ULL);
    }

    // Scope exit must not leave a second time.
    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.left, 1ULL);
    CHECK_EQ(snapshot.admitted, 1ULL);
    CHECK_EQ(snapshot.unbalancedLeaves, 0ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
}

// ---------------------------------------------------------------------------
// Test 11: moving the guard transfers the obligation to leave, exactly once.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainGuardMoveTransfersTheObligationToLeave)
{
    CallbackDrain drain;
    drain.Enable();

    {
        CallbackDrain::Entry outer(drain);
        REQUIRE(outer.Entered());

        CallbackDrain::Entry inner(std::move(outer));
        CHECK(!outer.Entered());
        CHECK(inner.Entered());
        CHECK_EQ(drain.Snapshot().inFlight, 1LL);
        CHECK_EQ(drain.Snapshot().left, 0ULL);
    }

    CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.admitted, 1ULL);
    CHECK_EQ(snapshot.left, 1ULL);
    CHECK_EQ(snapshot.unbalancedLeaves, 0ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);

    // Move assignment over an entered guard leaves the guard it displaces.
    {
        CallbackDrain::Entry first(drain);
        CallbackDrain::Entry second(drain);
        REQUIRE(first.Entered());
        REQUIRE(second.Entered());
        CHECK_EQ(drain.Snapshot().inFlight, 2LL);

        second = std::move(first);
        CHECK(!first.Entered());
        CHECK(second.Entered());
        CHECK_EQ(drain.Snapshot().left, 2ULL);   // 1 from before + second's own
        CHECK_EQ(drain.Snapshot().inFlight, 1LL);
    }

    snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.admitted, 3ULL);
    CHECK_EQ(snapshot.left, 3ULL);
    CHECK_EQ(snapshot.unbalancedLeaves, 0ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
}

// ---------------------------------------------------------------------------
// Test 12: a guard on a closed gate enters nothing and leaves nothing.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainGuardEntersNothingOnAClosedGate)
{
    CallbackDrain drain;

    {
        CallbackDrain::Entry entry(drain);      // never enabled
        CHECK(!entry.Entered());
        CHECK(!entry.Release());
    }

    drain.Enable();
    drain.Disable();

    {
        CallbackDrain::Entry entry(drain);      // closed by Disable()
        CHECK(!entry.Entered());
        CHECK(!entry.Release());
    }

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.admitted, 0ULL);
    CHECK_EQ(snapshot.left, 0ULL);
    CHECK_EQ(snapshot.unbalancedLeaves, 0ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.rejected, 2ULL);
    CHECK_EQ(snapshot.rejectedAfterDisable, 1ULL);
}

// ---------------------------------------------------------------------------
// Test 13: an unbalanced Leave() is reported instead of underflowing.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainUnbalancedLeaveIsReportedWithoutUnderflow)
{
    CallbackDrain drain;
    drain.Enable();

    drain.Leave();                    // never entered: a defect, not a crash

    CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.left, 0ULL);
    CHECK_EQ(snapshot.unbalancedLeaves, 1ULL);

    // The gate is still usable and still counts honestly.
    CHECK(drain.TryEnter());
    CHECK_EQ(drain.Snapshot().inFlight, 1LL);
    drain.Leave();

    snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.admitted, 1ULL);
    CHECK_EQ(snapshot.left, 1ULL);
    CHECK_EQ(snapshot.unbalancedLeaves, 1ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
}

// ---------------------------------------------------------------------------
// Test 14: the gate serves the next capture generation after a completed drain.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainIsReusableAfterACompletedDrain)
{
    CallbackDrain drain;

    // First capture generation: enable, run a callback, tear down, drain.
    CHECK(drain.Enable());
    CHECK(drain.TryEnter());
    drain.Leave();
    CHECK(drain.Disable());
    CHECK(drain.WaitForDrain(5000));

    // Second generation: the same gate admits again.
    CHECK(drain.Enable());
    CHECK(drain.IsEnabled());
    CHECK(drain.TryEnter());
    CHECK_EQ(drain.Snapshot().inFlight, 1LL);
    CHECK(drain.Disable());
    CHECK(!drain.WaitForDrain(80));   // this generation still has one running
    drain.Leave();
    CHECK(drain.WaitForDrain(5000));

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.enables, 2ULL);
    CHECK_EQ(snapshot.disables, 2ULL);
    CHECK_EQ(snapshot.admitted, 2ULL);
    CHECK_EQ(snapshot.left, 2ULL);
    CHECK_EQ(snapshot.drainCompleted, 2ULL);
    CHECK_EQ(snapshot.drainTimedOut, 1ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.maxInFlight, 1LL);
}

// ---------------------------------------------------------------------------
// Test 15: Disable() racing a callback that is already running can never be
//          reported as a completed drain.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainDisableRacingARunningCallbackNeverReportsSuccess)
{
    CallbackDrain drain;
    drain.Enable();

    std::atomic<bool> callbackEntered(false);
    std::atomic<bool> releaseCallback(false);
    std::atomic<bool> callbackFinished(false);

    std::thread callback([&]()
    {
        if (drain.TryEnter())
        {
            callbackEntered.store(true);
            WaitUntil(releaseCallback, 10000);
            callbackFinished.store(true);
            drain.Leave();
        }
    });
    Joiner callbackJoiner(callback);

    REQUIRE(WaitUntil(callbackEntered, 5000));
    REQUIRE(drain.Disable());
    CHECK_EQ(drain.Snapshot().inFlight, 1LL);

    // The callback is inside its body while the drain is asked to complete.
    const std::chrono::steady_clock::time_point start = Now();
    CHECK(!drain.WaitForDrain(80));
    const long long elapsed = MillisecondsSince(start);

    CHECK(elapsed >= 60);
    CHECK(!callbackFinished.load());            // it really was still running
    CHECK_EQ(drain.Snapshot().inFlight, 1LL);
    CHECK_EQ(drain.Snapshot().drainTimedOut, 1ULL);

    releaseCallback.store(true);
    callback.join();

    CHECK(callbackFinished.load());
    CHECK(drain.WaitForDrain(5000));

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.admitted, 1ULL);
    CHECK_EQ(snapshot.left, 1ULL);
    CHECK_EQ(snapshot.unbalancedLeaves, 0ULL);
    CHECK_EQ(snapshot.drainCompleted, 1ULL);
}

// ---------------------------------------------------------------------------
// Test 16: many threads hammering admission across a teardown - after the drain
//          reports success no callback body can be running or ever start again.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainConcurrentTeardownNeverAdmitsWorkAfterASuccessfulDrain)
{
    CallbackDrain drain;
    drain.Enable();

    const int kWorkerCount = 4;
    std::atomic<bool> stop(false);
    std::atomic<long> inside(0);
    std::atomic<long> completed(0);

    std::vector<std::thread> workers;
    for (int index = 0; index < kWorkerCount; ++index)
    {
        workers.push_back(std::thread([&]()
        {
            // Bounded in time as well as by "stop", so that even a failing run
            // that unwinds cannot leave a spinning thread behind.
            const std::chrono::steady_clock::time_point deadline =
                Now() + std::chrono::milliseconds(20000);
            while (!stop.load() && Now() < deadline)
            {
                if (drain.TryEnter())
                {
                    inside.fetch_add(1);
                    for (int spin = 0; spin < 8; ++spin)
                    {
                        std::this_thread::yield();
                    }
                    inside.fetch_sub(1);
                    drain.Leave();
                    completed.fetch_add(1);
                }
            }
        }));
    }

    // Join the workers even if an assertion below unwinds the test.
    std::vector<Joiner> joiners;
    for (std::size_t index = 0; index < workers.size(); ++index)
    {
        joiners.push_back(Joiner(workers[index]));
    }

    // Let the workers build up a real stream of admitted callbacks first. The
    // wait is bounded so that a failing (RED) run still terminates.
    bool warmedUp = false;
    const std::chrono::steady_clock::time_point warmupDeadline =
        Now() + std::chrono::milliseconds(5000);
    while (Now() < warmupDeadline)
    {
        if (drain.Snapshot().admitted >= 50)
        {
            warmedUp = true;
            break;
        }
        std::this_thread::yield();
    }

    // Teardown while the callbacks are arriving. Nothing here aborts the test
    // while the workers are still running; the observations are asserted after
    // they have been joined.
    const bool disabled = drain.Disable();
    const bool drained = drain.WaitForDrain(10000);
    const long insideAfterDrain = inside.load();
    const long long inFlightAfterDrain = drain.Snapshot().inFlight;

    stop.store(true);
    for (std::size_t index = 0; index < workers.size(); ++index)
    {
        workers[index].join();
    }

    // This is the whole point: a completed drain means no callback body is
    // running, and the closed gate means none can start.
    CHECK(warmedUp);
    CHECK(disabled);
    CHECK(drained);
    CHECK_EQ(insideAfterDrain, 0L);
    CHECK_EQ(inFlightAfterDrain, 0LL);

    CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.admitted, snapshot.left);
    CHECK_EQ(snapshot.unbalancedLeaves, 0ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.drainCompleted, 1ULL);
    CHECK(snapshot.maxInFlight >= 1);
    CHECK(snapshot.maxInFlight <= static_cast<long long>(snapshot.admitted));
    CHECK(completed.load() >= 50);

    // A callback arriving after the drain is refused, never run.
    const unsigned long long rejectedBefore = snapshot.rejectedAfterDisable;
    CHECK(!drain.TryEnter());
    snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.rejectedAfterDisable, rejectedBefore + 1ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.admitted, snapshot.left);
}

// ---------------------------------------------------------------------------
// Test 17: admission stays lock-free, allocation-free and fast enough to sit at
//          the top of a per-frame callback.
// ---------------------------------------------------------------------------
TEST_CASE(CallbackDrainAdmissionPathIsLockFreeAndAllocationFree)
{
    // A bounded lock would have to be a spin on a lock word; the gate uses
    // nothing but lock-free atomics, so it can never block on another thread
    // and never asks the runtime for a lock. (The C++11 macros are used instead
    // of the C++17 is_always_lock_free member, because the project compiles at
    // the compiler's default language standard.)
    CHECK_EQ(AtomicLockFreeLevels(), 3);

    std::atomic<bool> flag;
    std::atomic<long> counter;
    std::atomic<unsigned long long> total;
    CHECK(flag.is_lock_free());
    CHECK(counter.is_lock_free());
    CHECK(total.is_lock_free());

    // The guard is a pointer and a flag: it cannot be allocating either.
    CHECK(EntryGuardBytes() <= 2 * sizeof(void*));

    CallbackDrain drain;
    drain.Enable();

    const int kIterations = 200000;
    bool allAdmitted = true;
    const std::chrono::steady_clock::time_point start = Now();
    for (int index = 0; index < kIterations; ++index)
    {
        if (!drain.TryEnter())
        {
            allAdmitted = false;
        }
        drain.Leave();
    }
    const long long elapsed = MillisecondsSince(start);

    CHECK(allAdmitted);
    CHECK(elapsed < 5000);

    const CallbackDrainSnapshot snapshot = drain.Snapshot();
    CHECK_EQ(snapshot.admitted, static_cast<unsigned long long>(kIterations));
    CHECK_EQ(snapshot.left, static_cast<unsigned long long>(kIterations));
    CHECK_EQ(snapshot.rejected, 0ULL);
    CHECK_EQ(snapshot.inFlight, 0LL);
    CHECK_EQ(snapshot.maxInFlight, 1LL);
}
