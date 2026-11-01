// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The gradient field's pure part (lines and trails spec 6.1, "Editor widgets"): hit testing of
// the key markers along the bar, adding keys (at most Gradient::MAX_KEYS per list), moving them
// with re-sorting, removing them (never the last one of a list: Unity keeps one) and the preset
// gradients. Colours are linear, as Gradient stores them. No ImGui; the widget is GradientField.h.
// gtested (editor/tests/CurveFieldLogicTest.cpp). Main thread.

#pragma once

#include "math/Gradient.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::editor::gradientfield{

    // the time (clamped to [0, 1]) at screen x on a bar starting at `barX0`, `barWidth` wide
    float timeAtX(float barX0, float barWidth, float x);
    float xAtTime(float barX0, float barWidth, float time);

    // the marker (at x = barX0 + time * barWidth) nearest to `mouseX` within `radiusPx`, -1 when
    // none; on a tie the later one (drawn on top)
    int hitMarker(const std::vector<float>& times, float barX0, float barWidth, float mouseX, float radiusPx);

    std::vector<float> colorTimes(const Gradient& gradient);
    std::vector<float> alphaTimes(const Gradient& gradient);

    // a key at `time` (clamped to [0, 1]) with the gradient's current colour / alpha there: its
    // index (after the keys at the same time), -1 when the list has MAX_KEYS keys
    int addColorKey(Gradient& gradient, float time);
    int addAlphaKey(Gradient& gradient, float time);

    // moves key `index` to `time` (clamped to [0, 1]): the index it ends at after re-sorting
    // (after the keys at the same time), -1 when out of range
    int moveColorKey(Gradient& gradient, size_t index, float time);
    int moveAlphaKey(Gradient& gradient, size_t index, float time);

    // false when out of range or the key is the last one of its list
    bool removeColorKey(Gradient& gradient, size_t index);
    bool removeAlphaKey(Gradient& gradient, size_t index);

    enum class GradientPreset : uint8_t{ White = 0, FadeOut, FadeIn, BlackToWhite, WhiteToBlack, Rainbow };
    constexpr int GRADIENT_PRESET_COUNT = 6;

    const char* presetName(GradientPreset preset);
    // White: opaque white; FadeOut: white, alpha 1 to 0; FadeIn: white, alpha 0 to 1;
    // BlackToWhite / WhiteToBlack: opaque; Rainbow: red, yellow, green, cyan, blue, magenta.
    // The mode is Blend
    Gradient presetGradient(GradientPreset preset);

}
