// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Port of Anime.js 4.5.0 (MIT) timer/timer.js, core/clock.js and core/render.js. The engine
// runs Anime's millisecond mode only (globals.timeScale = 1). Deviation: resume() starts the
// timer at the engine's last tick time without Anime's 12 ms head start (see AnimEngine).

#include "AnimTimer.h"

#include "AnimAnimation.h"
#include "AnimCommon.h"
#include "AnimEngine.h"
#include "AnimProperty.h"
#include "AnimTimeline.h"
#include "Log.h"

#include <cmath>
#include <unordered_set>

using namespace jasperengine;

namespace {

    uint64_t timerId = 0;

    const std::unordered_set<std::string>& parameterKeys(){
        // Anime's globals.defaults keys (isKey() is false for them) + the engine's own
        static const std::unordered_set<std::string> keys = {
            "id", "keyframes", "playbackEase", "playbackRate", "frameRate", "loop", "reversed", "alternate",
            "autoplay", "persist", "duration", "delay", "loopDelay", "ease", "composition", "modifier",
            "onBegin", "onBeforeUpdate", "onUpdate", "onLoop", "onPause", "onComplete", "onRender",
            "clock", "root", "priority",
        };
        return keys;
    }

    const AnimParam& param(const AnimParam& params, const char* key){
        return params.get(key);
    }

    AnimTimer::Callback callbackOf(const AnimParam& params, const char* key, const AnimParam& defaults){
        const AnimParam& value = params.get(key);
        if (value.isCallback())
            return value.asCallback();
        const AnimParam& fallback = defaults.get(key);
        if (fallback.isCallback())
            return fallback.asCallback();
        return nullptr;
    }

    void call(const AnimTimer::Callback& callback, AnimTimer& timer){
        if (callback)
            callback(timer);
    }

    // forEachChildren on a timeline's children (next saved first: the callback may unlink)
    template<typename F>
    void forEachChild(AnimTimeline& timeline, F&& fn, bool reverse = false){
        AnimTimer* child = reverse ? timeline.childTail : timeline.childHead;
        while (child){
            AnimTimer* following = reverse ? child->prev : child->next;
            fn(*child);
            child = following;
        }
    }

    template<typename F>
    void forEachTween(AnimAnimation& animation, F&& fn, bool reverse = false){
        AnimTween* tween = reverse ? animation.tail : animation.head;
        while (tween){
            AnimTween* following = reverse ? tween->prev : tween->next;
            fn(*tween);
            tween = following;
        }
    }

    AnimTimer& resetTimerProperties(AnimTimer& timer){
        timer.paused = true;
        timer.began = false;
        timer.completed = false;
        return timer;
    }

    void reviveTimer(AnimTimer& timer){
        if (!timer.cancelled)
            return;
        if (timer.hasChildren){
            forEachChild(static_cast<AnimTimeline&>(timer), [](AnimTimer& child){ reviveTimer(child); });
        } else if (timer.kind == AnimTimer::Kind::ANIMATION){
            forEachTween(static_cast<AnimAnimation&>(timer), [](AnimTween& tween){
                if (tween.composition != AnimComposition::NONE){
                    AnimComposer::composeTween(tween, AnimComposer::getSiblings(*tween.target, tween.property, false));
                }
            });
        }
        timer.cancelled = 0;
    }

    // tick() nesting: the transform writes of a top-level call are flushed once at its end
    int renderDepth = 0;

}

bool jasperengine::isAnimParameterKey(const std::string& key){
    return parameterKeys().count(key) != 0;
}

bool jasperengine::parseAnimClock(const AnimParam& value, AnimClock& clock, std::string* error){
    if (value.isNil()){
        clock = AnimClock::UI;
        return true;
    }
    if (value.isString() && value.asString() == "ui"){
        clock = AnimClock::UI;
        return true;
    }
    if (value.isString() && value.asString() == "game"){
        clock = AnimClock::GAME;
        return true;
    }
    if (error){
        *error = "anime: clock must be \"ui\" (the UI clock, default) or \"game\" (scaled game time), got " +
                 (value.isString() ? "\"" + value.asString() + "\"" : std::string("a non-string value"));
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Timer

std::shared_ptr<AnimTimer> AnimTimer::create(const AnimParam& params){
    std::shared_ptr<AnimTimer> timer(new AnimTimer(Kind::TIMER, params, nullptr, 0));
    timer->init();
    return timer;
}

AnimTimer::AnimTimer(Kind kind, const AnimParam& params, AnimTimeline* parent, double parentPosition):
        kind(kind), parent(parent){
    AnimEngine::countTimer(1);
    ++timerId;

    AnimEngine& engine = AnimEngine::instance();
    const AnimParam& defaults = AnimDefaults::of(parent);

    if (parent){
        clock = parent->clock;
    } else {
        std::string error;
        if (!parseAnimClock(param(params, "clock"), clock, &error))
            Log::error("[HTML UI] %s", error.c_str());
    }

    const double timerInitTime = parent ? 0 : engine.now(clock);
    const AnimParam& delayParam = param(params, "delay");
    const AnimParam& durationParam = param(params, "duration");
    const double timerDelay = delayParam.isFunction() || delayParam.isNil() ? defaults.get("delay").toNumber() : delayParam.toNumber();
    const double timerDuration = durationParam.isFunction() || durationParam.isNil() ? AnimConst::infinity : durationParam.toNumber();
    const AnimParam& loopParam = params.has("loop") ? param(params, "loop") : defaults.get("loop");
    const double timerLoopDelay = params.has("loopDelay") ? param(params, "loopDelay").toNumber() : defaults.get("loopDelay").toNumber();
    double timerIterationCount;
    if ((loopParam.isBool() && loopParam.asBool()) || (loopParam.isNumber() && (std::isinf(loopParam.asNumber()) || loopParam.asNumber() < 0))){
        timerIterationCount = AnimConst::infinity;
    } else {
        timerIterationCount = (loopParam.isBool() ? 0.0 : loopParam.toNumber()) + 1;
    }

    double offsetPosition = parent ? parentPosition : engine.now(clock);

    const AnimParam* idParam = params.find("id");
    id = idParam ? *idParam : AnimParam((double)timerId);
    duration = AnimMath::clampInfinity(((timerDuration + timerLoopDelay) * timerIterationCount) - timerLoopDelay);
    if (!duration || std::isnan(duration))
        duration = AnimConst::minValue;
    onBegin = callbackOf(params, "onBegin", defaults);
    onBeforeUpdate = callbackOf(params, "onBeforeUpdate", defaults);
    onUpdate = callbackOf(params, "onUpdate", defaults);
    onLoop = callbackOf(params, "onLoop", defaults);
    onPause = callbackOf(params, "onPause", defaults);
    onComplete = callbackOf(params, "onComplete", defaults);
    iterationDuration = timerDuration;
    iterationCount = timerIterationCount;
    autoplay = parent ? false : (params.has("autoplay") ? param(params, "autoplay").isTruthy() : defaults.get("autoplay").isTruthy());
    offset = offsetPosition;
    delay = timerDelay;
    loopDelay = timerLoopDelay;
    reversedFlag = (params.has("reversed") ? param(params, "reversed").isTruthy() : defaults.get("reversed").isTruthy()) ? 1 : 0;
    reverseInit = reversedFlag;
    alternateFlag = params.has("alternate") ? param(params, "alternate").isTruthy() : defaults.get("alternate").isTruthy();

    lastTickTime = timerInitTime;
    startTime = timerInitTime;
    lastTime = timerInitTime;
    absoluteTime = 0;
    const double frameRate = params.has("frameRate") ? param(params, "frameRate").toNumber() : defaults.get("frameRate").toNumber();
    fps = frameRate;
    frameDuration = AnimConst::K / AnimConst::maxFps;
    speed = params.has("playbackRate") ? param(params, "playbackRate").toNumber() : defaults.get("playbackRate").toNumber();
    priority = params.has("priority") ? param(params, "priority").toNumber() : 1;
}

AnimTimer::~AnimTimer(){
    AnimEngine::countTimer(-1);
}

AnimTickMode AnimTimer::requestTick(double time){
    const double elapsed = time - lastTickTime;
    const double scaled = frameDuration * .25;
    const double tolerance = scaled < 4 ? scaled : 4;
    if (elapsed + tolerance < frameDuration)
        return AnimTickMode::NONE;
    lastTickTime = elapsed >= frameDuration ? time - std::fmod(elapsed, frameDuration) : time;
    return AnimTickMode::AUTO;
}

double AnimTimer::computeDeltaTime(double time){
    const double delta = time - lastTime;
    deltaTime = delta;
    lastTime = time;
    return delta;
}

void AnimTimer::setFps(double frameRate){
    const double fr = frameRate;
    const double f = fr < AnimConst::minValue ? AnimConst::minValue : fr;
    fps = f;
    frameDuration = AnimConst::K / f;
}

void AnimTimer::setSpeed(double playbackRate){
    const double pbr = playbackRate;
    speed = pbr < AnimConst::minValue ? AnimConst::minValue : pbr;
    resetTime();
}

void AnimTimer::setCancelled(bool value){
    if (value){
        cancel();
    } else {
        reset(true).play();
    }
}

double AnimTimer::getCurrentTime() const{
    return AnimMath::clamp(AnimMath::round(absoluteTime, AnimConst::precision), -delay, duration);
}

void AnimTimer::setCurrentTime(double time){
    const bool wasPaused = paused;
    pause().seek(time);
    if (!wasPaused)
        resume();
}

double AnimTimer::getIterationCurrentTime() const{
    return AnimMath::clamp(AnimMath::round(iterationTime, AnimConst::precision), 0, iterationDuration);
}

void AnimTimer::setIterationCurrentTime(double time){
    setCurrentTime((iterationDuration * currentIteration) + time);
}

double AnimTimer::getProgress() const{
    return AnimMath::clamp(AnimMath::round(absoluteTime / duration, 10), 0, 1);
}

void AnimTimer::setProgress(double progress){
    setCurrentTime(duration * progress);
}

double AnimTimer::getIterationProgress() const{
    return AnimMath::clamp(AnimMath::round(iterationTime / iterationDuration, 10), 0, 1);
}

void AnimTimer::setIterationProgress(double progress){
    setCurrentTime((iterationDuration * currentIteration) + (iterationDuration * progress));
}

int AnimTimer::getCurrentIteration() const{
    return (int)currentIteration;
}

void AnimTimer::setCurrentIteration(int iteration){
    setCurrentTime(iterationDuration * AnimMath::clamp(iteration, 0, iterationCount - 1));
}

bool AnimTimer::isReversed() const{
    return reversedFlag != 0;
}

void AnimTimer::setReversed(bool value){
    if (value){
        reverse();
    } else {
        play();
    }
}

AnimTimer& AnimTimer::reset(bool softReset){
    // revive before rendering so the tweens' siblings are composed again
    reviveTimer(*this);
    if (reversedFlag && !reverseInit)
        setReversed(false);
    // the timelines' children render backwards from the end
    iterationTime = iterationDuration;
    AnimRender::tick(*this, 0, 1, softReset ? 1 : 0, AnimTickMode::FORCE);
    resetTimerProperties(*this);
    if (hasChildren){
        forEachChild(static_cast<AnimTimeline&>(*this), [](AnimTimer& child){ resetTimerProperties(child); });
    }
    return *this;
}

AnimTimer& AnimTimer::init(bool internalRender){
    AnimEngine::instance().captureTimer(*this);
    setFps(fps);
    // speed setter (resetTime included)
    const double pbr = speed;
    speed = pbr < AnimConst::minValue ? AnimConst::minValue : pbr;
    resetTime();
    // timelines render every child's initial state
    if (!internalRender && hasChildren){
        AnimRender::tick(*this, duration, 1, internalRender ? 1 : 0, AnimTickMode::FORCE);
    }
    reset(internalRender);
    if (autoplay)
        resume();
    return *this;
}

AnimTimer& AnimTimer::resetTime(){
    AnimEngine& engine = AnimEngine::instance();
    const double timeScale = 1 / (speed * engine.speed);
    startTime = engine.now(clock) - (absoluteTime + delay) * timeScale;
    return *this;
}

AnimTimer& AnimTimer::pause(){
    if (paused)
        return *this;
    paused = true;
    std::shared_ptr<AnimTimer> keep = shared_from_this();
    call(onPause, *this);
    return *this;
}

AnimTimer& AnimTimer::resume(){
    if (!paused)
        return *this;
    paused = false;
    // a timer without duration and children renders at once
    if (duration <= AnimConst::minValue && !hasChildren){
        std::shared_ptr<AnimTimer> keep = shared_from_this();
        AnimRender::tick(*this, AnimConst::minValue, 0, 0, AnimTickMode::FORCE);
    } else {
        if (!running){
            AnimEngine::instance().add(*this);
        }
        resetTime();
    }
    return *this;
}

AnimTimer& AnimTimer::restart(){
    return reset().resume();
}

AnimTimer& AnimTimer::seek(double time, bool muteCallbacks, bool internalRender){
    std::shared_ptr<AnimTimer> keep = shared_from_this();
    // recompose the tweens' siblings in case the timer was cancelled
    reviveTimer(*this);
    // seeking a completed timer: the next play starts from here, not from 0
    completed = false;
    const bool isPaused = paused;
    paused = true;
    AnimRender::tick(*this, time + delay, muteCallbacks ? 1 : 0, internalRender ? 1 : 0, AnimTickMode::AUTO);
    return isPaused ? *this : resume();
}

AnimTimer& AnimTimer::alternate(){
    const int reversed = reversedFlag;
    const double count = iterationCount;
    const double itDuration = iterationDuration;
    const double iterations = std::isinf(count) ? std::floor(AnimConst::maxValue / itDuration) : count;
    reversedFlag = (alternateFlag && !std::fmod(iterations, 2)) ? reversed : !reversed;
    if (std::isinf(count)){
        // endless loops loop on themselves
        setIterationProgress(reversedFlag ? 1 - getIterationProgress() : getIterationProgress());
    } else {
        seek((itDuration * iterations) - absoluteTime);
    }
    resetTime();
    return *this;
}

AnimTimer& AnimTimer::play(){
    if (reversedFlag)
        alternate();
    return resume();
}

AnimTimer& AnimTimer::reverse(){
    if (!reversedFlag)
        alternate();
    return resume();
}

AnimTimer& AnimTimer::cancel(){
    if (hasChildren){
        forEachChild(static_cast<AnimTimeline&>(*this), [](AnimTimer& child){ child.cancel(); }, true);
    } else if (kind == Kind::ANIMATION){
        forEachTween(static_cast<AnimAnimation&>(*this), [](AnimTween& tween){ AnimComposer::removeTweenSiblings(tween); });
    }
    cancelled = 1;
    // pausing removes it from the engine
    return pause();
}

AnimTimer& AnimTimer::stretch(double newDuration){
    const double currentDuration = duration;
    const double normalizedDuration = AnimMath::normalizeTime(newDuration);
    if (currentDuration == normalizedDuration)
        return *this;
    const double timeScale = newDuration / currentDuration;
    const bool isSetter = newDuration <= AnimConst::minValue;
    duration = isSetter ? AnimConst::minValue : normalizedDuration;
    iterationDuration = isSetter ? AnimConst::minValue : AnimMath::normalizeTime(iterationDuration * timeScale);
    offset *= timeScale;
    delay *= timeScale;
    loopDelay *= timeScale;
    return *this;
}

AnimTimer& AnimTimer::revert(){
    std::shared_ptr<AnimTimer> keep = shared_from_this();
    AnimRender::tick(*this, 0, 1, 0, AnimTickMode::AUTO);
    return cancel();
}

AnimTimer& AnimTimer::complete(bool muteCallbacks){
    return seek(duration, muteCallbacks).cancel();
}

AnimTimer& AnimTimer::refresh(){
    return *this;
}

AnimTimer& AnimTimer::then(Callback fn){
    if (!fn)
        return *this;
    resolve = [fn](AnimTimer& timer){
        // once
        timer.resolve = nullptr;
        fn(timer);
    };
    if (completed){
        std::shared_ptr<AnimTimer> keep = shared_from_this();
        Callback r = resolve;
        r(*this);
    }
    return *this;
}

// ---------------------------------------------------------------------------------------------
// render / tick (core/render.js)

int AnimRender::render(AnimTimer& tickable, double time, int muteCallbacks, int internalRender, AnimTickMode tickMode){
    AnimTimeline* parent = tickable.parent;
    const double duration = tickable.duration;
    const bool completed = tickable.completed;
    const double iterationDuration = tickable.iterationDuration;
    const double iterationCount = tickable.iterationCount;
    const double previousIteration = tickable.currentIteration;
    const double loopDelay = tickable.loopDelay;
    const int reversed = tickable.reversedFlag;
    const bool alternate = tickable.alternateFlag;
    const bool hasChildren = tickable.hasChildren;
    const double tickableDelay = tickable.delay;
    const double tickablePrevAbsoluteTime = tickable.absoluteTime;
    const double tickableEndTime = tickableDelay + iterationDuration;
    const double tickableAbsoluteTime = time - tickableDelay;
    const double tickablePrevTime = AnimMath::clamp(tickablePrevAbsoluteTime, -tickableDelay, duration);
    const double tickableCurrentTime = AnimMath::clamp(tickableAbsoluteTime, -tickableDelay, duration);
    const double deltaTime = tickableAbsoluteTime - tickablePrevAbsoluteTime;
    const bool isCurrentTimeAboveZero = tickableCurrentTime > 0;
    const bool isCurrentTimeEqualOrAboveDuration = tickableCurrentTime >= duration;
    const bool isSetter = duration <= AnimConst::minValue;
    const bool forcedTick = tickMode == AnimTickMode::FORCE;

    int isOdd = 0;
    double iterationElapsedTime = tickableAbsoluteTime;
    int hasRendered = 0;

    if (iterationCount > 1){
        const double period = iterationDuration + (isCurrentTimeEqualOrAboveDuration ? 0 : loopDelay);
        const double iteration = AnimMath::truncate(tickableCurrentTime / period);
        tickable.currentIteration = AnimMath::clamp(iteration, 0, iterationCount);
        // the last iteration at the end, not one past it
        if (isCurrentTimeEqualOrAboveDuration)
            tickable.currentIteration--;
        isOdd = std::fmod(tickable.currentIteration, 2) != 0 ? 1 : 0;
        // the same truncation as the iteration (a modulo can disagree at the boundaries)
        iterationElapsedTime = tickableCurrentTime - iteration * period;
        if (std::isnan(iterationElapsedTime))
            iterationElapsedTime = 0;
    }

    const int isReversed = reversed ^ (alternate && isOdd ? 1 : 0);
    const EaseFn& playbackEase = tickable.ease;
    double iterationTime = isCurrentTimeEqualOrAboveDuration ? (isReversed ? 0 : duration)
                                                             : (isReversed ? iterationDuration - iterationElapsedTime : iterationElapsedTime);
    if (playbackEase){
        iterationTime = iterationDuration * playbackEase(iterationTime / iterationDuration);
        if (std::isnan(iterationTime))
            iterationTime = 0;
    }
    const bool isRunningBackwards = (parent ? parent->backwards : tickableAbsoluteTime < tickablePrevAbsoluteTime) ? !isReversed : isReversed != 0;

    tickable.absoluteTime = tickableAbsoluteTime;
    tickable.iterationTime = iterationTime;
    tickable.backwards = isRunningBackwards;

    if (isCurrentTimeAboveZero && !tickable.began){
        tickable.began = true;
        if (!muteCallbacks && !(parent && (isRunningBackwards || !parent->began))){
            if (tickable.onBegin) tickable.onBegin(tickable);
        }
    } else if (tickableAbsoluteTime <= 0){
        tickable.began = false;
    }

    // onLoop of timers without children here (timelines: in tick()), before rendering so
    // refresh() picks up the current values
    if (!muteCallbacks && !hasChildren && isCurrentTimeAboveZero && tickable.currentIteration != previousIteration){
        if (tickable.onLoop) tickable.onLoop(tickable);
    }

    if (
        forcedTick ||
        (tickMode == AnimTickMode::AUTO && (
            // timeline children render from their offset, covering a truncated sibling's gap on seek
            (time >= (parent && tickableDelay > 0 ? 0 : tickableDelay) && time <= tickableEndTime) ||
            // before the start: the initial state
            (time <= tickableDelay && tickablePrevTime > tickableDelay) ||
            // after the end: the end state
            (time >= tickableEndTime && tickablePrevTime != duration)
        )) ||
        (iterationTime >= tickableEndTime && tickablePrevTime != duration) ||
        // a backward seek into a looped iteration's delay, except at the settled end
        (iterationTime <= tickableDelay && tickablePrevTime > 0 && !isCurrentTimeEqualOrAboveDuration) ||
        // a seek on a completed timer
        (time <= tickablePrevTime && tickablePrevTime == duration && completed) ||
        // 0-duration timers are not skipped
        (isCurrentTimeEqualOrAboveDuration && !completed && isSetter)
    ){
        if (isCurrentTimeAboveZero){
            tickable.computeDeltaTime(tickablePrevTime);
            if (!muteCallbacks && tickable.onBeforeUpdate) tickable.onBeforeUpdate(tickable);
        }

        if (!hasChildren && tickable.kind == AnimTimer::Kind::ANIMATION){
            AnimAnimation& animation = static_cast<AnimAnimation&>(tickable);

            // a jump bigger than the tick threshold renders like a seek
            const bool forcedRender = forcedTick || (isRunningBackwards ? deltaTime * -1 : deltaTime) >= AnimConst::tickThreshold;
            // rounded like tween.absoluteStartTime so equal-time boundaries compare cleanly
            const double absoluteTime = AnimMath::round(tickable.offset + (parent ? parent->offset : 0) + tickableDelay + iterationTime, 12);

            AnimTween* tween = animation.head;
            while (tween){
                AnimTween* nextTween = tween->next;
                const AnimComposition tweenComposition = tween->composition;
                const double tweenCurrentTime = tween->currentTime;
                const double tweenChangeDuration = tween->changeDuration;
                const double tweenAbsEndTime = tween->absoluteStartTime + tween->changeDuration;
                AnimTween* tweenNextRep = tween->nextRep;
                AnimTween* tweenPrevRep = tween->prevRep;
                const bool tweenHasComposition = tweenComposition != AnimComposition::NONE;
                // the previous sibling stops writing at its truncated end
                const double tweenPrevRepEndTime = tweenPrevRep ? tweenPrevRep->absoluteStartTime + tweenPrevRep->changeDuration : 0;
                const bool tweenPrevRepIsCrossParent = tweenPrevRep && tweenPrevRep->parent != tween->parent;
                const double tweenNextRepTakeover = !tweenNextRep || tweenNextRep->isOverridden ? tweenAbsEndTime :
                    tweenNextRep->parent == tween->parent ? tweenAbsEndTime + tweenNextRep->delay :
                    tweenNextRep->absoluteStartTime < tweenNextRep->absoluteUpdateStartTime ? tweenNextRep->absoluteStartTime : tweenNextRep->absoluteUpdateStartTime;

                const bool gate =
                    (forcedRender || (
                        (tweenCurrentTime != tweenChangeDuration || absoluteTime <= tweenNextRepTakeover ||
                         (tweenPrevRep && !tweenPrevRepIsCrossParent && (!tweenNextRep || tweenNextRep->parent != tween->parent))) &&
                        (tweenCurrentTime != 0 || absoluteTime >= tween->absoluteStartTime ||
                         (tweenPrevRepIsCrossParent && !tween->hasFromValue && !tweenPrevRep->isOverridden && absoluteTime >= tweenPrevRepEndTime) ||
                         (tweenNextRep && !tweenNextRep->isOverridden && tweenNextRep->parent == tween->parent && tweenNextRep->currentTime != 0 && iterationTime < tweenNextRep->startTime))
                    )) &&
                    // later keyframes wait for their own start
                    (!tweenPrevRep || tweenPrevRepIsCrossParent || iterationTime >= tween->startTime) &&
                    (!tweenHasComposition || (
                        !tween->isOverridden &&
                        (!tween->isOverlapped || absoluteTime <= tweenAbsEndTime) &&
                        // the next sibling owns the value past its takeover point
                        (!tweenNextRep || tweenNextRep->isOverridden || absoluteTime <= tweenNextRepTakeover) &&
                        // the previous sibling owns the value up to its truncated end
                        (!tweenPrevRep || (tweenPrevRep->isOverridden || (!tweenPrevRepIsCrossParent ?
                            absoluteTime >= tweenPrevRepEndTime + tween->delay :
                            absoluteTime >= tween->absoluteStartTime || (!tween->hasFromValue && absoluteTime >= tweenPrevRepEndTime))))
                    ));

                if (gate){
                    const double tweenNewTime = tween->currentTime = AnimMath::clamp(iterationTime - tween->startTime, 0, tweenChangeDuration);
                    const double tweenProgress = tween->ease ? tween->ease(tweenNewTime / tween->updateDuration) : tweenNewTime / tween->updateDuration;
                    if (AnimComposer::renderTween(*tween, tweenProgress, internalRender) && isCurrentTimeAboveZero)
                        hasRendered = 1;
                } else if (tweenCurrentTime && tweenPrevRep && !tweenPrevRepIsCrossParent && iterationTime < tween->startTime){
                    // before its start: the previous keyframe owns the from value
                    tween->currentTime = 0;
                }

                tween = nextTween;
            }

            if (!muteCallbacks && hasRendered && tickable.onRender){
                tickable.onRender(tickable);
            }
        }

        if (!muteCallbacks && isCurrentTimeAboveZero && tickable.onUpdate){
            tickable.onUpdate(tickable);
        }
    }

    // setters on timelines, and onComplete again after a backward seek
    if (parent && isSetter){
        if (!muteCallbacks && (
            (parent->began && !isRunningBackwards && tickableAbsoluteTime > 0 && !completed) ||
            (isRunningBackwards && tickableAbsoluteTime <= AnimConst::minValue && completed)
        )){
            if (tickable.onComplete) tickable.onComplete(tickable);
            tickable.completed = !isRunningBackwards;
        }
    // above 0 and at the end: onComplete, or the next loop of an endless timer
    } else if (isCurrentTimeAboveZero && isCurrentTimeEqualOrAboveDuration){
        if (std::isinf(iterationCount)){
            // restart the endless timer from 0
            tickable.startTime += tickable.duration;
        } else if (tickable.currentIteration >= iterationCount - 1){
            // the engine drops paused timers at its next tick
            tickable.paused = true;
            if (!completed && !hasChildren){
                // timelines complete in tick() once every child completed
                tickable.completed = true;
                if (!muteCallbacks && !(parent && (isRunningBackwards || !parent->began))){
                    if (tickable.onComplete) tickable.onComplete(tickable);
                    if (tickable.resolve){
                        AnimTimer::Callback resolve = tickable.resolve;
                        resolve(tickable);
                    }
                }
            }
        }
    } else {
        tickable.completed = false;
    }

    return hasRendered;
}

void AnimRender::tick(AnimTimer& tickable, double time, int muteCallbacks, int internalRender, AnimTickMode tickMode){
    renderDepth++;
    const double previousIteration = tickable.currentIteration;
    render(tickable, time, muteCallbacks, internalRender, tickMode);
    if (tickable.hasChildren){
        AnimTimeline& tl = static_cast<AnimTimeline&>(tickable);
        const bool tlIsRunningBackwards = tl.backwards;
        const double tlChildrenTime = internalRender ? time : tl.iterationTime;
        const double tlChildrenTickTime = AnimEngine::instance().now(tl.clock);

        int tlChildrenHasRendered = 0;
        bool tlChildrenHaveCompleted = true;

        // looped forward: the children's skipped callbacks
        if (!internalRender && tl.currentIteration != previousIteration){
            const double tlIterationDuration = tl.iterationDuration;
            forEachChild(tl, [&](AnimTimer& child){
                if (!tlIsRunningBackwards){
                    // an internal render triggers the callbacks of a child that has not completed
                    if (!child.completed && !child.backwards && child.absoluteTime < child.iterationDuration){
                        render(child, tlIterationDuration, muteCallbacks, 1, AnimTickMode::FORCE);
                    }
                    // the next iteration triggers their callbacks again
                    child.began = false;
                    child.completed = false;
                } else {
                    const double childDuration = child.duration;
                    const double childStartTime = child.offset + child.delay;
                    const double childEndTime = childStartTime + childDuration;
                    // onComplete in reverse for the children at the timeline's edges
                    if (!muteCallbacks && childDuration <= AnimConst::minValue && (!childStartTime || childEndTime == tlIterationDuration)){
                        if (child.onComplete) child.onComplete(child);
                    }
                }
            });
            if (!muteCallbacks && tl.onLoop) tl.onLoop(tl);
        }

        forEachChild(tl, [&](AnimTimer& child){
            const double childTime = AnimMath::round((tlChildrenTime - child.offset) * child.speed, 12);
            // past-end siblings stay quiet on a backward iteration (their to-values would win)
            if (tlIsRunningBackwards && childTime > child.delay + child.duration)
                return;
            const AnimTickMode childTickMode = child.fps < tl.fps ? child.requestTick(tlChildrenTickTime) : tickMode;
            tlChildrenHasRendered += render(child, childTime, muteCallbacks, internalRender, childTickMode);
            if (!child.completed && tlChildrenHaveCompleted)
                tlChildrenHaveCompleted = false;
        }, tlIsRunningBackwards);

        // the timeline renders through its children
        if (!muteCallbacks && tlChildrenHasRendered && tl.onRender) tl.onRender(tl);
        // onComplete once every child completed and the time reached the end
        if ((tlChildrenHaveCompleted || tlIsRunningBackwards) && tl.absoluteTime >= tl.duration){
            tl.paused = true;
            if (!tl.completed){
                tl.completed = true;
                if (!muteCallbacks){
                    if (tl.onComplete) tl.onComplete(tl);
                    if (tl.resolve){
                        AnimTimer::Callback resolve = tl.resolve;
                        resolve(tl);
                    }
                }
            }
        }
    }
    renderDepth--;
    if (renderDepth == 0)
        AnimProperty::flush();
}
