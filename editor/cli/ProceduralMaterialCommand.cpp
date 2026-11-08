// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ProceduralMaterialCommand.h"

#include "CallCommand.h"
#include "CommandLine.h"

#include "json.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

using namespace jasperengine::editor::cli;

namespace fs = std::filesystem;

namespace {

    // `call`'s codes. Only "no editor could start" bakes in this process on the CPU tier, never a timeout, an
    // interrupt or a lost connection, after which the editor may have written the files
    constexpr int EXIT_USAGE = 2;
    constexpr int EXIT_CONNECTION = 3;

    void printUsage(const std::string& exe) {
        std::cout
            << "Usage:\n"
            << "  " << exe << " procedural-material bake <file.procmat> [options]\n\n"
            << "Bakes a Procedural Material to PNGs and a .material (bake_procedural_material) in the editor that has its\n"
            << "project open, else in a one-shot headless editor.\n\n"
            << "Options:\n"
            << "  --output <folder>     Project-relative folder (default: a folder named after the graph beside it).\n"
            << "  --size <n>            A power of two from 64 to 8192 (default 1024).\n"
            << "  --seed <n>            The seed (default: the graph's metadata.seed, else 0).\n"
            << "  --outputs <a,b,...>   base_color, normal, orm, emission, height (default: every connected one).\n"
            << "  --fail-on fail|warn   Write nothing when the material's checks reach it.\n"
            << "  --overwrite           Replace PNGs that are not this graph's.\n"
            << "  --timeout <seconds>   How long the bake may take (default 600).\n"
            << "  -h, --help            Show this help.\n\n"
            << "When no editor can start (no display, no Xvfb), it bakes in this process on the CPU tier (slower).\n\n"
            << "Exit codes: 0 baked; 1 the bake failed (a bad graph, a failed evaluation, fail_on reached); 2 usage;\n"
            << "3 the editor's connection was lost, timed out or interrupted (Ctrl-C), or another editor holds the project;\n"
            << "4 internal error.\n";
    }

    // the directory holding project.yaml at or above the file
    fs::path projectOf(const fs::path& file) {
        std::error_code ec;
        for (fs::path dir = file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
            if (fs::is_regular_file(dir / "project.yaml", ec)) return dir;
            if (dir == dir.parent_path()) break;
        }
        return fs::path();
    }

    bool wholeNumber(const std::string& text, long long low, long long high, long long& value) {
        try {
            size_t used = 0;
            value = std::stoll(text, &used);
            return used == text.size() && value >= low && value <= high;
        } catch (const std::exception&) {
            return false;
        }
    }

}

int ProceduralMaterialCommand::run(int argc, char** argv, const char* executableName) {
    const std::string exe = fs::path(executableName ? executableName : "jasperengine-editor").filename().string();
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        printUsage(exe);
        return args.empty() ? EXIT_USAGE : 0;
    }
    if (args[0] != "bake") {
        std::cerr << exe << " procedural-material: unknown action '" << args[0] << "' (bake)\n";
        return EXIT_USAGE;
    }
    nlohmann::json tool = nlohmann::json::object();
    std::string file;
    double timeout = 600.0;
    const auto usage = [&](const std::string& message) {
        std::cerr << exe << " procedural-material bake: " << message << "\n\n";
        printUsage(exe);
        return EXIT_USAGE;
    };
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& arg = args[i];
        const auto value = [&](std::string& out) {
            if (i + 1 >= args.size()) return false;
            out = args[++i];
            return true;
        };
        std::string text;
        long long number = 0;
        if (arg == "-h" || arg == "--help") {
            printUsage(exe);
            return 0;
        } else if (arg == "--output") {
            if (!value(text)) return usage("--output needs a folder");
            tool["output"] = text;
        } else if (arg == "--size") {
            if (!value(text) || !wholeNumber(text, 64, 8192, number) || (number & (number - 1)) != 0) return usage("--size is a power of two from 64 to 8192");
            tool["size"] = number;
        } else if (arg == "--seed") {
            if (!value(text) || !wholeNumber(text, 0, 2147483647, number)) return usage("--seed is a whole number from 0");
            tool["seed"] = number;
        } else if (arg == "--outputs") {
            if (!value(text)) return usage("--outputs needs a list");
            nlohmann::json list = nlohmann::json::array();
            for (size_t start = 0; start <= text.size();) {
                const size_t end = std::min(text.find(',', start), text.size());
                if (end > start) list.push_back(text.substr(start, end - start));
                start = end + 1;
            }
            tool["outputs"] = list;
        } else if (arg == "--fail-on") {
            if (!value(text) || (text != "fail" && text != "warn")) return usage("--fail-on is fail or warn");
            tool["fail_on"] = text;
        } else if (arg == "--overwrite") {
            tool["overwrite"] = true;
        } else if (arg == "--timeout") {
            if (!value(text) || !wholeNumber(text, 1, 3600, number)) return usage("--timeout is 1 to 3600 seconds");
            timeout = (double)number;
        } else if (!arg.empty() && arg[0] == '-') {
            return usage("unknown option " + arg);
        } else if (file.empty()) {
            file = arg;
        } else {
            return usage("one .procmat at a time (also got " + arg + ")");
        }
    }
    if (file.empty()) return usage("which .procmat?");
    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(fs::absolute(file, ec), ec);
    if (!fs::is_regular_file(absolute, ec)) return usage(file + " does not exist");
    const fs::path project = projectOf(absolute);
    if (project.empty()) return usage(file + " is not inside a project (no project.yaml above it)");
    tool["path"] = absolute.lexically_relative(project).generic_string();
    tool["timeout_seconds"] = timeout;
    tool["wait"] = true;

    // `call` does the rest: the running editor, else a one-shot one, and its exit codes
    std::vector<std::string> call = {"call", "--project", project.string(), "--timeout", std::to_string((int)timeout + 60), "--wait-job",
                                     "bake_procedural_material", tool.dump()};
    std::vector<char*> callArgv;
    for (std::string& part : call) callArgv.push_back(part.data());
    callArgv.push_back(nullptr);
    const int code = CallCommand::run((int)call.size(), callArgv.data(), executableName);
    // R39: no editor at all (nothing ran in one): the same bake here, on tier 3, with no window
    if (code == EXIT_CONNECTION && CallCommand::spawnFailed()) return jasperengine::editor::CommandLine::bakeProceduralMaterialOnCpu(argc, argv, project, tool, exe);
    return code;
}
