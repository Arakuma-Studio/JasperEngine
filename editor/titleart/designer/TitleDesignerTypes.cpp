// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleDesignerTypes.h"

#include <map>

using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    constexpr const char* TOOL_NAMES[] = {"hand", "move", "rotate", "scale", "rect"};
    constexpr double PI = 3.14159265358979323846;

}

const char* jasperengine::editor::titledesigner::toolName(Tool tool) {
    const size_t index = (size_t)tool;
    return index < sizeof(TOOL_NAMES) / sizeof(TOOL_NAMES[0]) ? TOOL_NAMES[index] : "";
}

bool jasperengine::editor::titledesigner::toolFromName(const std::string& name, Tool& out) {
    for (size_t i = 0; i < sizeof(TOOL_NAMES) / sizeof(TOOL_NAMES[0]); i++) {
        if (name == TOOL_NAMES[i]) {
            out = (Tool)i;
            return true;
        }
    }
    return false;
}

Json jasperengine::editor::titledesigner::editsToOps(const std::vector<FieldEdit>& edits) {
    Json ops = Json::array();
    std::map<std::string, size_t> byLayer;
    for (const FieldEdit& edit : edits) {
        auto it = byLayer.find(edit.layer);
        if (it == byLayer.end()) {
            Json op = {{"op", "set"}};
            if (!edit.layer.empty()) op["layer"] = "id:" + edit.layer;
            op["values"] = Json::object();
            ops.push_back(std::move(op));
            it = byLayer.emplace(edit.layer, ops.size() - 1).first;
        }
        ops[it->second]["values"][edit.path] = edit.value;
    }
    return ops;
}

Affine2 Affine2::inverse() const {
    const double det = determinant();
    if (std::abs(det) < 1e-300) return {};
    const double inv = 1.0 / det;
    Affine2 out;
    out.a = d * inv;
    out.b = -b * inv;
    out.c = -c * inv;
    out.d = a * inv;
    out.e = -(out.a * e + out.c * f);
    out.f = -(out.b * e + out.d * f);
    return out;
}

Affine2 Affine2::operator*(const Affine2& o) const {
    Affine2 out;
    out.a = a * o.a + c * o.b;
    out.b = b * o.a + d * o.b;
    out.c = a * o.c + c * o.d;
    out.d = b * o.c + d * o.d;
    out.e = a * o.e + c * o.f + e;
    out.f = b * o.e + d * o.f + f;
    return out;
}

Affine2 Affine2::rotateClockwise(double degrees) {
    // y up: clockwise on screen is a negative mathematical angle
    const double r = -degrees * PI / 180.0;
    const double cs = std::cos(r), sn = std::sin(r);
    return {cs, sn, -sn, cs, 0.0, 0.0};
}

Affine2 jasperengine::editor::titledesigner::layerTransformAffine(const ta::TitleTransform& t) {
    const Affine2 flip = Affine2::scale(t.flipX ? -1.0 : 1.0, t.flipY ? -1.0 : 1.0);
    return Affine2::translate(t.position.x, t.position.y) * Affine2::rotateClockwise(t.rotation) * Affine2::scale(t.scale, t.scale) * flip;
}
