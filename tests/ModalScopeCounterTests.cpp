// ModalScopeCounterTests.cpp - behavioural tests for the application-modal
// suppression counter.
//
// The live preview integration posts WM_APP_PREVIEW_REFRESH from the driver's
// signal thread while a capture is running. Every modal call the application
// makes on the UI thread - the shell's folder chooser, an MFC About box, any
// dialog that pumps a nested message loop - dispatches that message inside its
// own loop, and the continuous post/Invalidate stream entering that loop is
// what leaves a modal window hidden behind a disabled owner. The suppression
// has to cover the whole span of such a call, and it has to survive modal calls
// that nest (an About box opened while the folder chooser is up, a report shown
// from a dialog).
//
// These tests pin the properties that make the suppression usable for that:
//
//   * the suppression is on exactly while at least one modal scope is held;
//   * nested scopes keep it on until the outermost one ends;
//   * giving an inner scope back early does not lift it while an outer scope is
//     still held;
//   * moving a guard moves the obligation, and the moved-from guard is inert;
//   * a refused release (empty guard, double release, release after Reset) is
//     safe: it never underflows the depth into a permanent suppression;
//   * the counter returns to unsuppressed once every scope has ended;
//   * the path the acquisition callback and the UI thread run is allocation
//     free and lock free - observed through the interposed global allocator and
//     the atomics' own lock-free report.
//
// Each test was written before the implementation existed, compiled and run
// against the missing helper first (RED), then made to pass (GREEN).
//
// The standard headers are taken in before the harness pulls in <windows.h>, so
// that no Win32 macro can disturb the library.

#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "TestHarness.h"

#include "ModalScopeCounter.h"

using grablinkcore::ModalScope;
using grablinkcore::ModalScopeCounter;
using grablinkcore::ModalScopeCounterSnapshot;

// ---------------------------------------------------------------------------
// Test 1: the suppression is on only while a scope is held - the acquisition
// callback's check is exactly this flag.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterSuppressesOnlyWhileAScopeIsHeld)
{
    ModalScopeCounter counter;

    const ModalScopeCounterSnapshot initial = counter.Snapshot();
    CHECK_EQ(initial.suppressed, false);
    CHECK_EQ(initial.depth, 0ULL);
    CHECK_EQ(initial.maximumDepth, 0ULL);
    CHECK_EQ(initial.begins, 0ULL);
    CHECK_EQ(initial.ends, 0ULL);
    CHECK_EQ(initial.unbalancedEnds, 0ULL);
    CHECK_EQ(initial.resets, 0ULL);
    CHECK_EQ(counter.IsSuppressed(), false);
    CHECK_EQ(counter.Depth(), 0ULL);

    // A modal call begins: from here on the callback must not post a refresh.
    REQUIRE_EQ(counter.Begin(), 1ULL);
    CHECK_EQ(counter.IsSuppressed(), true);
    CHECK_EQ(counter.Depth(), 1ULL);
    CHECK_EQ(counter.Snapshot().maximumDepth, 1ULL);

    // The modal call returns: preview refreshes flow again.
    CHECK_EQ(counter.End(), true);
    CHECK_EQ(counter.IsSuppressed(), false);
    CHECK_EQ(counter.Depth(), 0ULL);

    const ModalScopeCounterSnapshot finalSnapshot = counter.Snapshot();
    CHECK_EQ(finalSnapshot.suppressed, false);
    CHECK_EQ(finalSnapshot.begins, 1ULL);
    CHECK_EQ(finalSnapshot.ends, 1ULL);
    CHECK_EQ(finalSnapshot.unbalancedEnds, 0ULL);
}

// ---------------------------------------------------------------------------
// Test 2: nested modal scopes keep the suppression on until the outermost one
// is released, in whatever order the inner ones end.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterNestedScopesStaySuppressedUntilTheOutermostEnds)
{
    ModalScopeCounter counter;

    {
        // A modal call begins. Its guard is the obligation: the scope ends when
        // this guard is destroyed, on every path out of the call.
        ModalScope outer(counter);
        REQUIRE_EQ(counter.Depth(), 1ULL);
        CHECK_EQ(counter.IsSuppressed(), true);

        {
            // A second modal call starts from inside the first one.
            ModalScope inner(counter);
            CHECK_EQ(counter.Depth(), 2ULL);
            CHECK_EQ(counter.IsSuppressed(), true);
        }

        // The inner modal call has returned; the outer one has not. The
        // suppression must outlive the inner scope.
        CHECK_EQ(counter.IsSuppressed(), true);
        CHECK_EQ(counter.Depth(), 1ULL);

        // A sibling inner scope, ending in the same way.
        {
            ModalScope sibling(counter);
            CHECK_EQ(counter.Depth(), 2ULL);
        }
        CHECK_EQ(counter.IsSuppressed(), true);
        CHECK_EQ(counter.Depth(), 1ULL);
    }

    // Only the outermost scope ending lifts the suppression.
    CHECK_EQ(counter.IsSuppressed(), false);
    CHECK_EQ(counter.Depth(), 0ULL);

    const ModalScopeCounterSnapshot snapshot = counter.Snapshot();
    CHECK_EQ(snapshot.begins, 3ULL);
    CHECK_EQ(snapshot.ends, 3ULL);
    CHECK_EQ(snapshot.unbalancedEnds, 0ULL);
    CHECK_EQ(snapshot.maximumDepth, 2ULL);
    CHECK_EQ(snapshot.suppressed, false);
}

// ---------------------------------------------------------------------------
// Test 3: giving an inner scope back early through the explicit release call
// does not lift the suppression while an outer scope is still held.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterExplicitReleaseOfAnInnerScopeKeepsTheOuterSuppression)
{
    ModalScopeCounter counter;

    {
        ModalScope outer(counter);
        ModalScope inner(counter);
        REQUIRE_EQ(counter.Depth(), 2ULL);

        // The inner modal call gives its scope back explicitly instead of
        // waiting for its destructor (a call that knows its own end before it
        // returns).
        CHECK_EQ(inner.IsHeld(), true);
        CHECK_EQ(inner.Release(), true);
        CHECK_EQ(inner.IsHeld(), false);

        // The outer modal call is still running, so refreshes must stay off.
        CHECK_EQ(counter.IsSuppressed(), true);
        CHECK_EQ(counter.Depth(), 1ULL);

        // Releasing an already released guard is refused, not applied twice.
        CHECK_EQ(inner.Release(), false);
        CHECK_EQ(counter.Depth(), 1ULL);

        // The outer scope ends explicitly here; its destructor must be inert
        // afterwards.
        CHECK_EQ(outer.Release(), true);
        CHECK_EQ(counter.IsSuppressed(), false);
        CHECK_EQ(counter.Depth(), 0ULL);
    }

    // Both guards have now been destroyed. An inert destructor is what keeps the
    // counts exact: a released guard that released again would show a third end
    // here. The refused second release of the inner guard is reported locally
    // and never reaches the counter - a guard that holds no scope has no counter
    // to report a defect to, and asking the counter to end a scope somebody else
    // holds would be the dangerous behaviour.
    const ModalScopeCounterSnapshot snapshot = counter.Snapshot();
    CHECK_EQ(snapshot.begins, 2ULL);
    CHECK_EQ(snapshot.ends, 2ULL);
    CHECK_EQ(snapshot.unbalancedEnds, 0ULL);
    CHECK_EQ(snapshot.depth, 0ULL);
    CHECK_EQ(snapshot.suppressed, false);
}

// ---------------------------------------------------------------------------
// Test 4: moving a guard transfers the obligation. The moved-from guard must
// not release anything, and a guard that is moved onto must end the scope it
// already held rather than dropping it.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterGuardMoveTransfersTheObligation)
{
    ModalScopeCounter counter;

    // Move construction.
    {
        ModalScope source(counter);
        ModalScope destination(std::move(source));

        CHECK_EQ(source.IsHeld(), false);
        CHECK_EQ(destination.IsHeld(), true);
        CHECK_EQ(counter.IsSuppressed(), true);
        CHECK_EQ(counter.Depth(), 1ULL);
    }
    CHECK_EQ(counter.Depth(), 0ULL);
    CHECK_EQ(counter.IsSuppressed(), false);

    // Move assignment onto a guard that holds nothing: the obligation moves and
    // the moved-from guard is destroyed without releasing.
    {
        ModalScope destination;
        {
            ModalScope source(counter);
            destination = std::move(source);
            CHECK_EQ(source.IsHeld(), false);
        }
        CHECK_EQ(destination.IsHeld(), true);
        CHECK_EQ(counter.IsSuppressed(), true);
        CHECK_EQ(counter.Depth(), 1ULL);
    }
    CHECK_EQ(counter.Depth(), 0ULL);

    // Move assignment onto a guard that already holds a scope: the target's own
    // obligation is ended by the assignment, so exactly one scope remains held -
    // neither guard may leak one.
    {
        ModalScope first(counter);
        ModalScope second(counter);
        REQUIRE_EQ(counter.Depth(), 2ULL);

        first = std::move(second);
        CHECK_EQ(second.IsHeld(), false);
        CHECK_EQ(first.IsHeld(), true);
        CHECK_EQ(counter.Depth(), 1ULL);
        CHECK_EQ(counter.IsSuppressed(), true);
    }
    CHECK_EQ(counter.Depth(), 0ULL);
    CHECK_EQ(counter.IsSuppressed(), false);

    // Every scope that was begun was ended exactly once, by the guard that ended
    // up holding it.
    const ModalScopeCounterSnapshot snapshot = counter.Snapshot();
    CHECK_EQ(snapshot.begins, 4ULL);
    CHECK_EQ(snapshot.ends, 4ULL);
    CHECK_EQ(snapshot.unbalancedEnds, 0ULL);
    CHECK_EQ(snapshot.maximumDepth, 2ULL);
}

// ---------------------------------------------------------------------------
// Test 5: a refused or no-op release is safe. Nothing may underflow the depth:
// a depth that wrapped to its maximum would read as permanently suppressed and
// would silently disable the live preview for the rest of the session.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterRefusedOrNoOpReleaseIsSafe)
{
    ModalScopeCounter counter;

    // An empty guard holds nothing, so releasing it cannot end somebody else's
    // scope.
    ModalScope empty;
    CHECK_EQ(empty.IsHeld(), false);
    CHECK_EQ(empty.Release(), false);
    CHECK_EQ(empty.Release(), false);
    CHECK_EQ(counter.Depth(), 0ULL);
    CHECK_EQ(counter.IsSuppressed(), false);

    // An explicit end with nothing held is refused rather than wrapping.
    CHECK_EQ(counter.End(), false);
    CHECK_EQ(counter.Depth(), 0ULL);
    CHECK_EQ(counter.IsSuppressed(), false);

    ModalScope held(counter);
    REQUIRE_EQ(counter.Depth(), 1ULL);

    // The empty guard's refusal again cannot lift the held scope.
    CHECK_EQ(empty.Release(), false);
    CHECK_EQ(counter.IsSuppressed(), true);
    CHECK_EQ(counter.Depth(), 1ULL);

    // A double release is applied once and then refused.
    CHECK_EQ(held.Release(), true);
    CHECK_EQ(held.Release(), false);
    CHECK_EQ(counter.IsSuppressed(), false);
    CHECK_EQ(counter.Depth(), 0ULL);

    // A moved-from guard is empty: releasing it is refused without touching the
    // counter, so it reports no defect it cannot know about.
    ModalScope source(counter);
    ModalScope destination(std::move(source));
    CHECK_EQ(source.Release(), false);
    CHECK_EQ(counter.Depth(), 1ULL);
    CHECK_EQ(destination.Release(), true);
    CHECK_EQ(counter.Depth(), 0ULL);

    // Everything up to here behaved, and the counts say so: two scopes begun,
    // two ended, and one refusal - the explicit end with nothing held. The
    // empty guard's local refusals are not in here; they never left the guard.
    const ModalScopeCounterSnapshot beforeReset = counter.Snapshot();
    CHECK_EQ(beforeReset.begins, 2ULL);
    CHECK_EQ(beforeReset.ends, 2ULL);
    CHECK_EQ(beforeReset.unbalancedEnds, 1ULL);
    CHECK_EQ(beforeReset.depth, 0ULL);

    // A guard that outlives a Reset() finds no scope of its own any more. Its
    // release is refused by the counter - and counted there, because this is the
    // case in which a caller really did hand back a scope the counter had
    // already dropped. The counter stays unsuppressed instead of wrapping.
    ModalScope stale(counter);
    REQUIRE_EQ(counter.Depth(), 1ULL);
    counter.Reset();
    CHECK_EQ(counter.Depth(), 0ULL);
    CHECK_EQ(counter.IsSuppressed(), false);
    CHECK_EQ(stale.IsHeld(), true);
    CHECK_EQ(stale.Release(), false);
    CHECK_EQ(stale.IsHeld(), false);
    CHECK_EQ(counter.Depth(), 0ULL);
    CHECK_EQ(counter.IsSuppressed(), false);

    // Reset() clears the counts along with the depth, so the refusal above is
    // the first thing the new generation counted.
    const ModalScopeCounterSnapshot snapshot = counter.Snapshot();
    CHECK_EQ(snapshot.begins, 0ULL);
    CHECK_EQ(snapshot.ends, 0ULL);
    CHECK_EQ(snapshot.unbalancedEnds, 1ULL);
    CHECK_EQ(snapshot.resets, 1ULL);
    CHECK_EQ(snapshot.suppressed, false);
}

// ---------------------------------------------------------------------------
// Test 6: the state returns to unsuppressed once every scope has ended, however
// the scopes were nested and whatever order they were released in - and the
// counter is immediately usable for the next modal call.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterReturnsToUnsuppressedAfterEveryScopeEnds)
{
    ModalScopeCounter counter;

    // Three scopes entered in one order and released in another, mixing an
    // explicit begin with guards, which is what the document does: the folder
    // chooser can be open while an About box is up, and either can return first.
    ModalScope first = counter.BeginScope();
    const unsigned long long secondDepth = counter.Begin();
    ModalScope third(counter);
    CHECK_EQ(secondDepth, 2ULL);
    CHECK_EQ(counter.IsSuppressed(), true);
    CHECK_EQ(counter.Depth(), 3ULL);

    // Innermost first.
    CHECK_EQ(third.Release(), true);
    CHECK_EQ(counter.IsSuppressed(), true);
    CHECK_EQ(counter.Depth(), 2ULL);

    // The explicit scope second.
    CHECK_EQ(counter.End(), true);
    CHECK_EQ(counter.IsSuppressed(), true);
    CHECK_EQ(counter.Depth(), 1ULL);

    // The outermost last: this is the moment refreshes flow again.
    CHECK_EQ(first.Release(), true);
    CHECK_EQ(counter.IsSuppressed(), false);
    CHECK_EQ(counter.Depth(), 0ULL);

    const ModalScopeCounterSnapshot drained = counter.Snapshot();
    CHECK_EQ(drained.begins, 3ULL);
    CHECK_EQ(drained.ends, 3ULL);
    CHECK_EQ(drained.unbalancedEnds, 0ULL);
    CHECK_EQ(drained.maximumDepth, 3ULL);
    CHECK_EQ(drained.suppressed, false);

    // The same counter serves the next modal call, and Reset() returns it to a
    // freshly constructed state.
    {
        ModalScope next = counter.BeginScope();
        CHECK_EQ(counter.IsSuppressed(), true);
        CHECK_EQ(counter.Depth(), 1ULL);
    }
    CHECK_EQ(counter.IsSuppressed(), false);
    CHECK_EQ(counter.Depth(), 0ULL);

    counter.Reset();
    const ModalScopeCounterSnapshot afterReset = counter.Snapshot();
    CHECK_EQ(afterReset.depth, 0ULL);
    CHECK_EQ(afterReset.maximumDepth, 0ULL);
    CHECK_EQ(afterReset.begins, 0ULL);
    CHECK_EQ(afterReset.ends, 0ULL);
    CHECK_EQ(afterReset.unbalancedEnds, 0ULL);
    CHECK_EQ(afterReset.resets, 1ULL);
    CHECK_EQ(afterReset.suppressed, false);
}

// ---------------------------------------------------------------------------
// Test 7: the checked path is allocation free and lock free.
//
// IsSuppressed() runs on the driver's signal thread for every acquired frame,
// and begin/release run on the UI thread inside modal calls. Allocating there
// would put a heap call on the capture path; taking a lock there could block the
// signal thread behind a UI thread that is sitting in a modal message loop,
// which is precisely the stall the suppression exists to avoid.
//
// Allocation is observed through the interposed global allocator this
// executable installs (see CaptureStatsTests.cpp and test::AllocationCount()),
// so the window below is a measurement and not a reading of the source.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterCheckedPathIsAllocationFreeAndLockFree)
{
    // The one primitive the whole class is built on, reporting for itself.
    CHECK_EQ(std::atomic<unsigned long long>().is_lock_free(), true);
    CHECK_EQ(std::atomic<bool>().is_lock_free(), true);

    ModalScopeCounterSnapshot observed = ModalScopeCounterSnapshot();

    const long before = test::AllocationCount().load(std::memory_order_relaxed);
    {
        ModalScopeCounter counter;      // construction
        ModalScope held(counter);       // a modal call begins
        observed = counter.Snapshot();  // the diagnostic copy, a POD assignment
        (void)counter.IsSuppressed();   // the callback's check
        (void)counter.Depth();
        {
            ModalScope nested(counter); // a nested modal call
            (void)counter.IsSuppressed();
        }
        (void)counter.End();            // refused: the guard holds the scope
        (void)held.Release();           // the outer modal call returns
        counter.Reset();
    }
    const long after = test::AllocationCount().load(std::memory_order_relaxed);

    CHECK_EQ(after - before, 0L);

    // The instrument is real. One deliberate allocation has to move the count,
    // so the zero above cannot come from a counter that nothing feeds: the
    // interposition lives in another translation unit, and if its increments
    // reached a different object than the reads here, the window would be
    // meaningless. This probe is what rules that out.
    const long probeBefore = test::AllocationCount().load(std::memory_order_relaxed);
    std::vector<unsigned char> probe(64, 7);
    const long probeAfter = test::AllocationCount().load(std::memory_order_relaxed);
    CHECK(probeAfter > probeBefore);
    CHECK_EQ(probe[0], static_cast<unsigned char>(7));

    // The window really did what it says it did: the snapshot was taken with
    // exactly one scope held, and the counter ended unsuppressed.
    CHECK_EQ(observed.suppressed, true);
    CHECK_EQ(observed.depth, 1ULL);
}

// ---------------------------------------------------------------------------
// Test 8: concurrent scopes keep the depth exact, so a busy UI thread cannot
// lose a scope and leave the preview suppressed for the rest of the session.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterConcurrentScopesKeepTheDepthExact)
{
    const int workers = 4;
    const int iterations = 20000;

    ModalScopeCounter counter;
    std::atomic<bool> start(false);
    std::atomic<long long> observations(0);

    std::vector<std::thread> threads;
    for (int worker = 0; worker < workers; ++worker)
    {
        threads.push_back(std::thread([&counter, &start, &observations, iterations]()
        {
            while (!start.load())
            {
                std::this_thread::yield();
            }

            for (int index = 0; index < iterations; ++index)
            {
                // A modal call begins and ends, and the callback's check runs
                // inside it, on the other thread.
                ModalScope scope(counter);
                if (counter.IsSuppressed())
                {
                    observations.fetch_add(1);
                }
            }
        }));
    }

    start.store(true);
    for (std::size_t index = 0; index < threads.size(); ++index)
    {
        threads[index].join();
    }

    CHECK_EQ(counter.Depth(), 0ULL);
    CHECK_EQ(counter.IsSuppressed(), false);

    const unsigned long long expected =
        static_cast<unsigned long long>(workers) * static_cast<unsigned long long>(iterations);
    const ModalScopeCounterSnapshot snapshot = counter.Snapshot();
    CHECK_EQ(snapshot.begins, expected);
    CHECK_EQ(snapshot.ends, expected);
    CHECK_EQ(snapshot.unbalancedEnds, 0ULL);
    CHECK_EQ(snapshot.suppressed, false);

    // Every worker saw its own scope held.
    CHECK_EQ(observations.load(), static_cast<long long>(expected));
}

// ---------------------------------------------------------------------------
// Test 9: the snapshot is plain-old-data, and neither the counter nor a guard
// can be copied - a copy of a guard would release a scope a second time.
// ---------------------------------------------------------------------------
TEST_CASE(ModalScopeCounterSnapshotIsPlainOldData)
{
    CHECK_EQ(std::is_trivially_copyable<ModalScopeCounterSnapshot>::value, true);
    CHECK_EQ(std::is_copy_constructible<ModalScopeCounter>::value, false);
    CHECK_EQ(std::is_copy_assignable<ModalScopeCounter>::value, false);
    CHECK_EQ(std::is_copy_constructible<ModalScope>::value, false);
    CHECK_EQ(std::is_copy_assignable<ModalScope>::value, false);
    CHECK_EQ(std::is_move_constructible<ModalScope>::value, true);
    CHECK_EQ(std::is_move_assignable<ModalScope>::value, true);

    // A snapshot is an independent reading: it does not follow the counter.
    ModalScopeCounter counter;
    ModalScope held(counter);
    const ModalScopeCounterSnapshot copy = counter.Snapshot();
    CHECK_EQ(copy.suppressed, true);
    CHECK_EQ(held.Release(), true);
    CHECK_EQ(copy.suppressed, true);
    CHECK_EQ(counter.Snapshot().suppressed, false);
}
