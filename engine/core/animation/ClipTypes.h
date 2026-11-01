// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 2.1-2.3, 9.R14)
// - Purpose: the small clip enums and the warning type, without the rest of the clip model, for
//   headers included everywhere (AnimationPlayerComponent reaches every TU through
//   EntityRegistry.h).
// - Thread-safety: plain types.
// - Invariants: the ClipChannel order is the canonical order of tracks with the same path in
//   files (Position < Rotation < Scale < Morph < Lattice < Property); appending is allowed,
//   reordering is not.

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
        // a (component, property) of the bound entity (anim-properties spec 5-7): a registry
        // entry of PropertyBindings.h or a Lua script field, sampled in key space (sRGB colours,
        // cone degrees); unknown properties are kept verbatim and not played
        Property
    };

    // The value of a property track (anim-properties spec 5.4): stored as floats per key (bools
    // 0/1, ints whole numbers, colours in key space: sRGB). None: not resolved yet (a script
    // field or shader member before its keys were read) or unknown
    enum class PropertyKind : uint8_t {
        None,
        Bool,       // discrete
        Int,        // discrete unless the registry entry interpolates it (rounded on write)
        Frame,      // a sprite frame index, discrete
        Float,
        Vector2,
        Vector3,
        Vector4,
        Color3,     // sRGB keys, linear writes
        Color4      // sRGB keys (alpha linear), linear writes
    };

    // How a key value maps to the stored value (keys use the Inspector's units)
    enum class PropertyDisplay : uint8_t {
        Identity,
        SRGB,           // r, g, b: sRGB in keys, linear stored; alpha as is
        ConeDegrees     // the full cone angle in degrees in keys (Unity's Spot Angle), cos(half) stored
    };

    // Unity's key tangent modes (anim-properties spec 10, The Curves view), an editor hint of
    // keys with tangents ("tangent_mode"): the runtime uses the tangents only
    enum class TangentMode : uint8_t {
        None,       // no hint (tangents as given)
        Auto,       // Clamped Auto: recomputed when the key or a neighbour moves
        Free,       // Free Smooth: in = out, kept
        Flat,       // in = out = 0
        Broken,     // in and out independent
        Linear      // the slopes to the neighbours, recomputed when they move
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
