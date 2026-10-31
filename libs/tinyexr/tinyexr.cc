// The tinyexr implementation (configured by CMakeLists.txt: stb zlib, no threads, no OpenMP)
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
