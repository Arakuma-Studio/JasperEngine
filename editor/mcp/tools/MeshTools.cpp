// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The `meshes` tool group (spec S53 R22): models' cooks, import settings and render budgets.
//   inspect_models             the Audit's numbers and issues per model; with settings, a dry run
//   set_model_import_settings  one undo step, then a cook job
//   reimport_models            cooks again, returns inspect_models' records
//   check_render_budgets       a scene against quality.render_budgets (fails when over)

#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "EditorHost.h"
#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"
#include "component/InstancedMeshComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "model/ModelAudit.h"
#include "model/ModelSettingsEdit.h"
#include "util/FileUtils.h"
#include "window/ProjectWindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;
namespace fs = std::filesystem;

namespace {

const std::vector<std::string> kPlatforms = {"desktop", "mobile", "web"};

void meshesWhenIndexed(const CallPtr& call, std::function<void()> then) {
    Project* project = call->ctx().project();
    project->refreshAssetDatabase(true);
    if (project->getAssetIndex().isReady()) {
        then();
        return;
    }
    call->waitUntil([project] { return project->getAssetIndex().isReady(); }, 60000, [call, then](bool timedOut) {
        if (timedOut) {
            call->fail("The asset index is still being built; try again in a moment.");
            return;
        }
        then();
    });
}

bool platformArg(const CallPtr& call, const char* key, ModelFamily& out) {
    out = ModelFamily::Desktop;
    const Json& args = call->args();
    if (!args.contains(key) || args[key].is_null()) return true;
    if (!args[key].is_string() || !modelFamilyFromName(args[key].get<std::string>(), out)) {
        call->fail(std::string(key) + " must be desktop, mobile or web.");
        return false;
    }
    return true;
}

// a model source named by its project-relative path; its asset-relative form
bool modelArg(const CallPtr& call, const std::string& given, std::string& assetPath) {
    Project* project = call->ctx().project();
    fs::path file;
    std::string error;
    if (!McpRefs::resolveProjectPath(project, given, file, error, true)) {
        call->fail(error);
        return false;
    }
    file = file.lexically_normal();
    assetPath = project->normalizeToAssetsRelative(file).generic_string();
    if (!project->isInsideAssetsPath(file) || !isModelSource(assetPath)) {
        call->fail(w4::projectRelative(project, file) + " is not a model in the assets directory (gltf, glb, fbx).");
        return false;
    }
    return true;
}

bool modelPathsArg(const CallPtr& call, std::vector<std::string>& assetPaths) {
    const Json& args = call->args();
    if (!args.contains("paths") || !args["paths"].is_array()) return true;
    for (const Json& item : args["paths"]) {
        std::string assetPath;
        if (!item.is_string()) {
            call->fail("paths holds project-relative model paths.");
            return false;
        }
        if (!modelArg(call, item.get<std::string>(), assetPath)) return false;
        if (std::find(assetPaths.begin(), assetPaths.end(), assetPath) == assetPaths.end()) assetPaths.push_back(assetPath);
    }
    return true;
}

// the models an open scene's GameObjects load
std::vector<std::string> sceneModels(Project* project, SceneProject* sceneProject) {
    std::set<std::string> paths;
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    if (scene && scene->isComponentRegistered<ModelComponent>()) {
        auto models = scene->getComponentArray<ModelComponent>();
        for (size_t i = 0; i < models->size(); i++) {
            const ModelComponent& model = models->getComponentFromIndex(i);
            if (model.filename.empty()) continue;
            const fs::path absolute = project->resolveAssetPath(model.filename).lexically_normal();
            if (!project->isInsideAssetsPath(absolute)) continue;
            const std::string assetPath = project->normalizeToAssetsRelative(absolute).generic_string();
            if (isModelSource(assetPath)) paths.insert(assetPath);
        }
    }
    return {paths.begin(), paths.end()};
}

// set_model_import_settings' (and the dry run's) settings as a sidecar edit
bool settingsEditArg(const CallPtr& call, const Json& settings, const Json& reset, std::optional<ModelFamily> platform,
                     ModelSettingsEdit& edit) {
    edit = ModelSettingsEdit();
    edit.platform = platform;
    if (settings.is_object()) {
        for (const auto& [key, value] : settings.items()) {
            const std::string metaKey = ModelAudit::metaKeyOf(key);
            YAML::Node yaml;
            if (value.is_boolean()) yaml = YAML::Node(value.get<bool>());
            else if (value.is_number_integer()) yaml = YAML::Node(value.get<long long>());
            else if (value.is_number()) yaml = YAML::Node(value.get<double>());
            else if (value.is_string()) yaml = YAML::Node(value.get<std::string>());
            std::string error;
            if (!yaml || !validateModelSettingsField(metaKey, yaml, error)) {
                std::string valid;
                for (const std::string& name : modelSettingKeys()) valid += (valid.empty() ? "" : ", ") + ModelAudit::snakeOf(name);
                const size_t colon = error.find(':');
                call->fail("settings." + key + ": " + (std::find(modelSettingKeys().begin(), modelSettingKeys().end(), metaKey) == modelSettingKeys().end()
                                                           ? "not a model import setting (" + valid + ")."
                                                           : (colon == std::string::npos ? error : error.substr(colon + 2)) + "."));
                return false;
            }
            edit.set.push_back({metaKey, yaml});
        }
    }
    if (reset.is_array()) {
        for (const Json& item : reset) {
            const std::string metaKey = item.is_string() ? ModelAudit::metaKeyOf(item.get<std::string>()) : std::string();
            if (std::find(modelSettingKeys().begin(), modelSettingKeys().end(), metaKey) == modelSettingKeys().end()) {
                call->fail("reset: " + item.dump() + " is not a model import setting.");
                return false;
            }
            edit.reset.push_back(metaKey);
        }
    }
    return true;
}

// Polls the cook queue off the main thread until these cooks are terminal, then builds the
// result on the main thread: the job ends there
void finishWhenCooked(const JobPtr& job, const std::vector<std::string>& paths, const std::vector<ModelFamily>& families,
                      std::function<void(const JobPtr&)> finish) {
    std::thread([job, paths, families, finish]() {
        ModelCookQueue& queue = ModelCookQueue::get();
        while (!job->cancelRequested()) {
            bool pending = false;
            int done = 0;
            for (const std::string& path : paths)
                for (ModelFamily family : families) {
                    if (queue.isPending(path, family)) pending = true;
                    else done++;
                }
            job->setProgress(paths.empty() ? 1.0 : (double)done / (double)(paths.size() * families.size()), "cooking models");
            if (!pending) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        editor::getEditorHost().enqueueMainThreadTask([job, finish]() {
            if (job->cancelRequested()) {
                job->markCancelled("Cancelled: the cooks go on in the background.");
                return;
            }
            finish(job);
        });
    }).detach();
}

// inspect_models' records of `paths` for one platform, and the text's worst line
Json modelRecords(Project* project, const std::vector<std::string>& paths, ModelFamily family, int& failed) {
    ModelUsageIndex::get().refresh(*project);
    Json models = Json::array();
    failed = 0;
    for (const std::string& path : paths) {
        const ModelAuditRow row = ModelAudit::row(*project, path, family);
        failed += row.status() == "failed" ? 1 : 0;
        models.push_back(ModelAudit::toJson(row, true));
    }
    return models;
}

// ---------------------------------------------------------------- inspect_models

void inspectModelsTool(const CallPtr& call) {
    meshesWhenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const Json& args = call->args();
        ModelFamily family;
        if (!platformArg(call, "platform", family)) return;
        std::vector<std::string> paths;
        if (!modelPathsArg(call, paths)) return;
        std::string scope = "all models";
        if (paths.empty() && args.contains("scene") && !args["scene"].is_null()) {
            std::string error;
            SceneProject* scene = McpRefs::resolveScene(project, args["scene"], error);
            if (!scene) {
                call->fail(error);
                return;
            }
            paths = sceneModels(project, scene);
            scope = "models of " + scene->name;
        } else if (paths.empty()) {
            paths = projectModelSources(*project);
        } else {
            scope = "the given models";
        }
        const bool dryRun = args.contains("settings") && args["settings"].is_object() && !args["settings"].empty();
        if (dryRun) {
            if (!args.contains("paths") || paths.empty() || paths.size() > 16) {
                call->fail("A dry run (settings) needs paths: at most 16 models.");
                return;
            }
            ModelSettingsEdit edit;
            if (!settingsEditArg(call, args["settings"], Json(), std::nullopt, edit)) return;
            ModelUsageIndex::get().refresh(*project);
            // the requests with the settings as they would be stored: the cache fills, no sidecar is written
            auto items = std::make_shared<std::vector<ModelBatchItem>>();
            for (const std::string& path : paths) {
                const YAML::Node stored = SetAssetMetaCmd::read(project->getAssetsPath() / FileUtils::pathFromUtf8(path), ModelImportSettings::metaKey);
                YAML::Node section;
                std::string error;
                ModelImportSettings settings;
                if (!editModelSection(stored, edit, section, error) || !ModelImportSettings::parse(section, settings, &error)) {
                    call->fail(path + ": " + error);
                    return;
                }
                ModelBatchItem item;
                item.assetPath = path;
                item.family = family;
                if (!gatherModelCookRequest(*project, path, family, &settings, item.request, error)) {
                    call->fail(error);
                    return;
                }
                items->push_back(std::move(item));
            }
            JobPtr job = call->ctx().jobs().create("model-dry-run", "Dry run of " + std::to_string(paths.size()) + " model(s)");
            std::thread([job, items, project]() {
                ModelBatchCook::run(ModelCookQueue::get().getCache(), *items, 0, [job](int done, int total, const std::string&) {
                    job->setProgress((double)done / std::max(total, 1), "cooking");
                }, [job]() { return job->cancelRequested(); });
                Json models = Json::array();
                int failed = 0;
                std::string text;
                for (const ModelBatchItem& item : *items) {
                    const std::string projectPath = project->normalizeToProjectRelative(project->getAssetsPath() / FileUtils::pathFromUtf8(item.assetPath)).generic_string();
                    Json entry = {{"path", projectPath}, {"platform", modelFamilyName(item.family)},
                                  {"settings", ModelAudit::resolvedJson(item.request.settings)}};
                    if (item.result.ok) {
                        entry["predicted"] = ModelAudit::recordJson(item.result.record);
                        if (text.size() < 900) {
                            const ModelCookRecord& r = item.result.record;
                            text += "\n" + projectPath + ": " + std::to_string(r.after.triangles) + " tris, memory " + ModelAudit::formatBytes(r.before.memoryBytes()) +
                                    " -> " + ModelAudit::formatBytes(r.after.memoryBytes()) + ", download " + ModelAudit::formatBytes(r.sourceBytes) + " -> " +
                                    ModelAudit::formatBytes(r.diskBytes) + ", " + std::to_string(r.lods.size()) + " LODs, change: " + r.visualChange();
                        }
                    } else {
                        failed++;
                        entry["error"] = item.result.error;
                        text += "\n" + projectPath + ": " + item.result.error;
                    }
                    models.push_back(entry);
                }
                Json result = {{"dry_run", true}, {"written", false}, {"models", models},
                               {"next", {{"tool", "set_model_import_settings"}, {"note", "the same paths and settings apply it; the cook is cached"}}}};
                const std::string summary = "Dry run (nothing written; the numbers are the real cook's):" + text;
                if (failed) job->fail(summary, result);
                else job->succeed(summary, result);
            }).detach();
            replyWhenJobDone(call, job, args.value("wait", true), 300.0, [](const McpJob& finished) {
                ToolResult result = finished.status() == JobStatus::Succeeded ? ToolResult::success(finished.message(), finished.result())
                                                                              : ToolResult::error(finished.message(), finished.result());
                return result;
            });
            return;
        }

        ModelUsageIndex::get().refresh(*project);
        std::vector<ModelAuditRow> rows;
        for (const std::string& path : paths) rows.push_back(ModelAudit::row(*project, path, family));
        const std::string sortBy = args.value("sort_by", std::string("triangles"));
        auto severity = [](const ModelAuditRow& row) {
            int score = 0;
            for (const ModelIssue& issue : row.issues) score += issue.severity == "error" ? 100 : issue.severity == "warning" ? 10 : 1;
            return score;
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const ModelAuditRow& a, const ModelAuditRow& b) {
            if (sortBy == "memory") return a.memoryBytes() > b.memoryBytes();
            if (sortBy == "download") return a.downloadBytes() > b.downloadBytes();
            if (sortBy == "issues") return severity(a) > severity(b);
            return a.triangles() > b.triangles();
        });
        const size_t limit = (size_t)std::clamp<long long>(args.value("limit", 20LL), 1, 500);
        Json models = Json::array();
        long long triangles = 0;
        uint64_t memory = 0, download = 0;
        int errors = 0, warnings = 0, cooked = 0;
        for (const ModelAuditRow& row : rows) {
            triangles += row.triangles();
            memory += row.memoryBytes();
            download += row.downloadBytes();
            cooked += row.status() == "cooked" ? 1 : 0;
            for (const ModelIssue& issue : row.issues) {
                errors += issue.severity == "error" ? 1 : 0;
                warnings += issue.severity == "warning" ? 1 : 0;
            }
        }
        std::string text = std::to_string(rows.size()) + " model(s), " + scope + ", " + modelFamilyName(family) + ": " +
                           std::to_string(triangles) + " tris, memory " + ModelAudit::formatBytes(memory) + ", download " +
                           ModelAudit::formatBytes(download) + ", " + std::to_string(cooked) + " cooked, " + std::to_string(errors) +
                           " error(s), " + std::to_string(warnings) + " warning(s).";
        int lines = 0;
        for (size_t i = 0; i < rows.size() && i < limit; i++) {
            models.push_back(ModelAudit::toJson(rows[i], true));
            if (lines < 12) {
                const ModelAuditRow& row = rows[i];
                text += "\n" + row.projectPath + ": " + std::to_string(row.triangles()) + " tris, " + ModelAudit::formatBytes(row.memoryBytes()) +
                        ", " + ModelAudit::formatBytes(row.downloadBytes()) + " download, " + std::to_string(row.lodLevels()) + " LODs, " + row.status() +
                        (row.issues.empty() ? "" : "; " + row.issues.front().message);
                lines++;
            }
        }
        if (rows.size() > limit) text += "\nShowing " + std::to_string(limit) + " of " + std::to_string(rows.size()) + " (limit).";
        Json data = {{"platform", modelFamilyName(family)}, {"sort_by", sortBy}, {"total", rows.size()}, {"truncated", rows.size() > limit},
                     {"totals", {{"triangles", triangles}, {"memory_bytes", memory}, {"download_bytes", download}, {"cooked", cooked},
                                 {"errors", errors}, {"warnings", warnings}}},
                     {"models", models}};
        if (!rows.empty() && !rows[0].issues.empty())
            data["next"] = {{"note", "each issue's fix holds the call that applies it; dry run a settings change with inspect_models {paths, settings}"}};
        call->ok(text, data);
    });
}

// ---------------------------------------------------------------- set_model_import_settings

void setModelImportSettingsTool(const CallPtr& call) {
    meshesWhenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const Json& args = call->args();
        std::vector<std::string> paths;
        if (!modelPathsArg(call, paths)) return;
        if (paths.empty()) {
            call->fail("paths is a non-empty list of project-relative model paths.");
            return;
        }
        std::optional<ModelFamily> platform;
        if (args.contains("platform") && !args["platform"].is_null()) {
            ModelFamily family;
            if (!platformArg(call, "platform", family)) return;
            platform = family;
        }
        const Json settings = args.value("settings", Json::object());
        const Json reset = args.value("reset", Json::array());
        if (settings.empty() && reset.empty()) {
            call->fail("Give settings (optimize_mesh, mesh_compression, generate_lods...) or reset (their names).");
            return;
        }
        ModelSettingsEdit edit;
        if (!settingsEditArg(call, settings, reset, platform, edit)) return;
        // every model's new section, checked whole before anything is written
        std::vector<YAML::Node> sections;
        for (const std::string& path : paths) {
            const YAML::Node stored = SetAssetMetaCmd::read(project->getAssetsPath() / FileUtils::pathFromUtf8(path), ModelImportSettings::metaKey);
            YAML::Node section;
            std::string error;
            if (!editModelSection(stored, edit, section, error)) {
                call->fail(path + ": " + error);
                return;
            }
            sections.push_back(section);
        }
        std::vector<std::string> changed;
        std::string error;
        if (!writeModelSections(*project, paths, sections, "Set Model Import Settings", changed, error)) {
            call->fail("Nothing was written: " + error + ".");
            return;
        }
        if (ProjectWindow* window = call->ctx().projectWindow()) window->refreshCurrentDirectory();
        // the sidecar writes requeued their cooks (the queue's write listener); these go first
        std::vector<ModelFamily> families = {ModelFamily::Desktop};
        if (platform && *platform != ModelFamily::Desktop) families.push_back(*platform);
        ModelCookQueue& queue = ModelCookQueue::get();
        for (const std::string& path : paths)
            for (ModelFamily family : families) queue.request(path, family, ModelCookQueue::Selected);
        Json updated = Json::array();
        for (const std::string& path : changed)
            updated.push_back(project->normalizeToProjectRelative(project->getAssetsPath() / FileUtils::pathFromUtf8(path)).generic_string());
        JobPtr job = call->ctx().jobs().create("model-cook", "Cook " + std::to_string(paths.size()) + " model(s)");
        const ModelFamily shown = platform.value_or(ModelFamily::Desktop);
        finishWhenCooked(job, paths, families, [project, paths, shown, updated](const JobPtr& job) {
            int failed = 0;
            const Json models = modelRecords(project, paths, shown, failed);
            const Json result = {{"updated", updated}, {"unchanged", paths.size() - updated.size()}, {"models", models}};
            const std::string text = "Updated " + std::to_string(updated.size()) + " model(s) (" + std::to_string(paths.size() - updated.size()) +
                                     " unchanged); cooked, " + std::to_string(failed) + " failed. Undo with scope project.";
            if (failed) job->fail(text, result);
            else job->succeed(text, result);
        });
        replyWhenJobDone(call, job, args.value("wait", true), 300.0, [](const McpJob& finished) {
            Json data = finished.result();
            data["job_id"] = finished.id();
            return finished.status() == JobStatus::Succeeded ? ToolResult::success(finished.message(), data)
                                                             : ToolResult::error(finished.message(), data);
        });
    });
}

// ---------------------------------------------------------------- reimport_models

void reimportModelsTool(const CallPtr& call) {
    meshesWhenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const Json& args = call->args();
        ModelCookQueue& queue = ModelCookQueue::get();
        if (queue.getProject() != project) {
            call->fail("The model cook queue is not running for this project yet; try again in a moment.");
            return;
        }
        std::vector<std::string> paths;
        if (!modelPathsArg(call, paths)) return;
        if (paths.empty()) paths = projectModelSources(*project);
        std::vector<ModelFamily> families;
        if (args.contains("platforms") && args["platforms"].is_array()) {
            for (const Json& item : args["platforms"]) {
                ModelFamily family;
                if (!item.is_string() || !modelFamilyFromName(item.get<std::string>(), family)) {
                    call->fail("platforms: " + item.dump() + " is not desktop, mobile or web.");
                    return;
                }
                if (std::find(families.begin(), families.end(), family) == families.end()) families.push_back(family);
            }
        }
        if (families.empty()) families = {ModelFamily::Desktop};
        for (const std::string& path : paths) {
            queue.invalidate(path);
            for (ModelFamily family : families) queue.request(path, family, ModelCookQueue::Selected);
        }
        JobPtr job = call->ctx().jobs().create("model-cook", "Reimport " + std::to_string(paths.size()) + " model(s)");
        const ModelFamily shown = families.front();
        finishWhenCooked(job, paths, families, [project, paths, shown](const JobPtr& job) {
            int failed = 0;
            const Json models = modelRecords(project, paths, shown, failed);
            const std::string text = "Reimported " + std::to_string(paths.size()) + " model(s)" +
                                     (failed ? ", " + std::to_string(failed) + " failed (each record names its error)." : ".");
            if (failed) job->fail(text, {{"models", models}});
            else job->succeed(text, {{"models", models}});
        });
        replyWhenJobDone(call, job, args.value("wait", true), 300.0, [](const McpJob& finished) {
            Json data = finished.result();
            data["job_id"] = finished.id();
            return finished.status() == JobStatus::Succeeded ? ToolResult::success(finished.message(), data)
                                                             : ToolResult::error(finished.message(), data);
        });
    });
}

// ---------------------------------------------------------------- check_render_budgets

struct SceneGeometry {
    struct Model {
        long long triangles = 0;
        long long drawCalls = 0;
        int objects = 0;
    };
    long long triangles = 0;
    long long drawCalls = 0;
    uint64_t otherMeshBytes = 0;
    std::map<std::string, Model> models;    // asset-relative
};

// The scene's geometry at LOD 0 with every object drawn: an upper bound of its main view
SceneGeometry measureScene(Project* project, SceneProject* sceneProject) {
    SceneGeometry out;
    Scene* scene = sceneProject->scene;
    if (!scene || !scene->isComponentRegistered<MeshComponent>()) return out;
    // the model each mesh entity belongs to
    std::map<Entity, std::string> owner;
    if (scene->isComponentRegistered<ModelComponent>()) {
        auto models = scene->getComponentArray<ModelComponent>();
        for (size_t i = 0; i < models->size(); i++) {
            const ModelComponent& model = models->getComponentFromIndex(i);
            if (model.filename.empty()) continue;
            const fs::path absolute = project->resolveAssetPath(model.filename).lexically_normal();
            if (!project->isInsideAssetsPath(absolute)) continue;
            const std::string path = project->normalizeToAssetsRelative(absolute).generic_string();
            out.models[path].objects++;
            owner[models->getEntity(i)] = path;
            for (const auto& node : model.meshNodesMapping) owner[node.second] = path;
        }
    }
    auto meshes = scene->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++) {
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        const Entity entity = meshes->getEntity(i);
        long long copies = 1;
        if (const InstancedMeshComponent* instanced = scene->findComponent<InstancedMeshComponent>(entity))
            copies = std::max<long long>(1, (long long)instanced->instances.size());
        long long triangles = 0, draws = 0;
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++) {
            const Submesh& submesh = mesh.submeshes[s];
            if (submesh.primitiveType == PrimitiveType::TRIANGLES) triangles += (long long)submesh.vertexCount / 3;
            draws++;
        }
        out.triangles += triangles * copies;
        out.drawCalls += draws;
        auto it = owner.find(entity);
        if (it != owner.end()) {
            SceneGeometry::Model& model = out.models[it->second];
            model.triangles += triangles * copies;
            model.drawCalls += draws;
        } else {
            out.otherMeshBytes += mesh.buffer.getSize() + mesh.indices.getSize();
        }
    }
    return out;
}

void checkRenderBudgetsTool(const CallPtr& call) {
    meshesWhenIndexed(call, [call] {
        Project* project = call->ctx().project();
        const Json& args = call->args();
        ModelFamily family;
        if (!platformArg(call, "platform", family)) return;
        std::string error;
        SceneProject* scene = McpRefs::resolveScene(project, args.value("scene", Json()), error);
        if (!scene) {
            call->fail(error);
            return;
        }
        const RenderBudget budget = project->getModelSettings().renderBudgets[(size_t)family];
        const std::string platformName = modelFamilyName(family);
        if (budget.empty()) {
            call->fail("No render budget for " + platformName + ": set one with set_project_settings (quality.render_budgets." + platformName +
                       ".triangles, .draw_calls, .mesh_memory_mb, .download_mb).",
                       Json{{"next", Json{{"tool", "set_project_settings"},
                                          {"arguments", Json{{"values", Json{{"quality.render_budgets." + platformName + ".triangles", 150000}}}}}}}});
            return;
        }
        auto geometry = std::make_shared<SceneGeometry>(measureScene(project, scene));
        std::vector<std::string> paths;
        for (const auto& [path, model] : geometry->models)
            if (isModelSource(path)) paths.push_back(path);
        // the platform's cooks give the memory and download numbers
        ModelCookQueue& queue = ModelCookQueue::get();
        for (const std::string& path : paths) queue.request(path, family, ModelCookQueue::Selected);
        JobPtr job = call->ctx().jobs().create("render-budget", "Check " + scene->name + " against the " + platformName + " budget");
        const std::string sceneName = scene->name;
        finishWhenCooked(job, paths, {family}, [project, geometry, paths, family, budget, sceneName, platformName](const JobPtr& job) {
            struct Offender {
                std::string path;
                long long triangles = 0, drawCalls = 0;
                uint64_t memory = 0, download = 0;
            };
            std::vector<Offender> offenders;
            uint64_t memory = geometry->otherMeshBytes, download = 0;
            for (const auto& [path, model] : geometry->models) {
                Offender offender;
                offender.path = project->normalizeToProjectRelative(project->getAssetsPath() / FileUtils::pathFromUtf8(path)).generic_string();
                offender.triangles = model.triangles;
                offender.drawCalls = model.drawCalls;
                ModelCookState state;
                if (ModelCookQueue::get().getState(path, family, state) && state.status == ModelCookStatus::Cooked) {
                    offender.memory = state.record.after.memoryBytes();
                    offender.download = state.record.diskBytes;
                } else {
                    ModelHeader header;
                    std::string error;
                    if (readModelHeader(project->getAssetsPath() / FileUtils::pathFromUtf8(path), header, error)) {
                        offender.memory = header.vertexBytes + header.indexBytes;
                        offender.download = header.fileBytes;
                    }
                }
                memory += offender.memory;
                download += offender.download;
                offenders.push_back(offender);
            }
            const double memoryMB = memory / 1048576.0, downloadMB = download / 1048576.0;
            Json violations = Json::array();
            std::string over;
            auto check = [&](const char* metric, double value, double limit, const char* code, const std::function<double(const Offender&)>& share) {
                if (limit <= 0.0 || value <= limit) return;
                violations.push_back({{"metric", metric}, {"value", value}, {"limit", limit}, {"over_fraction", value / limit - 1.0}, {"code", code}});
                char line[160];
                std::snprintf(line, sizeof(line), "%s %.6g > %.6g (+%.0f%%)", metric, value, limit, (value / limit - 1.0) * 100.0);
                over += (over.empty() ? "" : "; ") + std::string(line);
                std::stable_sort(offenders.begin(), offenders.end(), [&](const Offender& a, const Offender& b) { return share(a) > share(b); });
            };
            check("triangles", (double)geometry->triangles, (double)budget.triangles, "render-budget-triangles", [](const Offender& o) { return (double)o.triangles; });
            check("draw_calls", (double)geometry->drawCalls, (double)budget.drawCalls, "render-budget-draw-calls", [](const Offender& o) { return (double)o.drawCalls; });
            check("mesh_memory_mb", memoryMB, budget.meshMemoryMB, "render-budget-mesh-memory", [](const Offender& o) { return (double)o.memory; });
            check("download_mb", downloadMB, budget.downloadMB, "render-budget-download", [](const Offender& o) { return (double)o.download; });
            Json offendersJson = Json::array();
            std::string top;
            for (size_t i = 0; i < offenders.size() && i < 10; i++) {
                const Offender& o = offenders[i];
                const Json next = {{"tool", "inspect_models"}, {"arguments", Json{{"paths", Json::array({o.path})}, {"platform", platformName}}}};
                offendersJson.push_back(Json{{"model", o.path}, {"triangles", o.triangles}, {"draw_calls", o.drawCalls}, {"memory_bytes", o.memory},
                                             {"download_bytes", o.download}, {"next", next}});
                if (i < 3) top += (top.empty() ? "" : ", ") + o.path + " (" + std::to_string(o.triangles) + " tris, " + std::to_string(o.drawCalls) + " draws)";
            }
            Json budgetJson = {{"triangles", budget.triangles}, {"draw_calls", budget.drawCalls}, {"mesh_memory_mb", budget.meshMemoryMB},
                               {"download_mb", budget.downloadMB}};
            Json result = {{"ok", violations.empty()}, {"scene", sceneName}, {"platform", platformName},
                           {"measured", "the scene's geometry at LOD 0 with every object drawn: an upper bound of its main view"},
                           {"values", {{"triangles", geometry->triangles}, {"draw_calls", geometry->drawCalls}, {"mesh_memory_mb", memoryMB},
                                       {"download_mb", downloadMB}}},
                           {"budgets", budgetJson}, {"violations", violations}, {"offenders", offendersJson}};
            char values[200];
            std::snprintf(values, sizeof(values), "triangles %lld, draw_calls %lld, mesh memory %.2f MB, download %.2f MB",
                          geometry->triangles, geometry->drawCalls, memoryMB, downloadMB);
            if (violations.empty()) {
                job->succeed("OK " + sceneName + " on " + platformName + ": " + values + " (within the budget).", result);
            } else {
                result["next"] = {{"tool", "inspect_models"}, {"arguments", {{"scene", sceneName}, {"platform", platformName}, {"sort_by", "triangles"}}}};
                job->fail("FAIL " + sceneName + " on " + platformName + ": " + over + ". Top: " + top + ".", result);
            }
        });
        replyWhenJobDone(call, job, true, 300.0, [](const McpJob& finished) {
            return finished.status() == JobStatus::Succeeded ? ToolResult::success(finished.message(), finished.result())
                                                             : ToolResult::error(finished.message(), finished.result());
        });
    });
}

} // namespace

void editor::mcp::registerMeshTools(McpRegistry& registry) {
    using namespace schema;
    const Json platform = stringEnum("", kPlatforms);
    {
        ToolDef tool;
        tool.name = "inspect_models";
        tool.title = "Inspect Models";
        tool.description = "Models' cost for a platform from headers and cook records: triangles, vertices, memory, download, LODs, cook status, "
                           "issues with the call that fixes each. paths, scene, or all (default). With paths and settings: a dry run, nothing written.";
        tool.inputSchema = object({
            {"paths", array(string(""), "Project-relative models.")},
            {"scene", sceneRef("Its models.")},
            {"platform", withDefault(platform, "desktop")},
            {"sort_by", withDefault(stringEnum("", {"triangles", "memory", "download", "issues"}), "triangles")},
            {"limit", withDefault(integer("", 1, 500), 20)},
            {"settings", anyObject("As set_model_import_settings; needs paths.")},
            {"wait", withDefault(boolean(""), true)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = inspectModelsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_model_import_settings";
        tool.title = "Set Model Import Settings";
        tool.description = "Partial update of models' import settings (with platform: its override); one undo step, then a cook job (wait false "
                           "returns job_id). Dry run first: inspect_models with the same settings.";
        tool.inputSchema = object({
            {"paths", array(string(""), "Project-relative models.")},
            {"settings", object({
                {"optimize_mesh", stringEnum("", {"everything", "polygon_order", "vertex_order", "nothing"})},
                {"mesh_compression", stringEnum("", {"off", "lossless", "low", "medium", "high"})},
                {"weld_vertices", boolean("")},
                {"index_format", stringEnum("", {"auto", "u16", "u32"})},
                {"read_write", stringEnum("", {"automatic", "on"})},
                {"normals", stringEnum("", {"import", "calculate"})},
                {"smoothing_angle", number("", 0, 180)},
                {"generate_lods", stringEnum("", {"automatic", "on", "off"})},
                {"lod_count", integer("", 1, 3)},
                {"lod_reduction", stringEnum("", {"preserve_detail", "balanced", "smallest"})},
                {"ship_source", boolean("")}
            })},
            {"platform", platform},
            {"reset", array(string(""), "Settings back to their defaults.")},
            {"wait", withDefault(boolean(""), true)}
        }, {"paths"});
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = false;
        tool.handler = setModelImportSettingsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "reimport_models";
        tool.title = "Reimport Models";
        tool.description = "Cooks models again (paths, or all) for platforms (default desktop); returns inspect_models' records. wait false returns job_id.";
        tool.inputSchema = object({
            {"paths", array(string(""), "Project-relative models; omit for all.")},
            {"platforms", array(platform, "")},
            {"wait", withDefault(boolean(""), true)}
        });
        tool.allowedWhilePlaying = false;
        tool.handler = reimportModelsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "check_render_budgets";
        tool.title = "Check Render Budgets";
        tool.description = "A scene against quality.render_budgets of a platform (triangles, draw calls, mesh memory, download, from its "
                           "geometry and cooks). Over budget is an error listing the offenders (exit 1 from jasperengine-editor call).";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"platform", withDefault(platform, "desktop")}
        });
        tool.annotations.readOnly = true;
        tool.handler = checkRenderBudgetsTool;
        registry.addTool(std::move(tool));
    }
}
