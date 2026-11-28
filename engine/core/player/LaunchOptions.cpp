// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LaunchOptions.h"

#include "device/DeviceTypes.h"
#include "TestRunnerConfig.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <system_error>

using namespace jasperengine;

namespace jasperengine::player {

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

bool parseUint64(const std::string& text, uint64_t& out) {
    if (text.empty() || text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    out = static_cast<uint64_t>(value);
    return true;
}

bool parseDouble(const std::string& text, double& out) {
    if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return false;
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    out = value;
    return true;
}

std::string usage() {
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
        "  --device-tier TIER      low, medium, high or ultra instead of the GPU's detected tier\n"
        "  --gpu-quirk NAME:on|off force a GPU quirk on or off (repeatable or comma-separated)\n"
        "  --perf-summary PATH     sample gameplay frames from the ready frame; write the summary JSON at quit\n"
        "  --silent-audio          open no audio output\n"
        "  --audio-capture <dir>   a development player: write an audio capture session into <dir>\n"
        "  --content DIR           read the game's content from DIR\n"
        "  --log-level SPEC        the log level (\"debug\", \"warn,render=debug\")\n"
        "  --verify-content        check every content file's SHA-256 before starting (exit 5)\n"
        "  --fail-on-error         exit 3 when an error is logged (ignored with --run-tests)\n"
        "  --input-script PATH     run an input file (.input.json) from the ready frame (implies --after-ready)\n"
        "  --input-shots DIR       where its shot steps go (default data://input-shots/)\n"
        "  --quit-after-input      exit when its events are done (exit 10 failed, 11 diverged with --input-strict)\n"
        "  --input-strict          a checkpoint that differs fails the run\n"
        "  --input-allow-real      real input reaches the game while it runs\n"
        "  --command LINE          run a console command after \"Engine ready:\", one a frame (repeatable;\n"
        "                          development players, or Project Settings > Debug > Console Commands: Always)\n"
        "  --run-tests             a development player: run the content's Play Mode tests (exit 8 failed, 9 none)\n"
        "  --test-filter F         with --run-tests: case ids containing F, or tag:<t>; \",\" = or\n"
        "  --test-results PATH     with --run-tests: write the results JSON there\n"
        "  --keep-test-data        with --run-tests: keep each case's data folder\n"
        "  -- ARGS...              passed to the game's Lua (System.args)\n";
}

bool testRunnerCompiled() {
    return JASPERENGINE_TEST_RUNNER != 0;
}

const char* commandsOffRefusal() {
    return "--command needs a development player or Project Settings > Debug > Console Commands: Always.";
}

std::vector<std::string> argsFromQuery(const std::string& query) {
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

std::variant<LaunchOptions, ParseError> parse(std::span<const std::string> args, ParseMode mode) {
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

        if (name == "--quit-after-input" || name == "--input-strict" || name == "--input-allow-real") {
            if (!noValue()) return fail(name + " takes no value");
            if (name == "--quit-after-input") options.quitAfterInput = true;
            else if (name == "--input-strict") options.inputStrict = true;
            else options.inputAllowReal = true;
        } else if (name == "--input-script" || name == "--input-shots") {
            auto taken = takeValue(name == "--input-script" ? "tests/Input/menu.input.json" : "data://input-shots/");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            if (text.empty()) return fail(name + " needs a path, for example " + name + " tests/Input/menu.input.json");
            if (name == "--input-script") options.inputScript = text;
            else options.inputShots = text;
        } else if (name == "--headless" || name == "--after-ready" || name == "--silent-audio" || name == "--fail-on-error" ||
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
        } else if (name == "--device-tier") {
            auto taken = takeValue("low");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            DeviceTier tier;
            if (!DeviceNames::tierFromName(text.c_str(), tier))
                return fail("--device-tier needs low, medium, high or ultra, for example --device-tier=low (got \"" + text + "\")");
            options.deviceTier = DeviceNames::tier(tier);
        } else if (name == "--run-tests" || name == "--keep-test-data") {
            if (!noValue()) return fail(name + " takes no value");
            if (!testRunnerCompiled()) return fail(NO_TEST_RUNNER);
            if (name == "--run-tests") options.runTests = true;
            else options.keepTestData = true;
        } else if (name == "--test-filter" || name == "--test-results") {
            auto taken = takeValue(name == "--test-filter" ? "tag:smoke" : "results.json");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            if (!testRunnerCompiled()) return fail(NO_TEST_RUNNER);
            const std::string text = std::get<std::string>(taken);
            if (name == "--test-filter") {
                options.testFilter = text;
            } else {
                if (text.empty()) return fail("--test-results needs a file name, for example --test-results results.json");
                options.testResults = text;
                std::string problem;
                if (!writableDirectory(options.testResults, problem))
                    return fail("--test-results " + text + ": " + problem + "; create it or write the results elsewhere");
            }
        } else if (name == "--command") {
            auto taken = takeValue("\"help\"");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string line = std::get<std::string>(taken);
            if (line.find_first_not_of(" \t") == std::string::npos)
                return fail("--command needs a console line, for example --command \"give gold 10\"");
            options.commands.push_back(line);
        } else if (name == "--gpu-quirk") {
            auto taken = takeValue("gl_no_warm_draws:off");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            std::string problem;
            if (!GpuQuirks::parseOverrides(std::get<std::string>(taken), options.gpuQuirks, problem))
                return fail("--gpu-quirk: " + problem);
        } else if (name == "--perf-summary") {
            auto taken = takeValue("perf.json");
            if (auto* error = std::get_if<ParseError>(&taken)) return *error;
            const std::string text = std::get<std::string>(taken);
            if (text.empty()) return fail("--perf-summary needs a file name, for example --perf-summary perf.json");
            options.perfSummary = text;
#if !defined(__EMSCRIPTEN__)
            std::string problem;
            if (!writableDirectory(options.perfSummary, problem))
                return fail("--perf-summary " + text + ": " + problem + "; create it or write the summary elsewhere");
#endif
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

    if (!options.runTests && (!options.testFilter.empty() || !options.testResults.empty() || options.keepTestData))
        return fail("--test-filter, --test-results and --keep-test-data go with --run-tests");
    // a test run counts its frames from the ready frame (R23); --fixed-fps comes from tests.yaml unless given
    if (options.runTests) options.afterReady = true;
    if (screenshotFrameGiven && options.screenshot.empty())
        return fail("--screenshot-frame needs --screenshot PATH");
    if (screenshotFrameGiven && quitAfterGiven && options.screenshotFrame > options.quitAfter)
        return fail("--screenshot-frame " + std::to_string(options.screenshotFrame) + " is after --quit-after " +
                    std::to_string(options.quitAfter) + ": the game quits first; use a frame up to " + std::to_string(options.quitAfter));
    return options;
}

}  // namespace jasperengine::player
