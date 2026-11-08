// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtCli.h"

#include "CallCommand.h"
#include "CommandLine.h"
#include "asset/AssetFileWrite.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/interchange/TitleImport.h"
#include "titleart/localisation/TitleStrings.h"
#include "titleart/TitleTranslations.h"
#include "util/FileUtils.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
namespace fs = std::filesystem;

namespace {

    constexpr int EXIT_DONE = 0, EXIT_FAILED = 1, EXIT_USAGE = 2;

    struct Args {
        std::vector<std::string> inputs;
        std::map<std::string, std::string> values;     // --flag value
        std::set<std::string> flags;                   // --flag
    };

    // the flags of each subcommand: those that take a value, and the switches
    struct FlagTable {
        std::vector<std::string> valued, switches;
    };

    bool parseArgs(int argc, char** argv, const FlagTable& table, Args& out, std::string& error) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg.rfind("--", 0) != 0) {
                out.inputs.push_back(arg);
                continue;
            }
            std::string name = arg.substr(2), value;
            const size_t eq = name.find('=');
            const bool inlineValue = eq != std::string::npos;
            if (inlineValue) {
                value = name.substr(eq + 1);
                name = name.substr(0, eq);
            }
            std::replace(name.begin(), name.end(), '_', '-');
            if (std::find(table.switches.begin(), table.switches.end(), name) != table.switches.end()) {
                out.flags.insert(name);
                continue;
            }
            if (std::find(table.valued.begin(), table.valued.end(), name) == table.valued.end()) {
                error = "unknown option --" + name;
                return false;
            }
            if (!inlineValue) {
                if (i + 1 >= argc) {
                    error = "--" + name + " needs a value";
                    return false;
                }
                value = argv[++i];
            }
            out.values[name] = value;
        }
        return true;
    }

    fs::path absoluteOf(const std::string& path) {
        const fs::path p = FileUtils::pathFromUtf8(path);
        return (p.is_absolute() ? p : CommandLine::invocationDirectory() / p).lexically_normal();
    }

    // '*' and '?' in the last path component
    bool wildcard(const std::string& pattern, const std::string& name) {
        size_t p = 0, n = 0, star = std::string::npos, mark = 0;
        while (n < name.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
                p++;
                n++;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                mark = n;
            } else if (star != std::string::npos) {
                p = star + 1;
                n = ++mark;
            } else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*') p++;
        return p == pattern.size();
    }

    // inputs as files: globs expanded, directories walked for `extension` (empty: none)
    std::vector<fs::path> expand(const std::vector<std::string>& inputs, const std::string& extension) {
        std::vector<fs::path> out;
        std::error_code ec;
        for (const std::string& input : inputs) {
            const fs::path path = absoluteOf(input);
            const std::string name = path.filename().string();
            if (name.find_first_of("*?") != std::string::npos) {
                std::vector<fs::path> matched;
                for (fs::directory_iterator it(path.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
                    if (it->is_regular_file(ec) && wildcard(name, it->path().filename().string())) matched.push_back(it->path());
                }
                std::sort(matched.begin(), matched.end());
                out.insert(out.end(), matched.begin(), matched.end());
            } else if (!extension.empty() && fs::is_directory(path, ec)) {
                std::vector<fs::path> found;
                for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
                    if (it->is_regular_file(ec) && it->path().extension() == extension) found.push_back(it->path());
                }
                std::sort(found.begin(), found.end());
                out.insert(out.end(), found.begin(), found.end());
            } else {
                out.push_back(path);
            }
        }
        return out;
    }

    fs::path assetsOf(const fs::path& project) {
        try {
            const YAML::Node node = YAML::LoadFile((project / "project.yaml").string());
            if (node["assetsDir"]) return (project / FileUtils::pathFromUtf8(node["assetsDir"].as<std::string>())).lexically_normal();
        } catch (const std::exception&) {
        }
        return project;
    }

    bool readAll(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream buffer;
        buffer << in.rdbuf();
        out = buffer.str();
        return true;
    }

    bool writeAll(const std::vector<TitleOpFile>& files, std::string& error) {
        std::error_code ec;
        for (const TitleOpFile& f : files) {
            fs::create_directories(fs::path(f.path).parent_path(), ec);
            if (!assetio::writeFile(fs::path(f.path), f.content, assetio::Durability::Document, &error)) return false;
        }
        return true;
    }

    // "logo.pslot.json" -> "logo", "badge.svg" -> "badge"
    std::string stemOf(const fs::path& source) {
        std::string name = source.filename().string();
        for (const char* suffix : {".pslot.json", ".json", ".psd", ".psb", ".svg", ".asl"}) {
            const std::string s = suffix;
            if (name.size() > s.size()) {
                std::string tail = name.substr(name.size() - s.size());
                std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                if (tail == s) return name.substr(0, name.size() - s.size());
            }
        }
        return source.stem().string();
    }

    void report(bool json, const std::string& input, bool ok, const std::string& text, const Json& structured) {
        if (json) {
            Json line = Json{{"input", input}, {"ok", ok}, {"text", text}};
            if (!structured.is_null()) line["result"] = structured;
            std::cout << line.dump() << std::endl;
        } else {
            (ok ? std::cout : std::cerr) << (ok ? "" : "FAILED ") << input << ": " << text << std::endl;
        }
    }

    // ---- import ----

    // a strings file into the documents its rows name (or --title)
    bool importStrings(const fs::path& file, const fs::path& project, const Args& args, Json& result, std::string& text) {
        std::string body, error;
        if (!readAll(file, body)) {
            text = "cannot read the file";
            return false;
        }
        TitleStringsFile strings;
        if (!parseTitleStrings(body, "auto", strings, error)) {
            text = error;
            return false;
        }
        std::set<std::string> documents;
        if (args.values.count("title")) {
            documents.insert(args.values.at("title"));
        } else {
            for (const TitleStringRow& row : strings.rows) {
                if (!row.file.empty()) documents.insert(row.file);
            }
            if (documents.empty()) {
                text = "the file names no documents (no file column): pass --title <x.titleart>";
                return false;
            }
        }
        result = Json::array();
        bool ok = true;
        for (const std::string& document : documents) {
            const fs::path path = (FileUtils::pathFromUtf8(document).is_absolute() ? FileUtils::pathFromUtf8(document) : project / document).lexically_normal();
            std::string docText;
            titleart::TitleArtParseResult parsed;
            if (readAll(path, docText)) parsed = titleart::parseTitleArt(docText);
            if (!parsed.ok) {
                result.push_back(Json{{"document", document}, {"ok", false}, {"error", parsed.error.message.empty() ? "cannot read it" : parsed.error.message}});
                ok = false;
                continue;
            }
            TitleOpsOptions options;
            options.documentPath = path.string();
            options.projectPath = project.string();
            options.assetsPath = assetsOf(project).string();
            Json op = Json{{"op", "import_strings"}, {"file", file.string()}, {"add_languages", args.flags.count("add-languages") > 0}};
            // the rows of another document stay out of this one: route by the document's own path
            const TitleOpsResult r = TitleArtOps::apply(parsed.doc, op, options);
            if (!r.ok) {
                result.push_back(Json{{"document", document}, {"ok", false}, {"error", r.error()}});
                ok = false;
                continue;
            }
            if (!assetio::writeFile(path, titleart::writeTitleArt(r.doc), assetio::Durability::Document, &error)) {
                result.push_back(Json{{"document", document}, {"ok", false}, {"error", error}});
                ok = false;
                continue;
            }
            result.push_back(Json{{"document", document}, {"ok", true}, {"report", r.reports.value("0", Json::object())}});
        }
        text = std::to_string(documents.size()) + " document(s)";
        return ok;
    }

    bool importOne(const fs::path& source, const fs::path& project, const fs::path& into, const Args& args, Json& result, std::string& text) {
        std::string kind = args.values.count("kind") ? args.values.at("kind") : "auto";
        if (kind == "auto") {
            std::string ext = source.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".csv" || ext == ".xlf" || ext == ".xliff") kind = "strings";
            if (ext == ".asl") kind = "ps_styles";
        }
        if (kind == "strings") return importStrings(source, project, args, result, text);
        const fs::path assets = assetsOf(project);
        TitleOpsOptions options;
        options.projectPath = project.string();
        options.assetsPath = assets.string();
        if (kind == "ps_styles") {
            // a library op: the looks it writes are its result
            options.documentPath = (into / (stemOf(source) + ".titleart")).string();
            Json op = Json{{"op", "import_ps_styles"}, {"file", source.string()}, {"overwrite", args.flags.count("overwrite") > 0}};
            const TitleOpsResult r = TitleArtOps::apply(titleart::makeNewTitleArt(), op, options);
            if (!r.ok) {
                text = r.error();
                return false;
            }
            std::string error;
            if (!writeAll(r.files, error)) {
                text = error;
                return false;
            }
            result = r.reports.value("0", Json::object());
            text = std::to_string(r.files.size()) + " look(s) written";
            return true;
        }
        TitleCreateRequest request;
        request.sourcePath = source.string();
        request.documentPath = (into / (stemOf(source) + ".titleart")).string();
        request.projectPath = project.string();
        request.assetsPath = assets.string();
        std::error_code ec;
        if (fs::exists(request.documentPath, ec) && !args.flags.count("overwrite")) {
            text = "output_exists: " + fs::path(request.documentPath).filename().string() + " exists (pass --overwrite)";
            return false;
        }
        TitleCreateResult imported;
        std::string code, error;
        if (!TitleArtOps::createFrom(request, imported, code, error)) {
            text = code + ": " + error;
            return false;
        }
        if (kind != "auto" && kind != imported.kind) {
            text = "the file is a " + titleImportKindLabel(imported.kind) + ", not --kind " + kind;
            return false;
        }
        if (!titleart::isTitleArtGuid(imported.doc.guid())) imported.doc.root["guid"] = titleart::generateTitleArtGuid();
        titleart::normalizeTitleArt(imported.doc);
        std::vector<TitleOpFile> files = imported.files;
        files.push_back({request.documentPath, titleart::writeTitleArt(imported.doc)});
        if (!writeAll(files, error)) {
            text = "write_failed: " + error;
            return false;
        }
        result = titleImportJson(imported, project);
        std::error_code rel;
        result["document"] = fs::relative(request.documentPath, project, rel).generic_string();
        text = "imported a " + titleImportKindLabel(imported.kind) + " as " + result["document"].get<std::string>() + " (" +
               std::to_string(imported.files.size()) + " file(s), " + std::to_string(imported.notes.size()) + " note(s))";
        return true;
    }

    // ---- export ----

    // the render formats' `options` from the flags (dashes to underscores)
    bool exportOptions(const Args& args, Json& options, std::string& error) {
        options = Json::object();
        const auto number = [&](const char* flag, const char* key) {
            if (!args.values.count(flag)) return true;
            char* end = nullptr;
            const double v = std::strtod(args.values.at(flag).c_str(), &end);
            if (!end || *end) {
                error = std::string("--") + flag + " is a number";
                return false;
            }
            if (std::string(key) == "bit_depth" || std::string(key) == "alpha_threshold") options[key] = (int)v;
            else options[key] = v;
            return true;
        };
        if (!number("bit-depth", "bit_depth") || !number("scale", "scale") || !number("alpha-threshold", "alpha_threshold") || !number("fps", "fps")) return false;
        for (const auto& [flag, key] : std::vector<std::pair<const char*, const char*>>{{"alpha", "alpha"}, {"animation", "animation"}, {"family", "family"}}) {
            if (args.values.count(flag)) options[key] = args.values.at(flag);
        }
        if (args.flags.count("no-bleed")) options["alpha_bleed"] = false;
        for (const auto& [flag, key] : std::vector<std::pair<const char*, const char*>>{
                 {"split-titles", "split_titles"}, {"parts", "parts"}, {"zip", "zip"}, {"overwrite", "overwrite"}}) {
            if (args.flags.count(flag)) options[key] = true;
        }
        return true;
    }

    Json languagesArg(const Args& args) {
        if (!args.values.count("languages")) return nullptr;
        const std::string value = args.values.at("languages");
        if (value == "all") return "all";
        Json list = Json::array();
        std::stringstream ss(value);
        std::string code;
        while (std::getline(ss, code, ',')) {
            if (!code.empty()) list.push_back(code);
        }
        return list;
    }

    bool inside(const fs::path& root, const fs::path& path) {
        const std::string r = root.lexically_normal().generic_string(), p = path.lexically_normal().generic_string();
        return p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/');
    }

}

int TitleArtCli::runImport(int argc, char** argv, const char* executableName) {
    (void)executableName;
    Args args;
    std::string error;
    if (!parseArgs(argc, argv, {{"project", "into", "kind", "title"}, {"add-languages", "overwrite", "json", "help"}}, args, error) ||
        (!args.flags.count("help") && (args.inputs.empty() || !args.values.count("project")))) {
        std::cerr << (error.empty() ? "import-title-art needs files and --project <dir>" : error) << "\n"
                  << "Usage: import-title-art <file|glob>... --project <dir> [--into <assets dir>]\n"
                     "    [--kind auto|pachislop|psd|ps_styles|svg|strings] [--title <x.titleart>] [--add-languages] [--overwrite] [--json]\n";
        return args.flags.count("help") ? EXIT_DONE : EXIT_USAGE;
    }
    const std::string kind = args.values.count("kind") ? args.values.at("kind") : "auto";
    if (!std::set<std::string>{"auto", "pachislop", "psd", "ps_styles", "svg", "strings"}.count(kind)) {
        std::cerr << "--kind is auto, pachislop, psd, ps_styles, svg or strings\n";
        return EXIT_USAGE;
    }
    const fs::path project = absoluteOf(args.values.at("project"));
    std::error_code ec;
    if (!fs::is_regular_file(project / "project.yaml", ec)) {
        std::cerr << project.generic_string() << " is not a project (no project.yaml)\n";
        return EXIT_USAGE;
    }
    const fs::path assets = assetsOf(project);
    const fs::path into = args.values.count("into") ? absoluteOf(args.values.at("into")) : assets;
    if (!inside(assets, into)) {
        std::cerr << "--into must be inside the project's assets (" << assets.generic_string() << ")\n";
        return EXIT_USAGE;
    }
    registerTitleInterchange();
    int failed = 0;
    const std::vector<fs::path> files = expand(args.inputs, "");
    for (const fs::path& file : files) {
        Json result;
        std::string text;
        const bool ok = importOne(file, project, into, args, result, text);
        failed += ok ? 0 : 1;
        report(args.flags.count("json") > 0, file.generic_string(), ok, text, result);
    }
    if (files.empty()) {
        std::cerr << "no file matched\n";
        return EXIT_FAILED;
    }
    return failed ? EXIT_FAILED : EXIT_DONE;
}

int TitleArtCli::runExport(int argc, char** argv, const char* executableName) {
    Args args;
    std::string error;
    const FlagTable table{{"project", "format", "languages", "out", "bit-depth", "scale", "alpha", "alpha-threshold", "animation", "fps", "family"},
                          {"no-bleed", "split-titles", "parts", "zip", "overwrite", "json", "help"}};
    if (!parseArgs(argc, argv, table, args, error) ||
        (!args.flags.count("help") && (args.inputs.empty() || !args.values.count("project") || !args.values.count("format")))) {
        std::cerr << (error.empty() ? "export-title-art needs documents, --project <dir> and --format" : error) << "\n"
                  << "Usage: export-title-art <x.titleart|glob|dir>... --project <dir> --format png|psd|svg|sheet|atlas|sequence|ktx2|csv|xliff\n"
                     "    [--languages all|en,de] [--out <dir>] [--bit-depth 8|16] [--scale <s>] [--alpha straight|premultiplied] [--no-bleed]\n"
                     "    [--alpha-threshold <n>] [--split-titles] [--parts] [--animation <name>] [--fps <n>] [--family desktop|mobile|web]\n"
                     "    [--zip] [--overwrite] [--json]\n";
        return args.flags.count("help") ? EXIT_DONE : EXIT_USAGE;
    }
    const std::string format = args.values.at("format");
    static const std::set<std::string> formats = {"png", "psd", "svg", "sheet", "atlas", "sequence", "ktx2", "csv", "xliff"};
    if (!formats.count(format)) {
        std::cerr << "--format is png, psd, svg, sheet, atlas, sequence, ktx2, csv or xliff\n";
        return EXIT_USAGE;
    }
    const fs::path project = absoluteOf(args.values.at("project"));
    std::error_code ec;
    if (!fs::is_regular_file(project / "project.yaml", ec)) {
        std::cerr << project.generic_string() << " is not a project (no project.yaml)\n";
        return EXIT_USAGE;
    }
    const std::vector<fs::path> documents = expand(args.inputs, ".titleart");
    if (documents.empty()) {
        std::cerr << "no document matched\n";
        return EXIT_FAILED;
    }
    const bool json = args.flags.count("json") > 0;
    const fs::path out = args.values.count("out") ? absoluteOf(args.values.at("out")) : fs::path();

    // the strings formats: offline, one file for every document (R23)
    if (format == "csv" || format == "xliff") {
        std::vector<std::pair<std::string, titleart::TitleArtDoc>> loaded;
        for (const fs::path& d : documents) {
            std::string text;
            titleart::TitleArtParseResult parsed;
            if (readAll(d, text)) parsed = titleart::parseTitleArt(text);
            if (!parsed.ok) {
                report(json, d.generic_string(), false, parsed.error.message.empty() ? "cannot read it" : parsed.error.message, nullptr);
                return EXIT_FAILED;
            }
            std::error_code rel;
            loaded.emplace_back(fs::relative(d, project, rel).generic_string(), std::move(parsed.doc));
        }
        std::vector<TitleStringsDocument> docs;
        for (const auto& [file, doc] : loaded) docs.push_back({file, &doc});
        std::vector<std::string> languages;
        const Json asked = languagesArg(args);
        if (asked.is_array()) for (const Json& c : asked) languages.push_back(c.get<std::string>());
        const std::string stem = documents.size() == 1 ? documents.front().stem().string() : "strings";
        const fs::path dir = !out.empty() ? out : project / "Recordings" / "Title Art" / (documents.size() == 1 ? stem : std::string());
        std::vector<std::pair<fs::path, std::string>> files;
        if (format == "csv") {
            files.emplace_back(dir / (stem + ".csv"), exportTitleStringsCsv(docs, languages));
        } else {
            for (const auto& [code, text] : exportTitleStringsXliff(docs, languages)) {
                std::string lower = code;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                files.emplace_back(dir / (stem + "." + lower + ".xlf"), text);
            }
        }
        Json written = Json::array();
        for (const auto& [path, text] : files) {
            if (fs::exists(path, ec) && !args.flags.count("overwrite")) {
                report(json, path.generic_string(), false, "output_exists (pass --overwrite)", nullptr);
                return EXIT_FAILED;
            }
            fs::create_directories(path.parent_path(), ec);
            if (!assetio::writeFile(path, text, assetio::Durability::Document, &error)) {
                report(json, path.generic_string(), false, error, nullptr);
                return EXIT_FAILED;
            }
            written.push_back(path.generic_string());
        }
        report(json, std::to_string(documents.size()) + " document(s)", true, "wrote " + std::to_string(files.size()) + " file(s)", Json{{"files", written}});
        return EXIT_DONE;
    }

    // the render formats: one session of one headless editor for the whole batch
    Json options;
    if (!exportOptions(args, options, error)) {
        std::cerr << error << "\n";
        return EXIT_USAGE;
    }
    // MCP writes only inside the project: elsewhere the files are moved there afterwards
    const bool outside = !out.empty() && !inside(project, out);
    std::vector<std::pair<std::string, nlohmann::json>> calls;
    for (const fs::path& d : documents) {
        std::error_code rel;
        const std::string relative = fs::relative(d, project, rel).generic_string();
        Json arguments = Json{{"path", relative}, {"format", format}, {"options", options}, {"wait", true}, {"timeout_seconds", 3600}};
        const Json languages = languagesArg(args);
        if (!languages.is_null()) arguments["languages"] = languages;
        if (!out.empty()) {
            arguments["out"] = outside ? (fs::path("Recordings") / "Title Art" / ".cli" / d.stem()).generic_string()
                                       : fs::relative(out, project, rel).generic_string();
        }
        calls.emplace_back("export_title", nlohmann::json::parse(arguments.dump()));
    }
    std::vector<cli::CallResult> results;
    cli::CallCommand::runCalls(project.string(), calls, true, results, executableName);
    int failed = 0;
    for (size_t i = 0; i < documents.size(); i++) {
        if (i >= results.size()) {
            report(json, documents[i].generic_string(), false, results.empty() ? "no editor" : results.back().text, nullptr);
            failed++;
            continue;
        }
        const cli::CallResult& r = results[i];
        Json structured = r.structured.is_null() ? Json() : Json::parse(r.structured.dump());
        bool ok = r.ok;
        if (ok && outside && structured.is_object() && structured.contains("files")) {
            Json moved = Json::array();
            for (const Json& f : structured["files"]) {
                const fs::path from = (project / FileUtils::pathFromUtf8(f.get<std::string>())).lexically_normal();
                const fs::path to = out / from.filename();
                fs::create_directories(out, ec);
                if (fs::exists(to, ec) && !args.flags.count("overwrite")) {
                    ok = false;
                    break;
                }
                fs::rename(from, to, ec);
                if (ec) {
                    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
                    fs::remove(from, ec);
                }
                moved.push_back(to.generic_string());
            }
            structured["files"] = moved;
        }
        failed += ok ? 0 : 1;
        report(json, documents[i].generic_string(), ok, r.text, structured);
    }
    if (outside) fs::remove_all(project / "Recordings" / "Title Art" / ".cli", ec);
    return failed ? EXIT_FAILED : EXIT_DONE;
}
