#include "StdAfx.h"
#include "BmpHelper.h"

LONG BmpHelper::s8bppHeader_;
BITMAPFILEHEADER BmpHelper::f8bppHeader_;
BITMAPINFOHEADER BmpHelper::i8bppHeader_;
RGBQUAD BmpHelper::p8bpp[256];

void BmpHelper::Init8bppHeaders()
{
    f8bppHeader_.bfType = 'MB'; // Will be inverted during the fwrite
    s8bppHeader_ = (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD) + 256 * sizeof(RGBQUAD));
    f8bppHeader_.bfReserved1 = 0;
    f8bppHeader_.bfReserved2 = 0;
    f8bppHeader_.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD);

    i8bppHeader_.biSize = sizeof(BITMAPINFOHEADER);
    i8bppHeader_.biPlanes = 1;
    i8bppHeader_.biBitCount = 8;
    i8bppHeader_.biCompression = BI_RGB;
    i8bppHeader_.biSizeImage = 0;
    i8bppHeader_.biXPelsPerMeter = 0;
    i8bppHeader_.biYPelsPerMeter = 0;
    i8bppHeader_.biClrUsed = 0;
    i8bppHeader_.biClrImportant = 0;

    for (size_t index = 0; index < 256; ++index)
    {
        p8bpp[index].rgbBlue = (BYTE)index;
        p8bpp[index].rgbGreen = (BYTE)index;
        p8bpp[index].rgbRed = (BYTE)index;
        p8bpp[index].rgbReserved = 0;
    }
}

void BmpHelper::SaveTo8bppBmpFile(FILE *output, LONG width, LONG height, unsigned char *buffer)
{
    f8bppHeader_.bfSize = (DWORD)(s8bppHeader_ + width * height);

    i8bppHeader_.biWidth = (LONG)width;
    i8bppHeader_.biHeight = -(LONG)height;

    fwrite(&f8bppHeader_, sizeof(BITMAPFILEHEADER), 1, output);
    fwrite(&i8bppHeader_, sizeof(BITMAPINFOHEADER), 1, output);
    fwrite(p8bpp, sizeof(RGBQUAD), 256, output);
    fwrite(buffer, width, height, output);
}
