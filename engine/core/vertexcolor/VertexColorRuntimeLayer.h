// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R80-R82: paint written by gameplay. A transient layer over the
// authored values of one target; it never changes the .vcolors data. Fixed-size state per GPU
// vertex (R81), fades in game time recomputed at most 10 times a second (R82).

#include "Export.h"
#include "math/Matrix4.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "vertexcolor/VertexColorMesh.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace jasperengine::vertexcolor {

    struct RuntimePaint {
        float radius = 0.5f;
        Vector4 value = Vector4(1.0f, 0.0f, 0.0f, 1.0f);
        ChannelMask channels = CHANNELS_RGBA;
        float strength = 1.0f;
        float hardness = 0.0f;          // inner fraction of the radius at full strength (smooth falloff outside)
        float hold = -1.0f;             // seconds at full value; < 0: forever
        float fade = 0.0f;              // seconds to fade out after hold; 0 with hold >= 0: vanish at hold end
    };

    class JASPERENGINE_API RuntimeLayer {
    public:
        // paints around a point in the target's local space at game time `now`; returns the GPU
        // vertices touched
        uint32_t paint(const MeshView& mesh, const Vector3& localPoint, const RuntimePaint& paint, double now);
        // advances fades; true when combined values changed since the last combine()
        bool update(double now);
        // authored values (per GPU vertex) -> values with the layer over them; clears dirty()
        void combine(std::span<const Vector4> authored, std::vector<Vector4>& out) const;
        // ... over split vertices (R12): authored[i] is a copy of GPU vertex sourceVertex[i]
        void combine(std::span<const Vector4> authored, std::span<const uint32_t> sourceVertex, std::vector<Vector4>& out) const;
        // one GPU vertex's authored value with the layer over it (sampling; leaves dirty())
        Vector4 over(uint32_t vertex, const Vector4& authored) const;
        // no live paint: no per-frame work (R82)
        bool live() const { return liveCount > 0; }
        bool dirty() const { return changed; }
        void clear();
        size_t memoryBytes() const;

    private:
        struct State {
            Vector4 value;
            float weight = 0.0f;        // current blend weight 0..1
            float peak = 0.0f;          // weight at hold end
            float holdEnd = 0.0f;       // game time (seconds, from `epoch`)
            float fadeRate = 0.0f;      // weight per second after holdEnd; 0: permanent
            ChannelMask mask = 0;       // touched channels (R81)
        };
        std::vector<State> states;      // per GPU vertex, sized on first paint
        double epoch = 0.0;
        double lastUpdate = -1.0;
        uint32_t liveCount = 0;
        // set by paint/update/clear, cleared by combine (the caller uploads what it combined)
        mutable bool changed = false;
    };

}
