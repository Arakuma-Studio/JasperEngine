// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Sprite normal maps: generate_sprite_normals (spritenormals, the same session as Window > Sprite
// Normal Map and the generate-sprite-normals CLI), the recipe summary get_asset_info / view_asset
// show, and the file-sync hook naming the normal maps whose source changed
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/SpriteNormalsSheetWrite.h"
#include "mcp/tools/W4Shared.h"

#include "App.h"
#include "EditorHost.h"
#include "Project.h"
#include "Scene.h"
#include "command/CommandHistory.h"
#include "component/MeshComponent.h"
#include "component/PolygonComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "spritenormals/SpriteNormalsApply.h"
#include "spritenormals/SpriteNormalsCheck.h"
#include "spritenormals/SpriteNormalsPaint.h"
#include "spritenormals/SpriteNormalsSession.h"
#include "spritenormals/SpriteNormalsWatcher.h"
#include "texsynth/SynthesisSession.h"
#include "util/Base64.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;
namespace sn = spritenormals;

namespace {

const char* const TOOL = "generate_sprite_normals";

std::string fixed(double v, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
    return buffer;
}

double rounded(double v, int digits) {
    const double scale = std::pow(10.0, digits);
    return std::round(v * scale) / scale;
}

// 4.8, 4, 9.63 (two decimals at most, no trailing zeros)
std::string number(double v) {
    std::string text = fixed(v, 2);
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return text;
}

std::string lowerExtension(const std::string& path) {
    std::string ext = jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(path).extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string fileName(const std::string& path) {
    const std::string name = jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(path).filename());
    return name.empty() ? path : name;
}

std::string relativeOf(Project* project, const fs::path& absolute) {
    return sn::projectRelative(project->getProjectPath(), absolute);
}

// ---- problems

// The settings' field names (validateSettings, settingsFromJson) as the tool's argument names
const std::vector<std::string>& paramKeys() {
    static const std::vector<std::string> keys = {"strength", "detail", "smoothing", "volume", "radius", "softness", "alpha_threshold",
                                                  "profile", "edges", "resolution_scale", "soft_edge", "ignore_outline", "regions", "quantize", "gaps", "version"};
    return keys;
}

std::string argumentField(const std::string& field) {
    for (const std::string& key : paramKeys()) {
        if (field == key || field.rfind(key + "[", 0) == 0 || field.rfind(key + ".", 0) == 0) return "params." + field;
    }
    if (field == "entity") return "source";
    return field;
}

sn::Problem problem(const std::string& kind, const std::string& field, const std::string& message, const std::string& fix) {
    return {kind, field, message, fix};
}

// save_path (resolved by McpRefs::resolveProjectPath to `target`) still inside the project once
// symbolic links are resolved; false + error when a link on its way leads out of it
bool savePathStaysInside(Project* project, const std::string& given, const fs::path& target, std::string& error) {
    // inside the project's trusted roots (its directory, its assets directory) once resolved
    if (McpRefs::writeEscape(project, target).empty()) return true;
    error = "\"" + given + "\" resolves outside the project: a symbolic link on its way leads to " +
            FileUtils::pathToGenericUtf8(sn::resolvedPath(target)) + ".";
    return false;
}

// every error says how to fix it: a problem without its own fix points at the argument table
std::string fixOf(const sn::Problem& p) {
    return p.fix.empty() ? "check the argument against jasper://docs/sprite-normals (Arguments)" : p.fix;
}

Json problemJson(const sn::Problem& p) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", fixOf(p)}};
    const std::string field = argumentField(p.field);
    if (!field.empty()) j["field"] = field;
    return j;
}

Json problemsJson(const std::vector<sn::Problem>& problems) {
    Json list = Json::array();
    for (const sn::Problem& p : problems) list.push_back(problemJson(p));
    return list;
}

// "\n- kind: message Fix: fix" per problem
std::string problemsText(const std::vector<sn::Problem>& problems) {
    std::string text;
    for (const sn::Problem& p : problems) text += "\n- " + p.kind + ": " + p.message + " Fix: " + fixOf(p);
    return text;
}

void failCall(const CallPtr& call, const std::vector<sn::Problem>& problems, Json data = Json::object()) {
    if (!data.is_object()) data = Json::object();
    data["errors"] = problemsJson(problems);
    call->fail("Can't generate the normal map:" + problemsText(problems), data);
}

void failCall(const CallPtr& call, const sn::Problem& p) {
    failCall(call, std::vector<sn::Problem>{p});
}

void failJob(const JobPtr& job, const std::string& headline, const std::vector<sn::Problem>& problems, Json data = Json::object()) {
    if (!data.is_object()) data = Json::object();
    data["errors"] = problemsJson(problems);
    job->fail(headline + problemsText(problems), data);
}

// The session's output_exists problems in this tool's words, with the planned path of their
// role (field "output.<role>"), and those paths for `existing`
void rewordExisting(std::vector<sn::Problem>& problems, const std::vector<sn::PlannedOutput>& planned, std::vector<std::string>& existing) {
    for (sn::Problem& p : problems) {
        if (p.kind != "output_exists") continue;
        for (const sn::PlannedOutput& out : planned) {
            if (p.field != "output." + out.role) continue;
            existing.push_back(out.path);
            p.message = "would replace " + out.path;
        }
        p.fix = "pass overwrite: true (one undo step puts it back), or another output";
    }
}

// ---- arguments

bool isNonNegativeInt(const Json& v) {
    return v.is_number_integer() && v.get<long long>() >= 0 && v.get<long long>() <= 1000000;
}

// [x, y] of non-negative integers
bool pairArg(const Json& v, int& x, int& y) {
    if (!v.is_array() || v.size() != 2 || !isNonNegativeInt(v[0]) || !isNonNegativeInt(v[1])) return false;
    x = v[0].get<int>();
    y = v[1].get<int>();
    return true;
}

// preset, params, directx, height -> resolved settings (Settings s = presetSettings(p);
// settingsFromJson(overrides, s); s.preset = p; validateSettings(s), as every surface does);
// params.light_steps (the contact sheet's Light Steps, not a setting of the map) -> lightSteps
bool settingsArg(const Json& args, sn::Settings& out, int& lightSteps, std::vector<sn::Problem>& problems) {
    sn::Preset preset = sn::Preset::Default;
    if (args.contains("preset") && !args["preset"].is_null()) {
        const Json& p = args["preset"];
        if (!p.is_string() || !sn::parsePreset(p.get<std::string>(), preset)) {
            problems.push_back(problem("invalid_settings", "preset", "preset must be one of " + sn::presetNames() + " (got " + p.dump() + ")",
                                       "valid presets: " + sn::presetNames()));
            return false;
        }
    }
    sn::Settings s = sn::presetSettings(preset);
    std::string keyList;
    for (const std::string& key : paramKeys()) keyList += (keyList.empty() ? "" : ", ") + key;
    keyList += ", light_steps";
    if (args.contains("params") && !args["params"].is_null()) {
        Json params = args["params"];
        if (!params.is_object()) {
            problems.push_back(problem("invalid_settings", "params", "params must be an object of overrides (got " + params.dump() + ")",
                                       "e.g. {\"detail\": 2, \"edges\": \"extend\"}; valid keys: " + keyList));
            return false;
        }
        if (params.contains("light_steps")) {
            const Json& steps = params["light_steps"];
            if (!steps.is_number_integer() || steps.get<long long>() < 0 || steps.get<long long>() > sn::kMaxLightSteps) {
                problems.push_back(problem("invalid_settings", "params.light_steps",
                                           "params.light_steps must be a whole number between 0 and " + std::to_string(sn::kMaxLightSteps) + " (got " + steps.dump() + ")",
                                           "0 (smooth light) or 1..16 bands, as the scene's Light Steps"));
                return false;
            }
            lightSteps = steps.get<int>();
            params.erase("light_steps");
        }
        for (auto it = params.begin(); it != params.end(); ++it) {
            const std::vector<std::string>& keys = paramKeys();
            if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) {
                problems.push_back(problem("invalid_settings", "params." + it.key(), "params has an unknown key \"" + it.key() + "\"",
                                           "valid keys: " + keyList + " (directx and height are arguments of their own)"));
                return false;
            }
        }
        for (const char* key : {"strength", "detail", "smoothing", "volume", "softness", "alpha_threshold"}) {
            if (params.contains(key) && !params[key].is_number()) {
                problems.push_back(problem("invalid_settings", std::string("params.") + key,
                                           std::string("params.") + key + " must be a number (got " + params[key].dump() + ")",
                                           "give a number; the ranges are in jasper://docs/sprite-normals"));
                return false;
            }
        }
        if (params.contains("regions")) {
            const Json& regions = params["regions"];
            if (!regions.is_array()) {
                problems.push_back(problem("invalid_settings", "params.regions", "params.regions must be an array (got " + regions.dump() + ")",
                                           "[{\"colors\": [\"#ffffff\"], \"detail\": 0}]"));
                return false;
            }
            static const std::set<std::string> regionKeys = {"colors", "tolerance", "detail", "lift", "flat"};
            for (size_t i = 0; i < regions.size(); i++) {
                if (!regions[i].is_object()) continue;   // settingsFromJson names it
                for (auto it = regions[i].begin(); it != regions[i].end(); ++it) {
                    if (!regionKeys.count(it.key())) {
                        problems.push_back(problem("invalid_settings", "params.regions[" + std::to_string(i) + "]",
                                                   "params.regions[" + std::to_string(i) + "] has an unknown key \"" + it.key() + "\"",
                                                   "valid keys: colors, tolerance, detail, lift, flat"));
                        return false;
                    }
                }
            }
        }
        if (params.contains("quantize") && params["quantize"].is_object()) {
            static const std::set<std::string> quantizeKeys = {"directions", "tilts", "flat_deg", "rotation_deg"};
            for (auto it = params["quantize"].begin(); it != params["quantize"].end(); ++it) {
                if (!quantizeKeys.count(it.key())) {
                    problems.push_back(problem("invalid_settings", "params.quantize", "params.quantize has an unknown key \"" + it.key() + "\"",
                                               "valid keys: directions, tilts, flat_deg, rotation_deg"));
                    return false;
                }
            }
        }
        if (params.contains("gaps") && params["gaps"].is_object()) {
            for (auto it = params["gaps"].begin(); it != params["gaps"].end(); ++it) {
                if (it.key() != "share") {
                    problems.push_back(problem("invalid_settings", "params.gaps", "params.gaps has an unknown key \"" + it.key() + "\"",
                                               "give {\"share\": 0..0.9}: the darkest share of the pixels that are gaps"));
                    return false;
                }
            }
        }
        std::string error;
        if (!sn::settingsFromJson(params, s, error)) {
            problems.push_back(problem("invalid_settings", "params", "params." + error, "valid keys: " + keyList));
            return false;
        }
    }
    s.preset = preset;
    for (const char* key : {"directx", "height"}) {
        if (args.contains(key) && !args[key].is_null() && !args[key].is_boolean()) {
            problems.push_back(problem("invalid_settings", key, std::string(key) + " must be true or false (got " + args[key].dump() + ")",
                                       std::string("pass ") + key + ": true or " + key + ": false, or leave it out (false)"));
            return false;
        }
    }
    s.directx = args.value("directx", false);
    s.height = args.value("height", false);
    for (sn::Problem p : sn::validateSettings(s)) {
        p.field = argumentField(p.field);
        if (p.field.rfind("params.", 0) == 0 && p.message.rfind("params.", 0) != 0) p.message = "params." + p.message;
        problems.push_back(p);
    }
    out = s;
    return problems.empty();
}

// frames: "auto", "none", a grid {tile_size, columns, rows, count, offset, padding} (as
// set_sprite_frames.slice) or {cells: [[x, y, w, h], ...]}
bool framesArg(const Json& args, sn::FramesSpec& out, std::vector<sn::Problem>& problems) {
    out = sn::FramesSpec();
    if (!args.contains("frames") || args["frames"].is_null()) return true;
    const Json& f = args["frames"];
    auto refuse = [&](const std::string& field, const std::string& message, const std::string& fix) {
        problems.push_back(problem("frames_mismatch", field, message, fix));
        return false;
    };
    const std::string fix = "tile_size [32, 32] (or 32), or columns 4 and rows 1; or cells [[x, y, w, h], ...]";
    if (f.is_string()) {
        if (f == "auto") return true;
        if (f == "none") {
            out.mode = sn::FramesSpec::Mode::None;
            return true;
        }
        return refuse("frames", "frames must be \"auto\", \"none\" or an object (got " + f.dump() + ")", fix);
    }
    if (!f.is_object()) return refuse("frames", "frames must be \"auto\", \"none\" or an object (got " + f.dump() + ")", fix);
    static const std::set<std::string> gridKeys = {"tile_size", "columns", "rows", "count", "offset", "padding"};
    for (auto it = f.begin(); it != f.end(); ++it) {
        if (it.key() == "cells" || gridKeys.count(it.key())) continue;
        if (it.key() == "names" || it.key() == "prefix" || it.key() == "sheet_size") {
            return refuse("frames." + it.key(), "frames." + it.key() + " is a set_sprite_frames key that does not apply here (a normal map has the image's size and no frame names)",
                          "leave it out; valid keys: tile_size, columns, rows, count, offset, padding, or cells");
        }
        return refuse("frames." + it.key(), "frames has an unknown key \"" + it.key() + "\"",
                      "valid keys: tile_size, columns, rows, count, offset, padding, or cells");
    }
    if (f.contains("cells")) {
        for (const std::string& key : gridKeys) {
            if (f.contains(key)) return refuse("frames.cells", "frames.cells can't be combined with the grid key " + key, "give cells alone, or a grid");
        }
        const Json& cells = f["cells"];
        if (!cells.is_array() || cells.empty()) return refuse("frames.cells", "frames.cells must be a non-empty array of [x, y, w, h] (got " + cells.dump() + ")", fix);
        for (size_t i = 0; i < cells.size(); i++) {
            const Json& c = cells[i];
            bool ok = c.is_array() && c.size() == 4;
            for (size_t k = 0; ok && k < 4; k++) ok = isNonNegativeInt(c[k]);
            if (!ok) return refuse("frames.cells[" + std::to_string(i) + "]", "frames.cells[" + std::to_string(i) + "] must be [x, y, w, h] in pixels (got " + c.dump() + ")", fix);
            out.cells.push_back({c[0].get<int>(), c[1].get<int>(), c[2].get<int>(), c[3].get<int>()});
        }
        out.mode = sn::FramesSpec::Mode::Cells;
        out.from = sn::FramesFrom::Given;
        return true;
    }
    sn::Grid& g = out.grid;
    if (f.contains("tile_size")) {
        const Json& t = f["tile_size"];
        if (isNonNegativeInt(t) && t.get<int>() > 0) {
            g.tileWidth = g.tileHeight = t.get<int>();
        } else if (!pairArg(t, g.tileWidth, g.tileHeight) || g.tileWidth < 1 || g.tileHeight < 1) {
            return refuse("frames.tile_size", "frames.tile_size must be a pixel size N or [w, h] (got " + t.dump() + ")", fix);
        }
    }
    for (const auto& [key, target] : {std::pair<const char*, int*>{"columns", &g.columns}, {"rows", &g.rows}, {"count", &g.count}}) {
        if (!f.contains(key)) continue;
        if (!isNonNegativeInt(f[key])) return refuse(std::string("frames.") + key, std::string("frames.") + key + " must be a whole number >= 0 (got " + f[key].dump() + ")", fix);
        *target = f[key].get<int>();
    }
    if (f.contains("offset") && !pairArg(f["offset"], g.offsetX, g.offsetY)) {
        return refuse("frames.offset", "frames.offset must be [x, y] pixels (got " + f["offset"].dump() + ")", fix);
    }
    if (f.contains("padding") && !pairArg(f["padding"], g.paddingX, g.paddingY)) {
        return refuse("frames.padding", "frames.padding must be [x, y] pixels (got " + f["padding"].dump() + ")", fix);
    }
    if (!(g.tileWidth > 0 && g.tileHeight > 0) && !(g.columns > 0 && g.rows > 0)) {
        return refuse("frames", "a frames grid needs tile_size, or columns and rows", fix);
    }
    out.mode = sn::FramesSpec::Mode::Grid;
    return true;
}

// output: omitted / false = preview; true = the default names; a string = the normal map; an
// object {normal?, atlas?, height?, frames?}
bool outputArg(const Json& args, sn::OutputSpec& out, bool& write, bool& custom, std::vector<sn::Problem>& problems) {
    out = sn::OutputSpec();
    write = false;
    custom = false;
    if (!args.contains("output") || args["output"].is_null()) return true;
    const Json& o = args["output"];
    auto refuse = [&](const std::string& field, const std::string& message) {
        problems.push_back(problem("output_invalid", field, message, "an assets path ending in .png, or output: true"));
        return false;
    };
    if (o.is_boolean()) {
        write = o.get<bool>();
        return true;
    }
    if (o.is_string()) {
        if (o.get<std::string>().empty()) return refuse("output", "output is an empty path");
        out.normal = o.get<std::string>();
        write = custom = true;
        return true;
    }
    if (!o.is_object()) return refuse("output", "output must be true, a .png path or {normal?, atlas?, height?, frames?} (got " + o.dump() + ")");
    for (auto it = o.begin(); it != o.end(); ++it) {
        const std::string& key = it.key();
        if (key == "frames") {
            if (it.value().is_boolean()) {
                out.framesJson = it.value().get<bool>();
            } else if (it.value().is_string() && !it.value().get<std::string>().empty()) {
                out.framesJson = true;
                out.framesJsonPath = it.value().get<std::string>();
            } else {
                return refuse("output.frames", "output.frames must be true or a .json path (got " + it.value().dump() + ")");
            }
            continue;
        }
        if (key != "normal" && key != "atlas" && key != "height") {
            return refuse("output." + key, "output has an unknown key \"" + key + "\" (valid: normal, atlas, height, frames)");
        }
        if (!it.value().is_string() || it.value().get<std::string>().empty()) {
            return refuse("output." + key, "output." + key + " must be a .png path (got " + it.value().dump() + ")");
        }
        (key == "normal" ? out.normal : key == "atlas" ? out.atlas : out.height) = it.value().get<std::string>();
        custom = true;
    }
    write = true;
    return true;
}

// ---- the source

enum class MeshKind { None, Sprite, Polygon, Tilemap };

MeshKind meshKind(SceneProject* scene, Entity entity) {
    Scene* s = scene && scene->scene ? scene->scene : nullptr;
    if (!s || entity == NULL_ENTITY || !s->isEntityCreated(entity) || !s->findComponent<MeshComponent>(entity)) return MeshKind::None;
    if (s->findComponent<TilemapComponent>(entity)) return MeshKind::Tilemap;
    if (s->findComponent<SpriteComponent>(entity)) return MeshKind::Sprite;
    if (s->findComponent<PolygonComponent>(entity)) return MeshKind::Polygon;
    return MeshKind::None;
}

const char* meshKindName(MeshKind kind) {
    switch (kind) {
        case MeshKind::Sprite: return "Sprite";
        case MeshKind::Polygon: return "Polygon";
        case MeshKind::Tilemap: return "Tilemap";
        default: return "GameObject";
    }
}

// What a GameObject is, for messages ("a Box (mesh)", "a GameObject without a mesh")
std::string whatIs(SceneProject* scene, Entity entity) {
    Scene* s = scene->scene;
    if (s->findComponent<MeshComponent>(entity)) return "a mesh that is not a Sprite, Polygon or Tilemap";
    return "a GameObject without a mesh";
}

// The colour texture of a submesh as a project file: false with why ("its texture is not a file")
bool submeshTexture(Project* project, SceneProject* scene, Entity entity, unsigned submesh, std::string& rel, std::string& why) {
    MeshComponent* mesh = scene->scene->findComponent<MeshComponent>(entity);
    if (!mesh || submesh >= std::max(mesh->numSubmeshes, 1u)) {
        why = mesh ? "it has no submesh " + std::to_string(submesh) : std::string("it has no mesh");
        return false;
    }
    const Texture& base = mesh->submeshes[submesh].material.baseColorTexture;
    if (base.isFramebuffer()) {
        why = "its texture is not a file (a render texture)";
        return false;
    }
    const std::string stored = base.getPath();
    if (stored.empty()) {
        why = "it has no texture";
        return false;
    }
    rel = McpValues::assetPathToJson(project, stored);
    std::error_code ec;
    if (!fs::is_regular_file(project->getProjectPath() / fs::u8path(rel), ec)) {
        why = "its texture is not a file (got " + stored + ")";
        return false;
    }
    return true;
}

// ---- the call

struct NormalsTarget {
    sn::Spec spec;
    std::string colorPath;                     // project-relative: the texture the map is made from
    std::shared_ptr<sn::SpriteNormalsRun> run;
};

struct NormalsCall {
    McpContext* ctx = nullptr;
    JobPtr job;
    // the project the call started in: its result is written there or nowhere
    fs::path root;
    uint64_t generation = 0;
    bool write = false;
    bool overwrite = false;
    std::string sourceLabel;                   // the argument as the agent gave it (messages)
    // a GameObject source (0 / NULL_ENTITY for an image file)
    uint32_t sourceScene = 0;
    Entity sourceEntity = NULL_ENTITY;
    MeshKind sourceKind = MeshKind::None;
    // apply
    bool apply = false;
    uint32_t applyScene = 0;
    Entity applyEntity = NULL_ENTITY;
    std::string savePath;                      // project-relative, "" for none
    int lightSteps = -1;                       // the contact sheet's Light Steps (params.light_steps); -1: the scene's
    std::vector<NormalsTarget> targets;
    bool finished = false;
    // filled on the main thread when the runs are done
    std::vector<ToolImage> images;
    std::string text;
};
using NormalsCallPtr = std::shared_ptr<NormalsCall>;

std::string framesFromText(const sn::Prepared& p, size_t count) {
    const std::string n = std::to_string(count) + (count == 1 ? " frame" : " frames");
    switch (p.framesFrom) {
        case sn::FramesFrom::Sprite: return n + " from Sprite '" + (p.spriteEntityPath.empty() ? std::string("?") : p.spriteEntityPath) + "'";
        case sn::FramesFrom::Gif: return n + " from the GIF";
        case sn::FramesFrom::Apng: return n + " from the APNG";
        case sn::FramesFrom::Given: return n + " given";
        default: return "the whole image";
    }
}

int distinctCount(const std::vector<sn::Cell>& cells, bool columns) {
    std::set<int> values;
    for (const sn::Cell& c : cells) values.insert(columns ? c.x : c.y);
    return static_cast<int>(values.size());
}

Json settingsJson(const sn::Settings& s) {
    return sn::settingsToJson(s);
}

Json metricsJson(const sn::Metrics& m) {
    return {{"coverage_pct", rounded(m.coveragePct, 1)}, {"flat_pct", rounded(m.flatPct, 1)}, {"steep_pct", rounded(m.steepPct, 1)},
            {"mean_tilt_deg", rounded(m.meanTiltDeg, 1)}, {"noise_pct", rounded(m.noisePct, 1)}, {"lean_deg", rounded(m.leanDeg, 1)},
            {"soft_edge_pct", rounded(m.softEdgePct, 1)}, {"frames_touching_edges", m.framesTouchingEdges},
            {"has_transparency", m.hasTransparency}, {"opaque_pixels", m.opaquePixels}, {"painted_pct", rounded(m.paintedPct, 1)},
            {"roughness_deg", rounded(m.roughnessDeg, 2)}, {"crease_pct", m.creaseMeasured ? Json(rounded(m.creasePct, 2)) : Json(nullptr)}};
}

// The paint layer of a run (4.1): where it came from, what each op selected and changed
Json paintJson(const sn::Prepared& p, const sn::Metrics& m) {
    Json ops = Json::array();
    for (size_t i = 0; i < p.paint.ops.size(); i++) {
        const sn::PaintStats::Op counts = i < p.paint.stats.ops.size() ? p.paint.stats.ops[i] : sn::PaintStats::Op();
        ops.push_back({{"op", sn::toString(p.paint.ops[i].kind)}, {"selected", counts.selected}, {"changed", counts.changed}});
    }
    return {{"mode", sn::toString(p.spec.paintMode)},
            {"kept_from", p.paintKeptFrom.empty() ? Json(nullptr) : Json(p.paintKeptFrom)},
            {"ops", ops},
            {"painted_pixels", p.paint.stats.painted},
            {"hidden_pixels", p.paint.stats.hidden},
            {"painted_pct", rounded(m.paintedPct, 1)}};
}

Json warningsJson(const std::vector<sn::Problem>& warnings) {
    Json list = Json::array();
    for (const sn::Problem& w : warnings) list.push_back({{"kind", w.kind}, {"message", w.message}, {"fix", w.fix}});
    return list;
}

// What one finished target says: its data (3.7), its summary paragraph, its sheet and warnings
struct Report {
    Json data;
    std::string summary;
    std::vector<sn::Problem> warnings;
    std::vector<uint8_t> sheet;
    std::string defaultNormal;
    bool painted = false;          // the sheet outlines painted pixels
    int lightSteps = 0;            // the sheet's Light Steps
};

// the contact sheet's Light Steps: params.light_steps, else the Light Steps of the scene the map is
// for (applied to, its source GameObject's, else the selected one), as the window's "Scene" choice
int sheetLightSteps(Project* project, const NormalsCall& state) {
    if (state.lightSteps >= 0) return state.lightSteps;
    const uint32_t sceneId = state.apply ? state.applyScene : state.sourceEntity != NULL_ENTITY ? state.sourceScene : project->getSelectedSceneId();
    const SceneProject* sp = project->getScene(sceneId);
    return sp && sp->scene ? std::clamp(sp->scene->getLight2DSteps(), 0, sn::kMaxLightSteps) : 0;
}

Report report(Project* project, const NormalsCall& state, const NormalsTarget& target) {
    Report out;
    out.lightSteps = sheetLightSteps(project, state);
    const sn::Prepared& p = target.run->prepared();
    const sn::Result& r = target.run->result();
    const sn::Settings& s = p.request.settings;
    const std::vector<sn::Cell>& cells = p.request.cells;
    const sn::Source& source = p.source;
    const bool animated = source.kind == sn::SourceKind::Gif || source.kind == sn::SourceKind::Apng;
    Json& data = out.data;

    Json src = {{"path", p.sourcePath}, {"kind", p.spec.hasSprite() ? "sprite" : sn::toString(source.kind)},
                {"width", source.fileWidth}, {"height", source.fileHeight}};
    if (p.spec.hasSprite()) {
        if (SceneProject* scene = project->getScene(p.spec.sceneId)) src["entity"] = McpRefs::entityJson(scene, p.spec.entity);
        if (p.spec.submesh != 0) src["submesh"] = p.spec.submesh;
    }
    data["source"] = src;

    const size_t count = cells.empty() ? 1 : cells.size();
    Json frames = {{"from", sn::toString(p.framesFrom)}, {"count", count}};
    if (animated) {
        frames["columns"] = source.columns;
        frames["rows"] = source.rows;
        frames["frame_width"] = source.frameWidth;
        frames["frame_height"] = source.frameHeight;
        Json ms = Json::array();
        for (const sn::Delay& d : source.delays) ms.push_back(d.playbackMs());
        frames["frame_ms"] = ms;
        frames["loop"] = source.loopCount;
    } else if (!cells.empty()) {
        frames["columns"] = distinctCount(cells, true);
        frames["rows"] = distinctCount(cells, false);
        frames["frame_width"] = cells[0].width;
        frames["frame_height"] = cells[0].height;
    } else {
        frames["columns"] = 1;
        frames["rows"] = 1;
        frames["frame_width"] = source.color.width;
        frames["frame_height"] = source.color.height;
    }
    sn::SheetOptions sheetOptions;
    sheetOptions.lightSteps = out.lightSteps;
    if (count > static_cast<size_t>(sheetOptions.maxFrames)) {
        std::string shown;
        for (int i : sn::sheetFrames(static_cast<int>(count), sheetOptions.maxFrames)) shown += (shown.empty() ? "" : ", ") + std::to_string(i);
        frames["sheet_note"] = "the sheet shows " + std::to_string(sheetOptions.maxFrames) + " of the " + std::to_string(count) + " frames: " + shown;
    }
    data["frames"] = frames;
    data["settings"] = settingsJson(s);
    std::string radius;
    if (r.radiusPerIsland) {
        data["radius_resolved"] = "per-island";
        data["radius_range"] = Json::array({rounded(r.radiusMin, 2), rounded(r.radiusMax, 2)});
        radius = r.radiusMin == r.radiusMax ? "radius " + number(r.radiusMin) + " px" : "radius " + number(r.radiusMin) + "-" + number(r.radiusMax) + " px per island";
    } else {
        data["radius_resolved"] = rounded(r.radiusMin, 2);
        radius = "radius " + number(r.radiusMin) + " px";
    }
    data["scale_range"] = Json::array({rounded(r.scaleMin, 3), rounded(r.scaleMax, 3)});
    Json profiles = Json::array();
    for (sn::Profile pr : {sn::Profile::Rounded, sn::Profile::Dome, sn::Profile::Bevel, sn::Profile::Inflate}) {
        if (r.profilesUsed & (1u << static_cast<int>(pr))) profiles.push_back(sn::toString(pr));
    }
    data["profile_resolved"] = profiles;
    if (r.outlineColor) data["outline"] = sn::colorText(*r.outlineColor);

    // the metrics of the composite (the paint included)
    const sn::Metrics metrics = sn::measure(r.normal, cells, s.directx, s.alphaThreshold, &p.paint.overlay, r.shape.empty() ? nullptr : &r.shape);
    const sn::Assessment assessment = sn::assess(s, metrics, static_cast<int>(cells.size()), &r);
    data["metrics"] = metricsJson(metrics);
    data["paint"] = paintJson(p, metrics);
    data["assessment"] = assessment.lines;
    data["hint"] = assessment.hint.empty() ? std::string("Looks fine.") : assessment.hint;

    // image warnings (assess + the generator's), then what the scene says (sceneWarnings)
    out.warnings = assessment.warnings;
    out.warnings.insert(out.warnings.end(), r.warnings.begin(), r.warnings.end());
    SceneProject* scene = nullptr;
    Entity entity = NULL_ENTITY;
    if (state.apply) {
        scene = project->getScene(state.applyScene);
        entity = state.applyEntity;
    } else if (state.sourceEntity != NULL_ENTITY) {
        scene = project->getScene(state.sourceScene);
        entity = state.sourceEntity;
    }
    // the submesh the map is for (a tilemap's tileset; a file applied to a GameObject: its first)
    const unsigned submesh = state.sourceEntity != NULL_ENTITY && entity == state.sourceEntity ? p.spec.submesh : 0;
    const std::vector<sn::Problem> sceneWarnings = sn::sceneWarnings(scene ? scene->scene : nullptr, entity, s, r.normal.width, r.normal.height,
                                                                     source.delays, state.apply, submesh);
    out.warnings.insert(out.warnings.end(), sceneWarnings.begin(), sceneWarnings.end());
    data["warnings"] = warningsJson(out.warnings);

    Json defaults = {{"normal", nullptr}, {"atlas", nullptr}, {"height", nullptr}, {"frames_json", nullptr}};
    for (const sn::PlannedOutput& o : sn::defaultOutputs(p.sourcePath, source.kind, s.height, false)) defaults[o.role] = o.path;
    data["default_output"] = defaults;
    out.defaultNormal = defaults["normal"].is_string() ? defaults["normal"].get<std::string>() : std::string();
    data["seconds"] = rounded(r.seconds, 2);

    // the lit contact sheet (<= 1024 wide, <= 8 frames)
    // painted pixels outlined in magenta in the normal column (the lit ones stay clean)
    out.painted = metrics.paintedPixels > 0;
    const sn::Image sheet = sn::contactSheet(source.color, r.normal, cells, s.directx, s.alphaThreshold, sheetOptions, out.painted ? &p.paint.overlay : nullptr);
    std::string error;
    if (!sheet.empty()) sn::encodePng(sheet, out.sheet, error);

    out.summary = "Normal map of " + p.sourcePath + " (" + std::to_string(source.fileWidth) + "x" + std::to_string(source.fileHeight) + ", " +
                  framesFromText(p, count) + ", preset " + sn::toString(s.preset) +
                  (s.quantize.on() && s.preset != sn::Preset::Toon ? ", toon steps " + std::to_string(s.quantize.directions) : std::string()) + ", " +
                  radius + ") in " + fixed(r.seconds, 2) + " s.";
    for (const std::string& line : assessment.lines) out.summary += " " + line + ".";
    if (p.paint.active() || p.spec.paintMode != sn::PaintMode::Keep) {
        out.summary += " Paint (" + std::string(sn::toString(p.spec.paintMode)) + (p.paintKeptFrom.empty() ? "" : ", kept from " + p.paintKeptFrom) + "): " +
                       std::to_string(p.paint.ops.size()) + (p.paint.ops.size() == 1 ? " op, " : " ops, ") + std::to_string(p.paint.stats.painted) +
                       " px painted (" + std::to_string(p.paint.stats.hidden) + " hidden).";
    }
    out.summary += " Hint: " + data["hint"].get<std::string>();
    return out;
}

// set_properties / set_sprite_frames / set_sprite_animation calls that wire the written files
// onto the source GameObject (for an agent that wants to do it later)
Json applyArgs(Project* project, const NormalsCall& state, const std::vector<std::vector<sn::OutputFile>>& written) {
    SceneProject* scene = project->getScene(state.sourceScene);
    if (!scene || !scene->scene) return Json();
    MeshComponent* mesh = scene->scene->findComponent<MeshComponent>(state.sourceEntity);
    if (!mesh) return Json();
    Json calls = Json::array();
    Json values = Json::object();
    auto pathOf = [](const std::vector<sn::OutputFile>& files, const char* role) {
        for (const sn::OutputFile& f : files) {
            if (f.role == role) return f.path;
        }
        return std::string();
    };
    auto textureValue = [&](const std::string& path, unsigned submesh) {
        // the colour texture's sampling, as apply copies it
        const Json like = McpValues::textureToJson(project, scene, mesh->submeshes[submesh].material.baseColorTexture);
        Json value = {{"path", path}};
        for (const char* key : {"min_filter", "mag_filter", "wrap_u", "wrap_v"}) {
            if (like.is_object() && like.contains(key)) value[key] = like[key];
        }
        return value;
    };
    for (size_t t = 0; t < state.targets.size() && t < written.size(); t++) {
        const sn::Prepared& p = state.targets[t].run->prepared();
        const std::string normal = pathOf(written[t], "normal");
        const std::string atlas = pathOf(written[t], "atlas");
        if (!atlas.empty()) {
            Json frames = Json::array();
            for (size_t i = 0; i < p.source.cells.size(); i++) {
                const sn::Cell& c = p.source.cells[i];
                frames.push_back({{"name", "frame_" + std::to_string(i)}, {"rect", Json::array({c.x, c.y, c.width, c.height})}});
            }
            calls.push_back({{"tool", "set_sprite_frames"},
                             {"arguments", {{"scene", scene->id}, {"entity", state.sourceEntity}, {"texture", atlas}, {"frames", frames}}}});
            values["submeshes[0].material.normalTexture"] = textureValue(normal, 0);
            Json ms = Json::array();
            for (const sn::Delay& d : p.source.delays) ms.push_back(d.playbackMs());
            calls.push_back({{"tool", "set_properties"}, {"arguments", {{"scene", scene->id}, {"entity", state.sourceEntity}, {"values", {{"mesh", values}}}}}});
            calls.push_back({{"tool", "set_sprite_animation"},
                             {"arguments", {{"scene", scene->id}, {"sprite", state.sourceEntity}, {"name", jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(p.sourcePath).stem())},
                                            {"frame_ms", ms}, {"loop", p.source.loopCount != 1}}}});
            return calls;
        }
        // every submesh drawing this texture (a tilemap's tileset may be on several)
        const unsigned count = std::max(mesh->numSubmeshes, 1u);
        for (unsigned i = 0; i < count; i++) {
            std::string rel, why;
            if (submeshTexture(project, scene, state.sourceEntity, i, rel, why) && rel == state.targets[t].colorPath) {
                values["submeshes[" + std::to_string(i) + "].material.normalTexture"] = textureValue(normal, i);
            }
        }
    }
    calls.push_back({{"tool", "set_properties"}, {"arguments", {{"scene", scene->id}, {"entity", state.sourceEntity}, {"values", {{"mesh", values}}}}}});
    return calls;
}

// Builds the job's result once every run finished (main thread): writes and applies when asked
void finishNormals(const NormalsCallPtr& state) {
    McpContext& ctx = *state->ctx;
    const JobPtr& job = state->job;
    Project* project = ctx.project();
    // another project was opened meanwhile: nothing goes into it
    if (state->generation != texsynth::projectGeneration() || !project || project->getProjectPath() != state->root) {
        failJob(job, "Nothing was written:",
                {problem("project_changed", "", std::string(sn::kProjectChangedMessage) + " The project changed while the normal map was generated (it was started in " +
                                                    jasperengine::editor::FileUtils::pathToUtf8(state->root) + "); nothing was written.",
                         "call generate_sprite_normals again in the open project")});
        return;
    }
    bool cancelled = job->cancelRequested();
    std::vector<sn::Problem> errors;
    for (const NormalsTarget& t : state->targets) {
        const sn::Result& r = t.run->result();
        if (r.status == sn::Status::Cancelled) cancelled = true;
        else if (r.status != sn::Status::Ok) {
            for (sn::Problem p : r.errors) {
                if (p.kind.empty()) p.kind = "failed";
                errors.push_back(p);
            }
            if (r.errors.empty()) errors.push_back(problem("failed", "", "the generation failed (" + std::string(sn::toString(r.status)) + ")", "call again; report it if it persists"));
        }
    }
    if (cancelled) {
        job->markCancelled("Normal map generation cancelled; nothing was written.");
        return;
    }
    if (!errors.empty()) {
        failJob(job, "The normal map could not be generated, nothing was written:", errors);
        return;
    }

    std::vector<Report> reports;
    for (const NormalsTarget& t : state->targets) reports.push_back(report(project, *state, t));
    const bool tilemap = state->sourceKind == MeshKind::Tilemap;

    Json data = Json::object();
    Json files = Json::array();
    Json undo = Json::array();
    std::string writeText;
    std::vector<std::vector<sn::OutputFile>> written;
    // save_path first: the contact sheet (inside the project, any folder; replaced without
    // overwrite, like screenshot). A sheet that can't be written fails the call before anything
    // else is; a write that fails afterwards puts the sheet path back as it was
    SheetWrite sheet;
    if (!state->savePath.empty() && !reports.empty() && !reports[0].sheet.empty()) {
        fs::path target;
        std::string error;
        if (!McpRefs::resolveProjectPath(project, state->savePath, target, error) || !savePathStaysInside(project, state->savePath, target, error)) {
            failJob(job, "Nothing was written:", {problem("output_invalid", "save_path", error, "a project-relative .png path")});
            return;
        }
        std::string restoreWarning;
        if (!sheet.write(McpRefs::rootFor(project, target), target, reports[0].sheet, error, &restoreWarning)) {
            std::vector<sn::Problem> problems = {problem("write_failed", "save_path", "could not write the sheet to " + state->savePath + " (" + error + ")",
                                                         "save_path must name a writable file whose folders are folders (not files), then call again")};
            // the half-written sheet could not be put back: the file at save_path changed
            if (!restoreWarning.empty()) {
                problems.push_back(problem("write_failed", "save_path", restoreWarning, "restore that file yourself (its previous version could not be written back)"));
            }
            failJob(job, restoreWarning.empty() ? "Nothing was written:" : "Nothing else was written:", problems);
            return;
        }
        data["saved_to"] = relativeOf(project, target);
    }
    // a later failure puts the sheet's path back as it was (and says so when it could not)
    auto failWrite = [&](const std::string& headline, std::vector<sn::Problem> problems) {
        std::string warning;
        if (!sheet.rollback(&warning)) {
            problems.push_back(problem("write_failed", "save_path", warning, "restore that file yourself (its previous version could not be written back)"));
        }
        failJob(job, headline, problems);
    };

    SceneProject* applyScene = state->apply ? project->getScene(state->applyScene) : nullptr;
    Json applied;
    if (state->write) {
        // the scene must still be there and stopped (checked again: play may have started meanwhile)
        if (state->apply) {
            if (!applyScene || !applyScene->scene || !applyScene->scene->isEntityCreated(state->applyEntity)) {
                failWrite("Nothing was written:", {problem("invalid_apply", "apply", "the GameObject to apply to went away while the normal map was generated",
                                                              "undo its deletion or name another GameObject, then call again")});
                return;
            }
            if (applyScene->playState != ScenePlayState::STOPPED) {
                failWrite("Nothing was written:", {problem("scene_playing", "apply", "scene '" + applyScene->name + "' started playing while the normal map was generated",
                                                              "stop play first (play {action: \"stop\"}), then call again")});
                return;
            }
        }
        // checked again now: another call or a human may have written one of the files meanwhile
        std::vector<sn::OutputFile> all;
        for (const NormalsTarget& t : state->targets) {
            std::vector<sn::OutputFile> encoded;
            std::string error;
            sn::Problem why;
            if (!sn::encodeOutputs(t.run->prepared(), t.run->result(), encoded, error, &why)) {
                if (why.kind == "paint_too_large") failWrite("Nothing was written:", {problem(why.kind, "paint", why.message, why.fix)});
                else failWrite("Nothing was written:", {problem("write_failed", "output", error, "check the path is writable, then call again")});
                return;
            }
            all.insert(all.end(), encoded.begin(), encoded.end());
            written.push_back(std::move(encoded));
        }
        if (!state->overwrite) {
            std::vector<sn::Problem> existing;
            std::error_code ec;
            for (const sn::OutputFile& f : all) {
                if (fs::exists(f.absolute, ec)) {
                    existing.push_back(problem("output_exists", "output", "would replace " + f.path + " (it appeared while the normal map was generated)",
                                               "pass overwrite: true (one undo step puts it back), or another output"));
                }
            }
            if (!existing.empty()) {
                failWrite("Nothing was written:", existing);
                return;
            }
        }
        CommandHistory* history = project->getProjectCommandHistory();
        const size_t before = history ? history->position() : 0;
        std::string error;
        sn::Problem refusal;
        if (!sn::writeOutputsUndoable(project, all, error, &refusal)) {
            // an output a symbolic link now leads out of the assets: output_invalid with its fix
            if (refusal.kind == "output_invalid") failWrite("Nothing was written:", {refusal});
            else failWrite("Nothing was written:", {problem("write_failed", "output", error, "check the path is writable, then call again")});
            return;
        }
        int normals = 0;
        std::string firstNormal;
        for (const sn::OutputFile& f : all) {
            if (f.role == "normal") {
                if (firstNormal.empty()) firstNormal = f.path;
                normals++;
            }
        }
        // normal maps first, then the rest in the order written
        for (const sn::OutputFile& f : all) {
            if (f.role == "normal") files.push_back(f.path);
        }
        for (const sn::OutputFile& f : all) {
            if (f.role != "normal") files.push_back(f.path);
        }
        const std::string projectStep = normals > 1 ? "Generate Normal Maps (" + std::to_string(normals) + ")" : "Generate Normal Map " + fileName(firstNormal);
        undo.push_back({{"scope", "project"}, {"name", projectStep}});
        std::string list;
        for (const Json& f : files) list += (list.empty() ? "" : ", ") + f.get<std::string>();
        writeText = " Wrote " + list + " as one undo step (undo with scope project)";

        if (state->apply) {
            sn::ApplyRequest request;
            request.sceneId = state->applyScene;
            request.entity = state->applyEntity;
            const sn::Prepared& first = state->targets[0].run->prepared();
            const bool animated = first.source.kind == sn::SourceKind::Gif || first.source.kind == sn::SourceKind::Apng;
            for (size_t t = 0; t < written.size(); t++) {
                for (const sn::OutputFile& f : written[t]) {
                    if (f.role == "normal") request.maps.push_back({f.path, state->targets[t].colorPath});
                    if (f.role == "atlas") request.atlasPath = f.path;
                }
            }
            request.normalPath = request.maps.empty() ? std::string() : request.maps[0].normalPath;
            request.colorSourcePath = request.maps.empty() ? std::string() : request.maps[0].colorSourcePath;
            if (animated) {
                request.maps.clear();
                request.frames = first.source.cells;
                for (const sn::Delay& d : first.source.delays) request.frameMs.push_back(d.playbackMs());
                request.loopCount = first.source.loopCount;
                request.animationName = jasperengine::editor::FileUtils::pathToUtf8(fs::u8path(first.sourcePath).stem());
            }
            const sn::ApplyResult result = sn::applyToSprite(project, request);
            if (!result.ok) {
                // all or nothing: the files written for it go too
                if (history && history->position() > before && history->undo()) history->dropRedo();
                sn::Problem p = result.error;
                if (p.kind.empty()) p.kind = "write_failed";
                failWrite("Nothing was written (the files were removed again):", {p});
                return;
            }
            for (const sn::Problem& w : result.warnings) {
                bool seen = false;
                for (Report& r : reports) {
                    for (const sn::Problem& o : r.warnings) seen = seen || o.kind == w.kind;
                }
                if (!seen && !reports.empty()) {
                    reports[0].warnings.push_back(w);
                    reports[0].data["warnings"] = warningsJson(reports[0].warnings);
                }
            }
            undo.push_back({{"scope", "scene"}, {"scene", state->applyScene}, {"name", result.undoName}});
            data["entity"] = McpRefs::entityJson(applyScene, state->applyEntity);
            applied = {{"normal_texture", true}, {"submeshes", result.submeshes}};
            if (animated) {
                applied["frames"] = request.frames.size();
                if (result.animation != NULL_ENTITY) applied["animation"] = McpRefs::entityJson(applyScene, result.animation);
            }
            data["applied"] = applied;
            writeText += " and assigned it to GameObject " + applyScene->scene->getEntityName(state->applyEntity) + " (id " +
                         std::to_string(state->applyEntity) + ") as one undo step (undo with scope scene, scene '" + applyScene->name + "').";
        } else {
            writeText += ".";
            if (state->sourceEntity != NULL_ENTITY) {
                const Json args = applyArgs(project, *state, written);
                if (!args.is_null()) {
                    data["apply_args"] = args;
                    data["entity"] = McpRefs::entityJson(project->getScene(state->sourceScene), state->sourceEntity);
                    std::string normal;
                    for (const sn::OutputFile& f : written[0]) {
                        if (f.role == "normal") normal = f.path;
                    }
                    const std::string use = "Use it: set_properties {entity: " + std::to_string(state->sourceEntity) +
                                            ", values: {mesh: {\"submeshes[0].material.normalTexture\": \"" + normal + "\"}}}" +
                                            (args.size() > 1 ? " (the full calls, frames and Sprite Animation included, are in apply_args)" : std::string(" (apply_args)")) +
                                            ", or call again with apply: true.";
                    writeText += " " + use;
                    data["next"] = use;
                }
            }
        }
    }

    if (!sheet.path().empty()) w4::syncFiles(ctx, {sheet.path()}, false);

    // the result: one object for one source, `results` for a Tilemap's tilesets
    data["written"] = state->write;
    if (state->write) {
        data["files"] = files;
        data["undo"] = undo;
    }
    std::string text;
    if (tilemap) {
        Json results = Json::array();
        for (size_t i = 0; i < reports.size(); i++) {
            results.push_back(reports[i].data);
            text += (i ? "\n" : "") + std::string("Tileset ") + std::to_string(i + 1) + ": " + reports[i].summary;
        }
        data["results"] = results;
    } else {
        for (auto it = reports[0].data.begin(); it != reports[0].data.end(); ++it) data[it.key()] = it.value();
        text = reports[0].summary;
    }
    if (!state->write) {
        std::string defaults;
        for (const Report& r : reports) defaults += (defaults.empty() ? "" : ", ") + r.defaultNormal;
        const std::string next = "Preview only, nothing written: call again with the same arguments plus output (true for " + defaults + ") to write exactly this.";
        text += " " + next;
        data["next"] = next;
    } else {
        text += writeText;
    }
    if (data.contains("saved_to")) text += " Saved the sheet to " + data["saved_to"].get<std::string>() + ".";
    for (const Report& r : reports) {
        for (const sn::Problem& w : r.warnings) text += "\nWarning: " + w.kind + ": " + w.message + (w.fix.empty() ? std::string() : " Fix: " + w.fix);
    }
    if (!reports[0].sheet.empty()) {
        state->images.push_back({Base64::encode(reports[0].sheet.data(), reports[0].sheet.size()), "image/png"});
        const int lightSteps = reports[0].lightSteps;
        const std::string steps = lightSteps > 0 ? "; Light Steps " + std::to_string(lightSteps) : std::string("; smooth light");
        text += std::string("\nImage: the contact sheet") + (tilemap ? " of the first tileset" : "") +
                " (per frame: colour, normal, lit from the top-left, top-right, bottom-right, bottom-left" +
                (reports[0].painted ? "; magenta outline: painted pixels" : "") + steps + ").";
        // the legend where the agent reads (structuredContent)
        Json legend = {{"rows", tilemap ? "the frames of the first tileset" : "the frames"},
                       {"columns", Json::array({"colour", "normal", "lit from the top-left", "lit from the top-right", "lit from the bottom-right",
                                                "lit from the bottom-left"})},
                       {"light_elevation_deg", rounded(sn::kSheetLightElevationDeg, 0)},
                       {"light_steps", lightSteps}};
        if (reports[0].painted) legend["magenta_outline"] = "painted pixels";
        data["sheet_legend"] = legend;
    }
    state->text = text;
    sheet.commit();
    job->succeed(text, data);
}

void scheduleTick(const NormalsCallPtr& state);

void tick(const NormalsCallPtr& state) {
    if (state->finished) return;
    bool done = true;
    double fraction = 0.0;
    for (const NormalsTarget& t : state->targets) {
        if (!t.run->finished()) done = false;
        fraction += t.run->finished() ? 1.0 : t.run->progress().fraction;
    }
    if (!done) {
        if (state->job->cancelRequested()) {
            for (const NormalsTarget& t : state->targets) t.run->cancel();
        }
        state->job->setProgress(fraction / static_cast<double>(std::max<size_t>(1, state->targets.size())), "generating");
        scheduleTick(state);
        return;
    }
    state->finished = true;
    finishNormals(state);
}

void scheduleTick(const NormalsCallPtr& state) {
    state->ctx->app().requestRedraw();
    getEditorHost().enqueueMainThreadTask([state]() {
        // a continuation writes beneath the project, as the tool's run did
        McpWriteScope writeScope;
        // whatever goes wrong, the job ends (the main-thread loop would swallow an exception and
        // leave it running forever)
        try {
            tick(state);
        } catch (const std::exception& e) {
            state->finished = true;
            if (state->job->status() == JobStatus::Running) {
                failJob(state->job, "The normal map finished but its result could not be handled:", {problem("failed", "", e.what(), "call again")});
            }
        } catch (...) {
            state->finished = true;
            if (state->job->status() == JobStatus::Running) {
                failJob(state->job, "The normal map finished but its result could not be handled:", {problem("failed", "", "unknown error", "call again")});
            }
        }
    });
}

void generateSpriteNormalsTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    auto state = std::make_shared<NormalsCall>();
    state->ctx = &ctx;
    state->root = project->getProjectPath();
    state->generation = texsynth::projectGeneration();

    // ---- settings, frames, output (no file read yet)
    std::vector<sn::Problem> problems;
    sn::Settings settings;
    sn::FramesSpec frames;
    sn::OutputSpec output;
    bool customOutput = false;
    if (!settingsArg(args, settings, state->lightSteps, problems) || !framesArg(args, frames, problems) ||
        !outputArg(args, output, state->write, customOutput, problems)) {
        failCall(call, problems);
        return;
    }
    // paint (Phase 5): operations on the output and where the starting overlay comes from
    std::vector<sn::PaintOp> paint;
    sn::PaintMode paintMode = sn::PaintMode::Keep;
    if (args.contains("paint_mode") && !args["paint_mode"].is_null()) {
        const Json& m = args["paint_mode"];
        if (!m.is_string() || !sn::parsePaintMode(m.get<std::string>(), paintMode)) {
            failCall(call, problem("invalid_paint", "paint_mode", "paint_mode must be keep, replace or clear (got " + m.dump() + ")",
                                   "keep (default) starts from the existing map's paint, replace from none, clear writes none"));
            return;
        }
    }
    if (args.contains("paint") && !args["paint"].is_null()) {
        if (!sn::paintOpsFromJson(args["paint"], paint, problems)) {
            failCall(call, problems);
            return;
        }
        if (paintMode == sn::PaintMode::Clear && !paint.empty()) {
            failCall(call, problem("invalid_paint", "paint", "paint_mode \"clear\" drops the paint, so paint must be empty (got " + std::to_string(paint.size()) + " operations)",
                                   "use paint_mode \"replace\" to start over with these operations"));
            return;
        }
    }
    for (const char* key : {"overwrite", "wait"}) {
        if (args.contains(key) && !args[key].is_boolean()) {
            failCall(call, problem("invalid_settings", key, std::string(key) + " must be true or false (got " + args[key].dump() + ")",
                                   std::string("pass ") + key + ": true or " + key + ": false, or leave it out (" +
                                       (std::string(key) == "wait" ? "true" : "false") + ")"));
            return;
        }
    }
    state->overwrite = args.value("overwrite", false);
    const bool wait = args.value("wait", true);
    double timeout = 120.0;
    if (args.contains("timeout_seconds")) {
        const Json& t = args["timeout_seconds"];
        if (!t.is_number() || t.get<double>() < 1.0 || t.get<double>() > 3600.0) {
            failCall(call, problem("invalid_settings", "timeout_seconds", "timeout_seconds must be a number in [1, 3600] (got " + t.dump() + ")", "e.g. 120"));
            return;
        }
        timeout = t.get<double>();
    }
    const Json applyValue = args.contains("apply") ? args["apply"] : Json(false);
    state->apply = !(applyValue.is_null() || (applyValue.is_boolean() && !applyValue.get<bool>()));
    if (state->apply && !state->write) {
        failCall(call, problem("apply_needs_output", "apply", "apply assigns the written file: add output: true", "add output: true (or an output path)"));
        return;
    }
    if (args.contains("save_path")) {
        const Json& s = args["save_path"];
        if (!s.is_string() || lowerExtension(s.get<std::string>()) != ".png") {
            failCall(call, problem("output_invalid", "save_path", "save_path must be a project-relative .png path (got " + s.dump() + ")", "e.g. \"assets/sheets/hero_sheet_preview.png\""));
            return;
        }
        fs::path target;
        std::string error;
        if (!McpRefs::resolveProjectWritePath(project, s.get<std::string>(), target, error)) {
            failCall(call, problem("output_invalid", "save_path", "save_path: " + error, "a project-relative .png path"));
            return;
        }
        if (!savePathStaysInside(project, s.get<std::string>(), target, error)) {
            failCall(call, problem("output_invalid", "save_path", "save_path: " + error,
                                   "a project-relative .png path in a folder of the project that is not a link to elsewhere"));
            return;
        }
        state->savePath = relativeOf(project, target);
    }

    // ---- the source: an image file, or a Sprite / Polygon / Tilemap GameObject
    if (!args.contains("source") || args["source"].is_null()) {
        failCall(call, problem("invalid_source", "source", "source is required",
                               "pass a project image path (assets/sprites/hero.png) or a Sprite GameObject"));
        return;
    }
    const Json& sourceArg = args["source"];
    state->sourceLabel = sourceArg.is_string() ? sourceArg.get<std::string>() : sourceArg.dump();
    std::string fileSource;
    std::string sceneError;
    SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), sceneError);
    if (sourceArg.is_string()) {
        const std::string text = sourceArg.get<std::string>();
        fs::path absolute;
        std::string error;
        if (Util::isImageFile(text) && lowerExtension(text) != ".svg" && McpRefs::resolveProjectPath(project, text, absolute, error, true)) {
            fileSource = relativeOf(project, absolute);
        }
    }
    if (fileSource.empty()) {
        if (!sourceArg.is_string() && !sourceArg.is_number_integer() && !sourceArg.is_object()) {
            failCall(call, problem("invalid_source", "source", "source must be an image path or a GameObject (got " + sourceArg.dump() + ")",
                                   "pass a project image path (assets/sprites/hero.png) or a Sprite GameObject"));
            return;
        }
        if (!scene || !scene->scene) {
            failCall(call, problem("invalid_source", "source", "no image file '" + state->sourceLabel + "' in the project, and no scene to look for a GameObject in (" + sceneError + ")",
                                   "pass a project image path (assets/sprites/hero.png) or a Sprite GameObject"));
            return;
        }
        std::string error;
        const Entity entity = McpRefs::resolveEntity(scene, sourceArg, error);
        if (entity == NULL_ENTITY) {
            const bool image = sourceArg.is_string() && Util::isImageFile(sourceArg.get<std::string>());
            failCall(call, problem("invalid_source", "source",
                                   (sourceArg.is_string() ? "no image file '" + state->sourceLabel + "' in the project and no" : std::string("no")) +
                                       " GameObject " + state->sourceLabel + " in scene '" + scene->name + "' (" + (image ? "the file does not exist" : error) + ")",
                                   "pass a project image path (assets/sprites/hero.png) or a Sprite GameObject"));
            return;
        }
        const MeshKind kind = meshKind(scene, entity);
        if (kind == MeshKind::None) {
            failCall(call, problem("invalid_source", "source", "GameObject '" + scene->scene->getEntityName(entity) + "' is " + whatIs(scene, entity) + " (got " + state->sourceLabel + ")",
                                   "pass a project image path (assets/sprites/hero.png) or a Sprite, Polygon or Tilemap GameObject"));
            return;
        }
        state->sourceScene = scene->id;
        state->sourceEntity = entity;
        state->sourceKind = kind;
    }

    // ---- apply: the source GameObject, or the one named
    if (state->apply) {
        if (applyValue.is_boolean()) {
            if (state->sourceEntity == NULL_ENTITY) {
                failCall(call, problem("invalid_apply", "apply", "apply: true wires the source GameObject; with an image source name the GameObject: apply: \"Hero\"",
                                       "apply: \"<GameObject>\" (with the scene argument when it is not in the selected scene)"));
                return;
            }
            state->applyScene = state->sourceScene;
            state->applyEntity = state->sourceEntity;
        } else {
            if (!scene || !scene->scene) {
                failCall(call, problem("invalid_apply", "apply", "no scene to find the GameObject " + applyValue.dump() + " in (" + sceneError + ")", "pass scene"));
                return;
            }
            std::string error;
            const Entity entity = McpRefs::resolveEntity(scene, applyValue, error);
            if (entity == NULL_ENTITY) {
                failCall(call, problem("invalid_apply", "apply", "apply: " + error, "name a Sprite, Polygon or Tilemap GameObject of scene '" + scene->name + "'"));
                return;
            }
            if (meshKind(scene, entity) == MeshKind::None) {
                failCall(call, problem("invalid_apply", "apply", "GameObject '" + scene->scene->getEntityName(entity) + "' is " + whatIs(scene, entity),
                                       "apply to a Sprite, Polygon or Tilemap GameObject"));
                return;
            }
            state->applyScene = scene->id;
            state->applyEntity = entity;
        }
        SceneProject* target = project->getScene(state->applyScene);
        if (target && target->playState != ScenePlayState::STOPPED) {
            failCall(call, problem("scene_playing", "apply", "scene '" + target->name + "' is playing: apply edits the scene",
                                   "stop play first (play {action: \"stop\"}), then call again"));
            return;
        }
    }

    // ---- one spec per colour texture (a Tilemap: one per tileset)
    std::vector<unsigned> submeshes;
    if (state->sourceEntity != NULL_ENTITY) {
        SceneProject* sp = project->getScene(state->sourceScene);
        MeshComponent* mesh = sp->scene->findComponent<MeshComponent>(state->sourceEntity);
        const unsigned count = state->sourceKind == MeshKind::Tilemap ? mesh->numSubmeshes : 1;
        std::set<std::string> seen;
        std::string firstWhy;
        for (unsigned i = 0; i < count; i++) {
            std::string rel, why;
            if (!submeshTexture(project, sp, state->sourceEntity, i, rel, why)) {
                if (firstWhy.empty()) firstWhy = why;
                continue;
            }
            if (!seen.insert(rel).second) continue;
            NormalsTarget t;
            t.colorPath = rel;
            submeshes.push_back(i);
            state->targets.push_back(t);
        }
        if (state->targets.empty()) {
            failCall(call, problem("invalid_source", "source", "the " + std::string(meshKindName(state->sourceKind)) + " '" + sp->scene->getEntityName(state->sourceEntity) +
                                                                   "' can't be the source: " + (firstWhy.empty() ? std::string("it has no tileset texture") : firstWhy) +
                                                                   " (got " + state->sourceLabel + ")",
                                   "give it a texture file first, or pass the image path"));
            return;
        }
        if (state->targets.size() > 1 && !paint.empty()) {
            failCall(call, problem("invalid_paint", "paint", "paint applies to one normal map, and this Tilemap has " + std::to_string(state->targets.size()) +
                                                                 " tileset textures (one map each)",
                                   "one call per output: pass the tileset image as source"));
            return;
        }
        if (state->targets.size() > 1 && customOutput) {
            failCall(call, problem("output_invalid", "output", "a Tilemap with " + std::to_string(state->targets.size()) +
                                                                   " tileset textures needs output: true (one normal map per texture)",
                                   "output: true"));
            return;
        }
    } else {
        NormalsTarget t;
        t.colorPath = fileSource;
        state->targets.push_back(t);
    }
    for (size_t i = 0; i < state->targets.size(); i++) {
        sn::Spec& spec = state->targets[i].spec;
        spec.settings = settings;
        spec.frames = frames;
        spec.output = output;
        // a preview never writes: an existing default output is no problem then
        spec.overwrite = state->overwrite || !state->write;
        spec.paint = paint;
        spec.paintMode = paintMode;
        if (state->sourceEntity != NULL_ENTITY) {
            spec.sceneId = state->sourceScene;
            spec.entity = state->sourceEntity;
            spec.submesh = submeshes[i];
        } else {
            spec.source = fileSource;
        }
    }

    // ---- every problem visible without decoding, then the sources decoded (main thread, <= 64 MiB)
    SceneProject* sourceScene = state->sourceEntity != NULL_ENTITY ? project->getScene(state->sourceScene) : nullptr;
    const Scene* specScene = sourceScene ? sourceScene->scene : nullptr;
    std::vector<std::string> existing;
    for (NormalsTarget& t : state->targets) {
        std::vector<sn::Problem> found = sn::checkSpec(state->root, t.spec, specScene);
        if (std::any_of(found.begin(), found.end(), [](const sn::Problem& p) { return p.kind == "output_exists"; })) {
            // what the spec writes (its explicit paths, else the default names of the source's kind)
            sn::SourceInfo info;
            std::vector<sn::Problem> probe;
            sn::probeSource(state->root / fs::u8path(t.colorPath), info, probe);
            const bool animated = info.kind == sn::SourceKind::Gif || info.kind == sn::SourceKind::Apng;
            rewordExisting(found, sn::plannedOutputs(state->root, t.spec, animated ? info.kind : sn::SourceKind::Image), existing);
        }
        problems.insert(problems.end(), found.begin(), found.end());
    }
    if (!problems.empty()) {
        Json data = Json::object();
        if (!existing.empty()) data["existing"] = existing;
        failCall(call, problems, data);
        return;
    }
    std::vector<sn::Prepared> prepared(state->targets.size());
    std::set<std::string> outputs, inputs;
    for (size_t i = 0; i < state->targets.size(); i++) {
        if (!sn::prepare(state->root, state->targets[i].spec, specScene, prepared[i], problems)) {
            failCall(call, problems);
            return;
        }
        // compared with their links resolved: an alias of a file is that file
        inputs.insert(jasperengine::editor::FileUtils::pathToGenericUtf8(sn::resolvedPath(prepared[i].sourceAbsolute)));
        if (state->write && output.framesJson && prepared[i].request.cells.empty()) {
            failCall(call, problem("output_invalid", "output.frames", "output.frames writes a frames JSON, but " + prepared[i].sourcePath + " has no frames (one whole image)",
                                   "pass frames (a grid or cells) or a GIF/APNG, or leave output.frames out"));
            return;
        }
    }
    // outputs of different tilesets must differ, and never replace an input of the call (or the sheet)
    const std::string sheetKey = state->savePath.empty() ? std::string() : jasperengine::editor::FileUtils::pathToGenericUtf8(sn::resolvedPath(state->root / fs::u8path(state->savePath)));
    for (size_t i = 0; state->write && i < prepared.size(); i++) {
        for (const sn::PlannedOutput& o : prepared[i].outputs) {
            const std::string key = jasperengine::editor::FileUtils::pathToGenericUtf8(sn::resolvedPath(state->root / fs::u8path(o.path)));
            if (inputs.count(key) || !outputs.insert(key).second || (!sheetKey.empty() && key == sheetKey)) {
                failCall(call, problem("output_invalid", "output." + o.role, o.path + " is an input of this call, or written twice by it",
                                       "an assets path ending in .png, or output: true"));
                return;
            }
        }
    }
    if (!sheetKey.empty() && inputs.count(sheetKey)) {
        failCall(call, problem("output_invalid", "save_path", "save_path " + state->savePath + " is the source", "save the sheet elsewhere"));
        return;
    }

    // ---- the runs (worker threads), polled by the job's tick
    std::string firstNormal;
    for (const sn::PlannedOutput& o : prepared[0].outputs) {
        if (o.role == "normal") firstNormal = o.path;
    }
    const std::string title = "Generate normal map " + prepared[0].sourcePath + (state->write ? " -> " + firstNormal : std::string(" (preview)"));
    state->job = ctx.jobs().create(TOOL, title);
    for (size_t i = 0; i < state->targets.size(); i++) state->targets[i].run = sn::SpriteNormalsRun::start(std::move(prepared[i]));
    std::weak_ptr<NormalsCall> weak = state;
    state->job->setCancelHandler([weak]() {
        if (auto s = weak.lock()) {
            for (const NormalsTarget& t : s->targets) {
                if (t.run) t.run->cancel();
            }
        }
    });
    state->job->setProgress(0.0, "starting");
    scheduleTick(state);

    call->detach();
    replyWhenJobDone(call, state->job, wait, timeout, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(state->text.empty() ? finished.message() : state->text, result);
        reply.images = state->images;
        return reply;
    });
}

// ---- get_asset_info / view_asset

// The settings of a recipe (as specFromRecipe resolves them: a version 1 recipe keeps version 1)
// that differ from its preset's (the `params` of `regenerate`)
Json paramsDiff(const sn::Settings& resolved) {
    const Json preset = sn::settingsToJson(sn::presetSettings(resolved.preset));
    const Json actual = sn::settingsToJson(resolved);
    Json params = Json::object();
    for (const std::string& key : paramKeys()) {
        if (actual.contains(key) && (!preset.contains(key) || preset[key] != actual[key])) params[key] = actual[key];
    }
    return params;
}

// The open scene and GameObject a recipe's entity path names, when it still draws the source
bool findRecipeSprite(Project* project, const std::string& entityPath, const std::string& source, SceneProject*& scene, Entity& entity) {
    for (SceneProject& sp : project->getScenes()) {
        if (!sp.scene || entityPath.empty()) continue;
        const Entity e = EntityPath::find(sp, entityPath);
        const MeshKind kind = meshKind(&sp, e);
        if (kind != MeshKind::Sprite && kind != MeshKind::Polygon) continue;
        std::string rel, why;
        if (!submeshTexture(project, &sp, e, 0, rel, why) || rel != source) continue;
        scene = &sp;
        entity = e;
        return true;
    }
    return false;
}

// generate_sprite_normals arguments that write the map again (3.9)
bool regenerateArgs(Project* project, const Json& recipe, const fs::path& png, Json& args, std::vector<std::string>& notes, std::string& error) {
    sn::Spec spec;
    if (!sn::specFromRecipe(recipe, png, project->getProjectPath(), spec, notes, error)) return false;
    const Json& source = recipe["source"];
    const Json framesNode = recipe.contains("frames") ? recipe["frames"] : Json::object();
    const std::string from = framesNode.is_object() && framesNode.contains("from") && framesNode["from"].is_string() ? framesNode["from"].get<std::string>() : "single";
    args = Json::object();
    args["source"] = spec.source;
    bool fromEntity = false;
    if (source.contains("entity") && source["entity"].is_string() && spec.submesh == 0) {
        SceneProject* scene = nullptr;
        Entity entity = NULL_ENTITY;
        if (findRecipeSprite(project, source["entity"].get<std::string>(), spec.source, scene, entity)) {
            args["source"] = McpRefs::entityPath(scene, entity);
            args["scene"] = scene->id;
            fromEntity = true;
            notes.clear();
        }
    }
    const Json settings = recipe.contains("settings") ? recipe["settings"] : Json::object();
    args["preset"] = settings.is_object() && settings.contains("preset") ? settings["preset"] : Json("default");
    const Json params = paramsDiff(spec.settings);
    if (!params.empty()) args["params"] = params;
    // a grid (or Sprite frames that can't be read again from the GameObject): the cells themselves
    if (from == "given" || (from == "sprite" && !fromEntity)) {
        Json cells = Json::array();
        for (const sn::Cell& c : spec.frames.cells) cells.push_back(Json::array({c.x, c.y, c.width, c.height}));
        args["frames"] = cells.empty() ? Json("auto") : Json{{"cells", cells}};
    } else {
        args["frames"] = "auto";
    }
    args["directx"] = spec.settings.directx;
    args["height"] = spec.settings.height;
    Json output = Json::object();
    if (!spec.output.normal.empty()) output["normal"] = spec.output.normal;
    if (!spec.output.atlas.empty()) output["atlas"] = spec.output.atlas;
    if (!spec.output.height.empty()) output["height"] = spec.output.height;
    args["output"] = output;
    args["overwrite"] = true;
    // a painted map: its paint is kept (and re-composited) by the regeneration
    if (recipe.contains("paint")) args["paint_mode"] = "keep";
    return true;
}

bool sameFile(const fs::path& a, const fs::path& b) {
    return a.lexically_normal() == b.lexically_normal();
}

// The default-name normal maps of an image (contract 5): <stem>_normal.png, <stem>_sheet_normal.png
std::vector<fs::path> defaultMapsOf(const fs::path& image) {
    const std::string stem = jasperengine::editor::FileUtils::pathToUtf8(image.stem());
    return {image.parent_path() / fs::u8path(stem + "_normal.png"), image.parent_path() / fs::u8path(stem + "_sheet_normal.png")};
}

}

Json spriteNormalsAssetInfo(Project* project, const fs::path& file, bool full) {
    const fs::path root = project->getProjectPath();
    const std::string rel = relativeOf(project, file);
    if (!Util::isImageFile(rel) || lowerExtension(rel) == ".svg") return Json();
    if (lowerExtension(rel) == ".png") {
        std::string readError;
        sn::RecipeReadError origin = sn::RecipeReadError::None;
        const std::optional<Json> recipe = sn::readRecipeHead(file, &readError, &origin);
        // only a damaged jasper.spritenormals chunk is a garbled recipe; a file that is not a PNG,
        // can't be opened or has a broken chunk list simply has none (decided on the origin)
        if (recipe || origin == sn::RecipeReadError::Chunk) {
            std::string invalid;
            if (!recipe) return Json{{"sprite_normals", {{"error", readError}}}};
            if (!sn::validateRecipe(*recipe, invalid)) return Json{{"sprite_normals", {{"error", "garbled jasper.spritenormals recipe: " + invalid}}}};
            const sn::Staleness staleness = sn::checkStaleness(root, file);
            const Json& source = (*recipe)["source"];
            Json src = {{"path", staleness.sourceAbsolute.empty() ? staleness.sourcePath : relativeOf(project, staleness.sourceAbsolute)},
                        {"kind", source.value("kind", std::string("image"))}, {"state", sn::toString(staleness.state)}};
            if (source.contains("entity")) src["entity"] = source["entity"];
            const Json framesNode = recipe->contains("frames") ? (*recipe)["frames"] : Json::object();
            const size_t cells = framesNode.contains("cells") && framesNode["cells"].is_array() ? framesNode["cells"].size() : 0;
            Json info = {{"role", recipe->value("role", std::string("normal"))}, {"source", src},
                         {"preset", recipe->contains("settings") ? (*recipe)["settings"].value("preset", std::string("default")) : std::string("default")},
                         {"frames", {{"from", framesNode.value("from", std::string("single"))}, {"count", cells == 0 ? 1 : cells}}},
                         {"outputs", recipe->contains("outputs") ? (*recipe)["outputs"] : Json::object()},
                         {"stale", staleness.state != sn::Staleness::State::Fresh}};
            if (staleness.state != sn::Staleness::State::Fresh) info["staleness"] = staleness.message;
            // the paint layer: its summary from the chunk's head (not decoded)
            std::string paintError;
            if (const std::optional<sn::PaintSummary> paint = sn::readPaintSummary(file, &paintError)) {
                info["paint"] = {{"painted", paint->painted}, {"size", Json::array({paint->width, paint->height})}};
            } else if (!paintError.empty()) {
                info["paint"] = {{"error", paintError}};
            }
            if (full) {
                info["recipe"] = *recipe;
                Json args;
                std::vector<std::string> notes;
                std::string error;
                if (regenerateArgs(project, *recipe, file, args, notes, error)) {
                    info["regenerate"] = args;
                    if (!notes.empty()) info["regenerate_notes"] = notes;
                } else {
                    info["regenerate_error"] = error;
                }
            }
            return Json{{"sprite_normals", info}};
        }
    }
    // a plain image: its normal map by the default names (a cheap probe, no scan)
    for (const fs::path& candidate : defaultMapsOf(file)) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec)) continue;
        const sn::Staleness staleness = sn::checkStaleness(root, candidate);
        if (staleness.state == sn::Staleness::State::NoRecipe || staleness.state == sn::Staleness::State::Invalid ||
            staleness.state == sn::Staleness::State::Unreadable) continue;
        if (!sameFile(staleness.sourceAbsolute, file)) continue;
        return Json{{"normal_map", {{"path", relativeOf(project, candidate)}, {"stale", staleness.state != sn::Staleness::State::Fresh}}}};
    }
    return Json();
}

std::string spriteNormalsAssetText(const Json& info) {
    if (!info.is_object()) return std::string();
    if (info.contains("sprite_normals")) {
        const Json& n = info["sprite_normals"];
        if (n.contains("error")) return " Its sprite normal map recipe can't be used (" + n["error"].get<std::string>() + ").";
        const Json& source = n["source"];
        std::string text = " Sprite normal map (" + n.value("role", std::string("normal")) + ", preset " + n.value("preset", std::string("default")) + ") of " +
                           source.value("path", std::string("?"));
        const std::string state = source.value("state", std::string());
        if (state == "fresh") text += "; source unchanged.";
        else if (state == "changed") text += "; the source changed since: regenerate with generate_sprite_normals and the `regenerate` arguments (get_asset_info).";
        else if (state == "missing") text += "; the source is gone (the map itself still works).";
        else text += "; its source can't be checked (" + n.value("staleness", state) + ").";
        if (n.contains("paint") && n["paint"].is_object()) {
            const Json& paint = n["paint"];
            if (paint.contains("painted")) text += " Painted: " + std::to_string(paint.value("painted", 0)) + " px (kept when it is regenerated).";
            else text += " Its paint layer can't be read (" + paint.value("error", std::string()) + "); regenerating drops it.";
        }
        return text;
    }
    if (info.contains("normal_map")) {
        const Json& m = info["normal_map"];
        return " Its normal map: " + m.value("path", std::string()) +
               (m.value("stale", false) ? " (out of date: regenerate it with generate_sprite_normals and get_asset_info's `regenerate` arguments of that map)."
                                        : " (up to date).");
    }
    return std::string();
}

void spriteNormalsFileChanged(Project* project, const fs::path& path, Json& problems) {
    const std::string rel = relativeOf(project, path);
    if (!Util::isImageFile(rel) || lowerExtension(rel) == ".svg") return;
    const fs::path root = project->getProjectPath();
    // the maps made from it: first the editor's watcher index (SpriteNormalsWatcher::outputsOf,
    // every recipe of the project), then the default names beside it and the check cache's reverse
    // index (written by check --normals; no scan here), which also cover a map the watcher has not
    // scanned yet and editors without one. Each candidate's recipe is read again below.
    std::vector<fs::path> candidates;
    SpriteNormalsWatcher* watcher = SpriteNormalsWatcher::active();
    if (watcher) candidates = watcher->outputsOf(path);
    for (const fs::path& named : defaultMapsOf(path)) candidates.push_back(named);
    for (const fs::path& cached : sn::cachedMapsOf(root, path)) candidates.push_back(cached);
    std::set<fs::path> seen;
    for (const fs::path& candidate : candidates) {
        if (!seen.insert(candidate.lexically_normal()).second || sameFile(candidate, path)) continue;
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec)) continue;
        const sn::Staleness staleness = sn::checkStaleness(root, candidate);
        if (!sameFile(staleness.sourceAbsolute, path)) continue;
        const std::string map = relativeOf(project, candidate);
        if (staleness.state == sn::Staleness::State::Changed) {
            problems.push_back({{"path", map}, {"message", "made from " + rel + ", which changed: regenerate it (generate_sprite_normals with get_asset_info's regenerate arguments)"}});
        } else if (staleness.state == sn::Staleness::State::Missing) {
            problems.push_back({{"path", map}, {"message", "made from " + rel + ", which is gone: restore it, or generate the map from another source (the map itself still works)"}});
        }
    }
    // the watcher reads the changed file again soon (a new generated PNG wakes a dormant one)
    if (watcher) watcher->invalidate(path);
}

void registerSpriteNormalsTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "generate_sprite_normals";
    tool.title = "Generate Sprite Normals";
    tool.description =
        "Sprite normal maps, pixel art to 4096 px (sheets, GIF/APNG, tiles). No output: lit sheet + metrics; "
        "output writes it (one undo); apply wires it on. "
        "paint: fix spots (angle/flatten/smooth/erase by rect, pixels, colors or frame); kept on regenerate (paint_mode). "
        "Guide: jasper://docs/sprite-normals.";
    // nested objects are loose (the handler refuses unknown keys itself, saving tools/list bytes);
    // what each argument does: docs/mcp/guide/sprite-normals.md
    auto types = [](std::initializer_list<const char*> names) {
        Json j;
        j["type"] = Json::array();
        for (const char* n : names) j["type"].push_back(n);
        return j;
    };
    Json frames;
    frames["anyOf"] = Json::array({stringEnum("", {"auto", "none"}), Json{{"type", "object"}}});
    tool.inputSchema = object({
        {"source", types({"string", "integer", "object"})},
        {"scene", sceneRef()},
        {"preset", stringEnum("", {"soft", "default", "crisp", "detailed", "chunky", "toon", "painted", "props", "tile", "stones"})},
        {"params", anyObject("")},
        {"frames", frames},
        // the defaults (preset default; directx, height, overwrite false; wait true) are in the
        // guide's argument table
        {"directx", boolean("")},
        {"height", boolean("")},
        {"output", types({"string", "boolean", "object"})},
        {"overwrite", boolean("")},
        {"apply", types({"boolean", "string", "integer", "object"})},
        {"save_path", string("")},
        // paint ops are loose too (validated by the handler: paint[i].<field> errors); paint_mode
        // keep (default), replace or clear (the handler names them; the tool budget, P5)
        {"paint", Json{{"type", "array"}}},
        {"paint_mode", string("")},
        {"wait", boolean("")},
        {"timeout_seconds", withDefault(number("", 1, 3600), 120)}
    }, {"source"});
    tool.writesFiles = [](const Json& args) {
        const bool output = args.contains("output") && !args["output"].is_null() && !(args["output"].is_boolean() && !args["output"].get<bool>());
        return output || args.contains("save_path");
    };
    // files can be written while playing; apply is refused then (the handler checks)
    tool.allowedWhilePlaying = true;
    tool.activity = ToolActivity::Edit;
    tool.handler = generateSpriteNormalsTool;
    registry.addTool(std::move(tool));
}

}
