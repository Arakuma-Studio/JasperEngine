// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 2.1-2.3, 9.R14)
// - Purpose: the small clip enums and the warning type, without the rest of the clip model, for
//   headers included everywhere (AnimationPlayerComponent reaches every TU through
//   EntityRegistry.h).
// - Thread-safety: plain types.
// - Invariants: the enum order is the canonical order of tracks with the same path in files
//   (Position < Rotation < Scale < Morph < Lattice < Property); appending is allowed, reordering
//   is not. Humanoid tracks (anim-retarget 6.1) have no path: they sort after every path track,
//   by channel, then by bone.

#ifndef CLIP_TYPES_H
#define CLIP_TYPES_H

#include "Export.h"

#include <cstdint>
#include <string>

namespace jasperengine{

    // What a track animates on its bound entity (v1 = today's keyframe kinds)
    enum class ClipChannel : uint8_t {
        Position,   // Transform.position, [x, y, z]
        Rotation,   // Transform.rotation, quaternion [x, y, z, w]
        Scale,      // Transform.scale, [x, y, z]
        Morph,      // MeshComponent morph weights, one per morph target
        Lattice,    // LatticeComponent offsets of `points` (flat indices), [x, y, z] each
        Property,   // reserved (spec 2.3): kept and written back, never sampled
        // humanoid clips (anim-retarget 6.1): identified by `humanBone`, never by path
        Humanoid,           // a slot's joint rotation E relative to the neutral pose, or an extra
                            // bone's canonical rest delta: quaternion [x, y, z, w]
        HumanoidPosition    // the body (hips) offset or a foot IK goal: [left, up, forward] in
                            // humanScale units, relative to the neutral hips
    };

    // the two humanoid channels
    inline bool isHumanoidChannel(ClipChannel channel){
        return channel == ClipChannel::Humanoid || channel == ClipChannel::HumanoidPosition;
    }
    // channels whose keys are quaternions / three-float vectors
    inline bool isRotationChannel(ClipChannel channel){
        return channel == ClipChannel::Rotation || channel == ClipChannel::Humanoid;
    }
    inline bool isVectorChannel(ClipChannel channel){
        return channel == ClipChannel::Position || channel == ClipChannel::Scale || channel == ClipChannel::HumanoidPosition;
    }

    // `humanBone` of a HumanoidPosition track
    constexpr int HUMANOID_POSITION_HIPS = 0;
    constexpr int HUMANOID_POSITION_LEFT_FOOT = 1;
    constexpr int HUMANOID_POSITION_RIGHT_FOOT = 2;
    constexpr int HUMANOID_POSITION_COUNT = 3;

    // Unity's WrapMode. Default only in Animation Player clip entries: the clip's own.
    enum class ClipWrapMode : uint8_t {
        Default,
        Once,           // plays to the end and stops; the player resets the state's time to 0
        Loop,
        PingPong,
        ClampForever    // holds the last frame and keeps the state playing
    };

    // A load or validation problem of one part of a clip: `path` is a JSON path into the file
    // ("tracks[3].keys[2].value"), empty for the file as a whole
    struct JASPERENGINE_API ClipWarning{
        std::string path;
        std::string message;
    };

}

#endif //CLIP_TYPES_H
