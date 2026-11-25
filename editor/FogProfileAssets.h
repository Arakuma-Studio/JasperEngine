// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The project's Fog Profiles (docs/specs/S101-vapor.md R1-R6, R23, R31-R33) for the inspectors, the
// commands and MCP: listing them, the one way a profile is named (a .fogprofile path, or
// "builtin:<id>", which resolves through the project's copy of that built-in,
// assets/FogProfiles/<id>.fogprofile, made on first use and reused afterwards), who uses one,
// and telling the engine that the editor wrote one. What a Fog or Fog Zone stores is always the
// assets-relative path of a project file; the engine's built-ins are read from the engine.

#include "ecs/Entity.h"
#include "render/FogProfile.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::fogprofiles {

    struct ProjectFogProfile {
        std::string path;        // project-relative ("assets/FogProfiles/night.fogprofile")
        std::string assetPath;   // assets-relative: what Fog.profile and FogZone.profile store
        FogProfileData data;
    };

    // every readable .fogprofile under the assets directory, by path (hidden folders skipped)
    std::vector<ProjectFogProfile> listProjectProfiles(const Project& project);

    // the project's copy of a built-in (FogProfiles/<id>.fogprofile under the assets directory),
    // project-relative; empty when it has none yet
    std::string findBuiltinCopy(const Project& project, const std::string& id);
    // where a built-in's copy goes, project-relative (FogProfiles::builtinCopyPath under assets)
    std::string builtinCopyPath(const Project& project, const std::string& id);

    struct ResolvedFogProfile {
        bool ok = false;
        std::string error;          // what is wrong and the fix (ok false)
        std::string path;           // project-relative
        std::string assetPath;      // assets-relative: store this in the reference
        bool builtin = false;       // named as a built-in
        std::string builtinId;
        // a built-in with no copy yet: write copyText at path (in the same undo step as the
        // link that uses it), then store assetPath. copyText is FogProfileIO::write's.
        bool needsCopy = false;
        std::string copyText;
        FogProfileData data;
    };

    // "builtin:<id>" (a bare built-in id too), or a .fogprofile path (project- or assets-relative)
    ResolvedFogProfile resolveProfileName(const Project& project, const std::string& name);

    // a Fog or Fog Zone of an open scene (or prefab stage) whose profile is that file
    struct FogProfileUser {
        uint32_t sceneId = 0;
        std::string sceneName;
        Entity entity = NULL_ENTITY;
        bool zone = false;          // a Fog Zone, else a Fog
    };
    // `assetPath` assets-relative; by GUID when the reference has one, else by path
    std::vector<FogProfileUser> usersInOpenScenes(Project& project, const std::string& assetPath);
    // the project files (scenes, prefabs) that reference the profile, open or not (the asset
    // reference index: S04), project-relative
    std::vector<std::string> referencingFiles(const Project& project, const std::string& assetPath);

    // The editor wrote a profile file (a command's do or undo, MCP): the engine takes `data` at
    // once (no read back) and every open scene draws again, so every user shows it on the next
    // frame (R5). `assetPath` assets-relative.
    void profileWritten(Project& project, const std::string& assetPath, const FogProfileData& data);
    // the profile file is gone or changed in a way to read again (an undone creation): the next
    // use reads it
    void profileChanged(Project& project, const std::string& assetPath);

    // Everything that references a profile (`assetPath` assets-relative): its Fogs and Fog Zones
    // in open scenes and prefabs, and the project files that are not open (the asset reference
    // index; complete once it is ready)
    struct FogProfileUsers {
        std::vector<FogProfileUser> open;
        std::vector<std::string> closedFiles;   // project-relative
        bool indexReady = false;                // false: closedFiles may miss some
        size_t count() const { return open.size() + closedFiles.size(); }
    };
    FogProfileUsers profileUsers(Project& project, const std::string& assetPath);

    // a profile key as Fog.profile stores it (assets-relative) as a project-relative path; keys
    // that are no path ("builtin:<id>", "runtime:<n>", a GUID) as they are
    std::string projectPathOfKey(const Project& project, const std::string& key);

}
