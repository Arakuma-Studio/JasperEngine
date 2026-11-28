// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "save/SaveGameStore.h"

#include <algorithm>

namespace jasperengine::editor {

    // Project Settings > Player > Saved Data (docs/saved-data.md, docs/specs/savegame.md R34):
    // project.yaml `saveGame:` (only the values away from the defaults), game.json `saveGame`, the
    // generated init of compiled games and the editor's Play (its next write) through
    // savegame::configure. Described by its reflection table (editor/reflect/SaveGameReflect.cpp).
    struct SaveGameSettings {
        int backups = 2;
        int maxSlotMiB = 8;

        bool operator==(const SaveGameSettings& other) const { return backups == other.backups && maxSlotMiB == other.maxSlotMiB; }
        bool operator!=(const SaveGameSettings& other) const { return !(*this == other); }

        void clamp(){
            backups = std::clamp(backups, 0, 9);
            maxSlotMiB = std::clamp(maxSlotMiB, 1, 64);
        }

        savegame::SaveSettings engineSettings() const {
            savegame::SaveSettings out;
            out.backups = backups;
            out.maxSlotMiB = maxSlotMiB;
            return out;
        }
    };

}
