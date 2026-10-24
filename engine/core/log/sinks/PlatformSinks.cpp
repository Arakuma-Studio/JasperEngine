// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "log/LogSinks.h"

#include <memory>

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

#if !defined(_WIN32)
    #include <sys/stat.h>
#else
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#endif

using namespace jasperengine;

namespace je_platform_sinks {}
using namespace je_platform_sinks;

namespace je_platform_sinks {

    void makeDirectory(const std::string& path) {
#if defined(_WIN32)
        const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(length > 0 ? length : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], length);
        CreateDirectoryW(wide.c_str(), nullptr);
#else
        ::mkdir(path.c_str(), 0755);
#endif
    }

}

void jasperengine::installPlatformLogSinks(const PlatformLogOptions& options) {
#if defined(__ANDROID__)
    (void)options;
    Logger::addSink(std::make_unique<AndroidLogSink>());
#elif defined(__EMSCRIPTEN__)
    (void)options;
    Logger::addSink(std::make_unique<EmscriptenConsoleSink>());
#else
    if (options.console) {
        Logger::addSink(std::make_unique<ConsoleSink>(ConsoleSink::Style::Game));
    }
    #if defined(_WIN32)
    Logger::addSink(std::make_unique<DebuggerSink>());
    #endif
    #if defined(__APPLE__)
    Logger::addSink(std::make_unique<AppleLogSink>());
    #endif
    #if !defined(__APPLE__) || !TARGET_OS_IPHONE
    if (options.fileLog && !options.userDataPath.empty()) {
        const std::string directory = options.userDataPath + "/logs";
        makeDirectory(directory);
        RotatingFileSink::Options file;
        file.path = directory + "/player.log";
        file.maxBytes = options.fileBytes;
        file.maxFiles = options.fileCount;
        file.showSource = true;
        auto sink = std::make_unique<RotatingFileSink>(std::move(file));
        if (sink->isOpen()) Logger::addSink(std::move(sink));
    }
    #endif
#endif
}
