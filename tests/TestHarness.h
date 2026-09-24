#pragma once

// TestHarness.h - minimal, zero-dependency test harness for the hardware-free
// GrablinkSnapshot core tests.
//
// Design notes:
//  * Header-only: the registry lives in a function-local static so that the
//    harness can be shared by TestMain.cpp and CoreTests.cpp without a .cpp.
//  * No external framework, no MFC, no MultiCam: only the C++ standard library
//    plus a small Win32 section used to build and clean isolated temp folders.
//  * CHECK_* macros record a failure and keep going; REQUIRE_* macros record the
//    failure and abort the current test (used when continuing would crash).

#include <atomic>
#include <cstddef>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace test {

typedef void (*TestFunction)();

struct TestCase
{
    const char* name;
    TestFunction function;
};

struct RequireFailure
{
};

inline std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> registry;
    return registry;
}

inline int& FailureCount()
{
    static int count = 0;
    return count;
}

inline const char*& CurrentTest()
{
    static const char* name = "";
    return name;
}

// Process-wide count of C++ allocations, for the tests that have to prove a
// hot path does not allocate.
//
// A global replacement of operator new/delete may be defined only once per
// program, so it lives in CaptureStatsTests.cpp and feeds this counter. Making
// the counter a function-local static behind an inline accessor is what lets
// any test file read it: the executable then routes every C++ allocation
// through one observable count, and a test can bracket the calls it cares
// about with two reads.
//
// The increment inside the allocator is a relaxed atomic add and the reads
// here are relaxed loads, so observing the count costs no allocation of its own
// and cannot perturb the window it measures. A window is only meaningful while
// no other thread is allocating, so the tests that use it are single threaded.
inline std::atomic<long>& AllocationCount()
{
    // The value is given explicitly: std::atomic has no initialising default
    // constructor before C++20.
    static std::atomic<long> count(0);
    return count;
}

inline void ReportFailure(const char* file, int line, const std::string& message)
{
    ++FailureCount();
    std::cout << "    FAIL " << CurrentTest() << " [" << file << ":" << line << "]: "
              << message << std::endl;
}

inline void ReportFailure(const char* file, int line, const char* message)
{
    ReportFailure(file, line, std::string(message));
}

class Registrar
{
public:
    Registrar(const char* name, TestFunction function)
    {
        TestCase testCase;
        testCase.name = name;
        testCase.function = function;
        Registry().push_back(testCase);
    }
};

// --- comparison helpers ----------------------------------------------------

template <typename A, typename B>
inline bool ValuesEqual(const A& a, const B& b)
{
    return a == b;
}

inline bool ValuesEqual(const std::string& a, const char* b)
{
    return a == b;
}

inline bool ValuesEqual(const char* a, const std::string& b)
{
    return b == a;
}

template <typename A, typename B>
inline bool CheckEqual(const char* file, int line, const char* actualExpr,
                       const char* expectedExpr, const A& actual, const B& expected)
{
    if (!ValuesEqual(actual, expected))
    {
        std::ostringstream stream;
        stream << "expected " << expectedExpr << " but got " << actualExpr;
        ReportFailure(file, line, stream.str());
        return false;
    }
    return true;
}

inline int RunAll()
{
    std::vector<TestCase>& registry = Registry();
    std::cout << "Running " << registry.size() << " test(s)" << std::endl;

    int failed = 0;
    for (std::size_t index = 0; index < registry.size(); ++index)
    {
        CurrentTest() = registry[index].name;
        const int failuresBefore = FailureCount();
        std::cout << "  [ RUN  ] " << registry[index].name << std::endl;
        try
        {
            registry[index].function();
        }
        catch (const RequireFailure&)
        {
            // The failing REQUIRE already reported the reason.
        }
        catch (const std::exception& exception)
        {
            ReportFailure("<exception>", 0, exception.what());
        }
        catch (...)
        {
            ReportFailure("<exception>", 0, "unknown C++ exception");
        }

        if (FailureCount() == failuresBefore)
        {
            std::cout << "  [  OK  ] " << registry[index].name << std::endl;
        }
        else
        {
            ++failed;
            std::cout << "  [ FAIL ] " << registry[index].name << std::endl;
        }
    }

    const std::size_t passed = registry.size() - static_cast<std::size_t>(failed);
    if (failed == 0)
    {
        std::cout << "ALL TESTS PASSED (" << passed << " passed, 0 failed)" << std::endl;
        return 0;
    }

    std::cout << "TESTS FAILED (" << passed << " passed, " << failed << " failed)" << std::endl;
    return 1;
}

// --- little-endian readers for BMP assertions ------------------------------

inline unsigned int GetU16LE(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<unsigned int>(bytes[offset]) |
           (static_cast<unsigned int>(bytes[offset + 1]) << 8);
}

inline unsigned long long GetU32LE(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<unsigned long long>(bytes[offset]) |
           (static_cast<unsigned long long>(bytes[offset + 1]) << 8) |
           (static_cast<unsigned long long>(bytes[offset + 2]) << 16) |
           (static_cast<unsigned long long>(bytes[offset + 3]) << 24);
}

inline int GetI32LE(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return static_cast<int>(static_cast<unsigned int>(GetU32LE(bytes, offset)));
}

#if defined(_WIN32)

inline std::wstring ToWide(unsigned long value)
{
    std::wostringstream stream;
    stream << value;
    return stream.str();
}

inline bool FileExists(const std::wstring& path)
{
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes)
{
    bytes.clear();
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    unsigned char chunk[4096];
    DWORD read = 0;
    bool ok = true;
    for (;;)
    {
        if (!::ReadFile(handle, chunk, sizeof(chunk), &read, 0))
        {
            ok = false;
            break;
        }
        if (read == 0)
        {
            break;
        }
        bytes.insert(bytes.end(), chunk, chunk + read);
    }

    ::CloseHandle(handle);
    return ok;
}

// TempDir: isolated scratch directory under the system temp folder. The
// destructor removes every file it contains and then the directory itself, so
// tests clean up even when a REQUIRE_* aborts the test through an exception.
class TempDir
{
public:
    TempDir() : m_path()
    {
    }

    ~TempDir()
    {
        Remove();
    }

    const std::wstring& Path() const
    {
        return m_path;
    }

    std::wstring File(const wchar_t* name) const
    {
        return m_path + L"\\" + name;
    }

    bool Create(const wchar_t* label)
    {
        static unsigned long counter = 0;
        ++counter;

        wchar_t base[MAX_PATH] = {0};
        if (::GetTempPathW(MAX_PATH, base) == 0)
        {
            return false;
        }

        std::wstring name(L"grablink_core_tests_");
        name += label;
        name += L"_" + ToWide(::GetCurrentProcessId()) + L"_" + ToWide(counter);
        std::wstring candidate = std::wstring(base) + name;

        if (::CreateDirectoryW(candidate.c_str(), 0) == 0)
        {
            const DWORD error = ::GetLastError();
            if (error != ERROR_ALREADY_EXISTS)
            {
                return false;
            }
        }

        m_path = candidate;
        return true;
    }

    void Remove()
    {
        if (m_path.empty())
        {
            return;
        }

        std::wstring pattern = m_path + L"\\*";
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
                ::DeleteFileW((m_path + L"\\" + findData.cFileName).c_str());
            }
            while (::FindNextFileW(find, &findData) != 0);
            ::FindClose(find);
        }

        ::RemoveDirectoryW(m_path.c_str());
        m_path.clear();
    }

private:
    TempDir(const TempDir&);
    TempDir& operator=(const TempDir&);

    std::wstring m_path;
};

#endif // _WIN32

} // namespace test

// --- macros ----------------------------------------------------------------

#define TEST_CASE(name)                                                        \
    static void name();                                                        \
    static const test::Registrar registrar_##name(#name, &name);               \
    static void name()

#define CHECK(condition)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(condition))                                                      \
        {                                                                      \
            test::ReportFailure(__FILE__, __LINE__, "CHECK failed: " #condition); \
        }                                                                      \
    } while (0)

#define REQUIRE(condition)                                                     \
    do                                                                         \
    {                                                                          \
        if (!(condition))                                                      \
        {                                                                      \
            test::ReportFailure(__FILE__, __LINE__, "REQUIRE failed: " #condition); \
            throw test::RequireFailure();                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected)                                             \
    test::CheckEqual(__FILE__, __LINE__, #actual, #expected, (actual), (expected))

#define REQUIRE_EQ(actual, expected)                                           \
    do                                                                         \
    {                                                                          \
        if (!test::CheckEqual(__FILE__, __LINE__, #actual, #expected, (actual), (expected))) \
        {                                                                      \
            throw test::RequireFailure();                                      \
        }                                                                      \
    } while (0)
