// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_build_profile (docs/specs/build-profiles.md R22): the project's Build Profiles, each change one
// project undo step (EditBuildProfileCmd), and the user's active profile (not undoable).

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "buildprofile/BuildProfile.h"
#include "command/type/EditBuildProfileCmd.h"
#include "publish/Publishing.h"
#include "util/FileUtils.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
using namespace schema;

std::optional<std::string> profileFileText(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return std::nullopt;
    std::ifstream stream(file, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

bool isGuidText(const std::string& text) {
    if (text.size() != 32) return false;
    for (char c : text)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

std::string projectRelative(Project* project, const fs::path& path) {
    return FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(path));
}

// a scene named by path or GUID -> {guid, path}
bool sceneRef(Project* project, const std::string& text, BuildProfileScene& out, std::string& error) {
    Guid guid;
    if (isGuidText(text)) guid = Guid::parse(text);
    else guid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(FileUtils::pathFromUtf8(text)));
    const fs::path path = guid.isNull() ? fs::path() : project->getAssetDatabase().getPath(guid);
    if (path.empty() || path.extension() != ".scene") {
        error = text + " is not a scene of the project (a project-relative path or a GUID)";
        return false;
    }
    out.guid = guid.str();
    out.path = FileUtils::pathToGenericUtf8(path);
    return true;
}

bool stringList(const Json& value, const char* key, std::vector<std::string>& out, std::string& error) {
    out.clear();
    if (value.is_null()) return true;
    if (!value.is_array()) {
        error = std::string("values.") + key + " must be a list of strings";
        return false;
    }
    for (const Json& item : value) {
        if (!item.is_string()) {
            error = std::string("values.") + key + " must be a list of strings";
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

// values{} and overrides{} into `profile`
bool applyEdits(Project* project, const Json& args, BuildProfile& profile, std::string& error) {
    if (args.contains("platform") && args["platform"].is_string() && !parseBuildPlatform(args["platform"].get<std::string>(), profile.platform)) {
        error = "platform must be windows, linux, macos, android, ios or web";
        return false;
    }
    const Json values = args.value("values", Json::object());
    if (!values.is_object()) {
        error = "values must be an object";
        return false;
    }
    struct TextField {
        const char* key;
        std::string* field;
    };
    const TextField texts[] = {{"product_name", &profile.productName}, {"identifier", &profile.identifier}, {"version_suffix", &profile.versionSuffix},
                               {"icon", &profile.icon}, {"store", &profile.store}, {"store_title", &profile.storeTitle},
                               {"backend", &profile.backend}, {"output_dir", &profile.outputDir}};
    for (const auto& [key, value] : values.items()) {
        bool known = false;
        for (const TextField& text : texts) {
            if (key != text.key) continue;
            known = true;
            if (!value.is_null() && !value.is_string()) {
                error = "values." + key + " must be a string (\"\" or null inherits)";
                return false;
            }
            *text.field = value.is_null() ? std::string() : value.get<std::string>();
        }
        if (known) continue;
        if (key == "development") {
            if (!value.is_null() && !value.is_boolean()) {
                error = "values.development must be true, false or null (Build Settings' checkbox)";
                return false;
            }
            profile.development = value.is_null() ? std::nullopt : std::optional<bool>(value.get<bool>());
        } else if (key == "scenes") {
            if (value.is_null()) {
                profile.scenes.reset();
                continue;
            }
            if (!value.is_array()) {
                error = "values.scenes must be a list of scene paths or GUIDs, or null for the project's Scenes In Build";
                return false;
            }
            std::vector<BuildProfileScene> scenes;
            for (const Json& item : value) {
                BuildProfileScene scene;
                if (!item.is_string() || !sceneRef(project, item.get<std::string>(), scene, error)) {
                    if (!item.is_string()) error = "values.scenes holds scene paths or GUIDs";
                    else error = "values.scenes: " + error + "; edit_build_scenes lists the project's own";
                    return false;
                }
                scenes.push_back(std::move(scene));
            }
            profile.scenes = std::move(scenes);
        } else if (key == "start_scene") {
            if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
                profile.startScene.clear();
                continue;
            }
            BuildProfileScene scene;
            if (!value.is_string() || !sceneRef(project, value.get<std::string>(), scene, error)) {
                if (!value.is_string()) error = "values.start_scene is a scene path or GUID";
                else error = "values.start_scene: " + error;
                return false;
            }
            profile.startScene = scene.guid;
        } else if (key == "defines") {
            if (!stringList(value, "defines", profile.defines, error)) return false;
        } else if (key == "exclude_labels") {
            if (!stringList(value, "exclude_labels", profile.excludeLabels, error)) return false;
        } else if (key == "exclude_paths") {
            if (!stringList(value, "exclude_paths", profile.excludePaths, error)) return false;
        } else {
            error = "values has no key " + key + " (product_name, identifier, version_suffix, icon, store, store_title, backend, development, "
                    "output_dir, scenes, start_scene, defines, exclude_labels, exclude_paths)";
            return false;
        }
    }
    const Json overrides = args.value("overrides", Json::object());
    if (!overrides.is_object()) {
        error = "overrides must be an object of settings keys (null removes one)";
        return false;
    }
    for (const auto& [key, value] : overrides.items()) {
        if (value.is_null()) profile.overrides.erase(key);
        else profile.overrides[key] = value;
    }
    return true;
}

Json valuesJson(const BuildProfile& profile) {
    Json scenes = nullptr;
    if (profile.scenes) {
        scenes = Json::array();
        for (const BuildProfileScene& scene : *profile.scenes) scenes.push_back({{"guid", scene.guid}, {"path", scene.path}});
    }
    return {{"product_name", profile.productName},
            {"identifier", profile.identifier},
            {"version_suffix", profile.versionSuffix},
            {"icon", profile.icon},
            {"store", profile.store},
            {"store_title", profile.storeTitle},
            {"backend", profile.backend},
            {"development", profile.development ? Json(*profile.development) : Json(nullptr)},
            {"output_dir", profile.outputDir},
            {"scenes", scenes},
            {"start_scene", profile.startScene},
            {"defines", profile.defines},
            {"exclude_labels", profile.excludeLabels},
            {"exclude_paths", profile.excludePaths}};
}

Json overridesJson(const BuildProfile& profile) {
    Json out = Json::object();
    for (const auto& [key, value] : profile.overrides) out[key] = value;
    return out;
}

// one project undo step writing these files; files_written project-relative
bool commit(Project* project, const std::string& stepName, std::vector<EditBuildProfileCmd::FileChange> changes, Json& filesWritten,
            std::string& error, const std::optional<std::pair<std::string, std::string>>& activeNameChange = std::nullopt) {
    filesWritten = Json::array();
    for (const EditBuildProfileCmd::FileChange& change : changes) filesWritten.push_back(projectRelative(project, change.path));
    auto* command = new EditBuildProfileCmd(stepName, std::move(changes));
    command->setNoMerge();
    if (activeNameChange) command->setActiveNameChange(project->getProjectPath(), activeNameChange->first, activeNameChange->second);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        error = failure->empty() ? "the Build Profile could not be written (see the console)" : *failure;
        return false;
    }
    return true;
}

std::string findingsText(const std::vector<std::string>& findings) {
    std::string text;
    for (const std::string& finding : findings) text += (text.empty() ? "" : "; ") + finding;
    return text;
}

void editBuildProfileTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    const fs::path projectPath = project->getProjectPath();
    const std::string action = args.value("action", "list");
    const bool dryRun = args.value("dry_run", false);
    const std::vector<BuildProfileEntry> entries = buildprofiles::list(projectPath);
    const std::string activeName = buildprofiles::activeName(projectPath);
    std::string error;

    auto entryJson = [&](const BuildProfileEntry& entry) {
        Json item = {{"name", entry.name}, {"platform", entry.readable ? buildPlatformName(entry.profile.platform) : ""},
                     {"active", !activeName.empty() && buildprofiles::find({entry}, activeName) != nullptr}, {"readable", entry.readable}};
        if (!entry.readable) item["error"] = entry.error;
        return item;
    };

    if (action == "list") {
        Json list = Json::array();
        for (const BuildProfileEntry& entry : entries) list.push_back(entryJson(entry));
        call->ok(std::to_string(entries.size()) + " Build Profile(s)" + (activeName.empty() ? "" : ", active: " + activeName) + ".",
                 Json{{"profiles", list}, {"active_profile", activeName.empty() ? Json(nullptr) : Json(activeName)}});
        return;
    }

    if (action == "activate") {
        const Json name = args.contains("name") ? args["name"] : Json(nullptr);
        if (!name.is_null() && !name.is_string()) {
            call->fail("activate takes name (a Build Profile) or null for none.");
            return;
        }
        std::string chosen;
        if (name.is_string() && !name.get<std::string>().empty()) {
            const BuildProfileEntry* entry = buildprofiles::find(entries, name.get<std::string>());
            if (!entry || !entry->readable) {
                BuildProfile probe;
                buildprofiles::load(projectPath, name.get<std::string>(), probe, error);
                call->fail(error + ".");
                return;
            }
            chosen = entry->name;
        }
        if (!buildprofiles::setActiveName(projectPath, chosen, error)) {
            call->fail("The active Build Profile could not be saved: " + error + ".");
            return;
        }
        call->ok(chosen.empty() ? "No Build Profile is active: Play and Build Settings use the project's settings."
                                : "Build Profile '" + chosen + "' is active: Play uses its identity, defines and Scene List (per user, not undoable).",
                 Json{{"active_profile", chosen.empty() ? Json(nullptr) : Json(chosen)}});
        return;
    }

    const std::string name = args.value("name", "");
    if (name.empty()) {
        call->fail(action + " needs name (edit_build_profile action list names them).");
        return;
    }
    const BuildProfileEntry* entry = buildprofiles::find(entries, name);

    if (action == "create") {
        std::string why;
        if (!BuildProfileFile::isValidName(name, &why)) {
            call->fail(why + ".");
            return;
        }
        if (entry) {
            call->fail("Build Profile '" + entry->name + "' exists already: edit it with action set, or pick another name.");
            return;
        }
        if (!args.contains("platform") || !args["platform"].is_string()) {
            call->fail("create needs platform (windows, linux, macos, android, ios or web).");
            return;
        }
    } else if (!entry) {
        BuildProfile probe;
        buildprofiles::load(projectPath, name, probe, error);
        call->fail(error + ".");
        return;
    }

    if (action == "get") {
        Json data = entryJson(*entry);
        data["file"] = projectRelative(project, entry->file);
        if (entry->readable) {
            data["values"] = valuesJson(entry->profile);
            data["overrides"] = overridesJson(entry->profile);
            Json resolved = Json::object();
            for (const auto& [field, value] : buildprofiles::resolveIdentity(*project, entry->profile))
                resolved[field] = {{"value", value.value}, {"source", value.source}};
            data["resolved"] = resolved;
            Json store = Json::array();
            for (const BuildWarning& finding : buildprofiles::storeFindings(*project, entry->profile))
                store.push_back({{"code", finding.code}, {"message", finding.message}});
            data["store_findings"] = store;
            data["findings"] = validateProfile(*project, entry->profile);
        }
        call->ok("Build Profile '" + entry->name + "'" + (entry->readable ? " (" + std::string(buildPlatformName(entry->profile.platform)) + ")." :
                                                          ": " + entry->error + "."),
                 data);
        return;
    }

    if (action == "delete") {
        std::vector<EditBuildProfileCmd::FileChange> changes = {{entry->file, profileFileText(entry->file), std::nullopt}};
        Json files;
        if (dryRun) {
            call->ok("Would delete Build Profile '" + entry->name + "'.", Json{{"dry_run", true}});
            return;
        }
        if (!commit(project, "Delete Build Profile '" + entry->name + "'", std::move(changes), files, error)) {
            call->fail(error + ".");
            return;
        }
        call->ok("Deleted Build Profile '" + entry->name + "' (undo puts it back).", Json{{"files_written", files}});
        return;
    }

    if (action == "rename" || action == "duplicate") {
        const std::string newName = args.value("new_name", "");
        std::string why;
        if (!BuildProfileFile::isValidName(newName, &why)) {
            call->fail("new_name: " + why + ".");
            return;
        }
        const BuildProfileEntry* taken = buildprofiles::find(entries, newName);
        if (taken && taken != entry) {
            call->fail("Build Profile '" + taken->name + "' exists already: pick another new_name.");
            return;
        }
        const fs::path target = BuildProfileFile::pathOf(projectPath, newName);
        std::vector<EditBuildProfileCmd::FileChange> changes;
        const std::optional<std::string> text = profileFileText(entry->file);
        if (action == "rename") {
            changes.push_back({entry->file, text, std::nullopt});
            changes.push_back({target, std::nullopt, text});
            // publishing.json targets naming it follow the new name in the same step
            const fs::path publishingFile = publish::PublishingConfig::pathOf(projectPath);
            publish::PublishingConfig publishing;
            std::string publishingError;
            if (publish::PublishingConfig::load(publishingFile, publishing, publishingError)) {
                bool renamed = false;
                for (auto& [targetName, publishTarget] : publishing.targets) {
                    if (buildprofiles::find({*entry}, publishTarget.profile)) {
                        publishTarget.profile = newName;
                        renamed = true;
                    }
                }
                if (renamed) changes.push_back({publishingFile, profileFileText(publishingFile), publishing.toJson()});
            }
        } else {
            changes.push_back({target, std::nullopt, text});
        }
        if (dryRun) {
            call->ok("Would " + action + " Build Profile '" + entry->name + "' as '" + newName + "'.", Json{{"dry_run", true}});
            return;
        }
        Json files;
        const std::string verb = action == "rename" ? "Rename" : "Duplicate";
        // the user's active profile follows its rename, in the same undo step
        const bool renamesActive = action == "rename" && buildprofiles::find({*entry}, activeName);
        if (!commit(project, verb + " Build Profile '" + entry->name + "'", std::move(changes), files, error,
                    renamesActive ? std::optional<std::pair<std::string, std::string>>({activeName, newName}) : std::nullopt)) {
            call->fail(error + ".");
            return;
        }
        call->ok((action == "rename" ? "Renamed" : "Duplicated") + std::string(" Build Profile '") + entry->name + "' as '" + newName + "'.",
                 Json{{"files_written", files}});
        return;
    }

    if (action != "set" && action != "create") {
        call->fail("action must be list, get, create, duplicate, rename, delete, set or activate.");
        return;
    }
    if (entry && !entry->readable) {
        call->fail("Build Profile '" + entry->name + "' " + entry->error + ".");
        return;
    }
    BuildProfile profile = entry ? entry->profile : BuildProfile();
    profile.name = entry ? entry->name : name;
    if (!applyEdits(project, args, profile, error)) {
        call->fail(error + ".");
        return;
    }
    const std::vector<std::string> findings = validateProfile(*project, profile);
    const fs::path file = entry ? entry->file : BuildProfileFile::pathOf(projectPath, name);
    const std::string text = BuildProfileFile::encode(profile);
    if (dryRun) {
        call->ok(findings.empty() ? "The edit is valid (nothing written)." : "The edit would be refused: " + findingsText(findings) + ".",
                 Json{{"dry_run", true}, {"findings", findings}, {"values", valuesJson(profile)}, {"overrides", overridesJson(profile)}});
        return;
    }
    if (!findings.empty()) {
        call->fail("Build Profile '" + profile.name + "' not saved: " + findingsText(findings) + ".", Json{{"findings", findings}});
        return;
    }
    Json files;
    const std::string step = std::string(entry ? "Edit" : "Create") + " Build Profile '" + profile.name + "'";
    if (!commit(project, step, {{file, entry ? profileFileText(file) : std::nullopt, text}}, files, error)) {
        call->fail(error + ".");
        return;
    }
    Json data = {{"files_written", files}, {"values", valuesJson(profile)}, {"overrides", overridesJson(profile)}};
    Json store = Json::array();
    for (const BuildWarning& finding : buildprofiles::storeFindings(*project, profile)) store.push_back({{"code", finding.code}, {"message", finding.message}});
    data["store_findings"] = store;
    data["next"] = "build it with build_project profile \"" + profile.name + "\"";
    call->ok(std::string(entry ? "Saved" : "Created") + " Build Profile '" + profile.name + "' (" + buildPlatformName(profile.platform) + ").", data);
}

} // namespace

void registerBuildProfileTools(McpRegistry& registry) {
    const Json stringItem = Json{{"type", "string"}};
    ToolDef tool;
    tool.name = "edit_build_profile";
    tool.title = "Edit Build Profile";
    tool.description =
        "Build Profiles (Build Settings): named per-platform variants overriding identity, store, API, scenes, defines, content "
        "exclusions and Player Settings keys. Each file change is one undo step; activate (per user, not undoable) is what Play "
        "applies. scenes take paths or GUIDs (the project's own list: edit_build_scenes). Build with build_project profile.";
    tool.inputSchema = object({
        {"action", stringEnum("", {"list", "get", "create", "duplicate", "rename", "delete", "set", "activate"})},
        {"name", Json{{"type", Json::array({"string", "null"})}}},
        {"new_name", string("")},
        {"platform", stringEnum("", {"windows", "linux", "macos", "android", "ios", "web"})},
        {"values", object({
            {"product_name", string("")},
            {"identifier", string("")},
            {"version_suffix", string("")},
            {"icon", string("")},
            {"store", stringEnum("", {"none", "google-play", "app-store", "steam", "itch", "web"})},
            {"store_title", string("")},
            {"backend", string("")},
            {"development", Json{{"type", Json::array({"boolean", "null"})}}},
            {"output_dir", string("")},
            {"scenes", Json{{"type", Json::array({"array", "null"})}, {"items", stringItem}}},
            {"start_scene", string("")},
            {"defines", array(stringItem, "")},
            {"exclude_labels", array(stringItem, "")},
            {"exclude_paths", array(stringItem, "")}
        })},
        {"overrides", anyObject("Settings key -> value; null removes.")},
        {"dry_run", withDefault(boolean(""), false)}
    }, {"action"});
    tool.group = "shipping";
    tool.allowedWhilePlaying = false;
    tool.handler = editBuildProfileTool;
    registry.addTool(std::move(tool));
}

}
