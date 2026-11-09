// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelArtFix.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ProjectSettingsValuesCmd.h"
#include "command/type/ScenePropertyCmd.h"
#include "command/type/SetAssetMetaCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "mcp/McpRefs.h"
#include "mcp/McpUndo.h"
#include "mcp/McpValues.h"
#include "pixelart/ApplyPalette.h"
#include "pixelart/PaletteAssets.h"
#include "pixelart/PixelArtInput.h"
#include "pixelart/PixelArtRules.h"
#include "pixelart/RgbaImage.h"
#include "sprite/SpriteAtlasDocument.h"
#include "util/EntityPath.h"
#include "util/ParticleEffectEdit.h"
#include "util/ProjectFileWrites.h"
#include "particles/ParticleEffectCodec.h"

#include "component/MeshComponent.h"
#include "component/UIComponent.h"
#include "device/DeviceSettings.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

    bool isSceneField(const std::string& field){
        return field == "camera" || field.rfind("post_processing.", 0) == 0;
    }

    std::string readFile(const fs::path& path){
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    SceneProject* sceneByPath(Project& project, const std::string& path, std::string& error){
        for (SceneProject& scene : project.getScenes()){
            if (scenePathOf(project, scene) != path)
                continue;
            if (scene.scene)
                return &scene;
            // a closed scene of the build list opens to be edited (its changes need a save, as any edit)
            const uint32_t id = project.openSceneNow(project.getProjectPath() / fs::path(path), false, UnsavedChoice::Cancel, &error);
            return id == NULL_PROJECT_SCENE ? nullptr : project.getScene(id);
        }
        error = "no scene " + path;
        return nullptr;
    }

    // the command that hands the id of the GameObject it creates to the steps after it
    struct CreateCapture : public Command {
        CreateEntityCmd* inner;
        std::shared_ptr<Entity> out;
        CreateCapture(CreateEntityCmd* inner, std::shared_ptr<Entity> out): inner(inner), out(std::move(out)) {}
        ~CreateCapture() override { delete inner; }
        bool execute() override {
            if (!inner->execute()) return false;
            *out = inner->getEntity();
            return true;
        }
        void undo() override { inner->undo(); }
        std::string describe() const override { return inner->describe(); }
        const char* typeName() const override { return inner->typeName(); }
        size_t memoryBytes() const override { return sizeof(*this) + inner->memoryBytes(); }
        void collectKeepCommands(KeepContext& context) const override { inner->collectKeepCommands(context); }
    };

    // one scene's edits as one McpStepsCmd
    bool applySceneChanges(Project& project, const std::string& scenePath, const std::vector<FixChange>& changes, FixApplyReport& report,
                           std::string& error){
        SceneProject* sp = sceneByPath(project, scenePath, error);
        if (!sp){
            error = scenePath + ": " + error;
            return false;
        }
        const uint32_t sceneId = sp->id;
        Project* p = &project;
        auto* steps = new mcp::McpStepsCmd();
        size_t count = 0;
        // entity -> component values; components to add first
        std::map<std::string, Json> values;
        std::vector<std::pair<std::string, Json>> addCameras;
        std::vector<std::pair<std::string, Json>> addComponents;   // (entity, initial values)
        bool postChanged = false;
        PostProcessingSettings post = sp->scene->getPostProcessing();
        for (const FixChange& change : changes){
            const size_t hash = change.target.find('#');
            if (hash == std::string::npos){
                if (change.field == "camera"){
                    addCameras.push_back({change.target, change.to});
                }else{
                    const std::string name = change.field.substr(std::string("post_processing.").size());
                    if (name == "fxaa_enabled") post.fxaaEnabled = change.to.get<bool>();
                    else if (name == "msaa_samples") post.msaaSamples = change.to.get<int>();
                    else if (name == "blur_enabled") post.blurEnabled = change.to.get<bool>();
                    else if (name == "render_scale") post.renderScale = change.to.get<float>();
                    else if (name == "dynamic_resolution_enabled") post.dynamicResolutionEnabled = change.to.get<bool>();
                    postChanged = true;
                }
                continue;
            }
            const std::string entity = change.target.substr(hash + 1);
            if (change.field == "pixelperfectcamera"){
                addComponents.push_back({entity, change.to});
                continue;
            }
            if (change.field == "particlesystem.modules.pixelArt"){
                // S49's module through the particle effects' own merge patch (the asset when it has one)
                const Entity e = EntityPath::find(*sp, entity.size() > 0 && entity[0] == '/' ? entity.substr(1) : entity);
                if (e == NULL_ENTITY){
                    delete steps;
                    error = "no GameObject " + entity + " in " + scenePath;
                    return false;
                }
                nlohmann::ordered_json patch;
                patch["modules"]["pixelArt"] = nlohmann::ordered_json::parse(change.to.dump());
                steps->addStep([p, sceneId, e, patch](std::string& why) -> Command* {
                    why = "The Pixel Art module could not be set.";
                    Command* command = ParticleEffectEdit::build(p, sceneId, {e}, [&patch](particles::ParticleEffectDocument& document){
                        std::vector<particles::Diagnostic> diagnostics;
                        return particles::ParticleEffectCodec::applyMergePatch(document, patch, diagnostics);
                    });
                    return command ? command : new MultiPropertyCmd();
                });
                count++;
                report.changed.push_back(scenePath + "#" + entity);
                continue;
            }
            const size_t dot = change.field.find('.');
            const std::string component = change.field.substr(0, dot);
            const std::string property = change.field.substr(dot + 1);
            Json value = change.to;
            if (property == "submeshes[0].material.baseColorTexture" || (component == "ui" && property == "texture")){
                // the reference's own path and sampler, with the new filters
                const Entity e = EntityPath::find(*sp, entity.size() > 0 && entity[0] == '/' ? entity.substr(1) : entity);
                Json current = Json::object();
                if (e != NULL_ENTITY){
                    const Texture* texture = nullptr;
                    if (const MeshComponent* mesh = sp->scene->findComponent<MeshComponent>(e); mesh && component == "mesh" && mesh->numSubmeshes > 0)
                        texture = &mesh->submeshes[0].material.baseColorTexture;
                    if (const UIComponent* ui = sp->scene->findComponent<UIComponent>(e); ui && component == "ui")
                        texture = &ui->texture;
                    if (texture){
                        const Json json = mcp::McpValues::textureToJson(p, sp, *texture);
                        if (json.is_string()) current = {{"path", json}};
                        else if (json.is_object()) current = json;
                    }
                }
                for (auto it = change.to.begin(); it != change.to.end(); ++it)
                    current[it.key()] = it.value();
                value = current;
            }
            values[entity][component][property] = value;
        }

        for (const auto& [name, to] : addCameras){
            (void)name;
            auto created = std::make_shared<Entity>(NULL_ENTITY);
            const std::string cameraName = to.value("name", std::string("Main Camera"));
            const Json ppc = to.value("pixelperfectcamera", Json::object());
            steps->addStep([p, sceneId, cameraName, created](std::string& why) -> Command* {
                why = "The camera could not be created.";
                auto* cmd = new CreateEntityCmd(p, sceneId, cameraName, EntityCreationType::CAMERA);
                cmd->setQuiet(true);
                return new CreateCapture(cmd, created);
            });
            steps->addStep([p, sceneId, created](std::string& why) -> Command* {
                why = "The new camera could not be the main camera.";
                return new SetMainCameraCmd(p, sceneId, *created);
            });
            steps->addStep([p, sceneId, created](std::string& why) -> Command* {
                why = "Could not add a Pixel Perfect Camera.";
                return new AddComponentCmd(p, sceneId, *created, ComponentType::PixelPerfectCameraComponent);
            });
            if (!ppc.empty()){
                steps->addStep([p, sceneId, created, ppc](std::string& why) -> Command* {
                    std::vector<mcp::McpValues::Change> parsed;
                    if (!mcp::McpValues::parseChanges(Json{{"pixelperfectcamera", ppc}}, parsed, why)) return nullptr;
                    auto* multi = new MultiPropertyCmd();
                    if (!mcp::McpValues::appendChangeCommands(p, p->getScene(sceneId), {*created}, parsed, *multi, why)){
                        delete multi;
                        return nullptr;
                    }
                    return multi;
                });
            }
            count++;
            report.changed.push_back(scenePath + "#/" + cameraName);
        }
        for (const auto& [entityPath, initial] : addComponents){
            const Entity e = EntityPath::find(*sp, entityPath.size() > 0 && entityPath[0] == '/' ? entityPath.substr(1) : entityPath);
            if (e == NULL_ENTITY){
                delete steps;
                error = "no GameObject " + entityPath + " in " + scenePath;
                return false;
            }
            steps->addStep([p, sceneId, e](std::string& why) -> Command* {
                SceneProject* current = p->getScene(sceneId);
                if (mcp::McpRefs::hasComponent(current, e, ComponentType::PixelPerfectCameraComponent)) return new MultiPropertyCmd();
                why = "Could not add a Pixel Perfect Camera.";
                return new AddComponentCmd(p, sceneId, e, ComponentType::PixelPerfectCameraComponent);
            });
            if (!initial.empty())
                for (auto it = initial.begin(); it != initial.end(); ++it)
                    values[entityPath]["pixelperfectcamera"][it.key()] = it.value();
            count++;
        }
        for (const auto& [entityPath, entityValues] : values){
            const Entity e = EntityPath::find(*sp, entityPath.size() > 0 && entityPath[0] == '/' ? entityPath.substr(1) : entityPath);
            if (e == NULL_ENTITY){
                delete steps;
                error = "no GameObject " + entityPath + " in " + scenePath;
                return false;
            }
            const Json given = entityValues;
            steps->addStep([p, sceneId, e, given](std::string& why) -> Command* {
                std::vector<mcp::McpValues::Change> parsed;
                if (!mcp::McpValues::parseChanges(given, parsed, why)) return nullptr;
                auto* multi = new MultiPropertyCmd();
                if (!mcp::McpValues::appendChangeCommands(p, p->getScene(sceneId), {e}, parsed, *multi, why)){
                    delete multi;
                    return nullptr;
                }
                return multi;
            });
            count++;
            report.changed.push_back(scenePath + "#" + entityPath);
        }
        if (postChanged){
            steps->addStep([p, sceneId, post](std::string&) -> Command* {
                return new ScenePropertyCmd<PostProcessingSettings>(p, sceneId, "post_processing", post);
            });
            count++;
            report.changed.push_back(scenePath);
        }
        if (count == 0){
            delete steps;
            return true;
        }
        if (!mcp::runSceneCommand(sceneId, steps, error))
            return false;
        report.savedScenes.push_back(scenePath);
        return true;
    }

    bool applyAssetChange(Project& project, const FixChange& change, FixApplyReport& report, std::string& error){
        const fs::path absolute = project.getAssetsPath() / fs::path(change.target);
        const fs::path relative = project.normalizeToProjectRelative(absolute);
        if (change.field == "import.textureImporter"){
            const YAML::Node section = mcp::McpValues::jsonToYaml(change.to);
            TextureImportSettings check;
            if (!TextureImportSettings::parse(section, check, &error)){
                error = change.target + ": " + error;
                return false;
            }
            auto* cmd = new SetAssetMetaCmd(&project, relative, TextureImportSettings::metaKey, check.serialize());
            cmd->setAction("Set Pixel Art Import Settings");
            if (!mcp::runProjectCommand(&project, cmd, error, "the import settings of " + change.target + " could not be written"))
                return false;
            report.recooking.push_back(change.target);
            report.changed.push_back(change.target);
            return true;
        }
        if (change.field == "import.asepriteImporter.texture.compression"){
            YAML::Node section = SetAssetMetaCmd::read(absolute, "import.asepriteImporter");
            YAML::Node node = section && section.IsMap() ? YAML::Clone(section) : YAML::Node(YAML::NodeType::Map);
            node["texture"]["compression"] = change.to.get<std::string>();
            auto* cmd = new SetAssetMetaCmd(&project, relative, "import.asepriteImporter", node);
            if (!mcp::runProjectCommand(&project, cmd, error, "the Aseprite import settings of " + change.target + " could not be written"))
                return false;
            report.changed.push_back(change.target);
            return true;
        }
        if (change.field == "settings.compression"){
            sprite::SpriteAtlasParseResult parsed = sprite::parseSpriteAtlas(readFile(absolute));
            if (!parsed.ok()){
                error = change.target + ": " + parsed.error.message;
                return false;
            }
            parsed.document->settings.compression = change.to.get<std::string>();
            if (!ProjectFileWrites::writeUndoable(&project, {{absolute, sprite::writeSpriteAtlas(*parsed.document)}}, error))
                return false;
            report.changed.push_back(change.target);
            return true;
        }
        if (change.field == "pixels"){
            const std::string to = change.to.get<std::string>();
            const std::string palettePath = to.rfind("palette ", 0) == 0 ? to.substr(8) : to;
            const ResolvedPalette resolved = resolvePaletteName(project, palettePath);
            if (!resolved.ok){
                error = palettePath + ": " + resolved.error.message;
                return false;
            }
            const std::optional<RgbaImage> image = loadRgbaImage(absolute, &error);
            if (!image)
                return false;
            ApplyRequest request;
            request.palette = resolved.palette.colors;
            const ApplyResult applied = applyPalette(*image, request);
            if (!ProjectFileWrites::writeUndoable(&project, {{absolute, encodeRgbaPng(applied.image)}}, error))
                return false;
            report.changed.push_back(change.target);
            report.recooking.push_back(change.target);
            return true;
        }
        error = change.target + ": cannot apply a change of " + change.field;
        return false;
    }

    // undoes what the histories did since the group began (a fix that failed part way)
    void rollBack(Project& project, mcp::McpUndo::Group& group){
        for (SceneProject* scene : project.getScenesAndPrefabStages()){
            auto it = group.sceneMarks.find(scene->id);
            const size_t mark = it != group.sceneMarks.end() ? it->second : 0;
            CommandHistory* history = CommandHandle::get(scene->id);
            while (history->position() > mark && history->canUndo() && history->undo()) {}
        }
        CommandHistory* projectHistory = project.getProjectCommandHistory();
        while (projectHistory->position() > group.projectMark && projectHistory->canUndo() && projectHistory->undo()) {}
        group.trimHolds.clear();
    }

}

std::string jasperengine::editor::pixelart::fixStepName(const FixPlan& plan){
    return "Fix Pixel Art (" + std::to_string(plan.fixes.size()) + ")";
}

std::vector<std::string> jasperengine::editor::pixelart::staleChanges(Project& project, const Scope& scope, const std::vector<FixChange>& changes){
    Scope fresh = scope;
    fresh.assumeModeOn = true;
    std::vector<PixelProblem> errors;
    const PixelArtCheckInput input = inputFromProject(project, fresh, errors);
    const CheckResult result = run(input);
    std::map<std::pair<std::string, std::string>, std::vector<Json>> current;
    for (const Finding& finding : result.findings)
        for (const FixOption& option : finding.options)
            for (const FixChange& change : option.changes)
                current[{change.target, change.field}].push_back(change.from);
    const PixelArtProjectInfo& p = input.project;
    std::map<std::string, Json> settings = {
        {"pixel_art.enabled", p.modeEnabled}, {"graphics.canvas_width", p.canvasWidth}, {"graphics.canvas_height", p.canvasHeight},
        {"graphics.scaling_mode", rules::scalingName(p.scaling)}, {"graphics.pixels_per_unit", p.pixelsPerUnit},
        {"graphics.default_filter_mode", textureFilterModeName(p.defaultFilterMode)},
        {"player.window_width", p.windowWidth}, {"player.window_height", p.windowHeight}};
    // a Device Tier's Render Scale cell (render_scale's fix compares only that cell)
    const DeviceSettings& devices = project.getDeviceSettings();
    for (int g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++){
        for (int t = 0; t < DEVICE_TIER_COUNT; t++){
            const std::optional<float>& cell = devices.tier((DevicePlatformGroup)g, (DeviceTier)t).get(DeviceOverride::RenderScale);
            settings[std::string("quality.device_tiers.") + DeviceNames::group((DevicePlatformGroup)g) + "." + DeviceNames::tier((DeviceTier)t)] =
                Json{{"render_scale", cell ? number(*cell) : Json(nullptr)}};
        }
    }
    std::vector<std::string> stale;
    for (const FixChange& change : changes){
        if (change.target == "project"){
            auto it = settings.find(change.field);
            if (it != settings.end() && it->second != change.from)
                stale.push_back(change.target + " " + change.field);
            continue;
        }
        auto it = current.find({change.target, change.field});
        if (it == current.end() || std::find(it->second.begin(), it->second.end(), change.from) == it->second.end())
            stale.push_back(change.target + " " + change.field);
    }
    return stale;
}

bool jasperengine::editor::pixelart::applyChanges(Project& project, const std::vector<FixChange>& requested, const std::string& stepName,
                                                  FixApplyReport& report, std::string& error, const Scope* verify){
    report = FixApplyReport();
    // two findings may plan one change (integer_scale's and particles_grid's camera): it is made once
    std::vector<FixChange> changes;
    for (const FixChange& change : requested){
        const bool seen = std::any_of(changes.begin(), changes.end(), [&](const FixChange& c){
            return c.target == change.target && c.field == change.field && c.to == change.to;
        });
        if (!seen)
            changes.push_back(change);
    }
    if (verify){
        const std::vector<std::string> stale = staleChanges(project, *verify, changes);
        if (!stale.empty()){
            error = "stale: " + std::to_string(stale.size()) + " change(s) no longer match the project (" + stale.front() +
                    (stale.size() > 1 ? ", ..." : "") + "), edited since the check that planned them";
            return false;
        }
    }
    std::vector<std::pair<std::string, Json>> settings;
    std::vector<FixChange> assets;
    std::vector<std::string> sceneOrder;
    std::map<std::string, std::vector<FixChange>> byScene;
    for (const FixChange& change : changes){
        if (change.target == "project"){
            auto it = std::find_if(settings.begin(), settings.end(), [&](const auto& s){ return s.first == change.field; });
            if (it != settings.end()) it->second = change.to;
            else settings.push_back({change.field, change.to});
            continue;
        }
        const size_t hash = change.target.find('#');
        if (hash != std::string::npos || isSceneField(change.field)){
            const std::string scene = hash == std::string::npos ? change.target : change.target.substr(0, hash);
            if (!byScene.count(scene))
                sceneOrder.push_back(scene);
            byScene[scene].push_back(change);
            continue;
        }
        assets.push_back(change);
    }

    mcp::McpUndo::Group group;
    mcp::McpUndo::beginGroup(group, &project);
    bool ok = true;
    if (!settings.empty()){
        auto* cmd = new ProjectSettingsValuesCmd(&project, settings, stepName);
        cmd->setNoMerge();
        ok = mcp::runProjectCommand(&project, cmd, error, "the project settings could not be set");
        if (ok)
            report.changed.push_back("project");
    }
    for (size_t i = 0; ok && i < assets.size(); i++)
        ok = applyAssetChange(project, assets[i], report, error);
    for (size_t i = 0; ok && i < sceneOrder.size(); i++)
        ok = applySceneChanges(project, sceneOrder[i], byScene[sceneOrder[i]], report, error);
    if (!ok){
        rollBack(project, group);
        return false;
    }
    const Json grouped = mcp::McpUndo::endGroup(group, &project, stepName);
    report.undo = grouped.contains("undo") ? grouped["undo"] : Json(nullptr);
    return true;
}

bool jasperengine::editor::pixelart::applyFixPlan(Project& project, const FixPlan& plan, const std::string& stepName, FixApplyReport& report,
                                                  std::string& error, const Scope* verify){
    std::vector<FixChange> changes;
    for (const auto& [key, option] : plan.fixes)
        changes.insert(changes.end(), option.changes.begin(), option.changes.end());
    return applyChanges(project, changes, stepName, report, error, verify);
}
