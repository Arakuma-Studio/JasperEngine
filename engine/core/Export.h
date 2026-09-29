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

#endif // JASPERENGINE_EXPORT_H