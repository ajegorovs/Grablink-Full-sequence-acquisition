#pragma once

// BmpWriter.h - hardware-independent writer for 8-bpp grayscale BMP files.
//
// The output is a plain Windows bitmap with a 256 entry grayscale palette and a
// top-down image (negative biHeight), so the rows are stored in memory order
// and no vertical flip is needed on the write path.
//
// The path is std::wstring and is passed to the Unicode CRT entry points on
// Windows, so output folders containing non-ASCII characters work.
//
// Error model: writing never throws. Every failure (bad arguments, unopenable
// path, short write, failed close, unexpected file size) is reported through
// BmpWriteResult::success == false plus a human readable BmpWriteResult::error.
//
// Only the C++ standard library is used; there is no MFC and no MultiCam
// dependency, so the writer can be unit tested without hardware.

#include <cstddef>
#include <string>

namespace grablinkcore
{

struct BmpWriteResult
{
    bool success;
    std::string error;
    unsigned long long fileSize;      // size the BMP header declares
    unsigned long long bytesWritten;  // bytes actually handed to the file

    BmpWriteResult();
};

class BmpWriter
{
public:
    // Writes "pixels" (8 bits per pixel, grayscale) as an 8-bpp BMP to "path".
    //
    // "pitch" is the distance in bytes between the start of two consecutive
    // source rows and must be at least "width"; the padding at the end of each
    // source row is ignored. Rows shorter than 4 bytes in the file are padded
    // with zeros as required by the BMP format.
    static BmpWriteResult WriteGrayscale8(const std::wstring& path,
                                          const unsigned char* pixels,
                                          int width,
                                          int height,
                                          int pitch);

private:
    BmpWriter();
};

// Offset of the first pixel byte: 14-byte BITMAPFILEHEADER + 40-byte
// BITMAPINFOHEADER + 256 * 4-byte palette.
const unsigned int kBmp8bppPixelDataOffset = 14 + 40 + 256 * 4;

} // namespace grablinkcore
