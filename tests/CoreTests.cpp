// CoreTests.cpp - behavioural tests for the hardware-free core.
//
// Every test was added one at a time following RED -> GREEN: the test was
// compiled and executed against the current implementation first (RED), then
// the implementation was completed until the test passed (GREEN).

// The standard headers are taken in before the harness pulls in <windows.h>,
// so that no Win32 macro (min/max in particular) can disturb the library.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "TestHarness.h"

#include "BmpWriter.h"
#include "FrameBuffer.h"
#include "SaveWorker.h"

using grablinkcore::BmpFileSink;
using grablinkcore::BmpWriter;
using grablinkcore::BmpWriteResult;
using grablinkcore::FrameBuffer;
using grablinkcore::FrameSnapshot;
using grablinkcore::SaveProgress;
using grablinkcore::SaveSink;
using grablinkcore::SaveWorker;

namespace
{
// Expected BMP layout, spelled out here instead of being taken from the
// implementation: 14-byte BITMAPFILEHEADER, 40-byte BITMAPINFOHEADER, then
// 256 palette entries of 4 bytes.
const unsigned int kBmpPixelDataOffset = 14 + 40 + 1024;
const unsigned int kBmpPaletteOffset = 14 + 40;
}

// ---------------------------------------------------------------------------
// Test 1: the buffer reports the geometry it was configured with.
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferExposesConfiguredGeometryAfterInit)
{
    FrameBuffer buffer;

    REQUIRE(buffer.Init(4, 3, 5));

    CHECK(buffer.IsInitialized());
    CHECK_EQ(buffer.Width(), 4);
    CHECK_EQ(buffer.Height(), 3);
    CHECK_EQ(buffer.BytesPerFrame(), static_cast<std::size_t>(12));
    CHECK_EQ(buffer.Capacity(), static_cast<std::size_t>(5));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(60));
    CHECK(buffer.Data() != 0);
    CHECK(buffer.Frame(0) == 0);
    CHECK(buffer.LastError().empty());
}

// ---------------------------------------------------------------------------
// Test 2: Append copies one packed source frame (pitch == width).
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferAppendCopiesPackedFrame)
{
    FrameBuffer buffer;
    REQUIRE(buffer.Init(4, 2, 3));

    const unsigned char source[8] = { 10, 11, 12, 13, 20, 21, 22, 23 };
    REQUIRE(buffer.Append(source, 4));

    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(1));
    CHECK(buffer.LastError().empty());
    REQUIRE(buffer.Frame(0) != 0);
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_EQ(buffer.Frame(0)[index], source[index]);
    }
    CHECK(buffer.Frame(1) == 0);
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(1));
}

// ---------------------------------------------------------------------------
// Test 3: Append skips the padding of a source buffer whose pitch > width, and
// stores the frames packed (no padding) one after another.
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferAppendHonoursPaddedSourcePitch)
{
    FrameBuffer buffer;
    REQUIRE(buffer.Init(3, 2, 2));

    // Two rows of 3 useful bytes inside an 8-byte pitch. The 0xAA padding bytes
    // must never end up in the packed buffer.
    const unsigned char padded[16] = {
        1, 2, 3, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
        4, 5, 6, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA };
    REQUIRE(buffer.Append(padded, 8));

    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(1));
    REQUIRE(buffer.Frame(0) != 0);
    CHECK_EQ(buffer.Frame(0)[0], 1);
    CHECK_EQ(buffer.Frame(0)[1], 2);
    CHECK_EQ(buffer.Frame(0)[2], 3);
    CHECK_EQ(buffer.Frame(0)[3], 4);
    CHECK_EQ(buffer.Frame(0)[4], 5);
    CHECK_EQ(buffer.Frame(0)[5], 6);

    // The next frame starts immediately after the packed 6 bytes of frame 0.
    const unsigned char second[16] = {
        7, 8, 9, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB,
        10, 11, 12, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB };
    REQUIRE(buffer.Append(second, 8));

    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(2));
    REQUIRE(buffer.Frame(1) != 0);
    CHECK_EQ(buffer.Frame(1)[0], 7);
    CHECK_EQ(buffer.Frame(1)[1], 8);
    CHECK_EQ(buffer.Frame(1)[2], 9);
    CHECK_EQ(buffer.Frame(1)[3], 10);
    CHECK_EQ(buffer.Frame(1)[4], 11);
    CHECK_EQ(buffer.Frame(1)[5], 12);
}

// ---------------------------------------------------------------------------
// Test 4: an Append beyond Capacity() fails and must not touch Count() or the
// frames already stored.
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferRejectsAppendPastCapacityWithoutCorruption)
{
    FrameBuffer buffer;
    REQUIRE(buffer.Init(2, 2, 2));

    const unsigned char first[4] = { 1, 2, 3, 4 };
    const unsigned char second[4] = { 5, 6, 7, 8 };
    const unsigned char third[4] = { 9, 10, 11, 12 };

    REQUIRE(buffer.Append(first, 2));
    REQUIRE(buffer.Append(second, 2));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(2));

    CHECK(!buffer.Append(third, 2));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(2));
    CHECK(!buffer.LastError().empty());
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(8));
    CHECK(buffer.Frame(2) == 0);

    REQUIRE(buffer.Frame(0) != 0);
    REQUIRE(buffer.Frame(1) != 0);
    CHECK_EQ(buffer.Frame(0)[0], 1);
    CHECK_EQ(buffer.Frame(0)[1], 2);
    CHECK_EQ(buffer.Frame(0)[2], 3);
    CHECK_EQ(buffer.Frame(0)[3], 4);
    CHECK_EQ(buffer.Frame(1)[0], 5);
    CHECK_EQ(buffer.Frame(1)[1], 6);
    CHECK_EQ(buffer.Frame(1)[2], 7);
    CHECK_EQ(buffer.Frame(1)[3], 8);
}

// ---------------------------------------------------------------------------
// Test 5: Append rejects a null source and a pitch narrower than the frame
// width, and keeps working afterwards.
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferRejectsNullSourceAndNarrowPitch)
{
    FrameBuffer buffer;
    REQUIRE(buffer.Init(4, 2, 3));

    const unsigned char source[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    // pitch < width first: a rejected call must not read or write anything.
    REQUIRE(!buffer.Append(source, 3));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
    CHECK(!buffer.LastError().empty());

    CHECK(!buffer.Append(0, 4));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
    CHECK(!buffer.LastError().empty());

    CHECK(!buffer.Append(0, 0));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));

    // A buffer that was never initialised rejects Appends as well.
    FrameBuffer uninitialised;
    CHECK(!uninitialised.Append(source, 4));
    CHECK_EQ(uninitialised.Count(), static_cast<std::size_t>(0));
    CHECK(!uninitialised.LastError().empty());

    // The rejected calls must not have damaged the buffer.
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(24));
    REQUIRE(buffer.Append(source, 4));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(1));
    REQUIRE(buffer.Frame(0) != 0);
    CHECK_EQ(buffer.Frame(0)[4], 5);
    CHECK(buffer.LastError().empty());
}

// ---------------------------------------------------------------------------
// Test 6: Clear() drops the stored frames but keeps the allocation (and the
// bytes in it), so the buffer can be filled again without reallocating.
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferClearAllowsReuseWithoutReallocating)
{
    FrameBuffer buffer;
    REQUIRE(buffer.Init(2, 2, 2));

    const unsigned char first[4] = { 1, 2, 3, 4 };
    const unsigned char second[4] = { 5, 6, 7, 8 };

    REQUIRE(buffer.Append(first, 2));
    REQUIRE(buffer.Append(second, 2));
    const unsigned char* storage = buffer.Data();
    REQUIRE(storage != 0);

    buffer.Clear();

    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.Capacity(), static_cast<std::size_t>(2));
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(8));
    CHECK(buffer.IsInitialized());
    CHECK(buffer.Frame(0) == 0);
    CHECK(buffer.LastError().empty());

    // Clear() must not release the storage...
    CHECK(buffer.Data() == storage);
    // ...and must not zero it either: the previous payload is still readable.
    CHECK_EQ(buffer.Data()[0], 1);
    CHECK_EQ(buffer.Data()[3], 4);

    // Reuse: the next Append writes into frame slot 0 again.
    REQUIRE(buffer.Append(second, 2));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(1));
    REQUIRE(buffer.Frame(0) != 0);
    CHECK_EQ(buffer.Frame(0)[0], 5);
    CHECK_EQ(buffer.Frame(0)[3], 8);

    REQUIRE(buffer.Append(first, 2));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(2));
    CHECK(!buffer.Append(first, 2));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(2));
}

// ---------------------------------------------------------------------------
// Test 7: Init() rejects non-positive dimensions and a zero capacity, leaves no
// usable buffer behind and reports the reason.
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferRejectsInvalidDimensions)
{
    FrameBuffer buffer;

    REQUIRE(!buffer.Init(0, 4, 2));
    CHECK(!buffer.IsInitialized());
    CHECK(!buffer.LastError().empty());
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.Width(), 0);
    CHECK_EQ(buffer.Height(), 0);
    CHECK(buffer.Data() == 0);
    CHECK(buffer.Frame(0) == 0);

    REQUIRE(!buffer.Init(4, 0, 2));
    REQUIRE(!buffer.Init(-3, 4, 2));
    REQUIRE(!buffer.Init(4, -1, 2));
    REQUIRE(!buffer.Init(4, 4, 0));
    CHECK(!buffer.IsInitialized());
    CHECK(!buffer.LastError().empty());

    // A rejected Init() must release whatever a previous Init() allocated.
    REQUIRE(buffer.Init(2, 2, 1));
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(4));
    REQUIRE(!buffer.Init(0, 0, 0));
    CHECK(!buffer.IsInitialized());
    CHECK(buffer.Data() == 0);
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(0));

    // A good Init() still works after the rejected ones.
    REQUIRE(buffer.Init(2, 2, 1));
    CHECK(buffer.IsInitialized());
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(4));

    // The three-argument constructor reports failures instead of throwing.
    FrameBuffer invalid(-1, 2, 3);
    CHECK(!invalid.IsInitialized());
    CHECK(!invalid.LastError().empty());
    CHECK(invalid.Data() == 0);
}

// ---------------------------------------------------------------------------
// Test 8: Init() uses checked size arithmetic - a width*height*capacity that
// wraps around std::size_t must be rejected, never silently truncated.
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferRejectsOverflowingSizeArithmetic)
{
    FrameBuffer buffer;

    const std::size_t frameBytes = static_cast<std::size_t>(4096) * 4096;   // 16 MiB
    const std::size_t wrappingCapacity = static_cast<std::size_t>(1099511627776ULL); // 2^40

    // frameBytes * 2^40 wraps to exactly zero.
    REQUIRE(!buffer.Init(4096, 4096, wrappingCapacity));
    CHECK(!buffer.IsInitialized());
    CHECK(!buffer.LastError().empty());
    CHECK(buffer.Data() == 0);
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.BytesPerFrame(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.Width(), 0);

    // frameBytes * (2^40 + 1) wraps to a small, allocatable number: it must be
    // rejected all the same.
    REQUIRE(!buffer.Init(4096, 4096, wrappingCapacity + 1));
    CHECK(!buffer.IsInitialized());
    CHECK(buffer.Data() == 0);
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(0));

    // The same dimensions with a sane capacity are still accepted.
    REQUIRE(buffer.Init(4096, 4096, 1));
    CHECK(buffer.IsInitialized());
    CHECK_EQ(buffer.BytesPerFrame(), frameBytes);
    CHECK_EQ(buffer.TotalBytes(), frameBytes);
    REQUIRE(buffer.Data() != 0);
}

// ---------------------------------------------------------------------------
// Test 9: an allocation failure is reported through LastError() instead of
// propagating std::bad_alloc out of Init().
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferReportsAllocationFailure)
{
    FrameBuffer buffer;

    // 4096 * 4096 bytes per frame times 2^30 frames is 16 PiB: representable,
    // but far beyond the 128 TiB user-space limit of an x64 process.
    const std::size_t capacity = static_cast<std::size_t>(1) << 30;
    const std::size_t frameBytes = static_cast<std::size_t>(4096) * 4096;

    CHECK(!buffer.Init(4096, 4096, capacity));
    CHECK(!buffer.IsInitialized());
    CHECK(!buffer.LastError().empty());
    CHECK(buffer.Data() == 0);
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.BytesPerFrame(), static_cast<std::size_t>(0));
    CHECK_EQ(frameBytes, static_cast<std::size_t>(16777216));

    // The failed request must not poison the object: a sane Init() still works.
    REQUIRE(buffer.Init(2, 2, 1));
    CHECK(buffer.IsInitialized());
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(4));

    // The three-argument constructor reports the failure the same way.
    FrameBuffer huge(4096, 4096, capacity);
    CHECK(!huge.IsInitialized());
    CHECK(!huge.LastError().empty());
}

// ---------------------------------------------------------------------------
// Working-set probe used by the lazy-allocation test below.
//
// K32QueryWorkingSetEx is exported by kernel32.dll on Windows 7 and later, so it
// is reachable through GetProcAddress and the test project needs no psapi.lib
// (the project file is not part of this change). The struct below matches
// PSAPI_WORKING_SET_EX_INFORMATION: an address, then a flags word whose lowest
// bit is "Valid" - the page is resident because this process already touched it.
// ---------------------------------------------------------------------------
namespace
{
typedef BOOL (WINAPI *QueryWorkingSetExFn)(HANDLE, void*, unsigned long);

struct WorkingSetExEntry
{
    void* VirtualAddress;
    ULONG_PTR Attributes;
};

QueryWorkingSetExFn LoadPageResidencyProbe()
{
    const HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel == 0)
    {
        return 0;
    }
    return reinterpret_cast<QueryWorkingSetExFn>(
        reinterpret_cast<void*>(::GetProcAddress(kernel, "K32QueryWorkingSetEx")));
}

// True when the page holding "address" is resident in the working set, which is
// only possible once the process has read or written that very page.
bool PageIsResident(QueryWorkingSetExFn probe, const void* address)
{
    WorkingSetExEntry entry;
    entry.VirtualAddress = const_cast<void*>(address);
    entry.Attributes = 0;
    if (probe(::GetCurrentProcess(), &entry,
              static_cast<unsigned long>(sizeof(entry))) == 0)
    {
        return false;
    }
    return (entry.Attributes & 1) != 0;
}
} // namespace

// ---------------------------------------------------------------------------
// Lazy allocation: Init() must hand out storage without walking it. A full
// zero fill (what "assign(totalBytes, 0)" does) commits and faults in every
// page of a multi-gigabyte capture buffer before a single frame arrives.
// ---------------------------------------------------------------------------
TEST_CASE(FrameBufferInitLeavesTheStorageUntouched)
{
    const QueryWorkingSetExFn probe = LoadPageResidencyProbe();
    REQUIRE(probe != 0);

    FrameBuffer buffer;

    // 4096 x 4096 byte frames, 16 of them: 256 MiB. Small enough to run on any
    // machine, large enough that zero filling it would be plainly visible.
    REQUIRE(buffer.Init(4096, 4096, 16));
    REQUIRE(buffer.Data() != 0);
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(16) * 4096 * 4096);

    const unsigned char* base = buffer.Data();
    const unsigned char* quarter = base + buffer.TotalBytes() / 4;
    const unsigned char* middle = base + buffer.TotalBytes() / 2;
    const unsigned char* lastPage = base + buffer.TotalBytes() - 4096;

    // Nothing has been stored yet, so not one page of the allocation may have
    // been touched - not zeroed, not filled, not "prepared".
    CHECK(!PageIsResident(probe, quarter));
    CHECK(!PageIsResident(probe, middle));
    CHECK(!PageIsResident(probe, lastPage));

    // The probe is calibrated against a page the process really does touch, so
    // the assertions above cannot pass because the probe is simply broken.
    unsigned char* writable = const_cast<unsigned char*>(middle);
    writable[0] = 0x5A;
    CHECK(PageIsResident(probe, middle));
    CHECK(!PageIsResident(probe, lastPage));

    // Clear() drops the frames without zeroing the storage either.
    buffer.Clear();
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
    CHECK_EQ(writable[0], 0x5A);
    CHECK(!PageIsResident(probe, quarter));
    CHECK(!PageIsResident(probe, lastPage));

    // A failed Init() releases the allocation instead of leaving it behind.
    CHECK(!buffer.Init(4096, 4096, static_cast<std::size_t>(1) << 30));
    CHECK(buffer.Data() == 0);
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 10: the writer emits a real 8-bpp BMP: file header, info header, gray
// palette and a top-down (negative height) image.
// ---------------------------------------------------------------------------
TEST_CASE(BmpWriterWritesGrayscaleHeaderAndPalette)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"bmp_basic"));
    const std::wstring path = dir.File(L"frame.bmp");

    // 4 x 2 image: a multiple of 4 bytes per row, so no padding is involved.
    const unsigned char pixels[8] = { 0, 1, 2, 3, 254, 255, 128, 64 };

    const BmpWriteResult result = BmpWriter::WriteGrayscale8(path, pixels, 4, 2, 4);
    REQUIRE(result.success);
    CHECK(result.error.empty());
    CHECK(test::FileExists(path));

    std::vector<unsigned char> bytes;
    REQUIRE(test::ReadFileBytes(path, bytes));
    REQUIRE_EQ(bytes.size(), static_cast<std::size_t>(kBmpPixelDataOffset + 8));

    // BITMAPFILEHEADER
    CHECK_EQ(bytes[0], 'B');
    CHECK_EQ(bytes[1], 'M');
    CHECK_EQ(test::GetU32LE(bytes, 2), 1086ULL);          // bfSize == file size
    CHECK_EQ(test::GetU16LE(bytes, 6), 0u);               // bfReserved1
    CHECK_EQ(test::GetU16LE(bytes, 8), 0u);               // bfReserved2
    CHECK_EQ(test::GetU32LE(bytes, 10), 1078ULL);         // bfOffBits

    // BITMAPINFOHEADER
    CHECK_EQ(test::GetU32LE(bytes, 14), 40ULL);           // biSize
    CHECK_EQ(test::GetI32LE(bytes, 18), 4);               // biWidth
    CHECK_EQ(test::GetI32LE(bytes, 22), -2);              // biHeight: top-down
    CHECK_EQ(test::GetU16LE(bytes, 26), 1u);              // biPlanes
    CHECK_EQ(test::GetU16LE(bytes, 28), 8u);              // biBitCount
    CHECK_EQ(test::GetU32LE(bytes, 30), 0ULL);            // biCompression == BI_RGB
    CHECK_EQ(test::GetU32LE(bytes, 34), 8ULL);            // biSizeImage
    CHECK_EQ(test::GetU32LE(bytes, 38), 0ULL);            // biXPelsPerMeter
    CHECK_EQ(test::GetU32LE(bytes, 42), 0ULL);            // biYPelsPerMeter
    CHECK_EQ(test::GetU32LE(bytes, 46), 0ULL);            // biClrUsed
    CHECK_EQ(test::GetU32LE(bytes, 50), 0ULL);            // biClrImportant

    // Grayscale palette: index i maps to (i, i, i, 0).
    for (unsigned int index = 0; index < 256; ++index)
    {
        const std::size_t offset = kBmpPaletteOffset + static_cast<std::size_t>(index) * 4;
        CHECK_EQ(bytes[offset], index);
        CHECK_EQ(bytes[offset + 1], index);
        CHECK_EQ(bytes[offset + 2], index);
        CHECK_EQ(bytes[offset + 3], 0u);
    }

    CHECK_EQ(result.fileSize, 1086ULL);
    CHECK_EQ(result.bytesWritten, 1086ULL);
}

// ---------------------------------------------------------------------------
// Test 11: rows are padded to a multiple of 4 bytes, the source pitch is
// honoured and the pixels land in the file top-down.
// ---------------------------------------------------------------------------
TEST_CASE(BmpWriterPadsRowsAndCopiesPixelsTopDown)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"bmp_padding"));
    const std::wstring path = dir.File(L"padded.bmp");

    // 5 pixels per row: 5 bytes are padded to 8 in the file. The source uses a
    // pitch of 7 bytes so its own padding must be skipped as well.
    unsigned char source[21];
    std::memset(source, 0xEE, sizeof(source));
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 5; ++column)
        {
            source[row * 7 + column] = static_cast<unsigned char>(40 + row * 10 + column);
        }
    }

    const BmpWriteResult result = BmpWriter::WriteGrayscale8(path, source, 5, 3, 7);
    REQUIRE(result.success);
    CHECK(result.error.empty());

    std::vector<unsigned char> bytes;
    REQUIRE(test::ReadFileBytes(path, bytes));
    REQUIRE_EQ(bytes.size(), static_cast<std::size_t>(1102));  // 1078 + 3 * 8

    CHECK_EQ(test::GetU32LE(bytes, 2), 1102ULL);   // bfSize == real file size
    CHECK_EQ(test::GetU32LE(bytes, 10), 1078ULL);  // bfOffBits
    CHECK_EQ(test::GetI32LE(bytes, 18), 5);        // biWidth
    CHECK_EQ(test::GetI32LE(bytes, 22), -3);       // biHeight: top-down
    CHECK_EQ(test::GetU32LE(bytes, 34), 24ULL);    // biSizeImage == 3 * 8
    CHECK_EQ(result.fileSize, 1102ULL);
    CHECK_EQ(result.bytesWritten, 1102ULL);

    for (int row = 0; row < 3; ++row)
    {
        const std::size_t rowOffset = kBmpPixelDataOffset + static_cast<std::size_t>(row) * 8;
        for (int column = 0; column < 5; ++column)
        {
            CHECK_EQ(bytes[rowOffset + static_cast<std::size_t>(column)],
                     source[row * 7 + column]);
        }

        // The 3 padding bytes of the row must be zero, never the 0xEE filler of
        // the padded source row.
        CHECK_EQ(bytes[rowOffset + 5], 0u);
        CHECK_EQ(bytes[rowOffset + 6], 0u);
        CHECK_EQ(bytes[rowOffset + 7], 0u);
    }
}

// ---------------------------------------------------------------------------
// Test 12: fold and file names outside the ANSI code page are written through
// the Unicode entry points.
// ---------------------------------------------------------------------------
TEST_CASE(BmpWriterAcceptsUnicodePath)
{
    test::TempDir dir;
    // u6d4bu8bd5 = CJK, u00e4/u00f6/u00fc = umlauts, u03a9 = Omega.
    REQUIRE(dir.Create(L"\u6d4b\u8bd5_\u00e4\u00f6\u00fc_\u03a9"));

    // u30d5u30ec... = Japanese "frame_1.bmp".
    const std::wstring path = dir.File(L"\u30d5\u30ec\u30fc\u30e0_1.bmp");

    const unsigned char pixels[4] = { 7, 8, 9, 10 };
    const BmpWriteResult result = BmpWriter::WriteGrayscale8(path, pixels, 2, 2, 2);
    REQUIRE(result.success);
    CHECK(result.error.empty());
    CHECK(test::FileExists(path));

    std::vector<unsigned char> bytes;
    REQUIRE(test::ReadFileBytes(path, bytes));
    // 2 pixels per row are padded to 4 bytes: 1078 + 2 rows * 4 bytes.
    REQUIRE_EQ(bytes.size(), static_cast<std::size_t>(kBmpPixelDataOffset + 8));
    CHECK_EQ(test::GetU32LE(bytes, 2), 1086ULL);            // bfSize for 2 x 2
    CHECK_EQ(test::GetU32LE(bytes, 34), 8ULL);              // biSizeImage
    CHECK_EQ(test::GetI32LE(bytes, 18), 2);                 // biWidth
    CHECK_EQ(test::GetI32LE(bytes, 22), -2);                // biHeight
    CHECK_EQ(bytes[kBmpPixelDataOffset], 7);
    CHECK_EQ(bytes[kBmpPixelDataOffset + 1], 8);
    CHECK_EQ(bytes[kBmpPixelDataOffset + 2], 0u);           // row padding
    CHECK_EQ(bytes[kBmpPixelDataOffset + 3], 0u);           // row padding
    CHECK_EQ(bytes[kBmpPixelDataOffset + 4], 9);
    CHECK_EQ(bytes[kBmpPixelDataOffset + 5], 10);
    CHECK_EQ(bytes[kBmpPixelDataOffset + 6], 0u);
    CHECK_EQ(bytes[kBmpPixelDataOffset + 7], 0u);
}

// ---------------------------------------------------------------------------
// Test 13: a path that cannot be opened for writing is reported as a failure
// with an error text, and the writer keeps working afterwards.
// ---------------------------------------------------------------------------
TEST_CASE(BmpWriterReportsUnwritablePath)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"bmp_unwritable"));

    const unsigned char pixels[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    // A file inside a folder that does not exist.
    const std::wstring missing = dir.File(L"no_such_folder\\frame.bmp");
    const BmpWriteResult result = BmpWriter::WriteGrayscale8(missing, pixels, 4, 2, 4);
    CHECK(!result.success);
    CHECK(!result.error.empty());
    CHECK(!test::FileExists(missing));
    CHECK_EQ(result.fileSize, 0ULL);
    CHECK_EQ(result.bytesWritten, 0ULL);

    // An existing directory cannot be opened as a file either.
    const BmpWriteResult onDirectory = BmpWriter::WriteGrayscale8(dir.Path(), pixels, 4, 2, 4);
    CHECK(!onDirectory.success);
    CHECK(!onDirectory.error.empty());

    // A failed write must leave the object usable: the next write succeeds.
    const std::wstring good = dir.File(L"good.bmp");
    const BmpWriteResult ok = BmpWriter::WriteGrayscale8(good, pixels, 4, 2, 4);
    REQUIRE(ok.success);
    CHECK(ok.error.empty());
    CHECK(test::FileExists(good));
    CHECK_EQ(ok.fileSize, 1086ULL);
}

// ---------------------------------------------------------------------------
// Test 14: invalid arguments (null source, non-positive dimensions, narrow
// pitch) and images whose DWORD size fields would overflow are rejected without
// creating a file.
// ---------------------------------------------------------------------------
TEST_CASE(BmpWriterRejectsInvalidArgumentsAndOversizedImage)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"bmp_invalid"));
    const std::wstring path = dir.File(L"invalid.bmp");

    unsigned char pixels[32];
    std::memset(pixels, 0x5A, sizeof(pixels));

    // A source pitch narrower than the width would read rows outside the frame.
    const BmpWriteResult narrowPitch = BmpWriter::WriteGrayscale8(path, pixels, 4, 2, 2);
    REQUIRE(!narrowPitch.success);
    CHECK(!narrowPitch.error.empty());
    CHECK_EQ(narrowPitch.fileSize, 0ULL);
    CHECK(!test::FileExists(path));

    const BmpWriteResult nullPitch = BmpWriter::WriteGrayscale8(path, pixels, 4, 2, 0);
    CHECK(!nullPitch.success);

    // Null pixel source.
    const BmpWriteResult nullPixels = BmpWriter::WriteGrayscale8(path, 0, 4, 2, 4);
    CHECK(!nullPixels.success);
    CHECK(!nullPixels.error.empty());

    // Non-positive dimensions.
    const BmpWriteResult zeroWidth = BmpWriter::WriteGrayscale8(path, pixels, 0, 2, 4);
    CHECK(!zeroWidth.success);
    const BmpWriteResult negativeWidth = BmpWriter::WriteGrayscale8(path, pixels, -4, 2, 4);
    CHECK(!negativeWidth.success);
    const BmpWriteResult zeroHeight = BmpWriter::WriteGrayscale8(path, pixels, 4, 0, 4);
    CHECK(!zeroHeight.success);
    const BmpWriteResult negativeHeight = BmpWriter::WriteGrayscale8(path, pixels, 4, -2, 4);
    CHECK(!negativeHeight.success);

    // bfSize and biSizeImage are DWORD fields: 65536 x 65536 pixels is 4 GiB of
    // pixel data, which must be rejected instead of being truncated.
    const BmpWriteResult justOverDword = BmpWriter::WriteGrayscale8(path, pixels, 65536, 65536, 65536);
    CHECK(!justOverDword.success);
    CHECK(!justOverDword.error.empty());

    const BmpWriteResult tallImage = BmpWriter::WriteGrayscale8(path, pixels, 4, 0x40000000, 4);
    CHECK(!tallImage.success);
    CHECK(!tallImage.error.empty());

    // None of the rejected calls may have created a file.
    CHECK(!test::FileExists(path));

    // Valid arguments still succeed afterwards.
    const BmpWriteResult valid = BmpWriter::WriteGrayscale8(path, pixels, 4, 2, 4);
    REQUIRE(valid.success);
    CHECK(valid.error.empty());
    CHECK(test::FileExists(path));
}

// ===========================================================================
// SaveWorker: asynchronous, thread-safe BMP saving on an immutable snapshot.
// ===========================================================================

namespace
{
// Deterministic per-frame pixel pattern: every byte of every frame differs, so
// a frame written in the wrong place (or read from mutated memory) cannot pass.
unsigned char PatternByte(std::size_t frameIndex, std::size_t byteIndex)
{
    return static_cast<unsigned char>(((frameIndex + 1) * 37 + byteIndex * 11) & 0xFF);
}

std::size_t FrameBytes(int width, int height)
{
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

std::vector<unsigned char> MakePixelData(int width, int height, std::size_t frameCount)
{
    const std::size_t bytesPerFrame = FrameBytes(width, height);
    std::vector<unsigned char> pixels(bytesPerFrame * frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        for (std::size_t index = 0; index < bytesPerFrame; ++index)
        {
            pixels[frame * bytesPerFrame + index] = PatternByte(frame, index);
        }
    }
    return pixels;
}

// Reads a BMP back into packed pixels, checking the header on the way so that
// a truncated or mislabelled file cannot be mistaken for a good one.
bool ReadBmpPixels(const std::wstring& path, int width, int height,
                   std::vector<unsigned char>& pixels)
{
    std::vector<unsigned char> bytes;
    if (!test::ReadFileBytes(path, bytes))
    {
        return false;
    }
    if (bytes.size() < 54)
    {
        return false;
    }
    if (bytes[0] != 'B' || bytes[1] != 'M')
    {
        return false;
    }
    if (test::GetU32LE(bytes, 14) != 40ULL)          // biSize
    {
        return false;
    }
    if (test::GetI32LE(bytes, 18) != width)          // biWidth
    {
        return false;
    }
    if (test::GetI32LE(bytes, 22) != -height)        // biHeight: top-down
    {
        return false;
    }
    if (test::GetU16LE(bytes, 28) != 8u)             // biBitCount
    {
        return false;
    }

    const std::size_t offset = static_cast<std::size_t>(test::GetU32LE(bytes, 10));
    const std::size_t rowBytes = static_cast<std::size_t>(((width + 3) / 4) * 4);
    if (bytes.size() != offset + rowBytes * static_cast<std::size_t>(height))
    {
        return false;
    }

    pixels.assign(FrameBytes(width, height), 0);
    for (int row = 0; row < height; ++row)
    {
        for (int column = 0; column < width; ++column)
        {
            pixels[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(column)] =
                bytes[offset + static_cast<std::size_t>(row) * rowBytes +
                      static_cast<std::size_t>(column)];
        }
    }
    return true;
}

// Exact size of the BMP BmpWriter produces for a given geometry: 14-byte
// BITMAPFILEHEADER, 40-byte BITMAPINFOHEADER, 256 palette entries of 4 bytes,
// then one row of (width + 3) / 4 * 4 bytes per image row.
std::size_t ExpectedBmpFileSize(int width, int height)
{
    const std::size_t rowBytes = (static_cast<std::size_t>(width) + 3) / 4 * 4;
    return static_cast<std::size_t>(kBmpPixelDataOffset) +
           rowBytes * static_cast<std::size_t>(height);
}

std::size_t CountMatchingFiles(const std::wstring& folder, const std::wstring& pattern)
{
    std::size_t count = 0;
    WIN32_FIND_DATAW data;
    HANDLE find = ::FindFirstFileW((folder + L"\\" + pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            ++count;
        }
    }
    while (::FindNextFileW(find, &data) != 0);
    ::FindClose(find);
    return count;
}

// Sink that blocks inside Write() until the test releases it, so a test can
// observe the job at a point where it is provably still writing. The wait is
// bounded (blockMs) purely as an anti-hang guard: correctness comes from
// Release(), never from the clock. The job is only ever slowed down, so an
// implementation that (wrongly) performed the work inside Start() would still
// be caught by the assertions rather than by a timeout.
const unsigned long kBlockTimeoutMs = 10000;

class BlockingSink : public SaveSink
{
public:
    explicit BlockingSink(unsigned long blockMs = kBlockTimeoutMs)
        : m_mutex(), m_enteredSignal(), m_releaseSignal(), m_entered(false),
          m_released(false), m_writes(0), m_workerThreadId(0), m_blockMs(blockMs)
    {
    }

    virtual BmpWriteResult Write(const std::wstring& path, const unsigned char* pixels,
                                 int width, int height, int pitch)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_writes;
            m_workerThreadId = static_cast<unsigned long>(::GetCurrentThreadId());
            m_entered = true;
            m_enteredSignal.notify_all();
            m_releaseSignal.wait_for(lock,
                std::chrono::milliseconds(m_blockMs),
                [this] { return m_released; });
        }
        return BmpWriter::WriteGrayscale8(path, pixels, width, height, pitch);
    }

    // True once the worker has reached Write() at least once.
    bool WaitUntilEntered(unsigned long timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_enteredSignal.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                        [this] { return m_entered; });
    }

    void Release()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_released = true;
        m_releaseSignal.notify_all();
    }

    bool Entered() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entered;
    }

    unsigned long WorkerThreadId() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_workerThreadId;
    }

private:
    BlockingSink(const BlockingSink&);
    BlockingSink& operator=(const BlockingSink&);

    mutable std::mutex m_mutex;
    std::condition_variable m_enteredSignal;
    std::condition_variable m_releaseSignal;
    bool m_entered;
    bool m_released;
    std::size_t m_writes;
    unsigned long m_workerThreadId;
    unsigned long m_blockMs;
};

// Releases a blocking sink when the test leaves its scope, so a failing
// REQUIRE can never leave the worker blocked forever.
class SinkReleaser
{
public:
    explicit SinkReleaser(BlockingSink* sink) : m_sink(sink) {}
    ~SinkReleaser() { if (m_sink != 0) { m_sink->Release(); } }

private:
    SinkReleaser(const SinkReleaser&);
    SinkReleaser& operator=(const SinkReleaser&);

    BlockingSink* m_sink;
};

// Sink that fails every "failEvery"-th call and otherwise writes a real BMP.
// "failEvery" is counted over all calls, which makes the failing frames
// independent of any scheduling.
class FailingSink : public SaveSink
{
public:
    explicit FailingSink(std::size_t failEvery) : m_failEvery(failEvery), m_calls(0) {}

    virtual BmpWriteResult Write(const std::wstring& path, const unsigned char* pixels,
                                 int width, int height, int pitch)
    {
        ++m_calls;
        if (m_failEvery != 0 && (m_calls % m_failEvery) == 0)
        {
            BmpWriteResult failure;
            failure.error = "FailingSink: deliberate failure";
            return failure;
        }
        return BmpWriter::WriteGrayscale8(path, pixels, width, height, pitch);
    }

    std::size_t Calls() const { return m_calls; }

private:
    FailingSink(const FailingSink&);
    FailingSink& operator=(const FailingSink&);

    std::size_t m_failEvery;
    std::size_t m_calls;
};

// Sink that writes real BMPs and then makes the "throwOnCall"-th Write() throw:
// a std::exception when "throwStd" is true, a plain int otherwise. This is the
// shape of a sink that fails in ways a return value cannot describe - a broken
// encoder, a std::bad_alloc while building a path, a library that throws - and
// none of it may escape the worker thread: an exception leaving Run() calls
// std::terminate() and takes the whole application down with it.
class ThrowingSink : public SaveSink
{
public:
    ThrowingSink(std::size_t throwOnCall, bool throwStd)
        : m_throwOnCall(throwOnCall), m_throwStd(throwStd), m_calls(0)
    {
    }

    virtual BmpWriteResult Write(const std::wstring& path, const unsigned char* pixels,
                                 int width, int height, int pitch)
    {
        ++m_calls;
        if (m_throwOnCall != 0 && m_calls == m_throwOnCall)
        {
            if (m_throwStd)
            {
                throw std::runtime_error("ThrowingSink: deliberate exception");
            }
            throw 42;
        }
        return BmpWriter::WriteGrayscale8(path, pixels, width, height, pitch);
    }

    std::size_t Calls() const { return m_calls; }

private:
    ThrowingSink(const ThrowingSink&);
    ThrowingSink& operator=(const ThrowingSink&);

    std::size_t m_throwOnCall;
    bool m_throwStd;
    std::size_t m_calls;
};

// Sink that records the paths it is asked to write and reports success without
// touching the file system, so a job with many frames can be observed without
// creating that many files.
class RecordingSink : public SaveSink
{
public:
    RecordingSink() : m_paths() {}

    virtual BmpWriteResult Write(const std::wstring& path, const unsigned char*,
                                 int, int, int)
    {
        BmpWriteResult result;
        result.success = true;
        m_paths.push_back(path);
        return result;
    }

    const std::vector<std::wstring>& Paths() const { return m_paths; }

private:
    RecordingSink(const RecordingSink&);
    RecordingSink& operator=(const RecordingSink&);

    std::vector<std::wstring> m_paths;
};

// Sink that reports a fixed, caller-chosen byte count for every successful
// write and optionally delays each write. It never touches the file system, so
// the byte accounting and the elapsed-time/throughput fields can be asserted
// exactly and independently of disk speed, while the delay makes the job's
// duration predictable.
class PacingSink : public SaveSink
{
public:
    PacingSink(unsigned long long bytesPerWrite, unsigned long delayMs)
        : m_bytesPerWrite(bytesPerWrite), m_delayMs(delayMs), m_calls(0)
    {
    }

    virtual BmpWriteResult Write(const std::wstring&, const unsigned char*,
                                 int, int, int)
    {
        if (m_delayMs != 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_delayMs));
        }
        ++m_calls;

        BmpWriteResult result;
        result.success = true;
        result.fileSize = m_bytesPerWrite;
        result.bytesWritten = m_bytesPerWrite;
        return result;
    }

    std::size_t Calls() const { return m_calls; }

private:
    PacingSink(const PacingSink&);
    PacingSink& operator=(const PacingSink&);

    unsigned long long m_bytesPerWrite;
    unsigned long m_delayMs;
    std::size_t m_calls;
};

// Bounded poll driven by the worker's own state (not by a fixed sleep), used
// where a test needs the job to have finished without calling Wait().
bool WaitUntilIdle(SaveWorker& worker, unsigned long timeoutMs)
{
    unsigned long waited = 0;
    while (worker.Progress().running && waited < timeoutMs)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++waited;
    }
    return !worker.Progress().running;
}

// Sink that records the address of the first pixel of every frame it is asked
// to write, plus a copy of the pixel bytes. Nothing is saved, so the addresses
// can be compared with the ones the caller handed in - a worker that copied the
// frames would report others - and the copies can be compared with the captured
// bytes afterwards. The copies have to be taken inside Write(), because the
// frames belong to the job and are released as soon as it ends.
class PointerRecordingSink : public SaveSink
{
public:
    PointerRecordingSink() : m_pointers(), m_frames() {}

    virtual BmpWriteResult Write(const std::wstring&, const unsigned char* pixels,
                                 int width, int height, int)
    {
        BmpWriteResult result;
        result.success = true;
        m_pointers.push_back(pixels);

        std::vector<unsigned char> frame;
        if (pixels != 0 && width > 0 && height > 0)
        {
            const std::size_t bytes = static_cast<std::size_t>(width) *
                                      static_cast<std::size_t>(height);
            frame.assign(pixels, pixels + bytes);
        }
        m_frames.push_back(frame);
        return result;
    }

    const std::vector<const unsigned char*>& Pointers() const { return m_pointers; }
    const std::vector<std::vector<unsigned char> >& Frames() const { return m_frames; }

private:
    PointerRecordingSink(const PointerRecordingSink&);
    PointerRecordingSink& operator=(const PointerRecordingSink&);

    std::vector<const unsigned char*> m_pointers;
    std::vector<std::vector<unsigned char> > m_frames;
};
} // namespace

// ---------------------------------------------------------------------------
// Test 15: a snapshot reports the geometry and the frames it was given, and
// rejects a byte count that does not match width * height * frameCount.
// ---------------------------------------------------------------------------
TEST_CASE(FrameSnapshotCopiesFramesAndValidatesGeometry)
{
    const std::vector<unsigned char> pixels = MakePixelData(4, 2, 3);
    FrameSnapshot snapshot(4, 2, 3, pixels);

    REQUIRE(snapshot.IsValid());
    CHECK(snapshot.LastError().empty());
    CHECK_EQ(snapshot.Width(), 4);
    CHECK_EQ(snapshot.Height(), 2);
    CHECK_EQ(snapshot.BytesPerFrame(), static_cast<std::size_t>(8));
    CHECK_EQ(snapshot.FrameCount(), static_cast<std::size_t>(3));
    CHECK(snapshot.Frame(3) == 0);

    for (std::size_t frame = 0; frame < 3; ++frame)
    {
        REQUIRE(snapshot.Frame(frame) != 0);
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(snapshot.Frame(frame)[index], pixels[frame * 8 + index]);
        }
    }

    // The empty snapshot is invalid and says so.
    const FrameSnapshot empty;
    CHECK(!empty.IsValid());
    CHECK(!empty.LastError().empty());
    CHECK(empty.Frame(0) == 0);
    CHECK(!empty.LastError().empty());

    // A pixel vector that is too short, too long, or paired with a
    // non-positive geometry must be refused with a reason.
    CHECK(!FrameSnapshot(4, 2, 3, std::vector<unsigned char>(47, 0)).IsValid());
    CHECK(!FrameSnapshot(4, 2, 3, std::vector<unsigned char>(49, 0)).IsValid());
    CHECK(!FrameSnapshot(4, 2, 3, std::vector<unsigned char>()).IsValid());
    CHECK(!FrameSnapshot(0, 2, 3, pixels).IsValid());
    CHECK(!FrameSnapshot(4, 0, 3, pixels).IsValid());
    CHECK(!FrameSnapshot(-4, 2, 3, pixels).IsValid());
    CHECK(!FrameSnapshot(4, -2, 3, pixels).IsValid());
    CHECK(!FrameSnapshot(4, 2, 0, std::vector<unsigned char>()).IsValid());

    const FrameSnapshot rejected(4, 2, 3, std::vector<unsigned char>(47, 0));
    CHECK(!rejected.LastError().empty());
    CHECK_EQ(rejected.FrameCount(), static_cast<std::size_t>(0));

    // A frame count whose frame storage would wrap std::size_t is refused as
    // well rather than being truncated into a small, allocatable number.
    const FrameSnapshot wrapping(65536, 65536, static_cast<std::size_t>(1) << 34,
                                 std::vector<unsigned char>());
    CHECK(!wrapping.IsValid());
    CHECK(!wrapping.LastError().empty());
    CHECK_EQ(wrapping.FrameCount(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 16: FrameSnapshot::Capture() copies the frames out of a live capture
// buffer, so the buffer can keep being written without disturbing the copy.
// ---------------------------------------------------------------------------
TEST_CASE(FrameSnapshotCaptureCopiesOutOfTheLiveBuffer)
{
    FrameBuffer buffer;
    REQUIRE(buffer.Init(4, 2, 3));

    const std::vector<unsigned char> captured = MakePixelData(4, 2, 3);
    for (std::size_t frame = 0; frame < 3; ++frame)
    {
        REQUIRE(buffer.Append(&captured[frame * 8], 4));
    }

    FrameSnapshot snapshot;
    REQUIRE(FrameSnapshot::Capture(buffer, 3, snapshot));
    CHECK(snapshot.IsValid());
    CHECK(snapshot.LastError().empty());
    CHECK_EQ(snapshot.Width(), 4);
    CHECK_EQ(snapshot.Height(), 2);
    CHECK_EQ(snapshot.BytesPerFrame(), static_cast<std::size_t>(8));
    CHECK_EQ(snapshot.FrameCount(), static_cast<std::size_t>(3));

    for (std::size_t frame = 0; frame < 3; ++frame)
    {
        REQUIRE(snapshot.Frame(frame) != 0);
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(snapshot.Frame(frame)[index], captured[frame * 8 + index]);
        }
    }

    // The capture side keeps recording: it rewrites the very storage the
    // snapshot was taken from...
    buffer.Clear();
    const std::vector<unsigned char> overwritten(8, 0xAB);
    REQUIRE(buffer.Append(&overwritten[0], 4));

    // ...and the snapshot still holds the frames it was given.
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_EQ(snapshot.Frame(0)[index], captured[index]);
    }

    // Rejected captures explain themselves and leave the request usable.
    FrameSnapshot nothing;
    CHECK(!FrameSnapshot::Capture(buffer, 2, nothing));
    CHECK(!nothing.IsValid());
    CHECK(!nothing.LastError().empty());

    FrameSnapshot uninitialisedBuffer;
    const FrameBuffer uninitialised;
    CHECK(!FrameSnapshot::Capture(uninitialised, 1, uninitialisedBuffer));
    CHECK(!uninitialisedBuffer.IsValid());
    CHECK(!uninitialisedBuffer.LastError().empty());

    FrameSnapshot zeroFrames;
    CHECK(!FrameSnapshot::Capture(buffer, 0, zeroFrames));
    CHECK(!zeroFrames.IsValid());
}

// ---------------------------------------------------------------------------
// Test 16b: TakeFrom() hands the capture buffer's whole allocation to the
// snapshot. The frames keep their address, no second allocation is made, and
// the buffer is left uninitialised and empty - it cannot append until Init().
// ---------------------------------------------------------------------------
TEST_CASE(FrameSnapshotTakeFromMovesTheBufferAllocationWithoutCopying)
{
    FrameBuffer buffer;
    REQUIRE(buffer.Init(4, 2, 3));

    const std::vector<unsigned char> captured = MakePixelData(4, 2, 3);
    for (std::size_t frame = 0; frame < 3; ++frame)
    {
        REQUIRE(buffer.Append(&captured[frame * 8], 4));
    }

    const unsigned char* storage = buffer.Data();
    REQUIRE(storage != 0);

    FrameSnapshot snapshot;
    REQUIRE(FrameSnapshot::TakeFrom(buffer, 3, snapshot));

    CHECK(snapshot.IsValid());
    CHECK(snapshot.LastError().empty());
    CHECK_EQ(snapshot.Width(), 4);
    CHECK_EQ(snapshot.Height(), 2);
    CHECK_EQ(snapshot.BytesPerFrame(), static_cast<std::size_t>(8));
    CHECK_EQ(snapshot.FrameCount(), static_cast<std::size_t>(3));
    CHECK(snapshot.Frame(3) == 0);

    // The snapshot holds the very allocation the buffer had: same base address,
    // so not one byte was copied and no second block was allocated.
    REQUIRE(snapshot.Frame(0) != 0);
    CHECK(snapshot.Frame(0) == storage);
    for (std::size_t frame = 0; frame < 3; ++frame)
    {
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(snapshot.Frame(frame)[index], captured[frame * 8 + index]);
        }
    }

    // The buffer gave everything away: it is uninitialised, holds nothing and
    // reports no error, and Append() is refused until Init() runs again.
    CHECK(!buffer.IsInitialized());
    CHECK(buffer.Data() == 0);
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.Capacity(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.BytesPerFrame(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(0));
    CHECK_EQ(buffer.Width(), 0);
    CHECK_EQ(buffer.Height(), 0);
    CHECK(buffer.Frame(0) == 0);
    CHECK(buffer.LastError().empty());

    const unsigned char extra[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(!buffer.Append(extra, 4));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
    CHECK(!buffer.LastError().empty());

    // The frames the snapshot took are untouched by all of that.
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_EQ(snapshot.Frame(1)[index], captured[8 + index]);
    }

    // Init() makes the buffer usable again, with a fresh allocation of its own.
    REQUIRE(buffer.Init(4, 2, 2));
    CHECK(buffer.IsInitialized());
    CHECK_EQ(buffer.Capacity(), static_cast<std::size_t>(2));
    REQUIRE(buffer.Append(extra, 4));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(1));
}

// ---------------------------------------------------------------------------
// Test 16c: TakeFrom() takes only the frames asked for, even though the whole
// allocation changes hands, and the first frames keep their address.
// ---------------------------------------------------------------------------
TEST_CASE(FrameSnapshotTakeFromTakesOnlyTheFramesRequested)
{
    FrameBuffer buffer;
    REQUIRE(buffer.Init(4, 2, 3));

    const std::vector<unsigned char> captured = MakePixelData(4, 2, 3);
    for (std::size_t frame = 0; frame < 3; ++frame)
    {
        REQUIRE(buffer.Append(&captured[frame * 8], 4));
    }

    const unsigned char* storage = buffer.Data();
    REQUIRE(storage != 0);

    FrameSnapshot snapshot;
    REQUIRE(FrameSnapshot::TakeFrom(buffer, 2, snapshot));

    CHECK(snapshot.IsValid());
    CHECK_EQ(snapshot.FrameCount(), static_cast<std::size_t>(2));
    CHECK(snapshot.Frame(2) == 0);

    // The frames asked for are the prefix of the block, starting where the
    // buffer's frames started: the snapshot owns more than it holds and still
    // shares the one allocation.
    REQUIRE(snapshot.Frame(0) != 0);
    CHECK(snapshot.Frame(0) == storage);
    CHECK(snapshot.Frame(1) == storage + 8);

    for (std::size_t frame = 0; frame < 2; ++frame)
    {
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(snapshot.Frame(frame)[index], captured[frame * 8 + index]);
        }
    }

    // The buffer gave up the whole allocation, not only the frames that were
    // taken out of it.
    CHECK(!buffer.IsInitialized());
    CHECK(buffer.Data() == 0);
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 16d: a refused TakeFrom() explains itself, leaves the caller's frames
// exactly where they are and stays usable for the request that does make sense.
// ---------------------------------------------------------------------------
TEST_CASE(FrameSnapshotTakeFromRejectsInvalidRequestsLeavingTheBufferUsable)
{
    FrameSnapshot snapshot;

    // A buffer that was never initialised has nothing to hand over.
    FrameBuffer uninitialised;
    CHECK(!FrameSnapshot::TakeFrom(uninitialised, 1, snapshot));
    CHECK(!snapshot.IsValid());
    CHECK(!snapshot.LastError().empty());
    CHECK(uninitialised.Data() == 0);
    CHECK(!uninitialised.IsInitialized());

    // Room for one more frame than is captured, so that "the buffer is still
    // usable" can be shown by really appending, not merely by reading state.
    FrameBuffer buffer;
    REQUIRE(buffer.Init(4, 2, 4));

    const std::vector<unsigned char> captured = MakePixelData(4, 2, 4);
    for (std::size_t frame = 0; frame < 3; ++frame)
    {
        REQUIRE(buffer.Append(&captured[frame * 8], 4));
    }

    const unsigned char* storage = buffer.Data();
    REQUIRE(storage != 0);

    // A request for no frames at all, and one for more frames than were
    // captured, are both refused.
    const std::size_t refusals = 2;
    for (std::size_t attempt = 0; attempt < refusals; ++attempt)
    {
        const std::size_t frameCount = (attempt == 0) ? 0 : 5;

        CHECK(!FrameSnapshot::TakeFrom(buffer, frameCount, snapshot));
        CHECK(!snapshot.IsValid());
        CHECK(!snapshot.LastError().empty());
        CHECK(snapshot.Frame(0) == 0);

        // The refused transfer must not have taken anything: same allocation,
        // same frame count, same bytes, and the buffer is not even in an error
        // state of its own.
        CHECK(buffer.IsInitialized());
        CHECK(buffer.Data() == storage);
        CHECK_EQ(buffer.Count(), static_cast<std::size_t>(3));
        CHECK_EQ(buffer.Capacity(), static_cast<std::size_t>(4));
        CHECK_EQ(buffer.TotalBytes(), static_cast<std::size_t>(32));
        CHECK(buffer.LastError().empty());
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(buffer.Frame(0)[index], captured[index]);
            CHECK_EQ(buffer.Frame(2)[index], captured[16 + index]);
        }
    }

    // The capture side carries on recording into the same allocation, which is
    // the strongest way to show that nothing was taken away from it.
    const unsigned char extra[8] = { 9, 10, 11, 12, 13, 14, 15, 16 };
    REQUIRE(buffer.Append(extra, 4));
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(4));
    CHECK(buffer.Data() == storage);
    CHECK(buffer.LastError().empty());

    // And the request it can satisfy still works afterwards, taking the frames
    // it was asked for out of the very same allocation.
    REQUIRE(FrameSnapshot::TakeFrom(buffer, 3, snapshot));
    CHECK(snapshot.IsValid());
    CHECK(snapshot.Frame(0) == storage);
    CHECK_EQ(snapshot.FrameCount(), static_cast<std::size_t>(3));
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_EQ(snapshot.Frame(2)[index], captured[16 + index]);
    }

    CHECK(!buffer.IsInitialized());
    CHECK(buffer.Data() == 0);
    CHECK_EQ(buffer.Count(), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 17: the default sink is a thin pass-through to BmpWriter, successes and
// failures included.
// ---------------------------------------------------------------------------
TEST_CASE(BmpFileSinkWritesThroughBmpWriter)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"bmp_file_sink"));

    BmpFileSink sink;
    const unsigned char pixels[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    const BmpWriteResult result = sink.Write(dir.File(L"direct.bmp"), pixels, 4, 2, 4);
    REQUIRE(result.success);
    CHECK(result.error.empty());
    CHECK(test::FileExists(dir.File(L"direct.bmp")));

    std::vector<unsigned char> saved;
    REQUIRE(ReadBmpPixels(dir.File(L"direct.bmp"), 4, 2, saved));
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_EQ(saved[index], pixels[index]);
    }

    const BmpWriteResult failed = sink.Write(dir.File(L"missing\\direct.bmp"),
                                             pixels, 4, 2, 4);
    CHECK(!failed.success);
    CHECK(!failed.error.empty());
}

// ---------------------------------------------------------------------------
// Test 18: Start() returns while the job is still running (the work happens on
// another thread), and progress is consistent before, during and after it.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerStartReturnsBeforeTheJobFinishes)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_nonblocking"));

    BlockingSink sink;
    const std::vector<unsigned char> pixels = MakePixelData(4, 2, 3);
    FrameSnapshot snapshot(4, 2, 3, pixels);
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    SinkReleaser releaser(&sink);

    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));

    // Start() has already returned while the worker is still inside its first
    // write: the job is running, nothing has completed and no file exists. An
    // implementation that did the work inside Start() could never get here with
    // the sink still blocked.
    const SaveProgress started = worker.Progress();
    CHECK(started.running);
    CHECK_EQ(started.total, static_cast<std::size_t>(3));
    CHECK_EQ(started.completed, static_cast<std::size_t>(0));
    CHECK_EQ(started.failed, static_cast<std::size_t>(0));
    CHECK(!started.cancelled);
    CHECK(started.lastError.empty());

    REQUIRE(sink.WaitUntilEntered(5000));
    CHECK(sink.WorkerThreadId() != static_cast<unsigned long>(::GetCurrentThreadId()));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(0));

    sink.Release();
    worker.Wait();

    const SaveProgress finished = worker.Progress();
    CHECK(!finished.running);
    CHECK_EQ(finished.total, static_cast<std::size_t>(3));
    CHECK_EQ(finished.completed, static_cast<std::size_t>(3));
    CHECK_EQ(finished.failed, static_cast<std::size_t>(0));
    CHECK(!finished.cancelled);
    CHECK(finished.lastError.empty());
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(3));
}

// ---------------------------------------------------------------------------
// Test 19: every frame ends up in its own verified BMP, in frame order, with
// deterministic names.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerWritesEveryFrameInOrder)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_in_order"));

    const int width = 4;
    const int height = 2;
    const std::size_t frameCount = 5;
    const std::vector<unsigned char> pixels = MakePixelData(width, height, frameCount);
    FrameSnapshot snapshot(width, height, frameCount, pixels);
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"frame"));

    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK(!progress.cancelled);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));
    CHECK(progress.lastError.empty());

    CHECK_EQ(CountMatchingFiles(dir.Path(), L"frame_*.bmp"), frameCount);

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const std::wstring path = dir.File(SaveWorker::FileNameForIndex(L"frame", frame, frameCount).c_str());
        REQUIRE(test::FileExists(path));

        std::vector<unsigned char> saved;
        REQUIRE(ReadBmpPixels(path, width, height, saved));
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(saved[index], pixels[frame * 8 + index]);
        }
    }

    // A second Wait() must be harmless.
    worker.Wait();
    CHECK(!worker.Progress().running);
}

// ---------------------------------------------------------------------------
// Test 20: a frame whose write fails is counted, the reason is kept, and the
// remaining frames are still written.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerCountsFailuresAndKeepsGoing)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_failures"));

    FailingSink sink(2);            // fails the 2nd, 4th, 6th write: frames 1, 3, 5
    const std::size_t frameCount = 6;
    const std::vector<unsigned char> pixels = MakePixelData(4, 2, frameCount);
    FrameSnapshot snapshot(4, 2, frameCount, pixels);
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK(!progress.cancelled);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(3));
    CHECK_EQ(progress.completed, static_cast<std::size_t>(3));
    CHECK(!progress.lastError.empty());

    // Every frame was attempted, failures included: the job never gave up.
    CHECK_EQ(sink.Calls(), frameCount);

    // The good frames are on disk, the failed ones are not...
    CHECK(test::FileExists(dir.File(L"f_00000.bmp")));
    CHECK(!test::FileExists(dir.File(L"f_00001.bmp")));
    CHECK(test::FileExists(dir.File(L"f_00002.bmp")));
    CHECK(!test::FileExists(dir.File(L"f_00003.bmp")));
    CHECK(test::FileExists(dir.File(L"f_00004.bmp")));
    CHECK(!test::FileExists(dir.File(L"f_00005.bmp")));

    // ...and the frames written after a failure are complete and correct.
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(3));
    std::vector<unsigned char> saved;
    REQUIRE(ReadBmpPixels(dir.File(L"f_00004.bmp"), 4, 2, saved));
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_EQ(saved[index], pixels[4 * 8 + index]);
    }
}

// ---------------------------------------------------------------------------
// Test 21: when every write fails (folder that does not exist) the job still
// runs to the end, counts every failure and reports the last reason.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerCountsEveryFrameWhenTheFolderIsMissing)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_missing_folder"));

    const std::wstring missing = dir.Path() + L"\\does_not_exist";
    const std::size_t frameCount = 3;
    FrameSnapshot snapshot(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, missing, L"f"));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK(!progress.cancelled);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, static_cast<std::size_t>(0));
    CHECK_EQ(progress.failed, frameCount);
    CHECK(!progress.lastError.empty());
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 22: cancelling between frames stops the job after the frame that was
// already in flight, and no further file is created.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerCancelStopsTheJobBetweenFrames)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_cancel"));

    BlockingSink sink;
    const std::size_t frameCount = 6;
    FrameSnapshot snapshot(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    SinkReleaser releaser(&sink);

    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));

    // The first frame is provably inside Write() right now: cancel there.
    REQUIRE(sink.WaitUntilEntered(5000));

    worker.Cancel();

    const SaveProgress during = worker.Progress();
    CHECK(during.cancelled);
    CHECK(during.running);

    // The in-flight frame is allowed to finish; the job stops after it.
    sink.Release();
    worker.Wait();

    const SaveProgress stopped = worker.Progress();
    CHECK(!stopped.running);
    CHECK(stopped.cancelled);
    CHECK_EQ(stopped.total, frameCount);
    CHECK_EQ(stopped.completed, static_cast<std::size_t>(1));
    CHECK_EQ(stopped.failed, static_cast<std::size_t>(0));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(1));

    // Cancelling an idle worker is harmless, and a later job is unaffected.
    worker.Cancel();
    CHECK(worker.Progress().cancelled);
    // A new job needs frames of its own: the worker owns the ones it
    // was already given.
    FrameSnapshot again(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(worker.Start(again, dir.Path(), L"g"));
    worker.Wait();
    const SaveProgress restarted = worker.Progress();
    CHECK(!restarted.cancelled);
    CHECK_EQ(restarted.completed, frameCount);
}

// ---------------------------------------------------------------------------
// Test 23: only one job runs at a time; a refused start leaves the running job
// alone.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerRejectsAConcurrentStart)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_concurrent"));

    BlockingSink sink;
    const std::size_t frameCount = 4;
    FrameSnapshot snapshot(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(snapshot.IsValid());

    FrameSnapshot second(4, 2, 2, MakePixelData(4, 2, 2));
    REQUIRE(second.IsValid());

    SaveWorker worker;
    SinkReleaser releaser(&sink);

    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
    REQUIRE(sink.WaitUntilEntered(5000));

    // The running job owns the worker: the second request is refused...
    CHECK(!worker.Start(second, dir.Path(), L"second", &sink));
    CHECK(!worker.Start(second, dir.Path(), L"second"));
    // ...and a refused request must not take the frames with it.
    CHECK(second.IsValid());
    CHECK(worker.Progress().running);
    CHECK_EQ(worker.Progress().total, frameCount);

    // ...and the refusal must not have disturbed it.
    sink.Release();
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), frameCount);
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"second_*.bmp"), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 24: a finished job can be started again, with and without an explicit
// Wait() in between.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerRestartsAfterThePreviousJobFinished)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_restart"));

    SaveWorker worker;

    FrameSnapshot first(4, 2, 2, MakePixelData(4, 2, 2));
    REQUIRE(first.IsValid());
    REQUIRE(worker.Start(first, dir.Path(), L"a"));
    worker.Wait();
    CHECK_EQ(worker.Progress().completed, static_cast<std::size_t>(2));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"a_*.bmp"), static_cast<std::size_t>(2));

    // Started again after a join, without tearing anything down.
    FrameSnapshot second(4, 2, 3, MakePixelData(4, 2, 3));
    REQUIRE(second.IsValid());
    REQUIRE(worker.Start(second, dir.Path(), L"b"));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK(!progress.cancelled);
    CHECK_EQ(progress.total, static_cast<std::size_t>(3));
    CHECK_EQ(progress.completed, static_cast<std::size_t>(3));
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));
    CHECK(progress.lastError.empty());
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"a_*.bmp"), static_cast<std::size_t>(2));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"b_*.bmp"), static_cast<std::size_t>(3));

    // And again while the previous thread is finished but not yet joined: the
    // new Start() takes it over.
    FrameSnapshot third(4, 2, 1, MakePixelData(4, 2, 1));
    REQUIRE(third.IsValid());
    REQUIRE(worker.Start(third, dir.Path(), L"c"));
    REQUIRE(WaitUntilIdle(worker, 10000));

    FrameSnapshot fourth(4, 2, 2, MakePixelData(4, 2, 2));
    REQUIRE(worker.Start(fourth, dir.Path(), L"d"));
    worker.Wait();

    CHECK_EQ(worker.Progress().completed, static_cast<std::size_t>(2));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"c_*.bmp"), static_cast<std::size_t>(1));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"d_*.bmp"), static_cast<std::size_t>(2));
}

// ---------------------------------------------------------------------------
// Test 25: destroying a worker whose job is still running cancels the job and
// joins the thread instead of leaking it or touching freed memory.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerDestructorCancelsAndJoinsTheRunningJob)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_destructor"));

    // This sink blocks every write for half a second. The destructor cancels
    // the job while the first write is in flight, so the job must stop long
    // before the last of the four frames.
    BlockingSink sink(500);
    const std::size_t frameCount = 4;
    FrameSnapshot snapshot(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(snapshot.IsValid());

    {
        SaveWorker worker;
        REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
        REQUIRE(sink.WaitUntilEntered(5000));
        CHECK(sink.Entered());
        // Leaving the scope destroys a worker with a running job.
    }

    CHECK(sink.Entered());
    const std::size_t written = CountMatchingFiles(dir.Path(), L"f_*.bmp");
    CHECK(written >= 1);
    CHECK(written < frameCount);      // the destructor stopped it
}

// ---------------------------------------------------------------------------
// Test 26: file names stay unique, ordered and deterministic past 9999 frames.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerNamesStayUniqueAndOrderedBeyond9999Frames)
{
    CHECK(SaveWorker::FileNameForIndex(L"frame", 0, 5) == L"frame_00000.bmp");
    CHECK(SaveWorker::FileNameForIndex(L"frame", 4, 5) == L"frame_00004.bmp");

    // Five digits are enough for 0..9999...
    CHECK(SaveWorker::FileNameForIndex(L"f", 0, 1) == L"f_00000.bmp");
    CHECK(SaveWorker::FileNameForIndex(L"f", 9999, 10000) == L"f_09999.bmp");

    // ...and a wider job simply widens the whole field, so the order of the
    // names still matches the order of the frames and no two names collide.
    CHECK(SaveWorker::FileNameForIndex(L"f", 0, 12000) == L"f_00000.bmp");
    CHECK(SaveWorker::FileNameForIndex(L"f", 9999, 12000) == L"f_09999.bmp");
    CHECK(SaveWorker::FileNameForIndex(L"f", 10000, 12000) == L"f_10000.bmp");
    CHECK(SaveWorker::FileNameForIndex(L"f", 11999, 12000) == L"f_11999.bmp");
    CHECK(SaveWorker::FileNameForIndex(L"f", 100000, 100001) == L"f_100000.bmp");
    CHECK(SaveWorker::FileNameForIndex(L"", 3, 4) == L"_00003.bmp");

    CHECK_EQ(SaveWorker::FileNameForIndex(L"f", 0, 12000).size(), static_cast<std::size_t>(11));

    // The worker itself must use those names for a job beyond 9999 frames.
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_many_frames"));

    RecordingSink sink;
    const std::size_t frameCount = 12000;
    FrameSnapshot snapshot(1, 1, frameCount, MakePixelData(1, 1, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));

    REQUIRE_EQ(sink.Paths().size(), frameCount);
    CHECK(sink.Paths()[0] == dir.Path() + L"\\f_00000.bmp");
    CHECK(sink.Paths()[9999] == dir.Path() + L"\\f_09999.bmp");
    CHECK(sink.Paths()[10000] == dir.Path() + L"\\f_10000.bmp");
    CHECK(sink.Paths()[11999] == dir.Path() + L"\\f_11999.bmp");

    std::size_t outOfOrder = 0;
    for (std::size_t index = 1; index < sink.Paths().size(); ++index)
    {
        if (!(sink.Paths()[index - 1] < sink.Paths()[index]))
        {
            ++outOfOrder;
        }
    }
    CHECK_EQ(outOfOrder, static_cast<std::size_t>(0));

    // The chosen names stay inside the folder and nothing was written to disk.
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 27: the job saves its own snapshot, so the caller may keep writing into
// the buffer the snapshot was taken from the moment Start() returns.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerSavesItsOwnSnapshotNotTheLiveInput)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_snapshot"));

    std::vector<unsigned char> live = MakePixelData(4, 2, 3);
    const std::vector<unsigned char> expected = live;
    FrameSnapshot snapshot(4, 2, 3, live);      // copied, not referenced
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f"));

    // Capture restarts immediately: every byte of the caller's buffer changes.
    for (std::size_t index = 0; index < live.size(); ++index)
    {
        live[index] = 0xFF;
    }

    worker.Wait();
    CHECK_EQ(worker.Progress().completed, static_cast<std::size_t>(3));

    for (std::size_t frame = 0; frame < 3; ++frame)
    {
        std::vector<unsigned char> saved;
        REQUIRE(ReadBmpPixels(dir.File(SaveWorker::FileNameForIndex(L"f", frame, 3).c_str()),
                              4, 2, saved));
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(saved[index], expected[frame * 8 + index]);
        }
    }
}

// ---------------------------------------------------------------------------
// Test 27b: Start() hands the frames over instead of copying them. The sink is
// asked to write the very frame addresses the caller's snapshot holds, so a
// worker that duplicated the frames would be caught here.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerStartMovesTheSnapshotWithoutCopying)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_transfer"));

    const std::size_t frameCount = 3;

    // The temporary really is taken over by the snapshot, so "expected" is the
    // only copy of the captured bytes left for the comparisons below.
    const std::vector<unsigned char> expected = MakePixelData(4, 2, frameCount);
    FrameSnapshot snapshot(4, 2, frameCount, std::vector<unsigned char>(expected));
    REQUIRE(snapshot.IsValid());

    // Recorded before the hand over, because a moved-from snapshot is empty.
    std::vector<const unsigned char*> handedOver;
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        REQUIRE(snapshot.Frame(frame) != 0);
        handedOver.push_back(snapshot.Frame(frame));
    }

    PointerRecordingSink sink;
    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));

    REQUIRE_EQ(sink.Pointers().size(), frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        CHECK(sink.Pointers()[frame] == handedOver[frame]);
    }

    // The bytes the sink was given are the captured bytes, not something the
    // worker produced on the way.
    REQUIRE_EQ(sink.Frames().size(), frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        REQUIRE_EQ(sink.Frames()[frame].size(), static_cast<std::size_t>(8));
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(sink.Frames()[frame][index], expected[frame * 8 + index]);
        }
    }

    // The worker owns the frames now, so the caller's snapshot must not still
    // point at them: an empty, invalid snapshot is the only safe state.
    CHECK(!snapshot.IsValid());
    CHECK(snapshot.Frame(0) == 0);
}

// ---------------------------------------------------------------------------
// Test 27c: the production path end to end - a capture buffer hands its
// allocation to the worker, and the frames on disk are the captured bytes.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerSavesATakenOverCaptureBuffer)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_capture_handover"));

    const std::size_t frameCount = 3;
    const std::vector<unsigned char> captured = MakePixelData(4, 2, frameCount);

    FrameBuffer buffer;
    REQUIRE(buffer.Init(4, 2, frameCount));
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        REQUIRE(buffer.Append(&captured[frame * 8], 4));
    }

    FrameSnapshot snapshot;
    REQUIRE(FrameSnapshot::TakeFrom(buffer, frameCount, snapshot));
    REQUIRE(snapshot.IsValid());

    // Capture owns nothing now, so the next recording may start at once; the
    // frames in flight belong to the worker.
    CHECK(!buffer.IsInitialized());
    CHECK(buffer.Data() == 0);

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f"));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK(!progress.cancelled);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));
    CHECK(progress.lastError.empty());

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        std::vector<unsigned char> saved;
        REQUIRE(ReadBmpPixels(dir.File(SaveWorker::FileNameForIndex(L"f", frame, frameCount).c_str()),
                              4, 2, saved));
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(saved[index], captured[frame * 8 + index]);
        }
    }
}

// ---------------------------------------------------------------------------
// Test 27d: the worker is handed the capture buffer's own address, not a copy
// of it. Capture buffer -> snapshot -> worker -> sink, one block of memory.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerPassesTheCaptureAllocationStraightToTheSink)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_capture_address"));

    const std::size_t frameCount = 4;
    const std::size_t frameBytes = 8;
    const std::vector<unsigned char> captured = MakePixelData(4, 2, frameCount);

    FrameBuffer buffer;
    REQUIRE(buffer.Init(4, 2, frameCount));
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        REQUIRE(buffer.Append(&captured[frame * 8], 4));
    }

    const unsigned char* storage = buffer.Data();
    REQUIRE(storage != 0);

    FrameSnapshot snapshot;
    REQUIRE(FrameSnapshot::TakeFrom(buffer, frameCount, snapshot));
    REQUIRE(snapshot.Frame(0) == storage);

    PointerRecordingSink sink;
    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
    worker.Wait();

    CHECK_EQ(worker.Progress().completed, frameCount);
    REQUIRE_EQ(sink.Pointers().size(), frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        CHECK(sink.Pointers()[frame] == storage + frame * frameBytes);
    }

    // The frames the sink was handed are the captured bytes. They have to be
    // compared through the sink's copies: the worker owns the frames and hands
    // the allocation back the moment the job ends, so the addresses above must
    // not be read afterwards (they are only compared, never dereferenced).
    REQUIRE_EQ(sink.Frames().size(), frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        REQUIRE_EQ(sink.Frames()[frame].size(), frameBytes);
        for (std::size_t index = 0; index < frameBytes; ++index)
        {
            CHECK_EQ(sink.Frames()[frame][index], captured[frame * frameBytes + index]);
        }
    }
}

// ---------------------------------------------------------------------------
// Test 28: an unusable request is refused without starting a job, and a worker
// that was never started can still be waited on and destroyed.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerRejectsInvalidRequests)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_invalid"));

    SaveWorker worker;

    FrameSnapshot broken(4, 2, 3, std::vector<unsigned char>(7, 0));
    CHECK(!broken.IsValid());
    CHECK(!worker.Start(broken, dir.Path(), L"f"));
    CHECK(!worker.Progress().running);
    CHECK(!worker.Progress().lastError.empty());

    FrameSnapshot good(4, 2, 2, MakePixelData(4, 2, 2));
    REQUIRE(good.IsValid());

    // An empty output folder cannot be written into.
    CHECK(!worker.Start(good, L"", L"f"));
    // A refused request must not take the frames with it either.
    CHECK(good.IsValid());
    CHECK(!worker.Progress().running);
    CHECK(!worker.Progress().lastError.empty());

    // Waiting on, and destroying, a worker that never ran is safe.
    worker.Wait();
    worker.Cancel();
    CHECK(!worker.Progress().running);
    CHECK_EQ(worker.Progress().completed, static_cast<std::size_t>(0));
    CHECK_EQ(worker.Progress().total, static_cast<std::size_t>(0));

    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 29: a folder and a prefix outside the ANSI code page are written
// through the Unicode entry points.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerWritesIntoAUnicodeFolder)
{
    test::TempDir dir;
    // u6d4bu8bd5 = CJK, u00e4/u00f6/u00fc = umlauts, u03a9 = Omega.
    REQUIRE(dir.Create(L"\u6d4b\u8bd5_\u00e4\u00f6\u00fc_\u03a9"));

    const std::size_t frameCount = 2;
    const std::vector<unsigned char> pixels = MakePixelData(4, 2, frameCount);
    FrameSnapshot snapshot(4, 2, frameCount, pixels);
    REQUIRE(snapshot.IsValid());

    // u30d5u30ec... = Japanese "frame".
    const std::wstring prefix = L"\u30d5\u30ec\u30fc\u30e0";

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), prefix));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));
    CHECK(progress.lastError.empty());

    const std::wstring first = dir.File((prefix + L"_00000.bmp").c_str());
    const std::wstring second = dir.File((prefix + L"_00001.bmp").c_str());
    REQUIRE(test::FileExists(first));
    REQUIRE(test::FileExists(second));

    std::vector<unsigned char> saved;
    REQUIRE(ReadBmpPixels(first, 4, 2, saved));
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_EQ(saved[index], pixels[index]);
    }
    REQUIRE(ReadBmpPixels(second, 4, 2, saved));
    for (std::size_t index = 0; index < 8; ++index)
    {
        CHECK_EQ(saved[index], pixels[8 + index]);
    }
}

// ---------------------------------------------------------------------------
// Test 30: a Start() whose worker thread cannot be created is refused without
// swallowing the capture. The frames stay the caller's - same object, same
// pointers, still valid - so the caller can retry the hand over, which is what
// keeps a zero-copy capture from being lost silently.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerThreadCreationFailureKeepsTheFramesWithTheCaller)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_thread_failure"));

    const std::size_t frameCount = 3;
    const std::vector<unsigned char> expected = MakePixelData(4, 2, frameCount);

    FrameSnapshot snapshot(4, 2, frameCount, std::vector<unsigned char>(expected));
    REQUIRE(snapshot.IsValid());

    // Recorded before the attempt: after a *successful* hand over the caller's
    // snapshot is empty, so the only way to prove the frames were never moved
    // away is to compare the very pointers the caller held.
    std::vector<const unsigned char*> before;
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        REQUIRE(snapshot.Frame(frame) != 0);
        before.push_back(snapshot.Frame(frame));
    }

    SaveWorker worker;
    worker.FailNextThreadStartForTest();

    CHECK(!worker.Start(snapshot, dir.Path(), L"f"));

    // The refused Start() handed the frames straight back: the caller's
    // snapshot is still valid, still holds every frame, and still owns the
    // exact same pointers (nothing was copied, nothing was lost).
    CHECK(snapshot.IsValid());
    CHECK_EQ(snapshot.FrameCount(), frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        CHECK(snapshot.Frame(frame) == before[frame]);
    }

    // The refusal is visible through Progress() and no job is running, so the
    // worker stays usable.
    const SaveProgress rejected = worker.Progress();
    CHECK(!rejected.running);
    CHECK_EQ(rejected.total, static_cast<std::size_t>(0));
    CHECK(!rejected.lastError.empty());
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(0));

    // The one-shot seam is over: the very same snapshot is accepted now and its
    // bytes really are the captured ones.
    CHECK(worker.Start(snapshot, dir.Path(), L"f"));
    CHECK(!snapshot.IsValid());      // moved out by the successful hand over
    worker.Wait();

    const SaveProgress saved = worker.Progress();
    CHECK(!saved.running);
    CHECK_EQ(saved.completed, frameCount);
    CHECK_EQ(saved.failed, static_cast<std::size_t>(0));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), frameCount);

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        std::vector<unsigned char> bytes;
        REQUIRE(ReadBmpPixels(dir.File(SaveWorker::FileNameForIndex(L"f", frame, frameCount).c_str()),
                              4, 2, bytes));
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(bytes[index], expected[frame * 8 + index]);
        }
    }
}

// ---------------------------------------------------------------------------
// Test 31: ReapFinished() joins a finished job and releases its frames, but
// never waits for one that is still running - the non-blocking replacement for
// the check-then-Wait() pattern.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerReapFinishedNeverWaitsForARunningJob)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_reap"));

    SaveWorker worker;

    // Nothing has ever run: reaping is a no-op, not a wait.
    CHECK(!worker.ReapFinished());
    CHECK(!worker.Progress().running);

    // A job parked inside its first write. ReapFinished() has to return at once
    // and leave it alone; a blocking implementation would hang here until the
    // sink is released, which the test would never do.
    BlockingSink sink;
    SinkReleaser releaser(&sink);
    const std::size_t frameCount = 4;
    FrameSnapshot snapshot(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(snapshot.IsValid());
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
    REQUIRE(sink.WaitUntilEntered(5000));

    CHECK(!worker.ReapFinished());
    CHECK(worker.Progress().running);
    CHECK_EQ(worker.Progress().total, frameCount);

    // Let the job finish without joining it: the thread is still joinable, so
    // the next ReapFinished() is the one that must reap it.
    sink.Release();
    REQUIRE(WaitUntilIdle(worker, 10000));

    // Reaped exactly once...
    CHECK(worker.ReapFinished());
    // ...and nothing is left to reap the second time.
    CHECK(!worker.ReapFinished());

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), frameCount);

    // The worker is still usable after being reaped.
    FrameSnapshot second(4, 2, 2, MakePixelData(4, 2, 2));
    REQUIRE(second.IsValid());
    REQUIRE(worker.Start(second, dir.Path(), L"g"));
    worker.Wait();
    CHECK_EQ(worker.Progress().completed, static_cast<std::size_t>(2));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"g_*.bmp"), static_cast<std::size_t>(2));
}

// ---------------------------------------------------------------------------
// Test 32: an exception thrown by the sink is caught at the worker boundary.
// The frames written before it are kept, the frame it was thrown for counts as
// failed, the job ends, every waiter is woken, and the worker can be reaped and
// started again. Without the boundary catch the exception would leave Run() and
// call std::terminate(), killing the process with the capture unsaved.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerContainsAnExceptionThrownByTheSink)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_sink_exception"));

    const std::size_t frameCount = 5;
    const std::vector<unsigned char> expected = MakePixelData(4, 2, frameCount);

    // Calls 1 and 2 write real files; call 3 throws a std::exception.
    ThrowingSink sink(3, true);

    FrameSnapshot snapshot(4, 2, frameCount, std::vector<unsigned char>(expected));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));

    // Returning from Wait() at all proves the exception was contained: had it
    // escaped Run() the process would have been terminated by the failed thread
    // and this test would never report anything.
    worker.Wait();

    // Two frames were written, the throwing frame is counted as the one failure
    // that ended the job, and the count stays inside the job's own total.
    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, static_cast<std::size_t>(2));
    CHECK_EQ(progress.failed, static_cast<std::size_t>(1));
    CHECK(progress.completed + progress.failed <= progress.total);

    // The recorded reason names the failure and where it happened, so the UI
    // can show something better than "the save failed".
    CHECK(!progress.lastError.empty());
    CHECK(progress.lastError.find("ThrowingSink: deliberate exception") != std::string::npos);
    CHECK(progress.lastError.find("frame 2 of 5") != std::string::npos);

    // The two frames written before the throw are complete, correct files, and
    // the frame that threw left no file behind.
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(2));
    for (std::size_t frame = 0; frame < 2; ++frame)
    {
        std::vector<unsigned char> bytes;
        REQUIRE(ReadBmpPixels(dir.File(SaveWorker::FileNameForIndex(L"f", frame, frameCount).c_str()),
                              4, 2, bytes));
        for (std::size_t index = 0; index < 8; ++index)
        {
            CHECK_EQ(bytes[index], expected[frame * 8 + index]);
        }
    }
    CHECK(!test::FileExists(dir.File(SaveWorker::FileNameForIndex(L"f", 2, frameCount).c_str())));

    // The finished thread is reapable: Wait() has already joined it, so this
    // call has nothing left to do...
    CHECK(!worker.ReapFinished());

    // ...and a job that is aborted while nobody waits for it is reaped by the
    // next ReapFinished(), which is what releases the frames of a failed save.
    ThrowingSink second(2, true);
    FrameSnapshot third(4, 2, 3, MakePixelData(4, 2, 3));
    REQUIRE(third.IsValid());
    REQUIRE(worker.Start(third, dir.Path(), L"k", &second));
    REQUIRE(WaitUntilIdle(worker, 10000));
    CHECK(worker.ReapFinished());
    CHECK(!worker.ReapFinished());

    // A second job then runs normally: the failure is not sticky.
    FrameSnapshot next(4, 2, 2, MakePixelData(4, 2, 2));
    REQUIRE(next.IsValid());
    REQUIRE(worker.Start(next, dir.Path(), L"g"));

    worker.Wait();
    const SaveProgress restarted = worker.Progress();
    CHECK(!restarted.running);
    CHECK_EQ(restarted.completed, static_cast<std::size_t>(2));
    CHECK_EQ(restarted.failed, static_cast<std::size_t>(0));
    CHECK(restarted.lastError.empty());     // a fresh job clears the old reason
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"g_*.bmp"), static_cast<std::size_t>(2));
}

// ---------------------------------------------------------------------------
// Test 33: the same containment for an exception that is not a std::exception
// (no what() to quote), on the very first frame: nothing is counted as saved,
// the reason still says which frame died, and the worker stays usable.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerContainsAnUnknownExceptionThrownByTheSink)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_unknown_exception"));

    const std::size_t frameCount = 2;

    // Throws a plain int on call 1, i.e. before any file has been written.
    ThrowingSink sink(1, false);

    FrameSnapshot snapshot(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));

    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.total, frameCount);
    CHECK_EQ(progress.completed, static_cast<std::size_t>(0));
    CHECK_EQ(progress.failed, static_cast<std::size_t>(1));
    CHECK(!progress.lastError.empty());
    CHECK(progress.lastError.find("frame 0 of 2") != std::string::npos);
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(0));

    // Wait() already joined the aborted job, so there is nothing left to reap;
    // the default sink of a fresh job still writes every frame.
    CHECK(!worker.ReapFinished());

    FrameSnapshot second(4, 2, 2, MakePixelData(4, 2, 2));
    REQUIRE(second.IsValid());
    REQUIRE(worker.Start(second, dir.Path(), L"h"));
    worker.Wait();

    const SaveProgress restarted = worker.Progress();
    CHECK(!restarted.running);
    CHECK_EQ(restarted.completed, static_cast<std::size_t>(2));
    CHECK_EQ(restarted.failed, static_cast<std::size_t>(0));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"h_*.bmp"), static_cast<std::size_t>(2));
}

// ---------------------------------------------------------------------------
// Test 34: the byte counter reports the bytes that really reached the files:
// for the default sink that is the exact size of every BMP, checked against
// the files themselves. A finished job also reports a positive duration and a
// throughput that is its bytes divided by that duration.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerReportsBytesWrittenFromActualFileSizes)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_byte_count"));

    // 5 x 3: the width is not a multiple of 4, so the file rows carry padding
    // that a "width * height" counter would get wrong.
    const int width = 5;
    const int height = 3;
    const std::size_t frameCount = 4;
    const std::size_t fileBytes = ExpectedBmpFileSize(width, height);

    FrameSnapshot snapshot(width, height, frameCount,
                           MakePixelData(width, height, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f"));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK(!progress.cancelled);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.failed, static_cast<std::size_t>(0));

    // The counter equals the size the BMP writer declares and writes...
    CHECK_EQ(progress.bytesWritten,
             static_cast<unsigned long long>(fileBytes) * frameCount);

    // ...and that is what is actually on disk, frame by frame.
    unsigned long long onDiskBytes = 0;
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const std::wstring path =
            dir.File(SaveWorker::FileNameForIndex(L"f", frame, frameCount).c_str());
        std::vector<unsigned char> bytes;
        REQUIRE(test::ReadFileBytes(path, bytes));
        CHECK_EQ(bytes.size(), fileBytes);
        onDiskBytes += static_cast<unsigned long long>(bytes.size());
    }
    CHECK_EQ(progress.bytesWritten, onDiskBytes);

    // A real job took a measurable amount of time, so its throughput is the
    // bytes it wrote divided by that time - never zero while bytes are counted.
    CHECK(progress.elapsedSeconds > 0.0);
    CHECK(progress.bytesPerSecond > 0.0);
    const double measured = static_cast<double>(progress.bytesWritten) /
                            progress.elapsedSeconds;
    CHECK(progress.bytesPerSecond >= measured * 0.99);
    CHECK(progress.bytesPerSecond <= measured * 1.01);
}

// ---------------------------------------------------------------------------
// Test 35: only successful frames contribute bytes. The failed frames of a
// partially failing job are counted as failures and add nothing, and the
// reported total still matches the files that were left on disk.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerDoesNotCountBytesOfFailedWrites)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_failed_bytes"));

    FailingSink sink(2);            // fails every second call: frames 1, 3, 5
    const int width = 4;
    const int height = 2;
    const std::size_t frameCount = 6;
    const std::size_t fileBytes = ExpectedBmpFileSize(width, height);

    FrameSnapshot snapshot(width, height, frameCount,
                           MakePixelData(width, height, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK_EQ(progress.completed, static_cast<std::size_t>(3));
    CHECK_EQ(progress.failed, static_cast<std::size_t>(3));
    CHECK_EQ(progress.bytesWritten,
             static_cast<unsigned long long>(fileBytes) * 3);
    CHECK(progress.bytesWritten <
          static_cast<unsigned long long>(fileBytes) * frameCount);

    // The counter matches the three files that exist and ignores the three that
    // were never created.
    unsigned long long onDiskBytes = 0;
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const std::wstring path =
            dir.File(SaveWorker::FileNameForIndex(L"f", frame, frameCount).c_str());
        std::vector<unsigned char> bytes;
        if (test::ReadFileBytes(path, bytes))
        {
            onDiskBytes += static_cast<unsigned long long>(bytes.size());
        }
    }
    CHECK_EQ(progress.bytesWritten, onDiskBytes);
    CHECK_EQ(onDiskBytes, static_cast<unsigned long long>(fileBytes) * 3);

    CHECK(progress.elapsedSeconds > 0.0);
    CHECK(progress.bytesPerSecond > 0.0);
}

// ---------------------------------------------------------------------------
// Test 36: a job whose every write fails reports no bytes at all and therefore
// no throughput, however long it was busy.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerReportsNoBytesWhenEveryWriteFails)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_no_bytes"));

    const std::wstring missing = dir.Path() + L"\\does_not_exist";
    const std::size_t frameCount = 3;

    FrameSnapshot snapshot(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, missing, L"f"));
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.completed, static_cast<std::size_t>(0));
    CHECK_EQ(progress.failed, frameCount);
    CHECK_EQ(progress.bytesWritten, 0ULL);

    // No bytes were written, so no throughput can be reported - even though the
    // job did take time.
    CHECK(progress.bytesPerSecond == 0.0);
    CHECK(progress.elapsedSeconds >= 0.0);
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(0));
}

// ---------------------------------------------------------------------------
// Test 37: cancelling stops the byte count at the frame that was in flight: the
// frames after the cancel add nothing to the byte total, and no metric is
// invented for them.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerAccountsBytesAndTimeUpToTheCancelPoint)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_cancel_bytes"));

    BlockingSink sink;
    const int width = 4;
    const int height = 2;
    const std::size_t frameCount = 6;
    const std::size_t fileBytes = ExpectedBmpFileSize(width, height);

    FrameSnapshot snapshot(width, height, frameCount,
                           MakePixelData(width, height, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    SinkReleaser releaser(&sink);

    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));
    REQUIRE(sink.WaitUntilEntered(5000));

    // The first frame is provably inside Write(): nothing has been written yet.
    const SaveProgress inFlight = worker.Progress();
    CHECK(inFlight.running);
    CHECK_EQ(inFlight.completed, static_cast<std::size_t>(0));
    CHECK_EQ(inFlight.bytesWritten, 0ULL);
    CHECK(inFlight.bytesPerSecond >= 0.0);

    worker.Cancel();
    sink.Release();
    worker.Wait();

    const SaveProgress stopped = worker.Progress();
    CHECK(!stopped.running);
    CHECK(stopped.cancelled);
    CHECK_EQ(stopped.completed, static_cast<std::size_t>(1));
    CHECK_EQ(stopped.failed, static_cast<std::size_t>(0));

    // Exactly the one frame that finished is counted, in bytes too.
    CHECK_EQ(stopped.bytesWritten, static_cast<unsigned long long>(fileBytes));
    CHECK_EQ(CountMatchingFiles(dir.Path(), L"f_*.bmp"), static_cast<std::size_t>(1));
    CHECK(stopped.elapsedSeconds > 0.0);
    CHECK(stopped.bytesPerSecond > 0.0);
}

// ---------------------------------------------------------------------------
// Test 38: while a job runs its elapsed clock is already moving, and once it
// has finished the duration covers the whole job - the throughput reported for
// a paced sink reflects that duration, not an instant.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerReportsElapsedTimeAndThroughputWhileSaving)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_pacing"));

    // 3 writes of 40 ms each: a job that cannot finish in less than ~120 ms.
    const unsigned long long bytesPerFrame = 1000ULL;
    PacingSink sink(bytesPerFrame, 40);
    const std::size_t frameCount = 3;

    FrameSnapshot snapshot(4, 2, frameCount, MakePixelData(4, 2, frameCount));
    REQUIRE(snapshot.IsValid());

    SaveWorker worker;
    REQUIRE(worker.Start(snapshot, dir.Path(), L"f", &sink));

    // Observed while the job is running: the elapsed clock has started and the
    // job is still incomplete.
    bool sawRunningClock = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const SaveProgress running = worker.Progress();
        if (running.running && running.elapsedSeconds > 0.0)
        {
            sawRunningClock = true;
            CHECK(running.completed < frameCount);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(sawRunningClock);

    worker.Wait();

    const SaveProgress progress = worker.Progress();
    CHECK(!progress.running);
    CHECK_EQ(progress.completed, frameCount);
    CHECK_EQ(progress.bytesWritten, bytesPerFrame * frameCount);

    // The duration covers the three paced writes, not a rounding error.
    CHECK(progress.elapsedSeconds >= 0.10);
    CHECK(progress.elapsedSeconds < 5.0);

    const double measured = static_cast<double>(progress.bytesWritten) /
                            progress.elapsedSeconds;
    CHECK(progress.bytesPerSecond >= measured * 0.99);
    CHECK(progress.bytesPerSecond <= measured * 1.01);
    CHECK_EQ(sink.Calls(), frameCount);
}

// ---------------------------------------------------------------------------
// Test 39: a refused Start() never invents progress. Every refusal (invalid
// snapshot, empty folder, thread creation failure) leaves the metrics at zero,
// and a refusal after a finished job leaves that job's metrics untouched.
// ---------------------------------------------------------------------------
TEST_CASE(SaveWorkerReportsNoMetricsForRefusedStarts)
{
    test::TempDir dir;
    REQUIRE(dir.Create(L"save_worker_refused_metrics"));

    SaveWorker worker;

    // (1) An invalid snapshot.
    FrameSnapshot empty;
    CHECK(!worker.Start(empty, dir.Path(), L"f"));
    SaveProgress refused = worker.Progress();
    CHECK(!refused.running);
    CHECK_EQ(refused.bytesWritten, 0ULL);
    CHECK(refused.elapsedSeconds == 0.0);
    CHECK(refused.bytesPerSecond == 0.0);

    // (2) An empty output folder.
    FrameSnapshot good(4, 2, 2, MakePixelData(4, 2, 2));
    REQUIRE(good.IsValid());
    CHECK(!worker.Start(good, L"", L"f"));
    refused = worker.Progress();
    CHECK(!refused.running);
    CHECK_EQ(refused.bytesWritten, 0ULL);
    CHECK(refused.elapsedSeconds == 0.0);
    CHECK(refused.bytesPerSecond == 0.0);
    CHECK(good.IsValid());              // the refusal kept the frames

    // (3) A thread that cannot be created: no metric moves, the frames stay.
    worker.FailNextThreadStartForTest();
    CHECK(!worker.Start(good, dir.Path(), L"f"));
    refused = worker.Progress();
    CHECK(!refused.running);
    CHECK(!refused.lastError.empty());
    CHECK_EQ(refused.bytesWritten, 0ULL);
    CHECK(refused.elapsedSeconds == 0.0);
    CHECK(refused.bytesPerSecond == 0.0);
    CHECK(good.IsValid());

    // A real job fills the metrics in...
    REQUIRE(worker.Start(good, dir.Path(), L"f"));
    worker.Wait();
    const SaveProgress finished = worker.Progress();
    CHECK(finished.bytesWritten > 0ULL);
    CHECK(finished.elapsedSeconds > 0.0);
    CHECK(finished.bytesPerSecond > 0.0);

    // ...and a later refusal leaves them exactly as the finished job left them.
    FrameSnapshot invalid;
    CHECK(!worker.Start(invalid, dir.Path(), L"g"));
    const SaveProgress afterRefusal = worker.Progress();
    CHECK(!afterRefusal.running);
    CHECK_EQ(afterRefusal.bytesWritten, finished.bytesWritten);
    CHECK(afterRefusal.elapsedSeconds == finished.elapsedSeconds);
    CHECK(afterRefusal.bytesPerSecond == finished.bytesPerSecond);
    CHECK_EQ(afterRefusal.completed, finished.completed);
}
