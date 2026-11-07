// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's model clip settings (S74 R30, R31, R48), Unity's model importer clip list:
//   - the model's clips as rows: Clip, Length, Wrap Mode, Root Motion, Memory, Keys, Loop Match
//     (the numbers printed from ClipReport, measured a few rows a frame), multi-select with Ctrl
//     and Shift;
//   - the selected clips' Wrap Mode and Root Motion (Root Motion Node, Root Transform Rotation,
//     Position (Y), Position (XZ): Bake Into Pose) in a working copy, mixed values as an em
//     dash, Revert and Apply (one undo step writing the model's <model>.clips), and the prompt
//     for edits not applied when the model, the editor or Play moves on;
//   - the loop suggestion (a clip with no setting, a cycle's name and a Loop Match under 10 mm):
//     shown, never applied until Use;
//   - Extract to .anim... (one folder prompt, one undo step) and Add to Animator... (a state per
//     clip, one undo step).
// Self-contained so a host can move it (S53's Model Import Settings shows it in its Animation tab).

#pragma once

#include "animation/ClipReport.h"
#include "animation/ModelClipSettings.h"
#include "animation/ModelSubClips.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class ModelClipSettingsView {
    public:
        explicit ModelClipSettingsView(Project* project);
        ~ModelClipSettingsView();

        // Draws the section of `model` (asset-relative, as components store it; `file` its absolute
        // path) whose clips are `clips`. With edits not applied, another model opens the prompt and
        // the edited one stays shown.
        void draw(const std::filesystem::path& file, const std::string& model, const std::vector<anim::ModelClipInfo>& clips);
        bool hasPendingEdits() const;

        // what is shown: the model and its first selected clip (the host shows that clip under it)
        const std::string& shownModel() const { return model; }
        const std::filesystem::path& shownFile() const { return file; }
        std::string shownClip() const;

        // the loop suggestion's rule (R30): a clip with no setting, whose name says it cycles,
        // whose Loop Match is under LOOP_SUGGESTION_MM
        static constexpr double LOOP_SUGGESTION_MM = 10.0;
        static bool namesACycle(const std::string& clip);

    private:
        struct Row {
            anim::ModelClipInfo info;
            bool measured = false;
            anim::ClipReport report;
        };
        struct Stamp {
            int64_t time = 0;
            uintmax_t size = 0;
            bool exists = false;
            bool operator==(const Stamp& other) const { return time == other.time && size == other.size && exists == other.exists; }
        };

        Project* project;
        std::filesystem::path file;
        std::string model;
        std::string clipsKey;                       // the names shown, to see the model's clips change
        std::vector<Row> rows;
        std::set<std::string> selected;
        std::string anchor;                         // Shift-click's start
        anim::ModelClipSettings stored;
        std::string readError;                      // the sidecar does not parse: nothing can be applied
        std::map<std::string, anim::ModelClipSetting> working;
        Stamp modelStamp, sidecarStamp;
        std::string promptedModel;
        std::unique_ptr<anim::ClipRig> rig;
        bool rigRead = false;
        std::string actionError;
        std::vector<std::string> controllers;       // Add to Animator's list (project-relative)

        void show(const std::filesystem::path& newFile, const std::string& newModel, const std::vector<anim::ModelClipInfo>& clips);
        void readStored();
        void measure();
        Stamp stampOf(const std::filesystem::path& path) const;

        const anim::ModelClipSetting* storedSetting(const std::string& clip) const;
        anim::ModelClipSetting workingSetting(const std::string& clip) const;
        ClipWrapMode wrapOf(const std::string& clip) const;
        anim::ClipRootMotion rootMotionOf(const std::string& clip) const;
        std::vector<std::string> selectedInOrder() const;
        std::vector<std::string> suggestedLoops() const;

        void drawTable();
        void drawSettings();
        void drawActions();
        void apply();
        void revert();
        void extractSelected();
        void addSelectedToAnimator(const std::string& controller);
    };

}
