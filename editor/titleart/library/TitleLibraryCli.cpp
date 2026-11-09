// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibraryCli.h"

#include "PachislopPort.h"
#include "TitleLibraryApply.h"
#include "TitleLibraryCliTools.h"
#include "TitleLibraryFormats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace tl = jasperengine::editor::titlelibrary;

namespace {

    constexpr int EXIT_OK = 0;
    constexpr int EXIT_FINDINGS = 1;
    constexpr int EXIT_USAGE = 2;

    struct Args {
        std::vector<std::string> positional;
        std::vector<std::pair<std::string, std::string>> options;     // --name value ("" for flags)
        bool has(const char* name) const {
            return std::any_of(options.begin(), options.end(), [&](const auto& o) { return o.first == name; });
        }
        std::string get(const char* name, const std::string& fallback = {}) const {
            for (const auto& o : options) {
                if (o.first == name) return o.second;
            }
            return fallback;
        }
    };

    // `--name value` for the names in `valued`, `--name` alone for the rest
    bool parseArgs(int argc, char** argv, const std::vector<std::string>& valued, Args& out, std::string& error) {
        for (int i = 0; i < argc; i++) {
            const std::string a = argv[i];
            if (a.rfind("--", 0) == 0) {
                if (std::find(valued.begin(), valued.end(), a) != valued.end()) {
                    if (i + 1 >= argc) {
                        error = a + " needs a value";
                        return false;
                    }
                    out.options.push_back({a, argv[++i]});
                } else {
                    out.options.push_back({a, ""});
                }
            } else {
                out.positional.push_back(a);
            }
        }
        return true;
    }

    bool readText(const fs::path& path, std::string& text) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
        return true;
    }

    bool writeText(const fs::path& path, const std::string& text, std::string& error) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = path.string() + ": cannot be written";
            return false;
        }
        out << text;
        return (bool)out;
    }

    // every .titlelook / .titletemplate under `dir`, sorted
    std::vector<fs::path> libraryFiles(const fs::path& dir) {
        std::vector<fs::path> out;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            if (ext == tl::TITLE_LOOK_EXTENSION || ext == tl::TITLE_TEMPLATE_EXTENSION) out.push_back(it->path());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    // normalize: rewrites (or with `check` only lists) the files that are not canonical;
    // returns the count of findings, -1 on an I/O error
    int normalizeDir(const fs::path& dir, bool check, std::ostream& report) {
        int findings = 0;
        for (const fs::path& file : libraryFiles(dir)) {
            std::string text, canonical, error;
            if (!readText(file, text)) {
                std::cerr << file.string() << ": cannot be read\n";
                return -1;
            }
            if (!tl::canonicalLibraryText(file.filename().string(), text, canonical, &error)) {
                report << file.string() << ": does not parse: " << error << "\n";
                findings++;
                continue;
            }
            if (canonical == text) continue;
            if (check) {
                report << file.string() << ": not canonical\n";
                findings++;
            } else if (!writeText(file, canonical, error)) {
                std::cerr << error << "\n";
                return -1;
            }
        }
        return findings;
    }

    int runNormalize(int argc, char** argv, const char* exe) {
        Args args;
        std::string error;
        if (!parseArgs(argc, argv, {}, args, error) || args.positional.size() != 1) {
            std::cerr << (error.empty() ? "" : error + "\n") << "usage: " << exe << " title-library normalize [--check] <dir>\n";
            return EXIT_USAGE;
        }
        const fs::path dir = args.positional[0];
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            std::cerr << dir.string() << ": not a directory\n";
            return EXIT_USAGE;
        }
        const int findings = normalizeDir(dir, args.has("--check"), std::cout);
        if (findings < 0) return EXIT_USAGE;
        if (findings > 0) {
            std::cout << findings << (args.has("--check") ? " file(s) not canonical\n" : " file(s) do not parse\n");
            return EXIT_FINDINGS;
        }
        std::cout << "ok: " << libraryFiles(dir).size() << " library file(s) canonical\n";
        return EXIT_OK;
    }

    std::string portReport(const tl::PortResult& r) {
        std::ostringstream out;
        out << "Pachislop port (" << tl::PACHISLOP_COMMIT << "): " << r.looks << " looks, " << r.templates << " templates, " << r.colorSchemes
            << " color schemes, " << r.errors.size() << " error(s)\n";
        for (const std::string& e : r.errors) out << "error: " << e << "\n";
        for (const std::string& n : r.notes) out << n << "\n";
        return out.str();
    }

    int runPortPachislop(int argc, char** argv, const char* exe) {
        Args args;
        std::string error;
        if (!parseArgs(argc, argv, {"--dump", "--out", "--repo"}, args, error) || !args.positional.empty() || !args.has("--dump") ||
            (!args.has("--out") && !args.has("--check"))) {
            std::cerr << (error.empty() ? "" : error + "\n") << "usage: " << exe
                      << " title-library port-pachislop --dump <json> --out <dir> [--repo <dir>] [--check]\n";
            return EXIT_USAGE;
        }
        const fs::path repo = args.get("--repo", fs::current_path().string());
        std::string dumpText;
        if (!readText(args.get("--dump"), dumpText)) {
            std::cerr << args.get("--dump") << ": cannot be read\n";
            return EXIT_USAGE;
        }
        const tl::Json dump = tl::Json::parse(dumpText, nullptr, false);
        if (dump.is_discarded()) {
            std::cerr << args.get("--dump") << ": not JSON\n";
            return EXIT_USAGE;
        }
        tl::PortOptions options;
        if (!tl::loadPortOptions(repo, options, &error)) {
            std::cerr << error << "\n";
            return EXIT_USAGE;
        }
        const tl::PortResult result = tl::portDump(dump, options);
        const std::string report = portReport(result);

        const bool check = args.has("--check");
        fs::path out = args.get("--out");
        if (check) {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            out = fs::temp_directory_path() / ("jasper-pachislop-port-" + std::to_string((long long)stamp));
        }
        for (const auto& [path, text] : result.files) {
            if (!writeText(out / path, text, error)) {
                std::cerr << error << "\n";
                return EXIT_USAGE;
            }
        }
        if (!writeText(out / "pachislop-port-report.txt", report, error)) {
            std::cerr << error << "\n";
            return EXIT_USAGE;
        }
        for (const std::string& e : result.errors) std::cout << "error: " << e << "\n";
        if (!check) {
            std::cout << "ported " << result.looks << " looks, " << result.templates << " templates, " << result.colorSchemes << " color schemes into "
                      << out.string() << " (report: pachislop-port-report.txt)\n";
            return result.ok ? EXIT_OK : EXIT_FINDINGS;
        }

        // --check: the port, then normalize, then byte for byte against the committed files
        std::ostringstream normalizeReport;
        const int bad = normalizeDir(out, false, normalizeReport);
        if (bad != 0) std::cout << normalizeReport.str();
        std::map<std::string, std::string> files;
        for (const auto& entry : result.files) {
            std::string text;
            if (!readText(out / entry.first, text)) {
                std::cerr << (out / entry.first).string() << ": cannot be read back\n";
                return EXIT_USAGE;
            }
            files[entry.first] = text;
        }
        const std::vector<tl::PortFinding> findings = tl::comparePortFiles(files, repo);
        for (const tl::PortFinding& f : findings) std::cout << f.what << ": " << f.path << (f.detail.empty() ? "" : " (" + f.detail + ")") << "\n";
        std::error_code ec;
        fs::remove_all(out, ec);
        if (!result.ok || bad != 0 || !findings.empty()) {
            std::cout << "port-pachislop --check: " << result.errors.size() << " port error(s), " << findings.size()
                      << " file(s) differ from the committed tree\n";
            return EXIT_FINDINGS;
        }
        std::cout << "ok: " << files.size() << " files reproduce the committed ones (" << result.looks << " looks, " << result.templates
                  << " templates, " << result.colorSchemes << " color schemes, provenance)\n";
        return EXIT_OK;
    }

    struct Subcommand {
        const char* name;
        const char* usage;
        const char* summary;
        int (*run)(int argc, char** argv, const char* exe);
    };

    // the other subcommands (TitleLibraryCliTools.cpp) take their arguments as a list; the library's
    // ops are registered first, as the editor does at start
    std::vector<std::string> argList(int argc, char** argv) { return std::vector<std::string>(argv, argv + argc); }
    int runList(int argc, char** argv, const char*) { titlelibrary::registerTitleLibraryOps(); return titlelibrary::runListCommand(argList(argc, argv)); }
    int runVerify(int argc, char** argv, const char*) { titlelibrary::registerTitleLibraryOps(); return titlelibrary::runVerifyCommand(argList(argc, argv)); }
    int runLint(int argc, char** argv, const char*) { titlelibrary::registerTitleLibraryOps(); return titlelibrary::runLintCommand(argList(argc, argv)); }
    int runGenDocs(int argc, char** argv, const char*) { titlelibrary::registerTitleLibraryOps(); return titlelibrary::runGenDocsCommand(argList(argc, argv)); }
    int runBuildTemplates(int argc, char** argv, const char*) {
        titlelibrary::registerTitleLibraryOps();
        return titlelibrary::runBuildTemplatesCommand(argList(argc, argv));
    }
    int runBake(int argc, char** argv, const char* exe) { titlelibrary::registerTitleLibraryOps(); return titlelibrary::runBakeThumbnailsCommand(argList(argc, argv), exe); }
    int runReview(int argc, char** argv, const char* exe) { titlelibrary::registerTitleLibraryOps(); return titlelibrary::runReviewSheetCommand(argList(argc, argv), exe); }
    int runCalibrate(int argc, char** argv, const char* exe) { titlelibrary::registerTitleLibraryOps(); return titlelibrary::runCalibrateCommand(argList(argc, argv), exe); }

    const Subcommand SUBCOMMANDS[] = {
        {"list", "list [--kind K] [--family F] [--json]", "The library's items (built-in, project, personal) with their availability", runList},
        {"verify", "verify [--review <json>] [--no-thumbnails]",
         "Formats, references, metadata, fonts, denied terms, thumbnails' recipes of every built-in item (no GPU)", runVerify},
        {"lint", "lint [<id>...]", "The style lint of every look on its sample texts (allowlists excepted; no GPU)", runLint},
        {"gen-docs", "gen-docs [--check] [--out <md>]", "docs/mcp/guide/title-library.md from the library (no GPU)", runGenDocs},
        {"build-templates", "build-templates [--recipes <dir|json>] [--out <dir>] [--check]",
         "The new templates from their recipes (tools/titleart/templates; no GPU)", runBuildTemplates},
        {"bake-thumbnails", "bake-thumbnails [--only <glob>] [--check] [--out <dir>] [--scratch <dir>]",
         "Render every built-in item's thumbnails in a headless editor (GPU); --check compares recipes only", runBake},
        {"review-sheet", "review-sheet --pachislop <clone> --out <dir> [--only <glob>]",
         "Each ported item rendered beside Pachislop's reference picture (GPU; written outside the repository)", runReview},
        {"calibrate", "calibrate [--out <json>] [--red <dir>] [--scratch <dir>] [--samples all|latin]",
         "Every check_title metric over the library's renders and the red controls (GPU)", runCalibrate},
        {"port-pachislop", "port-pachislop --dump <json> --out <dir> [--repo <dir>] [--check]",
         "Map a Pachislop dump onto library files; --check compares a fresh port with the committed files", runPortPachislop},
        {"normalize", "normalize [--check] <dir>", "Rewrite every .titlelook and .titletemplate under <dir> canonically; --check lists the others",
         runNormalize},
    };

    void printHelp(std::ostream& out, const char* exe) {
        out << "usage: " << exe << " title-library <subcommand> [options]\n\nSubcommands:\n";
        for (const Subcommand& s : SUBCOMMANDS) out << "  " << s.usage << "\n      " << s.summary << "\n";
        out << "\nExit codes: 0 ok, 1 findings, 2 usage or I/O error.\n";
    }

}

int TitleLibraryCli::run(int argc, char** argv, const char* executableName) {
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "help") == 0) {
        printHelp(argc < 2 ? std::cerr : std::cout, executableName);
        return argc < 2 ? EXIT_USAGE : EXIT_OK;
    }
    for (const Subcommand& s : SUBCOMMANDS) {
        if (std::strcmp(argv[1], s.name) == 0) return s.run(argc - 2, argv + 2, executableName);
    }
    std::cerr << "title-library: unknown subcommand `" << argv[1] << "`\n";
    printHelp(std::cerr, executableName);
    return EXIT_USAGE;
}
