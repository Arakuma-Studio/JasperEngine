// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Export.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace jasperengine {

    enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error, Fatal, Off };

    // "trace" ... "fatal", "off"
    JASPERENGINE_API const char* logLevelName(LogLevel level);
    // Also accepts "warning", "err", "information", "verbose" and the MCP/syslog names
    // ("notice", "critical", "alert", "emergency"). False for anything else.
    JASPERENGINE_API bool parseLogLevel(const char* name, LogLevel& out);

    // One log category. The registry owns them (Logger::registerCategory, at most
    // MAX_CATEGORIES); effective is the gate every record of the category passes: the
    // lowest level that reaches a sink, never below the category's own level.
    struct LogCategory {
        static constexpr size_t MAX_CATEGORIES = 64;

        const char* name;
        uint8_t index;
        std::atomic<uint8_t> effective;
    };

    // The built-in categories (one accessor each, so the state lives in the engine library)
    JASPERENGINE_API LogCategory& logCategoryEngine();   // Log::*
    JASPERENGINE_API LogCategory& logCategoryEditor();   // Out::*
    JASPERENGINE_API LogCategory& logCategoryBuild();    // Out::build: compiler, CMake and shader builds
    JASPERENGINE_API LogCategory& logCategoryLua();      // Lua print, Log.* and Lua errors
    JASPERENGINE_API LogCategory& logCategoryScript();   // C++ scripts and script lifecycle
    JASPERENGINE_API LogCategory& logCategoryRender();
    JASPERENGINE_API LogCategory& logCategoryShader();
    JASPERENGINE_API LogCategory& logCategoryPhysics();
    JASPERENGINE_API LogCategory& logCategoryAudio();
    JASPERENGINE_API LogCategory& logCategoryAsset();
    JASPERENGINE_API LogCategory& logCategoryHtmlUI();
    JASPERENGINE_API LogCategory& logCategoryIO();
    JASPERENGINE_API LogCategory& logCategoryMcp();

}
