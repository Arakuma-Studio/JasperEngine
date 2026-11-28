// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUICLOCK_H
#define HTMLUICLOCK_H

#include "Export.h"

#include <optional>

namespace Rml {
    class Element;
}

namespace jasperengine {

    // A UI Document's Update Mode (Unity's Animator.updateMode names): the clock its CSS
    // animations and transitions, anime, effects, Lottie and text effects run on
    enum class UIDocumentUpdateMode { UNSCALED_TIME, GAME_TIME };

    // Seconds of UI time: what RmlUi transitions/animations (RmlSystem::GetElapsedTime), Lottie,
    // anime and phase-5 effects see. Advanced by the unscaled frame delta: Engine::timeScale
    // does not slow it (a pause menu animates while the game is frozen), Engine::setFixedDeltatime
    // makes it deterministic (MCP play fixed_fps), and it does not advance while a host holds
    // the simulation (MCP auto_pause). Main thread only.
    //
    // A Game Time document's clock (S93 R35) is a second accumulator, of the scaled game delta:
    // it follows Engine::timeScale and stops while the game is paused or held, while the game
    // events are paused (the editor outside Play and in its Pause), and while a fixed time is
    // set (both clocks are pinned then).
    class JASPERENGINE_API HtmlUIClock {
    public:
        static double now();                                     // fixed time if set, else the accumulated time
        // a clock by mode: UNSCALED_TIME is now(); GAME_TIME the game-time accumulator
        static double now(UIDocumentUpdateMode mode);
        // the clock of the document the element is in (UNSCALED_TIME outside a document)
        static double now(Rml::Element* element);
        static UIDocumentUpdateMode modeOf(Rml::Element* element);
        // the RmlUi animation clock id of a mode (ElementDocument::SetAnimationClock: 0 is RmlUi's
        // elapsed time, what Unscaled Time keeps) and back
        static int animationClockOf(UIDocumentUpdateMode mode);
        static UIDocumentUpdateMode modeOfAnimationClock(int clock);
        // once per engine frame, by Engine::systemDraw before the scene updates (0 while held
        // or paused); negative or non-finite deltas are ignored. Does nothing while a fixed
        // time is set
        static void advance(double unscaledSeconds);
        // once per engine frame the game clocks run, the scaled delta (Engine::getDeltatime);
        // ignored as advance() is
        static void advanceGame(double scaledSeconds);
        // tests, goldens, tools; nullopt = run. Clearing it resumes the accumulated time from
        // the fixed value, so now() never jumps on release; the game clock holds meanwhile.
        // Negative or non-finite: warning, ignored
        static void setFixedTime(std::optional<double> seconds);
        static std::optional<double> getFixedTime();
        // The Game Time clock pinned too (the recorder's frames, render_ui_frames' samples):
        // now(GAME_TIME) reads it while set, whatever setFixedTime holds. Clearing it resumes the
        // game-time accumulator from the pinned value. Negative or non-finite: warning, ignored
        static void setFixedGameTime(std::optional<double> seconds);
        static std::optional<double> getFixedGameTime();
        static void reset();                                     // tests: time 0, no fixed time
    };

}

#endif //HTMLUICLOCK_H
