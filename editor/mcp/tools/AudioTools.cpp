// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Audio mixer (spec docs/superpowers/specs/2026-10-31-audio-mixer-design.md 10): audio_mixer get
// (the structure; in Play the live values and meters) and edit (ops on the .mixer, one undo step;
// the project's mixer is created on first use and heard at once, Edit mode and Play alike).
#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "audio/AudioMixer.h"
#include "audio/AudioMixerDocument.h"
#include "util/FileUtils.h"

#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;

// the mixer a call is about: `mixer` (project- or assets-relative), else the project's
bool mixerFile(McpContext& ctx, const Json& args, bool create, fs::path& file, bool& projectMixer, bool& created, std::string& error) {
    Project* project = ctx.project();
    created = false;
    if (!project) {
        error = "No project is open.";
        return false;
    }
    const fs::path projectFile = project->getAudioMixerFile();
    if (args.contains("mixer") && args["mixer"].is_string() && !args["mixer"].get<std::string>().empty()) {
        const std::string text = args["mixer"].get<std::string>();
        fs::path path = project->getProjectPath() / FileUtils::pathFromUtf8(text);
        std::error_code ec;
        if (!fs::exists(path, ec) && text.rfind("assets/", 0) != 0) path = project->resolveAssetPath(FileUtils::pathFromUtf8(text));
        path = path.lexically_normal();
        if (!project->isInsideAssetsPath(path)) {
            error = "`mixer` must be a .mixer file inside the assets folder (\"audio/Main.mixer\").";
            return false;
        }
        if (path.extension() != ".mixer") {
            error = "`mixer` must be a .mixer file.";
            return false;
        }
        file = path;
        projectMixer = !projectFile.empty() && path == projectFile;
        if (!fs::is_regular_file(file, ec)) {
            if (!create) {
                error = w4::projectRelative(project, file) + " does not exist (audio_mixer edit creates it).";
                return false;
            }
            if (!AudioMixerDocument::commit(project, file, AudioMixerIO::createDefault(), "", &error)) return false;
            created = true;
        }
        return true;
    }
    projectMixer = true;
    std::error_code ec;
    if (!create) {
        if (projectFile.empty() || !fs::is_regular_file(projectFile, ec)) {
            file.clear();
            return true;   // the implicit Master
        }
        file = projectFile;
        return true;
    }
    return AudioMixerDocument::ensureProjectMixer(project, file, &error, &created);
}

std::string summary(const AudioMixerDef& def) {
    std::string groups;
    for (const AudioMixerGroupDef* group : def.treeOrder()) {
        groups += (groups.empty() ? "" : ", ") + group->name;
        if (!group->effects.empty()) {
            groups += " [";
            for (size_t i = 0; i < group->effects.size(); i++) {
                groups += (i ? " " : "") + (group->effects[i].type == AudioEffectType::Unknown ? group->effects[i].typeName
                                                                                                : AudioEffects::typeName(group->effects[i].type));
            }
            groups += "]";
        }
    }
    std::string exposed;
    for (const AudioMixerExposedDef& entry : def.exposed) exposed += (exposed.empty() ? "" : ", ") + entry.name;
    std::string snapshots;
    for (const AudioMixerSnapshotDef& snapshot : def.snapshots) {
        snapshots += (snapshots.empty() ? "" : ", ") + snapshot.name + (snapshot.name == def.startSnapshot ? " (start)" : "");
    }
    return "Groups: " + groups + ". Exposed: " + (exposed.empty() ? "none" : exposed) + ". Snapshots: " +
           (snapshots.empty() ? "none" : snapshots) + ".";
}

void audioMixerTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    const std::string action = args.value("action", "");
    Project* project = ctx.project();

    if (action != "get" && action != "edit") {
        call->fail("Unknown action \"" + action + "\". Valid: get, edit.");
        return;
    }

    fs::path file;
    bool projectMixer = false;
    bool created = false;
    std::string error;
    if (!mixerFile(ctx, args, action == "edit", file, projectMixer, created, error)) {
        call->fail(error);
        return;
    }
    const bool playing = project && project->isAnyScenePlaying();

    if (action == "get") {
        if (file.empty()) {
            // no mixer yet: the implicit Master
            Json data = AudioMixerDocument::describe(AudioMixer::getDef(), playing, project ? project->getAudioSettings().loudnessTargetLufs : -18.0f);
            data["mixer"] = nullptr;
            data["project_mixer"] = true;
            call->ok("The project has no Audio Mixer yet: sounds play through an implicit Master at 0 dB. "
                     "audio_mixer edit creates audio/Main.mixer in the assets folder.", data);
            return;
        }
        AudioMixerDef def;
        std::vector<std::string> warnings;
        if (!AudioMixerDocument::read(file, def, &error, &warnings)) {
            call->fail(w4::projectRelative(project, file) + ": " + error);
            return;
        }
        const bool live = playing && projectMixer;
        Json data = AudioMixerDocument::describe(def, live, project ? project->getAudioSettings().loudnessTargetLufs : -18.0f);
        data["mixer"] = w4::projectRelative(project, file);
        data["project_mixer"] = projectMixer;
        if (!warnings.empty()) data["warnings"] = warnings;
        call->ok(w4::projectRelative(project, file) + (projectMixer ? " (the project's mixer). " : ". ") + summary(def) +
                 (live ? " Live values and meters in data (Play)." : ""), data);
        return;
    }

    // edit
    if (!args.contains("ops")) {
        call->fail("edit: `ops` (a list of {op: ...}) is required. Ops: add_group, remove_group, set_group, add_effect, "
                   "set_effect, remove_effect, expose, unexpose, set_exposed, add_snapshot, set_snapshot, remove_snapshot, "
                   "set_start_snapshot, set_mixer. Guide: jasper://docs/audio.");
        return;
    }
    AudioMixerDef def;
    if (!AudioMixerDocument::read(file, def, &error)) {
        call->fail(w4::projectRelative(project, file) + ": " + error);
        return;
    }
    std::vector<std::string> notes;
    if (!AudioMixerDocument::applyOps(def, args["ops"], &error, &notes)) {
        call->fail(error);
        return;
    }
    if (!AudioMixerDocument::commit(project, file, def, "", &error)) {
        call->fail(error);
        return;
    }
    Json data = AudioMixerDocument::describe(def, false);
    data["mixer"] = w4::projectRelative(project, file);
    data["project_mixer"] = projectMixer;
    if (created) data["created"] = true;
    if (!notes.empty()) data["notes"] = notes;
    std::string text = std::string(created ? "Created " : "Edited ") + w4::projectRelative(project, file) +
                       (projectMixer ? " (the project's mixer, heard at once)" : "") + ": " + summary(def) + " One undo step.";
    for (const std::string& note : notes) text += " Note: " + note + ".";
    call->ok(text, data);
}

} // namespace

void registerAudioTools(McpRegistry& registry) {
    using namespace schema;

    ToolDef tool;
    tool.name = "audio_mixer";
    tool.title = "Audio Mixer";
    tool.description =
        "The project's Audio Mixer (.mixer; Unity's AudioMixer). get: groups, effects, exposed parameters, snapshots "
        "(in Play also live values and meters). edit: `ops` applied in order, one undo step, heard at once; creates "
        "audio/Main.mixer in the assets when the project has none. Ops and effect parameters: jasper://docs/audio.";
    tool.inputSchema = object({
        {"action", stringEnum("", {"get", "edit"})},
        {"mixer", string("A .mixer path; default the project's.")},
        {"ops", array(anyObject(""), "edit: [{op: add_group|remove_group|set_group|add_effect|set_effect|remove_effect|"
                                     "expose|unexpose|set_exposed|add_snapshot|set_snapshot|remove_snapshot|"
                                     "set_start_snapshot|set_mixer, ...}]")}
    }, {"action"});
    tool.writesFiles = [](const Json& args) { return args.value("action", "") == "edit"; };
    tool.activity = ToolActivity::Edit;
    tool.handler = audioMixerTool;
    registry.addTool(std::move(tool));
}

}
