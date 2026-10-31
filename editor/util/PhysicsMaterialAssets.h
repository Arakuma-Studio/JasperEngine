// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor side of .physicsmaterial assets (engine/core/physics/PhysicsMaterial.h): read,
// create and edit them as undoable file writes (the project command history), shared by the
// Project window's Create menu, the Inspector's asset view and the MCP edit_physics_material tool.
// Every write invalidates the PhysicsMaterialPool entry, so a playing game picks the new values up
// at its next step.

#pragma once

#include "physics/PhysicsMaterial.h"

#include <filesystem>
#include <string>

namespace jasperengine::editor {

    class Project;

    class PhysicsMaterialAssets {
    public:
        static bool isPhysicsMaterialFile(const std::string& path);

        static bool read(const std::filesystem::path& file, PhysicsMaterialData& data, std::string& error);
        // canonical text, one undo step; mergeKey (drags) merges with the previous write of the same key
        static bool write(Project* project, const std::filesystem::path& file, const PhysicsMaterialData& data,
                          std::string& error, const std::string& mergeKey = "");
        // "<dir>/<baseName>.physicsmaterial", " 1", " 2"... when taken; a new GUID
        static std::filesystem::path create(Project* project, const std::filesystem::path& directory, const std::string& baseName,
                                            const PhysicsMaterialData& data, std::string& error);
    };

}
