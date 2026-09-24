// RunFolderTests.cpp - behavioural tests for the run-unique output directory.
//
// RunFolder is the piece that keeps two runs of the same prefix - and, with a
// timestamp token, two runs of the same operator folder - from writing over
// each other's BMP files. The tests therefore concentrate on four properties:
//
//  * token policy: unsafe, empty and reserved names are refused, never repaired;
//  * the claim: exactly one CreateDirectoryW per candidate, base\token first
//    and base\token_<n> afterwards, with a file counting as a collision just
//    like a directory;
//  * the failure contract: Path() stays empty, nothing is left behind, and
//    LastError() says which of the reasons applied;
//  * the end-to-end promise: two runs with the same token really do keep their
//    identically named BMP files apart, checked through SaveWorker and
//    FrameSnapshot rather than through RunFolder alone.
//
// The tests touch the file system, so each one works inside its own TempDir
// under the system temp folder and removes the run folders it created before
// the TempDir itself is destroyed.

#include <cstddef>
#include <string>
#include <vector>

#include "TestHarness.h"

#include "RunFolder.h"
#include "FrameBuffer.h"
#include "SaveWorker.h"

using grablinkcore::FrameSnapshot;
using grablinkcore::RunFolder;
using grablinkcore::SaveProgress;
using grablinkcore::SaveWorker;

namespace
{
// Expected BMP layout, spelled out here rather than taken from the writer:
// 14-byte file header + 40-byte info header + 256 four-byte palette entries.
const unsigned int kBmpPixelDataOffset = 14 + 40 + 1024;

// Geometry of the frames the end-to-end tests save. Four pixels wide keeps the
// rows unpadded, so the pixel bytes in the file are exactly the frame bytes.
const int kWidth = 4;
const int kHeight = 2;
const unsigned char kFirstRunValue = 0x11;
const unsigned char kSecondRunValue = 0x22;

std::wstring Join(const std::wstring& folder, const wchar_t* name)
{
    return folder + L"\\" + name;
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool CreatePlainFile(const std::wstring& path)
{
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    ::CloseHandle(handle);
    return true;
}

// Creates a directory that the test wants to collide with a candidate name.
bool ClaimDirectory(const std::wstring& path)
{
    if (::CreateDirectoryW(path.c_str(), 0) != FALSE)
    {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

// Removes the files directly inside "folder" and then the folder itself, so the
// TempDir that owns it can still be removed when the test ends.
void RemoveFolder(const std::wstring& folder)
{
    if (folder.empty() || !DirectoryExists(folder))
    {
        return;
    }

    const std::wstring pattern = folder + L"\\*";
    WIN32_FIND_DATAW findData;
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &findData);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (findData.cFileName[0] == L'.')
            {
                continue;
            }
            ::DeleteFileW(Join(folder, findData.cFileName).c_str());
        }
        while (::FindNextFileW(find, &findData) != 0);
        ::FindClose(find);
    }

    ::RemoveDirectoryW(folder.c_str());
}

unsigned int CountFiles(const std::wstring& folder)
{
    const std::wstring pattern = folder + L"\\*";
    WIN32_FIND_DATAW findData;
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    unsigned int count = 0;
    do
    {
        if (findData.cFileName[0] == L'.')
        {
            continue;
        }
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            ++count;
        }
    }
    while (::FindNextFileW(find, &findData) != 0);
    ::FindClose(find);

    return count;
}

bool MentionsText(const std::string& message, const char* text)
{
    return message.find(text) != std::string::npos;
}

// Saves "frameCount" identical frames of "value" into "folder" through the real
// SaveWorker/FrameSnapshot path, with the same "Image" prefix every run - which
// is exactly the naming that used to collide.
bool SaveFrames(const std::wstring& folder, unsigned char value,
                std::size_t frameCount)
{
    const std::vector<unsigned char> pixels(
        static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight) *
            frameCount,
        value);

    FrameSnapshot snapshot(kWidth, kHeight, frameCount, pixels);
    if (!snapshot.IsValid())
    {
        return false;
    }

    SaveWorker worker;
    if (!worker.Start(snapshot, folder, L"Image"))
    {
        return false;
    }
    worker.Wait();

    const SaveProgress progress = worker.Progress();
    return !progress.running && !progress.cancelled && progress.failed == 0 &&
           progress.completed == frameCount;
}

// Reads the pixel bytes of a saved BMP and requires every one of them to hold
// "value".
bool PixelsAllEqual(const std::wstring& path, unsigned char value,
                    std::size_t expectedBytes)
{
    std::vector<unsigned char> bytes;
    if (!test::ReadFileBytes(path, bytes))
    {
        return false;
    }
    if (bytes.size() != static_cast<std::size_t>(kBmpPixelDataOffset) + expectedBytes)
    {
        return false;
    }
    for (std::size_t index = 0; index < expectedBytes; ++index)
    {
        if (bytes[kBmpPixelDataOffset + index] != value)
        {
            return false;
        }
    }
    return true;
}
} // namespace

// ---------------------------------------------------------------------------
// Test 1: a safe token - letters, digits, underscore, hyphen - is accepted.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderAcceptsSafeTokenShapes)
{
    std::string error;

    CHECK_EQ(RunFolder::IsSafeRunToken(L"run"), true);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"Image"), true);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"20260922_174012_045"), true);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"run-1_test_2"), true);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"0"), true);

    // A token of exactly the maximum accepted length is still safe.
    const std::wstring longest(grablinkcore::kMaxRunTokenLength, L'a');
    CHECK_EQ(RunFolder::IsSafeRunToken(longest), true);

    CHECK_EQ(RunFolder::ValidateRunToken(L"run", error), true);
    CHECK(error.empty());
}

// ---------------------------------------------------------------------------
// Test 2: an empty token is refused with a reason and never repaired.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderRejectsEmptyToken)
{
    std::string error;

    CHECK_EQ(RunFolder::IsSafeRunToken(L""), false);
    CHECK_EQ(RunFolder::ValidateRunToken(L"", error), false);
    CHECK(MentionsText(error, "empty"));
}

// ---------------------------------------------------------------------------
// Test 3: every character outside [A-Za-z0-9_-] is refused, so the leaf name
// can never be a wildcard, a separator, the relative name "." or "..", or a
// character a file system might reject.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderRejectsUnsafeTokenCharacters)
{
    std::string error;

    CHECK_EQ(RunFolder::IsSafeRunToken(L"run folder"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"run.folder"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"."), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L".."), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"nested\\run"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"nested/run"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"drive:run"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"wild*card"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"question?"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"quote\"mark"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"pipe|mark"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"angle<mark>"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"tab\tmark"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"newline\nmark"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"\u00e9t\u00e9"), false);

    CHECK_EQ(RunFolder::ValidateRunToken(L"run\u00e9", error), false);
    CHECK(MentionsText(error, "character"));
}

// ---------------------------------------------------------------------------
// Test 4: a token longer than the bound is refused; the bound keeps the joined
// path inside the classic MAX_PATH budget.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderRejectsOverlongToken)
{
    const std::wstring overlong(grablinkcore::kMaxRunTokenLength + 1, L'a');
    const std::wstring atBound(grablinkcore::kMaxRunTokenLength, L'a');
    std::string error;

    CHECK_EQ(RunFolder::IsSafeRunToken(atBound), true);
    CHECK_EQ(RunFolder::IsSafeRunToken(overlong), false);
    CHECK_EQ(RunFolder::ValidateRunToken(overlong, error), false);
    CHECK(MentionsText(error, "longer"));
}

// ---------------------------------------------------------------------------
// Test 5: the reserved Windows device names are refused - creating a folder
// called CON or LPT1 fails on the device name, not on the file system - while
// names that merely start with one are fine.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderRejectsReservedDeviceNames)
{
    std::string error;

    CHECK_EQ(RunFolder::IsSafeRunToken(L"CON"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"con"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"NUL"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"aux"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"COM1"), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"lpt9"), false);

    CHECK_EQ(RunFolder::ValidateRunToken(L"con", error), false);
    CHECK(MentionsText(error, "reserved"));

    // Only the exact device name is reserved, so these are ordinary names.
    CHECK_EQ(RunFolder::IsSafeRunToken(L"CONS"), true);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"COM10"), true);
    CHECK_EQ(RunFolder::IsSafeRunToken(L"run_con"), true);
}

// ---------------------------------------------------------------------------
// Test 6: timestamp tokens are deterministic, fixed width, and safe.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderFormatTimestampTokenIsDeterministicAndSafe)
{
    CHECK_EQ(RunFolder::FormatTimestampToken(2026, 9, 22, 17, 40, 12, 45),
             std::wstring(L"20260922_174012_045"));

    // Every field is zero padded to a fixed width, so different calls of the
    // same instant are identical and text order is chronological order.
    CHECK_EQ(RunFolder::FormatTimestampToken(2026, 1, 2, 3, 4, 5, 6),
             std::wstring(L"20260102_030405_006"));
    CHECK_EQ(RunFolder::FormatTimestampToken(2000, 12, 31, 23, 59, 59, 999),
             std::wstring(L"20001231_235959_999"));

    const std::wstring token = RunFolder::FormatTimestampToken(2026, 9, 22, 17, 40, 12, 45);
    CHECK_EQ(token.empty(), false);
    CHECK_EQ(RunFolder::IsSafeRunToken(token), true);

    // Deterministic: the same fields always give the same token, and a
    // different millisecond always gives a different one.
    CHECK_EQ(RunFolder::FormatTimestampToken(2026, 9, 22, 17, 40, 12, 45), token);
    CHECK(RunFolder::FormatTimestampToken(2026, 9, 22, 17, 40, 12, 46) != token);
}

// ---------------------------------------------------------------------------
// Test 7: an out-of-range timestamp component yields no token at all instead of
// a clamped one, which would silently name a different instant.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderRejectsInvalidTimestampComponents)
{
    CHECK(RunFolder::FormatTimestampToken(1969, 9, 22, 17, 40, 12, 45).empty());
    CHECK(RunFolder::FormatTimestampToken(2026, 0, 22, 17, 40, 12, 45).empty());
    CHECK(RunFolder::FormatTimestampToken(2026, 13, 22, 17, 40, 12, 45).empty());
    CHECK(RunFolder::FormatTimestampToken(2026, 9, 0, 17, 40, 12, 45).empty());
    CHECK(RunFolder::FormatTimestampToken(2026, 9, 32, 17, 40, 12, 45).empty());
    CHECK(RunFolder::FormatTimestampToken(2026, 9, 22, 24, 40, 12, 45).empty());
    CHECK(RunFolder::FormatTimestampToken(2026, 9, 22, 17, 60, 12, 45).empty());
    CHECK(RunFolder::FormatTimestampToken(2026, 9, 22, 17, 40, 60, 45).empty());
    CHECK(RunFolder::FormatTimestampToken(2026, 9, 22, 17, 40, 12, 1000).empty());

    // The empty result is then refused as a token like any other.
    CHECK_EQ(RunFolder::IsSafeRunToken(
                 RunFolder::FormatTimestampToken(2026, 0, 1, 0, 0, 0, 0)),
             false);
}

// ---------------------------------------------------------------------------
// Test 8: candidate names are deterministic and enumerated from zero.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderCandidateNamesAreDeterministic)
{
    CHECK_EQ(RunFolder::CandidateName(L"run", 0), std::wstring(L"run"));
    CHECK_EQ(RunFolder::CandidateName(L"run", 1), std::wstring(L"run_1"));
    CHECK_EQ(RunFolder::CandidateName(L"run", 2), std::wstring(L"run_2"));
    CHECK_EQ(RunFolder::CandidateName(L"run", 42), std::wstring(L"run_42"));

    // The enumeration never reuses a name, so a search can never claim a folder
    // it has already seen taken.
    CHECK(RunFolder::CandidateName(L"run", 0) != RunFolder::CandidateName(L"run", 1));
    CHECK(RunFolder::CandidateName(L"run", 1) != RunFolder::CandidateName(L"run", 2));
}

// ---------------------------------------------------------------------------
// Test 9: a free token claims base\token on the first attempt, and the claimed
// directory really exists.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderClaimsBaseTokenForAFreeToken)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_claim"));

    RunFolder run;
    REQUIRE(run.Create(base.Path(), L"run", 3));

    const std::wstring expected = Join(base.Path(), L"run");
    CHECK_EQ(run.Path(), expected);
    CHECK_EQ(run.DirectoryName(), std::wstring(L"run"));
    CHECK_EQ(run.Collisions(), static_cast<unsigned int>(0));
    CHECK(run.LastError().empty());
    CHECK(DirectoryExists(expected));

    RemoveFolder(expected);
}

// ---------------------------------------------------------------------------
// Test 10: an existing directory with the token's name is a collision, so the
// run is given the suffixed folder and the first run's directory is untouched.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderSuffixesOnDirectoryCollision)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_dircollision"));

    const std::wstring taken = Join(base.Path(), L"run");
    REQUIRE(ClaimDirectory(taken));
    REQUIRE(CreatePlainFile(Join(taken, L"Image_00000.bmp")));

    RunFolder run;
    REQUIRE(run.Create(base.Path(), L"run", 4));

    const std::wstring expected = Join(base.Path(), L"run_1");
    CHECK_EQ(run.Path(), expected);
    CHECK_EQ(run.DirectoryName(), std::wstring(L"run_1"));
    CHECK_EQ(run.Collisions(), static_cast<unsigned int>(1));
    CHECK(run.LastError().empty());
    CHECK(DirectoryExists(expected));

    // The previous run's frame is still in its own folder: the claim did not
    // reuse - or clear - a directory another run owns.
    CHECK(test::FileExists(Join(taken, L"Image_00000.bmp")));

    RemoveFolder(expected);
    RemoveFolder(taken);
}

// ---------------------------------------------------------------------------
// Test 11: a plain *file* at the token's name is a collision too. Neither
// reusing it nor reporting a generic I/O failure would be right: the run must
// get a folder it can actually write into.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderTreatsExistingFileAsCollision)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_filecollision"));

    const std::wstring taken = Join(base.Path(), L"run");
    REQUIRE(CreatePlainFile(taken));

    RunFolder run;
    REQUIRE(run.Create(base.Path(), L"run", 2));

    const std::wstring expected = Join(base.Path(), L"run_1");
    CHECK_EQ(run.Path(), expected);
    CHECK_EQ(run.Collisions(), static_cast<unsigned int>(1));
    CHECK(DirectoryExists(expected));
    CHECK(test::FileExists(taken));

    RemoveFolder(expected);
    ::DeleteFileW(taken.c_str());
}

// ---------------------------------------------------------------------------
// Test 12: the attempt budget is exactly what it says; when it is used up the
// claim fails, Path() stays empty and the error names the reason.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderFailsWhenEveryCandidateIsTaken)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_exhausted"));

    const std::wstring first = Join(base.Path(), L"run");
    const std::wstring second = Join(base.Path(), L"run_1");
    REQUIRE(ClaimDirectory(first));
    REQUIRE(ClaimDirectory(second));

    RunFolder run;
    CHECK_EQ(run.Create(base.Path(), L"run", 2), false);
    CHECK(run.Path().empty());
    CHECK(run.DirectoryName().empty());
    CHECK_EQ(run.Collisions(), static_cast<unsigned int>(2));
    CHECK(MentionsText(run.LastError(), "taken"));

    // A third attempt is not made quietly: with a budget of three the same
    // folder still succeeds, which proves the budget is what stopped the two
    // attempts above.
    REQUIRE(run.Create(base.Path(), L"run", 3));
    CHECK_EQ(run.Path(), Join(base.Path(), L"run_2"));
    CHECK_EQ(run.Collisions(), static_cast<unsigned int>(2));

    RemoveFolder(Join(base.Path(), L"run_2"));
    RemoveFolder(second);
    RemoveFolder(first);
}

// ---------------------------------------------------------------------------
// Test 13: a zero attempt budget is refused before anything is created.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderRefusesZeroAttempts)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_zeroattempts"));

    RunFolder run;
    CHECK_EQ(run.Create(base.Path(), L"run", 0), false);
    CHECK(run.Path().empty());
    CHECK_EQ(run.Collisions(), static_cast<unsigned int>(0));
    CHECK(MentionsText(run.LastError(), "attempt"));
    CHECK(!DirectoryExists(Join(base.Path(), L"run")));
}

// ---------------------------------------------------------------------------
// Test 14: a missing base folder, an empty base folder and a base that is a
// file are all refused, and nothing is created next to them.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderRefusesUnusableBaseFolder)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_badbase"));

    RunFolder run;

    CHECK_EQ(run.Create(Join(base.Path(), L"missing"), L"run", 3), false);
    CHECK(run.Path().empty());
    CHECK(MentionsText(run.LastError(), "does not exist"));

    CHECK_EQ(run.Create(L"", L"run", 3), false);
    CHECK(run.Path().empty());
    CHECK(MentionsText(run.LastError(), "empty"));

    const std::wstring fileBase = Join(base.Path(), L"basefile");
    REQUIRE(CreatePlainFile(fileBase));
    CHECK_EQ(run.Create(fileBase, L"run", 3), false);
    CHECK(run.Path().empty());
    CHECK(MentionsText(run.LastError(), "file, not a folder"));

    CHECK_EQ(run.Create(base.Path(), L"run", 3), true);
    CHECK(!run.Path().empty());
    RemoveFolder(run.Path());
}

// ---------------------------------------------------------------------------
// Test 15: an unsafe token is refused without touching the file system.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderRefusesUnsafeTokenWithoutCreatingAnything)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_unsafetoken"));

    RunFolder run;

    CHECK_EQ(run.Create(base.Path(), L"run folder", 3), false);
    CHECK(run.Path().empty());
    CHECK_EQ(run.Collisions(), static_cast<unsigned int>(0));
    CHECK(!run.LastError().empty());
    CHECK(!DirectoryExists(Join(base.Path(), L"run folder")));

    CHECK_EQ(run.Create(base.Path(), L"", 3), false);
    CHECK(run.Path().empty());

    CHECK_EQ(run.Create(base.Path(), std::wstring(grablinkcore::kMaxRunTokenLength + 1, L'a'), 3), false);
    CHECK(run.Path().empty());

    // Nothing at all was created in the base folder.
    CHECK_EQ(CountFiles(base.Path()), static_cast<unsigned int>(0));
}

// ---------------------------------------------------------------------------
// Test 16: the claim is released on a failed call, so a stale Path() can never
// be mistaken for a folder this call owns.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderFailedCreateClearsTheClaim)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_release"));

    RunFolder run;
    REQUIRE(run.Create(base.Path(), L"run", 2));
    const std::wstring claimed = run.Path();
    REQUIRE(!claimed.empty());

    CHECK_EQ(run.Create(Join(base.Path(), L"missing"), L"run", 2), false);
    CHECK(run.Path().empty());
    CHECK(run.DirectoryName().empty());
    CHECK_EQ(run.Collisions(), static_cast<unsigned int>(0));
    CHECK(!run.LastError().empty());

    // The directory the earlier call claimed is still there and untouched.
    CHECK(DirectoryExists(claimed));
    RemoveFolder(claimed);
}

// ---------------------------------------------------------------------------
// Test 17: a base folder written with a trailing separator claims the same
// directory as the plain one - no doubled separator, no second folder.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderTrailingSeparatorDoesNotDouble)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_trailing"));

    RunFolder plain;
    REQUIRE(plain.Create(base.Path(), L"run", 2));

    RunFolder trailing;
    REQUIRE(trailing.Create(base.Path() + L"\\", L"run", 2));

    CHECK_EQ(trailing.Path(), Join(base.Path(), L"run_1"));
    // The same name was taken by the first claim, so the second run is
    // suffixed: the trailing separator changed nothing about the names, and the
    // joined path has no doubled separator either.
    CHECK_EQ(trailing.DirectoryName(), std::wstring(L"run_1"));
    CHECK_EQ(trailing.Path(), plain.Path() + L"_1");
    CHECK(DirectoryExists(trailing.Path()));

    RemoveFolder(trailing.Path());
    RemoveFolder(plain.Path());
}

// ---------------------------------------------------------------------------
// Test 18 (end to end): two runs with the same token, saving the same
// deterministic BMP names through the real SaveWorker, keep their files apart.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderEndToEndSameTokenRunsKeepBmpNamesIsolated)
{
    test::TempDir base;
    REQUIRE(base.Create(L"runfolder_e2e"));

    // The token is the caller's own timestamp, formatted deterministically.
    const std::wstring token =
        RunFolder::FormatTimestampToken(2026, 9, 22, 17, 40, 12, 45);
    REQUIRE(!token.empty());

    RunFolder firstRun;
    REQUIRE(firstRun.Create(base.Path(), token, 4));
    RunFolder secondRun;
    REQUIRE(secondRun.Create(base.Path(), token, 4));

    // Same token, same base: the second run got its own folder instead of the
    // first run's.
    CHECK_EQ(firstRun.Path(), Join(base.Path(), token.c_str()));
    CHECK_EQ(secondRun.Path(), Join(base.Path(), (token + L"_1").c_str()));
    CHECK(firstRun.Path() != secondRun.Path());

    const std::size_t frameCount = 3;
    REQUIRE(SaveFrames(firstRun.Path(), kFirstRunValue, frameCount));
    REQUIRE(SaveFrames(secondRun.Path(), kSecondRunValue, frameCount));

    // Both runs produced the identical deterministic file names...
    const std::size_t frameBytes =
        static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight);
    const std::wstring firstName = SaveWorker::FileNameForIndex(L"Image", 0, frameCount);
    CHECK_EQ(firstName, std::wstring(L"Image_00000.bmp"));
    CHECK(test::FileExists(Join(firstRun.Path(), firstName.c_str())));
    CHECK(test::FileExists(Join(secondRun.Path(), firstName.c_str())));

    // ...into their own folders, each holding its own pixels, and neither run
    // lost or overwrote a frame of the other.
    CHECK_EQ(CountFiles(firstRun.Path()), static_cast<unsigned int>(frameCount));
    CHECK_EQ(CountFiles(secondRun.Path()), static_cast<unsigned int>(frameCount));
    CHECK(PixelsAllEqual(Join(firstRun.Path(), firstName.c_str()), kFirstRunValue,
                         frameBytes));
    CHECK(PixelsAllEqual(Join(secondRun.Path(), firstName.c_str()), kSecondRunValue,
                         frameBytes));
    for (std::size_t index = 1; index < frameCount; ++index)
    {
        const std::wstring name = SaveWorker::FileNameForIndex(L"Image", index, frameCount);
        CHECK(PixelsAllEqual(Join(firstRun.Path(), name.c_str()), kFirstRunValue,
                             frameBytes));
        CHECK(PixelsAllEqual(Join(secondRun.Path(), name.c_str()), kSecondRunValue,
                             frameBytes));
    }

    RemoveFolder(secondRun.Path());
    RemoveFolder(firstRun.Path());
}

// ---------------------------------------------------------------------------
// Test 19 (end to end): the same token in two different base folders - the
// operator picked another output folder between runs - stays isolated as well.
// ---------------------------------------------------------------------------
TEST_CASE(RunFolderEndToEndSameTokenInDifferentBasesStaysIsolated)
{
    test::TempDir baseA;
    test::TempDir baseB;
    REQUIRE(baseA.Create(L"runfolder_e2e_a"));
    REQUIRE(baseB.Create(L"runfolder_e2e_b"));

    const std::wstring token =
        RunFolder::FormatTimestampToken(2026, 9, 22, 17, 40, 12, 45);

    RunFolder runA;
    REQUIRE(runA.Create(baseA.Path(), token, 4));
    RunFolder runB;
    REQUIRE(runB.Create(baseB.Path(), token, 4));

    // Neither folder collided: the isolation comes from the base folder, and
    // the same leaf name is still used in both.
    CHECK_EQ(runA.Collisions(), static_cast<unsigned int>(0));
    CHECK_EQ(runB.Collisions(), static_cast<unsigned int>(0));
    CHECK_EQ(runA.DirectoryName(), token);
    CHECK_EQ(runB.DirectoryName(), token);

    const std::size_t frameCount = 2;
    REQUIRE(SaveFrames(runA.Path(), kFirstRunValue, frameCount));
    REQUIRE(SaveFrames(runB.Path(), kSecondRunValue, frameCount));

    const std::size_t frameBytes =
        static_cast<std::size_t>(kWidth) * static_cast<std::size_t>(kHeight);
    const std::wstring name = SaveWorker::FileNameForIndex(L"Image", 0, frameCount);
    CHECK(PixelsAllEqual(Join(runA.Path(), name.c_str()), kFirstRunValue, frameBytes));
    CHECK(PixelsAllEqual(Join(runB.Path(), name.c_str()), kSecondRunValue, frameBytes));
    CHECK_EQ(CountFiles(runA.Path()), static_cast<unsigned int>(frameCount));
    CHECK_EQ(CountFiles(runB.Path()), static_cast<unsigned int>(frameCount));

    RemoveFolder(runB.Path());
    RemoveFolder(runA.Path());
}
