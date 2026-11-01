// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ColorMath.h"

#include "util/Color.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    float clamp01(float v) {
        return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
    }

    int hexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

}

Vector3 colormath::linearToSrgb(const Vector3& linear) {
    return Color::linearTosRGB(linear);
}

Vector3 colormath::srgbToLinear(const Vector3& srgb) {
    return Color::sRGBToLinear(srgb);
}

uint8_t colormath::toByte(float value) {
    return (uint8_t)std::lround(clamp01(value) * 255.0f);
}

float colormath::fromByte(uint8_t value) {
    return (float)value / 255.0f;
}

std::string colormath::toHex(const Vector4& srgb, bool alpha) {
    char text[16];
    if (alpha) {
        std::snprintf(text, sizeof(text), "%02X%02X%02X%02X", toByte(srgb.x), toByte(srgb.y), toByte(srgb.z), toByte(srgb.w));
    } else {
        std::snprintf(text, sizeof(text), "%02X%02X%02X", toByte(srgb.x), toByte(srgb.y), toByte(srgb.z));
    }
    return text;
}

bool colormath::fromHex(std::string_view text, Vector4& srgb, bool* hasAlpha) {
    while (!text.empty() && std::isspace((unsigned char)text.front())) text.remove_prefix(1);
    while (!text.empty() && std::isspace((unsigned char)text.back())) text.remove_suffix(1);
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6 && text.size() != 8) return false;
    uint8_t bytes[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < text.size() / 2; i++) {
        const int hi = hexDigit(text[i * 2]);
        const int lo = hexDigit(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = (uint8_t)(hi * 16 + lo);
    }
    srgb = Vector4(fromByte(bytes[0]), fromByte(bytes[1]), fromByte(bytes[2]), fromByte(bytes[3]));
    if (hasAlpha) *hasAlpha = text.size() == 8;
    return true;
}

Vector3 colormath::rgbToHsv(const Vector3& rgb) {
    const float r = rgb.x, g = rgb.y, b = rgb.z;
    const float maxC = std::max({r, g, b});
    const float minC = std::min({r, g, b});
    const float delta = maxC - minC;
    float h = 0.0f;
    if (delta > 0.0f) {
        if (maxC == r) h = std::fmod((g - b) / delta, 6.0f);
        else if (maxC == g) h = (b - r) / delta + 2.0f;
        else h = (r - g) / delta + 4.0f;
        h /= 6.0f;
        if (h < 0.0f) h += 1.0f;
    }
    const float s = maxC > 0.0f ? delta / maxC : 0.0f;
    return Vector3(h, s, maxC);
}

Vector3 colormath::hsvToRgb(const Vector3& hsv) {
    const float h = hsv.x - std::floor(hsv.x);
    const float s = clamp01(hsv.y);
    const float v = hsv.z;
    const float c = v * s;
    const float hp = h * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    Vector3 rgb;
    if (hp < 1.0f) rgb = Vector3(c, x, 0.0f);
    else if (hp < 2.0f) rgb = Vector3(x, c, 0.0f);
    else if (hp < 3.0f) rgb = Vector3(0.0f, c, x);
    else if (hp < 4.0f) rgb = Vector3(0.0f, x, c);
    else if (hp < 5.0f) rgb = Vector3(x, 0.0f, c);
    else rgb = Vector3(c, 0.0f, x);
    const float m = v - c;
    return Vector3(rgb.x + m, rgb.y + m, rgb.z + m);
}

void colormath::hdrDecompose(const Vector3& linear, Vector3& base, float& intensity) {
    const float maxC = std::max({linear.x, linear.y, linear.z});
    intensity = (std::isfinite(maxC) && maxC > 1.0f) ? std::min(std::log2(maxC), kMaxIntensity) : 0.0f;
    const float scale = std::exp2(-intensity);
    base = Vector3(clamp01(linear.x * scale), clamp01(linear.y * scale), clamp01(linear.z * scale));
}

Vector3 colormath::hdrCompose(const Vector3& base, float intensity) {
    const float scale = std::exp2(intensity);
    return Vector3(base.x * scale, base.y * scale, base.z * scale);
}

// ---- ColorPickerModel

colormath::ColorPickerModel::ColorPickerModel(const Vector4& stored, bool hdr, bool linearStorage, bool alpha)
    : start(stored), hdr(hdr), linear(linearStorage), alpha(alpha) {
    Vector3 rgb(stored.x, stored.y, stored.z);
    if (hdr) {
        Vector3 b;
        hdrDecompose(rgb, b, stops);
        rgb = b;
    }
    if (linear) rgb = linearToSrgb(Vector3(clamp01(rgb.x), clamp01(rgb.y), clamp01(rgb.z)));
    base = Vector4(clamp01(rgb.x), clamp01(rgb.y), clamp01(rgb.z), alpha ? clamp01(stored.w) : 1.0f);
}

void colormath::ColorPickerModel::setShown(const Vector4& srgb) {
    base = Vector4(clamp01(srgb.x), clamp01(srgb.y), clamp01(srgb.z), alpha ? clamp01(srgb.w) : base.w);
    dirty = true;
}

void colormath::ColorPickerModel::setShownRgb(const Vector3& srgb) {
    setShown(Vector4(srgb, base.w));
}

void colormath::ColorPickerModel::setAlpha(float a) {
    if (!alpha) return;
    base.w = clamp01(a);
    dirty = true;
}

void colormath::ColorPickerModel::setIntensity(float intensity) {
    if (!hdr) return;
    stops = std::isfinite(intensity) ? std::clamp(intensity, kMinIntensity, kMaxIntensity) : 0.0f;
    dirty = true;
}

bool colormath::ColorPickerModel::setHex(std::string_view text) {
    Vector4 srgb;
    bool hasAlpha = false;
    if (!fromHex(text, srgb, &hasAlpha)) return false;
    setShown(Vector4(srgb.x, srgb.y, srgb.z, hasAlpha ? srgb.w : base.w));
    return true;
}

void colormath::ColorPickerModel::setLinear(const Vector4& value, bool* clamped) {
    Vector3 rgb(value.x, value.y, value.z);
    bool wasClamped = false;
    if (hdr) {
        Vector3 b;
        hdrDecompose(rgb, b, stops);
        rgb = b;
    } else {
        wasClamped = rgb.x > 1.0f || rgb.y > 1.0f || rgb.z > 1.0f;
    }
    const Vector3 srgb = linearToSrgb(Vector3(clamp01(rgb.x), clamp01(rgb.y), clamp01(rgb.z)));
    setShown(Vector4(srgb, alpha ? value.w : base.w));
    if (clamped) *clamped = wasClamped;
}

std::string colormath::ColorPickerModel::hex() const {
    return toHex(base, alpha);
}

Vector4 colormath::ColorPickerModel::value() const {
    if (!dirty) return start;
    Vector3 rgb(base.x, base.y, base.z);
    if (linear) rgb = srgbToLinear(rgb);
    if (hdr) rgb = hdrCompose(rgb, stops);
    return Vector4(rgb, alpha ? base.w : start.w);
}

Vector4 colormath::ColorPickerModel::linearValue() const {
    const Vector4 v = value();
    if (linear) return v;
    return Vector4(srgbToLinear(Vector3(v.x, v.y, v.z)), v.w);
}
