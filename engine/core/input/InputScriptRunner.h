// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef InputScriptRunner_h
#define InputScriptRunner_h

// The engine's frame loop runs input scripts (input/InputScript.h) through this: the loop needs
// only these entry points, not the format.

#include "Export.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace jasperengine::input {

    class ScriptPlayer;

    // Players run by the engine's frame loop: before the input latch of each running frame, and
    // after its update (Engine::systemDraw). A paused app's frames count for scripts (a resume step
    // runs), not for recordings (their stamps are update frames).
    class JASPERENGINE_API ScriptRunner {
    public:
        // idle: one relaxed load
        static bool isActive();
        // runs it from the next frame; `onEnded` is called once when it ends (done or failed)
        static void start(std::shared_ptr<ScriptPlayer> player, std::function<void(ScriptPlayer&)> onEnded = nullptr);
        // stops it (its held input released, onEnded not called); false when it was not running
        static bool stop(const ScriptPlayer* player);
        static void stopAll();
        static std::vector<std::shared_ptr<ScriptPlayer>> running();
        // the frames the runner gave it so far (its frameIndex), -1 when not running
        static int64_t frameOf(const ScriptPlayer* player);

        // Engine::systemDraw: `running` false for a frame of a paused app
        static void beforeFrame(bool running);
        static void afterUpdate();
    };

}

#endif
