// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMENGINE_H
#define ANIMENGINE_H

// The one process-wide engine of the anime API (Anime.js 4.5.0 engine/engine.js): ticks every
// running top-level timer, animation and timeline. UIDocumentSystem::update calls
// tick(HtmlUIClock::now()) every frame before the RmlUi context update, so anime runs on the
// UI clock like CSS animations (Engine::timeScale does not slow it); timers created with
// clock = "game" run on the scaled game time instead (Engine::getGameTime deltas).
// reset() drops everything (Engine::systemShutdown, the editor's play stop).

#include "Export.h"
#include "AnimTimer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace jasperengine {

    class AnimAnimation;
    class AnimTarget;
    struct AnimTween;

    class JASPERENGINE_API AnimEngine {
    public:
        static AnimEngine& instance();

        // Private document previews seek their own timers while updating RmlUi. Their clock
        // must not tick animations belonging to the editor's other documents.
        class JASPERENGINE_API ScopedTickPause {
        public:
            explicit ScopedTickPause(std::vector<std::shared_ptr<AnimTimer>>* created = nullptr);
            ~ScopedTickPause();
            ScopedTickPause(const ScopedTickPause&) = delete;
            ScopedTickPause& operator=(const ScopedTickPause&) = delete;
        private:
            std::vector<std::shared_ptr<AnimTimer>>* previousCollector;
        };
        // the one engine; not copyable (it owns its tweens: MSVC instantiates the copies of
        // an exported class)
        AnimEngine(const AnimEngine&) = delete;
        AnimEngine& operator=(const AnimEngine&) = delete;

        // once per frame (idempotent while the times do not change): the UI clock time in
        // seconds; the game clock follows Engine::getGameTime()
        void tick(double uiTimeSeconds);
        // both clocks explicitly, in seconds of game time accumulated (not deltas); tests
        void tick(double uiTimeSeconds, double gameTimeSeconds);
        // milliseconds, as Anime counts (reference tests)
        void tickMilliseconds(double uiTimeMs, double gameTimeMs);

        // A recorder's first frame starts after asynchronous scene loading. Align the input
        // clocks without treating that load as animation time; existing progress is preserved.
        void alignClockSources(double uiTimeSeconds, double gameTimeSeconds);

        // cancels and drops every timer, the additive state and the element targets' state,
        // and calls the reset listeners (the Lua binding frees its registry refs there)
        void reset();

        // ms of a clock: the time of the last tick
        double now(AnimClock clock) const;
        // running top-level timers
        size_t getActiveCount() const;
        // every timer object alive (tests: leaks)
        static size_t getLiveTimerCount();

        // reset() listeners; returns an id for removeResetListener
        uint64_t addResetListener(std::function<void()> listener);
        void removeResetListener(uint64_t id);

        // ---- internal (AnimTimer, AnimComposer) ----
        void add(AnimTimer& timer);          // resume(): into the running list
        static void registerAnimation(AnimAnimation* animation, bool alive);
        AnimTimer* head = nullptr;
        AnimTimer* tail = nullptr;
        bool hasChildren = false;
        double speed = 1;
        double fps = 240;
        // additive (composition: blend) lookup tweens, as Anime's additive animation
        AnimTween* additiveHead = nullptr;
        AnimTween* additiveTail = nullptr;
        std::vector<AnimTarget*> additiveTargets;
        std::vector<std::unique_ptr<AnimTween>> additiveTweens;
        static void countTimer(int delta);

    private:
        AnimEngine() = default;
        void update();

        double uiNow = 0;
        double gameNow = 0;
        double lastGameSource = 0;
        bool ticked = false;
        bool updating = false;
        unsigned int tickPauseDepth = 0;
        std::vector<std::shared_ptr<AnimTimer>>* timerCollector = nullptr;
        uint64_t nextListenerId = 1;
        std::vector<std::pair<uint64_t, std::function<void()>>> resetListeners;
    };

}

#endif //ANIMENGINE_H
