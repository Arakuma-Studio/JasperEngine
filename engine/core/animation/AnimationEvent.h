// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.5, 9.R16/R23)
// - Purpose: one clip event as it reaches the game: what an Animation Player's onEvent
//   subscribers receive and what is spread into the Lua script method of the same name.
// - Owner: T0 (this header, the accessors); T1b fires them (ActionSystem players) and binds the
//   type to Lua (args pushed as a 1-based table).
// - Thread-safety: a plain value type.
// - Invariants: functionName is a Lua identifier (the loader drops other events), args holds at
//   most MAX_ANIMATION_EVENT_ARGS JSON scalars; state is the Animation Player state (clip entry
//   name) that crossed the event, time its time in the clip (seconds).

#ifndef ANIMATION_EVENT_H
#define ANIMATION_EVENT_H

#include "Export.h"

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace jasperengine{

    // the most args one event carries (.anim `events[].args`)
    constexpr size_t MAX_ANIMATION_EVENT_ARGS = 8;

    // one event argument: a JSON number, string or bool
    using AnimationEventArg = std::variant<double, std::string, bool>;

    struct JASPERENGINE_API AnimationEvent{
        std::string functionName;
        std::vector<AnimationEventArg> args;
        std::string state;
        float time = 0.0f;

        // Lua and C++ script friendly reads (0-based index; out of range: 0 / "" / false)
        size_t getArgCount() const;
        // "number", "string", "boolean"; "" out of range
        std::string getArgType(size_t index) const;
        double getNumberArg(size_t index) const;
        std::string getStringArg(size_t index) const;
        bool getBoolArg(size_t index) const;
    };

}

#endif //ANIMATION_EVENT_H
