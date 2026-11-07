// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // Sounds dropped from the Sound Browser or the Project window (docs/specs/S37-sound-events.md R18):
    // project files as `resource_files`, built-in clips as `builtin_sounds`, imported first through
    // S36's planner. Each drop is one undo step.
    class SoundDrop {
    public:
        // a payload that carries a clip or a Sound Event
        static bool accepts(const ImGuiPayload* payload);
        // the payload's sounds as assets-relative paths (built-ins imported first); false + error
        static bool resolve(Project* project, const ImGuiPayload* payload, std::vector<std::string>& paths, std::string& error);

        // A new GameObject with an AudioSource playing the sound, in the scene (at `position` when 3D)
        static bool createSource(Project* project, uint32_t sceneId, const std::string& path, bool has3D, const Vector3& position, std::string& error);
        // Onto a GameObject with a Button: its AudioSource (added when it has none) plays the sound on
        // UI clicks. Anything else without a source: as createSource under it. False when it is neither.
        static bool assignToButton(Project* project, uint32_t sceneId, Entity entity, const std::string& path, std::string& error);
    };

}
