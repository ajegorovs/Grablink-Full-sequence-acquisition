// RunFolder.cpp - implementation of the run-unique output directory claimer.
//
// The claim is one CreateDirectoryW call per candidate. Windows creates the
// directory and tells us which of the two happened at once, which is exactly
// the atomicity a run folder needs: a caller who is told "created" owns the
// name, and a caller who is told ERROR_ALREADY_EXISTS learns that somebody -
// possibly another process, possibly a file that was left behind - is already
// there. There is no create-then-check window in which two runs could both
// believe they own the same folder.
//
// The candidate search is deliberately narrow. Only a collision advances to the
// next candidate; every other failure ends the search immediately, because a
// different name will not fix an access-denied base folder or a full volume,
// and retrying would bury the real error under a "no free name" message.
//
// Nothing here reads the clock. The caller owns the timestamp policy and hands
// in the fields; this file only formats them (see FormatTimestampToken in the
// header for why the caller keeps the clock).
//
// Only the C++ standard library plus the Win32 directory API is used: no MFC,
// no MultiCam, so this file can be unit tested without hardware.

#include "RunFolder.h"

#include <cerrno>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace grablinkcore
{

namespace
{

// Windows refuses to create a directory whose leaf name is one of the DOS
// device names, whatever folder it sits in: "C:\runs\CON" is not a directory
// but a reference to the console device. Such a token is refused up front so
// the caller gets "reserved device name" instead of a baffling system error.
bool IsReservedDeviceName(const std::wstring& token)
{
    static const wchar_t* kReserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5",
        L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
        L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };

    // ASCII-only folding: a token that survived ValidateRunToken() contains
    // nothing but ASCII, so towupper() and a locale are not needed here.
    std::wstring upper;
    upper.reserve(token.size());
    for (std::size_t index = 0; index < token.size(); ++index)
    {
        const wchar_t character = token[index];
        upper.push_back(character >= L'a' && character <= L'z'
                            ? static_cast<wchar_t>(character - L'a' + L'A')
                            : character);
    }

    const std::size_t reservedCount = sizeof(kReserved) / sizeof(kReserved[0]);
    for (std::size_t index = 0; index < reservedCount; ++index)
    {
        if (upper == kReserved[index])
        {
            return true;
        }
    }
    return false;
}

// What is at "path" right now: nothing, a directory, or something else (which
// for a run folder means a plain file). The base folder of a run must be a
// directory and nothing else.
enum PathKind
{
    kPathMissing,
    kPathDirectory,
    kPathOther
};

#if defined(_WIN32)
PathKind KindOf(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return kPathMissing;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? kPathDirectory : kPathOther;
}

// Removes the trailing directory separators of "base" so the join below never
// produces a doubled separator. A drive root ("C:\") keeps its separator: "C:"
// would be a drive-relative path, not the drive.
std::wstring WithoutTrailingSeparators(const std::wstring& base)
{
    std::wstring trimmed = base;
    for (;;)
    {
        if (trimmed.size() < 2)
        {
            break;
        }
        const wchar_t last = trimmed[trimmed.size() - 1];
        if (last != L'\\' && last != L'/')
        {
            break;
        }
        if (trimmed[trimmed.size() - 2] == L':')
        {
            break;
        }
        trimmed.erase(trimmed.size() - 1);
    }
    return trimmed;
}
#else
// Fallback for non-Windows builds: the byte-oriented stat entry point is used
// with a byte-truncated path, which is only correct for ASCII paths.
std::string NarrowPath(const std::wstring& path)
{
    std::string narrow;
    narrow.reserve(path.size());
    for (std::size_t index = 0; index < path.size(); ++index)
    {
        narrow.push_back(static_cast<char>(path[index] & 0xFF));
    }
    return narrow;
}

PathKind KindOf(const std::wstring& path)
{
    struct stat status;
    if (::stat(NarrowPath(path).c_str(), &status) != 0)
    {
        return kPathMissing;
    }
    return (status.st_mode & S_IFDIR) != 0 ? kPathDirectory : kPathOther;
}

std::wstring WithoutTrailingSeparators(const std::wstring& base)
{
    std::wstring trimmed = base;
    while (trimmed.size() > 1 &&
           (trimmed[trimmed.size() - 1] == L'\\' || trimmed[trimmed.size() - 1] == L'/'))
    {
        trimmed.erase(trimmed.size() - 1);
    }
    return trimmed;
}
#endif

// Attempts to create "path" as a directory. "collision" is set when something
// is already at that path (a directory or a plain file - both are a collision
// for a run folder) and "systemError" carries the code when the creation failed
// for any other reason.
bool TryClaimDirectory(const std::wstring& path, bool& collision,
                       unsigned long& systemError)
{
    collision = false;
    systemError = 0;

#if defined(_WIN32)
    if (::CreateDirectoryW(path.c_str(), 0) != FALSE)
    {
        return true;
    }

    const DWORD error = ::GetLastError();
    // ERROR_FILE_EXISTS is reported by some file systems where the other
    // reports ERROR_ALREADY_EXISTS; both mean "this name is taken".
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
    {
        collision = true;
        return false;
    }

    systemError = static_cast<unsigned long>(error);
    return false;
#else
    if (::mkdir(path.c_str()) == 0)
    {
        return true;
    }

    if (errno == EEXIST)
    {
        collision = true;
        return false;
    }

    systemError = static_cast<unsigned long>(errno);
    return false;
#endif
}

} // namespace

// --- RunFolder -------------------------------------------------------------

RunFolder::RunFolder()
    : m_path(),
      m_directoryName(),
      m_collisions(0),
      m_error()
{
}

void RunFolder::Reset()
{
    m_path.clear();
    m_directoryName.clear();
    m_collisions = 0;
    m_error.clear();
}

bool RunFolder::Fail(const std::string& message)
{
    // A refused call never leaves a claim behind: the caller must not be able
    // to read a stale Path() and save into a folder this call does not own.
    m_path.clear();
    m_directoryName.clear();
    m_error = message;
    return false;
}

bool RunFolder::Create(const std::wstring& baseFolder, const std::wstring& runToken,
                       unsigned int maxAttempts)
{
    Reset();

    std::string tokenError;
    if (!ValidateRunToken(runToken, tokenError))
    {
        return Fail(tokenError);
    }

    if (maxAttempts == 0)
    {
        return Fail("RunFolder: at least one creation attempt is required");
    }

    if (baseFolder.empty())
    {
        return Fail("RunFolder: the base folder is empty");
    }

    const std::wstring base = WithoutTrailingSeparators(baseFolder);

    // The base must already be a real directory. Creating it here would hide a
    // mistyped or unplugged output path behind a folder that suddenly exists on
    // some other volume, and reopening a base that is a file can only fail
    // later, after the capture has already been consumed.
    switch (KindOf(base))
    {
    case kPathDirectory:
        break;
    case kPathMissing:
        return Fail("RunFolder: the base folder does not exist");
    default:
        return Fail("RunFolder: the base folder is a file, not a folder");
    }

    for (unsigned int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        const std::wstring name = CandidateName(runToken, attempt);
        const std::wstring candidate = base + L"\\" + name;

        bool collision = false;
        unsigned long systemError = 0;
        if (TryClaimDirectory(candidate, collision, systemError))
        {
            // The directory is ours: CreateDirectoryW either created it or
            // failed, and it created it.
            m_directoryName = name;
            m_path = candidate;
            return true;
        }

        if (collision)
        {
            // Something is already at that path. For a run folder both a
            // directory and a file are a collision: an existing directory
            // belongs to another run and must never be written into, and a file
            // can never become the target of a BMP write at all. Either way the
            // run needs a name of its own, so the next candidate is tried.
            ++m_collisions;
            continue;
        }

        std::ostringstream message;
        message << "RunFolder: the run folder could not be created (system error "
                << systemError << ")";
        return Fail(message.str());
    }

    std::ostringstream message;
    message << "RunFolder: all " << maxAttempts
            << " candidate run folder names for this run are already taken";
    return Fail(message.str());
}

const std::wstring& RunFolder::Path() const
{
    return m_path;
}

const std::wstring& RunFolder::DirectoryName() const
{
    return m_directoryName;
}

unsigned int RunFolder::Collisions() const
{
    return m_collisions;
}

const std::string& RunFolder::LastError() const
{
    return m_error;
}

bool RunFolder::IsSafeRunToken(const std::wstring& runToken)
{
    std::string error;
    return ValidateRunToken(runToken, error);
}

bool RunFolder::ValidateRunToken(const std::wstring& runToken, std::string& error)
{
    error.clear();

    if (runToken.empty())
    {
        error = "RunFolder: the run token is empty";
        return false;
    }

    if (runToken.size() > kMaxRunTokenLength)
    {
        error = "RunFolder: the run token is longer than the 96 character limit";
        return false;
    }

    for (std::size_t index = 0; index < runToken.size(); ++index)
    {
        const wchar_t character = runToken[index];
        const bool asciiLetter = (character >= L'A' && character <= L'Z') ||
                                 (character >= L'a' && character <= L'z');
        const bool asciiDigit = character >= L'0' && character <= L'9';

        // Only ASCII letters, digits, '_' and '-' are allowed. The policy
        // refuses instead of repairing: a space, a dot (which would admit the
        // relative names "." and ".."), a path separator, a wildcard, a
        // reserved character or any non-ASCII character would make the leaf
        // name ambiguous, unsafe or dependent on the code page.
        if (!asciiLetter && !asciiDigit && character != L'_' && character != L'-')
        {
            error = "RunFolder: the run token contains a character that is not "
                    "allowed in a run folder name";
            return false;
        }
    }

    if (IsReservedDeviceName(runToken))
    {
        error = "RunFolder: the run token is a reserved Windows device name";
        return false;
    }

    return true;
}

std::wstring RunFolder::CandidateName(const std::wstring& runToken, unsigned int index)
{
    if (index == 0)
    {
        // The first candidate is the token itself, so a free name is used
        // exactly as the operator asked for it.
        return runToken;
    }

    std::wostringstream stream;
    stream << runToken << L'_' << index;
    return stream.str();
}

std::wstring RunFolder::FormatTimestampToken(unsigned int year, unsigned int month,
                                             unsigned int day, unsigned int hour,
                                             unsigned int minute, unsigned int second,
                                             unsigned int milliseconds)
{
    // Out of range fields produce no token at all. Clamping would silently name
    // a different instant, and a run folder that claims to belong to a moment
    // it does not belong to is worse than a run that refuses to start.
    if (year < 1970 || year > 9999 ||
        month < 1 || month > 12 ||
        day < 1 || day > 31 ||
        hour > 23 ||
        minute > 59 ||
        second > 59 ||
        milliseconds > 999)
    {
        return std::wstring();
    }

    // Fixed width fields ("YYYYMMDD_HHMMSS_mmm") make the token deterministic
    // and keep text order equal to chronological order.
    std::wostringstream stream;
    stream.fill(L'0');
    stream << std::setw(4) << year
           << std::setw(2) << month
           << std::setw(2) << day
           << L'_'
           << std::setw(2) << hour
           << std::setw(2) << minute
           << std::setw(2) << second
           << L'_'
           << std::setw(3) << milliseconds;
    return stream.str();
}

} // namespace grablinkcore
