// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUiImportCli.h"

#include "ImportManifestWriter.h"
#include "RemoteImporter.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ImportManifest.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <ostream>
#include <set>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using jasperengine::htmlui::compat::ImportEntry;
using jasperengine::htmlui::compat::ImportManifest;
namespace fs = std::filesystem;

namespace {

    struct Options {
        std::string project;
        std::string dest;
        std::vector<std::string> documents;
        bool force = false;
        bool quiet = false;
        bool help = false;
    };

    bool parseArgs(int argc, char** argv, Options& options, std::string& error){
        for (int i = 1; i < argc; i++){
            const std::string arg = argv[i];
            auto value = [&](std::string& target){
                if (i + 1 >= argc){
                    error = "missing value for " + arg;
                    return false;
                }
                target = argv[++i];
                return true;
            };
            if (arg == "-h" || arg == "--help"){
                options.help = true;
            }else if (arg == "-p" || arg == "--project"){
                if (!value(options.project))
                    return false;
            }else if (arg == "--dest"){
                if (!value(options.dest))
                    return false;
            }else if (arg == "--force"){
                options.force = true;
            }else if (arg == "-q" || arg == "--quiet"){
                options.quiet = true;
            }else if (!arg.empty() && arg[0] == '-'){
                error = "unknown option " + arg;
                return false;
            }else{
                options.documents.push_back(arg);
            }
        }
        if (options.help)
            return true;
        if (options.project.empty()){
            error = "--project is required";
            return false;
        }
        if (options.documents.empty()){
            error = "name at least one .html or .css document";
            return false;
        }
        return true;
    }

    std::string plural(size_t count, const char* word){
        return std::to_string(count) + " " + word + (count == 1 ? "" : "s");
    }

}

void jasperengine::editor::htmlui::printImportUiUsage(const std::string& commandName, std::ostream& out){
    out << "Usage:\n"
        << "  " << commandName << " import-ui --project <project-dir> <document> [<document>...] [options]\n\n"
        << "Downloads every remote resource (Google Fonts, stylesheets, images, Lottie animations) the\n"
        << "HTML/CSS documents reference into the project's assets/htmlui-imports and records them in\n"
        << "assets/htmlui-imports/manifest.json, so the game resolves them without a network.\n\n"
        << "Options:\n"
        << "  -p, --project <path>   Project directory or project.yaml file.\n"
        << "      --dest <dir>       Directory inside the assets for the files (default assets/htmlui-imports).\n"
        << "      --force            Download again what is already imported.\n"
        << "  -q, --quiet            No progress and summary lines on stderr.\n"
        << "  -h, --help             Show this help.\n\n"
        << "Documents are .html/.htm/.css files inside the assets, project-relative or absolute.\n"
        << "Diagnostics go to stdout as path:line:col: severity: message [HTMLUI-IMP-NNNN].\n"
        << "Exit code: 0 imported (or nothing to do), 1 an import failed, 2 usage error.\n";
}

int jasperengine::editor::htmlui::runImportUi(int argc, char** argv, const std::string& commandName, std::ostream& out, std::ostream& err){
    Options options;
    std::string error;
    if (!parseArgs(argc, argv, options, error)){
        err << commandName << " import-ui: " << error << "\n\n";
        printImportUiUsage(commandName, err);
        return 2;
    }
    if (options.help){
        printImportUiUsage(commandName, out);
        return 0;
    }

    std::error_code ec;
    fs::path projectRoot = fs::absolute(editor::FileUtils::pathFromUtf8(options.project), ec).lexically_normal();
    if (projectRoot.filename() == "project.yaml")
        projectRoot = projectRoot.parent_path();
    if (!fs::is_regular_file(projectRoot / "project.yaml", ec)){
        err << commandName << " import-ui: no project.yaml in " << projectRoot.string() << "\n";
        return 2;
    }
    ImportTarget target;
    if (!ImportTarget::forProject(projectRoot, options.dest, target, error)){
        err << commandName << " import-ui: " << error << "\n";
        return 2;
    }

    // documents: relative to the working directory, else to the project
    std::vector<std::string> documents;
    for (const std::string& given : options.documents){
        fs::path path = editor::FileUtils::pathFromUtf8(given);
        if (path.is_relative()){
            const fs::path fromCwd = fs::absolute(path, ec);
            path = fs::is_regular_file(fromCwd, ec) ? fromCwd : projectRoot / path;
        }
        const std::string assetPath = target.toAssetPath(path);
        if (assetPath.empty() || !fs::is_regular_file(path, ec)){
            err << commandName << " import-ui: " << (assetPath.empty() ? "not inside the project's assets: " : "not found: ") << given << "\n";
            return 2;
        }
        documents.push_back(assetPath);
    }

    RemoteImporter importer(target);
    if (!options.quiet)
        importer.setProgress([&err](const std::string& line){ err << line << "\n"; err.flush(); });

    size_t imported = 0, upToDate = 0, failed = 0;
    std::set<std::string> done;
    for (const std::string& document : documents){
        const ImportManifest manifest = ImportManifestWriter(target.assetsDir).read();
        for (const DiscoveredReference& reference : discoverRemoteReferences(target, document)){
            if (!done.insert(ImportManifest::normalizeUrl(reference.url)).second)
                continue;
            const ImportEntry* entry = manifest.find(reference.url);
            if (!options.force && entry && fs::is_regular_file(target.toAbsolute(entry->path), ec)){
                upToDate++;
                continue;
            }
            const ImportedResource result = importer.import(reference.url, reference.kind, reference.location);
            if (result.ok)
                imported++;
            else
                failed++;
        }
    }

    std::vector<compat::Diagnostic> diagnostics = importer.takeDiagnostics();
    compat::sortDiagnostics(diagnostics);
    size_t errors = 0, warnings = 0;
    for (compat::Diagnostic& diagnostic : diagnostics){
        if (!diagnostic.file.empty())
            diagnostic.file = target.toProjectRelative(diagnostic.file);
        if (diagnostic.severity == compat::Severity::Error)
            errors++;
        else if (diagnostic.severity == compat::Severity::Warning)
            warnings++;
        out << compat::toText(diagnostic) << "\n";
    }
    out.flush();
    if (!options.quiet){
        err << "import-ui: " << imported << " imported, " << upToDate << " up to date, " << failed << " failed (" << plural(errors, "error")
            << ", " << plural(warnings, "warning") << ")\n";
    }
    return (errors > 0 || failed > 0) ? 1 : 0;
}
