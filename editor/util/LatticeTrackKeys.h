// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Keyframe edits of LatticeTracks (spec §7), shared by the Animation window keying and the
// MCP edit_animation "lattice" channel, so a key means the same thing on both paths.

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "action/Ease.h"
#include "component/LatticeComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/KeyframeTracksComponent.h"

#include <cstdint>
#include <vector>

namespace jasperengine::editor{

    // Every array of a lattice track, edited as a whole and written back with property commands.
    struct LatticeTrackData{
        std::vector<float> times;
        std::vector<EaseType> easings;
        std::vector<int> points;
        std::vector<std::vector<Vector3>> values;
        std::vector<std::vector<Vector3>> inTangents;
        std::vector<std::vector<Vector3>> outTangents;

        LatticeTrackData() = default;
        LatticeTrackData(const KeyframeTracksComponent& keyframe, const LatticeTracksComponent& tracks);
    };

    class LatticeTrackKeys{
    public:
        // The points a key of the lattice records (spec §7): the Scene-view point selection when
        // points are selected, else every point with a non-zero offset plus every point already in
        // `tracks` (may be null). Only points of the lattice's current resolution, ascending. A
        // lattice at rest with a track holding no point gives every point, so its rest pose can be
        // keyed (an empty key would record nothing).
        static std::vector<int> pointsToKey(uint32_t sceneId, Entity latticeEntity, const LatticeComponent& lattice,
                                            const LatticeTracksComponent* tracks);

        // The lattice's current offset of a point (zero for a point beyond its offsets)
        static Vector3 currentOffset(const LatticeComponent& lattice, int point);

        // Column `column` of the track at local `time`, as ActionSystem plays it (segment easing,
        // Hermite tangents when they are complete). `fallback` when no key has that column.
        static Vector3 sample(const LatticeTrackData& data, size_t column, float time, const Vector3& fallback);

        // Records `values` (the offsets of `points`) as a key at `time`: the key within `epsilon`
        // of that time is updated, else a key is inserted keeping times sorted (the segment it
        // splits keeps its easing on both halves, tangents get zero rows). Points new to the track
        // become columns; the keys that already exist get the value of those points from
        // `lattice`'s current offsets, the only value known for them. Columns not recorded keep
        // their values in an updated key and get the track's own sampled value in an inserted
        // one, so the rest of the animation does not change. Rows shorter than the point list
        // (hand-edited data) are completed the same way.
        static void setKey(LatticeTrackData& data, float time, const std::vector<int>& points,
                           const std::vector<Vector3>& values, const LatticeComponent& lattice, float epsilon);
    };

}
