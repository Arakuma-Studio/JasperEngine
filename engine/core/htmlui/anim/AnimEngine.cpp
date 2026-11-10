// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Port of Anime.js 4.5.0 (MIT) engine/engine.js, driven by the engine's frame loop instead of
// requestAnimationFrame: UIDocumentSystem::update ticks it with the UI clock.

#include "AnimEngine.h"

#include "AnimAnimation.h"
#include "AnimCommon.h"
#include "AnimProperty.h"
#include "AnimTimeline.h"
#include "Engine.h"
#include "HtmlUIClock.h"

#include <unordered_set>

using namespace jasperengine;

namespace {

    size_t liveTimers = 0;

    // every live animation, so reset() can scrub the composition links of the ones scripts keep
    std::unordered_set<AnimAnimation*>& liveAnimations(){
        static auto* set = new std::unordered_set<AnimAnimation*>();
        return *set;
    }

    // a timer whose targets are all gone: cancelled without callbacks
    void cancelSilently(AnimTimer& timer){
        if (timer.kind == AnimTimer::Kind::TIMELINE){
            for (AnimTimer* child = static_cast<AnimTimeline&>(timer).childHead; child; child = child->next)
                cancelSilently(*child);
        } else if (timer.kind == AnimTimer::Kind::ANIMATION){
            for (AnimTween* tween = static_cast<AnimAnimation&>(timer).head; tween; tween = tween->next)
                AnimComposer::removeTweenSiblings(*tween);
        }
        timer.cancelled = 1;
        timer.paused = true;
    }

}

AnimEngine& AnimEngine::instance(){
    // leaked: timers held by scripts can outlive static destruction
    static AnimEngine* engine = new AnimEngine();
    return *engine;
}

AnimEngine::ScopedTickPause::ScopedTickPause(std::vector<std::shared_ptr<AnimTimer>>* created){
    AnimEngine& engine = AnimEngine::instance();
    previousCollector = engine.timerCollector;
    if (created) engine.timerCollector = created;
    ++engine.tickPauseDepth;
}

AnimEngine::ScopedTickPause::~ScopedTickPause(){
    AnimEngine& engine = AnimEngine::instance();
    --engine.tickPauseDepth;
    engine.timerCollector = previousCollector;
}

void AnimEngine::countTimer(int delta){
    liveTimers = (size_t)((long long)liveTimers + delta);
}

size_t AnimEngine::getLiveTimerCount(){
    return liveTimers;
}

void AnimEngine::tick(double uiTimeSeconds){
    tick(uiTimeSeconds, Engine::getGameTime());
}

void AnimEngine::tick(double uiTimeSeconds, double gameTimeSeconds){
    tickMilliseconds(uiTimeSeconds * AnimConst::K, gameTimeSeconds * AnimConst::K);
}

void AnimEngine::tickMilliseconds(double uiTimeMs, double gameTimeMs){
    if (updating || tickPauseDepth != 0)
        return;
    if (ticked && uiTimeMs == uiNow && gameTimeMs == lastGameSource)
        return;
    // the game clock accumulates the (scaled) deltas: a restarted game time never goes back
    const double gameDelta = ticked && gameTimeMs > lastGameSource ? gameTimeMs - lastGameSource : 0.0;
    gameNow += gameDelta;
    lastGameSource = gameTimeMs;
    uiNow = uiTimeMs;
    ticked = true;
    update();
}

void AnimEngine::alignClockSources(double uiTimeSeconds, double gameTimeSeconds){
    const double nextUi = uiTimeSeconds * AnimConst::K;
    const double shift = nextUi - now(AnimClock::UI);
    for (AnimTimer* timer = head; timer; timer = timer->next){
        if (timer->clock == AnimClock::UI){
            timer->startTime += shift;
            timer->lastTickTime += shift;
        }
    }
    uiNow = nextUi;
    lastGameSource = gameTimeSeconds * AnimConst::K;
    ticked = true;
}

double AnimEngine::now(AnimClock clock) const{
    if (clock == AnimClock::GAME)
        return gameNow;
    return ticked ? uiNow : HtmlUIClock::now() * AnimConst::K;
}

void AnimEngine::add(AnimTimer& timer){
    // sorted by priority (addChild with sortByPriority)
    AnimTimer* prev = tail;
    while (prev && prev->priority > timer.priority)
        prev = prev->prev;
    AnimTimer* next = prev ? prev->next : head;
    if (prev) prev->next = &timer; else head = &timer;
    if (next) next->prev = &timer; else tail = &timer;
    timer.prev = prev;
    timer.next = next;
    timer.running = true;
    timer.engineRef = timer.shared_from_this();
    hasChildren = true;
    if (timerCollector) timerCollector->push_back(timer.engineRef);
}

void AnimEngine::update(){
    updating = true;
    AnimTimer* active = head;
    while (active){
        AnimTimer* nextTickable = active->next;
        // alive until the end of its tick, whatever its callbacks do (utils.remove included)
        std::shared_ptr<AnimTimer> keep = active->engineRef;
        std::shared_ptr<AnimTimer> keepNext = nextTickable ? nextTickable->engineRef : nullptr;
        if (!active->paused && active->hasOnlyInvalidTargets())
            cancelSilently(*active);
        if (!active->paused){
            const double time = now(active->clock);
            AnimRender::tick(*active, (time - active->startTime) * active->speed * speed, 0, 0,
                             active->fps < fps ? active->requestTick(time) : AnimTickMode::AUTO);
        } else {
            AnimTimer* prev = active->prev;
            AnimTimer* next = active->next;
            if (prev) prev->next = next; else head = next;
            if (next) next->prev = prev; else tail = prev;
            active->prev = nullptr;
            active->next = nullptr;
            hasChildren = tail != nullptr;
            active->running = false;
            if (active->completed && !active->cancelled)
                active->cancel();
            active->engineRef.reset();
        }
        // a callback may have taken the next timer out of the list: the rest ticks next frame
        if (nextTickable && !nextTickable->running)
            nextTickable = active->running ? active->next : nullptr;
        active = nextTickable;
        keepNext.reset();
        keep.reset();
    }
    AnimComposer::updateAdditive();
    AnimProperty::flush();
    updating = false;
}

size_t AnimEngine::getActiveCount() const{
    size_t count = 0;
    for (AnimTimer* t = head; t; t = t->next)
        count++;
    return count;
}

uint64_t AnimEngine::addResetListener(std::function<void()> listener){
    const uint64_t id = nextListenerId++;
    resetListeners.emplace_back(id, std::move(listener));
    return id;
}

void AnimEngine::removeResetListener(uint64_t id){
    for (auto it = resetListeners.begin(); it != resetListeners.end(); ++it){
        if (it->first == id){
            resetListeners.erase(it);
            return;
        }
    }
}

void AnimEngine::registerAnimation(AnimAnimation* animation, bool alive){
    if (alive){
        liveAnimations().insert(animation);
    } else {
        liveAnimations().erase(animation);
    }
}

void AnimEngine::reset(){
    // out of the running list without callbacks
    std::vector<std::shared_ptr<AnimTimer>> running;
    AnimTimer* t = head;
    while (t){
        AnimTimer* next = t->next;
        t->prev = nullptr;
        t->next = nullptr;
        t->running = false;
        t->paused = true;
        t->cancelled = 1;
        running.push_back(std::move(t->engineRef));
        t = next;
    }
    head = nullptr;
    tail = nullptr;
    hasChildren = false;

    // the composition lists of every animation still alive (scripts may hold some)
    for (AnimAnimation* animation : liveAnimations()){
        animation->paused = true;
        animation->cancelled = 1;
        for (AnimTween* tween = animation->head; tween; tween = tween->next){
            if (tween->target){
                tween->target->replaceSiblings.clear();
                tween->target->additiveSiblings.clear();
            }
            tween->prevRep = tween->nextRep = nullptr;
            tween->prevAdd = tween->nextAdd = nullptr;
            tween->inReplaceList = false;
            tween->inAdditiveList = false;
        }
    }
    AnimComposer::resetAdditive();
    AnimProperty::flush();

    // the binding's references (Lua callbacks, function values, table targets)
    std::vector<std::pair<uint64_t, std::function<void()>>> listeners = resetListeners;
    for (auto& listener : listeners)
        listener.second();

    running.clear();
    ticked = false;
    gameNow = 0;
    lastGameSource = 0;
}
