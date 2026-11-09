// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MATH_CURVEJSON_H
#define MATH_CURVEJSON_H

// The one encoding of curves and gradients (docs/specs/S27-editor-tools.md R45, D4), shared by
// `.particles` files (ParticleEffectCodec), scene YAML and MCP (editor/stream/CurveCodec) and
// the curve libraries of the editor:
//
//   curve     [[time, value, inTangent, outTangent(, mode)], ...]   a null tangent: infinite (a step)
//             {"keys": [...], "preWrapMode": "loop", "postWrapMode": "pingPong"}  when a wrap is not clamp
//   gradient  {"colorKeys": [[time, r, g, b]], "alphaKeys": [[time, alpha]], "mode": "fixed", "linear": true}
//             colours sRGB-encoded; mode and linear only when not the default
//
// The readers also take upstream's (origin/main lines and trails) forms, so its scenes, prefabs
// and MCP calls read without loss (docs/specs/S27-editor-tools.md, Upstream absorption):
//   curve     a number (a constant curve); {"keys": [...], "pre": "loop", "post": "ping_pong"};
//             key maps {time, value, inTangent, outTangent}; "inf" / "-inf" step tangents.
//             Such a curve is normalized as upstream's was (sorted, equal times dropped).
//   gradient  {"mode": "blend", "colors": [[time, "#RRGGBB" | [r, g, b]]], "alphas": [[time, a]]}
//             or one colour ("#RRGGBB(AA)", [r, g, b(, a)]): array colours are linear and become
//             sRGB-encoded keys (the same colours; linear values above 1 stay above 1); at most 8
//             keys of each (the first 7 and the last).
//
// A key's mode token (5th element, left out for Free on both sides): "clampedAuto", "auto",
// "free", "linear" or "constant" when both sides agree and the key is not broken, else
// "in:<mode>,out:<mode>" with ",broken" when broken. Floats are written in their shortest
// round-trip form, so read -> write gives back the same text.

#include "Export.h"
#include "math/Curve.h"

#include "json.hpp"

#include <string>
#include <vector>

namespace jasperengine { namespace curvejson {

    using Json = nlohmann::ordered_json;

    // what a read found wrong: `path` extends the path given to the read ("[2]", ".colorKeys[0]")
    struct Issue {
        bool error = true;          // false: a warning (an unknown key, read past)
        std::string path;
        std::string message;
        bool unknownKey = false;    // a warning about a key the format does not have
    };

    JASPERENGINE_API Json curveToJson(const Curve& curve);
    // false on an error (the curve is then left as read so far); warnings do not fail
    JASPERENGINE_API bool curveFromJson(const Json& json, Curve& curve, std::vector<Issue>& issues);

    // a curve or gradient in upstream's dialect (above): what the readers take besides the one form
    JASPERENGINE_API bool isUpstreamCurve(const Json& json);
    JASPERENGINE_API bool isUpstreamGradient(const Json& json);

    JASPERENGINE_API Json gradientToJson(const Gradient& gradient);
    JASPERENGINE_API bool gradientFromJson(const Json& json, Gradient& gradient, std::vector<Issue>& issues);

    // a key's packed tangent modes as a token ("" for 0: Free on both sides, unbroken)
    JASPERENGINE_API std::string modeToken(uint8_t mode);
    JASPERENGINE_API bool modeFromToken(const std::string& token, uint8_t& mode);

    // "free" "auto" "linear" "constant" "clampedAuto"
    JASPERENGINE_API const char* tangentModeName(CurveTangentMode mode);
    JASPERENGINE_API bool tangentModeFromName(const std::string& name, CurveTangentMode& mode);
    // "clamp" "loop" "pingPong" "linear" "loopWithOffset"
    JASPERENGINE_API const char* wrapModeName(CurveWrapMode mode);
    JASPERENGINE_API bool wrapModeFromName(const std::string& name, CurveWrapMode& mode);

    // A float as the double its shortest round-trip decimal names, so JSON shows 0.1, not
    // 0.10000000149011612, and reading it back gives the same float (0 for a non-finite one)
    JASPERENGINE_API double jsonFloat(float value);

}}

#endif // MATH_CURVEJSON_H
