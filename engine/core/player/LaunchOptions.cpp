// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LaunchOptions.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::player;

namespace {

    // the engine's own flags (Engine::systemInit, the profiler): accepted, left to the engine
    const char* const ENGINE_VALUE_FLAGS[] = {"--log-level", "--profile-capture", "--profile-frames"};
    const char* const ENGINE_FLAGS[] = {"--profile"};

    bool isEngineValueFlag(const std::string& name) {
        for (const char* flag : ENGINE_VALUE_FLAGS)
            if (name == flag) return true;
        return false;
    }

    bool isEngineFlag(const std::string& name) {
        for (const char* flag : ENGINE_FLAGS)
            if (name == flag) return true;
        return false;
    }

    bool writableDirectory(const std::filesystem::path& file, std::string& problem) {
        std::filesystem::path dir = file.parent_path();
        if (dir.empty()) dir = ".";
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            problem = "its directory " + dir.string() + " does not exist";
            return false;
        }
        const std::filesystem::perms perms = std::filesystem::status(dir, ec).permissions();
        if (ec || (perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none) {
            problem = "its directory " + dir.string() + " is not writable";
            return false;
        }
        return true;
    }

}

bool jasperengine::player::parseUint64(const std::string& text, uint64_t& out) {
    if (text.empty() || text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    out = static_cast<uint64_t>(value);
    return true;
}

bool jasperengine::player::parseDouble(const std::string& text, double& out) {
    if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return false;
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

std::string jasperengine::player::usage() {
    return
        "Options:\n"
        "  --headless              never show the window, no audio output, no input; renders offscreen\n"
        "  --quit-after N          exit after N drawn frames (N >= 1)\n"
        "  --after-ready           count frames from the one that logs \"Engine ready:\"\n"
        "  --ready-timeout S       with --after-ready: give up after S seconds (exit 6; default 120)\n"
        "  --fixed-fps F           a fixed frame time of 1/F s (F > 0); headless: no vsync, no sleeps\n"
        "  --screenshot PATH       write a PNG of the final frame (exit 4 when it fails)\n"
        "  --screenshot-frame K    the frame of the screenshot (default: the last; K <= --quit-after)\n"
        "  --resolution WxH        the window size\n"
        "  --scene NAME|ID         start in this scene\n"
        "  --silent-audio          open no audio output\n"
        "  --audio-capture <dir>   a development player: write an audio capture session into <dir>\n"
        "  --content DIR           read the game's content from DIR\n"
        "  --log-level SPEC        the log level (\"debug\", \"warn,render=debug\")\n"
        "  --verify-content        check every content file's SHA-256 before starting (exit 5)\n"
        "  --fail-on-error         exit 3 when an error is logged\n"
        "  -- ARGS...              passed to the game's Lua (System.args)\n";
}

std::vector<std::string> jasperengine::player::argsFromQuery(const std::string& query) {
    std::vector<std::string> args;
    size_t start = (!query.empty() && query[0] == '?') ? 1 : 0;
    while (start < query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.size();
        std::string item = query.substr(start, end - start);
        start = end + 1;
        if (item.empty()) continue;
        // percent-decoding of the few characters a value needs (a path, a scene name)
        std::string decoded;
        for (size_t i = 0; i < item.size(); i++) {
            if (item[i] == '+') {
                decoded += ' ';
            } else if (item[i] == '%' && i + 2 < item.size() && std::isxdigit((unsigned char)item[i + 1]) &&
                       std::isxdigit((unsigned char)item[i + 2])) {
                decoded += static_cast<char>(std::stoi(item.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                decoded += item[i];
            }
        }
        args.push_back("--" + decoded);
    }
    return args;
}

std::variant<LaunchOptions, ParseError> jasperengine::player::parse(std::span<const std::string> args, ParseMode mode) {
    LaunchOptions options;
    bool quitAfterGiven = false;
    bool screenshotFrameGiven = false;

    auto fail = [](const std::string& message) { return ParseError{message}; };

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        if (arg == "--") {
            for (size_t j = i + 1; j < args.size(); j++) options.scriptArgs.push_back(args[j]);
            break;
        }
        if (arg.rfind("--", 0) != 0) {
            if (mode == ParseMode::Strict)
                return fail("unexpected argument \"" + arg + "\": game arguments go after \"--\" (--quit-after 60 -- " + arg + ")");
            options.scriptArgs.push_back(arg);
            continue;
        }

        std::string name = arg;
        std::string value;
        bool hasInlineValue = false;
        const size_t equals = arg.find('=');
        if (equals != std::string::npos) {
            name = arg.substr(0, equals);
            value = arg.substr(equals + 1);
            hasInlineValue = true;
        }
        auto takeValue = [&](const char* example) -> std::variant<std::string, ParseError> {
            if (hasInlineValue) return value;
            if (i + 1 >= args.size())
                return fail(name + " needs a value, for example " + name + " " + example);
            return args[++i];
        };
        auto noValue = [&]() -> bool { return !hasInlineValue; };

        if (name == "--headless" || name == "--after-ready" || name == "--silent-audio" || name == "--fail-on-error" ||
            name == "--verify-content") {
            if (!noValue()) return fail(name + " takes no value");
            if (name == "--headless") options.headless = true;
            else if (name == "--after-ready") options.afterReady = true;
            else if (name == "--silent-audio") options.silentAudio = true;
            else if (name == "--fail-on-error") options.failOnError = true;
            else options.verifyContent = true;
        } else if (name == "--quit-after") {
            auto taken = takeValue("600");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            if (!parseUint64(text, options.quitAfter) || options.quitAfter < 1)
                return fail("--quit-after needs a whole number of frames of at least 1, for example --quit-after 600 (got \"" + text + "\")");
            quitAfterGiven = true;
        } else if (name == "--screenshot-frame") {
            auto taken = takeValue("30");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            if (!parseUint64(text, options.screenshotFrame) || options.screenshotFrame < 1)
                return fail("--screenshot-frame needs a whole frame number of at least 1, for example --screenshot-frame 30 (got \"" + text + "\")");
            screenshotFrameGiven = true;
        } else if (name == "--fixed-fps") {
            auto taken = takeValue("60");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            if (!parseDouble(text, options.fixedFps) || !std::isfinite(options.fixedFps) || options.fixedFps <= 0.0)
                return fail("--fixed-fps needs a finite rate above 0, for example --fixed-fps 60 (got \"" + text + "\")");
        } else if (name == "--ready-timeout") {
            auto taken = takeValue("120");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            if (!parseDouble(text, options.readyTimeout) || !std::isfinite(options.readyTimeout) || options.readyTimeout <= 0.0)
                return fail("--ready-timeout needs a finite number of seconds above 0, for example --ready-timeout 120 (got \"" + text + "\")");
        } else if (name == "--audio-capture") {
            auto taken = takeValue("captures");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
#if JASPERENGINE_AUDIO_CAPTURE
            const std::string text = std::get<std::string>(taken);
            if (text.empty()) return fail("--audio-capture needs a directory, for example --audio-capture captures");
            options.audioCapture = text;
#else
            return fail("--audio-capture needs a development player");
#endif
        } else if (name == "--screenshot") {
            auto taken = takeValue("game.png");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            if (text.empty()) return fail("--screenshot needs a file name, for example --screenshot game.png");
            options.screenshot = text;
            std::string problem;
            if (!writableDirectory(options.screenshot, problem))
                return fail("--screenshot " + text + ": " + problem + "; create it or write the PNG elsewhere");
        } else if (name == "--resolution") {
            auto taken = takeValue("1280x720");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            const size_t x = text.find('x');
            uint64_t w = 0, h = 0;
            if (x == std::string::npos || !parseUint64(text.substr(0, x), w) || !parseUint64(text.substr(x + 1), h) ||
                w < 1 || h < 1 || w > 16384 || h > 16384)
                return fail("--resolution needs WIDTHxHEIGHT between 1 and 16384, for example --resolution 1280x720 (got \"" + text + "\")");
            options.width = static_cast<int>(w);
            options.height = static_cast<int>(h);
        } else if (name == "--scene") {
            auto taken = takeValue("Main");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            options.scene = std::get<std::string>(taken);
            if (options.scene.empty()) return fail("--scene needs a scene name or id, for example --scene Main");
        } else if (name == "--content") {
            auto taken = takeValue("path/to/content");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            std::error_code ec;
            if (text.empty() || !std::filesystem::is_directory(text, ec))
                return fail("--content " + text + ": not a directory; give the folder that holds game.json");
            options.content = text;
        } else if (isEngineValueFlag(name)) {
            auto taken = takeValue(name == "--log-level" ? "debug" : "1");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            if (name == "--log-level") options.logLevel = std::get<std::string>(taken);
        } else if (isEngineFlag(name)) {
            if (!noValue()) return fail(name + " takes no value");
        } else if (name == "--help") {
            return fail(usage());
        } else if (mode == ParseMode::Strict) {
            return fail("unknown option " + name + " (run with --help for the list; game arguments go after \"--\")");
        } else {
            options.scriptArgs.push_back(arg);
        }
    }

    if (screenshotFrameGiven && options.screenshot.empty())
        return fail("--screenshot-frame needs --screenshot PATH");
    if (screenshotFrameGiven && quitAfterGiven && options.screenshotFrame > options.quitAfter)
        return fail("--screenshot-frame " + std::to_string(options.screenshotFrame) + " is after --quit-after " +
                    std::to_string(options.quitAfter) + ": the game quits first; use a frame up to " + std::to_string(options.quitAfter));
    return options;
}
