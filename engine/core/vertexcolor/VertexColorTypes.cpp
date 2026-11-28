// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vertexcolor/VertexColorTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jasperengine::vertexcolor {

namespace {

    // The largest finite half: RGBA16F stores anything beyond it as this, never as infinity
    constexpr float kTypesHalfMax = 65504.0f;

    // IEEE binary16 with round to nearest even, subnormals included, so a stored value is the same
    // bits whatever the platform's own half support
    uint16_t typesFloatToHalf(float value) {
        uint32_t f;
        std::memcpy(&f, &value, sizeof(f));
        const uint32_t sign = (f >> 16) & 0x8000u;
        const uint32_t absBits = f & 0x7fffffffu;
        if (absBits == 0) return (uint16_t)sign;
        const int32_t exponent = (int32_t)((absBits >> 23) & 0xffu) - 127 + 15;
        uint32_t mantissa = absBits & 0x7fffffu;
        if (exponent <= 0) {
            if (exponent < -10) return (uint16_t)sign;
            mantissa |= 0x800000u;
            const uint32_t shift = (uint32_t)(14 - exponent);
            uint32_t half = mantissa >> shift;
            const uint32_t rest = mantissa & ((1u << shift) - 1u);
            const uint32_t halfway = 1u << (shift - 1u);
            if (rest > halfway || (rest == halfway && (half & 1u))) half++;
            return (uint16_t)(sign | half);
        }
        uint32_t half = ((uint32_t)exponent << 10) | (mantissa >> 13);
        const uint32_t rest = mantissa & 0x1fffu;
        if (rest > 0x1000u || (rest == 0x1000u && (half & 1u))) half++;
        return (uint16_t)(sign | half);
    }

    float typesHalfToFloat(uint16_t half) {
        const uint32_t sign = (uint32_t)(half & 0x8000u) << 16;
        const uint32_t exponent = (half >> 10) & 0x1fu;
        uint32_t mantissa = half & 0x3ffu;
        uint32_t f;
        if (exponent == 0) {
            if (mantissa == 0) {
                f = sign;
            } else {
                int32_t e = -1;
                do {
                    e++;
                    mantissa <<= 1;
                } while ((mantissa & 0x400u) == 0);
                f = sign | ((uint32_t)(127 - 15 - e) << 23) | ((mantissa & 0x3ffu) << 13);
            }
        } else if (exponent == 31) {
            f = sign | 0x7f800000u | (mantissa << 13);
        } else {
            f = sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
        }
        float out;
        std::memcpy(&out, &f, sizeof(out));
        return out;
    }

    float typesQuantizeChannel(float value, Format format) {
        // NaN stores as 0: a value no op meant to write must not poison the hash or the GPU stream
        if (!(value == value)) return 0.0f;
        float out;
        if (format == Format::RGBA8) {
            const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
            out = std::floor(clamped * 255.0f + 0.5f) / 255.0f;
        } else {
            const float clamped = value < -kTypesHalfMax ? -kTypesHalfMax : (value > kTypesHalfMax ? kTypesHalfMax : value);
            out = typesHalfToFloat(typesFloatToHalf(clamped));
        }
        // -0 and +0 store the same: one value, one hash
        return out == 0.0f ? 0.0f : out;
    }

}

std::string TargetKey::describe() const {
    if (node >= 0) return "node " + std::to_string(node) + " primitive " + std::to_string(primitive < 0 ? 0 : primitive);
    return "submesh " + std::to_string(submesh < 0 ? 0 : submesh);
}

Vector4 TargetData::cornerValue(uint32_t corner, const std::vector<uint32_t>& indices) const {
    const auto it = std::lower_bound(corners.begin(), corners.end(), corner,
                                     [](const CornerValue& entry, uint32_t c) { return entry.corner < c; });
    if (it != corners.end() && it->corner == corner) return it->value;
    if (corner >= indices.size() || indices[corner] >= base.size()) return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
    return base[indices[corner]];
}

void TargetData::setCorner(uint32_t corner, const Vector4& value, const std::vector<uint32_t>& indices) {
    const auto it = std::lower_bound(corners.begin(), corners.end(), corner,
                                     [](const CornerValue& entry, uint32_t c) { return entry.corner < c; });
    const bool present = it != corners.end() && it->corner == corner;
    const bool isBase = corner < indices.size() && indices[corner] < base.size() && base[indices[corner]] == value;
    if (isBase) {
        if (present) corners.erase(it);
        return;
    }
    if (present) {
        it->value = value;
    } else {
        corners.insert(it, CornerValue{corner, value});
    }
}

TargetData* Data::find(const TargetKey& key) {
    const auto it = std::lower_bound(targets.begin(), targets.end(), key,
                                     [](const TargetData& target, const TargetKey& k) { return target.key < k; });
    return it != targets.end() && it->key == key ? &*it : nullptr;
}

const TargetData* Data::find(const TargetKey& key) const {
    const auto it = std::lower_bound(targets.begin(), targets.end(), key,
                                     [](const TargetData& target, const TargetKey& k) { return target.key < k; });
    return it != targets.end() && it->key == key ? &*it : nullptr;
}

TargetData& Data::add(const TargetKey& key) {
    const auto it = std::lower_bound(targets.begin(), targets.end(), key,
                                     [](const TargetData& target, const TargetKey& k) { return target.key < k; });
    if (it != targets.end() && it->key == key) return *it;
    TargetData target;
    target.key = key;
    return *targets.insert(it, std::move(target));
}

Vector4 initialValueFor(InitialValue initial, const Vector4& custom, const Vector4* asset) {
    switch (initial) {
        case InitialValue::Asset: return asset ? *asset : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        case InitialValue::White: return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        // opaque: R6 never starts from transparent black by accident
        case InitialValue::Black: return Vector4(0.0f, 0.0f, 0.0f, 1.0f);
        case InitialValue::Custom: return custom;
    }
    return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
}

Vector4 quantize(const Vector4& value, Format format) {
    return Vector4(typesQuantizeChannel(value.x, format), typesQuantizeChannel(value.y, format), typesQuantizeChannel(value.z, format),
                   typesQuantizeChannel(value.w, format));
}

}
