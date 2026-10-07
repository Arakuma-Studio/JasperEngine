// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_EXPORT_H
#define JASPERENGINE_EXPORT_H

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

// Kept out of the shared engine's dynamic symbol table: internal types built on third-party
// C++ code, such as the RmlUi interfaces in core/htmlui (only the engine itself uses them)
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
    #define JASPERENGINE_INTERNAL __attribute__((visibility("hidden")))
#else
    #define JASPERENGINE_INTERNAL
#endif

#endif // JASPERENGINE_EXPORT_H