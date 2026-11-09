// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A graph parameter's stored JSON (GraphRegistry.h, ParamDesc::kind) as the values the editor's
// fields edit, and back (docs/specs/S90-procedural-material.md R26): a colour as Vector4, a
// gradient ({points: [{pos, r, g, b, a}]}) as a Gradient, a curve ({points: [{x, y, ls, rs}]}) as
// a Curve. Written back in the shape that was read, so an edit changes only what it edits. Pure:
// no ImGui.

#pragma once

#include "graph/Graph.h"
#include "math/Curve.h"
#include "math/Vector4.h"

#include <cstddef>
#include <optional>
#include <string>

namespace jasperengine::editor::graphvalues {

    // A graph's ramp (gradient points, curve points) holds up to 32 keys; the particles' stay at 8.
    // A file's ramp of more keys still edits, as long as an edit adds none.
    inline constexpr size_t RAMP_KEYS_MAX = 32;

    // A number field's range: the declared bounds, and where one is missing as far as the field
    // goes that way (a missing bound is no bound, never 0)
    void dragRange(const std::optional<double>& min, const std::optional<double>& max, bool whole, double& lo, double& hi);

    // {r, g, b, a} or [r, g, b(, a)]; false for anything else
    bool colorFrom(const graph::Json& value, Vector4& out);
    // in the shape of `like` (an array stays an array), {r, g, b, a} when it has none
    graph::Json colorJson(const Vector4& color, const graph::Json& like);

    // colour and alpha keys from the points (a point is both); interpolation 0 is Fixed
    bool gradientFrom(const graph::Json& value, Gradient& out);
    // a point per key time of either list, its colour and alpha there; `like`'s other keys and its
    // interpolation kept unless the mode changed; false with `error` past RAMP_KEYS_MAX points
    bool gradientJson(const Gradient& gradient, const graph::Json& like, graph::Json& out, std::string& error);

    // x, y and the slopes (ls: left, the in tangent; rs: right, the out tangent)
    bool curveFrom(const graph::Json& value, Curve& out);
    bool curveJson(const Curve& curve, const graph::Json& like, graph::Json& out, std::string& error);

}
