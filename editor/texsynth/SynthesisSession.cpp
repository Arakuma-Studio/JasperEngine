// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SynthesisSession.h"

#include "mcp/McpServer.h"
#include "mcp/tools/W4Shared.h"
#include "util/Util.h"

#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;

namespace jasperengine::editor::texsynth {

    namespace {

        bool isSeparator(char c) { return c == '_' || c == '-' || c == '.' || c == ' '; }

        std::string lower(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // The words PBR map files end with
        bool isMapKeyword(const std::string& word) {
            static const std::set<std::string> keywords = {
                "albedo", "basecolor", "base", "diffuse", "color", "colour", "col", "diff",
                "normal", "normals", "nrm", "nor", "normalgl", "normaldx", "nrml",
                "roughness", "rough", "rgh", "smoothness", "gloss", "glossiness",
                "height", "displacement", "disp", "depth", "bump",
                "ao", "occlusion", "ambientocclusion", "cavity", "curvature",
                "metallic", "metalness", "metal", "mtl", "specular", "spec",
                "emissive", "emission", "opacity", "alpha", "orm", "arm", "mra", "rma"};
            return keywords.count(lower(word)) > 0;
        }

        std::string firstToken(const std::string& text) {
            size_t end = 0;
            while (end < text.size() && !isSeparator(text[end])) end++;
            return text.substr(0, end);
        }

        // The example's name without its map keyword: rock_albedo -> rock, rock -> rock, albedo -> ""
        std::string baseOf(const std::string& stem) {
            size_t start = stem.size();
            while (start > 0 && !isSeparator(stem[start - 1])) start--;
            const std::string last = stem.substr(start);
            if (!isMapKeyword(last)) return stem;
            size_t end = start;
            while (end > 0 && isSeparator(stem[end - 1])) end--;
            return stem.substr(0, end);
        }

        bool isPng(const std::string& path) {
            return lower(fs::u8path(path).extension().u8string()) == ".png";
        }

        void add(std::vector<Problem>& problems, const std::string& field, const std::string& message) {
            problems.push_back({field, message});
        }

        std::string fixed(double v, int digits) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
            return buffer;
        }

        std::string percent(double fraction) {
            const double p = fraction * 100.0;
            if (p > 0.0 && p < 1.0) return fixed(p, 1) + "%";
            return fixed(p, 0) + "%";
        }

        // ---- running workers (SynthesisRun::cancelAll)
        std::mutex runsMutex;
        std::condition_variable runsDone;
        std::vector<std::weak_ptr<SynthesisRun>> runs;
        int activeRuns = 0;

    }

    // ------------------------------------------------------------------ spec

    std::string SynthesisSpec::fingerprint() const {
        json j;
        j["mode"] = toString(mode);
        json ex = json::array();
        for (const ExampleSpec& e : examples) ex.push_back({e.path, e.sampleMask, e.guide, e.sampleable});
        j["examples"] = ex;
        json co = json::array();
        // where the files go is not part of the result (the window saves a result anywhere)
        for (const CompanionSpec& c : companions) co.push_back({c.name, c.paths, c.enabled});
        j["companions"] = co;
        if (mode == Mode::Inpaint) {
            j["mask"] = {static_cast<int>(maskSource), maskPath, toString(maskChannel), maskInvert};
            if (maskSource == MaskSource::Painted && paintedMask) {
                j["painted"] = sha1Hex(paintedMask->data.data(), paintedMask->data.size());
            }
        }
        if (mode == Mode::Guided) j["targetGuide"] = targetGuide;
        if (!keepsExampleSize(mode)) j["size"] = {width, height};
        j["quality"] = toString(quality);
        j["params"] = paramsToJson(resolveParams(quality, overrides));
        j["seed"] = std::to_string(seed);
        return j.dump();
    }

    // ------------------------------------------------------------------ modes

    bool isTileableMode(Mode mode) { return mode == Mode::Tileable || mode == Mode::MakeTileable; }

    bool keepsExampleSize(Mode mode) { return mode == Mode::MakeTileable || mode == Mode::Inpaint; }

    const char* modeTitle(Mode mode) {
        switch (mode) {
            case Mode::Extend: return "Extend";
            case Mode::Tileable: return "Tileable";
            case Mode::MakeTileable: return "Make Tileable";
            case Mode::Inpaint: return "Inpaint";
            case Mode::Guided: return "Guided";
        }
        return "?";
    }

    const char* modeHelp(Mode mode) {
        switch (mode) {
            case Mode::Extend: return "A new texture of any size that looks like the examples (does not repeat seamlessly).";
            case Mode::Tileable: return "A new texture of any size that repeats seamlessly in both directions.";
            case Mode::MakeTileable: return "The example itself made seamless: only a band along its edges is regenerated.";
            case Mode::Inpaint: return "The masked region of the example regenerated from the rest (remove a blemish, a logo, a seam).";
            case Mode::Guided: return "The examples rearranged to follow a target guide image (layout from the guide, detail from the examples).";
        }
        return "";
    }

    // ------------------------------------------------------------------ paths

    bool resolveProjectFile(const fs::path& projectDir, const std::string& rel, fs::path& absolute, std::string& error) {
        if (rel.empty()) {
            error = "no path given";
            return false;
        }
        const fs::path given = fs::u8path(rel);
        if (given.is_absolute() || given.has_root_name() || given.has_root_directory()) {
            error = "\"" + rel + "\" is absolute: give a project-relative path (e.g. \"assets/rock_albedo.png\")";
            return false;
        }
        const fs::path normal = (projectDir / given).lexically_normal();
        const fs::path back = normal.lexically_relative(projectDir.lexically_normal());
        if (back.empty() || *back.begin() == "..") {
            error = "\"" + rel + "\" is outside the project";
            return false;
        }
        absolute = normal;
        return true;
    }

    std::string projectRelative(const fs::path& projectDir, const fs::path& absolute) {
        const fs::path rel = absolute.lexically_normal().lexically_relative(projectDir.lexically_normal());
        if (rel.empty() || *rel.begin() == ".." || rel.is_absolute()) return absolute.generic_u8string();
        return rel.generic_u8string();
    }

    std::string defaultOutputPath(const std::string& examplePath, Mode mode) {
        const fs::path example = fs::u8path(examplePath);
        const std::string suffix = isTileableMode(mode) ? "_tile.png" : "_synth.png";
        // rock_albedo.png -> rock_tile.png, so the companions follow as rock_tile_normal.png
        // (the naming rule appends their suffix to the output's stem); rock.png -> rock_tile.png
        std::string stem = baseOf(example.stem().u8string());
        if (stem.empty()) stem = example.stem().u8string();
        return (example.parent_path() / fs::u8path(stem + suffix)).generic_u8string();
    }

    std::string paintedMaskPath(const std::string& output) {
        const fs::path out = fs::u8path(output);
        return (out.parent_path() / fs::u8path(out.stem().u8string() + "_inpaint_mask.png")).generic_u8string();
    }

    std::vector<CompanionCandidate> findCompanions(const fs::path& projectDir, const std::string& examplePath) {
        std::vector<CompanionCandidate> found;
        fs::path example;
        std::string error;
        if (!resolveProjectFile(projectDir, examplePath, example, error)) return found;
        const std::string stem = example.stem().u8string();
        const std::string base = baseOf(stem);
        std::error_code ec;
        std::vector<fs::path> siblings;
        for (const auto& entry : fs::directory_iterator(example.parent_path(), ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const fs::path path = entry.path();
            if (path.filename() == example.filename() || !Util::isImageFile(path.filename().u8string())) continue;
            if (lower(path.extension().u8string()) == ".svg") continue;
            siblings.push_back(path);
        }
        std::sort(siblings.begin(), siblings.end());
        for (const fs::path& path : siblings) {
            const std::string other = path.stem().u8string();
            if (!base.empty()) {
                if (other.size() <= base.size() || other.compare(0, base.size(), base) != 0 || !isSeparator(other[base.size()])) continue;
            }
            char separator = '_';
            std::string name = companionSuffix(stem, other, separator);
            if (name.empty() || !isMapKeyword(firstToken(name))) continue;
            if (name == "primary" || name == "uncertainty" || name == "inpaint_mask") name += "_map";
            CompanionCandidate candidate;
            candidate.name = name;
            candidate.path = projectRelative(projectDir, path);
            int c = 0;
            if (!stbi_info(path.string().c_str(), &candidate.width, &candidate.height, &c)) continue;
            found.push_back(candidate);
        }
        return found;
    }

    void autoCompanions(const fs::path& projectDir, SynthesisSpec& spec, std::vector<std::string>& warnings) {
        spec.companions.clear();
        if (spec.examples.empty()) return;
        // per example: name -> candidate
        std::vector<std::map<std::string, CompanionCandidate>> perExample;
        std::vector<std::pair<int, int>> sizes;
        for (const ExampleSpec& ex : spec.examples) {
            std::map<std::string, CompanionCandidate> byName;
            for (const CompanionCandidate& c : findCompanions(projectDir, ex.path)) byName.emplace(c.name, c);
            perExample.push_back(byName);
            fs::path abs;
            std::string error;
            int w = 0, h = 0, c = 0;
            if (resolveProjectFile(projectDir, ex.path, abs, error)) stbi_info(abs.string().c_str(), &w, &h, &c);
            sizes.push_back({w, h});
        }
        for (const auto& [name, first] : perExample.front()) {
            CompanionSpec companion;
            companion.name = name;
            bool usable = true;
            for (size_t e = 0; e < perExample.size() && usable; e++) {
                auto it = perExample[e].find(name);
                if (it == perExample[e].end()) {
                    warnings.push_back("companion '" + name + "' left out: " + spec.examples[e].path + " has no " + name + " map next to it");
                    usable = false;
                    break;
                }
                const CompanionCandidate& c = it->second;
                if (c.width != sizes[e].first || c.height != sizes[e].second) {
                    warnings.push_back("companion '" + name + "' left out: " + c.path + " is " + std::to_string(c.width) + "x" +
                                       std::to_string(c.height) + ", its example " + std::to_string(sizes[e].first) + "x" +
                                       std::to_string(sizes[e].second) + " (resize it to match)");
                    usable = false;
                    break;
                }
                companion.paths.push_back(c.path);
            }
            if (usable) spec.companions.push_back(companion);
        }
    }

    std::vector<PlannedOutput> plannedOutputs(const SynthesisSpec& spec) {
        std::vector<PlannedOutput> outputs;
        if (spec.output.empty()) return outputs;
        outputs.push_back({"primary", spec.output});
        for (const CompanionSpec& c : spec.companions) {
            if (!c.enabled) continue;
            std::string path = c.output;
            if (path.empty() && !spec.examples.empty() && !c.paths.empty()) {
                path = companionOutputPath(fs::u8path(spec.output), fs::u8path(spec.examples.front().path), fs::u8path(c.paths.front()))
                           .generic_u8string();
                if (path.empty()) {
                    const fs::path out = fs::u8path(spec.output);
                    path = (out.parent_path() / fs::u8path(out.stem().u8string() + "_" + c.name + ".png")).generic_u8string();
                }
            }
            outputs.push_back({c.name, path});
        }
        if (spec.mode == Mode::Inpaint && spec.maskSource == MaskSource::Painted) outputs.push_back({"inpaint_mask", paintedMaskPath(spec.output)});
        if (!spec.uncertaintyOutput.empty()) outputs.push_back({"uncertainty", spec.uncertaintyOutput});
        return outputs;
    }

    // ------------------------------------------------------------------ request

    std::vector<Problem> checkSpec(const fs::path& projectDir, const SynthesisSpec& spec) {
        std::vector<Problem> problems;
        std::set<std::string> inputs;
        std::error_code ec;
        auto source = [&](const std::string& field, const std::string& rel, int* w = nullptr, int* h = nullptr) {
            fs::path abs;
            std::string error;
            if (!resolveProjectFile(projectDir, rel, abs, error)) {
                add(problems, field, error);
                return;
            }
            if (!fs::is_regular_file(abs, ec)) {
                add(problems, field, rel + " does not exist");
                return;
            }
            inputs.insert(abs.lexically_normal().generic_u8string());
            int c = 0, iw = 0, ih = 0;
            if (!stbi_info(abs.string().c_str(), &iw, &ih, &c)) {
                add(problems, field, rel + " is not an image the synthesis can read (PNG, JPEG, TGA, BMP, PSD, GIF)");
                return;
            }
            if (w) *w = iw;
            if (h) *h = ih;
        };
        if (spec.examples.empty()) add(problems, "examples", "at least one example image is needed");
        std::vector<std::pair<int, int>> sizes;
        for (size_t e = 0; e < spec.examples.size(); e++) {
            const ExampleSpec& ex = spec.examples[e];
            const std::string field = "examples[" + std::to_string(e) + "]";
            int w = 0, h = 0;
            source(field + ".path", ex.path, &w, &h);
            sizes.push_back({w, h});
            if (!ex.sampleMask.empty()) source(field + ".sample_mask", ex.sampleMask);
            if (!ex.guide.empty()) {
                if (spec.mode != Mode::Guided) add(problems, field + ".guide", "example guides are only used in guided mode");
                else source(field + ".guide", ex.guide);
            }
        }
        if (!spec.examples.empty() && std::none_of(spec.examples.begin(), spec.examples.end(), [](const ExampleSpec& e) { return e.sampleable; })) {
            add(problems, "examples", "at least one example must be used for sampling");
        }
        std::set<std::string> names;
        for (size_t c = 0; c < spec.companions.size(); c++) {
            const CompanionSpec& companion = spec.companions[c];
            if (!companion.enabled) continue;
            const std::string field = "companions[" + std::to_string(c) + "]";
            if (companion.name.empty()) add(problems, field, "a companion needs a name");
            else if (!names.insert(companion.name).second) add(problems, field, "two companions are named '" + companion.name + "'");
            if (companion.paths.size() != spec.examples.size()) {
                add(problems, field, "companion '" + companion.name + "' needs one map per example (" + std::to_string(spec.examples.size()) +
                                         "), got " + std::to_string(companion.paths.size()));
                continue;
            }
            for (size_t e = 0; e < companion.paths.size(); e++) {
                int w = 0, h = 0;
                const size_t before = problems.size();
                source(field + ".paths[" + std::to_string(e) + "]", companion.paths[e], &w, &h);
                if (problems.size() == before && e < sizes.size() && sizes[e].first > 0 && (w != sizes[e].first || h != sizes[e].second)) {
                    add(problems, field, companion.paths[e] + " is " + std::to_string(w) + "x" + std::to_string(h) + " but its example " +
                                             spec.examples[e].path + " is " + std::to_string(sizes[e].first) + "x" + std::to_string(sizes[e].second) +
                                             ": companions must match exactly (resize it, or leave it out)");
                }
            }
        }
        if (spec.mode == Mode::Inpaint) {
            if (spec.maskSource == MaskSource::Image) {
                if (spec.maskPath.empty()) add(problems, "mask", "inpaint needs a mask image (white = regenerate), or paint the region");
                else source("mask", spec.maskPath);
            } else if (spec.maskSource == MaskSource::Painted) {
                bool any = false;
                if (spec.paintedMask) {
                    for (uint8_t v : spec.paintedMask->data) {
                        if (v >= 128) {
                            any = true;
                            break;
                        }
                    }
                }
                if (!any && !spec.maskInvert) add(problems, "mask", "paint the region to regenerate on the example first");
                if (spec.paintedMask && !sizes.empty() &&
                    (spec.paintedMask->width != sizes[0].first || spec.paintedMask->height != sizes[0].second)) {
                    add(problems, "mask", "the painted mask does not match the example's size: clear it and paint again");
                }
            }
        }
        if (spec.mode == Mode::Guided) {
            if (spec.targetGuide.empty()) add(problems, "target_guide", "guided mode needs a target guide image");
            else source("target_guide", spec.targetGuide);
        }
        if (!keepsExampleSize(spec.mode) && !(spec.mode == Mode::Guided && spec.width == 0 && spec.height == 0)) {
            if (spec.width < 1 || spec.height < 1 || spec.width > 8192 || spec.height > 8192) {
                add(problems, "size", "the output size must be 1..8192 on each side");
            } else if (static_cast<int64_t>(spec.width) * spec.height > 4096LL * 4096LL) {
                add(problems, "size", "at most 4096 x 4096 pixels (16.7 megapixels)");
            }
        }
        for (const Problem& p : validateParams(resolveParams(spec.quality, spec.overrides))) {
            add(problems, p.field, p.message);
        }
        // outputs: .png, inside the project, distinct, not an input
        std::set<std::string> written;
        for (const PlannedOutput& out : plannedOutputs(spec)) {
            const std::string field = out.role == "primary" ? "output" : "output." + out.role;
            if (!isPng(out.path)) {
                add(problems, field, "the " + out.role + " output must be a .png file: " + out.path);
                continue;
            }
            fs::path abs;
            std::string error;
            if (!resolveProjectFile(projectDir, out.path, abs, error)) {
                add(problems, field, error);
                continue;
            }
            const std::string key = abs.lexically_normal().generic_u8string();
            if (!written.insert(key).second) add(problems, field, "two outputs would be written to " + out.path);
            if (inputs.count(key)) add(problems, field, "the " + out.role + " output would replace an input: " + out.path + " (write it elsewhere)");
        }
        return problems;
    }

    bool buildRequest(const fs::path& projectDir, const SynthesisSpec& spec, Request& request, std::vector<Problem>& errors) {
        errors = checkSpec(projectDir, spec);
        if (!errors.empty()) return false;
        auto abs = [&](const std::string& rel) {
            fs::path path;
            std::string error;
            resolveProjectFile(projectDir, rel, path, error);
            return path;
        };
        request = Request();
        request.mode = spec.mode;
        for (const ExampleSpec& ex : spec.examples) {
            Example example = Example::fromPath(abs(ex.path));
            if (!ex.sampleMask.empty()) example.sampleMask = ImageInput::fromPath(abs(ex.sampleMask));
            if (spec.mode == Mode::Guided && !ex.guide.empty()) example.guide = ImageInput::fromPath(abs(ex.guide));
            example.sampleable = ex.sampleable;
            request.examples.push_back(example);
        }
        for (const CompanionSpec& c : spec.companions) {
            if (!c.enabled) continue;
            Companion companion;
            companion.name = c.name;
            for (const std::string& path : c.paths) companion.images.push_back(ImageInput::fromPath(abs(path)));
            request.companions.push_back(companion);
        }
        if (spec.mode == Mode::Inpaint) {
            if (spec.maskSource == MaskSource::Image) {
                request.inpaintMask.image = ImageInput::fromPath(abs(spec.maskPath));
                request.inpaintMask.channel = spec.maskChannel;
            } else if (spec.maskSource == MaskSource::Painted && spec.paintedMask) {
                request.inpaintMask.image = ImageInput::fromImage(*spec.paintedMask);
                request.inpaintMask.channel = MaskChannel::Luma;
            }
            request.inpaintMask.invert = spec.maskInvert;
        }
        if (spec.mode == Mode::Guided) request.targetGuide = ImageInput::fromPath(abs(spec.targetGuide));
        if (!keepsExampleSize(spec.mode)) {
            request.width = spec.width;
            request.height = spec.height;
        }
        request.quality = spec.quality;
        request.overrides = spec.overrides;
        request.seed = spec.seed;
        return true;
    }

    // ------------------------------------------------------------------ outputs

    bool encodeOutputs(const fs::path& projectDir, const SynthesisSpec& spec, Request& request, const Result& result,
                       std::vector<OutputFile>& files, std::string& error) {
        files.clear();
        if (result.status != Status::Ok) {
            error = "there is no finished result to write";
            return false;
        }
        const std::vector<PlannedOutput> planned = plannedOutputs(spec);
        if (planned.empty()) {
            error = "no output path";
            return false;
        }
        RecipePaths paths;
        paths.projectDir = projectDir;
        std::map<std::string, fs::path> absolute;
        for (const PlannedOutput& out : planned) {
            fs::path abs;
            if (!resolveProjectFile(projectDir, out.path, abs, error)) return false;
            absolute[out.role] = abs;
            if (out.role != "inpaint_mask") paths.outputs[out.role] = abs;
        }
        // a painted mask: saved with the result and named by the recipe, so it can be regenerated
        std::string maskBytes;
        if (absolute.count("inpaint_mask")) {
            const ImageInput& mask = request.inpaintMask.image;
            std::vector<uint8_t> png;
            if (mask.image.empty() || !encodePng(mask.image, png)) {
                error = "the painted mask could not be encoded";
                return false;
            }
            maskBytes.assign(png.begin(), png.end());
            request.inpaintMask.image.path = absolute["inpaint_mask"];
            request.inpaintMask.image.sha1 = sha1Hex(png.data(), png.size());
        }
        for (const PlannedOutput& out : planned) {
            OutputFile file;
            file.role = out.role;
            file.path = projectRelative(projectDir, absolute[out.role]);
            file.absolute = absolute[out.role];
            if (out.role == "inpaint_mask") {
                file.bytes = maskBytes;
                files.push_back(std::move(file));
                continue;
            }
            const Image* image = &result.primary;
            if (out.role == "uncertainty") image = &result.uncertaintyMap;
            for (const CompanionOutput& c : result.companions) {
                if (c.name == out.role) image = &c.image;
            }
            std::vector<uint8_t> png, withRecipe;
            if (!encodePng(*image, png)) {
                error = "PNG encoding failed for the " + out.role + " output (" + describeFormat(*image) + ")";
                return false;
            }
            paths.outputDir = file.absolute.parent_path();
            if (!insertRecipeChunk(png, makeRecipe(request, result, out.role, paths).dump(), withRecipe, error)) return false;
            file.bytes.assign(withRecipe.begin(), withRecipe.end());
            files.push_back(std::move(file));
        }
        return true;
    }

    bool writeOutputsUndoable(const std::vector<OutputFile>& files, std::string& error) {
        std::vector<mcp::w4::WriteFilesCmd::Item> items;
        for (const OutputFile& file : files) items.push_back({file.absolute, file.bytes});
        return mcp::w4::writeFilesUndoable(mcp::McpServer::get().context(), std::move(items), error);
    }

    // ------------------------------------------------------------------ feedback

    std::string seamWords(double score) {
        const std::string value = fixed(score, 2);
        if (score <= 1.25) return "none (" + value + ")";
        if (score <= 1.5) return "faint (" + value + ")";
        return "visible (" + value + ")";
    }

    Assessment assess(const SynthesisSpec& spec, const Result& result) {
        Assessment a;
        const Metrics& m = result.metrics;
        std::vector<std::string> hints;
        if (isTileableMode(spec.mode)) {
            a.lines.push_back("Seams: " + seamWords(m.seamScore));
            if (m.seamScore > 1.5) {
                a.seamVisible = true;
                if (spec.mode == Mode::Tileable) {
                    hints.push_back(spec.quality == Quality::High ? "the seam is visible: try another seed, or make_tileable on a larger example"
                                                                  : "the seam is visible: try quality high, or another seed");
                } else {
                    const float border = resolveParams(spec.quality, spec.overrides).borderFraction;
                    hints.push_back(border < 0.2f ? "the seam is visible: widen the border (0.2-0.25) or try quality high"
                                                  : "the seam is visible: try quality high or another seed");
                }
            }
        } else {
            a.lines.push_back(std::string("Tiles: ") + (m.seamScore <= 1.5 ? "about seamlessly" : "no") + " (seam " + fixed(m.seamScore, 2) + ")" +
                              (spec.mode == Mode::Extend && m.seamScore > 1.5 ? ", use Tileable for a repeating texture" : ""));
        }
        a.lines.push_back("Uncertain pixels: " + percent(m.highUncertaintyFraction) + " (mean " + fixed(m.meanUncertainty, 3) + ")");
        if (m.highUncertaintyFraction > 0.15) {
            a.highUncertainty = true;
            hints.push_back(spec.mode == Mode::Inpaint
                                ? "many regenerated pixels found no good match: paint a smaller region, or give more examples of the surroundings"
                                : "many pixels found no good match: the example may be too regular or structured (bricks, tiles, text) or too "
                                  "small for this output; try quality high, a larger or more varied example, or expect artifacts");
        } else if (m.highUncertaintyFraction > 0.05) {
            hints.push_back("some pixels matched poorly (look at the uncertainty map); quality high or another seed may help");
        }
        if (m.lockedPixels > 0) {
            a.lines.push_back("Kept: " + percent(static_cast<double>(m.lockedPixels) / std::max<int64_t>(1, m.lockedPixels + m.synthesizedPixels)) +
                              " of the pixels");
        }
        for (size_t i = 0; i < hints.size(); i++) a.hint += (i ? "; " : "") + hints[i];
        if (!a.hint.empty()) {
            a.hint[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(a.hint[0])));
            a.hint += ".";
        }
        return a;
    }

    // ------------------------------------------------------------------ recipes

    bool specFromRecipe(const json& recipe, const fs::path& pngFile, const fs::path& projectDir, SynthesisSpec& spec,
                        std::vector<std::string>& notes, std::string& error) {
        if (!recipe.is_object() || recipe.value("format", 0) < 1) {
            error = "not a texture synthesis recipe";
            return false;
        }
        spec = SynthesisSpec();
        if (!parseMode(recipe.value("mode", std::string()), spec.mode)) {
            error = "the recipe's mode \"" + recipe.value("mode", std::string()) + "\" is unknown";
            return false;
        }
        if (!parseQuality(recipe.value("quality", std::string("balanced")), spec.quality)) spec.quality = Quality::Balanced;
        const bool fromProject = recipe.value("pathBase", std::string("file")) == "project";
        const fs::path base = fromProject ? projectDir : pngFile.parent_path();
        auto rel = [&](const json& entry) -> std::string {
            if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string()) return std::string();
            const fs::path recorded = fs::u8path(entry["path"].get<std::string>());
            return projectRelative(projectDir, recorded.is_absolute() ? recorded : base / recorded);
        };
        if (recipe.contains("params") && recipe["params"].is_object()) {
            const json& p = recipe["params"];
            ParamOverrides& o = spec.overrides;
            if (p.contains("nearestNeighbors")) o.nearestNeighbors = p["nearestNeighbors"].get<int>();
            if (p.contains("randomSampleLocations")) o.randomSampleLocations = p["randomSampleLocations"].get<int>();
            if (p.contains("cauchyDispersion")) o.cauchyDispersion = p["cauchyDispersion"].get<float>();
            if (p.contains("backtrackPercent")) o.backtrackPercent = p["backtrackPercent"].get<float>();
            if (p.contains("backtrackStages")) o.backtrackStages = p["backtrackStages"].get<int>();
            if (p.contains("guideAlpha")) o.guideAlpha = p["guideAlpha"].get<float>();
            if (p.contains("guideDecay")) o.guideDecay = p["guideDecay"].get<bool>();
            if (p.contains("borderFraction")) o.borderFraction = p["borderFraction"].get<float>();
            // only what differs from the preset stays an override
            const Params preset = presetParams(spec.quality);
            if (o.nearestNeighbors == preset.nearestNeighbors) o.nearestNeighbors.reset();
            if (o.randomSampleLocations == preset.randomSampleLocations) o.randomSampleLocations.reset();
            if (o.cauchyDispersion && std::abs(*o.cauchyDispersion - preset.cauchyDispersion) < 1e-6f) o.cauchyDispersion.reset();
            if (o.backtrackPercent && std::abs(*o.backtrackPercent - preset.backtrackPercent) < 1e-6f) o.backtrackPercent.reset();
            if (o.backtrackStages == preset.backtrackStages) o.backtrackStages.reset();
            if (o.guideAlpha && std::abs(*o.guideAlpha - preset.guideAlpha) < 1e-6f) o.guideAlpha.reset();
            if (o.guideDecay == preset.guideDecay) o.guideDecay.reset();
            if (o.borderFraction && std::abs(*o.borderFraction - preset.borderFraction) < 1e-6f) o.borderFraction.reset();
        }
        if (recipe.contains("seed")) {
            const json& seed = recipe["seed"];
            if (seed.is_number_unsigned() || seed.is_number_integer()) spec.seed = seed.get<uint64_t>();
            else if (seed.is_string()) spec.seed = std::strtoull(seed.get<std::string>().c_str(), nullptr, 10);
        }
        if (recipe.contains("size") && recipe["size"].is_array() && recipe["size"].size() == 2) {
            spec.width = recipe["size"][0].get<int>();
            spec.height = recipe["size"][1].get<int>();
        }
        const json examples = recipe.value("examples", json::array());
        for (size_t i = 0; i < examples.size(); i++) {
            ExampleSpec ex;
            ex.path = rel(examples[i]);
            if (ex.path.empty()) {
                error = "the recipe's example " + std::to_string(i + 1) + " was given in memory (no file): it can't be restored";
                return false;
            }
            ex.sampleable = examples[i].value("sampleable", true);
            if (examples[i].contains("sampleMask")) ex.sampleMask = rel(examples[i]["sampleMask"]);
            if (examples[i].contains("guide")) ex.guide = rel(examples[i]["guide"]);
            spec.examples.push_back(ex);
        }
        if (spec.examples.empty()) {
            error = "the recipe names no example";
            return false;
        }
        const json outputs = recipe.value("outputs", json::object());
        auto outputPath = [&](const std::string& role) -> std::string {
            if (!outputs.contains(role) || !outputs[role].is_string()) return std::string();
            const fs::path recorded = fs::u8path(outputs[role].get<std::string>());
            return projectRelative(projectDir, recorded.is_absolute() ? recorded : base / recorded);
        };
        spec.output = outputPath("primary");
        if (spec.output.empty()) spec.output = projectRelative(projectDir, pngFile);
        spec.uncertaintyOutput = outputPath("uncertainty");
        for (const json& c : recipe.value("companions", json::array())) {
            CompanionSpec companion;
            companion.name = c.value("name", std::string());
            for (const json& source : c.value("sources", json::array())) companion.paths.push_back(rel(source));
            if (std::any_of(companion.paths.begin(), companion.paths.end(), [](const std::string& p) { return p.empty(); })) {
                notes.push_back("companion '" + companion.name + "' had a source given in memory: left out");
                continue;
            }
            const std::string out = outputPath(companion.name);
            // the naming rule's own path stays implicit (it follows the output when that is renamed)
            spec.companions.push_back(companion);
            if (!out.empty()) {
                SynthesisSpec probe = spec;
                const std::vector<PlannedOutput> planned = plannedOutputs(probe);
                bool sameAsRule = false;
                for (const PlannedOutput& p : planned) {
                    if (p.role == companion.name && p.path == out) sameAsRule = true;
                }
                if (!sameAsRule) spec.companions.back().output = out;
            }
        }
        if (spec.mode == Mode::Inpaint) {
            const json mask = recipe.value("inpaintMask", json::object());
            if (mask.value("source", std::string()) == "alpha") {
                spec.maskSource = MaskSource::Alpha;
            } else if (!rel(mask).empty()) {
                spec.maskSource = MaskSource::Image;
                spec.maskPath = rel(mask);
                parseMaskChannel(mask.value("channel", std::string("luma")), spec.maskChannel);
            } else {
                spec.maskSource = MaskSource::Painted;
                notes.push_back("the inpaint mask was painted and not saved: paint the region again");
            }
            spec.maskInvert = mask.value("invert", false);
        }
        if (spec.mode == Mode::Guided) {
            spec.targetGuide = rel(recipe.value("targetGuide", json::object()));
            if (spec.targetGuide.empty()) notes.push_back("the target guide was given in memory: pick it again");
        }
        if (keepsExampleSize(spec.mode)) {
            spec.width = kDefaultSize;
            spec.height = kDefaultSize;
        }
        return true;
    }

    // ------------------------------------------------------------------ SynthesisRun

    std::shared_ptr<SynthesisRun> SynthesisRun::start(Request request, const Options& options, Finished onFinished) {
        std::shared_ptr<SynthesisRun> run(new SynthesisRun());
        run->req = std::move(request);
        if (options.threads > 0) run->req.threads = options.threads;
        {
            std::lock_guard<std::mutex> lock(runsMutex);
            runs.erase(std::remove_if(runs.begin(), runs.end(), [](const std::weak_ptr<SynthesisRun>& w) { return w.expired(); }), runs.end());
            runs.push_back(run);
            activeRuns++;
        }
        std::thread([run, options, onFinished]() {
            Control control;
            control.cancel = &run->cancelFlag;
            control.previewIntervalSeconds = options.previewIntervalSeconds;
            control.previewMaxSize = options.previewMaxSize;
            control.onProgress = [run, forward = options.onProgress](const Progress& p) {
                {
                    std::lock_guard<std::mutex> lock(run->mutex);
                    run->last = p;
                    run->last.preview.reset();
                    if (p.preview) {
                        run->preview = p.preview;
                        run->previewSerial++;
                    }
                }
                if (forward) forward(p);
            };
            Result result = synthesize(run->req, control);
            run->res = std::move(result);
            run->done = true;
            if (onFinished) onFinished(*run);
            std::lock_guard<std::mutex> lock(runsMutex);
            activeRuns--;
            runsDone.notify_all();
        }).detach();
        return run;
    }

    void SynthesisRun::cancelAll() {
        std::unique_lock<std::mutex> lock(runsMutex);
        for (const std::weak_ptr<SynthesisRun>& w : runs) {
            if (std::shared_ptr<SynthesisRun> run = w.lock()) run->cancel();
        }
        runsDone.wait_for(lock, std::chrono::seconds(10), []() { return activeRuns == 0; });
    }

    Progress SynthesisRun::progress() const {
        std::lock_guard<std::mutex> lock(mutex);
        return last;
    }

    std::shared_ptr<const Image> SynthesisRun::latestPreview(uint64_t* serial) const {
        std::lock_guard<std::mutex> lock(mutex);
        if (serial) *serial = previewSerial;
        return preview;
    }

}
