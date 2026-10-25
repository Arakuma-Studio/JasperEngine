// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The key grammar of edit_animation (WorldTools.cpp), shared by its scene mode (entity
// Animations) and its clip-file mode (AnimationClipTools.cpp): keys {time, value | points, ease},
// rotations as quaternions or {"euler": [...]}, lattice points {"<index>": [x,y,z]} /
// [[[i,j,k] | index, [x,y,z]], ...] / "current" / "rest" / a pose name, ease names. Main thread.

#include "mcp/McpJson.h"

#include "action/Ease.h"
#include "ecs/Entity.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "util/LatticeTrackKeys.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {
    struct LatticeComponent;
    struct LatticeTracksComponent;
    class Scene;
}

namespace jasperengine::editor::mcp::anim_keys {

    enum class Channel { Position, Rotation, Scale, Morph, Lattice };

    struct Key {
        float time = 0.0f;
        Vector3 vec;
        Quaternion quat;
        std::vector<float> weights;
        // lattice: offsets of points (flat indices, ascending, once each), or the lattice's own
        // current offsets chosen as editor keying does (resolved by the caller)
        std::vector<int> points;
        std::vector<Vector3> offsets;
        bool current = false;
        bool rest = false;     // "rest": every keyed point at zero
        std::string pose;      // a pose name: the pose's offsets
        EaseType ease = EaseType::LINEAR;
    };

    // Ease names as Stream writes them (QUAD_IN_OUT); any case, '-' or '_'
    bool parseEase(const Json& value, EaseType& out, std::string& error);
    // "quad_in_out"
    std::string easeName(EaseType type);

    // A lattice key's points (the forms above); `lattice` gives the resolution and the poses
    bool parseLatticePoints(const Json& v, const LatticeComponent& lattice, Key& key, std::string& error);
    // keys [{time, value | points, ease}], sorted by time; lattice keys need `lattice`
    bool parseKeys(Channel channel, const Json& value, std::vector<Key>& keys, std::string& error,
                   const LatticeComponent* lattice = nullptr);
    // "current" keys take the cage's offsets of the points the Animation window keys
    void resolveCurrentLatticeKeys(uint32_t sceneId, Entity latticeEntity, const LatticeComponent& lattice,
                                   const LatticeTracksComponent* existing, std::vector<Key>& keys);
    // "rest" and pose keys hold every point the other keys (and `existing`) animate
    void resolveShapeLatticeKeys(const LatticeComponent& lattice, const LatticeTracksComponent* existing, std::vector<Key>& keys);
    // Lattice keys as a whole track: one column per point any key names, the points a key does
    // not name interpolated in time between the keys naming them. `track` NULL_ENTITY (or `merge`
    // false): built from `keys` alone.
    LatticeTrackData latticeTrackData(Scene* scene, Entity track, const std::vector<Key>& keys, const LatticeComponent& lattice, bool merge);

}
