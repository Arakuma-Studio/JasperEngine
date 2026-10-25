// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's curves view, its pure part (animation clips spec 4.4, 9.R50): what a
// track's curves are (components, labels), their values at given times through the runtime
// sampler (anim::sampleTrack: the curves show what the Animation Player plays, never a second
// interpolation), rotations as Euler degrees kept continuous across keys, and the value axis'
// ticks. No ImGui; gtested (editor/tests/ClipRecordCurvesTest.cpp). Main thread.

#pragma once

#include "animation/AnimationClip.h"
#include "math/Vector3.h"

#include <cstddef>
#include <string>
#include <vector>

namespace jasperengine::editor::clipcurves{

    // the value components a track's curves show: position / scale 3, rotation 3 (Euler
    // degrees), morph one per weight of its widest key, lattice 3 per point; property none
    size_t componentCount(const ClipTrackData& track);

    // a component's name: "x" "y" "z", a blend shape's name (`morphNames[i]`, else "Weight i"),
    // a lattice point's "Point 4 x"
    std::string componentLabel(const ClipTrackData& track, size_t component, const std::vector<std::string>& morphNames);

    // The Euler triple (degrees) of the same rotation as `euler` nearest to `reference`: each angle
    // moved by whole turns, and the other solution (x + 180, 180 - y, z + 180) when it is nearer
    Vector3 nearestEuler(const Vector3& euler, const Vector3& reference);

    // the Euler degrees of every key of a rotation track, each the nearest to the key before it
    // (the first one as decomposed): a rotation's curves are continuous across its keys
    std::vector<Vector3> keyEulers(const ClipTrackData& track);

    // A track's curve values at `times` (ascending): out[i][component] with the runtime sampler
    // (`runtime` is the AnimationClip track of `track`). Rotations: Euler degrees continuous
    // along the times and through the keys (a sample at a key's time is keyEulers' value; between
    // keys each sample is the nearest to the one before). False for a property track.
    bool sampleCurves(const AnimationClip::Track& runtime, const ClipTrackData& track, const std::vector<float>& times,
                      std::vector<std::vector<float>>& out);

    // a key's values as the curves show them (rotation: `eulers[key]`, from keyEulers)
    std::vector<float> keyValues(const ClipTrackData& track, size_t key, const std::vector<Vector3>& eulers);

    // the value axis' step: 1, 2 or 5 x 10^n, the smallest giving ticks at least `minSpacing`
    // pixels apart when `range` value units take `pixels`
    float niceStep(float range, float pixels, float minSpacing);
    // the multiples of `step` in [min, max]
    std::vector<float> ticks(float min, float max, float step);

}
