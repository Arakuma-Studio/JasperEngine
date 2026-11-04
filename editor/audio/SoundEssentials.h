// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "AudioAnalysis.h"
#include "SoundManifest.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // The 40 CC0 essentials: Kenney clips compiled into the editor with their manifest
    // (tools/sound-library/embed_essentials.py), reached as `essentials:<id>` and imported by
    // Assets > Import Sound Essentials. Read-only data, loaded on first use, safe on any thread.
    class SoundEssentials {
    public:
        static constexpr const char* PREFIX = "essentials:";
        // the menu command's folder, under the assets directory
        static constexpr const char* IMPORT_FOLDER = "sounds/essentials";

        // Every clip, sorted by id
        static const std::vector<SoundManifestClip>& all();
        static const SoundManifestClip* find(const std::string& id);
        // A clip's embedded bytes
        static std::string bytes(const SoundManifestClip& clip);
        // The groups (the part of the ids before the slash), sorted
        static std::vector<std::string> groups();

        static bool isReference(const std::string& text);
        // "essentials:<id>", "essentials:<group>/*" or "essentials:*". An unknown id or group fails
        // with the list of groups.
        static bool match(const std::string& reference, std::vector<const SoundManifestClip*>& out, std::string& error);
        // A clip's type, manifest kind, tags and licence, as the analysis labels it
        static AudioAnalysis::Labels labels(const SoundManifestClip& clip);
    };

}
