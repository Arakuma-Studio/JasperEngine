// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's MinMaxCurve and MinMaxGradient fields (particles spec 6.8): Unity's value
// field with a small mode arrow after it. A curve is a number (Constant), two numbers (Random
// Between Two Constants), a multiplier and a curve (Curve) or a multiplier and two curves in
// one editor, the band between them shaded (Random Between Two Curves: CurveField); a gradient
// is a colour, two colours, a gradient (GradientField), two gradients or a random colour of a
// gradient. Switching the mode keeps the value it had (a constant becomes a flat curve).
//
// Contract (as CurveField): call every frame the row is drawn with the property's value;
// `changed` on every frame it was modified, `finished` when one edit ends (commit an undo step
// then). Colours are shown and picked in sRGB and stored linear. Main thread.

#pragma once

#include "particles/MinMax.h"
#include "CurveField.h"

namespace jasperengine::editor {

    struct MinMaxFieldResult{
        bool changed = false;
        bool finished = false;
    };

    // `displayScale` multiplies what the numbers show (degrees for a value stored in radians:
    // 57.29578); `format` and `step` are the drag fields'
    MinMaxFieldResult minMaxCurveField(const char* id, MinMaxCurve& value, bool mixed, float width = -1.0f,
                                       float displayScale = 1.0f, const char* format = "%.2f", float step = 0.1f);

    MinMaxFieldResult minMaxGradientField(const char* id, MinMaxGradient& value, bool mixed, float width = -1.0f);

    // a mode change that keeps the value the field had (pure: gtested)
    MinMaxCurve convertMinMaxCurve(const MinMaxCurve& value, ParticleSystemCurveMode mode);
    MinMaxGradient convertMinMaxGradient(const MinMaxGradient& value, ParticleSystemGradientMode mode);

}
