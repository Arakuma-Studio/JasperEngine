// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// An Audio Source's own effects in the editor (docs/specs/S97-audio-gaps.md REQ-5, REQ-38,
// REQ-42): Unity's filter components as Add Component entries, the undoable add, and the checks
// set_properties makes of an effect list.
#pragma once

#include "ecs/Entity.h"
#include "json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {
    struct AudioSourceComponent;
}

namespace jasperengine::editor {

    class Command;
    class Project;

    namespace SourceEffectEdit {

        // Unity's Audio Low Pass Filter and its siblings: each adds its effect to the GameObject's
        // Audio Source (an Audio Source first when it has none)
        struct FilterAlias {
            const char* name;         // "Audio Low Pass Filter"
            const char* effect;       // "lowpass"
            const char* description;
        };
        const std::vector<FilterAlias>& filterAliases();
        // by its name, any case, with or without spaces ("AudioLowPassFilter"); null when none
        const FilterAlias* findFilterAlias(const std::string& name);

        // One undo step: `effect` appended to each GameObject's Audio Source, an Audio Source added
        // first where there is none. Null with `why` when a type is not a source's or a source
        // already holds the most effects.
        Command* addEffectCommand(Project* project, uint32_t sceneId, const std::vector<Entity>& entities,
                                  const std::string& effect, std::string& why);

        // A set_properties change of an Audio Source's effects ("effects", "effects[N]",
        // "effects[N].type", "effects[N].params", op set or insert) on a source holding `current`:
        // false with an error that lists the valid types or parameters. Other properties pass.
        bool checkChange(const AudioSourceComponent& current, const std::string& property, const std::string& op,
                         const nlohmann::json& value, std::string& error);
    }

}
