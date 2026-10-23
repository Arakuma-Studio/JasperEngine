// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Texture synthesis: synthesize_texture (texsynth, the same code as Window > Texture Synthesis
// through texsynth/SynthesisSession), and the recipe summary get_asset_info / view_asset show
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "App.h"
#include "EditorHost.h"
#include "Project.h"
#include "texsynth/SynthesisRecipe.h"
#include "texsynth/SynthesisSession.h"
#include "util/Base64.h"
#include "window/TextureSynthesisWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;
namespace ts = texsynth;

namespace {

constexpr double MAX_SEED = 9007199254740992.0;   // 2^53: exact in JSON numbers

std::string fixed(double v, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
    return buffer;
}

double rounded(double v, int digits) {
    const double scale = std::pow(10.0, digits);
    return std::round(v * scale) / scale;
}

// The request field names of the core and the session as the tool's argument names
std::string argumentField(const std::string& field) {
    static const std::vector<std::pair<std::string, std::string>> names = {
        {"params.nearestNeighbors", "params.nearest_neighbors"},
        {"params.randomSampleLocations", "params.random_sample_locations"},
        {"params.cauchyDispersion", "params.cauchy_dispersion"},
        {"params.backtrackPercent", "params.backtrack_percent"},
        {"params.backtrackStages", "params.backtrack_stages"},
        {"params.guideAlpha", "params.guide_alpha"},
        {"params.borderFraction", "border"},
        {"sampleMask", "sample_mask"},
        {"targetGuide", "target_guide"},
        {"inpaintMask", "mask"},
        {".image", ".path"},
        {"width", "size"},
        {"height", "size"}};
    std::string out = field;
    for (const auto& [from, to] : names) {
        const size_t at = out.find(from);
        if (at != std::string::npos) out.replace(at, from.size(), to);
    }
    return out;
}

std::string problemsText(const std::vector<ts::Problem>& problems) {
    std::string text;
    for (const ts::Problem& p : problems) text += "\n- " + argumentField(p.field) + ": " + p.message;
    return text;
}

Json problemsJson(const std::vector<ts::Problem>& problems) {
    Json list = Json::array();
    for (const ts::Problem& p : problems) list.push_back({{"field", argumentField(p.field)}, {"message", p.message}});
    return list;
}

// A path argument: project-relative, sandboxed (McpRefs), returned normalized
bool pathArg(Project* project, const Json& value, const std::string& field, bool mustExist, std::string& out, std::string& error) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        error = field + " must be a project-relative path string (e.g. \"assets/textures/rock_albedo.png\").";
        return false;
    }
    fs::path absolute;
    if (!McpRefs::resolveProjectPath(project, value.get<std::string>(), absolute, error, mustExist)) {
        error = field + ": " + error;
        return false;
    }
    out = ts::projectRelative(project->getProjectPath(), absolute);
    return true;
}

bool numberArg(const Json& params, const char* key, double minimum, double maximum, std::optional<double>& out, std::string& error) {
    if (!params.contains(key)) return true;
    const Json& v = params[key];
    if (!v.is_number() || v.get<double>() < minimum || v.get<double>() > maximum) {
        error = std::string("params.") + key + " must be a number in [" + fixed(minimum, minimum == std::floor(minimum) ? 0 : 2) + ", " +
                fixed(maximum, maximum == std::floor(maximum) ? 0 : 2) + "] (got " + v.dump() + ").";
        return false;
    }
    out = v.get<double>();
    return true;
}

// The tool's arguments as a spec; false with a fix-it error
bool specFromArgs(Project* project, const Json& args, ts::SynthesisSpec& spec, bool& seedGiven, std::vector<std::string>& warnings,
                  std::string& error) {
    const fs::path root = project->getProjectPath();
    spec = ts::SynthesisSpec();
    const std::string mode = args.value("mode", std::string("tileable"));
    if (!ts::parseMode(mode, spec.mode)) {
        error = "mode must be extend, tileable, make_tileable, inpaint or guided (got \"" + mode + "\").";
        return false;
    }
    const std::string quality = args.value("quality", std::string("balanced"));
    if (!ts::parseQuality(quality, spec.quality)) {
        error = "quality must be draft, balanced or high (got \"" + quality + "\").";
        return false;
    }

    // examples
    if (!args.contains("examples") || !args["examples"].is_array() || args["examples"].empty()) {
        error = "examples is required: an array of project-relative image paths (or {path, sample_mask?, guide?, sampleable?}), "
                "e.g. [\"assets/textures/rock_albedo.png\"].";
        return false;
    }
    for (size_t i = 0; i < args["examples"].size(); i++) {
        const Json& item = args["examples"][i];
        const std::string field = "examples[" + std::to_string(i) + "]";
        ts::ExampleSpec ex;
        if (item.is_string()) {
            if (!pathArg(project, item, field, true, ex.path, error)) return false;
        } else if (item.is_object()) {
            for (auto it = item.begin(); it != item.end(); ++it) {
                static const std::set<std::string> keys = {"path", "sample_mask", "guide", "sampleable"};
                if (!keys.count(it.key())) {
                    error = field + " has an unknown key \"" + it.key() + "\" (allowed: path, sample_mask, guide, sampleable).";
                    return false;
                }
            }
            if (!pathArg(project, item.value("path", Json()), field + ".path", true, ex.path, error)) return false;
            if (item.contains("sample_mask") && !pathArg(project, item["sample_mask"], field + ".sample_mask", true, ex.sampleMask, error)) return false;
            if (item.contains("guide") && !pathArg(project, item["guide"], field + ".guide", true, ex.guide, error)) return false;
            if (item.contains("sampleable")) {
                if (!item["sampleable"].is_boolean()) {
                    error = field + ".sampleable must be true or false.";
                    return false;
                }
                ex.sampleable = item["sampleable"].get<bool>();
            }
        } else {
            error = field + " must be a path string or {path, sample_mask?, guide?, sampleable?}.";
            return false;
        }
        spec.examples.push_back(ex);
    }

    // companions
    const Json companions = args.contains("companions") ? args["companions"] : Json("auto");
    if (companions.is_string()) {
        if (companions == "auto") {
            ts::autoCompanions(root, spec, warnings);
        } else if (companions != "none") {
            error = "companions must be \"auto\" (the PBR maps next to each example, e.g. rock_normal.png for rock_albedo.png), \"none\", "
                    "or an array of paths / {path | paths, name?, output?}.";
            return false;
        }
    } else if (companions.is_array()) {
        for (size_t c = 0; c < companions.size(); c++) {
            const Json& item = companions[c];
            const std::string field = "companions[" + std::to_string(c) + "]";
            ts::CompanionSpec companion;
            std::vector<Json> paths;
            if (item.is_string()) {
                paths.push_back(item);
            } else if (item.is_object()) {
                for (auto it = item.begin(); it != item.end(); ++it) {
                    static const std::set<std::string> keys = {"path", "paths", "name", "output"};
                    if (!keys.count(it.key())) {
                        error = field + " has an unknown key \"" + it.key() + "\" (allowed: path, paths, name, output).";
                        return false;
                    }
                }
                if (item.contains("paths") && item["paths"].is_array()) {
                    for (const Json& p : item["paths"]) paths.push_back(p);
                } else if (item.contains("path")) {
                    paths.push_back(item["path"]);
                }
                if (item.contains("name")) {
                    if (!item["name"].is_string()) {
                        error = field + ".name must be a string (\"normal\").";
                        return false;
                    }
                    companion.name = item["name"].get<std::string>();
                }
                if (item.contains("output") && !pathArg(project, item["output"], field + ".output", false, companion.output, error)) return false;
            } else {
                error = field + " must be a path string or {path | paths, name?, output?}.";
                return false;
            }
            if (paths.size() != spec.examples.size()) {
                error = field + " needs one map per example (" + std::to_string(spec.examples.size()) + "): give {\"paths\": [...]} in "
                        "example order.";
                return false;
            }
            for (size_t e = 0; e < paths.size(); e++) {
                std::string rel;
                if (!pathArg(project, paths[e], field + (paths.size() > 1 ? ".paths[" + std::to_string(e) + "]" : ".path"), true, rel, error)) return false;
                companion.paths.push_back(rel);
            }
            if (companion.name.empty()) {
                char separator = '_';
                companion.name = ts::companionSuffix(fs::u8path(spec.examples[0].path).stem().u8string(),
                                                     fs::u8path(companion.paths[0]).stem().u8string(), separator);
                if (companion.name.empty()) companion.name = "companion" + std::to_string(c + 1);
            }
            spec.companions.push_back(companion);
        }
    } else {
        error = "companions must be \"auto\", \"none\" or an array.";
        return false;
    }

    // inpaint mask
    if (args.contains("mask")) {
        if (spec.mode != ts::Mode::Inpaint) {
            error = "mask is only used in inpaint mode (mode: \"inpaint\").";
            return false;
        }
        const Json& mask = args["mask"];
        if (!mask.is_object()) {
            error = "mask must be {path?, channel?, invert?}: path a mask image (white = regenerate); without path the example's "
                    "transparent pixels are regenerated.";
            return false;
        }
        for (auto it = mask.begin(); it != mask.end(); ++it) {
            if (it.key() != "path" && it.key() != "channel" && it.key() != "invert") {
                error = "mask has an unknown key \"" + it.key() + "\" (allowed: path, channel, invert).";
                return false;
            }
        }
        if (mask.contains("path")) {
            if (!pathArg(project, mask["path"], "mask.path", true, spec.maskPath, error)) return false;
            spec.maskSource = ts::MaskSource::Image;
        } else {
            spec.maskSource = ts::MaskSource::Alpha;
        }
        const std::string channel = mask.value("channel", std::string("luma"));
        if (!ts::parseMaskChannel(channel, spec.maskChannel)) {
            error = "mask.channel must be luma, r, g, b or a (got \"" + channel + "\").";
            return false;
        }
        spec.maskInvert = mask.value("invert", false);
    } else if (spec.mode == ts::Mode::Inpaint) {
        spec.maskSource = ts::MaskSource::Alpha;
    }
    if (args.contains("target_guide")) {
        if (spec.mode != ts::Mode::Guided) {
            error = "target_guide is only used in guided mode (mode: \"guided\").";
            return false;
        }
        if (!pathArg(project, args["target_guide"], "target_guide", true, spec.targetGuide, error)) return false;
    }

    // size
    if (args.contains("size") && !args["size"].is_null()) {
        if (ts::keepsExampleSize(spec.mode)) {
            error = std::string(ts::toString(spec.mode)) + " keeps the example's size: leave size out.";
            return false;
        }
        const Json& size = args["size"];
        if (size.is_number_integer()) {
            spec.width = spec.height = size.get<int>();
        } else if (size.is_object() && size.size() == 2 && size.contains("width") && size.contains("height") &&
                   size["width"].is_number_integer() && size["height"].is_number_integer()) {
            spec.width = size["width"].get<int>();
            spec.height = size["height"].get<int>();
        } else {
            error = "size must be an integer (square) or {width, height}, e.g. 1024 or {\"width\": 2048, \"height\": 1024}.";
            return false;
        }
    } else if (spec.mode == ts::Mode::Guided) {
        spec.width = spec.height = 0;   // the target guide's size
    } else {
        spec.width = spec.height = ts::kDefaultSize;
    }

    // params
    if (args.contains("params")) {
        const Json& params = args["params"];
        if (!params.is_object()) {
            error = "params must be an object of overrides, e.g. {\"nearest_neighbors\": 30, \"cauchy_dispersion\": 0.5}.";
            return false;
        }
        static const std::set<std::string> keys = {"nearest_neighbors", "random_sample_locations", "cauchy_dispersion", "backtrack_percent",
                                                   "backtrack_stages", "guide_alpha", "guide_decay"};
        for (auto it = params.begin(); it != params.end(); ++it) {
            if (!keys.count(it.key())) {
                error = "params has an unknown key \"" + it.key() + "\" (allowed: nearest_neighbors, random_sample_locations, "
                        "cauchy_dispersion, backtrack_percent, backtrack_stages, guide_alpha, guide_decay; the border is `border`).";
                return false;
            }
        }
        std::optional<double> v;
        if (!numberArg(params, "nearest_neighbors", 1, 256, v, error)) return false;
        if (v) spec.overrides.nearestNeighbors = static_cast<int>(*v);
        v.reset();
        if (!numberArg(params, "random_sample_locations", 1, 1024, v, error)) return false;
        if (v) spec.overrides.randomSampleLocations = static_cast<int>(*v);
        v.reset();
        if (!numberArg(params, "cauchy_dispersion", 0.0001, 1, v, error)) return false;
        if (v) spec.overrides.cauchyDispersion = static_cast<float>(*v);
        v.reset();
        if (!numberArg(params, "backtrack_percent", 0, 0.95, v, error)) return false;
        if (v) spec.overrides.backtrackPercent = static_cast<float>(*v);
        v.reset();
        if (!numberArg(params, "backtrack_stages", 0, 10, v, error)) return false;
        if (v) spec.overrides.backtrackStages = static_cast<int>(*v);
        v.reset();
        if (!numberArg(params, "guide_alpha", 0, 1, v, error)) return false;
        if (v) spec.overrides.guideAlpha = static_cast<float>(*v);
        if (params.contains("guide_decay")) {
            if (!params["guide_decay"].is_boolean()) {
                error = "params.guide_decay must be true or false.";
                return false;
            }
            spec.overrides.guideDecay = params["guide_decay"].get<bool>();
        }
    }
    if (args.contains("border")) {
        if (spec.mode != ts::Mode::MakeTileable) {
            error = "border is only used in make_tileable mode (the band re-synthesized along each edge).";
            return false;
        }
        const Json& border = args["border"];
        if (!border.is_number() || border.get<double>() <= 0.0 || border.get<double>() > 0.5) {
            error = "border must be a share of the size in (0, 0.5], e.g. 0.125 (default) or 0.25 for a wider band.";
            return false;
        }
        spec.overrides.borderFraction = border.get<float>();
    }

    // seed
    seedGiven = args.contains("seed") && !args["seed"].is_null();
    if (seedGiven) {
        const Json& seed = args["seed"];
        if (!seed.is_number_integer() || seed.get<double>() < 0 || seed.get<double>() > MAX_SEED) {
            error = "seed must be a whole number from 0 to 2^53 (got " + seed.dump() + ").";
            return false;
        }
        spec.seed = seed.get<uint64_t>();
    } else {
        std::random_device device;
        spec.seed = std::uniform_int_distribution<uint64_t>(0, 2147483647ULL)(device);
    }

    // outputs
    if (args.contains("output") && !args["output"].is_null()) {
        if (!pathArg(project, args["output"], "output", false, spec.output, error)) return false;
        fs::path absolute = project->getProjectPath() / fs::u8path(spec.output);
        if (!project->isInsideAssetsPath(absolute)) {
            error = "output \"" + spec.output + "\" is outside the assets directory (\"" +
                    ts::projectRelative(root, project->getAssetsPath()) + "\"), so no component could use it.";
            return false;
        }
    }
    if (args.contains("uncertainty_output")) {
        if (spec.output.empty()) {
            error = "uncertainty_output needs output too (a preview-only call writes nothing).";
            return false;
        }
        if (!pathArg(project, args["uncertainty_output"], "uncertainty_output", false, spec.uncertaintyOutput, error)) return false;
    }
    return true;
}

// ---- images of the result

std::string pngBase64(const ts::Image& rgba) {
    std::vector<uint8_t> png;
    if (!ts::encodePng(rgba, png)) return std::string();
    return Base64::encode(png.data(), png.size());
}

ts::Image fitRgba8(const ts::Image& image, int maxSize) {
    ts::Image rgba = ts::toRgba8(image);
    if (rgba.width <= maxSize && rgba.height <= maxSize) return rgba;
    const double scale = static_cast<double>(maxSize) / std::max(rgba.width, rgba.height);
    return ts::resizeRgba8(rgba, std::max(1, static_cast<int>(std::lround(rgba.width * scale))),
                           std::max(1, static_cast<int>(std::lround(rgba.height * scale))));
}

ts::Image tiled2x2(const ts::Image& tile) {
    ts::Image out = ts::Image::make(tile.width * 2, tile.height * 2, 4, 8);
    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            std::copy_n(tile.pixel(x % tile.width, y % tile.height), 4, out.pixel(x, y));
        }
    }
    return out;
}

Json paramsJson(const ts::Params& p) {
    return {{"nearest_neighbors", p.nearestNeighbors}, {"random_sample_locations", p.randomSampleLocations},
            {"cauchy_dispersion", rounded(p.cauchyDispersion, 4)}, {"backtrack_percent", rounded(p.backtrackPercent, 4)},
            {"backtrack_stages", p.backtrackStages}, {"guide_alpha", rounded(p.guideAlpha, 4)}, {"guide_decay", p.guideDecay},
            {"border", rounded(p.borderFraction, 4)}};
}

// What the agent passes to synthesize_texture to reproduce a spec
Json toolArguments(const ts::SynthesisSpec& spec) {
    Json args = {{"mode", ts::toString(spec.mode)}, {"quality", ts::toString(spec.quality)}};
    Json examples = Json::array();
    for (const ts::ExampleSpec& ex : spec.examples) {
        if (ex.sampleMask.empty() && ex.guide.empty() && ex.sampleable) {
            examples.push_back(ex.path);
            continue;
        }
        Json e = {{"path", ex.path}};
        if (!ex.sampleMask.empty()) e["sample_mask"] = ex.sampleMask;
        if (!ex.guide.empty()) e["guide"] = ex.guide;
        if (!ex.sampleable) e["sampleable"] = false;
        examples.push_back(e);
    }
    args["examples"] = examples;
    Json companions = Json::array();
    for (const ts::CompanionSpec& c : spec.companions) {
        if (!c.enabled) continue;
        Json item = {{"name", c.name}};
        if (c.paths.size() == 1) item["path"] = c.paths[0];
        else item["paths"] = c.paths;
        if (!c.output.empty()) item["output"] = c.output;
        companions.push_back(item);
    }
    args["companions"] = companions.empty() ? Json("none") : companions;
    if (spec.mode == ts::Mode::Inpaint && spec.maskSource != ts::MaskSource::Painted) {
        Json mask = Json::object();
        if (spec.maskSource == ts::MaskSource::Image) {
            mask["path"] = spec.maskPath;
            if (spec.maskChannel != ts::MaskChannel::Luma) mask["channel"] = ts::toString(spec.maskChannel);
        }
        if (spec.maskInvert) mask["invert"] = true;
        args["mask"] = mask;
    }
    if (spec.mode == ts::Mode::Guided && !spec.targetGuide.empty()) args["target_guide"] = spec.targetGuide;
    if (!ts::keepsExampleSize(spec.mode) && spec.width > 0) {
        args["size"] = spec.width == spec.height ? Json(spec.width) : Json{{"width", spec.width}, {"height", spec.height}};
    }
    const ts::Params preset = ts::presetParams(spec.quality);
    const ts::Params p = ts::resolveParams(spec.quality, spec.overrides);
    Json params = Json::object();
    if (p.nearestNeighbors != preset.nearestNeighbors) params["nearest_neighbors"] = p.nearestNeighbors;
    if (p.randomSampleLocations != preset.randomSampleLocations) params["random_sample_locations"] = p.randomSampleLocations;
    if (p.cauchyDispersion != preset.cauchyDispersion) params["cauchy_dispersion"] = rounded(p.cauchyDispersion, 6);
    if (p.backtrackPercent != preset.backtrackPercent) params["backtrack_percent"] = rounded(p.backtrackPercent, 6);
    if (p.backtrackStages != preset.backtrackStages) params["backtrack_stages"] = p.backtrackStages;
    if (p.guideAlpha != preset.guideAlpha) params["guide_alpha"] = rounded(p.guideAlpha, 6);
    if (p.guideDecay != preset.guideDecay) params["guide_decay"] = p.guideDecay;
    if (!params.empty()) args["params"] = params;
    if (spec.mode == ts::Mode::MakeTileable && p.borderFraction != preset.borderFraction) args["border"] = rounded(p.borderFraction, 6);
    args["seed"] = spec.seed;
    if (!spec.output.empty()) args["output"] = spec.output;
    if (!spec.uncertaintyOutput.empty()) args["uncertainty_output"] = spec.uncertaintyOutput;
    return args;
}

// ---- synthesize_texture

struct SynthesisCall {
    ts::SynthesisSpec spec;
    bool seedGiven = false;
    bool overwrite = false;
    bool showInEditor = false;
    std::vector<std::string> warnings;
    std::shared_ptr<ts::SynthesisRun> run;
    std::string jobId;
    // filled on the main thread when the run is done
    std::vector<ToolImage> images;
    std::string text;
};

// Builds the job's result once the run finished (main thread): writes the outputs when asked
void finishSynthesis(McpContext& ctx, const std::shared_ptr<SynthesisCall>& state, const JobPtr& job) {
    ts::SynthesisRun& run = *state->run;
    const ts::Result& result = run.result();
    const ts::SynthesisSpec& spec = state->spec;
    Json data = {{"mode", ts::toString(spec.mode)}, {"quality", ts::toString(spec.quality)}, {"seed", spec.seed}};
    if (result.status == ts::Status::Cancelled || job->cancelRequested()) {
        job->markCancelled("Synthesis cancelled; nothing was written.");
        return;
    }
    if (result.status != ts::Status::Ok) {
        data["errors"] = problemsJson(result.errors);
        job->fail("The synthesis failed (" + std::string(ts::toString(result.status)) + "), nothing written:" + problemsText(result.errors), data);
        return;
    }

    // write
    Json outputs = Json::array();
    Project* project = ctx.project();
    if (!spec.output.empty()) {
        std::string error;
        std::vector<std::string> written;
        TextureSynthesisWindow* window = ctx.app().getTextureSynthesisWindow();
        bool ok = false;
        if (state->showInEditor && window && window->currentRun() == state->run) {
            ok = window->save(true, written, error);   // existence was checked when the call started
        } else {
            std::vector<ts::OutputFile> files;
            ok = ts::encodeOutputs(project->getProjectPath(), spec, run.request(), result, files, error) && ts::writeOutputsUndoable(files, error);
            for (const ts::OutputFile& f : files) written.push_back(f.path);
        }
        if (!ok) {
            data["errors"] = Json::array({Json{{"field", "output"}, {"message", error}}});
            job->fail("The synthesis finished but writing failed: " + error, data);
            return;
        }
        for (const std::string& path : written) outputs.push_back(path);
    }

    const ts::Metrics& m = result.metrics;
    const ts::Assessment assessment = ts::assess(spec, result);
    Json companions = Json::array();
    for (const ts::PlannedOutput& out : ts::plannedOutputs(spec)) {
        if (out.role == "primary" || out.role == "uncertainty" || out.role == "inpaint_mask") continue;
        companions.push_back({{"name", out.role}, {"path", out.path}});
    }
    if (spec.output.empty()) {
        for (const ts::CompanionSpec& c : spec.companions) {
            if (c.enabled) companions.push_back({{"name", c.name}, {"path", nullptr}, {"source", c.paths.front()}});
        }
    }
    Json metrics = {{"mean_uncertainty", rounded(m.meanUncertainty, 4)}, {"high_uncertainty_pct", rounded(m.highUncertaintyFraction * 100.0, 2)}};
    if (ts::isTileableMode(spec.mode)) metrics["seam_score"] = rounded(m.seamScore, 3);
    Json warnings = state->warnings;
    for (const std::string& w : result.warnings) warnings.push_back(w);
    data["outputs"] = outputs;
    data["written"] = !outputs.empty();
    data["companions"] = companions;
    data["width"] = result.width;
    data["height"] = result.height;
    data["seconds"] = rounded(result.seconds, 2);
    data["metrics"] = metrics;
    data["assessment"] = assessment.lines;
    data["warnings"] = warnings;
    data["hint"] = assessment.hint.empty() ? Json("Looks fine.") : Json(assessment.hint);
    data["params"] = paramsJson(result.params);

    // images: the result (<= 512), a 2x2 tiling for the tileable modes, the uncertainty map when high
    std::vector<ToolImage> images;
    std::string imagesText = "Images: the result";
    const ts::Image shown = fitRgba8(result.primary, 512);
    images.push_back({pngBase64(shown), "image/png"});
    if (ts::isTileableMode(spec.mode)) {
        images.push_back({pngBase64(tiled2x2(fitRgba8(result.primary, 256))), "image/png"});
        imagesText += ", a 2x2 tiling (a seam would show along the centre lines)";
    }
    if (m.highUncertaintyFraction > 0.05) {
        images.push_back({pngBase64(fitRgba8(result.uncertaintyMap, 256)), "image/png"});
        imagesText += ", the uncertainty map (brighter = worse match)";
    }
    state->images = std::move(images);

    std::string text = "Synthesized " + std::to_string(result.width) + "x" + std::to_string(result.height) + " " + ts::toString(spec.mode) + " (" +
                       ts::toString(spec.quality) + ", seed " + std::to_string(spec.seed) + ") in " + fixed(result.seconds, 1) + " s. ";
    if (outputs.empty()) {
        text += "Preview only, nothing written: call again with the same arguments, seed " + std::to_string(spec.seed) +
                " and output to write exactly this.";
    } else {
        text += "Wrote " + std::to_string(outputs.size()) + " file(s) as one undo step: ";
        for (size_t i = 0; i < outputs.size(); i++) text += (i ? ", " : "") + outputs[i].get<std::string>();
        text += ".";
    }
    for (const std::string& line : assessment.lines) text += " " + line + ".";
    if (!assessment.hint.empty()) text += " Hint: " + assessment.hint;
    for (const Json& w : warnings) text += "\nWarning: " + w.get<std::string>();
    text += "\n" + imagesText + ".";
    state->text = text;
    job->succeed(text, data);
}

void synthesizeTextureTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    auto state = std::make_shared<SynthesisCall>();
    std::string error;
    if (!specFromArgs(project, args, state->spec, state->seedGiven, state->warnings, error)) {
        call->fail(error);
        return;
    }
    state->overwrite = args.value("overwrite", false);
    state->showInEditor = args.value("show_in_editor", false);
    const bool wait = args.value("wait", true);
    const double timeout = args.value("timeout_seconds", 120.0);
    const fs::path root = project->getProjectPath();
    ts::SynthesisSpec& spec = state->spec;

    std::vector<ts::Problem> problems = ts::checkSpec(root, spec);
    if (!problems.empty()) {
        call->fail("Can't synthesize:" + problemsText(problems), Json{{"errors", problemsJson(problems)}});
        return;
    }
    if (!spec.output.empty() && !state->overwrite) {
        std::vector<std::string> existing;
        std::error_code ec;
        for (const ts::PlannedOutput& out : ts::plannedOutputs(spec)) {
            if (fs::exists(root / fs::u8path(out.path), ec)) existing.push_back(out.path);
        }
        if (!existing.empty()) {
            std::string list;
            for (const std::string& p : existing) list += (list.empty() ? "" : ", ") + p;
            call->fail("Would replace existing file(s): " + list + ". Pass overwrite: true to replace them (one undo step puts them back), "
                       "or choose another output.", Json{{"existing", existing}});
            return;
        }
    }

    const std::string title = "Synthesize " + std::string(ts::toString(spec.mode)) + " texture" +
                              (spec.output.empty() ? std::string(" (preview)") : " " + spec.output);
    JobPtr job = ctx.jobs().create("synthesize_texture", title);
    state->jobId = job->id();
    McpContext* context = &ctx;
    ts::SynthesisRun::Options options;
    options.onProgress = [job](const ts::Progress& p) {
        std::string message = "stage " + std::to_string(p.stage + 1) + "/" + std::to_string(p.stageCount);
        if (p.etaSeconds >= 0.0) message += ", about " + fixed(p.etaSeconds, 0) + " s left";
        job->setProgress(p.fraction, message);
    };
    ts::SynthesisRun::Finished onFinished = [context, state, job](ts::SynthesisRun&) {
        getEditorHost().enqueueMainThreadTask([context, state, job]() {
            // the run may finish before start() returned to the handler
            if (!state->run) return;
            finishSynthesis(*context, state, job);
        });
    };
    if (state->showInEditor) {
        TextureSynthesisWindow* window = ctx.app().getTextureSynthesisWindow();
        if (!window) {
            call->fail("The Texture Synthesis window is unavailable.");
            return;
        }
        window->open();
        window->setSpec(spec);
        state->run = window->generate(error, options, onFinished);
        if (!state->run) {
            job->fail(error);
            call->fail("Can't synthesize: " + error);
            return;
        }
    } else {
        ts::Request request;
        if (!ts::buildRequest(root, spec, request, problems)) {
            job->fail("invalid request");
            call->fail("Can't synthesize:" + problemsText(problems), Json{{"errors", problemsJson(problems)}});
            return;
        }
        state->run = ts::SynthesisRun::start(std::move(request), options, onFinished);
    }
    std::weak_ptr<ts::SynthesisRun> weakRun = state->run;
    job->setCancelHandler([weakRun]() {
        if (auto run = weakRun.lock()) run->cancel();
    });
    job->setProgress(0.0, "starting");

    replyWhenJobDone(call, job, wait, timeout, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) {
            return ToolResult::error(finished.message(), result);
        }
        ToolResult reply = ToolResult::success(state->text.empty() ? finished.message() : state->text, result);
        reply.images = state->images;
        return reply;
    });
}

}

// ---- the recipe of a PNG, for get_asset_info / view_asset

Json synthesisAssetInfo(Project* project, const fs::path& file, bool full) {
    std::string readError;
    const std::optional<nlohmann::json> recipe = ts::readRecipe(file, &readError);
    if (!recipe) {
        if (!readError.empty()) return Json{{"error", readError}};
        return Json();
    }
    const fs::path root = project->getProjectPath();
    Json info = {{"role", recipe->value("role", std::string("primary"))}, {"mode", recipe->value("mode", std::string())},
                 {"quality", recipe->value("quality", std::string())}};
    if (recipe->contains("seed")) info["seed"] = (*recipe)["seed"];
    if (recipe->contains("size")) info["size"] = (*recipe)["size"];
    Json sources = Json::array();
    int changed = 0, missing = 0;
    for (const ts::SourceState& s : ts::checkRecipeSources(*recipe, file, root)) {
        sources.push_back({{"role", s.role}, {"path", s.resolved.empty() ? s.path : ts::projectRelative(root, s.resolved)}, {"state", s.state}});
        if (s.state == "changed") changed++;
        if (s.state == "missing") missing++;
    }
    info["sources"] = sources;
    info["sources_changed"] = changed;
    info["sources_missing"] = missing;
    if (full) {
        info["recipe"] = *recipe;
        ts::SynthesisSpec spec;
        std::vector<std::string> notes;
        std::string error;
        if (ts::specFromRecipe(*recipe, file, root, spec, notes, error)) {
            Json args = toolArguments(spec);
            args["overwrite"] = true;
            info["regenerate"] = args;
            if (!notes.empty()) info["regenerate_notes"] = notes;
        } else {
            info["regenerate_error"] = error;
        }
    }
    return info;
}

std::string synthesisAssetText(const Json& info) {
    if (!info.is_object() || !info.contains("mode")) return std::string();
    std::string text = " Synthesized (" + info.value("mode", std::string()) + ", " + info.value("quality", std::string()) + ", seed " +
                       (info.contains("seed") ? info["seed"].dump() : std::string("?")) + ", role " + info.value("role", std::string()) + ")";
    const int changed = info.value("sources_changed", 0), missing = info.value("sources_missing", 0);
    if (changed || missing) {
        text += "; " + std::to_string(changed) + " source(s) changed and " + std::to_string(missing) +
                " missing since: regenerate with synthesize_texture and the `regenerate` arguments (get_asset_info)";
    } else {
        text += "; sources unchanged";
    }
    return text + ".";
}

void registerSynthesisTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "synthesize_texture";
    tool.title = "Synthesize Texture";
    tool.description =
        "Example-based texture synthesis (editor-side): a new texture like the examples (extend), a seamless tiling one (tileable), an "
        "image made seamless (make_tileable), a masked region regenerated (inpaint), or examples following a guide (guided). PBR "
        "companions (normal, roughness...) get the same arrangement. Without output nothing is written: a preview with images and "
        "metrics; the same arguments + the returned seed + output write exactly that (one undo step, PNGs with a recipe). Read "
        "hint. Guide: jasper://docs/texture-synthesis.";
    // nested objects: the handler refuses unknown keys itself (saves tools/list bytes)
    auto loose = [](Json o) {
        o.erase("additionalProperties");
        return o;
    };
    const Json pathOrObject = [&] {
        Json j;
        j["anyOf"] = Json::array({Json{{"type", "string"}},
                                  loose(object({{"path", string("")}, {"sample_mask", string("White may be copied.")},
                                                {"guide", string("guided.")}, {"sampleable", boolean("")}},
                                               {"path"}))});
        return j;
    }();
    Json companionItem;
    companionItem["anyOf"] = Json::array({Json{{"type", "string"}},
                                          loose(object({{"path", string("")}, {"paths", array(Json{{"type", "string"}}, "One per example.")},
                                                        {"name", string("")}, {"output", string("")}}))});
    Json companions;
    companions["description"] = "auto: the maps next to each example (rock_albedo.png -> rock_normal.png...).";
    companions["default"] = "auto";
    companions["anyOf"] = Json::array({stringEnum("", {"auto", "none"}), array(companionItem, "")});
    Json size;
    size["description"] = "Default 1024; guided: the target guide's. Not for make_tileable/inpaint.";
    size["anyOf"] = Json::array({integer("", 1, 8192), loose(object({{"width", integer("")}, {"height", integer("")}}, {"width", "height"}))});
    tool.inputSchema = object({
        {"mode", withDefault(stringEnum("", {"extend", "tileable", "make_tileable", "inpaint", "guided"}), "tileable")},
        {"examples", array(pathOrObject, "Image paths (albedo).")},
        {"companions", companions},
        {"mask", loose(object({{"path", string("White = regenerate. Omitted: the example's transparent pixels.")},
                               {"channel", withDefault(stringEnum("", {"luma", "r", "g", "b", "a"}), "luma")},
                               {"invert", boolean("")}}))},
        {"target_guide", string("guided.")},
        {"size", size},
        {"quality", withDefault(stringEnum("draft ~2x faster, high ~2x slower.", {"draft", "balanced", "high"}), "balanced")},
        {"params", loose(object({{"nearest_neighbors", integer("", 1, 256)}, {"random_sample_locations", integer("", 1, 1024)},
                           {"cauchy_dispersion", number("Low: hard patch edges.", 0.0001, 1)}, {"backtrack_percent", number("", 0, 0.95)},
                           {"backtrack_stages", integer("", 0, 10)}, {"guide_alpha", number("", 0, 1)}, {"guide_decay", boolean("")}}))},
        {"border", withDefault(number("make_tileable band per edge.", 0.0001, 0.5), 0.125)},
        {"seed", integer("0..2^53. Default random (returned).")},
        {"output", string("A .png in the assets dir; omitted = preview only.")},
        {"overwrite", withDefault(boolean(""), false)},
        {"uncertainty_output", string("")},
        {"show_in_editor", withDefault(boolean("Run it in the Texture Synthesis window."), false)},
        {"wait", withDefault(boolean(""), true)},
        {"timeout_seconds", withDefault(number("Then a job_id (get_job).", 1, 3600), 120)}
    }, {"examples"});
    tool.handler = synthesizeTextureTool;
    registry.addTool(std::move(tool));
}

}
