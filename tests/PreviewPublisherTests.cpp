// PreviewPublisherTests.cpp - behavioural tests for the live preview pool.
//
// The pool is what the acquisition callback publishes into and what the UI
// draws from, so the tests concentrate on the properties that make that safe:
// the row layout a blit needs, coalescing when the UI falls behind, the
// guarantee that a displayed frame is never overwritten, the counters that
// report what was lost, and the absence of tearing while a publisher and a
// display thread run at the same time.
//
// The tests are hardware free: the C++ standard library plus the Win32
// allocator underneath PixelStore is all they need.

#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include "TestHarness.h"

#include "PreviewPublisher.h"

using grablinkcore::ComputePreviewLayout;
using grablinkcore::PreviewLayout;
using grablinkcore::PreviewPublisher;
using grablinkcore::SaturatingIncrement;

namespace
{

// Fills the useful bytes of every row of a padded source surface with "value"
// and marks the padding with a different byte, so that a test can prove only
// the useful bytes of a row were copied.
void FillSource(std::vector<unsigned char>& source, std::size_t pitch,
                int width, int height, unsigned char value,
                unsigned char paddingValue)
{
    std::memset(&source[0], paddingValue, source.size());
    for (int row = 0; row < height; ++row)
    {
        std::memset(&source[static_cast<std::size_t>(row) * pitch], value,
                    static_cast<std::size_t>(width));
    }
}

// True when every useful byte of every row of a frame is "expected".
bool FrameIsUniform(const unsigned char* frame, std::size_t pitch, int width,
                    int height, unsigned char expected)
{
    for (int row = 0; row < height; ++row)
    {
        const unsigned char* rowStart = frame + static_cast<std::size_t>(row) * pitch;
        for (int column = 0; column < width; ++column)
        {
            if (rowStart[column] != expected)
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1: the DIB layout helper pads rows out to a DWORD boundary.
// ---------------------------------------------------------------------------
TEST_CASE(PreviewLayoutAlignsRowsToDwordBoundary)
{
    PreviewLayout layout;

    REQUIRE(ComputePreviewLayout(5, 3, layout));
    CHECK_EQ(layout.width, 5);
    CHECK_EQ(layout.height, 3);
    CHECK_EQ(layout.pitch, static_cast<std::size_t>(8));
    CHECK_EQ(layout.bytes, static_cast<std::size_t>(24));

    // Already a multiple of four: no extra padding.
    REQUIRE(ComputePreviewLayout(4, 2, layout));
    CHECK_EQ(layout.pitch, static_cast<std::size_t>(4));
    CHECK_EQ(layout.bytes, static_cast<std::size_t>(8));

    // The narrowest possible row still occupies a whole DWORD.
    REQUIRE(ComputePreviewLayout(1, 1, layout));
    CHECK_EQ(layout.pitch, static_cast<std::size_t>(4));
    CHECK_EQ(layout.bytes, static_cast<std::size_t>(4));

    REQUIRE(ComputePreviewLayout(640, 480, layout));
    CHECK_EQ(layout.pitch, static_cast<std::size_t>(640));
    CHECK_EQ(layout.bytes, static_cast<std::size_t>(640 * 480));
}

// ---------------------------------------------------------------------------
// Test 2: impossible geometry is refused and leaves the layout cleared.
// ---------------------------------------------------------------------------
TEST_CASE(PreviewLayoutRejectsImpossibleGeometry)
{
    PreviewLayout layout;
    REQUIRE(ComputePreviewLayout(4, 4, layout));

    CHECK(!ComputePreviewLayout(0, 4, layout));
    CHECK(!ComputePreviewLayout(-3, 4, layout));
    CHECK(!ComputePreviewLayout(4, 0, layout));
    CHECK(!ComputePreviewLayout(4, -1, layout));

    CHECK_EQ(layout.width, 0);
    CHECK_EQ(layout.height, 0);
    CHECK_EQ(layout.pitch, static_cast<std::size_t>(0));
    CHECK_EQ(layout.bytes, static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 3: counters saturate instead of wrapping around.
// ---------------------------------------------------------------------------
TEST_CASE(SaturatingIncrementStopsAtTheLargestCount)
{
    CHECK_EQ(SaturatingIncrement(0), static_cast<std::size_t>(1));
    CHECK_EQ(SaturatingIncrement(41), static_cast<std::size_t>(42));

    const std::size_t largest = static_cast<std::size_t>(-1);
    CHECK_EQ(SaturatingIncrement(largest - 1), largest);
    CHECK_EQ(SaturatingIncrement(largest), largest);
}

// ---------------------------------------------------------------------------
// Test 4: Configure() preallocates the whole pool in one block of three slots.
// ---------------------------------------------------------------------------
TEST_CASE(ConfigurePreallocatesThreeDistinctSlots)
{
    PreviewPublisher publisher;
    REQUIRE(publisher.Configure(5, 3));

    CHECK(publisher.IsConfigured());
    CHECK_EQ(publisher.Width(), 5);
    CHECK_EQ(publisher.Height(), 3);
    CHECK_EQ(publisher.Pitch(), static_cast<std::size_t>(8));
    CHECK_EQ(publisher.SlotBytes(), static_cast<std::size_t>(24));
    CHECK_EQ(publisher.ReadyCount(), static_cast<std::size_t>(0));
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(0));
    CHECK(publisher.LastError().empty());

    const unsigned char* first = publisher.SlotData(0);
    const unsigned char* second = publisher.SlotData(1);
    const unsigned char* third = publisher.SlotData(2);
    REQUIRE(first != 0);
    REQUIRE(second != 0);
    REQUIRE(third != 0);
    CHECK(first != second);
    CHECK(second != third);
    CHECK(first != third);

    // The slots are consecutive: one allocation, three equal parts.
    CHECK_EQ(static_cast<std::size_t>(second - first), publisher.SlotBytes());
    CHECK_EQ(static_cast<std::size_t>(third - second), publisher.SlotBytes());
    CHECK(publisher.SlotData(3) == 0);
}

// ---------------------------------------------------------------------------
// Test 5: a configure that cannot work leaves the publisher unconfigured.
// ---------------------------------------------------------------------------
TEST_CASE(ConfigureRejectsInvalidGeometry)
{
    PreviewPublisher publisher;

    CHECK(!publisher.Configure(0, 10));
    CHECK(!publisher.IsConfigured());
    CHECK(!publisher.LastError().empty());
    CHECK(publisher.SlotData(0) == 0);
    CHECK_EQ(publisher.Pitch(), static_cast<std::size_t>(0));

    CHECK(!publisher.Configure(-4, 10));
    CHECK(!publisher.IsConfigured());

    CHECK(!publisher.Configure(10, 0));
    CHECK(!publisher.IsConfigured());

    CHECK(!publisher.Configure(10, -2));
    CHECK(!publisher.IsConfigured());
}

// ---------------------------------------------------------------------------
// Test 6: an allocation the system cannot satisfy is reported, not thrown.
// ---------------------------------------------------------------------------
TEST_CASE(ConfigureReportsAllocationFailureInsteadOfThrowing)
{
    PreviewPublisher publisher;

    // A billion by a billion pixels is arithmetically representable but far
    // larger than any address space, so the allocation itself has to fail.
    CHECK(!publisher.Configure(1000000000, 1000000000));
    CHECK(!publisher.IsConfigured());
    CHECK(!publisher.LastError().empty());

    // The refused configure leaves a usable publisher behind.
    REQUIRE(publisher.Configure(4, 2));
    CHECK(publisher.SlotData(0) != 0);
}

// ---------------------------------------------------------------------------
// Test 7: Publish() copies the useful bytes of each row from a padded source
// into the DWORD-aligned rows of the slot.
// ---------------------------------------------------------------------------
TEST_CASE(PublishCopiesUsefulBytesRowByRowFromAPaddedSource)
{
    const int width = 5;
    const int height = 3;
    const std::size_t sourcePitch = 7;   // deliberately not the slot pitch

    PreviewPublisher publisher;
    REQUIRE(publisher.Configure(width, height));
    REQUIRE_EQ(publisher.Pitch(), static_cast<std::size_t>(8));

    std::vector<unsigned char> source(sourcePitch * static_cast<std::size_t>(height));
    FillSource(source, sourcePitch, width, height, 0x5A, 0xEE);

    REQUIRE(publisher.Publish(&source[0], static_cast<int>(sourcePitch)));

    CHECK_EQ(publisher.ReadyCount(), static_cast<std::size_t>(1));
    CHECK_EQ(publisher.Snapshot().published, static_cast<std::size_t>(1));
    CHECK(publisher.LastError().empty());

    const unsigned char* frame = publisher.TakeForDisplay();
    REQUIRE(frame != 0);

    // Every useful byte landed, and the rows are one DWORD-aligned pitch apart.
    for (int row = 0; row < height; ++row)
    {
        const unsigned char* rowStart = frame + static_cast<std::size_t>(row) * publisher.Pitch();
        for (int column = 0; column < width; ++column)
        {
            CHECK_EQ(rowStart[column], static_cast<unsigned char>(0x5A));
        }
        // Only the useful bytes were copied: the source's padding byte must not
        // have been carried into the padding of the destination row.
        for (std::size_t offset = static_cast<std::size_t>(width);
             offset < publisher.Pitch(); ++offset)
        {
            CHECK(rowStart[offset] != static_cast<unsigned char>(0xEE));
        }
    }
}

// ---------------------------------------------------------------------------
// Test 8: bad arguments are refused and counted, and do not poison the pool.
// ---------------------------------------------------------------------------
TEST_CASE(PublishRefusesInvalidArguments)
{
    PreviewPublisher publisher;

    // Nothing to publish into yet.
    CHECK(!publisher.Publish(0, 0));
    CHECK(!publisher.LastError().empty());

    REQUIRE(publisher.Configure(5, 3));
    CHECK_EQ(publisher.Snapshot().refused, static_cast<std::size_t>(0));

    std::vector<unsigned char> source(15, 0x11);

    CHECK(!publisher.Publish(0, 5));       // null source
    CHECK(!publisher.Publish(&source[0], 4));   // pitch narrower than the width
    CHECK(!publisher.Publish(&source[0], -1));  // negative pitch

    const PreviewPublisher::Counters counters = publisher.Snapshot();
    CHECK_EQ(counters.refused, static_cast<std::size_t>(3));
    CHECK_EQ(counters.published, static_cast<std::size_t>(0));
    CHECK_EQ(counters.dropped, static_cast<std::size_t>(0));
    CHECK_EQ(publisher.ReadyCount(), static_cast<std::size_t>(0));
    CHECK(!publisher.LastError().empty());

    // A valid publish still works after the refusals.
    CHECK(publisher.Publish(&source[0], 5));
    CHECK_EQ(publisher.ReadyCount(), static_cast<std::size_t>(1));
}

// ---------------------------------------------------------------------------
// Test 9: when the UI falls behind, an older unpublished frame is coalesced
// away instead of the new frame being dropped.
// ---------------------------------------------------------------------------
TEST_CASE(PublishCoalescesOlderUnpublishedFrames)
{
    const int width = 8;
    const int height = 2;
    const std::size_t pitch = 8;

    PreviewPublisher publisher;
    REQUIRE(publisher.Configure(width, height));

    std::vector<unsigned char> source(pitch * static_cast<std::size_t>(height));
    for (int frame = 1; frame <= 5; ++frame)
    {
        FillSource(source, pitch, width, height, static_cast<unsigned char>(frame), 0xEE);
        REQUIRE(publisher.Publish(&source[0], static_cast<int>(pitch)));
    }

    const PreviewPublisher::Counters counters = publisher.Snapshot();
    CHECK_EQ(counters.published, static_cast<std::size_t>(5));
    CHECK_EQ(counters.replaced, static_cast<std::size_t>(2));   // five frames, three slots
    CHECK_EQ(counters.dropped, static_cast<std::size_t>(0));
    CHECK_EQ(publisher.ReadyCount(), static_cast<std::size_t>(3));

    // The newest frame survived the coalescing...
    const unsigned char* newest = publisher.TakeForDisplay();
    REQUIRE(newest != 0);
    CHECK(FrameIsUniform(newest, publisher.Pitch(), width, height, 5));

    // ...and the frame published just before it is still there.
    const unsigned char* previous = publisher.TakeForDisplay();
    REQUIRE(previous != 0);
    CHECK(previous != newest);
    CHECK(FrameIsUniform(previous, publisher.Pitch(), width, height, 4));

    publisher.ReleaseDisplay();
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 10: the frame the UI is displaying is never overwritten - the flaw the
// single-buffer design had.
// ---------------------------------------------------------------------------
TEST_CASE(PublishNeverOverwritesTheDisplayedFrame)
{
    const int width = 6;
    const int height = 4;
    const std::size_t pitch = 8;

    PreviewPublisher publisher;
    REQUIRE(publisher.Configure(width, height));

    std::vector<unsigned char> source(pitch * static_cast<std::size_t>(height));
    FillSource(source, pitch, width, height, 0xA1, 0xEE);
    REQUIRE(publisher.Publish(&source[0], static_cast<int>(pitch)));

    const unsigned char* displayed = publisher.TakeForDisplay();
    REQUIRE(displayed != 0);
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(1));

    // Twenty further frames must not touch the frame being displayed.
    for (int frame = 0; frame < 20; ++frame)
    {
        FillSource(source, pitch, width, height, 0x22, 0xEE);
        REQUIRE(publisher.Publish(&source[0], static_cast<int>(pitch)));
        CHECK(FrameIsUniform(displayed, publisher.Pitch(), width, height, 0xA1));
    }

    CHECK_EQ(publisher.Snapshot().dropped, static_cast<std::size_t>(0));
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(1));

    // The newer frame can be taken without giving up the pinned one.
    const unsigned char* newer = publisher.TakeForDisplay();
    REQUIRE(newer != 0);
    CHECK(newer != displayed);
    CHECK(FrameIsUniform(newer, publisher.Pitch(), width, height, 0x22));
    CHECK(FrameIsUniform(displayed, publisher.Pitch(), width, height, 0xA1));

    publisher.ReleaseDisplay();
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 11: a frame with nowhere to go is dropped and counted, and the pool
// recovers as soon as the display releases its slots.
// ---------------------------------------------------------------------------
TEST_CASE(DroppedFramesAreCountedAndThePoolRecoversAfterRelease)
{
    PreviewPublisher publisher;
    REQUIRE(publisher.Configure(4, 2));

    std::vector<unsigned char> source(8, 0x33);
    for (int frame = 0; frame < 3; ++frame)
    {
        REQUIRE(publisher.Publish(&source[0], 4));
    }

    // Pin every slot: with all three held by the display there is nowhere for a
    // new frame to go, so it is dropped - counted, not silently lost.
    REQUIRE(publisher.TakeForDisplay() != 0);
    REQUIRE(publisher.TakeForDisplay() != 0);
    REQUIRE(publisher.TakeForDisplay() != 0);
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(3));

    CHECK(!publisher.Publish(&source[0], 4));
    CHECK(!publisher.LastError().empty());

    PreviewPublisher::Counters counters = publisher.Snapshot();
    CHECK_EQ(counters.dropped, static_cast<std::size_t>(1));
    CHECK_EQ(counters.published, static_cast<std::size_t>(3));
    CHECK_EQ(counters.replaced, static_cast<std::size_t>(0));

    publisher.ReleaseDisplay();
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(0));

    CHECK(publisher.Publish(&source[0], 4));
    CHECK(publisher.LastError().empty());
    counters = publisher.Snapshot();
    CHECK_EQ(counters.published, static_cast<std::size_t>(4));
    CHECK_EQ(counters.dropped, static_cast<std::size_t>(1));
    CHECK_EQ(publisher.ReadyCount(), static_cast<std::size_t>(1));
}

// ---------------------------------------------------------------------------
// Test 12: TakeForDisplay() refuses politely when there is nothing to show.
// ---------------------------------------------------------------------------
TEST_CASE(TakeForDisplayRefusesWhenNothingIsReady)
{
    PreviewPublisher publisher;

    CHECK(publisher.TakeForDisplay() == 0);   // not configured

    REQUIRE(publisher.Configure(4, 2));
    CHECK_EQ(publisher.Snapshot().refused, static_cast<std::size_t>(0));

    CHECK(publisher.TakeForDisplay() == 0);   // configured, nothing published
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(0));
    CHECK_EQ(publisher.Snapshot().leased, static_cast<std::size_t>(0));
    CHECK_EQ(publisher.Snapshot().refused, static_cast<std::size_t>(1));
    CHECK(!publisher.LastError().empty());

    std::vector<unsigned char> source(8, 0x77);
    REQUIRE(publisher.Publish(&source[0], 4));
    REQUIRE(publisher.TakeForDisplay() != 0);
    CHECK_EQ(publisher.Snapshot().leased, static_cast<std::size_t>(1));
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(1));
    CHECK(publisher.LastError().empty());

    publisher.ReleaseDisplay();
}

// ---------------------------------------------------------------------------
// Test 13: publishing never moves or reallocates the pool - the frames come out
// of the storage Configure() allocated once.
// ---------------------------------------------------------------------------
TEST_CASE(PublishNeverReallocatesThePool)
{
    PreviewPublisher publisher;
    REQUIRE(publisher.Configure(9, 5));

    const unsigned char* slots[3];
    for (std::size_t index = 0; index < 3; ++index)
    {
        slots[index] = publisher.SlotData(index);
        REQUIRE(slots[index] != 0);
    }

    std::vector<unsigned char> source(publisher.Pitch() * static_cast<std::size_t>(publisher.Height()), 0x10);
    for (int frame = 0; frame < 200; ++frame)
    {
        source[0] = static_cast<unsigned char>(frame);
        REQUIRE(publisher.Publish(&source[0], static_cast<int>(publisher.Pitch())));

        const unsigned char* displayed = publisher.TakeForDisplay();
        if (displayed != 0)
        {
            CHECK(displayed == slots[0] || displayed == slots[1] || displayed == slots[2]);
            publisher.ReleaseDisplay();
        }
    }

    // The slots are where they were before any of the traffic above.
    for (std::size_t index = 0; index < 3; ++index)
    {
        CHECK_EQ(publisher.SlotData(index), slots[index]);
    }
}

// ---------------------------------------------------------------------------
// Test 14: Reset() gives the pool up, and a later configure may use another
// geometry.
// ---------------------------------------------------------------------------
TEST_CASE(ResetReleasesThePoolAndItsCounters)
{
    PreviewPublisher publisher;
    REQUIRE(publisher.Configure(5, 5));

    std::vector<unsigned char> source(publisher.Pitch() * 5, 0x40);
    REQUIRE(publisher.Publish(&source[0], static_cast<int>(publisher.Pitch())));
    REQUIRE(publisher.TakeForDisplay() != 0);

    publisher.Reset();

    CHECK(!publisher.IsConfigured());
    CHECK_EQ(publisher.Width(), 0);
    CHECK_EQ(publisher.Height(), 0);
    CHECK_EQ(publisher.Pitch(), static_cast<std::size_t>(0));
    CHECK_EQ(publisher.SlotBytes(), static_cast<std::size_t>(0));
    CHECK_EQ(publisher.ReadyCount(), static_cast<std::size_t>(0));
    CHECK_EQ(publisher.LeaseCount(), static_cast<std::size_t>(0));
    CHECK(publisher.SlotData(0) == 0);
    CHECK(publisher.TakeForDisplay() == 0);

    PreviewPublisher::Counters counters = publisher.Snapshot();
    CHECK_EQ(counters.published, static_cast<std::size_t>(0));
    CHECK_EQ(counters.replaced, static_cast<std::size_t>(0));
    CHECK_EQ(counters.dropped, static_cast<std::size_t>(0));
    CHECK_EQ(counters.leased, static_cast<std::size_t>(0));
    CHECK_EQ(counters.refused, static_cast<std::size_t>(1));   // the take above

    // Reconfiguring with another geometry gives a pool of the new size.
    REQUIRE(publisher.Configure(10, 2));
    CHECK_EQ(publisher.Pitch(), static_cast<std::size_t>(12));
    CHECK_EQ(publisher.SlotBytes(), static_cast<std::size_t>(24));
    CHECK_EQ(publisher.Snapshot().published, static_cast<std::size_t>(0));
    CHECK_EQ(publisher.Snapshot().refused, static_cast<std::size_t>(0));
    CHECK(publisher.SlotData(2) != 0);
    CHECK(publisher.SlotData(3) == 0);

    publisher.Reset();
    CHECK(!publisher.IsConfigured());
}

// ---------------------------------------------------------------------------
// Test 15: a publisher and a display running at the same time never hand out a
// frame that is half one image and half the next.
// ---------------------------------------------------------------------------
TEST_CASE(PreviewPublisherDoesNotTearUnderConcurrentPublishAndDisplay)
{
    const int width = 64;
    const int height = 32;
    const std::size_t pitch = 64;

    PreviewPublisher publisher;
    REQUIRE(publisher.Configure(width, height));

    // Every byte of a frame carries the frame's seed, so any mixture of two
    // frames is detectable by inspection alone.
    std::vector<unsigned char> source(pitch * static_cast<std::size_t>(height));

    std::atomic<bool> stop(false);
    std::atomic<int> tears(0);
    std::atomic<int> inspected(0);

    std::thread display([&publisher, &stop, &tears, &inspected, width, height, pitch]()
    {
        while (!stop.load())
        {
            const unsigned char* frame = publisher.TakeForDisplay();
            if (frame == 0)
            {
                continue;
            }

            const unsigned char seed = frame[0];
            if (!FrameIsUniform(frame, pitch, width, height, seed))
            {
                ++tears;
            }
            ++inspected;
            publisher.ReleaseDisplay();
        }
    });

    const int frames = 3000;
    for (int frame = 0; frame < frames; ++frame)
    {
        const unsigned char seed = static_cast<unsigned char>(1 + (frame % 250));
        std::memset(&source[0], seed, source.size());
        publisher.Publish(&source[0], static_cast<int>(pitch));
    }

    // Keep publishing until the display thread has certainly inspected a frame,
    // so the check below cannot pass merely because nothing was ever taken.
    for (int guard = 0; guard < 200000 && inspected.load() == 0; ++guard)
    {
        publisher.Publish(&source[0], static_cast<int>(pitch));
    }
    const int seen = inspected.load();

    stop = true;
    display.join();

    CHECK_EQ(tears.load(), 0);
    CHECK(seen > 0);
    CHECK_EQ(publisher.Snapshot().published > 0, true);
}
