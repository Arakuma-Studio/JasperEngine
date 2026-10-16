// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effects' per-element runtime (phase-5 spec 3.4, 3.6): instances, the state that survives
// re-instancing, the clock, the pointer, and the frame block.

#include "EffectRuntime.h"

#include "HtmlUIClock.h"
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
    const uint64_t key = SystemRender::getFrameCount();
    if (key == frameKey)
        return;
    frameKey = key;
    frame++;
    frameTotals = EffectStats();
    frameTotals.liveInstances = live.size();
}

void EffectRuntime::countReinstance(){
    totals.reinstances++;
    frameTotals.reinstances++;
}

void EffectRuntime::onPointerMove(Rml::Context* context, Rml::Vector2f contextPx){
    PointerState& p = pointers[context];
    if (p.testOverride)
        return;
    p.position = contextPx;
    p.valid = true;
}

void EffectRuntime::onPointerButton(Rml::Context* context, Rml::Vector2f contextPx, bool down){
    PointerState& p = pointers[context];
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
    if (p.testOverride)
        return;
    p.valid = false;
    p.down = false;
}

void EffectRuntime::setTestPointer(Rml::Context* context, std::optional<Rml::Vector2f> contextPx, bool down, float sincePress){
    PointerState& p = pointers[context];
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

    auto instance = std::make_unique<EffectInstance>();
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
    live.erase(it);
    totals.liveInstances = live.size();
    frameTotals.liveInstances = live.size();
}

bool EffectRuntime::isLive(const EffectInstance* instance) const{
    return live.count(const_cast<EffectInstance*>(instance)) > 0;
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
}

void EffectRuntime::OnDocumentUnload(Rml::ElementDocument* /*document*/){
    // the elements' destruction purges their states
}

void EffectRuntime::OnShutdown(){
    elements.clear();
    pointers.clear();
}

void EffectRuntime::integrate(EffectInstance& instance, EffectElementState& state, double now, float dt, bool fixed,
                              Rml::Vector2f areaOrigin, const PointerState* pointer){
    const EffectManifest& m = *instance.manifest;
    Rml::Element* element = instance.element;
    const float time = (float)(now * instance.timeScale + instance.timeOffset);

    // integrated clocks (@time): never jump when a rate changes, the closed form when fixed
    for (size_t i = 0; i < m.timeParams.size() && i < 4; i++){
        const float rate = paramNumber(instance, m.timeParams[i]);
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
    if (m.pointer == EffectPointer::Trail && !(pointer && pointer->testTrail)){
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

    const double now = EffectClock::now();
    const bool fixed = EffectClock::isFixed();
    EffectElementState& state = *instance.state;
    Rml::Context* context = element->GetContext();
    auto pointerIt = pointers.find(context);
    const PointerState* pointer = pointerIt != pointers.end() ? &pointerIt->second : nullptr;

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

    // the trail and the ripples in local px
    for (int i = 0; i < 32; i++)
        set4(instance.trail[i], 0.f, 0.f, 0.f, 0.f);
    for (int i = 0; i < 8; i++)
        set4(instance.ripples[i], 0.f, 0.f, 0.f, 0.f);
    if (m.pointer == EffectPointer::Trail){
        if (pointer && pointer->testTrail){
            for (size_t i = 0; i < pointer->trail.size() && i < 32; i++){
                const Rml::Vector4f s = pointer->trail[i];
                const Rml::Vector2f p = toLocal(element, Rml::Vector2f(s.x, s.y), areaOrigin);
                set4(instance.trail[i], p.x, p.y, s.z, 1.f);
            }
        }else{
            for (int i = 0; i < state.trailCount; i++){
                const Rml::Vector4f s = state.trail[i];
                const Rml::Vector2f p = toLocal(element, Rml::Vector2f(s.x, s.y), areaOrigin);
                set4(instance.trail[i], p.x, p.y, s.z, s.w);
            }
        }
    }
    if (m.pointer == EffectPointer::Ripples){
        for (int i = 0; i < state.rippleCount; i++){
            const Rml::Vector4f r = state.ripples[i];
            const Rml::Vector2f p = toLocal(element, Rml::Vector2f(r.x, r.y), areaOrigin);
            set4(instance.ripples[i], p.x, p.y, r.z, r.w);
        }
    }
}
