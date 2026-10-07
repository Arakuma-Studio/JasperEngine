// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMTIMER_H
#define ANIMTIMER_H

// Timer of the anime API, ported from Anime.js 4.5.0 (timer/timer.js, core/clock.js,
// core/render.js): playback of a duration with delay, loops, alternate, reversed,
// playbackRate, playbackEase and frameRate, callbacks and playback control. JSAnimation
// (AnimAnimation) and Timeline (AnimTimeline) derive from it. Times are milliseconds of the
// clock the timer runs on (AnimEngine: the UI clock, or the scaled game clock).

#include "Export.h"
#include "AnimEasing.h"
#include "AnimParams.h"

#include <functional>
#include <memory>
#include <string>

namespace jasperengine {

    class AnimTimeline;

    enum class AnimClock { UI, GAME };
    // Anime's tickModes
    enum class AnimTickMode { NONE = 0, AUTO = 1, FORCE = 2 };

    class JASPERENGINE_API AnimTimer : public std::enable_shared_from_this<AnimTimer> {
    public:
        enum class Kind { TIMER, ANIMATION, TIMELINE };
        using Callback = std::function<void(AnimTimer&)>;

        // anime.createTimer(params): a started timer (unless autoplay = false)
        static std::shared_ptr<AnimTimer> create(const AnimParam& params = AnimParam::object());

        AnimTimer(const AnimTimer&) = delete;
        AnimTimer& operator=(const AnimTimer&) = delete;
        virtual ~AnimTimer();

        Kind getKind() const { return kind; }
        AnimClock getClock() const { return clock; }

        // --- properties ---
        const AnimParam& getId() const { return id; }
        double getCurrentTime() const;                 // ms, clamped to -delay..duration
        void setCurrentTime(double time);
        double getIterationCurrentTime() const;
        void setIterationCurrentTime(double time);
        double getProgress() const;                    // 0..1 of the whole duration
        void setProgress(double progress);
        double getIterationProgress() const;
        void setIterationProgress(double progress);
        int getCurrentIteration() const;
        void setCurrentIteration(int iteration);
        bool isReversed() const;
        void setReversed(bool reversed);
        double getSpeed() const { return speed; }      // playbackRate
        void setSpeed(double playbackRate);
        double getFps() const { return fps; }          // frameRate
        void setFps(double frameRate);
        bool isCancelled() const { return cancelled != 0; }
        void setCancelled(bool cancelled);
        double getDuration() const { return duration; }
        double getIterationDuration() const { return iterationDuration; }
        double getIterationCount() const { return iterationCount; }   // infinity for endless loops
        double getDelay() const { return delay; }
        double getDeltaTime() const { return deltaTime; }
        bool isPaused() const { return paused; }
        bool hasBegun() const { return began; }
        bool isCompleted() const { return completed; }
        bool isBackwards() const { return backwards; }
        AnimTimeline* getParent() const { return parent; }

        // --- control (each returns this) ---
        AnimTimer& reset(bool softReset = false);
        AnimTimer& init(bool internalRender = false);
        AnimTimer& resetTime();
        AnimTimer& pause();
        AnimTimer& resume();
        AnimTimer& restart();
        AnimTimer& seek(double time, bool muteCallbacks = false, bool internalRender = false);
        AnimTimer& alternate();
        AnimTimer& play();
        AnimTimer& reverse();
        AnimTimer& cancel();
        virtual AnimTimer& stretch(double newDuration);
        // seeks to 0, cancels, and restores the animated values (animations, timelines)
        virtual AnimTimer& revert();
        // seeks to the end, calls onComplete (unless muted) and cancels
        AnimTimer& complete(bool muteCallbacks = false);
        // recomputes function-based values (animations, timelines)
        virtual AnimTimer& refresh();
        // fn(this) once completed (now when already completed); Anime's then() without the Promise
        AnimTimer& then(Callback fn);

        // callbacks (Anime's onBegin ...); setting one replaces it
        Callback onBegin, onBeforeUpdate, onUpdate, onLoop, onPause, onComplete, onRender;

        // ---- internal state, named after Anime's fields; read through the accessors above ----
        Kind kind = Kind::TIMER;
        AnimClock clock = AnimClock::UI;
        AnimParam id;
        AnimTimeline* parent = nullptr;
        double duration = 0;
        bool backwards = false;
        bool paused = true;
        bool began = false;
        bool completed = false;
        double iterationDuration = 0;
        double iterationCount = 1;
        bool autoplay = true;
        double offset = 0;              // _offset
        double delay = 0;               // _delay
        double loopDelay = 0;           // _loopDelay
        double iterationTime = 0;       // _iterationTime
        double currentIteration = 0;    // _currentIteration (integral)
        Callback resolve;               // _resolve (then)
        bool running = false;           // _running: in the engine's list
        int reversedFlag = 0;           // _reversed
        int reverseInit = 0;            // _reverse
        int cancelled = 0;              // _cancelled
        bool alternateFlag = false;     // _alternate
        double priority = 1;            // _priority
        EaseFn ease;                    // _ease: playbackEase (animations, timelines)
        bool hasChildren = false;       // _hasChildren: a timeline with children
        // Clock
        double deltaTime = 0;
        double absoluteTime = 0;        // _currentTime
        double lastTickTime = 0;
        double startTime = 0;
        double lastTime = 0;
        double frameDuration = 0;
        double fps = 0;
        double speed = 1;
        // the engine's list, or the timeline's children list
        AnimTimer* prev = nullptr;
        AnimTimer* next = nullptr;
        // keeps a running top-level timer alive while it is in the engine's list
        std::shared_ptr<AnimTimer> engineRef;

        AnimTickMode requestTick(double time);
        double computeDeltaTime(double time);
        // whether the timer only animates targets that are gone (AnimEngine cancels it)
        virtual bool hasOnlyInvalidTargets() const { return false; }

    protected:
        AnimTimer(Kind kind, const AnimParam& params, AnimTimeline* parent, double parentPosition);

        friend class AnimEngine;
        friend class AnimTimeline;
    };

    // Anime's render() and tick() (core/render.js), on any timer
    namespace AnimRender {
        int render(AnimTimer& tickable, double time, int muteCallbacks, int internalRender, AnimTickMode tickMode);
        void tick(AnimTimer& tickable, double time, int muteCallbacks, int internalRender, AnimTickMode tickMode);
    }

    // Anime's globals.defaults, and a timeline's defaults merged over them
    class JASPERENGINE_API AnimDefaults {
    public:
        static const AnimParam& global();
        static const AnimParam& of(const AnimTimeline* parent);
    };

    // the callback/property keys of the parameters (Anime's globals.defaults + the engine's own)
    JASPERENGINE_API bool isAnimParameterKey(const std::string& key);
    // "ui" / "game" (nullopt-like false + an error for anything else)
    JASPERENGINE_API bool parseAnimClock(const AnimParam& value, AnimClock& clock, std::string* error);

}

#endif //ANIMTIMER_H
