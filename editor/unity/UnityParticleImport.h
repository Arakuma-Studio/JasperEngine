// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Unity ParticleSystem importer (docs/specs/S20-particles.md, Upstream absorption): a
// `.prefab` or `.unity` file of a Unity project becomes engine prefabs (one per selected root
// GameObject: its systems as S20 Particle Systems with inline effects, child systems as
// children; each renderer's material in its Renderer module: texture, tint, tiling, soft
// particles, blend), copied textures, and a report of what was imported, approximated or not
// supported. It never writes into the Unity project. Pure enough to run without the GUI
// (the CLI `import-unity-particles`, MCP `import_unity_particles`, Assets > Import Unity
// Particle System...).

#include "json.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::unity {

    struct ImportOptions {
        std::filesystem::path file;            // the .prefab / .unity
        std::filesystem::path engineProject;   // the engine project the files go into
        std::string object;                    // a GameObject path ("Player/Camera/EndParticles"); empty: every root with systems
        std::string dest = "assets/fx";        // project-relative output folder
        std::filesystem::path unityProject;    // empty: the nearest ancestor with Assets/ and ProjectSettings/
        bool dryRun = false;                   // report only, no file written
        bool overwrite = false;                // false: a numbered name next to an existing prefab, existing materials and textures reused
        bool writeFiles = true;                // false: the files are only returned (ImportResult::files: the editor writes them as one undo step)
    };

    struct ImportNote {
        std::string code;      // module-unsupported, shape-unsupported, ui-particle, shader-approximated, ...
        std::string message;
    };

    struct ImportResult {
        bool ok = false;
        std::string error;
        // project-relative paths of the prefabs written (or that would be, dry run)
        std::vector<std::string> prefabs;
        // the report (spec 6.10.9): {source, prefabs, systems: [{path, engine_entity, modules,
        // notes}], materials, textures, warnings}
        nlohmann::json report;
        // any note or warning (the CLI exits 2)
        bool hasWarnings = false;
        // project-relative paths of every file written
        std::vector<std::string> written;
        // the report's project-relative path (empty in a dry run)
        std::string reportFile;
        // every file of the import (absolute path, contents), written or not
        std::vector<std::pair<std::filesystem::path, std::string>> files;
    };

    ImportResult importParticles(const ImportOptions& options);

    // The editor's import (Assets > Import Unity Particle System..., MCP import_unity_particles):
    // into `project` (options.engineProject is set to it), its files written as one undo step of
    // the project history (ProjectFileWrites::writeUndoable, beneath the project) and the
    // Project window refreshed; a dry run writes nothing. result.ok false + result.error on failure.
    ImportResult importParticlesIntoProject(jasperengine::editor::Project* project, ImportOptions options);

    // the shape rotation of a Unity shape (Euler degrees, Unity's Z-X-Y order) in the engine's
    // shape frame (S20's shape rotation: Euler degrees applied Z, then X, then Y); `edge`:
    // Unity's Edge, which emits along its +Y
    void convertShapeRotation(const float unityEuler[3], bool edge, float engineEuler[3]);

}
