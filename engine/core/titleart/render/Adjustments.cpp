// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/adjustments.ts (adjustmentIsIdentity, the tone curves,
// applyAdjustment's parameters) by SafEight, used with the author's permission.

#include "Adjustments.h"

#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/render/TitleRenderServices.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    // titleart/render/adjust.glsl's operations
    enum AdjustOp : int {
        OP_LUT = 0,
        OP_EXPOSURE = 3,
        OP_VIBRANCE = 4,
        OP_HUE_SATURATION = 5,
        OP_COLOR_BALANCE = 6,
        OP_BLACK_WHITE = 7,
        OP_PHOTO_FILTER = 8,
        OP_GRADIENT_MAP = 9,
        OP_INVERT = 10,
        OP_POSTERIZE = 11,
        OP_THRESHOLD = 12
    };

    double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

    double num(const Json& o, const char* key, double fallback) {
        auto it = o.find(key);
        return it != o.end() && it->is_number() ? it->get<double>() : fallback;
    }

    bool flag(const Json& o, const char* key, bool fallback) {
        auto it = o.find(key);
        return it != o.end() && it->is_boolean() ? it->get<bool>() : fallback;
    }

    const Json& member(const Json& o, const char* key) {
        static const Json empty = Json::object();
        auto it = o.find(key);
        return it != o.end() && it->is_object() ? *it : empty;
    }

    std::string typeOf(const Json& adjustment) {
        return adjustment.is_object() ? adjustment.value("type", std::string()) : std::string();
    }

    // Photoshop Levels on one level: input range -> gamma (v^(1/g), g > 1 lightens) -> output range
    std::function<double(double)> levelsFn(const Json& ch) {
        const double lo = num(ch, "in_black", 0) / 255.0;
        const double hi = std::max(num(ch, "in_white", 255), num(ch, "in_black", 0) + 1.0) / 255.0;
        const double g = std::max(0.01, num(ch, "gamma", 1));
        const double ob = num(ch, "out_black", 0), ow = num(ch, "out_white", 255);
        return [=](double x) { return (ob + std::pow(clamp01((x - lo) / (hi - lo)), 1.0 / g) * (ow - ob)) / 255.0; };
    }

    // Modern Brightness/Contrast: brightness a midtone power curve (+150 ~ gamma 2.5), contrast an
    // S-curve on 50 % grey whose midtone slope is 1 + contrast / 100 (a fixed pivot)
    std::function<double(double)> brightnessContrastFn(double brightness, double contrast) {
        const double b = brightness / 100.0;
        const double e = b >= 0.0 ? 1.0 / (1.0 + b) : 1.0 - b;
        const double k = std::max(0.05, 1.0 + contrast / 100.0);
        return [=](double x) {
            const double y = std::pow(clamp01(x), e);
            return y < 0.5 ? 0.5 * std::pow(2.0 * y, k) : 1.0 - 0.5 * std::pow(2.0 * (1.0 - y), k);
        };
    }

    bool levelsIdentity(const Json& ch) {
        return num(ch, "in_black", 0) == 0 && num(ch, "in_white", 255) == 255 && num(ch, "gamma", 1) == 1 && num(ch, "out_black", 0) == 0 &&
               num(ch, "out_white", 255) == 255;
    }

    bool curveIdentity(const Json& curve) {
        const TitleCurve c = decodeCurve(curve);
        for (const TitleCurvePoint& p : c.points) {
            if (std::abs(p.y - p.x) >= 1e-4) return false;
        }
        return true;
    }

    bool rangeIdentity(const Json& r) {
        return num(r, "cyan_red", 0) == 0 && num(r, "magenta_green", 0) == 0 && num(r, "yellow_blue", 0) == 0;
    }

    uint16_t toHalf(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t sign = (bits >> 16) & 0x8000u;
        int32_t exponent = (int32_t)((bits >> 23) & 0xFF) - 127 + 15;
        uint32_t mantissa = bits & 0x7FFFFFu;
        if (exponent <= 0) {
            if (exponent < -10) return (uint16_t)sign;
            mantissa |= 0x800000u;
            const uint32_t shift = (uint32_t)(14 - exponent);
            uint32_t half = mantissa >> shift;
            if ((mantissa >> (shift - 1)) & 1u) half++;
            return (uint16_t)(sign | half);
        }
        if (exponent >= 31) return (uint16_t)(sign | 0x7C00u);
        uint32_t half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
        if (mantissa & 0x1000u) half++;
        return (uint16_t)half;
    }

}

bool titleart::adjustmentIsIdentity(const Json& adj) {
    const std::string type = typeOf(adj);
    if (type == "brightness_contrast") return num(adj, "brightness", 0) == 0 && num(adj, "contrast", 0) == 0;
    if (type == "levels") {
        const Json& ch = member(adj, "channels");
        for (const char* c : {"rgb", "r", "g", "b"}) {
            if (!levelsIdentity(member(ch, c))) return false;
        }
        return true;
    }
    if (type == "curves") {
        const Json& ch = member(adj, "channels");
        for (const char* c : {"rgb", "r", "g", "b"}) {
            auto it = ch.find(c);
            if (it != ch.end() && !curveIdentity(*it)) return false;
        }
        return true;
    }
    if (type == "exposure") return num(adj, "exposure", 0) == 0 && num(adj, "offset", 0) == 0 && num(adj, "gamma", 1) == 1;
    if (type == "vibrance") return num(adj, "vibrance", 0) == 0 && num(adj, "saturation", 0) == 0;
    if (type == "hue_saturation") {
        return !flag(adj, "colorize", false) && std::fmod(num(adj, "hue", 0), 360.0) == 0 && num(adj, "saturation", 0) == 0 &&
               num(adj, "lightness", 0) == 0;
    }
    if (type == "color_balance") {
        return rangeIdentity(member(adj, "shadows")) && rangeIdentity(member(adj, "midtones")) && rangeIdentity(member(adj, "highlights"));
    }
    if (type == "photo_filter") return num(adj, "density", 25) == 0;
    if (type == "effects_only") return true;
    if (type == "black_white" || type == "gradient_map" || type == "invert" || type == "posterize" || type == "threshold") return false;
    // an unknown type draws nothing (unknown_type, reported by the document's problems)
    return true;
}

bool titleart::adjustmentUsesToneLut(const Json& adj) {
    const std::string type = typeOf(adj);
    return type == "brightness_contrast" || type == "levels" || type == "curves";
}

void titleart::bakeToneLut(const Json& adj, float out[256 * 4]) {
    const std::string type = typeOf(adj);
    auto x = [](int i) { return i / 255.0; };
    for (int i = 0; i < 256; i++) {
        out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = (float)x(i);
        out[i * 4 + 3] = 1.0f;
    }
    if (type == "brightness_contrast") {
        const auto f = brightnessContrastFn(num(adj, "brightness", 0), num(adj, "contrast", 0));
        for (int i = 0; i < 256; i++) out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = (float)f(x(i));
    } else if (type == "levels") {
        // the composite first, then each channel's own levels (Photoshop)
        const Json& ch = member(adj, "channels");
        const auto rgb = levelsFn(member(ch, "rgb"));
        const std::function<double(double)> per[3] = {levelsFn(member(ch, "r")), levelsFn(member(ch, "g")), levelsFn(member(ch, "b"))};
        for (int i = 0; i < 256; i++) {
            const double v = rgb(x(i));
            for (int c = 0; c < 3; c++) out[i * 4 + c] = (float)clamp01(per[c](v));
        }
    } else if (type == "curves") {
        // each channel's curve, then the composite on its result (Photoshop)
        const Json& ch = member(adj, "channels");
        auto lutOf = [&ch](const char* name) {
            auto it = ch.find(name);
            TitleCurve c = it != ch.end() ? decodeCurve(*it) : TitleCurve();
            if (c.points.empty()) c.points = {{0.0, 0.0, false}, {1.0, 1.0, false}};
            return c.sample(256);
        };
        const std::vector<float> rgb = lutOf("rgb");
        const std::vector<float> per[3] = {lutOf("r"), lutOf("g"), lutOf("b")};
        auto at = [](const std::vector<float>& lut, double v) {
            const double p = clamp01(v) * 255.0;
            const int i = std::min(254, (int)std::floor(p));
            return lut[i] + (lut[i + 1] - lut[i]) * (p - i);
        };
        for (int i = 0; i < 256; i++) {
            for (int c = 0; c < 3; c++) out[i * 4 + c] = (float)at(rgb, per[c][i]);
        }
    }
}

bool titleart::setupAdjustment(const Json& adj, TitleRenderServices& services, TitleGpuPass& pass, std::shared_ptr<TitleCacheEntry>& lut) {
    if (adjustmentIsIdentity(adj)) return false;
    const std::string type = typeOf(adj);
    pass.program = TitleRenderProgram::Adjust;
    if (adjustmentUsesToneLut(adj)) {
        pass.params.set(2, (float)OP_LUT);
        const uint64_t key = hashCombine(0x746f6e656c7574ull, fnv1a64(writeTitleArtJson(adj)));
        const TitleBufferFormat& format = services.caps.format(TitleBuffer::ToneLut);
        const uint64_t formatKey = hashCombine(key, (uint64_t)format.format);
        lut = services.cache.find(formatKey);
        if (!lut) {
            float values[256 * 4];
            bakeToneLut(adj, values);
            auto entry = std::make_shared<TitleCacheEntry>();
            entry->key = formatKey;
            entry->upload = std::make_unique<TitleUpload>();
            bool made = false;
            if (format.format == ColorFormat::RGBA16F) {
                uint16_t halves[256 * 4];
                for (int i = 0; i < 256 * 4; i++) halves[i] = toHalf(values[i]);
                made = entry->upload->create("title art tone LUT", 256, 1, ColorFormat::RGBA16F, halves, sizeof(halves), TextureFilter::LINEAR);
            } else {
                uint8_t bytes[256 * 4];
                for (int i = 0; i < 256 * 4; i++) bytes[i] = (uint8_t)std::lround(clamp01(values[i]) * 255.0);
                made = entry->upload->create("title art tone LUT", 256, 1, ColorFormat::RGBA, bytes, sizeof(bytes), TextureFilter::LINEAR);
            }
            if (!made) return false;
            services.cache.insert(entry);
            lut = entry;
        }
        pass.tex[1] = lut->texture();
        return true;
    }
    if (type == "exposure") {
        pass.params.set(2, (float)OP_EXPOSURE, (float)num(adj, "exposure", 0), (float)num(adj, "offset", 0),
                        (float)std::max(0.01, num(adj, "gamma", 1)));
    } else if (type == "vibrance") {
        pass.params.set(2, (float)OP_VIBRANCE, (float)(num(adj, "vibrance", 0) / 100.0), (float)(num(adj, "saturation", 0) / 100.0));
    } else if (type == "hue_saturation") {
        const bool colorize = flag(adj, "colorize", false);
        const double hue = std::fmod(std::fmod(num(adj, "hue", 0) / 360.0, 1.0) + 1.0, 1.0);
        // colorize: Photoshop's 0..100 colour saturation; otherwise a -1..1 change
        const double sat = colorize ? clamp01(num(adj, "saturation", 0) / 100.0) : num(adj, "saturation", 0) / 100.0;
        pass.params.set(2, (float)OP_HUE_SATURATION, (float)hue, (float)sat, (float)(num(adj, "lightness", 0) / 100.0));
        pass.params.set(3, colorize ? 1.0f : 0.0f);
    } else if (type == "color_balance") {
        auto range = [&adj](const char* name, int index, TitleGpuPass& p) {
            const Json& r = member(adj, name);
            p.params.set(index, (float)(num(r, "cyan_red", 0) / 100.0), (float)(num(r, "magenta_green", 0) / 100.0),
                         (float)(num(r, "yellow_blue", 0) / 100.0));
        };
        pass.params.set(2, (float)OP_COLOR_BALANCE, flag(adj, "preserve_luminosity", true) ? 1.0f : 0.0f);
        range("shadows", 3, pass);
        range("midtones", 4, pass);
        range("highlights", 5, pass);
    } else if (type == "black_white") {
        TitleColor tint;
        parseTitleColor(adj.value("tint_color", Json("#e1d3b3")), tint);
        pass.params.set(2, (float)OP_BLACK_WHITE, (float)(num(adj, "reds", 40) / 100.0), (float)(num(adj, "yellows", 60) / 100.0),
                        (float)(num(adj, "greens", 40) / 100.0));
        pass.params.set(3, (float)(num(adj, "cyans", 60) / 100.0), (float)(num(adj, "blues", 20) / 100.0),
                        (float)(num(adj, "magentas", 80) / 100.0), flag(adj, "tint", false) ? 1.0f : 0.0f);
        // sRGB-encoded, like the colour the shader works on
        pass.params.set(4, tint.r, tint.g, tint.b);
    } else if (type == "photo_filter") {
        TitleColor color;
        parseTitleColor(adj.value("color", Json("#ec8a00")), color);
        pass.params.set(2, (float)OP_PHOTO_FILTER, (float)clamp01(num(adj, "density", 25) / 100.0),
                        flag(adj, "preserve_luminosity", true) ? 1.0f : 0.0f);
        pass.params.set(3, color.r, color.g, color.b);
    } else if (type == "gradient_map") {
        lut = services.gradientLut(decodeGradient(adj.value("gradient", Json::object())));
        if (!lut) return false;
        pass.params.set(2, (float)OP_GRADIENT_MAP, flag(adj, "reverse", false) ? 1.0f : 0.0f);
        pass.tex[1] = lut->texture();
    } else if (type == "invert") {
        pass.params.set(2, (float)OP_INVERT);
    } else if (type == "posterize") {
        pass.params.set(2, (float)OP_POSTERIZE, (float)std::clamp(std::round(num(adj, "levels", 4)), 2.0, 255.0));
    } else if (type == "threshold") {
        pass.params.set(2, (float)OP_THRESHOLD, (float)std::clamp(std::round(num(adj, "level", 128)), 1.0, 255.0));
    } else {
        return false;
    }
    return true;
}
