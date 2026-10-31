// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CurveFieldLogic.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the smallest horizontal handle extent, pixels: a handle dragged past the key's vertical
    // keeps a steep but finite slope
    constexpr float MIN_HANDLE_DX = 0.5f;

    bool nearlyEqual(float a, float b){
        if (std::isinf(a) || std::isinf(b)) return a == b;
        const float scale = std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
        return std::fabs(a - b) <= 1e-4f * scale;
    }

    float secant(const CurveKey& a, const CurveKey& b){
        const float dt = b.time - a.time;
        return (dt > 0.0f) ? (b.value - a.value) / dt : 0.0f;
    }

    // the slopes setLinear gives a key
    void linearSlopes(const AnimationCurve& curve, size_t index, float& in, float& out){
        const size_t n = curve.keys.size();
        in = out = 0.0f;
        if (n < 2) return;
        if (index == 0){
            in = out = secant(curve.keys[0], curve.keys[1]);
        }else if (index == n - 1){
            in = out = secant(curve.keys[n - 2], curve.keys[n - 1]);
        }else{
            in = secant(curve.keys[index - 1], curve.keys[index]);
            out = secant(curve.keys[index], curve.keys[index + 1]);
        }
    }

    std::vector<char> autoFlags(const AnimationCurve& curve){
        std::vector<char> flags(curve.keys.size(), 0);
        for (size_t i = 0; i < curve.keys.size(); i++){
            flags[i] = curvefield::detectMode(curve, i) == curvefield::TangentMode::Auto ? 1 : 0;
        }
        return flags;
    }

    // recomputes the flagged keys, left to right (an Auto slope depends on values only)
    void applyAuto(AnimationCurve& curve, const std::vector<char>& flags){
        for (size_t i = 0; i < curve.keys.size() && i < flags.size(); i++){
            if (flags[i]) curvefield::setAuto(curve, i);
        }
    }

}

// ---- the view ----

float curvefield::View::pixelsPerTime() const{
    const float span = timeMax - timeMin;
    const float ppt = (span > 0.0f) ? canvas.width / span : 0.0f;
    return (ppt > 0.0f && std::isfinite(ppt)) ? ppt : 1.0f;
}

float curvefield::View::pixelsPerValue() const{
    const float span = valueMax - valueMin;
    const float ppv = (span > 0.0f) ? canvas.height / span : 0.0f;
    return (ppv > 0.0f && std::isfinite(ppv)) ? ppv : 1.0f;
}

float curvefield::View::timeToX(float time) const{
    return canvas.x + (time - timeMin) * pixelsPerTime();
}

float curvefield::View::valueToY(float value) const{
    return canvas.y + canvas.height - (value - valueMin) * pixelsPerValue();
}

float curvefield::View::xToTime(float x) const{
    return timeMin + (x - canvas.x) / pixelsPerTime();
}

float curvefield::View::yToValue(float y) const{
    return valueMin + (canvas.y + canvas.height - y) / pixelsPerValue();
}

curvefield::Point curvefield::View::toScreen(float time, float value) const{
    return Point{timeToX(time), valueToY(value)};
}

curvefield::View curvefield::fitView(const AnimationCurve& curve, const Rect& canvas, const AnimationCurve* second,
                                     float timeMargin, float valueMargin){
    View view;
    view.canvas = canvas;

    const AnimationCurve* curves[2] = {&curve, second};
    bool anyKey = false;
    float tMin = 0.0f, tMax = 0.0f;
    for (const AnimationCurve* c : curves){
        if (!c || c->keys.empty()) continue;
        if (!anyKey){
            tMin = c->firstTime();
            tMax = c->lastTime();
            anyKey = true;
        }else{
            tMin = std::min(tMin, c->firstTime());
            tMax = std::max(tMax, c->lastTime());
        }
    }
    if (!anyKey){
        tMin = 0.0f;
        tMax = 1.0f;
    }else if (!(tMax - tMin > 1e-6f)){
        tMax = tMin + 1.0f;
    }

    bool anyValue = false;
    float vMin = 0.0f, vMax = 0.0f;
    for (const AnimationCurve* c : curves){
        if (!c) continue;
        float lo = 0.0f, hi = 0.0f;
        c->valueRange(tMin, tMax, lo, hi);
        if (!std::isfinite(lo) || !std::isfinite(hi)) continue;
        if (!anyValue){
            vMin = lo;
            vMax = hi;
            anyValue = true;
        }else{
            vMin = std::min(vMin, lo);
            vMax = std::max(vMax, hi);
        }
    }
    if (!(vMax - vMin > 1e-6f)){
        const float v = 0.5f * (vMin + vMax);
        const float range = std::max(1.0f, std::fabs(v));
        vMin = v - 0.5f * range;
        vMax = v + 0.5f * range;
    }

    const float dt = (tMax - tMin) * std::max(0.0f, timeMargin);
    const float dv = (vMax - vMin) * std::max(0.0f, valueMargin);
    view.timeMin = tMin - dt;
    view.timeMax = tMax + dt;
    view.valueMin = vMin - dv;
    view.valueMax = vMax + dv;
    return view;
}

curvefield::View curvefield::zoomView(const View& view, Point anchor, float timeFactor, float valueFactor){
    View out = view;
    if (timeFactor > 0.0f && std::isfinite(timeFactor)){
        const float t = view.xToTime(anchor.x);
        const float tMin = t - (t - view.timeMin) / timeFactor;
        const float tMax = t + (view.timeMax - t) / timeFactor;
        if (tMax - tMin > 1e-5f && std::isfinite(tMin) && std::isfinite(tMax)){
            out.timeMin = tMin;
            out.timeMax = tMax;
        }
    }
    if (valueFactor > 0.0f && std::isfinite(valueFactor)){
        const float v = view.yToValue(anchor.y);
        const float vMin = v - (v - view.valueMin) / valueFactor;
        const float vMax = v + (view.valueMax - v) / valueFactor;
        if (vMax - vMin > 1e-5f && std::isfinite(vMin) && std::isfinite(vMax)){
            out.valueMin = vMin;
            out.valueMax = vMax;
        }
    }
    return out;
}

curvefield::View curvefield::panView(const View& view, float dxPx, float dyPx){
    View out = view;
    const float dt = dxPx / view.pixelsPerTime();
    const float dv = dyPx / view.pixelsPerValue();
    out.timeMin -= dt;
    out.timeMax -= dt;
    out.valueMin += dv;
    out.valueMax += dv;
    return out;
}

// ---- hit testing ----

curvefield::Point curvefield::keyPosition(const View& view, const CurveKey& key){
    return view.toScreen(key.time, key.value);
}

int curvefield::hitKey(const AnimationCurve& curve, const View& view, Point mouse, float radiusPx){
    int best = -1;
    float bestDistance = radiusPx * radiusPx;
    for (size_t i = 0; i < curve.keys.size(); i++){
        const Point p = keyPosition(view, curve.keys[i]);
        const float dx = p.x - mouse.x, dy = p.y - mouse.y;
        const float d = dx * dx + dy * dy;
        if (d <= bestDistance){
            bestDistance = d;
            best = (int)i;
        }
    }
    return best;
}

bool curvefield::hasTangentHandle(const AnimationCurve& curve, size_t key, TangentSide side){
    if (key >= curve.keys.size()) return false;
    if (side == TangentSide::In){
        return key > 0 && std::isfinite(curve.keys[key].inTangent);
    }
    return key + 1 < curve.keys.size() && std::isfinite(curve.keys[key].outTangent);
}

curvefield::Point curvefield::tangentHandlePosition(const AnimationCurve& curve, size_t key, TangentSide side, const View& view,
                                                    float lengthPx){
    if (key >= curve.keys.size()) return Point{};
    const CurveKey& k = curve.keys[key];
    const Point origin = keyPosition(view, k);
    const float slope = (side == TangentSide::In) ? k.inTangent : k.outTangent;
    if (!std::isfinite(slope)) return origin;
    // one time unit to the right, `slope` value units up, in pixels
    float dx = view.pixelsPerTime();
    float dy = -slope * view.pixelsPerValue();
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0.0f) || !std::isfinite(length)) return origin;
    dx /= length;
    dy /= length;
    if (side == TangentSide::In){
        dx = -dx;
        dy = -dy;
    }
    return Point{origin.x + dx * lengthPx, origin.y + dy * lengthPx};
}

curvefield::TangentHit curvefield::hitTangentHandle(const AnimationCurve& curve, const View& view, int key, Point mouse,
                                                    float radiusPx, float lengthPx){
    TangentHit hit;
    if (key < 0 || (size_t)key >= curve.keys.size()) return hit;
    float bestDistance = radiusPx * radiusPx;
    for (TangentSide side : {TangentSide::In, TangentSide::Out}){
        if (!hasTangentHandle(curve, (size_t)key, side)) continue;
        const Point p = tangentHandlePosition(curve, (size_t)key, side, view, lengthPx);
        const float dx = p.x - mouse.x, dy = p.y - mouse.y;
        const float d = dx * dx + dy * dy;
        if (d <= bestDistance){
            bestDistance = d;
            hit.key = key;
            hit.side = side;
        }
    }
    return hit;
}

// ---- tangent modes ----

bool curvefield::isBroken(const CurveKey& key){
    return key.inTangent != key.outTangent;
}

float curvefield::autoTangent(const AnimationCurve& curve, size_t index){
    const size_t n = curve.keys.size();
    if (n < 3 || index == 0 || index >= n - 1) return 0.0f;
    const CurveKey& k0 = curve.keys[index - 1];
    const CurveKey& k1 = curve.keys[index];
    const CurveKey& k2 = curve.keys[index + 1];
    const float left = secant(k0, k1);
    const float right = secant(k1, k2);
    // a local extremum (or a flat side): flat, no overshoot
    if (left * right <= 0.0f) return 0.0f;
    const float span = k2.time - k0.time;
    if (!(span > 0.0f)) return 0.0f;
    const float slope = (k2.value - k0.value) / span;
    // a Hermite segment stays monotone while its end slopes are within 3x its secant
    const float limit = 3.0f * std::min(std::fabs(left), std::fabs(right));
    return std::max(-limit, std::min(limit, slope));
}

void curvefield::setAuto(AnimationCurve& curve, size_t index){
    if (index >= curve.keys.size()) return;
    const float slope = autoTangent(curve, index);
    curve.keys[index].inTangent = slope;
    curve.keys[index].outTangent = slope;
}

void curvefield::setLinear(AnimationCurve& curve, size_t index){
    if (index >= curve.keys.size()) return;
    float in = 0.0f, out = 0.0f;
    linearSlopes(curve, index, in, out);
    curve.keys[index].inTangent = in;
    curve.keys[index].outTangent = out;
}

void curvefield::setConstant(AnimationCurve& curve, size_t index){
    if (index >= curve.keys.size()) return;
    curve.keys[index].inTangent = AnimationCurve::STEP;
    curve.keys[index].outTangent = AnimationCurve::STEP;
}

void curvefield::setFree(const AnimationCurve& curve, size_t index, std::vector<char>& broken){
    if (index >= curve.keys.size()) return;
    if (broken.size() < curve.keys.size()) broken.resize(curve.keys.size(), 0);
    broken[index] = 1;
}

void curvefield::setMode(AnimationCurve& curve, size_t index, TangentMode mode){
    switch (mode){
        case TangentMode::Auto: setAuto(curve, index); break;
        case TangentMode::Linear: setLinear(curve, index); break;
        case TangentMode::Constant: setConstant(curve, index); break;
        case TangentMode::Free: break;
    }
}

curvefield::TangentMode curvefield::detectMode(const AnimationCurve& curve, size_t index){
    if (index >= curve.keys.size()) return TangentMode::Free;
    const CurveKey& key = curve.keys[index];
    if (std::isinf(key.inTangent) && std::isinf(key.outTangent)) return TangentMode::Constant;
    const float a = autoTangent(curve, index);
    if (nearlyEqual(key.inTangent, a) && nearlyEqual(key.outTangent, a)) return TangentMode::Auto;
    float in = 0.0f, out = 0.0f;
    linearSlopes(curve, index, in, out);
    if (nearlyEqual(key.inTangent, in) && nearlyEqual(key.outTangent, out)) return TangentMode::Linear;
    return TangentMode::Free;
}

// ---- key edits ----

int curvefield::insertKey(AnimationCurve& curve, float time){
    if (!std::isfinite(time)) return -1;
    for (const CurveKey& key : curve.keys){
        if (key.time == time) return -1;
    }
    std::vector<char> flags = autoFlags(curve);
    const int index = curve.addKey(CurveKey(time, curve.evaluate(time)));
    if (index < 0) return -1;
    flags.insert(flags.begin() + index, 1);
    applyAuto(curve, flags);
    return index;
}

bool curvefield::deleteKey(AnimationCurve& curve, size_t index){
    if (index >= curve.keys.size() || curve.keys.size() <= 1) return false;
    std::vector<char> flags = autoFlags(curve);
    curve.removeKey(index);
    flags.erase(flags.begin() + (long)index);
    applyAuto(curve, flags);
    return true;
}

int curvefield::moveKey(AnimationCurve& curve, size_t index, float time, float value){
    if (index >= curve.keys.size() || !std::isfinite(time) || !std::isfinite(value)) return -1;
    CurveKey key = curve.keys[index];
    for (size_t i = 0; i < curve.keys.size(); i++){
        if (i != index && curve.keys[i].time == time){
            time = key.time;
            break;
        }
    }
    key.time = time;
    key.value = value;

    std::vector<char> flags = autoFlags(curve);
    const char moved = flags[index];
    const CurveKey original = curve.keys[index];
    flags.erase(flags.begin() + (long)index);
    curve.keys.erase(curve.keys.begin() + (long)index);
    const int to = curve.addKey(key);
    if (to < 0){
        // not reached (finite, no other key at that time): restore
        curve.keys.insert(curve.keys.begin() + (long)index, original);
        return -1;
    }
    flags.insert(flags.begin() + to, moved);
    applyAuto(curve, flags);
    return to;
}

bool curvefield::dragTangent(AnimationCurve& curve, size_t key, TangentSide side, Point handle, const View& view, bool broken){
    if (key >= curve.keys.size()) return false;
    CurveKey& k = curve.keys[key];
    const Point origin = keyPosition(view, k);
    const float dx = handle.x - origin.x;
    const float dy = handle.y - origin.y;
    // the handle's horizontal extent away from the key, and its rise (screen up = value up)
    const float extent = std::max(MIN_HANDLE_DX, (side == TangentSide::Out) ? dx : -dx);
    const float rise = (side == TangentSide::Out) ? -dy : dy;
    const float slope = (rise / view.pixelsPerValue()) / (extent / view.pixelsPerTime());
    if (!std::isfinite(slope)) return false;
    if (side == TangentSide::In){
        k.inTangent = slope;
        if (!broken) k.outTangent = slope;
    }else{
        k.outTangent = slope;
        if (!broken) k.inTangent = slope;
    }
    return true;
}

// ---- presets ----

const char* curvefield::presetName(CurvePreset preset){
    switch (preset){
        case CurvePreset::Constant: return "Constant";
        case CurvePreset::LinearUp: return "Linear Up";
        case CurvePreset::LinearDown: return "Linear Down";
        case CurvePreset::EaseIn: return "Ease In";
        case CurvePreset::EaseOut: return "Ease Out";
        case CurvePreset::EaseInOut: return "Ease In Out";
    }
    return "";
}

AnimationCurve curvefield::presetCurve(CurvePreset preset, float low, float high){
    const float d = high - low;
    switch (preset){
        case CurvePreset::Constant:
            return AnimationCurve({CurveKey(0.0f, high), CurveKey(1.0f, high)});
        case CurvePreset::LinearUp:
            return AnimationCurve({CurveKey(0.0f, low, d, d), CurveKey(1.0f, high, d, d)});
        case CurvePreset::LinearDown:
            return AnimationCurve({CurveKey(0.0f, high, -d, -d), CurveKey(1.0f, low, -d, -d)});
        case CurvePreset::EaseIn:
            return AnimationCurve({CurveKey(0.0f, low, 0.0f, 0.0f), CurveKey(1.0f, high, 2.0f * d, 2.0f * d)});
        case CurvePreset::EaseOut:
            return AnimationCurve({CurveKey(0.0f, low, 2.0f * d, 2.0f * d), CurveKey(1.0f, high, 0.0f, 0.0f)});
        case CurvePreset::EaseInOut:
            return AnimationCurve({CurveKey(0.0f, low), CurveKey(1.0f, high)});
    }
    return AnimationCurve({CurveKey(0.0f, high), CurveKey(1.0f, high)});
}

AnimationCurve curvefield::presetCurve(CurvePreset preset, const AnimationCurve& current){
    float low = 0.0f, high = 1.0f;
    if (!current.keys.empty()){
        low = high = current.keys[0].value;
        for (const CurveKey& key : current.keys){
            low = std::min(low, key.value);
            high = std::max(high, key.value);
        }
        if (!(high - low > 1e-6f)){
            const float v = high;
            low = std::min(0.0f, v);
            high = std::max(0.0f, v);
            if (low == high){
                low = 0.0f;
                high = 1.0f;
            }
        }
    }
    AnimationCurve curve = presetCurve(preset, low, high);
    curve.preWrap = current.preWrap;
    curve.postWrap = current.postWrap;
    return curve;
}
