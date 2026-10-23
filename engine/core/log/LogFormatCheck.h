// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// printf format checking of the logging functions: GCC/Clang check the call sites with
// -Wformat, MSVC with its SAL annotation (/analyze)
#if defined(__GNUC__) || defined(__clang__)
    #define JE_PRINTF_FORMAT(formatIndex, firstArg) __attribute__((format(printf, formatIndex, firstArg)))
#else
    #define JE_PRINTF_FORMAT(formatIndex, firstArg)
#endif
#if defined(_MSC_VER)
    #include <sal.h>
    #define JE_FORMAT_STRING _Printf_format_string_
#else
    #define JE_FORMAT_STRING
#endif
