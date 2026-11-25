// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The atmosphere (docs/specs/S12-atmosphere.md, docs/specs/S101-vapor.md R31-R34): the Fog Profiles
// (built-in and the project's), linking a Fog or a Fog Zone to one, editing one, the Volumetric
// Clouds presets, and what a view's height fog, volumetric fog and clouds ran last frame with the
// fog's resolved look. The components' fields themselves are set_properties' (Catalog).
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/PlayViewCommon.h"

#include "FogProfileAssets.h"
#include "Project.h"
#include "command/type/AtmospherePresetCmd.h"
#include "command/type/FogProfileAssetCmd.h"
#include "command/type/LinkFogProfileCmd.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "object/Camera.h"
#include "render/AtmosphereDiagnostics.h"
#include "render/AtmospherePresets.h"
#include "render/FogResolve.h"
#include "render/RenderUtil.h"
#include "render/SceneRender.h"
#include "subsystem/RenderSystem.h"
#include "util/FileUtils.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

const char* qualityName(AtmosphereQuality quality) {
    switch (quality) {
        case AtmosphereQuality::OFF: return "off";
        case AtmosphereQuality::LOW: return "low";
        case AtmosphereQuality::MEDIUM: return "medium";
        case AtmosphereQuality::HIGH: return "high";
    }
    return "off";
}

double roundOpacity(float value) {
    return std::round((double)value * 1000.0) / 1000.0;
}

// the tool route to a diagnostic's fix (its fix sentence names the editor's way)
std::string diagnosticRoute(const AtmosphereDiagnostic& diagnostic, const AtmosphereInfo& info) {
    if (diagnostic.code == "scene_view_fog_disabled")
        return "set_scene_camera {display: {disable_fog: false}}";
    if (diagnostic.code == "fog_missing") {
        if (diagnostic.message.find("Local Volumetric Fog") == std::string::npos) return "create_entity {type: \"fog\"}";
        return "create_entity {type: \"fog\"}, then set_properties fog {type: \"height\", volumetric: true}";
    }
    if (diagnostic.code == "profile_missing" || diagnostic.code == "zone_no_effect")
        return "apply_atmosphere_preset {entity, preset} with a profile of list_atmosphere_presets";
    if (diagnostic.code == "profile_no_effect")
        return "set_properties fog {type}, or apply_atmosphere_preset another profile";
    if (diagnostic.code == "volumetric_quality_off") {
        if (info.qualitySource == "game_view_preview") return "configure_game_view {atmosphere_quality: \"project\"}";
        if (info.qualitySource == "project") return "set_project_settings quality.atmosphere_quality_desktop (or _mobile, _web)";
    }
    return "set_properties on its entity";
}

Json presetsJson(const std::vector<AtmospherePreset>& presets) {
    Json list = Json::array();
    for (const AtmospherePreset& preset : presets)
        list.push_back({{"id", preset.id}, {"name", preset.name}, {"description", preset.description}});
    return list;
}

// ------------------------------------------------------------------ Fog Profiles

namespace fs = std::filesystem;

std::string assetsRelative(const Project& project, const std::string& projectRelative) {
    const fs::path file = project.getProjectPath() / FileUtils::pathFromUtf8(projectRelative);
    return FileUtils::pathToGenericUtf8(file.lexically_normal().lexically_relative(project.getAssetsPath().lexically_normal()));
}

// the fields of `set` with `look`'s values, as a .fogprofile writes them ({"attenuationDistance": 120, "colorMode": "lit"})
Json fogFieldsJson(const FogComponent& look, const FogFieldSet& set) {
    FogProfileData data;
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++)
        if (set.test(i)) data.setFrom(look, static_cast<FogField>(i));
    const Json written = Json::parse(FogProfileIO::write(data), nullptr, false);
    return written.is_object() && written.contains("fields") ? written["fields"] : Json::object();
}

Json usersJson(const fogprofiles::FogProfileUsers& users) {
    Json list = Json::array();
    for (const fogprofiles::FogProfileUser& user : users.open)
        list.push_back({{"scene", user.sceneId}, {"sceneName", user.sceneName}, {"entity", (uint64_t)user.entity}, {"kind", user.zone ? "zone" : "fog"}});
    for (const std::string& file : users.closedFiles)
        list.push_back({{"file", file}});
    return list;
}

std::string projectProfileList(Project& project) {
    std::string paths;
    for (const fogprofiles::ProjectFogProfile& profile : fogprofiles::listProjectProfiles(project))
        paths += (paths.empty() ? "" : ", ") + profile.path;
    return paths.empty() ? std::string("none") : paths;
}

void listFogProfiles(const CallPtr& call) {
    Project& project = *call->ctx().project();
    Json builtins = Json::array();
    std::string text = std::to_string(FogProfiles::builtins().size()) + " built-in fog profiles:";
    for (const FogProfileBuiltin& builtin : FogProfiles::builtins()) {
        Json item = {{"id", builtin.id}, {"name", builtin.data.name}, {"description", builtin.data.description},
                     {"overrides", builtin.data.set.count()}, {"usedBy", 0}};
        const std::string copy = fogprofiles::findBuiltinCopy(project, builtin.id);
        if (!copy.empty()) {
            item["copy"] = copy;
            item["usedBy"] = fogprofiles::profileUsers(project, assetsRelative(project, copy)).count();
        }
        builtins.push_back(item);
        text += " " + std::string(builtin.id);
    }
    Json profiles = Json::array();
    const std::vector<fogprofiles::ProjectFogProfile> listed = fogprofiles::listProjectProfiles(project);
    text += "; " + std::to_string(listed.size()) + " project profiles:";
    for (const fogprofiles::ProjectFogProfile& profile : listed) {
        const size_t users = fogprofiles::profileUsers(project, profile.assetPath).count();
        profiles.push_back({{"path", profile.path}, {"name", profile.data.name}, {"description", profile.data.description},
                            {"overrides", profile.data.set.count()}, {"usedBy", users}});
        text += " " + profile.path + " (used by " + std::to_string(users) + ")";
    }
    if (listed.empty()) text += " none";
    call->ok(text + ".", {{"kind", "fog"}, {"presets", builtins}, {"profiles", profiles}});
}

void listAtmospherePresetsTool(const CallPtr& call) {
    const std::string kind = call->args().value("kind", std::string("fog"));
    if (kind != "fog" && kind != "clouds") {
        call->fail("kind must be fog or clouds.");
        return;
    }
    if (kind == "fog") {
        listFogProfiles(call);
        return;
    }
    const std::vector<AtmospherePreset>& presets = AtmospherePresets::getCloudsPresets();
    Json data = {{"kind", kind}, {"presets", presetsJson(presets)}};
    std::string text = std::to_string(presets.size()) + " Volumetric Clouds presets:";
    for (const AtmospherePreset& preset : presets)
        text += " " + preset.id;
    call->ok(text, data);
}

// what the next agent step should know of a profile with more than one user
std::string sharedProfileNext(const std::string& path, size_t users) {
    return "\"" + path + "\" is used by " + std::to_string(users) + " Fogs or Fog Zones: edit_fog_profile {path: \"" + path +
           "\", set} changes all of them; for this one only, edit_fog_profile {path: <new .fogprofile>, from: {profile: \"" + path +
           "\"}} and link the copy here.";
}

void applyFogProfile(const CallPtr& call, Project& project, SceneProject& scene, Entity entity) {
    const Json& args = call->args();
    std::string error;
    const std::string name = scene.scene->getEntityName(entity);
    if (args.value("unlink", false)) {
        if (!scene.scene->findComponent<FogComponent>(entity)) {
            call->fail("\"" + name + "\" is a Fog Zone: its look is its profile. Link another profile (preset), or delete the zone.");
            return;
        }
        Command* cmd = LinkFogProfileCmd::unlink(&project, scene.id, {entity}, ComponentType::FogComponent, error);
        if (!cmd) {
            if (!error.empty()) call->fail(error + ".");
            else call->ok("The Fog of " + name + " has no profile: nothing changed.", {{"changed", false}});
            return;
        }
        if (!runSceneCommand(scene.id, cmd, error)) {
            call->fail(error);
            return;
        }
        call->ok("Unlinked the Fog of " + name + " from its profile; its own fields now hold the same look (one undo step).",
                 {{"changed", true}, {"entity", (uint64_t)entity}, {"component", "fog"}, {"profile", nullptr}});
        return;
    }
    const std::string preset = args.value("preset", std::string());
    const FogComponent* fog = scene.scene->findComponent<FogComponent>(entity);
    const fogprofiles::ResolvedFogProfile profile = fogprofiles::resolveProfileName(project, preset);
    if (!profile.ok) {
        call->fail("Unknown fog profile: " + profile.error + ". Project profiles: " + projectProfileList(project) + ".");
        return;
    }
    const bool typeToHeight = fog && fog->type != FogType::HEIGHT && LinkFogProfileCmd::setsHeightFields(profile.data);
    Command* cmd = LinkFogProfileCmd::link(&project, scene.id, {entity}, fog ? ComponentType::FogComponent : ComponentType::FogZoneComponent,
                                           preset, error);
    if (!cmd) {
        if (!error.empty()) call->fail(error + ".");
        else call->ok(name + " already uses " + profile.path + ": nothing changed.", {{"changed", false}, {"profile", profile.path}});
        return;
    }
    if (!runSceneCommand(scene.id, cmd, error)) {
        call->fail(error);
        return;
    }
    const size_t users = fogprofiles::profileUsers(project, profile.assetPath).count();
    const std::string component = fog ? "fog" : "fogzone";
    std::string text = "Linked the " + std::string(fog ? "Fog" : "Fog Zone") + " of " + name + " to " + profile.path;
    if (profile.needsCopy) text += " (the project's new copy of builtin:" + profile.builtinId + ")";
    if (typeToHeight) text += "; its Type is now Height";
    text += " (one undo step). Used by " + std::to_string(users) + ".";
    Json data = {{"changed", true}, {"entity", (uint64_t)entity}, {"component", component}, {"profile", profile.path},
                 {"created", profile.needsCopy}, {"typeChanged", typeToHeight}, {"usedBy", users},
                 {"overrides", fogFieldsJson(profile.data.values, profile.data.set)}};
    if (users > 1) data["next"] = sharedProfileNext(profile.path, users);
    call->ok(text, data);
}

void applyAtmospherePresetTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!scene) {
        call->fail(error);
        return;
    }
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    const Entity entity = McpRefs::resolveEntity(scene, args.contains("entity") ? args["entity"] : Json(), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    const bool unlink = args.value("unlink", false);
    if (unlink == args.contains("preset")) {
        call->fail("Pass preset (a fog profile or clouds preset) or unlink: true, not both.");
        return;
    }
    // the GameObject's Fog or Fog Zone (a profile), else its Volumetric Clouds (a preset)
    if (scene->scene->findComponent<FogComponent>(entity) ||
        (scene->scene->isComponentRegistered<FogZoneComponent>() && scene->scene->findComponent<FogZoneComponent>(entity))) {
        applyFogProfile(call, *project, *scene, entity);
        return;
    }
    if (!scene->scene->findComponent<VolumetricCloudsComponent>(entity)) {
        call->fail("\"" + scene->scene->getEntityName(entity) + "\" has no Fog, Fog Zone or Volumetric Clouds component.");
        return;
    }
    if (unlink) {
        call->fail("unlink is for a Fog linked to a profile; Volumetric Clouds take presets only.");
        return;
    }
    const ComponentType component = ComponentType::VolumetricCloudsComponent;
    const std::string preset = args.value("preset", std::string());
    Command* cmd = AtmospherePresetCmd::build(project, scene->id, {entity}, component, preset, error);
    if (!cmd) {
        if (!error.empty()) {
            std::string ids;
            for (const AtmospherePreset& p : AtmospherePresets::getCloudsPresets())
                ids += (ids.empty() ? "" : ", ") + p.id;
            call->fail(error + " (clouds presets: " + ids + ")");
            return;
        }
        call->ok("The preset is already applied: nothing changed.", {{"changed", false}});
        return;
    }
    if (!runSceneCommand(scene->id, cmd, error)) {
        call->fail(error);
        return;
    }
    call->ok("Applied the " + preset + " preset to " + scene->scene->getEntityName(entity) + " (one undo step).",
             {{"changed", true}, {"entity", (uint64_t)entity}, {"component", Catalog::getComponentName(component, true)}, {"preset", preset}});
}

// the look fields a profile may not hold: they set cost or technique, and stay on the Fog
bool isTechniqueField(const std::string& name) {
    return name == "type" || name == "volumetric" || name == "volumetricDistance" || name == "lightShafts" || name == "qualityOverride";
}

std::string lookFieldNames() {
    std::string names;
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++)
        names += (names.empty() ? "" : ", ") + std::string(FogFields::name(static_cast<FogField>(i)));
    return names;
}

// the project file edit_fog_profile's `path` names: project- or assets-relative, under the assets folder
bool profileFile(Project& project, const std::string& given, std::string& projectRelative, bool& exists, std::string& error) {
    if (!FogProfileIO::isProfilePath(given)) {
        error = "path must be a .fogprofile file (\"FogProfiles/cave.fogprofile\"); built-ins are list_atmosphere_presets ids, "
                "copied with from: {builtin}.";
        return false;
    }
    const fs::path root = project.getProjectPath().lexically_normal();
    const fs::path assets = project.getAssetsPath().lexically_normal();
    fs::path file = (root / FileUtils::pathFromUtf8(given)).lexically_normal();
    std::error_code ec;
    const auto inside = [](const fs::path& path, const fs::path& folder) {
        const std::string relative = FileUtils::pathToGenericUtf8(path.lexically_relative(folder));
        return !relative.empty() && relative.rfind("..", 0) != 0;
    };
    if (!inside(file, assets) || (!fs::is_regular_file(file, ec) && fs::is_regular_file(assets / FileUtils::pathFromUtf8(given), ec)))
        file = (assets / FileUtils::pathFromUtf8(given)).lexically_normal();
    if (!inside(file, assets)) {
        error = "path must be inside the project's assets folder.";
        return false;
    }
    projectRelative = FileUtils::pathToGenericUtf8(file.lexically_relative(root));
    exists = fs::is_regular_file(file, ec);
    return true;
}

bool readProfileFile(Project& project, const std::string& projectRelative, FogProfileData& data, std::string& error) {
    const fs::path file = project.getProjectPath() / FileUtils::pathFromUtf8(projectRelative);
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "\"" + projectRelative + "\" does not read.";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string why;
    if (!FogProfileIO::parse(text, data, &why)) {
        error = "\"" + projectRelative + "\" is not a fog profile (" + why + "): fix it, or create another path with from.";
        return false;
    }
    return true;
}

// edit_fog_profile's `from`: the profile a new file starts as
bool profileFrom(Project& project, const Json& from, const std::string& path, FogProfileData& data, std::string& error) {
    const int given = (int)from.contains("builtin") + (int)from.contains("entity") + (int)from.contains("profile");
    if (given != 1) {
        error = "from takes one of builtin (an id), entity (a GameObject with a Fog or Fog Zone) or profile (a .fogprofile path).";
        return false;
    }
    if (from.contains("builtin")) {
        const FogProfileBuiltin* builtin = FogProfiles::findBuiltin(from.value("builtin", std::string()));
        if (!builtin) {
            std::string ids;
            for (const FogProfileBuiltin& b : FogProfiles::builtins()) ids += (ids.empty() ? "" : ", ") + std::string(b.id);
            error = "No built-in fog profile '" + from.value("builtin", std::string()) + "': " + ids + ".";
            return false;
        }
        data = builtin->data;
        return true;
    }
    if (from.contains("profile")) {
        const fogprofiles::ResolvedFogProfile source = fogprofiles::resolveProfileName(project, from.value("profile", std::string()));
        if (!source.ok) {
            error = "from.profile: " + source.error + ".";
            return false;
        }
        data = source.data;
        data.name = fs::path(path).stem().generic_string();
        return true;
    }
    SceneProject* scene = McpRefs::resolveScene(&project, from.contains("scene") ? from["scene"] : Json(), error);
    if (!scene || !scene->scene) {
        if (scene) error = McpRefs::notOpenError(&project, *scene);
        return false;
    }
    const Entity entity = McpRefs::resolveEntity(scene, from["entity"], error);
    if (entity == NULL_ENTITY) return false;
    if (const FogComponent* fog = scene->scene->findComponent<FogComponent>(entity)) {
        // the look the Fog shows (its own fields under its profile), every field set: the Inspector's New
        FogComponent look = *fog;
        if (const std::shared_ptr<const FogProfileData> linked = FogScene::profileOf(*fog)) linked->applyTo(look);
        data = FogProfileData::fromFog(look, fs::path(path).stem().generic_string());
        return true;
    }
    if (scene->scene->isComponentRegistered<FogZoneComponent>()) {
        if (const FogZoneComponent* zone = scene->scene->findComponent<FogZoneComponent>(entity)) {
            const std::shared_ptr<const FogProfileData> linked = FogProfileLibrary::getRef(zone->profile);
            if (!linked) {
                error = "The Fog Zone \"" + scene->scene->getEntityName(entity) + "\" has no readable profile to start from.";
                return false;
            }
            data = *linked;
            data.name = fs::path(path).stem().generic_string();
            return true;
        }
    }
    error = "\"" + scene->scene->getEntityName(entity) + "\" has neither a Fog nor a Fog Zone.";
    return false;
}

// the Fog of `scene` resolved at its main camera: each look field's value and the layer it comes from
Json resolvedLookJson(Scene* scene, const FogComponent& fog, const Vector3& camera) {
    FogResolve resolve;
    FogScene::resolve(scene, fog, camera, resolve);
    FogFieldSet all;
    all.set();
    const Json values = fogFieldsJson(resolve.resolved, all);
    Json fields = Json::object();
    for (size_t i = 0; i < FOG_FIELD_COUNT; i++) {
        const char* name = FogFields::name(static_cast<FogField>(i));
        fields[name] = {{"value", values.contains(name) ? values[name] : Json(nullptr)}, {"source", fogLayerSourceName(resolve.source[i])}};
    }
    return fields;
}

void editFogProfileTool(const CallPtr& call) {
    const Json& args = call->args();
    Project& project = *call->ctx().project();
    std::string error;
    std::string path;
    bool exists = false;
    if (!profileFile(project, args.value("path", std::string()), path, exists, error)) {
        call->fail(error);
        return;
    }
    const bool creating = args.contains("from");
    if (creating && exists) {
        call->fail("\"" + path + "\" exists: drop from to edit it, or create another path.");
        return;
    }
    if (!creating && !exists) {
        call->fail("No fog profile at \"" + path + "\": pass from ({builtin}, {entity} or {profile}) to create it. Project profiles: " +
                   projectProfileList(project) + ".");
        return;
    }
    FogProfileData before;
    FogProfileData data;
    if (creating) {
        if (!profileFrom(project, args["from"], path, data, error)) {
            call->fail(error);
            return;
        }
    } else {
        if (!readProfileFile(project, path, before, error)) {
            call->fail(error);
            return;
        }
        data = before;
    }

    // set: look fields as a .fogprofile writes them (the file's own parser checks them), name, description
    std::vector<std::string> changedFields;
    if (args.contains("set")) {
        const Json& set = args["set"];
        Json document = {{"version", FogProfileIO::VERSION}, {"fields", Json::object()}};
        for (auto it = set.begin(); it != set.end(); ++it) {
            if (it.key() == "name" || it.key() == "description") document[it.key()] = it.value();
            else if (isTechniqueField(it.key())) {
                call->fail(it.key() + " sets the fog's cost or technique and stays on each Fog: set_properties fog {" + it.key() + "}.");
                return;
            } else document["fields"][it.key()] = it.value();
        }
        FogProfileData parsed;
        std::vector<std::string> unknown;
        std::vector<std::string> problems;
        if (!FogProfileIO::parse(document.dump(), parsed, &error, &unknown, &problems)) {
            call->fail("set does not read: " + error + ".");
            return;
        }
        if (!unknown.empty()) {
            std::string names;
            for (const std::string& name : unknown) names += (names.empty() ? "" : ", ") + name.substr(name.rfind('.') + 1);
            call->fail("Not a fog look field: " + names + ". Fields: " + lookFieldNames() + ".");
            return;
        }
        if (!problems.empty()) {
            std::string text;
            for (const std::string& problem : problems) text += (text.empty() ? "" : "; ") + problem.substr(problem.rfind("fields.", 0) == 0 ? 7 : 0);
            call->fail("set: " + text + ".");
            return;
        }
        if (set.contains("name")) data.name = parsed.name;
        if (set.contains("description")) data.description = parsed.description;
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++) {
            if (!parsed.set.test(i)) continue;
            data.setFrom(parsed.values, static_cast<FogField>(i));
            changedFields.push_back(FogFields::name(static_cast<FogField>(i)));
        }
    }
    if (args.contains("clear")) {
        for (const Json& item : args["clear"]) {
            FogField field;
            const std::string name = item.is_string() ? item.get<std::string>() : item.dump();
            if (!FogFields::find(name, field)) {
                call->fail("clear: '" + name + "' is not a fog look field. Fields: " + lookFieldNames() + ".");
                return;
            }
            data.clear(field);
        }
    }

    const std::string assetPath = assetsRelative(project, path);
    Command* cmd = creating ? FogProfileAssetCmd::create(&project, assetPath, data) : FogProfileAssetCmd::edit(&project, assetPath, before, data);
    bool changed = false;
    if (cmd) {
        if (!runProjectCommand(&project, cmd, error, "Could not write " + path + ".")) {
            call->fail(error);
            return;
        }
        changed = true;
    }

    const fogprofiles::FogProfileUsers users = fogprofiles::profileUsers(project, assetPath);
    Json result = {{"path", path}, {"created", creating}, {"changed", changed}, {"name", data.name}, {"description", data.description},
                   {"fields", fogFieldsJson(data.values, data.set)}, {"users", usersJson(users)}, {"usedBy", users.count()}};
    // the look of the open scene that uses it, else of the Game view's scene, at its main camera
    SceneProject* lookScene = nullptr;
    for (const fogprofiles::FogProfileUser& user : users.open) {
        if (!user.zone || !lookScene) lookScene = project.getScene(user.sceneId);
        if (!user.zone) break;
    }
    if (!lookScene) lookScene = w3::gameViewScene(&project);
    const Entity fogEntity = lookScene && lookScene->scene ? FogScene::findFog(lookScene->scene) : NULL_ENTITY;
    if (fogEntity != NULL_ENTITY) {
        const FogComponent& fog = lookScene->scene->getComponent<FogComponent>(fogEntity);
        result["resolved"] = {{"scene", lookScene->id}, {"entity", (uint64_t)fogEntity},
                              {"fields", resolvedLookJson(lookScene->scene, fog, FogScene::mainCameraPosition(lookScene->scene))}};
    }
    std::string text = std::string(creating ? "Created" : (changed ? "Edited" : "Unchanged:")) + " the fog profile " + path;
    if (!changedFields.empty()) {
        text += " (set";
        for (const std::string& field : changedFields) text += " " + field;
        text += ")";
    }
    text += changed ? "; one undo step." : ".";
    text += " " + std::to_string(data.set.count()) + " fields set; used by " + std::to_string(users.count()) + ".";
    if (users.count() == 0) result["next"] = "apply_atmosphere_preset {entity, preset: \"" + path + "\"} links it to a Fog or a Fog Zone.";
    call->ok(text, result);
}

// where a view looks from: the Game view (and the game) through the game camera, the Scene view through its own
Vector3 viewCameraPosition(Project* project, SceneProject* scene, const std::string& view) {
    if (view == "scene" && scene->sceneRender) {
        if (Camera* camera = scene->sceneRender->getCamera()) return camera->getWorldPosition();
    }
    return FogScene::cameraPosition(scene->scene, project->getGameCamera(scene->id));
}

void getAtmosphereInfoTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    const std::string view = args.value("view", std::string("game"));
    if (view != "game" && view != "scene") {
        call->fail("view must be game or scene.");
        return;
    }
    std::string error;
    const bool playing = project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
    SceneProject* scene = nullptr;
    if (playing && !args.contains("scene")) {
        scene = w3::resolveRuntimeScene(project, Json(), error);
    } else if (!args.contains("scene") && view == "game") {
        scene = w3::gameViewScene(project);
        if (!scene) error = "No scene shows in the Game view.";
    } else {
        scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    }
    if (!scene) {
        call->fail(error);
        return;
    }
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    // the frame's own view is the game while playing, else the Scene view (RenderUtil.h ids)
    uint32_t viewId = 0;
    if (view == "game" && !playing) viewId = GAME_VIEW_ID;
    if (view == "scene" && playing) viewId = SCENE_VIEW_ID;
    const AtmosphereInfo& info = scene->scene->getSystem<RenderSystem>()->getAtmosphereInfo(viewId);

    Json passes = Json::array();
    for (const std::string& label : info.passes)
        passes.push_back({{"label", label}});
    Json volumetric = {
        {"enabled", info.volumetric},
        {"active", info.volumetricActive},
        {"width", info.fogWidth},
        {"height", info.fogHeight},
        {"steps", info.fogSteps},
        {"format", info.floatTargets ? "RGBA16F" : "RGBA8"},
        {"volumes", {{"used", info.volumesUsed}, {"culled", info.volumesCulled}}},
        {"localLights", info.localLights},
        {"forcedDepthPrePass", info.forcedDepthPrePass}
    };
    const char* mode = info.cloudsMode == CloudsRendering::SCREEN ? "screen" : "sky_map";
    Json data = {
        {"view", view},
        {"quality", qualityName(info.quality)},
        {"platformDefault", qualityName(info.platformDefault)},
        {"fog", {{"present", info.fog}, {"type", info.heightFog ? "height" : (info.fog ? "distance" : "none")},
                 {"horizonSource", info.horizonSource}, {"volumetric", volumetric}}},
        {"clouds", {{"present", info.clouds}, {"active", info.cloudsActive}, {"mode", info.cloudsActive ? Json(mode) : Json(nullptr)},
                    {"noiseReady", info.noiseReady}, {"mapSize", info.mapSize}, {"sliceFrames", info.sliceFrames}}},
        {"passes", passes},
        {"vramBytes", (uint64_t)info.vramBytes},
        {"vramScreenBytes", (uint64_t)info.vramScreenBytes}
    };
    std::string text = "Atmosphere (" + view + " view): quality " + qualityName(info.quality) + ", " + std::to_string(info.passes.size()) + " passes";
    if (info.volumetricActive)
        text += ", volumetric fog " + std::to_string(info.fogWidth) + "x" + std::to_string(info.fogHeight) + " " + std::to_string(info.fogSteps) + " steps";
    if (info.cloudsActive)
        text += std::string(", clouds ") + mode;

    // the fog's profile, the Fog Zones and a running blend at the view's camera, and with resolved
    // each look field and its layer (docs/specs/S101-vapor.md R34)
    const Vector3 camera = viewCameraPosition(project, scene, view);
    const Entity fogEntity = FogScene::findFog(scene->scene);
    const FogComponent* fog = fogEntity != NULL_ENTITY ? &scene->scene->getComponent<FogComponent>(fogEntity) : nullptr;
    const std::string profileKey = fog ? FogScene::profileKey(*fog) : std::string();
    data["fog"]["profile"] = profileKey.empty() ? Json(nullptr) : Json(fogprofiles::projectPathOfKey(*project, profileKey));
    if (!profileKey.empty())
        text += ", fog profile " + fogprofiles::projectPathOfKey(*project, profileKey);
    Json zones = Json::array();
    for (const FogZoneState& zone : FogScene::zoneStates(scene->scene, camera)) {
        zones.push_back({{"entity", (uint64_t)zone.entity}, {"name", scene->scene->getEntityName(zone.entity)}, {"priority", zone.priority},
                         {"weight", roundOpacity(zone.weight)}, {"distance", roundOpacity(zone.distance)},
                         {"profile", zone.profileKey.empty() ? Json(nullptr) : Json(fogprofiles::projectPathOfKey(*project, zone.profileKey))}});
    }
    if (!zones.empty()) {
        text += ", " + std::to_string(zones.size()) + " Fog Zones (weight";
        for (const Json& zone : zones) text += " " + zone["name"].get<std::string>() + " " + zone["weight"].dump();
        text += ")";
    }
    data["zones"] = zones;
    const FogBlendState blend = fog ? FogScene::blendState(*fog) : FogBlendState();
    data["blend"] = blend.active ? Json({{"target", blend.targetKey.empty() ? Json(nullptr) : Json(fogprofiles::projectPathOfKey(*project, blend.targetKey))},
                                         {"progress", roundOpacity(blend.progress)}, {"seconds", blend.seconds}})
                                 : Json(nullptr);
    if (blend.active)
        text += ", blending " + std::to_string((int)std::lround(blend.progress * 100.0f)) + "%";
    if (fog && args.value("resolved", false))
        data["resolved"] = resolvedLookJson(scene->scene, *fog, camera);

    // why the fog shows less (or more) than expected, and how dense it is (docs/specs/S101-vapor.md R17, R18)
    AtmosphereDiagnosticsInput input;
    input.scene = scene->scene;
    input.info = &info;
    input.sceneViewFogDisabled = scene->displaySettings.disableFog;
    input.camera = project->getGameCamera(scene->id);
    const AtmosphereReport report = AtmosphereDiagnostics::run(input);
    Json diagnostics = Json::array();
    const AtmosphereDiagnostic* firstWarning = nullptr;
    for (const AtmosphereDiagnostic& diagnostic : report.diagnostics) {
        Json item = {{"code", diagnostic.code}, {"severity", AtmosphereDiagnostics::severityName(diagnostic.severity)},
                     {"message", diagnostic.message}};
        if (diagnostic.entity != NULL_ENTITY) item["entity"] = (uint64_t)diagnostic.entity;
        item["fix"] = diagnostic.fix;
        diagnostics.push_back(item);
        if (!firstWarning && diagnostic.severity != AtmosphereSeverity::Info) firstWarning = &diagnostic;
    }
    data["diagnostics"] = diagnostics;
    data["opacity"] = report.opacity.valid ? Json({{"at10", roundOpacity(report.opacity.at10)}, {"at50", roundOpacity(report.opacity.at50)},
                                                   {"at200", roundOpacity(report.opacity.at200)}, {"includes", "analytic"}})
                                           : Json(nullptr);
    if (!report.diagnostics.empty()) {
        text += ", " + std::to_string(report.diagnostics.size()) + " diagnostics:";
        for (const AtmosphereDiagnostic& diagnostic : report.diagnostics)
            text += " " + diagnostic.code;
    }
    if (firstWarning)
        data["next"] = "Apply each warning's fix before changing the fog's look: " + firstWarning->code + " -> " +
                       diagnosticRoute(*firstWarning, info) + ". Then call get_atmosphere_info again.";
    call->ok(text, data);
}

} // namespace

void registerAtmosphereTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "list_atmosphere_presets";
        tool.title = "List Atmosphere Presets";
        tool.description = "What apply_atmosphere_preset takes: fog (the built-in fog profiles and the project's .fogprofile files, "
                           "each with its users) or clouds (Volumetric Clouds presets).";
        tool.inputSchema = object({
            {"kind", withDefault(stringEnum("", {"fog", "clouds"}), "fog")}
        });
        tool.annotations.readOnly = true;
        tool.handler = listAtmospherePresetsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "apply_atmosphere_preset";
        tool.title = "Apply Atmosphere Preset";
        tool.description = "Link a Fog or Fog Zone to a fog profile (a built-in id, copied into the project on first use, or a "
                           ".fogprofile path), or set a Volumetric Clouds preset; one undo step. unlink: true keeps the Fog's look "
                           "and drops its profile.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("The GameObject with the Fog, Fog Zone or Volumetric Clouds.")},
            {"preset", string("Fog profile (night, FogProfiles/cave.fogprofile) or clouds preset id.")},
            {"unlink", boolean("Fog: unlink its profile instead.")}
        }, {"entity"});
        tool.allowedWhilePlaying = false;
        tool.handler = applyAtmospherePresetTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "edit_fog_profile";
        tool.title = "Edit Fog Profile";
        tool.group = "rendering_fx";
        tool.description = "Change or create a .fogprofile, the fog look every linked Fog and Fog Zone shares; one undo step. from "
                           "creates path from a built-in, a GameObject's Fog or another profile; set writes look fields and ticks "
                           "them; clear unticks. It changes every user: for one scene only, create a copy (from: {profile}) and "
                           "link that.";
        tool.inputSchema = object({
            {"path", string("The .fogprofile, e.g. FogProfiles/cave.fogprofile.")},
            {"from", object({
                {"builtin", string("")},
                {"entity", entityRef()},
                {"scene", sceneRef()},
                {"profile", string("")}
            })},
            {"set", anyObject("{field: value} as the file holds them; also name, description.")},
            {"clear", array(string(""), "Fields to untick.")}
        }, {"path"});
        tool.allowedWhilePlaying = false;
        tool.handler = editFogProfileTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_atmosphere_info";
        tool.title = "Get Atmosphere Info";
        tool.description = "What a view's height fog, volumetric fog and clouds ran last frame, the fog's profile, Fog Zone weights "
                           "and blend, its estimated opacity ahead of the main camera, and diagnostics: why fog does not show or "
                           "looks wrong, each with its fix. Call it first when fog is missing, too thin or too dense.";
        tool.inputSchema = object({
            {"view", withDefault(stringEnum("", {"game", "scene"}), "game")},
            {"scene", sceneRef("Default: the playing scene, else the Game view's.")},
            {"resolved", withDefault(boolean("Each look field's value and the layer it comes from."), false)}
        });
        tool.annotations.readOnly = true;
        tool.handler = getAtmosphereInfoTool;
        registry.addTool(std::move(tool));
    }
}

}
