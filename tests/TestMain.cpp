// TestMain.cpp - entry point of the hardware-free core test executable.
//
// The executable only needs the C++ runtime and Win32; it never initialises
// MFC or the MultiCam driver.

#include "TestHarness.h"

#include <iostream>

int main()
{
    std::cout << "GrablinkSnapshot core tests (hardware-free)" << std::endl;
    return test::RunAll();
}
