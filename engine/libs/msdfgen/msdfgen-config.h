#pragma once

// JasperEngine: the configuration msdfgen's CMake generates (core only, a static library, C++11
// code paths), written once for every build system (CMake, Xcode); core/base.h includes it
// relatively instead of <msdfgen/msdfgen-config.h>.

#define MSDFGEN_PUBLIC
#define MSDFGEN_EXT_PUBLIC

#define MSDFGEN_VERSION 1.13.0
#define MSDFGEN_VERSION_MAJOR 1
#define MSDFGEN_VERSION_MINOR 13
#define MSDFGEN_VERSION_REVISION 0
#define MSDFGEN_COPYRIGHT_YEAR 2025

#ifndef MSDFGEN_USE_CPP11
#define MSDFGEN_USE_CPP11
#endif
