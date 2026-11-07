// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The gradient editor's key edits (docs/specs/S27-editor-tools.md R9), without ImGui: hit
// testing of the markers along the bar, adding a key with the gradient's value there (at most
// Gradient::kMaxKeys, or the caller's limit: a graph's ramp holds 32), moving one (the list resorted, the key followed), removing one (a list
// keeps its last key). GradientField.cpp turns the bar's input into these calls. Taken over
// from origin/main's GradientFieldLogic (S27, Upstream absorption).

#include "math/Curve.h"

#include <cstddef>
#include <vector>

namespace jasperengine::editor::gradientops {

    // the time (0..1, clamped) of the pixel x on a bar from x0, `width` wide, and back
    float timeAtX(float x0, float width, float x);
    float xAtTime(float x0, float width, float time);

    // the marker nearest to the pixel x within `radius` pixels (a tie: the later one, drawn on
    // top); -1 for none
    int hitMarker(const std::vector<float>& times, float x0, float width, float x, float radius);
    std::vector<float> colorTimes(const Gradient& gradient);
    std::vector<float> alphaTimes(const Gradient& gradient);

    // a key at `time` (clamped to 0..1) with the gradient's value there, after the keys at that
    // time; its index, -1 when the list has `maxKeys` already
    int addColorKey(Gradient& gradient, float time, size_t maxKeys = Gradient::kMaxKeys);
    int addAlphaKey(Gradient& gradient, float time, size_t maxKeys = Gradient::kMaxKeys);

    // key `index` moved to `time` (clamped), the list resorted (after the keys at that time):
    // its new index, -1 for no such key
    int moveColorKey(Gradient& gradient, size_t index, float time);
    int moveAlphaKey(Gradient& gradient, size_t index, float time);

    // key `index` removed; false for no such key or the list's last one
    bool removeColorKey(Gradient& gradient, size_t index);
    bool removeAlphaKey(Gradient& gradient, size_t index);

}
