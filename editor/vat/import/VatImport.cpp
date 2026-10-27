// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/import/VatImport.h"
#include "vat/import/VatImportInternal.h"

#include "util/FileUtils.h"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <new>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::vat;
using jasperengine::editor::FileUtils;
using Json = nlohmann::json;

namespace jasperengine::editor::vat{

const char* importKindName(VatImportKind kind){
    switch (kind){
        case VatImportKind::HoudiniVat3: return "houdini_vat3";
        case VatImportKind::OpenVat: return "openvat";
        default: return "";
    }
}

const char* houdiniModeName(HoudiniMode mode){
    switch (mode){
        case HoudiniMode::Soft: return "soft";
        case HoudiniMode::Rigid: return "rigid";
        case HoudiniMode::Fluid: return "fluid";
        case HoudiniMode::Sprite: return "sprite";
        default: return "";
    }
}

VatImportDetection detectVatImport(const std::string& path){
    VatImportDetection out;
    if (path.empty()){
        out.summary = "no path given";
        return out;
    }
    std::error_code ec;
    if (!fs::exists(FileUtils::pathFromUtf8(path), ec)){
        out.summary = path + " doesn't exist";
        return out;
    }
    importer::detectOpenVat(path, out);
    if (!out.found()) importer::detectHoudini(path, out);
    if (!out.found()) out.summary = "no Houdini VAT 3 or OpenVAT export found at " + path;
    return out;
}

namespace{

    // 32 lowercase hex digits (the .vat/.anim GUID form); a generator per call so worker threads
    // never share state
    std::string newGuid(){
        std::random_device device;
        std::seed_seq seed{device(), device(), device(), device(), device(), device(), device(), device()};
        std::mt19937_64 generator(seed);
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (int part = 0; part < 2; part++){
            const uint64_t value = generator();
            for (int shift = 60; shift >= 0; shift -= 4) out += hex[(value >> shift) & 0xF];
        }
        return out;
    }

    // project-relative with '/' when inside `root`, else the absolute path
    std::string recipePath(const std::string& path, const std::string& root){
        std::error_code ec;
        const fs::path absolute = fs::absolute(FileUtils::pathFromUtf8(path), ec).lexically_normal();
        if (!root.empty()){
            const fs::path base = fs::absolute(FileUtils::pathFromUtf8(root), ec).lexically_normal();
            const fs::path relative = absolute.lexically_relative(base);
            const std::string text = FileUtils::pathToGenericUtf8(relative);
            if (!relative.empty() && text.rfind("..", 0) != 0) return text;
        }
        return FileUtils::pathToGenericUtf8(absolute);
    }

    bool validateSpec(const VatImportSpec& spec, VatImportResult& result){
        importer::PresetLimits limits;
        if (!importer::presetLimits(spec.preset, limits)){
            importer::addError(result, "invalid_settings", "unknown preset \"" + spec.preset + "\"", "use mobile_web or desktop");
        }
        if (spec.precision != "auto" && spec.precision != "half" && spec.precision != "float"){
            importer::addError(result, "invalid_settings", "unknown precision \"" + spec.precision + "\"", "use auto, half or float");
        }
        if (spec.mode != "auto" && spec.mode != "vertices" && spec.mode != "bones"){
            importer::addError(result, "invalid_settings", "unknown mode \"" + spec.mode + "\"", "use auto, vertices or bones");
        }
        if (!spec.axis.empty() && spec.axis != "unity" && spec.axis != "unreal" && spec.axis != "houdini" && spec.axis != "zup"){
            importer::addError(result, "invalid_settings", "unknown axis \"" + spec.axis + "\"", "use unity, unreal, houdini or zup");
        }
        if (!(spec.fps >= 0.0f) || spec.fps > 1000.0f){
            importer::addError(result, "invalid_settings", "fps " + importer::num(spec.fps) + " is out of range", "use a frame rate in (0, 1000]");
        }
        if (!(spec.axisScale >= 0.0f)){
            importer::addError(result, "invalid_settings", "axis scale " + importer::num(spec.axisScale) + " is negative", "use a positive scale");
        }
        if (spec.upsample < 0 || spec.upsample > VAT_IMPORT_MAX_UPSAMPLE){
            importer::addError(result, "invalid_settings", "upsample " + std::to_string(spec.upsample) + " is out of range",
                               "use 0 (automatic) or a factor from 1 to 1024");
        }
        if (!(spec.toleranceMm > 0.0f)){
            importer::addError(result, "invalid_settings", "tolerance " + importer::num(spec.toleranceMm) + " mm is not positive", "use 1 mm");
        }
        return result.errors.empty();
    }

    // the options that differ from the defaults, as the recipe's "import" object
    std::string importOptions(const VatImportSpec& spec){
        Json options = Json::object();
        if (!spec.clipName.empty()) options["clip"] = spec.clipName;
        if (spec.fps > 0.0f) options["fps"] = spec.fps;
        if (!spec.axis.empty()) options["axis"] = spec.axis;
        if (spec.axisScale > 0.0f) options["axis_scale"] = spec.axisScale;
        if (spec.upsample > 0) options["upsample"] = spec.upsample;
        if (!spec.colors) options["colors"] = false;
        const std::pair<const char*, const std::string*> files[] = {
            {"mesh", &spec.mesh}, {"position", &spec.position}, {"position2", &spec.position2}, {"rotation", &spec.rotation},
            {"color", &spec.color}, {"lookup", &spec.lookup}, {"vat", &spec.vat}, {"normals_texture", &spec.normalsTexture}};
        for (const auto& file : files){
            if (!file.second->empty()) options[file.first] = recipePath(*file.second, spec.projectRoot);
        }
        return options.empty() ? std::string() : options.dump();
    }

}

VatImportResult importVat(const VatImportSpec& spec){
    VatImportResult result;
    if (!validateSpec(spec, result)) return result;
    const VatImportDetection detection = detectVatImport(spec.path);
    if (!detection.found()){
        importer::addError(result, "not_found", detection.summary,
                           "pass a Houdini VAT 3 export (its folder, mesh or .mat) or an OpenVAT export (its folder or -remap_info.json)");
        return result;
    }
    result.report.kind = detection.kind;
    // the readers allocate from sizes in the files: running out of memory (or any library
    // exception) is an error of this import, never a crash of the caller
    try{
        if (detection.kind == VatImportKind::OpenVat){
            importer::importOpenVat(spec, detection, result);
        }else{
            importer::importHoudini(spec, detection, result);
        }
    }catch (const std::bad_alloc&){
        result = VatImportResult();
        result.report.kind = detection.kind;
        importer::addError(result, "too_large", "not enough memory to read the export", "export fewer frames or a smaller mesh");
        return result;
    }catch (const std::exception& e){
        result = VatImportResult();
        result.report.kind = detection.kind;
        importer::addError(result, "read_failed", std::string("the export can't be read: ") + e.what(), "re-export it");
        return result;
    }
    if (!result.ok()) return result;

    // the recipe: every file read, hashed, so Rebake can tell when the export changed
    VatData& data = result.data;
    data.guid = newGuid();
    VatRecipeSource source;
    source.kind = importKindName(detection.kind);
    source.path = recipePath(spec.path, spec.projectRoot);
    for (const VatClip& clip : data.clips) source.clips.push_back(clip.name);
    std::vector<std::string> files = result.filesRead;
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    for (const std::string& file : files){
        const std::string hash = importer::fileSha1(file);
        if (!hash.empty()) source.hashes.push_back({recipePath(file, spec.projectRoot), hash});
    }
    std::sort(source.hashes.begin(), source.hashes.end());
    data.recipe = VatRecipe();
    data.recipe.sources.push_back(source);
    data.recipe.mode = spec.mode;
    data.recipe.fps = result.report.fps > 0.0f ? result.report.fps : (data.clips.empty() ? 30.0f : data.clips.front().fps);
    data.recipe.preset = spec.preset;
    data.recipe.normals = spec.normals;
    data.recipe.precision = spec.precision;
    data.recipe.toleranceMm = spec.toleranceMm;
    data.recipe.importOptions = importOptions(spec);
    return result;
}

bool importSpecFromRecipe(const VatRecipe& recipe, const std::string& projectRoot, VatImportSpec& spec, std::string& error){
    if (recipe.sources.empty() || (recipe.sources[0].kind != "houdini_vat3" && recipe.sources[0].kind != "openvat")){
        error = "the recipe is not an import's (houdini_vat3 or openvat)";
        return false;
    }
    spec = VatImportSpec();
    spec.projectRoot = projectRoot;
    auto resolvePath = [&](const std::string& path) -> std::string {
        const fs::path p = FileUtils::pathFromUtf8(path);
        if (p.is_absolute() || projectRoot.empty()) return path;
        return FileUtils::pathToUtf8(FileUtils::pathFromUtf8(projectRoot) / p);
    };
    spec.path = resolvePath(recipe.sources[0].path);
    spec.mode = recipe.mode;
    spec.preset = recipe.preset;
    spec.normals = recipe.normals;
    spec.precision = recipe.precision;
    spec.toleranceMm = recipe.toleranceMm;
    if (!recipe.importOptions.empty()){
        const Json options = Json::parse(recipe.importOptions, nullptr, false);
        if (!options.is_object()){
            error = "the recipe's import options are not a JSON object";
            return false;
        }
        auto text = [&](const char* key, std::string& out, bool path){
            auto found = options.find(key);
            if (found != options.end() && found->is_string()) out = path ? resolvePath(found->get<std::string>()) : found->get<std::string>();
        };
        text("clip", spec.clipName, false);
        text("axis", spec.axis, false);
        text("mesh", spec.mesh, true);
        text("position", spec.position, true);
        text("position2", spec.position2, true);
        text("rotation", spec.rotation, true);
        text("color", spec.color, true);
        text("lookup", spec.lookup, true);
        text("vat", spec.vat, true);
        text("normals_texture", spec.normalsTexture, true);
        auto fps = options.find("fps");
        if (fps != options.end() && fps->is_number()) spec.fps = fps->get<float>();
        auto scale = options.find("axis_scale");
        if (scale != options.end() && scale->is_number()) spec.axisScale = scale->get<float>();
        auto upsample = options.find("upsample");
        if (upsample != options.end() && upsample->is_number_integer()) spec.upsample = upsample->get<int>();
        auto colors = options.find("colors");
        if (colors != options.end() && colors->is_boolean()) spec.colors = colors->get<bool>();
    }
    return true;
}

bool encodeVatImport(VatImportResult& result, const std::string& vatPath, VatEncodedFiles& out, std::string* error){
    if (!result.ok()){
        if (error) *error = "the import failed: " + vatImportIssuesText(result.errors);
        return false;
    }
    return encodeVatFiles(vatPath, result.data, result.mesh, result.payload, result.sockets, out, error);
}

std::string vatImportReportJson(const VatImportReport& report){
    // the shared report, then what an import adds
    Json j = reportJson(report);
    j["kind"] = importKindName(report.kind);
    j["houdini_mode"] = houdiniModeName(report.houdiniMode);
    j["mesh_vertices"] = report.meshVertices;
    j["source_points"] = report.sourcePoints;
    j["pieces"] = report.pieces;
    j["source_frames"] = report.sourceFrames;
    j["frames"] = report.frames;
    j["upsample"] = report.upsample;
    j["fps"] = report.fps;
    j["target"] = report.target;
    j["interpolation_error_mm"] = report.interpolationErrorMm;
    return j.dump();
}

std::string vatImportDetectionJson(const VatImportDetection& detection){
    Json j = Json::object();
    j["kind"] = importKindName(detection.kind);
    j["mode"] = houdiniModeName(detection.mode);
    j["mode_source"] = detection.modeSource;
    j["asset"] = detection.assetName;
    j["folder"] = detection.folder;
    Json files = Json::object();
    const std::pair<const char*, const std::string*> list[] = {
        {"mesh", &detection.files.mesh}, {"position", &detection.files.position}, {"position2", &detection.files.position2},
        {"rotation", &detection.files.rotation}, {"color", &detection.files.color}, {"lookup", &detection.files.lookup},
        {"material", &detection.files.material}, {"data_json", &detection.files.dataJson}, {"vat", &detection.files.vat},
        {"normals", &detection.files.normals}, {"remap_info", &detection.files.remapInfo}};
    for (const auto& file : list){
        if (!file.second->empty()) files[file.first] = *file.second;
    }
    j["files"] = files;
    j["metadata"] = detection.metadataSource;
    j["frames"] = detection.frames;
    j["fps"] = detection.fps;
    j["target"] = detection.target;
    j["clips"] = detection.clips;
    Json issues = Json::array();
    for (const VatImportIssue& issue : detection.issues){
        issues.push_back({{"kind", issue.kind}, {"message", issue.message}, {"fix", issue.fix}, {"error", issue.error}});
    }
    j["issues"] = issues;
    j["summary"] = detection.summary;
    return j.dump();
}

std::string vatImportIssuesText(const std::vector<VatImportIssue>& issues){
    return issuesText(issues);
}

}
