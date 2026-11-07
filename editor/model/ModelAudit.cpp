// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelAudit.h"

#include "Project.h"
#include "util/FbxImporter.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // where a model's LOD levels start to pay off: below this a mesh is cheap enough
    constexpr long long kLodWorthTriangles = 5000;
    // a download above this with Lossless or Off is worth a lossy level
    constexpr uint64_t kLargeDownload = 2ull << 20;

}

std::string ModelAudit::formatBytes(uint64_t bytes){
    char text[32];
    if (bytes >= (1ull << 20)) std::snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) std::snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
    else std::snprintf(text, sizeof(text), "%llu B", (unsigned long long)bytes);
    return text;
}

std::string ModelAudit::snakeOf(const std::string& metaKey){
    std::string out;
    for (char c : metaKey){
        if (std::isupper((unsigned char)c)){
            out += '_';
            out += (char)std::tolower((unsigned char)c);
        }else{
            out += c;
        }
    }
    return out;
}

std::string ModelAudit::metaKeyOf(const std::string& snakeKey){
    std::string out;
    bool upper = false;
    for (char c : snakeKey){
        if (c == '_'){
            upper = true;
            continue;
        }
        out += upper ? (char)std::toupper((unsigned char)c) : c;
        upper = false;
    }
    return out;
}

long long ModelAuditRow::triangles() const{
    return hasState && state.status == ModelCookStatus::Cooked ? state.record.after.triangles : header.triangles;
}

long long ModelAuditRow::vertices() const{
    return hasState && state.status == ModelCookStatus::Cooked ? state.record.after.vertices : header.vertices;
}

uint64_t ModelAuditRow::memoryBytes() const{
    return hasState && state.status == ModelCookStatus::Cooked ? state.record.after.memoryBytes() : header.vertexBytes + header.indexBytes;
}

uint64_t ModelAuditRow::downloadBytes() const{
    return hasState && state.status == ModelCookStatus::Cooked ? state.record.diskBytes : header.fileBytes;
}

int ModelAuditRow::lodLevels() const{
    return hasState && state.status == ModelCookStatus::Cooked ? (int)state.record.lods.size() : 0;
}

std::string ModelAuditRow::status() const{
    if (!hasState) return "not cooked";
    if (state.status == ModelCookStatus::Cooked) return state.stale ? "cooking" : "cooked";
    if (state.status == ModelCookStatus::Queued || state.status == ModelCookStatus::Cooking) return "cooking";
    if (state.status == ModelCookStatus::Failed) return "failed";
    return "not cooked";
}

ModelAuditRow ModelAudit::row(Project& project, const std::string& assetPath, ModelFamily family){
    ModelAuditRow row;
    row.assetPath = assetPath;
    row.family = family;
    const fs::path absolute = (project.getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).lexically_normal();
    row.projectPath = project.normalizeToProjectRelative(absolute).generic_string();
    const fs::path read = FbxImporter::isFbxFile(absolute) ? project.getImportedModelPath(absolute) : absolute;
    row.headerRead = readModelHeader(read, row.header, row.headerError);
    std::string error;
    row.storedRead = modelSettingsOf(project, assetPath, row.stored, &error);
    if (!row.storedRead)
        row.headerError = "its import settings cannot be read: " + error;
    row.resolved = row.stored.resolve(family, project.getModelSettings());
    ModelCookQueue& queue = ModelCookQueue::get();
    row.hasState = queue.getProject() == &project && queue.getState(assetPath, family, row.state);
    ModelUsageIndex& usage = ModelUsageIndex::get();
    row.usage = usage.usageOf(assetPath);
    const bool baked = row.hasState && row.state.status == ModelCookStatus::Cooked && row.state.record.lodPrimitives > 0;
    row.readWrite = usage.decideReadWrite(assetPath, row.resolved.readWrite, project.getModelSettings().scriptMeshData, row.header.skinned, baked);
    if (row.hasState && (row.state.status == ModelCookStatus::Cooked || row.state.status == ModelCookStatus::Failed))
        row.diagnostics = row.state.record.diagnostics;
    // the last load's, unless the cook's record has the finding: their messages name the file by
    // different paths (asset-relative, absolute), so a finding is what it is about
    for (const GltfDiagnostic& diagnostic : ModelLoadDiagnostics::get(absolute.generic_string())){
        const bool known = std::any_of(row.diagnostics.begin(), row.diagnostics.end(), [&diagnostic](const GltfDiagnostic& d){
            return d.severity == diagnostic.severity && d.extension == diagnostic.extension && d.part == diagnostic.part &&
                   d.cookFixes == diagnostic.cookFixes && (!d.extension.empty() || !d.part.empty() || d.message == diagnostic.message);
        });
        if (!known)
            row.diagnostics.push_back(diagnostic);
    }
    row.issues = issues(row);
    return row;
}

std::vector<ModelIssue> ModelAudit::issues(const ModelAuditRow& row){
    std::vector<ModelIssue> out;
    auto add = [&](const char* code, const char* severity, std::string message){
        ModelIssue issue;
        issue.code = code;
        issue.severity = severity;
        issue.message = std::move(message);
        out.push_back(issue);
        return &out.back();
    };
    const std::string name = fs::path(row.assetPath).filename().generic_string();
    if (!row.headerRead && !row.headerError.empty()){
        ModelIssue* issue = add("model-unreadable", "error", name + ": " + row.headerError);
        issue->fix = ModelIssue::Fix::OpenImporter;
        issue->fixLabel = "Open the importer";
    }
    // the loader's findings (R5): an error fails export, a warning is shown, what the cook removes is told
    for (const GltfDiagnostic& diagnostic : row.diagnostics){
        if (diagnostic.cookFixes){
            add("gltf-cook-transcodes", "info", diagnostic.text());
        }else if (diagnostic.severity == GltfDiagnosticSeverity::Error){
            ModelIssue* issue = add(diagnostic.extension.empty() ? "gltf-load-error" : "gltf-required-extension", "error",
                                    diagnostic.text() + "; it does not cook, and exports fail, until it is fixed");
            issue->fix = ModelIssue::Fix::OpenImporter;
            issue->fixLabel = "Open the importer";
        }else if (diagnostic.severity == GltfDiagnosticSeverity::Warning){
            add("gltf-load-warning", "warning", diagnostic.text());
        }
    }
    // (a cook the loader's error stopped is that issue, once)
    const bool explained = row.hasState && std::any_of(row.diagnostics.begin(), row.diagnostics.end(), [&row](const GltfDiagnostic& d){
        return d.severity == GltfDiagnosticSeverity::Error && d.text() == row.state.error;
    });
    if (row.hasState && row.state.status == ModelCookStatus::Failed){
        if (!explained){
            ModelIssue* issue = add("mesh-cook-failed", "error", name + ": the cook failed: " + row.state.error);
            issue->fix = ModelIssue::Fix::Reimport;
            issue->fixLabel = "Reimport";
        }
    }else if (!row.hasState || row.state.status == ModelCookStatus::None){
        ModelIssue* issue = add("mesh-not-cooked", "info", name + " is not cooked for " + std::string(modelFamilyName(row.family)) + " yet");
        issue->fix = ModelIssue::Fix::Reimport;
        issue->fixLabel = "Reimport";
    }
    if (row.hasState && row.state.status == ModelCookStatus::Cooked && row.state.record.passThrough && !row.state.record.warnings.empty())
        add("mesh-pass-through", "warning", name + ": " + row.state.record.warnings.front());
    const long long triangles = row.triangles();
    if (triangles >= kLodWorthTriangles && row.resolved.generateLods == GenerateLods::Off){
        ModelIssue* issue = add("mesh-no-lod", "warning", name + " has " + std::to_string(triangles) + " triangles and Generate LODs is Off");
        issue->fix = ModelIssue::Fix::Settings;
        issue->fixLabel = "Generate LODs";
        issue->settings = {{"generate_lods", "automatic"}};
    }
    if (triangles >= kLodWorthTriangles && row.lodLevels() > 0){
        for (const ModelDocumentUse& use : row.usage.documents){
            if (use.scene && !use.lodEnabled && use.document.size() > 6 && use.document.compare(use.document.size() - 6, 6, ".scene") == 0){
                ModelIssue* issue = add("mesh-lod-disabled-in-scene", "warning",
                                        "Mesh LOD is off in " + use.document + ", so " + name + "'s " + std::to_string(row.lodLevels()) + " levels are never drawn");
                issue->fix = ModelIssue::Fix::SceneLod;
                issue->fixLabel = "Turn on LODs in this scene";
                issue->scene = use.document;
                break;
            }
        }
    }
    if (row.downloadBytes() >= kLargeDownload &&
            (row.resolved.meshCompression == MeshCompression::Off || row.resolved.meshCompression == MeshCompression::Lossless)){
        ModelIssue* issue = add("mesh-download-large", "info", name + " downloads " + formatBytes(row.downloadBytes()) +
                                ": Mesh Compression Medium stores it in fewer bits (dry run with inspect_models settings first)");
        issue->fix = ModelIssue::Fix::Settings;
        issue->fixLabel = "Mesh Compression Medium";
        issue->settings = {{"mesh_compression", "medium"}};
    }
    if (row.header.wideIndices && row.resolved.indexFormat == IndexFormat::Bits32){
        ModelIssue* issue = add("mesh-index-wider-than-needed", "info", name + " uses 32-bit indices where 16 bits reach every vertex");
        issue->fix = ModelIssue::Fix::Settings;
        issue->fixLabel = "Index Format Auto";
        issue->settings = {{"index_format", "auto"}};
    }
    if (row.hasState && row.state.status == ModelCookStatus::Cooked && row.state.record.duplicatePercent == 0.0 &&
            !row.resolved.weldVertices && row.resolved.optimizeMesh == OptimizeMesh::Everything){
        ModelIssue* issue = add("mesh-unwelded", "info", name + " keeps exact duplicate vertices (Weld Vertices is off)");
        issue->fix = ModelIssue::Fix::Settings;
        issue->fixLabel = "Weld Vertices";
        issue->settings = {{"weld_vertices", true}};
    }
    if (row.resolved.optimizeMesh == OptimizeMesh::Nothing && triangles >= kLodWorthTriangles){
        ModelIssue* issue = add("mesh-not-optimized", "info", name + " keeps its source's triangle and vertex order (Optimize Mesh is Nothing)");
        issue->fix = ModelIssue::Fix::Settings;
        issue->fixLabel = "Optimize Mesh Everything";
        issue->settings = {{"optimize_mesh", "everything"}};
    }
    return out;
}

Json ModelAudit::recordJson(const ModelCookRecord& r){
    Json lods = Json::array();
    for (const ModelLodLevelStats& level : r.lods)
        lods.push_back({{"level", level.level}, {"triangles", level.triangles}, {"share", level.share}, {"error", level.error},
                        {"deviation_percent", level.deviation * 100.0}});
    auto stats = [](const ModelGeometryStats& s){
        return Json{{"triangles", s.triangles}, {"vertices", s.vertices}, {"memory_bytes", s.memoryBytes()}, {"acmr", s.acmr}, {"overdraw", s.overdraw}};
    };
    return {{"source", stats(r.before)}, {"cooked", stats(r.after)}, {"source_bytes", r.sourceBytes}, {"download_bytes", r.diskBytes},
            {"duplicate_percent", r.duplicatePercent}, {"lods", lods}, {"visual_change", r.visualChange()},
            {"position_error", r.positionError}, {"model_size", r.modelSize}, {"required_extensions", r.requiredExtensions},
            {"kept_order", r.keptOrder}, {"warnings", r.warnings}, {"cook_ms", r.cookMs}, {"pass_through", r.passThrough},
            {"transcoded", r.transcoded}, {"authored_lod_primitives", r.authoredLodPrimitives}};
}

Json ModelAudit::resolvedJson(const ResolvedModelSettings& r){
    return {{"optimize_mesh", optimizeMeshName(r.optimizeMesh)}, {"mesh_compression", meshCompressionName(r.meshCompression)},
            {"weld_vertices", r.weldVertices}, {"index_format", indexFormatName(r.indexFormat)}, {"read_write", modelReadWriteName(r.readWrite)},
            {"normals", modelNormalsName(r.normals)}, {"smoothing_angle", r.smoothingAngle}, {"generate_lods", generateLodsName(r.generateLods)},
            {"lod_count", r.lodCount}, {"lod_reduction", lodReductionName(r.lodReduction)}, {"ship_source", r.shipSource}};
}

Json ModelAudit::storedJson(const ModelImportSettings& settings){
    const YAML::Node node = settings.serialize();
    std::function<Json(const YAML::Node&)> convert = [&](const YAML::Node& value) -> Json {
        if (value.IsMap()){
            Json out = Json::object();
            for (const auto& entry : value)
                out[snakeOf(entry.first.as<std::string>())] = convert(entry.second);
            return out;
        }
        if (value.IsSequence()){
            Json out = Json::array();
            for (const auto& item : value) out.push_back(convert(item));
            return out;
        }
        const std::string text = value.Scalar();
        if (text == "true" || text == "false") return text == "true";
        char* end = nullptr;
        const long integer = std::strtol(text.c_str(), &end, 10);
        if (end && *end == '\0' && !text.empty()) return integer;
        const double number = std::strtod(text.c_str(), &end);
        if (end && *end == '\0' && !text.empty()) return number;
        return text;
    };
    Json out = convert(node);
    out.erase("version");
    return out;
}

Json ModelAudit::toJson(const ModelAuditRow& row, bool details){
    Json issues = Json::array();
    for (const ModelIssue& issue : row.issues){
        Json entry = {{"code", issue.code}, {"severity", issue.severity}, {"message", issue.message}};
        if (issue.fix == ModelIssue::Fix::Settings)
            entry["fix"] = Json{{"tool", "set_model_import_settings"}, {"arguments", Json{{"paths", Json::array({row.projectPath})}, {"settings", issue.settings}}}};
        else if (issue.fix == ModelIssue::Fix::Reimport)
            entry["fix"] = Json{{"tool", "reimport_models"}, {"arguments", Json{{"paths", Json::array({row.projectPath})}}}};
        else if (issue.fix == ModelIssue::Fix::SceneLod)
            entry["fix"] = Json{{"tool", "set_scene_settings"}, {"arguments", Json{{"scene", issue.scene}, {"settings", Json{{"mesh_lod_enabled", true}}}}}};
        issues.push_back(entry);
    }
    Json out = {{"path", row.projectPath}, {"platform", modelFamilyName(row.family)}, {"status", row.status()},
                {"triangles", row.triangles()}, {"vertices", row.vertices()}, {"memory_bytes", row.memoryBytes()},
                {"download_bytes", row.downloadBytes()}, {"lods", row.lodLevels()}, {"issues", issues},
                {"required_extensions", row.header.requiredExtensions},
                {"uses", {{"objects", row.usage.objects()}, {"documents", row.usage.documents.size()}}}};
    if (row.hasState && row.state.status == ModelCookStatus::Cooked){
        out["duplicate_percent"] = row.state.record.duplicatePercent;
        out["acmr"] = row.state.record.after.acmr;
        out["overdraw"] = row.state.record.after.overdraw;
    }
    if (!details)
        return out;
    out["settings"] = {{"resolved", resolvedJson(row.resolved)}, {"stored", storedJson(row.stored)}};
    out["read_write"] = {{"keep", row.readWrite.keep}, {"reasons", row.readWrite.reasons}};
    Json diagnostics = Json::array();
    for (const GltfDiagnostic& diagnostic : row.diagnostics)
        diagnostics.push_back(modelDiagnosticJson(diagnostic));
    out["load_diagnostics"] = diagnostics;
    Json documents = Json::array();
    for (const ModelDocumentUse& use : row.usage.documents)
        documents.push_back({{"document", use.document}, {"objects", use.objects}, {"lod_enabled", use.lodEnabled}});
    out["uses"]["in"] = documents;
    if (row.hasState){
        Json cook = {{"status", modelCookStatusName(row.state.status)}, {"stale", row.state.stale}, {"cache_hit", row.state.cacheHit}};
        if (!row.state.error.empty()) cook["error"] = row.state.error;
        if (row.state.status == ModelCookStatus::Cooked){
            cook["key"] = row.state.record.key;
            cook["record"] = recordJson(row.state.record);
        }
        out["cook"] = cook;
    }
    return out;
}

Json ModelAudit::cookSummaryJson(Project& project, const std::string& assetPath){
    const fs::path absolute = (project.getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).lexically_normal();
    const fs::path read = FbxImporter::isFbxFile(absolute) ? project.getImportedModelPath(absolute) : absolute;
    ModelHeader header;
    std::string error;
    Json out = Json::object();
    if (readModelHeader(read, header, error)){
        out["required_extensions"] = header.requiredExtensions;
        out["source_bytes"] = header.fileBytes;
    }
    Json platforms = Json::object();
    ModelCookQueue& queue = ModelCookQueue::get();
    for (size_t f = 0; f < kModelFamilyCount; f++){
        ModelCookState state;
        if (queue.getProject() != &project || !queue.getState(assetPath, (ModelFamily)f, state))
            continue;
        Json entry = {{"status", state.stale ? "cooking" : modelCookStatusName(state.status)}};
        if (state.status == ModelCookStatus::Cooked){
            entry["download_bytes"] = state.record.diskBytes;
            entry["memory_bytes"] = state.record.after.memoryBytes();
            entry["lods"] = state.record.lods.size();
        }
        if (!state.error.empty())
            entry["error"] = state.error;
        platforms[modelFamilyName((ModelFamily)f)] = entry;
    }
    out["cooked"] = platforms;
    return out;
}
