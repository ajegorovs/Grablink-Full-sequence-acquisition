#pragma once
//#include <Windows.h>
#include <stdio.h>
#include "StdAfx.h"

class BmpHelper
{
public:
    static void SaveTo8bppBmpFile(FILE *output, LONG width, LONG height, unsigned char *buffer);
    static void Init8bppHeaders();

private:
    static LONG s8bppHeader_;
    static BITMAPFILEHEADER f8bppHeader_;
    static BITMAPINFOHEADER i8bppHeader_;
    static RGBQUAD p8bpp[256];

    BmpHelper();
    ~BmpHelper();
    BmpHelper(const BmpHelper &);
    BmpHelper &operator=(const BmpHelper &);
};
