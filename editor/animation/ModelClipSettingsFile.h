// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (S74 R27, R48, R53)
// - Purpose: the editor's reading and writing of a model's "<model>.clips" sidecar
//   (anim/ModelClipSettings.h), shared by the Inspector's model clip section and edit_animation.
// - A write is one project undo step (a WriteFilesCmd: undo puts the previous bytes back, or
//   removes a file it made); after it the files-written refresh reloads the model's clip
//   documents and players (AnimationClipDocument::filesChanged). Unknown keys survive.
// - A sidecar that does not parse is never written over: its settings would be lost.
// - Thread-safety: main thread.

#pragma once

#include "animation/ModelClipSettings.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor{

    class Project;

    // the stored settings of `model` (an absolute model file path): none when it has no sidecar;
    // false + `error` when its sidecar does not parse. `text` (when given) gets the file's bytes
    bool readModelClipSettingsFile(const std::filesystem::path& model, anim::ModelClipSettings& out, std::string& error,
                                   std::string* text = nullptr);

    // writes `settings` beside `model` as one project undo step named `undoName`; `changed` false
    // (and no step) when the file already holds those bytes. False + `error` when it can't be written
    bool writeModelClipSettingsFile(Project* project, const std::filesystem::path& model, const anim::ModelClipSettings& settings,
                                    const std::string& undoName, std::string& error, bool* changed = nullptr);

}
