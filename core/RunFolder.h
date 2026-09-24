#pragma once

// RunFolder.h - hardware-independent, run-unique output directory claimer.
//
// Every capture run must write into a directory of its own. The application
// used to save straight into the operator's chosen folder, which made two runs
// with the same frame prefix overwrite each other's BMP files. RunFolder makes
// "one run, one directory" the model: the caller hands in a base folder and a
// run token (a short, filesystem-safe leaf name, typically a formatted
// timestamp), and RunFolder claims base\token for it.
//
// The claim is a single CreateDirectoryW call, so it is atomic: whoever wins
// the call owns the directory, and a second process asking for the same name is
// told ERROR_ALREADY_EXISTS instead of writing into someone else's run. A
// collision - an existing *directory* or an existing *file* - never reuses the
// name: the next candidate, token_1, token_2, ... is tried until the attempt
// budget is used up. A file at base\token is a collision too, because writing
// into it is impossible and the run must not be given a folder it cannot use.
//
// Nothing is created and no directory is left behind by a refused request. On
// every failure Path() stays empty and LastError() explains what to fix: the
// token is empty or unsafe, the attempt budget is zero, the base folder is
// missing or is a file, the candidates are all taken, or the file system
// refused the call. The caller can therefore always decide between "retry with
// another name", "ask the operator to choose another folder" and "report a
// real I/O failure" from the message alone.
//
// The clock is the caller's, exactly as in CaptureStats: nothing here reads the
// system time. FormatTimestampToken() turns the caller's own year/month/day/
// hour/minute/second/millisecond fields into a deterministic, filesystem-safe
// token, so naming policy - which clock, which time zone, which resolution -
// stays with the capture code and the formatter is testable without a clock.
//
// Only the C++ standard library is used in this header: no MFC, no MultiCam and
// no Win32 type leaks out, so RunFolder can be unit tested without hardware.

#include <cstddef>
#include <string>

namespace grablinkcore
{

// Longest accepted run token. The value is a safety bound, not a file system
// one: it keeps the joined path (base + separator + token + "_<n>") well inside
// the classic MAX_PATH budget, so a token that is too long is refused as unsafe
// instead of failing later with a confusing "path not found".
const std::size_t kMaxRunTokenLength = 96;

// --- RunFolder -------------------------------------------------------------

// Claims one output directory per run. An instance is a value: it starts empty,
// Create() either fills it in with the claimed directory or leaves it empty.
class RunFolder
{
public:
    RunFolder();

    // Claims "maxAttempts" candidates at most, in order:
    //
    //     baseFolder\<runToken>, baseFolder\<runToken>_1, ... _<maxAttempts-1>
    //
    // The first candidate that CreateDirectoryW really creates becomes the run
    // folder: Path() is that directory, and no other run can claim it. Every
    // collision (existing directory or existing file) is counted in
    // Collisions() and the next candidate is tried.
    //
    // A candidate the file system refused for any reason other than a collision
    // (access denied, a name the volume rejects, a full disk) stops the search
    // at once: retrying a different name would only hide the real problem.
    //
    // Returns false, with Path() empty and the reason in LastError(), when the
    // run token is empty or unsafe, "maxAttempts" is zero, the base folder is
    // empty, missing or a file, every candidate collided, or the file system
    // refused the creation. The call is safe to repeat on the same object: a
    // failed Create() always leaves the previous claim released and Path()
    // empty.
    bool Create(const std::wstring& baseFolder, const std::wstring& runToken,
                unsigned int maxAttempts);

    // Directory claimed by the last successful Create(): baseFolder joined with
    // the accepted candidate name, with no doubled separator. Empty until a
    // claim succeeds, and empty again after a Create() that fails.
    const std::wstring& Path() const;

    // Leaf name of the claimed directory: the token itself, or the first free
    // "<runToken>_<n>". Empty until a claim succeeds.
    const std::wstring& DirectoryName() const;

    // Number of candidates that were already taken by the last Create(). Zero
    // for a token that was free, one for the first suffixed candidate, and equal
    // to the attempt budget when the budget was exhausted.
    unsigned int Collisions() const;

    // Text of the most recent failure, empty after a success.
    const std::string& LastError() const;

    // --- token policy: pure functions, no file system access ---------------

    // True when "runToken" may be used as a run folder leaf name.
    static bool IsSafeRunToken(const std::wstring& runToken);

    // Same decision, with the reason in "error" (cleared on success). A token is
    // safe when it is not empty, is at most kMaxRunTokenLength characters, uses
    // only ASCII letters, digits, '_' and '-', and is not one of the reserved
    // Windows device names (CON, NUL, COM1, LPT1, ...).
    //
    // The policy refuses rather than repairs. A space, a dot - which would also
    // admit the relative names "." and ".." - a path separator, a wildcard or
    // any non-ASCII character would make the leaf name ambiguous, unsafe or
    // non-portable, and silently rewriting the operator's name would produce a
    // folder they never asked for.
    static bool ValidateRunToken(const std::wstring& runToken, std::string& error);

    // Deterministic candidate name for "index" of a search: index 0 is the
    // token itself, index n is "<runToken>_<n>". Pure: the same arguments always
    // produce the same name, so a caller can predict and log the whole search
    // before Create() runs.
    static std::wstring CandidateName(const std::wstring& runToken,
                                      unsigned int index);

    // Deterministic run token for a caller-supplied local timestamp:
    // "YYYYMMDD_HHMMSS_mmm", for example "20260922_174012_045". Every field is
    // zero padded to a fixed width, so tokens sort chronologically as text and
    // two runs of the same millisecond format identically.
    //
    // Returns an empty string when a component is outside its range (year 1970
    // to 9999, month 1 to 12, day 1 to 31, hour 0 to 23, minute and second 0 to
    // 59, milliseconds 0 to 999). Clamping instead would silently name a
    // different instant, which is the one thing a run token must never do; an
    // empty token is then refused by ValidateRunToken() like any other.
    static std::wstring FormatTimestampToken(unsigned int year, unsigned int month,
                                             unsigned int day, unsigned int hour,
                                             unsigned int minute, unsigned int second,
                                             unsigned int milliseconds);

private:
    RunFolder(const RunFolder&);
    RunFolder& operator=(const RunFolder&);

    // Clears the claim and records the reason; returns false for the callers.
    bool Fail(const std::string& message);
    void Reset();

    std::wstring m_path;
    std::wstring m_directoryName;
    unsigned int m_collisions;
    std::string m_error;
};

} // namespace grablinkcore
