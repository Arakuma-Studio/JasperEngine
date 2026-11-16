// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R80-R82: the transient gameplay paint layer

#include "vertexcolor/VertexColorRuntimeLayer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jasperengine::vertexcolor {

    namespace {

        constexpr double RUNTIME_UPDATE_PERIOD = 0.1;   // R82: at most 10 recomputes a second

        float runtimeChannel(const Vector4& v, int c) { return c == 0 ? v.x : c == 1 ? v.y : c == 2 ? v.z : v.w; }
        void runtimeSetChannel(Vector4& v, int c, float value) {
            if (c == 0) v.x = value;
            else if (c == 1) v.y = value;
            else if (c == 2) v.z = value;
            else v.w = value;
        }

        // 1 inside hardness * radius, then smoothstep down to 0 at the radius
        float runtimeFalloff(float x, float hardness) {
            if (x <= hardness) return 1.0f;
            if (x >= 1.0f) return 0.0f;
            const float s = (x - hardness) / (1.0f - hardness);
            return 1.0f - s * s * (3.0f - 2.0f * s);
        }

        // weight of a state at `t` (seconds from epoch): peak while holding, then down at fadeRate.
        // fadeRate 0 is permanent; infinity vanishes right after the hold
        float runtimeWeightAt(float peak, float holdEnd, float fadeRate, float t) {
            if (fadeRate == 0.0f) return peak;
            const float elapsed = t - holdEnd;
            if (elapsed <= 0.0f) return peak;
            const float w = peak - fadeRate * elapsed;
            return w > 0.0f ? w : 0.0f;
        }

    }

    uint32_t RuntimeLayer::paint(const MeshView& mesh, const Vector3& localPoint, const RuntimePaint& paint, double now) {
        const uint32_t vertexCount = mesh.vertexCount();
        if (states.size() != vertexCount) {
            if (!states.empty()) changed = true;
            states.assign(vertexCount, State());
            liveCount = 0;
        }
        const float radius = paint.radius;
        const float strength = std::clamp(paint.strength, 0.0f, 1.0f);
        const ChannelMask channels = (ChannelMask)(paint.channels & CHANNELS_RGBA);
        if (!(radius > 0.0f) || strength <= 0.0f || channels == 0) return 0;
        const float hardness = std::clamp(paint.hardness, 0.0f, 1.0f);

        // holdEnd is a float from the epoch: the epoch moves while nothing fades, so it keeps precision
        if (liveCount == 0) epoch = now;
        const float t = (float)(now - epoch);
        const bool permanent = paint.hold < 0.0f;
        const float holdEnd = t + std::max(paint.hold, 0.0f);
        const float fadeEnd = holdEnd + std::max(paint.fade, 0.0f);
        constexpr float INF = std::numeric_limits<float>::infinity();

        uint32_t touched = 0;
        const float radius2 = radius * radius;
        for (uint32_t v = 0; v < vertexCount; v++) {
            const float d2 = (mesh.positions[v] - localPoint).squaredLength();
            if (d2 > radius2) continue;
            const float w = strength * runtimeFalloff(std::sqrt(d2) / radius, hardness);
            if (w <= 0.0f) continue;
            touched++;

            State& s = states[v];
            const bool wasLive = s.mask != 0 && s.weight > 0.0f && s.fadeRate != 0.0f;
            const float current = s.mask != 0 ? runtimeWeightAt(s.peak, s.holdEnd, s.fadeRate, t) : 0.0f;
            State merged;
            if (current <= 0.0f) {
                merged.value = paint.value;
                merged.mask = channels;
                merged.peak = w;
                if (permanent) {
                    merged.fadeRate = 0.0f;
                } else {
                    merged.holdEnd = holdEnd;
                    merged.fadeRate = fadeEnd > holdEnd ? w / (fadeEnd - holdEnd) : INF;
                }
            } else {
                // One state per vertex (R81): the new paint goes over the old one as two lerps would,
                // lerp(lerp(a, old, W), new, w) = lerp(a, merged, W + w - W w), exact for the channels
                // both touch; the longer-lasting timing wins
                const float combined = current + w - current * w;
                merged.mask = (ChannelMask)(s.mask | channels);
                merged.value = s.value;
                for (int c = 0; c < 4; c++) {
                    if (!(channels & (1u << c))) continue;
                    const float p = runtimeChannel(paint.value, c);
                    const float value = (s.mask & (1u << c)) ? (runtimeChannel(s.value, c) * current * (1.0f - w) + p * w) / combined : p;
                    runtimeSetChannel(merged.value, c, value);
                }
                merged.peak = combined;
                if (permanent || s.fadeRate == 0.0f) {
                    merged.fadeRate = 0.0f;
                } else {
                    const float oldEnd = s.fadeRate == INF ? s.holdEnd : s.holdEnd + s.peak / s.fadeRate;
                    merged.holdEnd = std::max(s.holdEnd, holdEnd);
                    const float end = std::max(oldEnd, fadeEnd);
                    merged.fadeRate = end > merged.holdEnd ? combined / (end - merged.holdEnd) : INF;
                }
            }
            merged.weight = merged.peak;
            s = merged;
            const bool isLive = s.fadeRate != 0.0f;
            if (wasLive && !isLive) liveCount--;
            if (!wasLive && isLive) liveCount++;
            changed = true;
        }
        return touched;
    }

    bool RuntimeLayer::update(double now) {
        if (liveCount == 0) return changed;
        if (lastUpdate >= 0.0 && now >= lastUpdate && now - lastUpdate < RUNTIME_UPDATE_PERIOD) return changed;
        lastUpdate = now;
        const float t = (float)(now - epoch);
        for (State& s : states) {
            if (s.mask == 0 || s.fadeRate == 0.0f) continue;
            const float w = runtimeWeightAt(s.peak, s.holdEnd, s.fadeRate, t);
            if (w == s.weight) continue;
            changed = true;
            if (w > 0.0f) {
                s.weight = w;
            } else {
                s = State();
                liveCount--;
            }
        }
        return changed;
    }

    void RuntimeLayer::combine(std::span<const Vector4> authored, std::vector<Vector4>& out) const {
        out.assign(authored.begin(), authored.end());
        const size_t count = std::min(out.size(), states.size());
        for (size_t v = 0; v < count; v++) {
            const State& s = states[v];
            if (s.mask == 0 || s.weight <= 0.0f) continue;
            for (int c = 0; c < 4; c++) {
                if (!(s.mask & (1u << c))) continue;
                const float a = runtimeChannel(out[v], c);
                runtimeSetChannel(out[v], c, a + (runtimeChannel(s.value, c) - a) * s.weight);
            }
        }
        changed = false;
    }

    void RuntimeLayer::combine(std::span<const Vector4> authored, std::span<const uint32_t> sourceVertex, std::vector<Vector4>& out) const {
        out.resize(authored.size());
        for (size_t i = 0; i < authored.size(); i++)
            out[i] = over(i < sourceVertex.size() ? sourceVertex[i] : (uint32_t)i, authored[i]);
        changed = false;
    }

    Vector4 RuntimeLayer::over(uint32_t vertex, const Vector4& authored) const {
        Vector4 out = authored;
        if (vertex >= states.size()) return out;
        const State& s = states[vertex];
        if (s.mask == 0 || s.weight <= 0.0f) return out;
        for (int c = 0; c < 4; c++) {
            if (!(s.mask & (1u << c))) continue;
            const float a = runtimeChannel(out, c);
            runtimeSetChannel(out, c, a + (runtimeChannel(s.value, c) - a) * s.weight);
        }
        return out;
    }

    void RuntimeLayer::clear() {
        if (!states.empty()) changed = true;
        std::vector<State>().swap(states);
        liveCount = 0;
        epoch = 0.0;
        lastUpdate = -1.0;
    }

    size_t RuntimeLayer::memoryBytes() const {
        return sizeof(*this) + states.capacity() * sizeof(State);
    }

}
