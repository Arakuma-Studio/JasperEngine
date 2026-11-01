// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim-properties spec 10, The Curves view)
// - Purpose: Unity's key tangent editing on the scalar curves of a clip track (position, scale,
//   morph weights, lattice offsets, continuous properties; not rotations, not discrete
//   properties), as the format's Hermite tangents (value units per second) plus the key's
//   `tangentMode` hint: Auto (Clamped Auto), Free Smooth, Flat, Broken, Linear, and per side
//   Free / Linear / Constant (Constant right is the key's step ease, left the previous key's).
// - Pure functions of a ClipTrackData (no editor state): the controller applies them in its
//   commits, the tests call them directly. Thread-safety: none needed.
// - Invariants: after any of them every key of the track has tangents of the value's width
//   (a track gaining tangents gives its other keys their linear slopes: its curve is unchanged
//   where it was linear); refresh() recomputes Auto, Linear and Flat keys from their
//   neighbours, so an edit that moves keys keeps those modes' meaning.

#pragma once

#include "animation/AnimationClip.h"

#include <cstddef>
#include <vector>

namespace jasperengine::editor::cliptangents{

    enum class Side{ Both, Left, Right };
    enum class SideType{ Free, Linear, Constant };

    // a track whose keys take tangent edits: not a rotation, not a discrete or unknown property
    bool editable(const ClipTrackData& track);

    // Clamped Auto: the Catmull-Rom slope (v+ - v-) / (t+ - t-), 0 at a local extremum and at the
    // ends, limited (3x each side's linear slope) so the segment does not overshoot its neighbours
    float clampedAutoSlope(float previousTime, float previousValue, float time, float value, float nextTime, float nextValue,
                           bool hasPrevious, bool hasNext);

    // every key with tangents (the ones without get their linear slopes). Tangents win over
    // eases, so the track's eases other than step (cubic_in_out, bounce...) become linear: the
    // callers say so (droppedEases first)
    void ensureTangents(ClipTrackData& track);
    // the keys whose ease ensureTangents would replace (0 when every key has tangents already)
    size_t droppedEases(const ClipTrackData& track);
    // Auto, Linear and Flat keys computed again from their neighbours (after keys moved)
    void refresh(ClipTrackData& track);
    // a key's mode (Auto, Free = Free Smooth, Flat, Broken, Linear)
    void setMode(ClipTrackData& track, size_t key, ClipTangentMode mode);
    // one side (or both) Free, Linear or Constant (Unity's Left / Right / Both Tangents); the
    // key becomes Broken, except Both Linear, which is Linear
    void setSide(ClipTrackData& track, size_t key, Side side, SideType type);
    // a handle drag: `side`'s slope of one component (mirrored to the other side unless the key
    // is Broken); the key becomes Free (Broken stays Broken)
    void setSlope(ClipTrackData& track, size_t key, size_t component, Side side, float slope);

}
