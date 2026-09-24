#include "BmpWriter.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace
{

// BMP layout constants. They are file-format constants, not mutable state.
const unsigned int kFileHeaderSize = 14;
const unsigned int kInfoHeaderSize = 40;
const unsigned int kPaletteEntries = 256;
const unsigned int kPaletteEntrySize = 4;
const unsigned int kPaletteSize = kPaletteEntries * kPaletteEntrySize;
const unsigned int kPixelDataOffset = kFileHeaderSize + kInfoHeaderSize + kPaletteSize;

// Little-endian field writers: the BMP format is little-endian regardless of
// the host, so the fields are packed byte by byte instead of being memcpy'd out
// of a struct (which would also depend on struct padding).
void PutU16(unsigned char* destination, unsigned int value)
{
    destination[0] = static_cast<unsigned char>(value & 0xFF);
    destination[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
}

void PutU32(unsigned char* destination, unsigned long long value)
{
    destination[0] = static_cast<unsigned char>(value & 0xFF);
    destination[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    destination[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
    destination[3] = static_cast<unsigned char>((value >> 24) & 0xFF);
}

void PutI32(unsigned char* destination, int value)
{
    PutU32(destination, static_cast<unsigned long long>(static_cast<unsigned int>(value)));
}

#if defined(_WIN32)
// Windows: open the wide path with the Unicode CRT entry point so that folders
// and file names outside the ANSI code page work.
std::FILE* OpenBinaryForWrite(const std::wstring& path)
{
    std::FILE* file = 0;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0)
    {
        return 0;
    }
    return file;
}
#else
// Fallback for non-Windows builds: the byte-oriented CRT entry point is used
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

std::FILE* OpenBinaryForWrite(const std::wstring& path)
{
    return std::fopen(NarrowPath(path).c_str(), "wb");
}
#endif

// Reports the size of an existing file, used to verify that the write really
// produced the file size advertised in the BMP header.
#if defined(_WIN32)
bool FileSize(const std::wstring& path, unsigned long long& size)
{
    struct _stat64 status;
    std::memset(&status, 0, sizeof(status));
    if (::_wstat64(path.c_str(), &status) != 0)
    {
        return false;
    }
    if (status.st_size < 0)
    {
        return false;
    }
    size = static_cast<unsigned long long>(status.st_size);
    return true;
}
#else
bool FileSize(const std::wstring& path, unsigned long long& size)
{
    struct stat status;
    std::memset(&status, 0, sizeof(status));
    if (::stat(NarrowPath(path).c_str(), &status) != 0)
    {
        return false;
    }
    if (status.st_size < 0)
    {
        return false;
    }
    size = static_cast<unsigned long long>(status.st_size);
    return true;
}
#endif

} // namespace

namespace grablinkcore
{

BmpWriteResult::BmpWriteResult()
    : success(false), error(), fileSize(0), bytesWritten(0)
{
}

BmpWriteResult BmpWriter::WriteGrayscale8(const std::wstring& path,
                                          const unsigned char* pixels,
                                          int width,
                                          int height,
                                          int pitch)
{
    BmpWriteResult result;

    if (pixels == 0)
    {
        result.error = "BmpWriter: pixel source is null";
        return result;
    }

    if (width <= 0)
    {
        result.error = "BmpWriter: width must be positive";
        return result;
    }

    if (height <= 0)
    {
        result.error = "BmpWriter: height must be positive";
        return result;
    }

    if (pitch < width)
    {
        result.error = "BmpWriter: source pitch is narrower than the image width";
        return result;
    }

    // BMP rows are always padded to a multiple of 4 bytes, whatever the pixel
    // count is: ((width * 8) + 31) / 32 * 4 == ((width + 3) / 4) * 4.
    const unsigned long long rowBytes = ((static_cast<unsigned long long>(width) * 8ULL + 31ULL) / 32ULL) * 4ULL;
    const unsigned long long imageSize = rowBytes * static_cast<unsigned long long>(height);
    const unsigned long long fileSize = kPixelDataOffset + imageSize;

    // bfSize and biSizeImage are 32-bit fields: an image that does not fit in a
    // DWORD is refused instead of being truncated into a corrupt header.
    if (fileSize > 0xFFFFFFFFULL)
    {
        result.error = "BmpWriter: the image is too large for the DWORD size fields of a BMP";
        return result;
    }

    // --- header + palette -------------------------------------------------
    unsigned char header[kPixelDataOffset];
    std::memset(header, 0, sizeof(header));

    header[0] = 'B';
    header[1] = 'M';
    PutU32(header + 2, fileSize);                          // bfSize
    PutU32(header + 10, kPixelDataOffset);                 // bfOffBits
    PutU32(header + 14, kInfoHeaderSize);                  // biSize
    PutI32(header + 18, width);                            // biWidth
    PutI32(header + 22, -height);                          // biHeight: top-down
    PutU16(header + 26, 1);                                // biPlanes
    PutU16(header + 28, 8);                                // biBitCount
    PutU32(header + 30, 0);                                // biCompression: BI_RGB
    PutU32(header + 34, imageSize);                        // biSizeImage
    PutU32(header + 38, 0);                                // biXPelsPerMeter
    PutU32(header + 42, 0);                                // biYPelsPerMeter
    PutU32(header + 46, 0);                                // biClrUsed
    PutU32(header + 50, 0);                                // biClrImportant

    const unsigned int paletteOffset = kPixelDataOffset - kPaletteSize;
    for (unsigned int index = 0; index < kPaletteEntries; ++index)
    {
        unsigned char* entry = header + paletteOffset + index * kPaletteEntrySize;
        entry[0] = static_cast<unsigned char>(index);      // blue
        entry[1] = static_cast<unsigned char>(index);      // green
        entry[2] = static_cast<unsigned char>(index);      // red
        entry[3] = 0;                                      // reserved
    }

    // --- pixel data -------------------------------------------------------
    std::FILE* file = OpenBinaryForWrite(path);
    if (file == 0)
    {
        result.error = "BmpWriter: the output file could not be opened for writing";
        return result;
    }

    bool writeFailed = false;
    unsigned long long written = 0;

    if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header))
    {
        writeFailed = true;
    }
    else
    {
        written += kPixelDataOffset;
    }

    // One padded row buffer reused for every row: the useful bytes are taken
    // from the source (pitch bytes apart) and the padding stays zero.
    std::vector<unsigned char> row;
    try
    {
        row.assign(static_cast<std::size_t>(rowBytes), 0);
    }
    catch (const std::bad_alloc&)
    {
        std::fclose(file);
        result.error = "BmpWriter: no memory for the padded row buffer";
        return result;
    }

    for (int index = 0; index < height && !writeFailed; ++index)
    {
        const unsigned char* rowSource = pixels +
            static_cast<std::size_t>(index) * static_cast<std::size_t>(pitch);
        std::memcpy(&row[0], rowSource, static_cast<std::size_t>(width));
        if (std::fwrite(&row[0], 1, row.size(), file) != row.size())
        {
            writeFailed = true;
        }
        else
        {
            written += row.size();
        }
    }

    // Short writes may only become visible when the buffers are flushed or the
    // file is closed, so all three steps are checked.
    if (std::fflush(file) != 0)
    {
        writeFailed = true;
    }

    if (std::fclose(file) != 0)
    {
        writeFailed = true;
    }

    if (writeFailed)
    {
        result.error = "BmpWriter: writing the BMP failed (disk full or file system error)";
        return result;
    }

    // The header promises fileSize bytes: verify the file really is that big so
    // a silently truncated write cannot be reported as a success.
    unsigned long long actualSize = 0;
    if (!FileSize(path, actualSize) || actualSize != fileSize)
    {
        result.error = "BmpWriter: the written file size does not match the BMP header";
        return result;
    }

    result.success = true;
    result.fileSize = fileSize;
    result.bytesWritten = written;
    return result;
}

} // namespace grablinkcore
