// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CurveEditorOps.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::curveops;

namespace {

    bool selected(const std::vector<KeyRef>& selection, int curve, int key) {
        return std::find(selection.begin(), selection.end(), KeyRef{curve, key}) != selection.end();
    }

    void clampKey(CurveKey& key, const CurveRange* range) {
        if (!range || !range->clamp) return;
        key.time = std::clamp(key.time, std::min(range->t0, range->t1), std::max(range->t0, range->t1));
        key.value = std::clamp(key.value, std::min(range->v0, range->v1), std::max(range->v0, range->v1));
    }

    float distance(float ax, float ay, float bx, float by) {
        return std::hypot(ax - bx, ay - by);
    }

}

void curveops::handlePosition(const CurveKey& key, Side side, const View& view, float length, float& x, float& y) {
    const float slope = side == Side::In ? key.inTangent : key.outTangent;
    float dx = view.scaleX();
    float dy = std::isfinite(slope) ? -slope * view.scaleY() : 0.0f;
    const float len = std::hypot(dx, dy);
    if (len > 0.0f) {
        dx = dx / len * length;
        dy = dy / len * length;
    }
    const float sign = side == Side::In ? -1.0f : 1.0f;
    x = view.toX(key.time) + sign * dx;
    y = view.toY(key.value) + sign * dy;
}

Hit curveops::hitTest(const std::vector<const Curve*>& curves, const std::vector<bool>& editable, const View& view, float x, float y,
                      float radius, float handleLength, const std::vector<KeyRef>& selection) {
    Hit hit;
    // the handles of the selected keys
    for (const KeyRef& ref : selection) {
        if (ref.curve < 0 || (size_t)ref.curve >= curves.size() || !editable[(size_t)ref.curve]) continue;
        const Curve& curve = *curves[(size_t)ref.curve];
        if (ref.key < 0 || (size_t)ref.key >= curve.keys.size()) continue;
        const CurveKey& key = curve.keys[(size_t)ref.key];
        for (Side side : {Side::In, Side::Out}) {
            if (side == Side::In && ref.key == 0) continue;
            if (side == Side::Out && (size_t)ref.key + 1 == curve.keys.size()) continue;
            float hx, hy;
            handlePosition(key, side, view, handleLength, hx, hy);
            if (distance(hx, hy, x, y) <= radius) {
                hit.kind = side == Side::In ? Hit::Kind::InHandle : Hit::Kind::OutHandle;
                hit.curve = ref.curve;
                hit.key = ref.key;
                return hit;
            }
        }
    }
    // the nearest key
    float best = radius;
    for (size_t c = 0; c < curves.size(); c++) {
        if (!editable[c]) continue;
        for (size_t k = 0; k < curves[c]->keys.size(); k++) {
            const CurveKey& key = curves[c]->keys[k];
            const float d = distance(view.toX(key.time), view.toY(key.value), x, y);
            if (d <= best) {
                best = d;
                hit.kind = Hit::Kind::Key;
                hit.curve = (int)c;
                hit.key = (int)k;
            }
        }
    }
    if (hit.kind != Hit::Kind::None) return hit;
    // a curve under the point
    const float time = view.toTime(x);
    best = radius;
    for (size_t c = 0; c < curves.size(); c++) {
        if (!editable[c] || curves[c]->keys.empty()) continue;
        const float d = std::fabs(view.toY(curves[c]->evaluate(time)) - y);
        if (d <= best) {
            best = d;
            hit.kind = Hit::Kind::Curve;
            hit.curve = (int)c;
            hit.time = time;
        }
    }
    return hit;
}

int curveops::addKeyOnCurve(Curve& curve, float time, const CurveRange* range) {
    return addKeyAt(curve, time, curve.evaluate(time), range);
}

int curveops::addKeyAt(Curve& curve, float time, float value, const CurveRange* range) {
    CurveKey key;
    key.time = time;
    key.value = value;
    clampKey(key, range);
    key.setBothModes(CurveTangentMode::ClampedAuto);
    return curve.addKey(key);
}

void curveops::deleteKeys(std::vector<Curve*>& curves, std::vector<KeyRef>& selection) {
    std::vector<KeyRef> sorted = selection;
    // from the last index down, so the indices of the keys still to delete hold
    std::sort(sorted.begin(), sorted.end(), [](const KeyRef& a, const KeyRef& b) { return a.curve != b.curve ? a.curve < b.curve : a.key > b.key; });
    for (const KeyRef& ref : sorted) {
        if (ref.curve < 0 || (size_t)ref.curve >= curves.size()) continue;
        Curve& curve = *curves[(size_t)ref.curve];
        if (curve.keys.size() <= 1 || ref.key < 0 || (size_t)ref.key >= curve.keys.size()) continue;
        curve.removeKey((size_t)ref.key);
    }
    selection.clear();
}

void curveops::moveKeys(std::vector<Curve*>& curves, const std::vector<Curve>& original, std::vector<KeyRef>& selection, float dt, float dv,
                        const CurveRange* range) {
    std::vector<KeyRef> moved;
    for (size_t c = 0; c < curves.size() && c < original.size(); c++) {
        const Curve& from = original[c];
        struct Entry {
            CurveKey key;
            bool selected;
        };
        std::vector<Entry> entries;
        bool any = false;
        for (size_t k = 0; k < from.keys.size(); k++) {
            const bool sel = selected(selection, (int)c, (int)k);
            any = any || sel;
            entries.push_back({from.keys[k], sel});
        }
        if (!any) continue;
        for (Entry& e : entries) {
            if (!e.selected) continue;
            CurveKey key = e.key;
            key.time += dt;
            key.value += dv;
            clampKey(key, range);
            // never onto a key that stays: the old time is kept
            bool clash = false;
            for (const Entry& other : entries) {
                if (!other.selected && other.key.time == key.time) clash = true;
            }
            if (clash) key.time = e.key.time;
            e.key = key;
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.key.time < b.key.time; });
        Curve& curve = *curves[c];
        curve.keys.clear();
        for (size_t k = 0; k < entries.size(); k++) {
            curve.keys.push_back(entries[k].key);
            if (entries[k].selected) moved.push_back({(int)c, (int)k});
        }
        curve.preWrapMode = from.preWrapMode;
        curve.postWrapMode = from.postWrapMode;
        curve.updateAllTangents();
    }
    selection = moved;
}

void curveops::setTangentMode(Curve& curve, size_t key, Side side, CurveTangentMode mode) {
    if (key >= curve.keys.size()) return;
    CurveKey& k = curve.keys[key];
    if (side == Side::Both) {
        const bool broken = k.broken();
        k.setInMode(mode);
        k.setOutMode(mode);
        k.setBroken(broken);
    } else {
        if (side == Side::In) k.setInMode(mode);
        else k.setOutMode(mode);
        k.setBroken(true);
    }
    curve.updateTangents(key);
}

void curveops::setClampedAuto(Curve& curve, size_t key) {
    if (key >= curve.keys.size()) return;
    curve.keys[key].setBothModes(CurveTangentMode::ClampedAuto);
    curve.updateTangents(key);
}

void curveops::setAuto(Curve& curve, size_t key) {
    if (key >= curve.keys.size()) return;
    curve.keys[key].setBothModes(CurveTangentMode::Auto);
    curve.updateTangents(key);
}

void curveops::setFreeSmooth(Curve& curve, size_t key) {
    if (key >= curve.keys.size()) return;
    CurveKey& k = curve.keys[key];
    // the sides joined at their mean slope (a step side counts as flat)
    const float in = std::isfinite(k.inTangent) ? k.inTangent : 0.0f;
    const float out = std::isfinite(k.outTangent) ? k.outTangent : 0.0f;
    const float slope = key == 0 ? out : (key + 1 == curve.keys.size() ? in : 0.5f * (in + out));
    k.setBothModes(CurveTangentMode::Free);
    k.inTangent = slope;
    k.outTangent = slope;
    curve.updateTangents(key);
}

void curveops::setFlat(Curve& curve, size_t key) {
    if (key >= curve.keys.size()) return;
    CurveKey& k = curve.keys[key];
    k.setBothModes(CurveTangentMode::Free);
    k.inTangent = 0.0f;
    k.outTangent = 0.0f;
    curve.updateTangents(key);
}

void curveops::setBroken(Curve& curve, size_t key, bool broken) {
    if (key >= curve.keys.size()) return;
    CurveKey& k = curve.keys[key];
    if (broken) {
        // each side keeps its tangent, now Free
        k.setInMode(CurveTangentMode::Free);
        k.setOutMode(CurveTangentMode::Free);
        k.setBroken(true);
    } else {
        setFreeSmooth(curve, key);
        return;
    }
    curve.updateTangents(key);
}

void curveops::dragTangent(Curve& curve, size_t key, Side side, float slope) {
    if (key >= curve.keys.size() || !std::isfinite(slope)) return;
    CurveKey& k = curve.keys[key];
    if (k.broken()) {
        if (side == Side::In) {
            k.setInMode(CurveTangentMode::Free);
            k.inTangent = slope;
        } else {
            k.setOutMode(CurveTangentMode::Free);
            k.outTangent = slope;
        }
    } else {
        k.setBothModes(CurveTangentMode::Free);
        k.inTangent = slope;
        k.outTangent = slope;
    }
}

float curveops::slopeTowards(const CurveKey& key, Side side, const View& view, float x, float y) {
    float dx = view.toTime(x) - key.time;
    const float dv = view.toValue(y) - key.value;
    // a handle cannot cross to the other side of its key
    if (side == Side::In) dx = std::min(dx, -1e-6f * std::fabs(view.t1 - view.t0));
    else dx = std::max(dx, 1e-6f * std::fabs(view.t1 - view.t0));
    return dv / dx;
}

void curveops::clampToRange(Curve& curve, const CurveRange& range) {
    if (!range.clamp) return;
    for (CurveKey& key : curve.keys) clampKey(key, &range);
    curve.sortKeys();
    // keys clamped onto one time: the first stays
    for (size_t i = 1; i < curve.keys.size();) {
        if (curve.keys[i].time == curve.keys[i - 1].time) curve.keys.erase(curve.keys.begin() + (std::ptrdiff_t)i);
        else i++;
    }
    curve.updateAllTangents();
}

Curve curveops::remap(const Curve& normalised, const CurveRange& range) {
    Curve out = normalised;
    const float ts = range.t1 - range.t0;
    const float vs = range.v1 - range.v0;
    const float slopeScale = ts != 0.0f ? vs / ts : 0.0f;
    for (CurveKey& key : out.keys) {
        key.time = range.t0 + key.time * ts;
        key.value = range.v0 + key.value * vs;
        if (std::isfinite(key.inTangent)) key.inTangent *= slopeScale;
        if (std::isfinite(key.outTangent)) key.outTangent *= slopeScale;
    }
    return out;
}

Curve curveops::normalise(const Curve& curve, const CurveRange& range) {
    Curve out = curve;
    const float ts = range.t1 - range.t0;
    const float vs = range.v1 - range.v0;
    const float slopeScale = vs != 0.0f ? ts / vs : 0.0f;
    for (CurveKey& key : out.keys) {
        key.time = ts != 0.0f ? (key.time - range.t0) / ts : 0.0f;
        key.value = vs != 0.0f ? (key.value - range.v0) / vs : 0.0f;
        if (std::isfinite(key.inTangent)) key.inTangent *= slopeScale;
        if (std::isfinite(key.outTangent)) key.outTangent *= slopeScale;
    }
    return out;
}

void curveops::alignKeys(std::vector<Curve*>& curves, const std::vector<KeyRef>& selection) {
    float sum = 0.0f;
    int count = 0;
    for (const KeyRef& ref : selection) {
        if (ref.curve < 0 || (size_t)ref.curve >= curves.size() || ref.key < 0 || (size_t)ref.key >= curves[(size_t)ref.curve]->keys.size()) continue;
        sum += curves[(size_t)ref.curve]->keys[(size_t)ref.key].value;
        count++;
    }
    if (count == 0) return;
    const float mean = sum / (float)count;
    for (const KeyRef& ref : selection) {
        if (ref.curve < 0 || (size_t)ref.curve >= curves.size() || ref.key < 0 || (size_t)ref.key >= curves[(size_t)ref.curve]->keys.size()) continue;
        curves[(size_t)ref.curve]->keys[(size_t)ref.key].value = mean;
    }
    // the modes set the tangents again around the keys that moved
    for (const KeyRef& ref : selection) {
        if (ref.curve < 0 || (size_t)ref.curve >= curves.size() || ref.key < 0 || (size_t)ref.key >= curves[(size_t)ref.curve]->keys.size()) continue;
        curves[(size_t)ref.curve]->updateTangents((size_t)ref.key);
    }
}

void curveops::smoothKeys(std::vector<Curve*>& curves, const std::vector<KeyRef>& selection) {
    for (const KeyRef& ref : selection) {
        if (ref.curve < 0 || (size_t)ref.curve >= curves.size() || ref.key < 0 || (size_t)ref.key >= curves[(size_t)ref.curve]->keys.size()) continue;
        Curve& curve = *curves[(size_t)ref.curve];
        CurveKey& key = curve.keys[(size_t)ref.key];
        key.setBothModes(CurveTangentMode::Free);
        key.setBroken(false);
        curve.smoothTangents((size_t)ref.key, 0.0f);
    }
}

void curveops::sharpenKeys(std::vector<Curve*>& curves, const std::vector<KeyRef>& selection) {
    for (const KeyRef& ref : selection) {
        if (ref.curve < 0 || (size_t)ref.curve >= curves.size() || ref.key < 0 || (size_t)ref.key >= curves[(size_t)ref.curve]->keys.size()) continue;
        setTangentMode(*curves[(size_t)ref.curve], (size_t)ref.key, Side::Both, CurveTangentMode::Linear);
    }
}

std::vector<CurveKey> curveops::copyKeys(const Curve& curve, const std::vector<KeyRef>& selection, int curveIndex) {
    std::vector<CurveKey> keys;
    for (size_t k = 0; k < curve.keys.size(); k++) {
        if (selected(selection, curveIndex, (int)k)) keys.push_back(curve.keys[k]);
    }
    if (keys.empty()) return keys;
    const float first = keys.front().time;
    for (CurveKey& key : keys) key.time -= first;
    return keys;
}

std::vector<int> curveops::pasteKeys(Curve& curve, const std::vector<CurveKey>& keys, float time, const CurveRange* range) {
    std::vector<float> times;
    for (CurveKey key : keys) {
        key.time += time;
        clampKey(key, range);
        bool replaced = false;
        for (CurveKey& existing : curve.keys) {
            if (existing.time == key.time) {
                existing = key;
                replaced = true;
            }
        }
        if (!replaced) curve.keys.push_back(key);
        times.push_back(key.time);
    }
    curve.sortKeys();
    curve.updateAllTangents();
    std::vector<int> indices;
    for (float t : times) {
        for (size_t k = 0; k < curve.keys.size(); k++) {
            if (curve.keys[k].time == t) {
                indices.push_back((int)k);
                break;
            }
        }
    }
    return indices;
}

View curveops::zoom(const View& view, float x, float y, float timeFactor, float valueFactor) {
    View out = view;
    const float t = view.toTime(x), v = view.toValue(y);
    out.t0 = t + (view.t0 - t) * timeFactor;
    out.t1 = t + (view.t1 - t) * timeFactor;
    out.v0 = v + (view.v0 - v) * valueFactor;
    out.v1 = v + (view.v1 - v) * valueFactor;
    return out;
}

View curveops::pan(const View& view, float dx, float dy) {
    View out = view;
    const float dt = dx / view.scaleX(), dv = -dy / view.scaleY();
    out.t0 -= dt;
    out.t1 -= dt;
    out.v0 -= dv;
    out.v1 -= dv;
    return out;
}

View curveops::frame(const std::vector<const Curve*>& curves, const std::vector<KeyRef>& selection, const View& pixels, float margin,
                     float minSpan) {
    float t0 = std::numeric_limits<float>::infinity(), t1 = -t0, v0 = t0, v1 = -t0;
    auto include = [&](float t, float v) {
        t0 = std::min(t0, t);
        t1 = std::max(t1, t);
        v0 = std::min(v0, v);
        v1 = std::max(v1, v);
    };
    for (size_t c = 0; c < curves.size(); c++) {
        const Curve& curve = *curves[c];
        bool any = false;
        for (size_t k = 0; k < curve.keys.size(); k++) {
            if (!selection.empty() && !selected(selection, (int)c, (int)k)) continue;
            include(curve.keys[k].time, curve.keys[k].value);
            any = true;
        }
        // all keys: what the curve reaches between them too
        if (selection.empty() && any && curve.keys.size() > 1) {
            float lo, hi;
            curve.valueRange(lo, hi);
            include(curve.keys.front().time, lo);
            include(curve.keys.back().time, hi);
        }
    }
    View view = pixels;
    if (!std::isfinite(t0)) {
        view.t0 = 0.0f;
        view.t1 = 1.0f;
        view.v0 = 0.0f;
        view.v1 = 1.0f;
        return view;
    }
    if (t1 - t0 < minSpan) {
        const float mid = 0.5f * (t0 + t1);
        t0 = mid - 0.5f * minSpan;
        t1 = mid + 0.5f * minSpan;
    }
    if (v1 - v0 < minSpan) {
        const float mid = 0.5f * (v0 + v1);
        v0 = mid - 0.5f * minSpan;
        v1 = mid + 0.5f * minSpan;
    }
    const float mt = (t1 - t0) * margin, mv = (v1 - v0) * margin;
    view.t0 = t0 - mt;
    view.t1 = t1 + mt;
    view.v0 = v0 - mv;
    view.v1 = v1 + mv;
    return view;
}
