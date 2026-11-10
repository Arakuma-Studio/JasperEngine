// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CleanupCli.h"

#include "CleanupSession.h"
#include "pixelart/lint/PixelLint.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::cleanup {

    namespace fs = std::filesystem;
    using nlohmann::json;

    namespace {

        constexpr int kExitOk = 0;
        constexpr int kExitFailed = 1;
        constexpr int kExitUsage = 2;
        constexpr int kExitFixable = 3;
        constexpr int kExitNeedsPerson = 4;

        std::vector<std::string> arguments(int argc, char** argv) {
            std::vector<std::string> args;
            for (int i = 1; i < argc; i++) args.emplace_back(argv[i]);
            return args;
        }

        // A project-relative path when --project is given and the path is inside it, else as given
        std::string projectRelativeArg(const fs::path& project, const std::string& given) {
            if (project.empty()) return given;
            std::error_code ec;
            const fs::path relative = fs::relative(fs::absolute(fs::u8path(given), ec), fs::absolute(project, ec), ec);
            if (!ec && !relative.empty() && *relative.begin() != "..") return FileUtils::pathToGenericUtf8(relative);
            return given;
        }

        bool readJsonFile(const std::string& path, json& out, std::string& error) {
            std::ifstream in(fs::u8path(path), std::ios::binary);
            if (!in) {
                error = "cannot read " + path;
                return false;
            }
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            try {
                out = json::parse(text);
            } catch (const json::exception& e) {
                error = path + " is not JSON: " + e.what();
                return false;
            }
            return true;
        }

        // <w>[x<h>][+<ox>,<oy>]
        bool parseGridArg(const std::string& text, json& grid, std::string& error) {
            std::string spec = text;
            double offsetX = 0, offsetY = 0;
            const size_t plus = spec.find('+');
            if (plus != std::string::npos) {
                const std::string offsets = spec.substr(plus + 1);
                const size_t comma = offsets.find(',');
                try {
                    offsetX = std::stod(offsets.substr(0, comma));
                    offsetY = comma == std::string::npos ? 0.0 : std::stod(offsets.substr(comma + 1));
                } catch (const std::exception&) {
                    error = "the offset of --grid is <ox>,<oy>";
                    return false;
                }
                spec = spec.substr(0, plus);
            }
            try {
                const size_t x = spec.find('x');
                const double width = std::stod(spec.substr(0, x));
                grid = {{"kind", "cells"}, {"cell_width", width}, {"offset_x", offsetX}, {"offset_y", offsetY}};
                if (x != std::string::npos) grid["cell_height"] = std::stod(spec.substr(x + 1));
            } catch (const std::exception&) {
                error = "--grid is auto or <w>[x<h>][+<ox>,<oy>]";
                return false;
            }
            return true;
        }

        std::string cliExtension(const std::string& path) {
            const size_t dot = path.find_last_of('.');
            if (dot == std::string::npos) return {};
            std::string extension = path.substr(dot);
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return extension;
        }

        json problemJson(const PixelProblem& p) {
            json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix}};
            if (!p.field.empty()) j["field"] = p.field;
            return j;
        }

        int failUsage(std::ostream& out, std::ostream& err, bool asText, const std::string& message, const std::string& fix) {
            if (asText) {
                err << message << " Fix: " << fix << "\n";
            } else {
                out << json{{"error", {{"kind", "usage"}, {"message", message}, {"fix", fix}}}}.dump() << "\n";
            }
            return kExitUsage;
        }

        void printResultText(std::ostream& out, const json& result) {
            out << result.value("verdict", "") << ": " << result.value("summary", "") << "\n";
        }

        int verdictExit(Verdict verdict) {
            switch (verdict) {
                case Verdict::Clean: return kExitOk;
                case Verdict::Fixable: return kExitFixable;
                case Verdict::NeedsPerson: return kExitNeedsPerson;
            }
            return kExitFailed;
        }

    }

    int runFixPixelArt(int argc, char** argv, const std::string&, std::ostream& out, std::ostream& err) {
        const std::vector<std::string> args = arguments(argc, argv);
        Spec spec;
        std::string input, recipePath, outPath, imagePath, gridArg, reportFormat = "json";
        bool dryRun = false, gridGiven = false;
        std::string project;
        bool asText = false;
        for (size_t i = 0; i < args.size(); i++) {
            const std::string& arg = args[i];
            const auto value = [&](std::string& target) {
                if (i + 1 >= args.size()) return false;
                target = args[++i];
                return true;
            };
            std::string given;
            if (arg == "--recipe") {
                if (!value(recipePath)) return failUsage(out, err, asText, "--recipe needs a file", "give --recipe r.json");
            } else if (arg == "--rerun") {
                spec.rerun = true;
            } else if (arg == "--palette") {
                if (!value(given)) return failUsage(out, err, asText, "--palette needs a value", "a name, a .palette path or #rrggbb,...");
                spec.palette = given;
            } else if (arg == "--grid") {
                if (!value(gridArg)) return failUsage(out, err, asText, "--grid needs a value", "auto or <w>[x<h>][+<ox>,<oy>]");
                gridGiven = gridArg != "auto";
            } else if (arg == "--sheet") {
                if (!value(given)) return failUsage(out, err, asText, "--sheet needs a value", "auto or off");
                if (given == "off") spec.sheet = SheetArg::Off;
                else if (given == "on") spec.sheet = SheetArg::On;
                else if (given != "auto") return failUsage(out, err, asText, "--sheet is auto or off", "auto or off");
            } else if (arg == "--format") {
                if (!value(given)) return failUsage(out, err, asText, "--format needs a value", "png, aseprite, json or text");
                if (given == "png") spec.format = OutputFormat::Png;
                else if (given == "aseprite") spec.format = OutputFormat::Aseprite;
                else if (given == "json" || given == "text") {
                    reportFormat = given;
                    asText = given == "text";
                } else return failUsage(out, err, asText, "--format is png, aseprite, json or text", "e.g. --format png");
            } else if (arg == "--out") {
                if (!value(outPath)) return failUsage(out, err, asText, "--out needs a path", "a .png or .aseprite path");
            } else if (arg == "--overwrite") {
                spec.overwrite = true;
            } else if (arg == "--force") {
                spec.force = true;
            } else if (arg == "--dry-run") {
                dryRun = true;
            } else if (arg == "--image") {
                if (!value(imagePath)) return failUsage(out, err, asText, "--image needs a path", "--image out.png");
            } else if (arg == "--project") {
                if (!value(project)) return failUsage(out, err, asText, "--project needs a directory", "--project <dir>");
            } else if (arg == "--fail-on") {
                if (!value(given) || (given != "fail" && given != "warn")) return failUsage(out, err, asText, "--fail-on is fail or warn", "fail or warn");
                spec.failOnWarn = given == "warn";
            } else if (arg == "-h" || arg == "--help") {
                out << "usage: fix-pixel-art <in> [--recipe r.json | --rerun] [--palette p] [--grid auto|<w>[x<h>][+<ox>,<oy>]] [--sheet auto|off] "
                       "[--format png|aseprite] [--out path] [--overwrite] [--force] [--dry-run] [--image out.png] [--project dir] [--format text]\n";
                return kExitOk;
            } else if (!arg.empty() && arg[0] == '-') {
                return failUsage(out, err, asText, "unknown option " + arg, "see fix-pixel-art --help");
            } else if (input.empty()) {
                input = arg;
            } else {
                return failUsage(out, err, asText, "more than one input: " + arg, "give one input");
            }
        }
        if (input.empty()) return failUsage(out, err, asText, "no input file", "give an image or .aseprite file");

        json recipe = json::object();
        if (!recipePath.empty()) {
            std::string error;
            if (!readJsonFile(recipePath, recipe, error)) return failUsage(out, err, asText, error, "give a recipe .json file");
        }
        if (gridGiven) {
            json grid;
            std::string error;
            if (!parseGridArg(gridArg, grid, error)) return failUsage(out, err, asText, error, "--grid auto or <w>[x<h>][+<ox>,<oy>]");
            recipe["grid"] = grid;
        }
        if (!recipe.empty()) spec.recipe = recipe;

        spec.projectDir = project.empty() ? fs::path() : fs::u8path(project);
        spec.source = projectRelativeArg(spec.projectDir, input);
        if (!dryRun) {
            spec.output.write = true;
            if (!outPath.empty()) {
                const std::string extension = cliExtension(outPath);
                const std::string path = projectRelativeArg(spec.projectDir, outPath);
                if (extension == ".png") spec.output.png = path;
                else if (extension == ".aseprite" || extension == ".ase") spec.output.aseprite = path;
                else return failUsage(out, err, asText, "--out must end in .png or .aseprite", "e.g. --out hero_clean.png");
            }
        }

        std::vector<PixelProblem> problems;
        std::optional<Prepared> prepared = prepare(nullptr, spec, problems);
        if (!prepared) {
            const bool usage = !problems.empty() && (problems.front().kind == "bad_recipe" || problems.front().kind == "recipe_version");
            if (asText) {
                for (const PixelProblem& p : problems) err << p.kind << ": " << p.message << (p.fix.empty() ? "" : " Fix: " + p.fix) << "\n";
            } else {
                json list = json::array();
                for (const PixelProblem& p : problems) list.push_back(problemJson(p));
                out << json{{"errors", list}}.dump() << "\n";
            }
            return usage ? kExitUsage : kExitFailed;
        }
        const Result result = run(*prepared);
        if (!result.problem.empty()) {
            if (asText) err << result.problem.kind << ": " << result.problem.message << "\n";
            else out << json{{"errors", json::array({problemJson(result.problem)})}}.dump() << "\n";
            return kExitFailed;
        }
        json data = toJson(*prepared, result);
        if (!imagePath.empty()) {
            const PixelImage preview = renderPreview(*prepared, result, PreviewKind::Compare);
            if (preview.empty() || !preview.savePng(fs::u8path(imagePath))) {
                err << "could not write " << imagePath << "\n";
                return kExitFailed;
            }
            data["image"] = imagePath;
        }

        if (dryRun) {
            if (asText) printResultText(out, data);
            else out << data.dump() << "\n";
            return verdictExit(result.report.verdict);
        }

        // a write: R30's refusal and R31's checks are encodeOutputs'
        std::vector<OutputFile> files;
        PixelProblem problem;
        if (!encodeOutputs(*prepared, result, files, problem)) {
            data["errors"] = json::array({problemJson(problem)});
            if (asText) err << problem.kind << ": " << problem.message << (problem.fix.empty() ? "" : " Fix: " + problem.fix) << "\n";
            else out << data.dump() << "\n";
            return problem.kind == "needs_person" ? kExitNeedsPerson : kExitFailed;
        }
        if (!writeDirect(*prepared, files, problem)) {
            data["errors"] = json::array({problemJson(problem)});
            if (asText) err << problem.message << "\n";
            else out << data.dump() << "\n";
            return kExitFailed;
        }
        json written = json::array();
        for (const OutputFile& file : files) written.push_back({{"role", file.role}, {"path", file.path}});
        data["written"] = written;
        if (asText) {
            printResultText(out, data);
            out << "wrote " << files.front().path << "\n";
        } else {
            out << data.dump() << "\n";
        }
        if (result.report.verdict == Verdict::NeedsPerson) return kExitNeedsPerson;   // forced
        if (spec.failOnWarn && result.report.lints.warn > 0) return kExitFixable;
        return kExitOk;
    }

    int runLintPixelArt(int argc, char** argv, const std::string&, std::ostream& out, std::ostream& err) {
        const std::vector<std::string> args = arguments(argc, argv);
        std::vector<std::string> inputs;
        LintSpec spec;
        std::string project, imagePath, rulesPath, reportFormat = "json";
        bool asText = false;
        for (size_t i = 0; i < args.size(); i++) {
            const std::string& arg = args[i];
            const auto value = [&](std::string& target) {
                if (i + 1 >= args.size()) return false;
                target = args[++i];
                return true;
            };
            std::string given;
            if (arg == "--palette") {
                if (!value(given)) return failUsage(out, err, asText, "--palette needs a value", "a name, a .palette path or #rrggbb,...");
                spec.palette = given;
            } else if (arg == "--lints") {
                if (!value(given)) return failUsage(out, err, asText, "--lints needs ids", lint::lintIdList());
                size_t start = 0;
                while (start <= given.size()) {
                    size_t comma = given.find(',', start);
                    if (comma == std::string::npos) comma = given.size();
                    const std::string id = given.substr(start, comma - start);
                    if (!lint::isLintId(id)) return failUsage(out, err, asText, "unknown lint \"" + id + "\"", "valid lints: " + lint::lintIdList());
                    spec.lints.push_back(id);
                    start = comma + 1;
                }
            } else if (arg == "--rules") {
                if (!value(rulesPath)) return failUsage(out, err, asText, "--rules needs a file", "--rules r.json");
            } else if (arg == "--backdrops") {
                if (!value(given)) return failUsage(out, err, asText, "--backdrops needs values", "#hex,#hex");
                size_t start = 0;
                while (start <= given.size()) {
                    size_t comma = given.find(',', start);
                    if (comma == std::string::npos) comma = given.size();
                    spec.backdrops.push_back(given.substr(start, comma - start));
                    start = comma + 1;
                }
            } else if (arg == "--fail-on") {
                if (!value(given) || (given != "fail" && given != "warn")) return failUsage(out, err, asText, "--fail-on is fail or warn", "fail or warn");
                spec.failOnWarn = given == "warn";
            } else if (arg == "--image") {
                if (!value(imagePath)) return failUsage(out, err, asText, "--image needs a path", "--image out.png");
            } else if (arg == "--project") {
                if (!value(project)) return failUsage(out, err, asText, "--project needs a directory", "--project <dir>");
            } else if (arg == "--format") {
                if (!value(given) || (given != "json" && given != "text")) return failUsage(out, err, asText, "--format is json or text", "json or text");
                reportFormat = given;
                asText = given == "text";
            } else if (arg == "-h" || arg == "--help") {
                out << "usage: lint-pixel-art <in>... [--palette p] [--lints a,b] [--rules r.json] [--backdrops #hex,...] [--fail-on fail|warn] [--image out.png] [--project dir] [--format text]\n";
                return kExitOk;
            } else if (!arg.empty() && arg[0] == '-') {
                return failUsage(out, err, asText, "unknown option " + arg, "see lint-pixel-art --help");
            } else {
                inputs.push_back(arg);
            }
        }
        if (inputs.empty()) return failUsage(out, err, asText, "no input file", "give an image or .aseprite file");
        if (!rulesPath.empty()) {
            json rules;
            std::string error;
            if (!readJsonFile(rulesPath, rules, error) || !rules.is_object()) return failUsage(out, err, asText, error.empty() ? "--rules must hold an object" : error, "{\"max_colors\": 16}");
            if (rules.contains("max_colors") && rules["max_colors"].is_number_integer()) spec.rules.maxColors = rules["max_colors"].get<int>();
            if (rules.contains("transparent_background")) spec.rules.transparentBackground = rules.value("transparent_background", false);
            if (rules.contains("sizes") && rules["sizes"].is_array()) {
                for (const json& size : rules["sizes"]) {
                    if (size.is_array() && size.size() == 2 && size[0].is_number_integer() && size[1].is_number_integer()) spec.rules.sizes.push_back({size[0].get<int>(), size[1].get<int>()});
                    else return failUsage(out, err, asText, "--rules sizes are [[w, h], ...]", "e.g. {\"sizes\": [[16, 16]]}");
                }
            }
        }
        spec.projectDir = project.empty() ? fs::path() : fs::u8path(project);

        Verdict worstVerdict = Verdict::Clean;
        bool failed = false;
        json files = json::array();
        for (const std::string& input : inputs) {
            spec.source = projectRelativeArg(spec.projectDir, input);
            std::vector<PixelProblem> problems;
            std::optional<LintPrepared> prepared = prepareLint(nullptr, spec, problems);
            if (!prepared) {
                failed = true;
                const bool usage = !problems.empty() && problems.front().kind == "bad_recipe";
                json list = json::array();
                for (const PixelProblem& p : problems) {
                    list.push_back(problemJson(p));
                    if (asText) err << input << ": " << p.kind << ": " << p.message << "\n";
                }
                files.push_back({{"source", input}, {"errors", list}});
                if (usage) return failUsage(out, err, asText, problems.front().message, problems.front().fix);
                continue;
            }
            const LintOutcome outcome = runLint(*prepared);
            worstVerdict = worse(worstVerdict, outcome.verdict);
            json entry = toJson(*prepared, outcome);
            entry["source"] = input;
            if (!imagePath.empty()) {
                const PixelImage image = renderLintImage(*prepared, outcome);
                if (!image.empty() && image.savePng(fs::u8path(imagePath))) entry["image"] = imagePath;
            }
            if (asText) {
                out << input << ": " << outcome.summary << "\n";
                for (const lint::LintResult& result : outcome.report.results) {
                    if (result.status == lint::LintStatus::Pass || result.status == lint::LintStatus::Skip) continue;
                    out << "  " << lint::lintStatusName(result.status) << " " << result.id << ": " << result.message << "\n";
                }
            }
            files.push_back(std::move(entry));
        }
        json report = inputs.size() == 1 ? files.front() : json{{"verdict", verdictName(worstVerdict)}, {"files", files}};
        if (!asText) out << report.dump() << "\n";
        if (failed) return kExitFailed;
        return verdictExit(worstVerdict);
    }

}
