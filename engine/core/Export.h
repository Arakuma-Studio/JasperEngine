// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_EXPORT_H
#define JASPERENGINE_EXPORT_H

// Every engine and Play plugin translation unit gets here (through JasperEngine.h), so a build
// at an older standard fails with this one message. MSVC keeps __cplusplus at 199711L unless
// /Zc:__cplusplus is given; _MSVC_LANG is always right.
#ifdef __cplusplus
    #if defined(_MSVC_LANG)
        #define JASPERENGINE_CPLUSPLUS _MSVC_LANG
    #else
        #define JASPERENGINE_CPLUSPLUS __cplusplus
    #endif
    static_assert(JASPERENGINE_CPLUSPLUS >= 202002L, "JasperEngine requires C++20");
#endif

#if defined(_WIN32)
    #ifdef JASPERENGINE_SHARED
        #ifdef JASPERENGINE_EXPORTS
            #define JASPERENGINE_API __declspec(dllexport)
        #else
            #define JASPERENGINE_API __declspec(dllimport)
        #endif
    #else
        #define JASPERENGINE_API
    #endif
#else
    #define JASPERENGINE_API
#endif

// The optional modules a player variant or a Compile build may leave out (docs/specs/
// S34-player-size.md R1): CMake defines each 0 or 1 for every target that links the engine; a
// build that defines none of them (the Xcode workspace) has them all
#ifndef JASPERENGINE_HTMLUI
    #define JASPERENGINE_HTMLUI 1
#endif
#ifndef JASPERENGINE_VIDEO
    #define JASPERENGINE_VIDEO JASPERENGINE_HTMLUI
#endif

// Kept out of the shared engine's dynamic symbol table: internal types built on third-party
// C++ code, such as the RmlUi interfaces in core/htmlui (only the engine itself uses them)
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
    #define JASPERENGINE_INTERNAL __attribute__((visibility("hidden")))
#else
    #define JASPERENGINE_INTERNAL
#endif

#endif // JASPERENGINE_EXPORT_H