// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUICLOCK_H
#define HTMLUICLOCK_H

#include "Export.h"

#include <optional>

namespace jasperengine {

    // Seconds of UI time: what RmlUi transitions/animations (RmlSystem::GetElapsedTime), Lottie,
    // anime and phase-5 effects see. Advanced by the unscaled frame delta: Engine::timeScale
    // does not slow it (a pause menu animates while the game is frozen), Engine::setFixedDeltatime
    // makes it deterministic (MCP play fixed_fps), and it does not advance while a host holds
    // the simulation (MCP auto_pause). Main thread only.
    class JASPERENGINE_API HtmlUIClock {
    public:
        static double now();                                     // fixed time if set, else the accumulated time
        // once per engine frame, by Engine::systemDraw before the scene updates (0 while held
        // or paused); negative or non-finite deltas are ignored. Does nothing while a fixed
        // time is set
        static void advance(double unscaledSeconds);
        // tests, goldens, tools; nullopt = run. Clearing it resumes the accumulated time from
        // the fixed value, so now() never jumps on release. Negative or non-finite: warning, ignored
        static void setFixedTime(std::optional<double> seconds);
        static std::optional<double> getFixedTime();
        static void reset();                                     // tests: time 0, no fixed time
    };

}

#endif //HTMLUICLOCK_H
