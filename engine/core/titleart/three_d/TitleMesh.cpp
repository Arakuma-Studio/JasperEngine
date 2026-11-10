// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleMesh.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::titleart;

namespace {
    const char* const TIER_NAMES[] = {"bake", "high", "medium", "low"};

    int16_t snorm16(float v) {
        return (int16_t)std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
    }
}

const char* jasperengine::titleart::titleTierName(TitleTier tier) {
    return TIER_NAMES[(size_t)tier];
}

bool jasperengine::titleart::titleTierFromName(const std::string& name, TitleTier& out) {
    for (size_t i = 0; i < 4; i++) {
        if (name == TIER_NAMES[i]) {
            out = (TitleTier)i;
            return true;
        }
    }
    return false;
}

size_t TitleMesh::bytes() const {
    return vertices.size() * sizeof(TitleMeshVertex) + wrap.size() * sizeof(uint16_t) + indices16.size() * sizeof(uint16_t) +
           indices32.size() * sizeof(uint32_t);
}

std::array<int16_t, 2> jasperengine::titleart::encodeTitleNormal(float x, float y, float z) {
    const float l1 = std::abs(x) + std::abs(y) + std::abs(z);
    if (l1 <= 0) return {0, 0};
    x /= l1;
    y /= l1;
    z /= l1;
    if (z < 0) {
        // octWrap: (1 - |v.yx|) x sign(v.xy), signs of zero positive as in octahedral.glsl
        const float wx = (1 - std::abs(y)) * (x >= 0 ? 1.0f : -1.0f);
        const float wy = (1 - std::abs(x)) * (y >= 0 ? 1.0f : -1.0f);
        x = wx;
        y = wy;
    }
    return {snorm16(x), snorm16(y)};
}

std::array<float, 3> jasperengine::titleart::decodeTitleNormal(int16_t ix, int16_t iy) {
    const float fx = std::max(-1.0f, ix / 32767.0f), fy = std::max(-1.0f, iy / 32767.0f);
    float x = fx, y = fy, z = 1 - std::abs(fx) - std::abs(fy);
    const float t = std::clamp(-z, 0.0f, 1.0f);
    x += x >= 0 ? -t : t;
    y += y >= 0 ? -t : t;
    const float l = std::sqrt(x * x + y * y + z * z);
    return {x / l, y / l, z / l};
}
