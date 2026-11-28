// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef InputReplay_h
#define InputReplay_h

// A replay run by the engine's frame loop (docs/specs/input-recorder-and-input-scripts.md R16,
// R18, R27, R28): a ScriptPlayer on the ScriptRunner, with a hold on real input (blocked, or
// ending an attract loop at the first real press), looping, and the end reported once.

#include "Export.h"
#include "input/InputScript.h"

#include <functional>
#include <memory>
#include <vector>

namespace jasperengine::input {

    class Replay;

    struct ReplayOptions {
        bool loop = false;
        bool blockRealInput = true;     // device events the interpreter did not make do not reach the game
        bool stopOnRealInput = false;   // the first real key, button or touch press ends it (and reaches the game)
        bool stopOnDivergence = false;  // a checkpoint that differs fails it
        PlayerHost host;
        PlayOptions play;
        std::function<void(Replay&)> onFinished;
    };

    // a runtime player's host (Lua, a player's --input-script, Test.input): targets resolve in the
    // running UI Documents when the htmlui module is in
    JASPERENGINE_API PlayerHost runtimePlayerHost();

    class JASPERENGINE_API Replay : public std::enable_shared_from_this<Replay> {
    public:
        // starts it from the next frame
        static std::shared_ptr<Replay> start(std::shared_ptr<const ParsedScript> script, ReplayOptions options);
        static std::vector<std::shared_ptr<Replay>> running();
        static void stopAll();

        ~Replay();
        // done, failed or stopped
        bool isDone() const { return done; }
        bool isStopped() const { return stopped; }
        void stop();
        // the frame it is on (-1 once done)
        int64_t frame() const;
        const ParsedScript& script() const { return *script_; }
        // its player's report (the last loop's)
        const ReplayReport& report() const;
        std::string errorText() const;

    private:
        explicit Replay(std::shared_ptr<const ParsedScript> script, ReplayOptions options);
        void runOnce();
        void finish();

        std::shared_ptr<const ParsedScript> script_;
        ReplayOptions options;
        std::shared_ptr<ScriptPlayer> player;
        uint64_t gate = 0;
        bool done = false;
        bool stopped = false;
    };

}

#endif
