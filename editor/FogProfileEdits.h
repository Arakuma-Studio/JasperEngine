// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The undoable Fog Profile operations (docs/specs/vapor.md R23, R32, R33, R43), one function each,
// for MCP and anything else that edits profiles without the Inspector: a profile file's edit
// (creating it, with a GUID that stays the same on redo), linking a Fog or a Fog Zone (copying a
// built-in into the project on first use, in the same step), unlinking a Fog (its look kept),
// and who uses a profile. Each returns one command for the caller's history: the file edit for
// the project's, a link or an unlink for its scene's.

#include "FogProfileAssets.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Command;
    class Project;
}

namespace jasperengine::editor::fogprofiles {

    // An edit of the profile file at `projectRelativePath`: `after` written canonically, `before`
    // put back on undo. `before` null creates the file (undo deletes it and its .meta). Every user
    // shows it on the next frame. Null when `after` is what the file holds.
    Command* buildProfileEdit(Project& project, const std::string& projectRelativePath, const FogProfileData* before,
                              const FogProfileData& after);

    struct FogLinkPlan {
        ResolvedFogProfile profile;
        bool zone = false;          // the entity's Fog Zone, else its Fog
        bool typeToHeight = false;  // the Fog becomes Type Height (the profile sets height fog fields)
    };

    // Links the Fog of `entity` (else its Fog Zone) to the profile `name` (resolveProfileName:
    // "builtin:<id>", a bare built-in id, or a .fogprofile path). A built-in without a project copy
    // gets one in the same step; a Fog not of Type Height becomes Height when the profile sets
    // height fog fields. Null with `error` when it cannot; null with `error` empty when it is
    // already linked to it.
    Command* buildLink(Project& project, uint32_t sceneId, Entity entity, const std::string& name, FogLinkPlan& plan, std::string& error);

    // Unlinks the Fog of `entity`: its look with the profile applied becomes its own fields and
    // the link is cleared, so it looks the same. Null with `error` when it cannot; null with
    // `error` empty when it has no profile.
    Command* buildUnlink(Project& project, uint32_t sceneId, Entity entity, std::string& error);

    // Everything that references a profile: its Fogs and Fog Zones in open scenes, and the
    // project files that are not open (the asset reference index)
    struct FogProfileUsers {
        std::vector<FogProfileUser> open;
        std::vector<std::string> closedFiles;   // project-relative
        size_t count() const { return open.size() + closedFiles.size(); }
    };
    FogProfileUsers profileUsers(Project& project, const std::string& assetPath);

    // a profile key as Fog.profile stores it (assets-relative) as a project-relative path; keys
    // that are no path ("builtin:<id>", "runtime:<n>", a GUID) as they are
    std::string projectPathOfKey(const Project& project, const std::string& key);

}
