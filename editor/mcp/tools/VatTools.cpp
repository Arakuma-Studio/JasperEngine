// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Vertex Animation (vertex animation spec 6): bake_vertex_animation (the baker through
// vat/VatBakeSession, the Houdini VAT 3 / OpenVAT importers through vat/import, sharing
// vat/VatFrontEnd with the bake-vat CLI), and the .vat parts of get_asset_info, write_file,
// view_asset and the file sync. Guide: docs/mcp/guide/vertex-animation.md.
//
// A bake is a job (the place_with_physics pattern): posing runs on the main thread a slice per
// editor frame (VatBakeSession::advance), encoding on the session's worker; an import runs on a
// worker thread of its own. Without output nothing is written: the report and a contact sheet
// (vat/VatContactSheet: the source and the VAT drawn the same way at 4 times per clip).
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
#include "animation/ClipConverters.h"
#include "animation/ClipEditorUtil.h"
#include "pose/PoseEditSession.h"
#include "texsynth/SynthesisSession.h"
#include "command/type/CreateVatCmd.h"
#include "util/AssetDatabase.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "vat/VatContactSheet.h"
#include "vat/VatDocument.h"
#include "vat/VatFrontEnd.h"

#include "Scene.h"
#include "component/VertexAnimatorComponent.h"
#include "pool/VertexAnimationPool.h"
#include "util/Base64.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>

namespace jasperengine::editor::mcp {

namespace fs = std::filesystem;
using namespace ::jasperengine::editor::vat;
using ::jasperengine::vat::VatClip;
using ::jasperengine::vat::VatData;
using ::jasperengine::vat::VatMode;

namespace {

constexpr double SLICE_MS = 8.0;

std::string fixed(double v, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
    return buffer;
}

std::string relativeOf(Project* project, const fs::path& absolute) {
    return FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(absolute.lexically_normal()));
}

// "kind: message (fix)" lines for an error result
std::string issuesLines(const std::vector<VatIssue>& issues, bool errorsOnly) {
    std::string text;
    for (const VatIssue& issue : issues) {
        if (errorsOnly && !issue.error) continue;
        text += "\n- " + issue.kind + ": " + issue.message + (issue.fix.empty() ? std::string() : ". Fix: " + issue.fix);
        if (!issue.fixes.empty()) {
            text += " (";
            for (size_t i = 0; i < issue.fixes.size(); i++) {
                text += (i ? "; " : "") + issue.fixes[i].label + ": " + issue.fixes[i].setting + " " + issue.fixes[i].value;
            }
            text += ")";
        }
    }
    return text;
}

Json errorsJson(const std::vector<VatIssue>& issues) {
    Json list = Json::array();
    for (const VatIssue& issue : issues) {
        if (issue.error) list.push_back(issueJson(issue));
    }
    return list;
}

VatIssue argIssue(const std::string& message, const std::string& fix) {
    VatIssue issue;
    issue.kind = "invalid_settings";
    issue.error = true;
    issue.message = message;
    issue.fix = fix;
    return issue;
}

// ---------------------------------------------------------------- the request

struct VatRequest {
    bool import = false;
    bool rebake = false;
    std::string rebakePath;             // project-relative .vat
    bool rebakeImport = false;
    VatBakeSpec spec;                   // bake (output only when writing)
    VatImportSpec importSpec;
    VatImportDetection detection;
    VatImportWriteOptions importWrite;
    bool write = false;                 // output given, or rebake
    bool defaultOutput = false;         // output: true
    std::string sceneName;              // for messages: the scene placed in / posed
    uint32_t placeScene = 0;            // a file source placed: the scene given (selected before the write)
};

// A GameObject reference (id, or a path in `scene`): false when it is not one; error set when it
// looked like one but failed
bool gameObjectArg(Project* project, const Json& args, const Json& ref, SceneProject*& scene, Entity& entity, std::string& error) {
    scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene) return false;
    if (!scene->scene) {
        error = McpRefs::notOpenError(project, *scene);
        return false;
    }
    entity = McpRefs::resolveEntity(scene, ref, error);
    return entity != NULL_ENTITY;
}

// a project file that exists (or "model#Clip")
bool looksLikeFile(Project* project, const std::string& text) {
    std::error_code ec;
    const fs::path given = FileUtils::pathFromUtf8(text);
    if (given.is_absolute()) return fs::exists(given, ec);
    if (fs::exists(project->getProjectPath() / given, ec)) return true;
    const size_t hash = text.rfind('#');
    return hash != std::string::npos && hash > 0 && fs::exists(project->getProjectPath() / FileUtils::pathFromUtf8(text.substr(0, hash)), ec);
}

bool stringList(const Json& value, const char* field, std::vector<std::string>& out, std::string& error) {
    if (value.is_string()) {
        out.push_back(value.get<std::string>());
        return true;
    }
    if (!value.is_array()) {
        error = std::string(field) + " must be a string or an array of strings.";
        return false;
    }
    for (const Json& item : value) {
        if (!item.is_string() || item.get<std::string>().empty()) {
            error = std::string(field) + " must hold non-empty strings (got " + item.dump() + ").";
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

bool numberArg(const Json& args, const char* key, double minimum, double maximum, bool exclusiveMinimum, double& out, std::string& error) {
    if (!args.contains(key)) return true;
    const Json& v = args[key];
    if (!v.is_number() || (exclusiveMinimum ? v.get<double>() <= minimum : v.get<double>() < minimum) || v.get<double>() > maximum) {
        error = std::string(key) + " must be a number in " + (exclusiveMinimum ? "(" : "[") + fixed(minimum, 0) + ", " + fixed(maximum, 0) + "] (got " +
                v.dump() + ").";
        return false;
    }
    out = v.get<double>();
    return true;
}

bool boolArg(const Json& args, const char* key, bool& out, std::string& error) {
    if (!args.contains(key)) return true;
    if (!args[key].is_boolean()) {
        error = std::string(key) + " must be true or false.";
        return false;
    }
    out = args[key].get<bool>();
    return true;
}

bool placeArg(const Json& args, VatPlace& place, VatCrowd& crowd, std::string& error) {
    if (!args.contains("place")) return true;
    const Json& value = args["place"];
    if (value.is_string()) {
        const std::string name = value.get<std::string>();
        if (name == "crowd") {
            place = VatPlace::Crowd;
            return true;
        }
        if (!placeFromName(name, place) || place == VatPlace::Crowd) {
            error = "place must be none, beside, replace or {\"crowd\": {\"count\": 500, \"area\": 30, \"seed\": 1}} (got \"" + name + "\").";
            return false;
        }
        return true;
    }
    if (value.is_object() && value.size() == 1 && value.contains("crowd") && value["crowd"].is_object()) {
        place = VatPlace::Crowd;
        for (auto it = value["crowd"].begin(); it != value["crowd"].end(); ++it) {
            const std::string key = it.key();
            const Json& v = it.value();
            if (key == "count" && v.is_number_integer() && v.get<long long>() >= 1 && v.get<long long>() <= 100000) {
                crowd.count = v.get<int>();
            } else if (key == "area" && v.is_number() && v.get<double>() > 0.0 && v.get<double>() <= 100000.0) {
                crowd.area = v.get<float>();
            } else if (key == "seed" && v.is_number_integer() && v.get<long long>() >= 0 && v.get<long long>() <= 4294967295LL) {
                crowd.seed = static_cast<uint32_t>(v.get<long long>());
            } else {
                error = "place.crowd." + key + ": count is an integer 1..100000, area a side in metres > 0, seed an integer >= 0 (got " + v.dump() + ").";
                return false;
            }
        }
        return true;
    }
    error = "place must be none, beside, replace or {\"crowd\": {\"count\", \"area\", \"seed\"}} (got " + value.dump() + ").";
    return false;
}

bool importOptionsArg(Project* project, const Json& args, VatImportSpec& spec, std::string& error) {
    if (!args.contains("import")) return true;
    const Json& options = args["import"];
    if (!options.is_object()) {
        error = "import must be an object of importer options (axis, axis_scale, upsample, clip, colors, or file overrides: mesh, position, "
                "position2, rotation, color, lookup, vat, normals_texture).";
        return false;
    }
    for (auto it = options.begin(); it != options.end(); ++it) {
        const std::string key = it.key();
        const Json& v = it.value();
        std::string* file = key == "mesh" ? &spec.mesh : key == "position" ? &spec.position : key == "position2" ? &spec.position2
                          : key == "rotation" ? &spec.rotation : key == "color" ? &spec.color : key == "lookup" ? &spec.lookup
                          : key == "vat" ? &spec.vat : key == "normals_texture" ? &spec.normalsTexture : nullptr;
        if (file) {
            std::string relative;
            if (!v.is_string() || !projectRelativeFile(project, v.get<std::string>(), true, relative, error)) {
                error = "import." + key + ": " + (v.is_string() ? error : std::string("a project-relative file path")) + ".";
                return false;
            }
            *file = FileUtils::pathToUtf8(project->getProjectPath() / FileUtils::pathFromUtf8(relative));
        } else if (key == "axis" && v.is_string()) {
            spec.axis = v.get<std::string>();
            if (spec.axis != "unity" && spec.axis != "unreal" && spec.axis != "houdini" && spec.axis != "zup") {
                error = "import.axis must be unity, unreal, houdini or zup (the export's target engine; got \"" + spec.axis + "\").";
                return false;
            }
        } else if (key == "axis_scale" && v.is_number() && v.get<double>() > 0.0) {
            spec.axisScale = v.get<float>();
        } else if (key == "upsample" && v.is_number_integer() && v.get<long long>() >= 0 && v.get<long long>() <= 1024) {
            spec.upsample = v.get<int>();
        } else if (key == "clip" && v.is_string()) {
            spec.clipName = v.get<std::string>();
        } else if (key == "colors" && v.is_boolean()) {
            spec.colors = v.get<bool>();
        } else {
            error = "import." + key + " is not an importer option or has a wrong value (" + v.dump() + "): axis (unity|unreal|houdini|zup), "
                    "axis_scale (> 0), upsample (0 = auto, 1..1024), clip (name), colors (bool), or a file override (mesh, position, "
                    "position2, rotation, color, lookup, vat, normals_texture).";
            return false;
        }
    }
    return true;
}

// The arguments as a request; false with a fix-it error
bool requestFromArgs(Project* project, const Json& args, VatRequest& request, std::string& error) {
    // rebake: the recipe, nothing else about what to bake
    if (args.contains("rebake")) {
        if (!args["rebake"].is_string()) {
            error = "rebake must be the project-relative path of a .vat (e.g. \"assets/vat/hero.vat\").";
            return false;
        }
        for (const char* key : {"source", "on", "clips", "mode", "fps", "normals", "precision", "preset", "sockets", "tolerance_mm", "output",
                                "overwrite", "place", "import"}) {
            if (args.contains(key)) {
                error = std::string("rebake re-runs the .vat's recipe and writes over it (GUID and hand edits kept): leave out ") + key +
                        ". To change settings, bake again from the source with output: the .vat and overwrite: true.";
                return false;
            }
        }
        if (!projectRelativeFile(project, args["rebake"].get<std::string>(), true, request.rebakePath, error) ||
            !Util::isVertexAnimationFile(request.rebakePath)) {
            error = "rebake: " + (error.empty() ? "'" + args["rebake"].get<std::string>() + "' is not a .vat file" : error) +
                    " (list them with list_assets {type: \"vertex_animation\"}).";
            return false;
        }
        VatData data;
        if (!readVatAsset(project, request.rebakePath, data, &error)) {
            error = "rebake: " + request.rebakePath + " can't be read: " + error;
            return false;
        }
        request.rebake = true;
        request.write = true;
        request.rebakeImport = isImportRecipe(data.recipe);
        request.import = request.rebakeImport;
        if (request.rebakeImport && !importSpecFromRecipe(data.recipe, FileUtils::pathToUtf8(project->getProjectPath()), request.importSpec, error)) {
            error = "rebake: " + error;
            return false;
        }
        if (request.rebakeImport) {
            request.detection = detectVatImport(request.importSpec.path);
            request.importWrite.output = request.rebakePath;
            request.importWrite.overwrite = true;
            request.importWrite.keepGuidOf = true;
        }
        return true;
    }

    if (!args.contains("source")) {
        error = "source is required: a GameObject (id or path), a model (\"assets/models/hero.gltf\", \"...hero.gltf#Walk\"), .anim files "
                "with on, a .poseanim, or a Houdini VAT 3 / OpenVAT export path; or pass rebake: a .vat.";
        return false;
    }
    const Json& sourceArg = args["source"];
    std::vector<std::string> files;
    SceneProject* sourceScene = nullptr;
    Entity sourceEntity = NULL_ENTITY;
    if (sourceArg.is_number_integer() || (sourceArg.is_string() && !looksLikeFile(project, sourceArg.get<std::string>()))) {
        std::string why;
        if (!gameObjectArg(project, args, sourceArg, sourceScene, sourceEntity, why)) {
            error = "source: " + sourceArg.dump() + " is neither a project file nor a GameObject (" + why +
                    "). Give a file path from the project root (\"assets/models/hero.gltf\") or a GameObject id/path (scene: its scene).";
            return false;
        }
    } else if (!stringList(sourceArg, "source", files, error)) {
        return false;
    }

    // `on`: a file or a GameObject
    std::string onFile;
    SceneProject* onScene = nullptr;
    Entity onEntity = NULL_ENTITY;
    if (args.contains("on")) {
        const Json& on = args["on"];
        if (on.is_string() && looksLikeFile(project, on.get<std::string>())) {
            onFile = on.get<std::string>();
        } else if (on.is_string() || on.is_number_integer()) {
            std::string why;
            if (!gameObjectArg(project, args, on, onScene, onEntity, why)) {
                error = "on: " + on.dump() + " is neither a model/prefab file nor a GameObject (" + why + ").";
                return false;
            }
        } else {
            error = "on must be a model or prefab path, or a GameObject id/path.";
            return false;
        }
    }

    VatBakeSpec& spec = request.spec;
    if (sourceEntity != NULL_ENTITY) {
        if (args.contains("on")) {
            error = "on is for .anim / .poseanim sources; a GameObject source plays its own Animation Player, Animations or Pose Animator.";
            return false;
        }
        spec.source.kind = VatSourceKind::Entity;
        spec.source.sceneId = sourceScene->id;
        spec.source.path = McpRefs::entityPath(sourceScene, sourceEntity);
        request.sceneName = sourceScene->name;
    } else {
        VatSourceRoute route;
        VatIssue issue;
        if (!routeVatSource(project, files, onFile, false, route, issue)) {
            error = "source: " + issue.message + ". Fix: " + issue.fix + ".";
            return false;
        }
        if (route.import) {
            request.import = true;
            request.detection = route.detection;
            request.importSpec.path = route.importPath;
            for (const char* key : {"on", "clips", "sockets"}) {
                if (args.contains(key)) {
                    error = std::string(key) + " is for baked sources; an import (" + importKindName(route.detection.kind) +
                            ") takes the export's clips as they are: leave it out.";
                    return false;
                }
            }
        } else {
            if (args.contains("import")) {
                error = "import options are for Houdini VAT 3 / OpenVAT exports; this source is baked: leave import out.";
                return false;
            }
            spec.source = route.source;
            spec.clips = route.clips;
            if (onEntity != NULL_ENTITY) {
                spec.source.sceneId = onScene->id;
                spec.source.path = McpRefs::entityPath(onScene, onEntity);
                request.sceneName = onScene->name;
            } else if (route.needsTarget) {
                error = "on is required: the model, prefab or GameObject the " + std::string(route.source.kind == VatSourceKind::Pose ? ".poseanim" : ".anim") +
                        " plays on (e.g. on: \"assets/models/hero.gltf\").";
                return false;
            }
        }
    }

    // settings
    std::string name;
    if (args.contains("clips")) {
        std::vector<std::string> clips;
        if (!stringList(args["clips"], "clips", clips, error)) return false;
        for (const std::string& clip : clips) {
            if (std::find(spec.clips.begin(), spec.clips.end(), clip) == spec.clips.end()) spec.clips.push_back(clip);
        }
    }
    name = args.value("mode", std::string("auto"));
    if (!modeChoiceFromName(name, spec.mode)) {
        error = "mode must be auto, vertices or bones (got \"" + name + "\").";
        return false;
    }
    name = args.value("precision", std::string("auto"));
    if (!precisionChoiceFromName(name, spec.precision)) {
        error = "precision must be auto, half or float (got \"" + name + "\").";
        return false;
    }
    name = args.value("preset", std::string("mobile_web"));
    if (!presetFromName(name, spec.preset)) {
        error = "preset must be mobile_web (2048-wide payload, WebGL2/GLES3) or desktop (got \"" + name + "\").";
        return false;
    }
    double fps = 0.0, tolerance = 1.0;
    if (!numberArg(args, "fps", 0.0, 240.0, true, fps, error)) return false;
    if (fps > 0.0) spec.fps = static_cast<float>(fps);
    if (!numberArg(args, "tolerance_mm", 0.0, 1000.0, true, tolerance, error)) return false;
    spec.toleranceMm = static_cast<float>(tolerance);
    if (!boolArg(args, "normals", spec.normals, error) || !boolArg(args, "overwrite", spec.overwrite, error)) return false;
    if (args.contains("sockets") && !stringList(args["sockets"], "sockets", spec.sockets, error)) return false;
    if (!placeArg(args, spec.place, spec.crowd, error)) return false;
    if (args.contains("output")) {
        const Json& output = args["output"];
        if (output.is_boolean()) {
            request.write = output.get<bool>();
            request.defaultOutput = request.write;
        } else if (output.is_string()) {
            std::string relative;
            if (!projectRelativeFile(project, output.get<std::string>(), false, relative, error)) {
                error = "output: " + error + ".";
                return false;
            }
            const fs::path absolute = project->getProjectPath() / FileUtils::pathFromUtf8(relative);
            if (!Util::isVertexAnimationFile(relative) || !project->isInsideAssetsPath(absolute)) {
                error = "output must be a .vat path inside the assets folder (e.g. \"assets/vat/hero.vat\"; the mesh goes to hero.vat.glb), or "
                        "true for the default name next to the source (got \"" + relative + "\").";
                return false;
            }
            spec.output = relative;
            request.write = true;
        } else {
            error = "output must be a .vat path (e.g. \"assets/vat/hero.vat\") or true (the default name next to the source).";
            return false;
        }
    }
    if (!request.write && args.contains("overwrite") && spec.overwrite) {
        error = "overwrite needs output (a preview writes nothing).";
        return false;
    }

    // a file source placed in the scene given (GameObject sources are placed in their own scene)
    if (!request.import && spec.place != VatPlace::None && !isGameObjectSource(spec.source) && args.contains("scene")) {
        std::string why;
        SceneProject* scene = McpRefs::resolveScene(project, args["scene"], why);
        if (!scene || !scene->scene) {
            error = "place: no open scene to place the bake in (" + (scene ? McpRefs::notOpenError(project, *scene) : why) + ").";
            return false;
        }
        request.placeScene = scene->id;
        request.sceneName = scene->name;
    }

    if (request.import) {
        VatImportSpec& importSpec = request.importSpec;
        importSpec.projectRoot = FileUtils::pathToUtf8(project->getProjectPath());
        if (!importOptionsArg(project, args, importSpec, error)) return false;
        importSpec.fps = fps > 0.0 ? static_cast<float>(fps) : 0.0f;
        importSpec.mode = modeChoiceName(spec.mode);
        importSpec.precision = precisionChoiceName(spec.precision);
        importSpec.preset = presetName(spec.preset);
        importSpec.normals = spec.normals;
        importSpec.toleranceMm = spec.toleranceMm;
        request.importWrite.output = spec.output;
        request.importWrite.overwrite = spec.overwrite;
        request.importWrite.place = spec.place;
        request.importWrite.crowd = spec.crowd;
        if (spec.place == VatPlace::Replace) {
            error = "place replace needs a GameObject source; an import has none: use beside or {crowd: {...}}.";
            return false;
        }
        if (spec.place != VatPlace::None) {
            std::string why;
            SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), why);
            if (!scene || !scene->scene) {
                error = "place: no open scene to place the import in (" + why + ").";
                return false;
            }
            request.importWrite.sceneId = scene->id;
            request.sceneName = scene->name;
        }
    }
    return true;
}

// ---------------------------------------------------------------- the job

struct ImportWork {
    VatImportSpec spec;
    VatImportResult result;
    std::atomic<bool> done{false};
};

struct VatCall {
    McpContext* ctx = nullptr;
    CallPtr call;
    JobPtr job;
    VatRequest request;
    std::unique_ptr<VatBakeSession> session;
    std::shared_ptr<ImportWork> work;
    fs::path root;
    uint64_t generation = 0;
    bool started = false;
    bool finished = false;
    std::string text;
    std::vector<ToolImage> images;
};
using VatCallPtr = std::shared_ptr<VatCall>;

void scheduleTick(const VatCallPtr& state);

std::string pngBase64(const PoseImage& image) {
    std::vector<uint8_t> png;
    if (!image.encodePng(png)) return std::string();
    return Base64::encode(png.data(), png.size());
}

std::string clipsText(const std::vector<VatClip>& clips) {
    std::string text;
    for (size_t i = 0; i < clips.size() && i < 8; i++) {
        text += (i ? ", " : "") + clips[i].name + " (" + std::to_string(clips[i].frames) + " frames)";
    }
    if (clips.size() > 8) text += ", +" + std::to_string(clips.size() - 8) + " more";
    return text;
}

// "Vertices (5,120 vertices) 2048x37 rgba16f, 1.8 MB, error max 0.21 mm"
std::string reportLine(const VatReport& report) {
    std::string text = std::string(report.mode == VatMode::Bones ? "Bones" : "Vertices") + " mode";
    if (!report.modeReason.empty()) text += " (" + report.modeReason + ")";
    text += ": " + std::to_string(report.vertices) + " vertices";
    if (report.mode == VatMode::Bones) text += ", " + std::to_string(report.bones) + " bones";
    text += ", texture " + std::to_string(report.width) + "x" + std::to_string(report.height) + " " + textureFormatName(report.precision) + ", " +
            fixed(static_cast<double>(report.bytes) / (1024.0 * 1024.0), 2) + " MB, error max " + fixed(report.maxErrorMm, 2) + " mm (mean " +
            fixed(report.meanErrorMm, 2) + "), " + std::to_string(report.fetchesPerVertex) + " fetches per vertex.";
    return text;
}

std::string warningsText(const std::vector<VatIssue>& warnings) {
    std::string text;
    for (const VatIssue& w : warnings) text += "\nWarning " + w.kind + ": " + w.message + (w.fix.empty() ? std::string() : ". Fix: " + w.fix + ".");
    return text;
}

// The contact sheet of a finished bake: the source posed again (a target and poser of its own)
// next to the VAT decoded from the result
PoseImage bakeSheet(Project* project, const VatBakeSpec& spec, const VatBakeResult& result, std::string& note) {
    std::vector<VatSheetRow> rows;
    std::vector<std::vector<float>> times;
    const VatData& data = result.preview->getData();
    const size_t shown = std::min(data.clips.size(), static_cast<size_t>(VAT_SHEET_MAX_CLIPS));
    if (data.clips.size() > shown) note = "the sheet shows the first " + std::to_string(shown) + " of " + std::to_string(data.clips.size()) + " clips";

    VatBakeTarget target;
    std::vector<VatIssue> issues;
    std::unique_ptr<VatPoser> poser;
    std::string error;
    if (resolveVatSource(project, spec.source, target, issues)) {
        poser = std::make_unique<VatPoser>();
        if (!target.scratch()) PoseEditSession::suspendActiveFor(target.sceneId, {target.root});
        std::vector<VatIssue> beginIssues;
        if (!poser->begin(target, {}, beginIssues, error)) poser.reset();
    }
    if (!poser) note += std::string(note.empty() ? "" : "; ") + "the source could not be posed again: VAT rows only";
    for (size_t c = 0; c < shown; c++) {
        const VatClip& clip = data.clips[c];
        const std::vector<float> clipTimes = vatSheetTimes(clip.length(), clip.wrap);
        times.push_back(clipTimes);
        if (poser) {
            const int sourceIndex = c < result.sourceClips.size() ? target.findClip(result.sourceClips[c]) : -1;
            if (sourceIndex >= 0) {
                VatSheetRow row;
                row.label = clip.name + " src";
                row.frameGroup = static_cast<int>(c);
                for (float t : clipTimes) {
                    VatSheetMesh mesh;
                    if (!sourceMeshAt(*poser, target.clips[static_cast<size_t>(sourceIndex)], t, mesh, &error)) break;
                    row.meshes.push_back(std::move(mesh));
                }
                if (row.meshes.size() == clipTimes.size()) rows.push_back(std::move(row));
            }
        }
        VatSheetRow vatRow;
        vatRow.label = clip.name + " VAT";
        vatRow.frameGroup = static_cast<int>(c);
        for (float t : clipTimes) {
            VatSheetMesh mesh;
            vatMeshAt(data, result.payload(), result.mesh, static_cast<int>(c), t, mesh, &error);
            vatRow.meshes.push_back(std::move(mesh));
        }
        rows.push_back(std::move(vatRow));
    }
    if (poser) poser->restore();
    const int cell = shown > 3 ? 128 : 160;
    return composeVatSheet(rows, times, poser ? "source (src) vs VAT at 4 times per clip" : "VAT at 4 times per clip", cell);
}

PoseImage importSheet(const VatImportResult& result, std::string& note) {
    std::vector<VatSheetRow> rows;
    std::vector<std::vector<float>> times;
    const VatData& data = result.data;
    const size_t shown = std::min(data.clips.size(), static_cast<size_t>(VAT_SHEET_MAX_CLIPS));
    if (data.clips.size() > shown) note = "the sheet shows the first " + std::to_string(shown) + " of " + std::to_string(data.clips.size()) + " clips";
    for (size_t c = 0; c < shown; c++) {
        const VatClip& clip = data.clips[c];
        times.push_back(vatSheetTimes(clip.length(), clip.wrap));
        VatSheetRow row;
        row.label = clip.name + " VAT";
        row.frameGroup = static_cast<int>(c);
        for (float t : times.back()) {
            VatSheetMesh mesh;
            vatMeshAt(data, result.payload, result.mesh, static_cast<int>(c), t, mesh);
            row.meshes.push_back(std::move(mesh));
        }
        rows.push_back(std::move(row));
    }
    return composeVatSheet(rows, times, "imported VAT at 4 times per clip (an export has no source to compare)", shown > 3 ? 128 : 160);
}

// The written part of the result and its text
void writtenJson(Project* project, const VatWriteResult& out, Json& data, std::string& text) {
    data["written"] = true;
    data["vat"] = out.vat;
    data["mesh"] = out.mesh;
    data["files"] = out.files;
    data["guid"] = out.guid;
    data["rewritten"] = out.rewritten;
    data["undo_name"] = out.undoName;
    std::string scope = "undo with scope project";
    if (out.sceneId != 0) {
        SceneProject* scene = project->getScene(out.sceneId);
        data["undo"] = {{"scope", "scene"}, {"scene", out.sceneId}};
        scope = "undo with scope scene" + (scene ? ", scene \"" + scene->name + "\"" : std::string());
        if (scene && out.entity != NULL_ENTITY && scene->scene) data["entity"] = McpRefs::entityJson(scene, out.entity);
    } else {
        data["undo"] = {{"scope", "project"}};
    }
    text += " Wrote ";
    for (size_t i = 0; i < out.files.size(); i++) text += (i ? ", " : "") + out.files[i];
    text += std::string(out.rewritten ? " (rewrote the existing file: GUID and hand edits kept)" : "") + " as one undo step \"" + out.undoName +
            "\" (" + scope + ").";
    if (data.contains("entity")) {
        text += " Placed GameObject " + data["entity"].value("path", std::string()) + " (id " + std::to_string(data["entity"].value("id", 0)) +
                ") with Model " + out.mesh + " and a Vertex Animator.";
    } else {
        text += " Use it: add_component vertexanimator {animation: \"" + out.vat + "\"} on a GameObject whose Model is " + out.mesh + ".";
    }
}

void finishBake(const VatCallPtr& state) {
    JobPtr job = state->job;
    Project* project = state->ctx->project();
    VatBakeSession& session = *state->session;
    VatRequest& request = state->request;
    Json data = Json::object();
    if (session.state() == VatBakeState::Cancelled || job->cancelRequested()) {
        job->markCancelled("Bake cancelled; nothing was written and the scene is at rest.");
        return;
    }
    if (session.state() != VatBakeState::Done) {
        data["errors"] = errorsJson(session.failureIssues());
        job->fail("The bake failed, nothing was written: " + session.error() + issuesLines(session.failureIssues(), true), data);
        return;
    }
    const VatBakeResult& result = *session.result();
    data = reportJson(result.report);
    Json clips = Json::array();
    for (const VatClip& clip : result.data.clips) clips.push_back(clip.name);
    std::string text = std::string(request.rebake ? "Rebaked " + request.rebakePath : "Baked") + " " + std::to_string(result.data.clips.size()) +
                       " clip(s): " + clipsText(result.data.clips) + ". " + reportLine(result.report);
    if (!request.write) {
        std::string planned, why;
        bool rewrite = false;
        if (session.plannedOutput(planned, rewrite, why)) {
            data["default_output"] = planned;
            data["default_output_rewrites"] = rewrite;
        }
        data["written"] = false;
        std::string note;
        const PoseImage sheet = bakeSheet(project, session.spec(), result, note);
        if (!sheet.isEmpty()) state->images.push_back({pngBase64(sheet), "image/png"});
        if (!note.empty()) data["sheet_note"] = note;
        text += " Preview only, nothing written: call again with the same arguments plus output (a .vat path, or true for " +
                (planned.empty() ? std::string("the default name") : planned + (rewrite ? ", rewriting the bake of this source there" : "")) +
                ") to write exactly this bake.";
        text += warningsText(result.report.warnings);
        text += "\nImage: the contact sheet (source rows vs VAT rows at 4 times per clip" + (note.empty() ? std::string() : "; " + note) + ").";
        state->text = text;
        job->succeed(text, data);
        return;
    }
    // another project opened meanwhile: nothing goes into it
    if (state->generation != texsynth::projectGeneration() || !project || project->getProjectPath() != state->root) {
        job->fail("The project changed while the bake ran: nothing was written. Call bake_vertex_animation again.", data);
        return;
    }
    // a file source goes into the scene given: the session places into the selected scene
    if (request.placeScene != 0 && project->getScene(request.placeScene)) project->setSelectedSceneId(request.placeScene);
    VatWriteResult out;
    std::string error;
    if (!session.write(out, error)) {
        data["errors"] = Json::array({Json{{"kind", "write_failed"}, {"message", error}, {"fix", "pass overwrite: true, or choose another output"}}});
        job->fail("The bake finished but nothing was written: " + error + ".", data);
        return;
    }
    writtenJson(project, out, data, text);
    text += warningsText(result.report.warnings);
    state->text = text;
    job->succeed(text, data);
}

void finishImport(const VatCallPtr& state) {
    JobPtr job = state->job;
    Project* project = state->ctx->project();
    VatRequest& request = state->request;
    if (job->cancelRequested()) {
        job->markCancelled("Import cancelled; nothing was written.");
        return;
    }
    VatImportResult& result = state->work->result;
    Json data = Json::parse(vatImportReportJson(result.report), nullptr, false);
    if (!data.is_object()) data = Json::object();
    if (!result.ok()) {
        Json list = Json::array();
        for (const VatIssue& issue : result.errors) list.push_back(issueJson(issue));
        data["errors"] = list;
        std::vector<VatIssue> all = result.errors;
        for (VatIssue& issue : all) issue.error = true;
        job->fail("The import failed, nothing was written:" + issuesLines(all, true), data);
        return;
    }
    std::string text = std::string(request.rebake ? "Re-imported " + request.rebakePath + " from" : "Imported") + " " +
                       importKindName(result.report.kind) + (result.report.houdiniMode != HoudiniMode::Unknown
                                                                 ? std::string(" (") + houdiniModeName(result.report.houdiniMode) + ")" : std::string()) +
                       " " + relativeOf(project, FileUtils::pathFromUtf8(request.importSpec.path)) + ": " + clipsText(result.data.clips) + ". " +
                       reportLine(result.report);
    if (!request.write) {
        std::string planned, why;
        bool rewrite = false;
        if (planImportOutput(project, result, request.detection, request.importWrite, planned, rewrite, why)) {
            data["default_output"] = planned;
            data["default_output_rewrites"] = rewrite;
        }
        data["written"] = false;
        std::string note;
        const PoseImage sheet = importSheet(result, note);
        if (!sheet.isEmpty()) state->images.push_back({pngBase64(sheet), "image/png"});
        if (!note.empty()) data["sheet_note"] = note;
        text += " Preview only, nothing written: call again with the same arguments plus output (a .vat path, or true for " +
                (planned.empty() ? std::string("the default name") : planned) + ") to write exactly this import.";
        text += warningsText(result.report.warnings);
        text += "\nImage: the VAT at 4 times per clip.";
        state->text = text;
        job->succeed(text, data);
        return;
    }
    if (state->generation != texsynth::projectGeneration() || !project || project->getProjectPath() != state->root) {
        job->fail("The project changed while the import ran: nothing was written. Call bake_vertex_animation again.", data);
        return;
    }
    VatWriteResult out;
    std::string error;
    VatImportWriteOptions options = request.importWrite;
    if (request.defaultOutput) options.output.clear();
    if (!writeVatImport(project, result, request.detection, options, out, error)) {
        data["errors"] = Json::array({Json{{"kind", "write_failed"}, {"message", error}, {"fix", "pass overwrite: true, or choose another output"}}});
        job->fail("The import finished but nothing was written: " + error + ".", data);
        return;
    }
    writtenJson(project, out, data, text);
    text += warningsText(result.report.warnings);
    state->text = text;
    job->succeed(text, data);
}

void tick(const VatCallPtr& state) {
    JobPtr job = state->job;
    if (state->finished || job->finished()) return;
    Project* project = state->ctx->project();
    if (state->generation != texsynth::projectGeneration() || !project || project->getProjectPath() != state->root) {
        state->finished = true;
        if (state->session) state->session->cancel();
        job->fail("The project changed while the bake ran: nothing was written.");
        return;
    }
    if (state->request.import) {
        if (!state->work->done.load()) {
            if (job->cancelRequested()) {
                // the importer can't stop mid-file: its result is dropped when it ends
                state->finished = true;
                job->markCancelled("Import cancelled; nothing was written.");
                return;
            }
            scheduleTick(state);
            return;
        }
        state->finished = true;
        finishImport(state);
        return;
    }
    VatBakeSession& session = *state->session;
    if (!state->started) {
        state->started = true;
        if (!session.start()) {
            state->finished = true;
            Json data = {{"errors", errorsJson(session.failureIssues())}};
            job->fail("Can't bake:" + issuesLines(session.failureIssues(), true), data);
            return;
        }
    }
    if (job->cancelRequested()) session.cancel();
    const bool done = session.advance(SLICE_MS);
    const VatBakeProgress progress = session.progress();
    job->setProgress(progress.fraction, progress.stage);
    if (!done) {
        scheduleTick(state);
        return;
    }
    state->finished = true;
    finishBake(state);
}

void scheduleTick(const VatCallPtr& state) {
    state->ctx->app().requestRedraw();
    getEditorHost().enqueueMainThreadTask([state]() {
        // whatever goes wrong, the job ends (the main-thread loop would swallow an exception)
        try {
            tick(state);
        } catch (const std::exception& e) {
            state->finished = true;
            if (state->session) state->session->cancel();
            if (state->job->status() == JobStatus::Running) state->job->fail(std::string("The bake failed: ") + e.what());
        } catch (...) {
            state->finished = true;
            if (state->session) state->session->cancel();
            if (state->job->status() == JobStatus::Running) state->job->fail("The bake failed.");
        }
    });
}

void bakeVertexAnimationTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    auto state = std::make_shared<VatCall>();
    state->ctx = &ctx;
    state->call = call;
    std::string error;
    if (!requestFromArgs(project, args, state->request, error)) {
        call->fail(error);
        return;
    }
    bool wait = true;
    double timeout = 120.0;
    if (!boolArg(args, "wait", wait, error) || !numberArg(args, "timeout_seconds", 1.0, 3600.0, false, timeout, error)) {
        call->fail(error);
        return;
    }
    VatRequest& request = state->request;
    state->root = project->getProjectPath();
    state->generation = texsynth::projectGeneration();

    std::string title;
    if (request.import) {
        // the export's own problems before any decoding
        std::vector<VatIssue> errors;
        for (const VatIssue& issue : request.detection.issues) {
            if (issue.error) errors.push_back(issue);
        }
        if (!errors.empty()) {
            call->fail("Can't import " + relativeOf(project, FileUtils::pathFromUtf8(request.importSpec.path)) + " (" + request.detection.summary +
                           "):" + issuesLines(errors, true),
                       Json{{"errors", errorsJson(errors)}, {"detection", Json::parse(vatImportDetectionJson(request.detection), nullptr, false)}});
            return;
        }
        if (request.write && !request.rebake && !request.importWrite.output.empty() && !request.importWrite.overwrite) {
            std::error_code ec;
            const fs::path absolute = state->root / FileUtils::pathFromUtf8(request.importWrite.output);
            if (fs::exists(absolute, ec) || fs::exists(FileUtils::pathFromUtf8(meshPathFor(FileUtils::pathToUtf8(absolute))), ec)) {
                call->fail("output_exists: '" + request.importWrite.output + "' exists. Fix: pass overwrite: true (one undo step puts it back), "
                           "or choose another output, or output: true for a free name next to the export.");
                return;
            }
        }
        state->work = std::make_shared<ImportWork>();
        state->work->spec = request.importSpec;
        title = "Import Vertex Animation " + relativeOf(project, FileUtils::pathFromUtf8(request.importSpec.path));
    } else {
        state->session = std::make_unique<VatBakeSession>(project);
        if (request.rebake) {
            if (!state->session->loadRebake(request.rebakePath, error)) {
                call->fail("Can't rebake " + request.rebakePath + ": " + error + ".");
                return;
            }
        } else {
            state->session->setSpec(request.spec);
        }
        const std::vector<VatIssue>& issues = state->session->check();
        if (hasErrors(issues)) {
            Json clips = Json::array();
            for (const VatSourceClip& clip : state->session->sourceClips()) clips.push_back(clip.name);
            Json data = {{"errors", errorsJson(issues)}};
            if (!clips.empty()) data["source_clips"] = clips;
            call->fail("Can't bake:" + issuesLines(issues, true), data);
            return;
        }
        title = std::string(request.rebake ? "Rebake " + request.rebakePath : "Bake Vertex Animation") + (request.write ? "" : " (preview)");
    }

    state->job = ctx.jobs().create("bake_vertex_animation", title);
    std::weak_ptr<VatCall> weak = state;
    state->job->setCancelHandler([weak]() {
        if (auto s = weak.lock()) {
            if (s->session && !s->finished) s->session->cancel();
        }
    });
    state->job->setProgress(0.0, "starting");
    if (state->work) {
        std::shared_ptr<ImportWork> work = state->work;
        std::thread([work]() {
            try {
                work->result = importVat(work->spec);
            } catch (const std::exception& e) {
                VatIssue issue;
                issue.kind = "read_failed";
                issue.error = true;
                issue.message = e.what();
                issue.fix = "check the export's files";
                work->result.errors.push_back(issue);
            }
            work->done = true;
        }).detach();
    }
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

// ---------------------------------------------------------------- .vat assets

std::string wrapName(ClipWrapMode wrap) {
    switch (wrap) {
        case ClipWrapMode::Once: return "once";
        case ClipWrapMode::PingPong: return "ping_pong";
        case ClipWrapMode::ClampForever: return "clamp_forever";
        default: return "loop";
    }
}

Json boundsJson(const ::jasperengine::vat::VatBounds& bounds) {
    return {{"min", {bounds.min.x, bounds.min.y, bounds.min.z}}, {"max", {bounds.max.x, bounds.max.y, bounds.max.z}}};
}

} // namespace

Json vatUsers(Project* project, const std::string& vatRelative) {
    Json users = Json::array();
    const std::string asset = ClipConverters::assetRelative(project, project->getProjectPath() / FileUtils::pathFromUtf8(vatRelative));
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        if (!sceneProject->scene) continue;
        for (Entity entity : sceneProject->entities) {
            const VertexAnimatorComponent* animator = sceneProject->scene->findComponent<VertexAnimatorComponent>(entity);
            if (!animator || animator->animation.empty()) continue;
            const std::string used = FileUtils::pathToGenericUtf8(FileUtils::pathFromUtf8(animator->animation).lexically_normal());
            if (used != asset && used != vatRelative) continue;
            users.push_back({{"scene", sceneProject->id}, {"entity", McpRefs::entityJson(sceneProject, entity)}});
            if (users.size() >= 50) return users;
        }
    }
    return users;
}

bool vatAssetInfo(Project* project, const fs::path& file, Json& data, std::string& text) {
    const std::string rel = relativeOf(project, file);
    std::string content;
    if (!clipui::readWholeFile(file, content)) {
        data["error"] = "can't read " + rel;
        return false;
    }
    ::jasperengine::vat::VatParseResult parsed = ::jasperengine::vat::parseVat(content);
    if (!parsed.ok()) {
        data["error"] = parsed.error;
        data["fix"] = "Rebake it (bake_vertex_animation {rebake: \"" + rel + "\"}) or bake it again from its source.";
        text += " Not a loadable Vertex Animation: " + parsed.error;
        return false;
    }
    const VatData& vat = *parsed.data;
    Json info = {{"guid", vat.guid}, {"mode", ::jasperengine::vat::modeName(vat.mode)}, {"mesh", vat.mesh}};
    info["layout"] = {{"precision", ::jasperengine::vat::precisionName(vat.layout.precision)}, {"width", vat.layout.width},
                      {"height", vat.layout.height}, {"samples", vat.layout.samples}, {"vertices", vat.layout.vertices},
                      {"bones", vat.layout.bones}, {"normals", vat.layout.normals}, {"colors", vat.layout.colors}};
    info["bounds"] = boundsJson(vat.bounds);
    Json clips = Json::array();
    for (const VatClip& clip : vat.clips) {
        Json events = Json::array();
        for (const ClipEventData& e : clip.events) events.push_back({{"time", e.time}, {"function", e.function}});
        clips.push_back({{"name", clip.name}, {"frames", clip.frames}, {"fps", clip.fps}, {"length", clip.length()}, {"wrap", wrapName(clip.wrap)},
                         {"speed", clip.speed}, {"interpolate", clip.interpolate}, {"events", events}});
    }
    info["clips"] = clips;
    Json sockets = Json::array();
    for (const auto& socket : vat.sockets) sockets.push_back({{"name", socket.name}, {"path", socket.path}});
    info["sockets"] = sockets;
    info["stats"] = {{"bytes", vat.stats.bytes}, {"max_error_mm", vat.stats.maxErrorMm}, {"mean_error_mm", vat.stats.meanErrorMm}};
    Json sources = Json::array();
    for (const auto& source : vat.recipe.sources) sources.push_back({{"kind", source.kind}, {"path", source.path}, {"clips", source.clips}});
    info["recipe"] = {{"sources", sources}, {"mode", vat.recipe.mode}, {"fps", vat.recipe.fps}, {"preset", vat.recipe.preset},
                      {"normals", vat.recipe.normals}, {"precision", vat.recipe.precision}};
    Json warnings = Json::array();
    for (const ClipWarning& w : parsed.warnings) warnings.push_back(w.path.empty() ? w.message : w.path + ": " + w.message);
    info["warnings"] = warnings;
    const std::vector<VatIssue> staleness = checkVatStaleness(project, rel);
    info["staleness"] = issuesJson(staleness);
    info["stale"] = !staleness.empty();
    if (!staleness.empty()) info["rebake"] = {{"rebake", rel}};
    const Json users = vatUsers(project, rel);
    info["users"] = users;
    data["vertex_animation"] = info;
    text += ", " + std::string(::jasperengine::vat::modeName(vat.mode)) + " Vertex Animation, " + std::to_string(vat.clips.size()) + " clip(s): " +
            clipsText(vat.clips) + ", texture " + std::to_string(vat.layout.width) + "x" + std::to_string(vat.layout.height) + " " +
            ::jasperengine::vat::precisionName(vat.layout.precision) + ", " + std::to_string(users.size()) + " user(s)";
    if (!staleness.empty()) {
        text += "; " + staleness.front().kind + ": " + staleness.front().message + " (bake_vertex_animation {rebake: \"" + rel + "\"})";
    } else {
        text += "; up to date";
    }
    return true;
}

bool checkVatText(Project* project, const fs::path& file, const std::string& content, std::vector<std::string>& warnings, std::string& error) {
    const std::string rel = relativeOf(project, file);
    ::jasperengine::vat::VatParseResult parsed = ::jasperengine::vat::parseVat(content);
    if (!parsed.ok()) {
        error = parsed.error;
        return false;
    }
    for (const ClipWarning& w : parsed.warnings) warnings.push_back(w.path.empty() ? w.message : w.path + ": " + w.message);
    ::jasperengine::vat::VatData next = *parsed.data;
    if (!VatDocument::normalize(next, &error)) return false;
    // an existing .vat: only the editable fields may change (VatDocument's rule, the Inspector's too)
    std::string oldText;
    if (!clipui::readWholeFile(file, oldText)) return true;     // a new file: its mesh decides when it loads
    ::jasperengine::vat::VatParseResult oldParsed = ::jasperengine::vat::parseVat(oldText);
    if (!oldParsed.ok()) return true;
    const std::string changed = VatDocument::generatedChange(*oldParsed.data, next);
    if (!changed.empty()) {
        error = "generated field " + changed + " changed: rebake (Inspector > Rebake, or bake_vertex_animation {rebake: \"" + rel +
                "\"}). Only clip name, wrap, speed, interpolate, events, socket names and removing whole clips are editable";
        return false;
    }
    return true;
}

void vatFileChanged(Project* project, const fs::path& path, Json& problems) {
    // the pool, the documents, parse errors and warnings: syncFiles (VatDocument::filesChanged,
    // addAnimationFileProblems). Here: a .vat that doesn't match its mesh does not load
    const std::string text = FileUtils::pathToUtf8(path);
    if (!Util::isVertexAnimationFile(text)) return;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    std::string content;
    if (!clipui::readWholeFile(path, content) || !::jasperengine::vat::parseVat(content).ok()) return;
    const std::string rel = relativeOf(project, path);
    for (const VatIssue& issue : checkVatStaleness(project, rel)) {
        if (!issue.error) continue;
        problems.push_back({{"path", rel}, {"message", issue.kind + ": " + issue.message + ". Fix: " + issue.fix + "."}});
    }
}

void registerVatTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "bake_vertex_animation";
    tool.title = "Bake Vertex Animation";
    tool.description =
        "Bake animation into a Vertex Animation (.vat + .vat.glb: crowds animated on the GPU, Vertex Animator component), or import a "
        "Houdini VAT 3 / OpenVAT export. Without output (or rebake) nothing is written: the report (mode, sizes, errors, warnings with "
        "fixes) and a contact sheet (source vs VAT, 4 times per clip). The same arguments + output write exactly that (one undo step). "
        "Guide: jasper://docs/vertex-animation.";
    auto loose = [](Json o) {
        o.erase("additionalProperties");
        return o;
    };
    Json source;
    source["description"] = "GameObject, model, model#Clip, .anim list (+on), .poseanim, or an export path.";
    source["type"] = Json::array({"string", "integer", "array"});
    Json on;
    on["description"] = "Model, prefab or GameObject the clips play on.";
    on["type"] = Json::array({"string", "integer"});
    Json output;
    output["description"] = ".vat path, or true: default name. Omitted: preview.";
    output["type"] = Json::array({"string", "boolean"});
    Json place;
    place["description"] = "On write.";
    place["anyOf"] = Json::array({stringEnum("", {"none", "beside", "replace"}),
                                  loose(object({{"crowd", loose(object({{"count", integer("")}, {"area", number("m")}, {"seed", integer("")}}))}}))});
    tool.inputSchema = object({
        {"source", source},
        {"scene", sceneRef()},
        {"on", on},
        {"clips", array(Json{{"type", "string"}}, "Default all.")},
        {"mode", withDefault(stringEnum("", {"auto", "vertices", "bones"}), "auto")},
        {"fps", number("Default 30.")},
        {"normals", withDefault(boolean(""), true)},
        {"precision", withDefault(stringEnum("", {"auto", "half", "float"}), "auto")},
        {"preset", withDefault(stringEnum("", {"mobile_web", "desktop"}), "mobile_web")},
        {"tolerance_mm", number("")},
        {"sockets", array(Json{{"type", "string"}}, "Node paths.")},
        {"output", output},
        {"overwrite", withDefault(boolean(""), false)},
        {"rebake", string("A .vat: run its recipe again.")},
        {"place", place},
        {"import", anyObject("Importer options (guide).")},
        {"wait", withDefault(boolean(""), true)},
        {"timeout_seconds", withDefault(number("Then a job_id (get_job).", 1, 3600), 120)}
    });
    tool.allowedWhilePlaying = false;
    tool.activity = ToolActivity::Edit;
    tool.handler = bakeVertexAnimationTool;
    registry.addTool(std::move(tool));
}

}
