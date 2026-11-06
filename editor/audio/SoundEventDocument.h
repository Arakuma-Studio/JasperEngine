// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "audio/SoundEventIO.h"

#include "json.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // A .soundevent asset as the editor edits it (docs/specs/S37-sound-events.md R3, R16, R20): the
    // Inspector view and the MCP edit_sound_event tool. Every change is one step of the project
    // history (WriteFilesCmd: undo puts the file back). The file holds no GUID: the asset database
    // gives it its .meta when it appears.
    class SoundEventDocument {
    public:
        // a .soundevent file -> def (false + error: missing, not JSON, a newer version)
        static bool read(const std::filesystem::path& file, SoundEventDef& def, std::string* error);
        // The def written as one undo step named `undoName` (validated first: false + error,
        // nothing written). A non-empty merge key merges it with the step before it that has the
        // same key (a slider drag: one step).
        static bool commit(Project* project, const std::filesystem::path& file, const SoundEventDef& def, const std::string& undoName,
                           const std::string& mergeKey, std::string* error);

        // The clips by GUID: the paths of those named in `moved` (GUID -> absolute path) are
        // rewritten in every event of the project that holds them. Follows a clip's move or
        // rename, and the undo of one, so its event always names it. The files it rewrote.
        static std::vector<std::filesystem::path> relinkGuids(Project* project, const std::map<std::string, std::filesystem::path>& moved);

        // The MCP values on a def (R1's keys; `clips` items take a path, a GUID or {clip, weight, ...};
        // `output_group` a group by name; null resets a key). False + error naming the key.
        static bool applyValues(Project* project, SoundEventDef& def, const nlohmann::json& values, std::string* error);
        // the values as MCP reports them: R1's keys with `output_group`'s name
        static nlohmann::json describe(const SoundEventDef& def);
        // What a look at the def and its files finds without decoding: clips missing from the
        // project, a group not in the mixer
        static std::vector<std::string> problems(Project* project, const SoundEventDef& def);
        // A clip by its path or GUID as a reference: its assets-relative path and its GUID ("" with error: an unknown GUID)
        static AssetRef clipRef(Project* project, const std::string& pathOrGuid, std::string* error);
    };

}
