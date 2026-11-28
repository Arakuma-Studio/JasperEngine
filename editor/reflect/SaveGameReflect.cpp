// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Project Settings > Player > Saved Data (docs/specs/savegame.md R34, R44) on a reflection table:
// the project file's saveGame block (only the values away from their defaults, so older projects
// re-save unchanged), its schema in jasper://reflection, and the labels, ranges and help of the
// settings rows.

#include "reflect/Reflect.h"
#include "reflect/Field.h"
#include "reflect/TypeOps.h"

#include "SaveGameSettings.h"

namespace jasperengine::editor::reflect {

namespace {

    constexpr FieldDesc kSaveGameFields[] = {
        JE_FIELD(SaveGameSettings, backups, .flags = OmitIfDefault | Clamped, .min = 0.0f, .max = 9.0f, .label = "Backups Kept",
                 .tooltip = "Older generations of each SaveGame slot kept beside the newest; a damaged save is recovered from the "
                            "newest valid one. 0 keeps only the newest file."),
        JE_FIELD(SaveGameSettings, maxSlotMiB, .flags = OmitIfDefault | Clamped, .min = 1.0f, .max = 64.0f, .label = "Max Slot Size (MiB)",
                 .tooltip = "The largest a SaveGame slot may be encoded; a bigger write fails with too_large naming its largest "
                            "namespaces. Play applies a change at its next write."),
    };

}

JE_REFLECT_STRUCT_TRAITS(SaveGameSettings, kSaveGameStruct);
const StructDesc kSaveGameStruct = JE_STRUCT_DESC(SaveGameSettings, "SaveGameSettings", kSaveGameFields);

const StructDesc& saveGameSettingsStruct() {
    return kSaveGameStruct;
}

}
