// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "audio/AudioMixerIO.h"
#include "component/AudioSourceComponent.h"

#include "json.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct AudioSettings;

    class Project;

    // A .mixer asset as the editor edits it (audio-mixer spec 9, 10): the Audio Mixer window and
    // the MCP audio_mixer tool. Every change is one step of the project history (WriteFilesCmd:
    // undo puts the file back), and the project's mixer is heard as the file says at once
    // (Project::audioFilesChanged), in Edit mode and in Play.
    class AudioMixerDocument {
    public:
        // The project's mixer file: Project Settings > Audio's, else audio/Main.mixer in the assets (set
        // as the project's). Created with Master when missing (`created`). False + error when it
        // cannot be written.
        static bool ensureProjectMixer(Project* project, std::filesystem::path& file, std::string* error, bool* created = nullptr);
        // a .mixer file -> def (false + error: missing, not JSON, invalid)
        // What a new project starts from (S38 R8): a Master true-peak limiter at -1 dBTP, 1.5 ms of
        // lookahead. writeNewProjectAudio makes a project folder start from it, unless its
        // project.yaml has an audio node of its own (a template's): the mixer file and the keys.
        static AudioMixerDef newProjectMixer();
        static AudioSettings newProjectAudioSettings();
        static bool writeNewProjectMixer(const std::filesystem::path& assetsDir, std::string* error);
        static bool writeNewProjectAudio(const std::filesystem::path& projectDir, std::string* error);

        static bool read(const std::filesystem::path& file, AudioMixerDef& def, std::string* error,
                         std::vector<std::string>* warnings = nullptr);
        // The def written as one undo step (validated first: false + error, nothing written). A
        // non-empty merge key merges it with the step before it that has the same key (a fader
        // drag: one step).
        static bool commit(Project* project, const std::filesystem::path& file, const AudioMixerDef& def,
                           const std::string& mergeKey, std::string* error);

        // The MCP ops (spec 10) on a def, in order; false + error naming the op ("ops[2] add_effect:
        // ..."), the def then as it was before the first. `notes` gets what was done implicitly.
        static bool applyOps(AudioMixerDef& def, const nlohmann::json& ops, std::string* error,
                             std::vector<std::string>* notes = nullptr);
        static bool applyOp(AudioMixerDef& def, const nlohmann::json& op, std::string* error,
                            std::vector<std::string>* notes = nullptr);

        // every parameter's value now: the runtime's when `def` is the mixer playing (Play), else
        // the def's own
        static std::map<std::string, float> captureValues(const AudioMixerDef& def, bool runtime);
        // A snapshot / set_snapshot key -> a target: an exposed name, a target, "<group>/volume",
        // "<group>/<effect type or index>/<param>"; "" when it does not resolve
        static std::string resolveKey(const AudioMixerDef& def, const std::string& key);

        // The structure for the MCP get (names first, ids for the files); `live` adds the runtime
        // values, overrides and meters (the def must be the playing mixer)
        static nlohmann::json describe(const AudioMixerDef& def, bool live, float targetLufs = -18.0f);

        // effect lookup in a group by index (0-based, a number) or type ("lowpass"; the first of it)
        static int findEffect(const AudioMixerGroupDef& group, const nlohmann::json& which);
        // the groups under `groupId` (itself included), children first
        static std::vector<std::string> subtree(const AudioMixerDef& def, const std::string& groupId);
        // removes a group, its subgroups, their effects, what was exposed and snapshotted from them
        static void removeGroup(AudioMixerDef& def, const std::string& groupId);
        static void removeEffect(AudioMixerDef& def, const std::string& groupId, size_t index);
        // drops exposed parameters and snapshot values whose target no longer resolves
        static void pruneTargets(AudioMixerDef& def);
        // a unique name from `base` ("Group", "Group 2", ...)
        static std::string uniqueGroupName(const AudioMixerDef& def, const std::string& base);

        // A new Audio Source as the editor makes it (Add Component, GameObject > Audio): it plays
        // through Project Settings > Audio's Default Output Group (Project::setAudioSettings sets it)
        static AudioSourceComponent newAudioSource();
        static void setDefaultOutputGroup(const std::string& group);
    };

}
