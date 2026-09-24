// RefreshCoalescerTests.cpp - behavioural tests for the preview refresh gate.
//
// The gate sits between the acquisition callback and the UI queue, so the
// tests concentrate on the properties that make it useful there: the first
// publish of a generation claims exactly one post and every later publish is
// coalesced away, the UI acknowledgement re-arms the gate, Reset() starts a
// fresh generation, and concurrent producers of one generation produce exactly
// one winner.
//
// The tests are hardware free: the C++ standard library is all they need.

#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <vector>

#include "TestHarness.h"

#include "RefreshCoalescer.h"

using grablinkcore::RefreshCoalescer;
using grablinkcore::RefreshCoalescerSnapshot;
using grablinkcore::SaturatingAddCount;

// ---------------------------------------------------------------------------
// Test 1: the first publish of a generation claims the post; every later
// publish is coalesced away until the UI acknowledges.
// ---------------------------------------------------------------------------
TEST_CASE(RefreshCoalescerFirstClaimWinsAndLaterClaimsCoalesce)
{
    RefreshCoalescer coalescer;

    const RefreshCoalescerSnapshot initial = coalescer.Snapshot();
    CHECK_EQ(initial.claims, static_cast<unsigned long long>(0));
    CHECK_EQ(initial.coalesced, static_cast<unsigned long long>(0));
    CHECK_EQ(initial.acknowledgements, static_cast<unsigned long long>(0));
    CHECK_EQ(initial.unbalancedAcknowledgements, static_cast<unsigned long long>(0));
    CHECK_EQ(initial.resets, static_cast<unsigned long long>(0));
    CHECK_EQ(initial.pending, false);
    CHECK_EQ(coalescer.IsPending(), false);

    // The first publish of the generation is the one that posts.
    REQUIRE(coalescer.TryClaimPost());
    CHECK_EQ(coalescer.IsPending(), true);

    // A second claim before the acknowledgement is refused: the outstanding
    // message already covers this frame.
    CHECK_EQ(coalescer.TryClaimPost(), false);

    // The publish rate is far above the UI rate, so simulate the surplus a
    // single unacknowledged generation produces at ~350 FPS.
    const int surplus = 350;
    for (int frame = 0; frame < surplus; ++frame)
    {
        CHECK_EQ(coalescer.TryClaimPost(), false);
    }

    const RefreshCoalescerSnapshot snapshot = coalescer.Snapshot();
    CHECK_EQ(snapshot.claims, static_cast<unsigned long long>(1));
    CHECK_EQ(snapshot.coalesced, static_cast<unsigned long long>(1 + surplus));
    CHECK_EQ(snapshot.acknowledgements, static_cast<unsigned long long>(0));
    CHECK_EQ(snapshot.pending, true);
}

// ---------------------------------------------------------------------------
// Test 2: the UI acknowledgement clears the claim and lets the next publish
// post again; an acknowledgement with nothing pending is counted, not applied
// to a later frame.
// ---------------------------------------------------------------------------
TEST_CASE(RefreshCoalescerAcknowledgeRearmsTheGate)
{
    RefreshCoalescer coalescer;

    REQUIRE(coalescer.TryClaimPost());
    CHECK_EQ(coalescer.TryClaimPost(), false);

    // The UI received the message.
    REQUIRE(coalescer.Acknowledge());
    CHECK_EQ(coalescer.IsPending(), false);

    // The gate is re-armed: the next publish is the one that posts again.
    REQUIRE(coalescer.TryClaimPost());
    CHECK_EQ(coalescer.IsPending(), true);
    REQUIRE(coalescer.Acknowledge());
    CHECK_EQ(coalescer.IsPending(), false);

    // An acknowledgement with no outstanding claim clears nothing and cannot
    // disarm a claim that belongs to a later frame.
    CHECK_EQ(coalescer.Acknowledge(), false);
    CHECK_EQ(coalescer.IsPending(), false);

    const RefreshCoalescerSnapshot snapshot = coalescer.Snapshot();
    CHECK_EQ(snapshot.claims, static_cast<unsigned long long>(2));
    CHECK_EQ(snapshot.coalesced, static_cast<unsigned long long>(1));
    CHECK_EQ(snapshot.acknowledgements, static_cast<unsigned long long>(2));
    CHECK_EQ(snapshot.unbalancedAcknowledgements, static_cast<unsigned long long>(1));
}

// ---------------------------------------------------------------------------
// Test 3: Reset() clears the pending claim and every counter, so the coalescer
// is ready for a new capture generation.
// ---------------------------------------------------------------------------
TEST_CASE(RefreshCoalescerResetStartsAFreshGeneration)
{
    RefreshCoalescer coalescer;

    REQUIRE(coalescer.TryClaimPost());
    CHECK_EQ(coalescer.TryClaimPost(), false);
    CHECK_EQ(coalescer.Acknowledge(), true);

    coalescer.Reset();

    const RefreshCoalescerSnapshot afterReset = coalescer.Snapshot();
    CHECK_EQ(afterReset.claims, static_cast<unsigned long long>(0));
    CHECK_EQ(afterReset.coalesced, static_cast<unsigned long long>(0));
    CHECK_EQ(afterReset.acknowledgements, static_cast<unsigned long long>(0));
    CHECK_EQ(afterReset.unbalancedAcknowledgements, static_cast<unsigned long long>(0));
    CHECK_EQ(afterReset.resets, static_cast<unsigned long long>(1));
    CHECK_EQ(afterReset.pending, false);

    // A reset coalescer behaves like a fresh one.
    REQUIRE(coalescer.TryClaimPost());
    CHECK_EQ(coalescer.Snapshot().claims, static_cast<unsigned long long>(1));
}

// ---------------------------------------------------------------------------
// Test 4: a reset while a claim is outstanding drops the claim rather than
// leaving the gate stuck.
// ---------------------------------------------------------------------------
TEST_CASE(RefreshCoalescerResetClearsAnOutstandingClaim)
{
    RefreshCoalescer coalescer;

    REQUIRE(coalescer.TryClaimPost());
    REQUIRE(coalescer.IsPending());

    coalescer.Reset();
    CHECK_EQ(coalescer.IsPending(), false);

    REQUIRE(coalescer.TryClaimPost());
    CHECK_EQ(coalescer.Snapshot().claims, static_cast<unsigned long long>(1));
    CHECK_EQ(coalescer.Snapshot().resets, static_cast<unsigned long long>(1));
}

// ---------------------------------------------------------------------------
// Test 5: concurrent producers of one generation produce exactly one
// successful claim, and the gate re-arms for the next generation.
//
// Every producer of a generation is admitted to the race, and the UI
// acknowledges only after every producer of that generation has attempted its
// claim. One claim per generation is then the only outcome that keeps the
// count of winners equal to the generation number.
// ---------------------------------------------------------------------------
TEST_CASE(RefreshCoalescerConcurrentProducersClaimOncePerGeneration)
{
    const int producers = 8;
    const int generations = 200;

    RefreshCoalescer coalescer;

    std::atomic<int> arrived(0);
    std::atomic<int> finished(0);
    std::atomic<int> releaseEpoch(0);
    std::atomic<long long> wins(0);

    std::vector<std::thread> threads;
    for (int producer = 0; producer < producers; ++producer)
    {
        threads.push_back(std::thread([&coalescer, &arrived, &finished,
                                       &releaseEpoch, &wins, generations]()
        {
            for (int generation = 1; generation <= generations; ++generation)
            {
                // Announce arrival, then wait for this generation's start.
                arrived.fetch_add(1);
                while (releaseEpoch.load() < generation)
                {
                    std::this_thread::yield();
                }

                if (coalescer.TryClaimPost())
                {
                    wins.fetch_add(1);
                }

                finished.fetch_add(1);
            }
        }));
    }

    for (int generation = 1; generation <= generations; ++generation)
    {
        const int expected = producers * generation;

        while (arrived.load() < expected)
        {
            std::this_thread::yield();
        }

        releaseEpoch.store(generation);

        while (finished.load() < expected)
        {
            std::this_thread::yield();
        }

        // Every producer of this generation has raced; exactly one won.
        CHECK_EQ(wins.load(), static_cast<long long>(generation));

        REQUIRE(coalescer.Acknowledge());
    }

    for (std::size_t index = 0; index < threads.size(); ++index)
    {
        threads[index].join();
    }

    const RefreshCoalescerSnapshot snapshot = coalescer.Snapshot();
    CHECK_EQ(snapshot.claims, static_cast<unsigned long long>(generations));
    CHECK_EQ(snapshot.coalesced,
             static_cast<unsigned long long>(producers * generations - generations));
    CHECK_EQ(snapshot.acknowledgements, static_cast<unsigned long long>(generations));
    CHECK_EQ(snapshot.pending, false);
}

// ---------------------------------------------------------------------------
// Test 6: with producers and the UI running freely at the same time, the
// invariant that one post is outstanding at a time still holds: every
// successful claim is matched by an acknowledgement, except possibly one
// claim that is still pending at the end.
// ---------------------------------------------------------------------------
TEST_CASE(RefreshCoalescerFreeRunningProducersKeepOnePostOutstanding)
{
    const int producers = 4;
    const int attemptsPerProducer = 20000;

    RefreshCoalescer coalescer;

    std::atomic<bool> start(false);
    std::atomic<long long> wins(0);
    std::atomic<long long> attempts(0);

    std::vector<std::thread> threads;
    for (int producer = 0; producer < producers; ++producer)
    {
        threads.push_back(std::thread([&]()
        {
            while (!start.load())
            {
                std::this_thread::yield();
            }

            for (int attempt = 0; attempt < attemptsPerProducer; ++attempt)
            {
                attempts.fetch_add(1);
                if (coalescer.TryClaimPost())
                {
                    wins.fetch_add(1);
                }
            }
        }));
    }

    // The UI side acknowledges whatever is outstanding, for as long as the
    // producers are running.
    std::thread ui([&]()
    {
        while (!start.load())
        {
            std::this_thread::yield();
        }

        while (attempts.load() < static_cast<long long>(producers) * attemptsPerProducer)
        {
            coalescer.Acknowledge();
        }
    });

    start.store(true);

    for (std::size_t index = 0; index < threads.size(); ++index)
    {
        threads[index].join();
    }
    ui.join();

    // Drain the last outstanding claim, if any. After this, no claim can be
    // outstanding, so every successful claim must have been acknowledged.
    coalescer.Acknowledge();

    const RefreshCoalescerSnapshot snapshot = coalescer.Snapshot();
    CHECK_EQ(snapshot.claims, static_cast<unsigned long long>(wins.load()));
    CHECK_EQ(snapshot.claims,
             static_cast<unsigned long long>(producers) * attemptsPerProducer -
                 snapshot.coalesced);
    CHECK_EQ(snapshot.acknowledgements, snapshot.claims);
    CHECK_EQ(snapshot.pending, false);
}

// ---------------------------------------------------------------------------
// Test 7: the counters saturate instead of wrapping.
// ---------------------------------------------------------------------------
TEST_CASE(RefreshCoalescerCountersSaturateInsteadOfWrapping)
{
    const unsigned long long maximum = static_cast<unsigned long long>(-1);

    CHECK_EQ(SaturatingAddCount(0, 0), static_cast<unsigned long long>(0));
    CHECK_EQ(SaturatingAddCount(2, 3), static_cast<unsigned long long>(5));
    CHECK_EQ(SaturatingAddCount(maximum - 1, 1), maximum);
    CHECK_EQ(SaturatingAddCount(maximum, 1), maximum);
    CHECK_EQ(SaturatingAddCount(maximum, maximum), maximum);
    CHECK_EQ(SaturatingAddCount(maximum - 3, 5), maximum);
    CHECK_EQ(SaturatingAddCount(maximum, 0), maximum);
}

// ---------------------------------------------------------------------------
// Test 8: the snapshot is plain-old-data and the coalescer owns atomic state
// only - it is not copyable, and copying a snapshot is a byte copy.
// ---------------------------------------------------------------------------
TEST_CASE(RefreshCoalescerSnapshotIsPlainOldData)
{
    CHECK_EQ(std::is_trivially_copyable<RefreshCoalescerSnapshot>::value, true);
    CHECK_EQ(std::is_copy_constructible<RefreshCoalescer>::value, false);
    CHECK_EQ(std::is_copy_assignable<RefreshCoalescer>::value, false);

    // A copy of a snapshot is independent of the coalescer.
    RefreshCoalescer coalescer;
    REQUIRE(coalescer.TryClaimPost());
    const RefreshCoalescerSnapshot copy = coalescer.Snapshot();
    REQUIRE(coalescer.Acknowledge());
    CHECK_EQ(copy.pending, true);
    CHECK_EQ(coalescer.Snapshot().pending, false);
}
