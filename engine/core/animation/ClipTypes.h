// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 2.1-2.3, 9.R14)
// - Purpose: the small clip enums and the warning type, without the rest of the clip model, for
//   headers included everywhere (AnimationPlayerComponent reaches every TU through
//   EntityRegistry.h).
// - Owner: T0. Thread-safety: plain types.
// - Invariants: the enum order is the canonical order of tracks with the same path in files
//   (Position < Rotation < Scale < Morph < Lattice < Property); appending is allowed, reordering
//   is not.

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
        Property    // reserved (spec 2.3): kept and written back, never sampled
    };

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
