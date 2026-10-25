// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef log_h
#define log_h

#include "Export.h"
#include "log/LogFormatCheck.h"

namespace jasperengine {

    // The engine's printf-style log, category "engine" (Logger in log/Logger.h has
    // categories, source locations and the JE_LOG_* macros): print is Info, verbose Trace,
    // debug Debug (compiled out with NDEBUG), warn Warn, error Error.
    class JASPERENGINE_API Log {
    public:
        static void print(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);

        static void verbose(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void debug(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void warn(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
        static void error(JE_FORMAT_STRING const char* text, ...) JE_PRINTF_FORMAT(1, 2);
    };

}

#endif
