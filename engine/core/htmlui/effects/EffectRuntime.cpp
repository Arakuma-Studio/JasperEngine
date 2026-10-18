// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effects' per-element runtime (phase-5 spec 3.4, 3.6): instances, the state that survives
// re-instancing, the clock, the pointer, and the frame block.

#include "EffectRuntime.h"
#include "EffectSimulator.h"

#include "HtmlUIClock.h"
#include "Log.h"
#include "render/SystemRender.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/RenderBox.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    std::optional<double>& fixedTime(){
        static std::optional<double> value;
        return value;
    }

    constexpr size_t kMaxStatesPerElement = 16;
    constexpr float kTrailMaxAge = 4.f;
    constexpr float kRippleMaxAge = 4.f;
    constexpr float kRippleMoveStep = 40.f;
    constexpr float kSmoothHalfLife = 0.08f;
    constexpr float kPresenceTime = 0.15f;
    constexpr float kNone = 1e6f;

    void set4(float out[4], float x, float y, float z, float w){
        out[0] = x;
        out[1] = y;
        out[2] = z;
        out[3] = w;
    }

    // a context point in the area's local CSS px (through the element's transform)
    Rml::Vector2f toLocal(Rml::Element* element, Rml::Vector2f point, Rml::Vector2f areaOrigin){
        Rml::Vector2f p = point;
        element->Project(p);
        return p - areaOrigin;
    }

    // the value of a number parameter of the snapshot (0 when absent)
    float paramNumber(const EffectInstance& instance, const std::string& name){
        if (!instance.snapshot || !instance.manifest)
            return 0.f;
        const int index = instance.manifest->paramIndex(name);
        if (index < 0 || index >= (int)instance.snapshot->values.size())
            return 0.f;
        return instance.snapshot->values[index].f;
    }

}

double EffectClock::now(){
    if (fixedTime())
        return *fixedTime();
    return HtmlUIClock::now();
}

void EffectClock::setFixedTime(std::optional<double> seconds){
    if (seconds && !std::isfinite(*seconds))
        return;
    fixedTime() = seconds;
}

bool EffectClock::isFixed(){
    return fixedTime().has_value();
}

std::optional<double> EffectClock::getFixedTime(){
    return fixedTime();
}

EffectRuntime& EffectRuntime::get(){
    // outlives RmlUi (it is a plugin while RmlUi runs); never destroyed
    static EffectRuntime* instance = new EffectRuntime();
    return *instance;
}

int EffectRuntime::GetEventClasses(){
    return EVT_BASIC | EVT_DOCUMENT | EVT_ELEMENT;
}

void EffectRuntime::beginFrame(){
    // every context render (a context may render twice in a sokol frame, Context::Update
    // clearing its next-update request in between)
    renders++;
    const uint64_t key = SystemRender::getFrameCount();
    if (key == frameKey)
        return;
    frameKey = key;
    frame++;
    frameTotals = EffectStats();
    frameTotals.liveInstances = live.size();
    sweepSimulations();
}

void EffectRuntime::countReinstance(){
    totals.reinstances++;
    frameTotals.reinstances++;
}

void EffectRuntime::onPointerMove(Rml::Context* context, Rml::Vector2f contextPx){
    PointerState& p = pointers[context];
    pointerCacheValid = false;
    if (p.testOverride)
        return;
    p.position = contextPx;
    p.valid = true;
}

void EffectRuntime::onPointerButton(Rml::Context* context, Rml::Vector2f contextPx, bool down){
    PointerState& p = pointers[context];
    pointerCacheValid = false;
    if (p.testOverride)
        return;
    p.position = contextPx;
    p.valid = true;
    if (down && !p.down){
        p.pressCount++;
        p.pressPosition = contextPx;
        p.pressClock = EffectClock::now();
    }
    p.down = down;
}

void EffectRuntime::onPointerButton(Rml::Context* context, bool down){
    auto it = pointers.find(context);
    const Rml::Vector2f position = it != pointers.end() ? it->second.position : Rml::Vector2f(0.f, 0.f);
    onPointerButton(context, position, down);
}

void EffectRuntime::onPointerLeave(Rml::Context* context){
    PointerState& p = pointers[context];
    pointerCacheValid = false;
    if (p.testOverride)
        return;
    p.valid = false;
    p.down = false;
}

void EffectRuntime::setTestPointer(Rml::Context* context, std::optional<Rml::Vector2f> contextPx, bool down, float sincePress){
    PointerState& p = pointers[context];
    pointerCacheValid = false;
    p.testOverride = true;
    p.valid = contextPx.has_value();
    if (contextPx)
        p.position = *contextPx;
    p.down = down;
    p.testSincePress = sincePress;
    if (sincePress >= 0.f){
        p.pressCount++;
        p.pressPosition = p.position;
        p.pressClock = EffectClock::now() - sincePress;
    }
}

void EffectRuntime::setTestTrail(Rml::Context* context, const std::vector<Rml::Vector4f>& samples){
    PointerState& p = pointers[context];
    pointerCacheValid = false;
    p.testTrail = true;
    p.trail = samples;
}

void EffectRuntime::clearTestInput(){
    for (auto& entry : pointers){
        entry.second.testOverride = false;
        entry.second.testTrail = false;
        entry.second.trail.clear();
        entry.second.valid = false;
        entry.second.down = false;
    }
}

float EffectRuntime::seedOf(Rml::Element* element){
    if (!element)
        return 0.f;
    const std::string address = element->GetAddress();
    uint32_t h = 2166136261u;
    for (unsigned char c : address){
        h ^= c;
        h *= 16777619u;
    }
    return (float)((double)(h >> 8) / (double)(1u << 24));
}

EffectInstance* EffectRuntime::acquire(Rml::Element* element, const EffectManifest& manifest, bool filterForm,
                                       const std::shared_ptr<const EffectParamsSnapshot>& snapshot, int paintArea){
    ElementEntry& entry = elements[element];
    if (entry.acquireFrame != frame){
        entry.acquireFrame = frame;
        entry.ordinals.clear();
    }
    const int ordinal = entry.ordinals[manifest.name]++;
    const std::string key = manifest.name + "#" + std::to_string(ordinal);
    std::shared_ptr<EffectElementState> state;
    for (size_t i = 0; i < entry.states.size(); i++){
        if (entry.states[i].first == key){
            state = entry.states[i].second;
            // most recently used last
            auto keep = entry.states[i];
            entry.states.erase(entry.states.begin() + (long)i);
            entry.states.push_back(keep);
            break;
        }
    }
    if (!state){
        state = std::make_shared<EffectElementState>();
        entry.states.push_back({key, state});
        while (entry.states.size() > kMaxStatesPerElement)
            entry.states.erase(entry.states.begin());
    }

    std::unique_ptr<EffectInstance> instance;
    if (!released.empty()){
        instance = std::move(released.back());
        released.pop_back();
        *instance = EffectInstance();
    }else{
        instance = std::make_unique<EffectInstance>();
    }
    state->liveInstances++;
    state->orphanFrame = UINT64_MAX;
    instance->alive = true;
    instance->context = element ? element->GetContext() : nullptr;
    instance->manifest = &manifest;
    instance->filterForm = filterForm;
    instance->element = element;
    instance->state = state;
    instance->snapshot = snapshot;
    if (snapshot){
        instance->opacity = snapshot->opacity;
        instance->timeScale = snapshot->timeScale;
        instance->timeOffset = snapshot->timeOffset;
        instance->blend = snapshot->blend;
    }
    instance->paintArea = paintArea;
    instance->clipToArea = !manifest.boundsGeometry;
    // the integrated clocks' rates, looked up by name once (integrate runs every frame)
    for (size_t i = 0; i < manifest.timeParams.size() && i < 4; i++)
        instance->timeRates[i] = paramNumber(*instance, manifest.timeParams[i]);
    EffectInstance* raw = instance.get();
    live[raw] = std::move(instance);
    totals.instancesCreated++;
    frameTotals.instancesCreated++;
    totals.liveInstances = live.size();
    frameTotals.liveInstances = live.size();
    return raw;
}

void EffectRuntime::release(EffectInstance* instance){
    auto it = live.find(instance);
    if (it == live.end())
        return;
    std::unique_ptr<EffectInstance> owned = std::move(it->second);
    live.erase(it);
    // the simulation of a state nothing uses any more is released at a later frame start (a
    // re-instance releases and acquires within one update)
    if (owned->state && --owned->state->liveInstances <= 0){
        owned->state->liveInstances = 0;
        owned->state->orphanFrame = frame;
    }
    // its references (state, snapshot, shader) go now; the memory waits for the next acquire
    *owned = EffectInstance();
    released.push_back(std::move(owned));
    totals.liveInstances = live.size();
    frameTotals.liveInstances = live.size();
}

void EffectRuntime::requestRedraw(Rml::Context* context){
    if (!context || (context == redrawContext && renders == redrawRender))
        return;
    context->RequestNextUpdate(0);
    redrawContext = context;
    redrawRender = renders;
}

bool EffectRuntime::isLive(const EffectInstance* instance) const{
    // every handle's instance is the runtime's memory (live or released, see `released`)
    return instance && instance->alive;
}

size_t EffectRuntime::elementStateCount() const{
    size_t count = 0;
    for (const auto& entry : elements)
        count += entry.second.states.size();
    return count;
}

void EffectRuntime::OnElementDestroy(Rml::Element* element){
    elements.erase(element);
    // an instance outliving its element (released right after) draws nothing
    for (auto& entry : live){
        if (entry.first->element == element)
            entry.first->element = nullptr;
    }
}

void EffectRuntime::OnContextDestroy(Rml::Context* context){
    pointers.erase(context);
    pointerCacheValid = false;
    if (redrawContext == context)
        redrawContext = nullptr;
}

void EffectRuntime::OnDocumentUnload(Rml::ElementDocument* document){
    // the elements' destruction purges their states (and simulations)
    overBudgetDocuments.erase(document);
}

void EffectRuntime::OnShutdown(){
    elements.clear();
    activeSimulations.clear();
    overBudgetDocuments.clear();
    EffectStatePool::get().destroyAll();
    pointers.clear();
    pointerCacheValid = false;
    redrawContext = nullptr;
}

void EffectRuntime::integrate(EffectInstance& instance, EffectElementState& state, double now, float dt, bool fixed,
                              Rml::Vector2f areaOrigin, const PointerState* pointer){
    const EffectManifest& m = *instance.manifest;
    Rml::Element* element = instance.element;
    const float time = (float)(now * instance.timeScale + instance.timeOffset);

    // integrated clocks (@time): never jump when a rate changes, the closed form when fixed
    for (size_t i = 0; i < m.timeParams.size() && i < 4; i++){
        const float rate = instance.timeRates[i];
        if (fixed){
            state.anim[i] = time * rate;
        }else{
            if (!state.animStarted)
                state.anim[i] = instance.timeOffset * rate;
            state.anim[i] += dt * rate * instance.timeScale;
        }
    }
    state.animStarted = true;

    // the pointer in area-local px
    const bool hasPointer = pointer && pointer->valid && element;
    Rml::Vector2f local(0.f, 0.f);
    bool inside = false;
    if (hasPointer){
        local = toLocal(element, pointer->position, areaOrigin);
        inside = local.x >= 0.f && local.y >= 0.f && local.x <= instance.areaSize.x && local.y <= instance.areaSize.y;
    }
    // smoothing (80 ms half-life) and presence (150 ms), snapped with a fixed clock
    if (fixed || !state.smoothValid){
        if (hasPointer){
            state.smoothPointer = local;
            state.smoothValid = true;
        }
        if (fixed)
            state.presence = inside ? 1.f : 0.f;
    }else if (hasPointer){
        const float k = 1.f - std::pow(2.f, -dt / kSmoothHalfLife);
        state.smoothPointer = state.smoothPointer + (local - state.smoothPointer) * k;
    }
    if (!fixed){
        const float step = dt / kPresenceTime;
        state.presence = inside ? std::min(1.f, state.presence + step) : std::max(0.f, state.presence - step);
    }
    // velocity from the last two samples
    if (hasPointer){
        if (state.lastLocalValid && dt > 0.f)
            state.velocity = (local - state.lastLocal) / dt;
        else if (fixed)
            state.velocity = Rml::Vector2f(0.f, 0.f);
        state.lastLocal = local;
        state.lastLocalValid = true;
    }else{
        state.velocity = Rml::Vector2f(0.f, 0.f);
        state.lastLocalValid = false;
    }
    // the ripples age first (a ripple added below is as old as its press)
    if (m.pointer == EffectPointer::Ripples){
        int kept = 0;
        for (int i = 0; i < state.rippleCount; i++){
            Rml::Vector4f r = state.ripples[i];
            r.z += dt;
            if (r.z <= kRippleMaxAge)
                state.ripples[kept++] = r;
        }
        state.rippleCount = kept;
    }
    // entering and pressing
    if (inside){
        if (state.enterTime < 0.0)
            state.enterTime = now;
    }else{
        state.enterTime = -1.0;
    }
    if (pointer && pointer->pressCount != state.lastPressSeen){
        state.lastPressSeen = pointer->pressCount;
        if (element){
            const Rml::Vector2f press = toLocal(element, pointer->pressPosition, areaOrigin);
            const bool pressInside = press.x >= 0.f && press.y >= 0.f && press.x <= instance.areaSize.x && press.y <= instance.areaSize.y;
            if (pressInside){
                state.pressTime = pointer->pressClock;
                if (m.pointer == EffectPointer::Ripples){
                    // newest first; at most 8
                    for (int i = std::min(state.rippleCount, 7); i > 0; i--)
                        state.ripples[i] = state.ripples[i - 1];
                    state.ripples[0] = Rml::Vector4f(pointer->pressPosition.x, pointer->pressPosition.y, (float)(now - pointer->pressClock), 1.f);
                    state.rippleCount = std::min(state.rippleCount + 1, 8);
                }
            }
        }
    }

    // the trail: one sample a frame while moving (1 px or more), aged by dt, 4 s at most
    // (a stateful effect records it too: the pinned clock's replay reads the pointer history, phase 6 3.8)
    if ((m.pointer == EffectPointer::Trail || m.stateful()) && !(pointer && pointer->testTrail)){
        int kept = 0;
        for (int i = 0; i < state.trailCount; i++){
            Rml::Vector4f s = state.trail[i];
            s.z += dt;
            if (s.z <= kTrailMaxAge)
                state.trail[kept++] = s;
        }
        state.trailCount = kept;
        if (pointer && pointer->valid){
            const Rml::Vector2f d = pointer->position - state.lastTrailSample;
            if (!state.trailSampled || std::sqrt(d.x * d.x + d.y * d.y) >= 1.f){
                for (int i = std::min(state.trailCount, 31); i > 0; i--)
                    state.trail[i] = state.trail[i - 1];
                state.trail[0] = Rml::Vector4f(pointer->position.x, pointer->position.y, 0.f, 1.f);
                state.trailCount = std::min(state.trailCount + 1, 32);
                state.lastTrailSample = pointer->position;
                state.trailSampled = true;
            }
        }
    }
    // ripples: aged; one per 40 px of movement when the effect's trigger is "move"
    if (m.pointer == EffectPointer::Ripples){
        const EffectParamDef* trigger = m.findParam("trigger");
        const bool onMove = trigger && trigger->type == EffectParamType::Enum && !trigger->options.empty() &&
                            trigger->options[(size_t)std::max(0, (int)std::lround(paramNumber(instance, "trigger")))] == "move";
        if (onMove && hasPointer && inside){
            // the movement inside the area counts (entering starts from where it entered)
            const Rml::Vector2f d = state.rippleTracking ? pointer->position - state.lastRippleSample : Rml::Vector2f(0.f, 0.f);
            state.rippleTracking = true;
            state.moveSinceRipple += std::sqrt(d.x * d.x + d.y * d.y);
            state.lastRippleSample = pointer->position;
            if (state.moveSinceRipple >= kRippleMoveStep){
                state.moveSinceRipple = 0.f;
                for (int i = std::min(state.rippleCount, 7); i > 0; i--)
                    state.ripples[i] = state.ripples[i - 1];
                state.ripples[0] = Rml::Vector4f(pointer->position.x, pointer->position.y, 0.f, 0.6f);
                state.rippleCount = std::min(state.rippleCount + 1, 8);
            }
        }else{
            state.rippleTracking = false;
        }
    }
}

void EffectRuntime::prepare(EffectInstance& instance){
    if (!instance.manifest || !instance.element)
        return;
    Rml::Element* element = instance.element;
    const EffectManifest& m = *instance.manifest;

    // the area: the decorator's paint area, a filter's border box
    const Rml::BoxArea area = instance.filterForm ? Rml::BoxArea::Border : (Rml::BoxArea)instance.paintArea;
    const Rml::RenderBox box = element->GetRenderBox(area);
    instance.areaSize = box.GetFillSize();
    instance.areaOffset = box.GetFillOffset();
    const Rml::CornerSizes2 radius = box.GetBorderRadius();
    const Rml::EdgeSizes edges = box.GetBorderWidths();   // top right bottom left: the area's distance to the border edge
    // the area's corner radii: the border box's, less the edges between them (CSS inner radii)
    instance.radii[0] = std::max(0.f, std::min(radius[0].x - edges[3], radius[0].y - edges[0]));
    instance.radii[1] = std::max(0.f, std::min(radius[1].x - edges[1], radius[1].y - edges[0]));
    instance.radii[2] = std::max(0.f, std::min(radius[2].x - edges[1], radius[2].y - edges[2]));
    instance.radii[3] = std::max(0.f, std::min(radius[3].x - edges[3], radius[3].y - edges[2]));
    const Rml::Vector2f areaOrigin = element->GetAbsoluteOffset(Rml::BoxArea::Border) + instance.areaOffset;
    instance.areaOrigin = areaOrigin;

    const double now = EffectClock::now();
    const bool fixed = EffectClock::isFixed();
    EffectElementState& state = *instance.state;
    Rml::Context* context = instance.context ? instance.context : element->GetContext();
    if (!pointerCacheValid || pointerCacheContext != context){
        auto pointerIt = pointers.find(context);
        pointerCache = pointerIt != pointers.end() ? &pointerIt->second : nullptr;
        pointerCacheContext = context;
        pointerCacheValid = true;
    }
    const PointerState* pointer = pointerCache;

    float dt = 0.f;
    if (state.lastFrame != frame){
        dt = fixed || state.lastClock < 0.0 ? 0.f : (float)std::min(std::max(now - state.lastClock, 0.0), 0.25);
        state.lastClock = now;
        state.lastFrame = frame;
        integrate(instance, state, now, dt, fixed, areaOrigin, pointer);
    }
    if (instance.preparedFrame == frame)
        return;
    instance.preparedFrame = frame;

    EffectFrameBlock& f = instance.frame;
    const Rml::Vector2f size = instance.areaSize;
    set4(f.size, size.x, size.y, size.x > 0.f ? 1.f / size.x : 0.f, size.y > 0.f ? 1.f / size.y : 0.f);
    const float time = (float)(now * instance.timeScale + instance.timeOffset);
    float dpr = 1.f;
    if (context && context->GetDensityIndependentPixelRatio() > 0.f)
        dpr = context->GetDensityIndependentPixelRatio();
    set4(f.time, time, fixed ? 0.f : dt, dpr, fixed ? 1.f : 0.f);
    set4(f.anim, state.anim[0], state.anim[1], state.anim[2], state.anim[3]);

    const bool hasPointer = pointer && pointer->valid;
    Rml::Vector2f local(-kNone, -kNone);
    bool inside = false;
    if (hasPointer){
        local = toLocal(element, pointer->position, areaOrigin);
        inside = local.x >= 0.f && local.y >= 0.f && local.x <= size.x && local.y <= size.y;
    }
    set4(f.pointer, local.x, local.y, inside ? 1.f : 0.f, (hasPointer && pointer->down) ? 1.f : 0.f);
    const Rml::Vector2f smooth = state.smoothValid ? state.smoothPointer : local;
    set4(f.pointerSmooth, smooth.x, smooth.y, state.presence, 0.f);
    float sincePress = kNone;
    if (pointer && pointer->testOverride && pointer->testSincePress >= 0.f)
        sincePress = pointer->testSincePress;
    else if (state.pressTime >= 0.0)
        sincePress = (float)std::max(0.0, now - state.pressTime);
    const float sinceEnter = state.enterTime >= 0.0 ? (float)std::max(0.0, now - state.enterTime) : kNone;
    set4(f.pointerEx, state.velocity.x, state.velocity.y, sincePress, sinceEnter);
    set4(f.radii, instance.radii[0], instance.radii[1], instance.radii[2], instance.radii[3]);
    // misc.w: the paint form clips to the area's rounded box (@geometry fill)
    // the element's address string is built once per instance, not every frame
    if (instance.seed < 0.f)
        instance.seed = seedOf(element);
    set4(f.misc, instance.opacity, (float)instance.blend, instance.seed, instance.clipToArea ? 1.f : 0.f);

    // the trail and the ripples in local px, for the effects that read them (the others' stay
    // zero); the entries past the current ones are cleared
    if (m.pointer == EffectPointer::Trail){
        int count = 0;
        if (pointer && pointer->testTrail){
            for (size_t i = 0; i < pointer->trail.size() && i < 32; i++, count++){
                const Rml::Vector4f s = pointer->trail[i];
                const Rml::Vector2f p = toLocal(element, Rml::Vector2f(s.x, s.y), areaOrigin);
                set4(instance.trail[i], p.x, p.y, s.z, 1.f);
            }
        }else{
            for (int i = 0; i < state.trailCount && i < 32; i++, count++){
                const Rml::Vector4f s = state.trail[i];
                const Rml::Vector2f p = toLocal(element, Rml::Vector2f(s.x, s.y), areaOrigin);
                set4(instance.trail[i], p.x, p.y, s.z, s.w);
            }
        }
        for (int i = count; i < instance.trailUsed; i++)
            set4(instance.trail[i], 0.f, 0.f, 0.f, 0.f);
        instance.trailUsed = count;
    }
    if (m.pointer == EffectPointer::Ripples){
        int count = 0;
        for (int i = 0; i < state.rippleCount && i < 8; i++, count++){
            const Rml::Vector4f r = state.ripples[i];
            const Rml::Vector2f p = toLocal(element, Rml::Vector2f(r.x, r.y), areaOrigin);
            set4(instance.ripples[i], p.x, p.y, r.z, r.w);
        }
        for (int i = count; i < instance.ripplesUsed; i++)
            set4(instance.ripples[i], 0.f, 0.f, 0.f, 0.f);
        instance.ripplesUsed = count;
    }
}

// --- phase 6: stateful effects (docs/html-ui/phase-6-stateful-effects.md) ---

EffectSimulation::~EffectSimulation(){
    releaseImages();
    EffectRuntime::get().forgetSimulation(this);
}

void EffectSimulation::releaseImages(){
    EffectStatePool& pool = EffectStatePool::get();
    for (Images& images : states){
        for (auto& image : images.image){
            if (image)
                pool.release(std::move(image));
            image.reset();
        }
    }
    states.clear();
    allocated = false;
    bytes = 0;
    steps = 0;
    anchored = false;
    pinned = false;
    pointerKnown = false;
}

EffectSimulation* EffectRuntime::simulationOf(EffectInstance& instance){
    if (!instance.manifest || !instance.manifest->stateful() || !instance.state)
        return nullptr;
    EffectElementState& state = *instance.state;
    // a manifest that changed (a project effect's hot reload) restarts the simulation
    if (state.simulation && (state.simulation->manifest != instance.manifest || state.simulation->signature != instance.manifest->stateSignature()))
        state.simulation.reset();
    if (!state.simulation){
        state.simulation = std::make_shared<EffectSimulation>();
        state.simulation->manifest = instance.manifest;
        state.simulation->signature = instance.manifest->stateSignature();
    }
    if (instance.element)
        state.simulation->document = instance.element->GetOwnerDocument();
    return state.simulation.get();
}

void EffectRuntime::forgetSimulation(EffectSimulation* simulation){
    activeSimulations.erase(simulation);
}

int EffectRuntime::splat(Rml::Element* element, const EffectSplatData& splat, const std::string& effectName){
    auto it = elements.find(element);
    if (it == elements.end())
        return 0;
    int taken = 0;
    const bool pinnedClock = EffectClock::isFixed();
    for (auto& entry : it->second.states){
        EffectElementState& state = *entry.second;
        const std::string name = entry.first.substr(0, entry.first.rfind('#'));
        if (!effectName.empty() && name != effectName)
            continue;
        EffectSimulation* simulation = state.simulation.get();
        if (!simulation || state.liveInstances <= 0)
            continue;
        if (pinnedClock){
            // logged with the pinned time: the replay applies it at its last step
            simulation->log.push_back({*EffectClock::getFixedTime(), splat});
            while (simulation->log.size() > 64)
                simulation->log.erase(simulation->log.begin());
        }else{
            simulation->pending.push_back(splat);
            while (simulation->pending.size() > 8)
                simulation->pending.erase(simulation->pending.begin());
        }
        simulation->inputGeneration++;
        taken++;
    }
    if (taken > 0)
        element->GetContext() ? element->GetContext()->RequestNextUpdate(0) : (void)0;
    return taken;
}

int EffectRuntime::resetSimulations(Rml::Element* element, const std::string& effectName){
    auto it = elements.find(element);
    if (it == elements.end())
        return 0;
    int count = 0;
    for (auto& entry : it->second.states){
        const std::string name = entry.first.substr(0, entry.first.rfind('#'));
        if (!effectName.empty() && name != effectName)
            continue;
        if (entry.second->simulation){
            entry.second->simulation->releaseImages();
            entry.second->simulation->log.clear();
            entry.second->simulation->pending.clear();
            entry.second->simulation->inputGeneration++;
            count++;
        }
    }
    return count;
}

EffectSimulationInfo EffectRuntime::simulationInfo(Rml::Element* element, const std::string& effectName, int ordinal) const{
    EffectSimulationInfo info;
    auto it = elements.find(element);
    if (it == elements.end())
        return info;
    const std::string key = effectName + "#" + std::to_string(ordinal);
    for (const auto& entry : it->second.states){
        if (entry.first != key || !entry.second->simulation)
            continue;
        const EffectSimulation& sim = *entry.second->simulation;
        info.exists = sim.allocated;
        info.bytes = sim.bytes;
        info.steps = sim.steps;
        info.time = sim.simTime;
        info.paused = sim.paused || (sim.allocated && sim.lastAdvanceFrame + 1 < frame);
        break;
    }
    return info;
}

bool EffectRuntime::pointerOf(Rml::Context* context, Rml::Vector2f& position, bool& down, uint64_t& pressCount) const{
    auto it = pointers.find(context);
    if (it == pointers.end() || !it->second.valid)
        return false;
    position = it->second.position;
    down = it->second.down;
    pressCount = it->second.pressCount;
    return true;
}

void EffectRuntime::pointerHistory(const EffectInstance& instance, std::vector<Rml::Vector4f>& out) const{
    out.clear();
    Rml::Context* context = instance.context ? instance.context : (instance.element ? instance.element->GetContext() : nullptr);
    auto it = pointers.find(context);
    if (it != pointers.end() && it->second.testTrail){
        for (const Rml::Vector4f& sample : it->second.trail)
            out.push_back(sample);
        return;
    }
    if (!instance.state)
        return;
    for (int i = 0; i < instance.state->trailCount && i < 32; i++)
        out.push_back(instance.state->trail[i]);
}

size_t EffectRuntime::simulationCount() const{
    size_t count = 0;
    for (const auto& entry : elements){
        for (const auto& state : entry.second.states){
            if (state.second->simulation && state.second->simulation->allocated)
                count++;
        }
    }
    return count;
}

bool EffectRuntime::admitSimulation(EffectSimulation& simulation){
    if (!simulation.allocated && simulationCount() >= (size_t)statefulBudget.hardCap){
        if (!simulation.refused){
            simulation.refused = true;
            reportStateful(simulation.document, "HTMLUI-FX-0018",
                           "the process already runs " + std::to_string(statefulBudget.hardCap) + " stateful simulations (the hard cap): '" +
                           (simulation.manifest ? simulation.manifest->name : std::string()) + "' is not simulated until one ends", true);
        }
        return false;
    }
    simulation.refused = false;
    activeSimulations[&simulation] = frame;
    return true;
}

void EffectRuntime::clearStatefulDiagnostics(){
    statefulReports.clear();
    overBudgetDocuments.clear();
    hardCapReported = false;
}

void EffectRuntime::reportStateful(Rml::ElementDocument* document, const std::string& codeId, const std::string& message, bool error){
    std::string where = document ? document->GetSourceURL() : std::string();
    const std::string text = (where.empty() ? std::string() : where + ": ") + message + " [" + codeId + "]";
    statefulReports.push_back({codeId, text});
    if (error)
        Log::error("[HTML UI] %s", text.c_str());
    else
        Log::warn("[HTML UI] %s", text.c_str());
}

void EffectRuntime::sweepSimulations(){
    // the simulations of states no live instance uses (the effect changed or was removed): their
    // images go back to the pool; the CPU state stays in the element's LRU
    size_t images = 0, bytes = 0;
    for (auto& entry : elements){
        for (auto& state : entry.second.states){
            EffectElementState& st = *state.second;
            if (!st.simulation)
                continue;
            if (st.liveInstances <= 0 && st.orphanFrame != UINT64_MAX && frame > st.orphanFrame + 1){
                st.simulation.reset();
                continue;
            }
            if (st.simulation->allocated){
                for (const auto& image : st.simulation->states)
                    images += image.image[1] ? 2 : 1;
                bytes += st.simulation->bytes;
            }
        }
    }
    // active: stepped this frame or the one before (the map keeps the frame of the last step)
    std::unordered_map<Rml::ElementDocument*, std::pair<int, size_t>> perDocument;
    size_t active = 0;
    for (auto it = activeSimulations.begin(); it != activeSimulations.end();){
        if (it->second + 2 < frame){
            it = activeSimulations.erase(it);
            continue;
        }
        active++;
        auto& counts = perDocument[it->first->document];
        counts.first++;
        counts.second += it->first->bytes;
        ++it;
    }
    totals.statefulActive = active;
    totals.stateImages = images;
    totals.stateBytes = bytes;
    totals.statePoolBytes = EffectStatePool::get().pooledBytes();
    frameTotals.stateImages = images;
    frameTotals.stateBytes = bytes;
    frameTotals.statePoolBytes = totals.statePoolBytes;
    // soft budgets: one warning per document while it is over
    for (const auto& doc : perDocument){
        const bool over = doc.second.first > statefulBudget.perDocument || doc.second.second > statefulBudget.bytesPerDocument ||
                          (int)active > statefulBudget.global;
        if (!over){
            overBudgetDocuments.erase(doc.first);
            continue;
        }
        if (overBudgetDocuments.count(doc.first))
            continue;
        overBudgetDocuments[doc.first] = (int)frame;
        reportStateful(doc.first, "HTMLUI-FX-0018",
                       std::to_string(doc.second.first) + " stateful simulations active in this document (" +
                       std::to_string(doc.second.second / (1024 * 1024)) + " MB of state), " + std::to_string(active) +
                       " in all: over the budget of " + std::to_string(statefulBudget.perDocument) + " per document, " +
                       std::to_string(statefulBudget.global) + " in all and " +
                       std::to_string(statefulBudget.bytesPerDocument / (1024 * 1024)) + " MB per document", false);
    }
    EffectStatePool::get().beginFrame();
}
