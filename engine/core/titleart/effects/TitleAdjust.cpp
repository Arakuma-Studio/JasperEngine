// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/engine/adjustments.ts (the tone lookups, the uniforms of each
// operation, adjustmentIsIdentity) by SafEight, used with the author's permission.

#include "TitleAdjust.h"
#include "TitleFx.h"

#include "texture/TexturePrepare.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    double clamp01(double v) {
        return std::min(1.0, std::max(0.0, v));
    }

    double num(const Json& o, const char* key, double fallback = 0.0) {
        auto it = o.find(key);
        return it != o.end() && it->is_number() ? it->get<double>() : fallback;
    }

    bool flagOf(const Json& o, const char* key) {
        auto it = o.find(key);
        return it != o.end() && it->is_boolean() && it->get<bool>();
    }

    const Json& member(const Json& o, const char* key) {
        static const Json empty = Json::object();
        auto it = o.find(key);
        return it != o.end() && it->is_object() ? *it : empty;
    }

    std::string typeOf(const Json& adjustment) {
        return adjustment.is_object() ? adjustment.value("type", std::string()) : std::string();
    }

    const char* const CHANNELS[] = {"rgb", "r", "g", "b"};

    // Photoshop Levels on one 0..1 level: input range, gamma (v^(1/g), g > 1 lightens), output range
    double levels(const Json& ch, double x) {
        const double lo = num(ch, "in_black", 0) / 255.0;
        const double hi = std::max(num(ch, "in_white", 255), num(ch, "in_black", 0) + 1.0) / 255.0;
        const double g = std::max(0.01, num(ch, "gamma", 1));
        const double outBlack = num(ch, "out_black", 0), outWhite = num(ch, "out_white", 255);
        return (outBlack + std::pow(clamp01((x - lo) / (hi - lo)), 1.0 / g) * (outWhite - outBlack)) / 255.0;
    }

    // modern Brightness/Contrast: brightness a midtone power curve (+150 ~ gamma 2.5, -150 ~ 0.4),
    // contrast an S-curve about 50 % grey with midtone slope 1 + contrast / 100 (a fixed pivot:
    // Photoshop's image-mean pivot would depend on pixels outside the layer)
    double brightnessContrast(double brightness, double contrast, double x) {
        const double b = brightness / 100.0;
        const double e = b >= 0.0 ? 1.0 / (1.0 + b) : 1.0 - b;
        const double k = std::max(0.05, 1.0 + contrast / 100.0);
        const double y = std::pow(clamp01(x), e);
        return y < 0.5 ? 0.5 * std::pow(2.0 * y, k) : 1.0 - 0.5 * std::pow(2.0 * (1.0 - y), k);
    }

    // a baked curve read between its samples
    double lutAt(const std::vector<float>& lut, double v) {
        const double p = clamp01(v) * (double)(lut.size() - 1);
        const size_t i = std::min(lut.size() - 2, (size_t)std::floor(p));
        return lut[i] + (lut[i + 1] - lut[i]) * (p - (double)i);
    }

}

TitleAdjustOp titleart::titleAdjustOp(const Json& adjustment) {
    const std::string type = typeOf(adjustment);
    if (type == "brightness_contrast" || type == "levels" || type == "curves") return TitleAdjustOp::Lut;
    if (type == "exposure") return TitleAdjustOp::Exposure;
    if (type == "vibrance") return TitleAdjustOp::Vibrance;
    if (type == "hue_saturation") return TitleAdjustOp::HueSaturation;
    if (type == "color_balance") return TitleAdjustOp::ColorBalance;
    if (type == "black_white") return TitleAdjustOp::BlackWhite;
    if (type == "photo_filter") return TitleAdjustOp::PhotoFilter;
    if (type == "gradient_map") return TitleAdjustOp::GradientMap;
    if (type == "invert") return TitleAdjustOp::Invert;
    if (type == "posterize") return TitleAdjustOp::Posterize;
    if (type == "threshold") return TitleAdjustOp::Threshold;
    return TitleAdjustOp::None;
}

bool titleart::titleAdjustmentIsIdentity(const Json& a) {
    const std::string type = typeOf(a);
    if (type == "brightness_contrast") return num(a, "brightness") == 0 && num(a, "contrast") == 0;
    if (type == "levels") {
        const Json& channels = member(a, "channels");
        return std::all_of(std::begin(CHANNELS), std::end(CHANNELS), [&](const char* c) {
            const Json& ch = member(channels, c);
            return num(ch, "in_black", 0) == 0 && num(ch, "in_white", 255) == 255 && num(ch, "gamma", 1) == 1 &&
                   num(ch, "out_black", 0) == 0 && num(ch, "out_white", 255) == 255;
        });
    }
    if (type == "curves") {
        const Json& channels = member(a, "channels");
        return std::all_of(std::begin(CHANNELS), std::end(CHANNELS), [&](const char* c) {
            const TitleCurve curve = decodeCurve(member(channels, c));
            return std::all_of(curve.points.begin(), curve.points.end(), [](const TitleCurvePoint& p) { return std::fabs(p.y - p.x) < 1e-4; });
        });
    }
    if (type == "exposure") return num(a, "exposure") == 0 && num(a, "offset") == 0 && num(a, "gamma", 1) == 1;
    if (type == "vibrance") return num(a, "vibrance") == 0 && num(a, "saturation") == 0;
    if (type == "hue_saturation") {
        return !flagOf(a, "colorize") && std::fmod(num(a, "hue"), 360.0) == 0 && num(a, "saturation") == 0 && num(a, "lightness") == 0;
    }
    if (type == "color_balance") {
        for (const char* range : {"shadows", "midtones", "highlights"}) {
            const Json& r = member(a, range);
            if (num(r, "cyan_red") != 0 || num(r, "magenta_green") != 0 || num(r, "yellow_blue") != 0) return false;
        }
        return true;
    }
    if (type == "photo_filter") return num(a, "density", 25) == 0;
    if (type == "black_white" || type == "gradient_map" || type == "invert" || type == "posterize" || type == "threshold") return false;
    // effects_only and unknown types change no colour
    return true;
}

bool titleart::bakeTitleToneCurve(const Json& a, std::vector<float>& rgb) {
    const std::string type = typeOf(a);
    const int n = TITLE_LUT_SIZE;
    rgb.assign((size_t)n * 3, 0.0f);
    auto x = [n](int i) { return (double)i / (n - 1); };
    if (type == "brightness_contrast") {
        for (int i = 0; i < n; i++) {
            const float v = (float)brightnessContrast(num(a, "brightness"), num(a, "contrast"), x(i));
            rgb[(size_t)i * 3] = rgb[(size_t)i * 3 + 1] = rgb[(size_t)i * 3 + 2] = v;
        }
        return true;
    }
    const Json& channels = member(a, "channels");
    if (type == "levels") {
        for (int i = 0; i < n; i++) {
            const double v = levels(member(channels, "rgb"), x(i));
            for (int c = 0; c < 3; c++) rgb[(size_t)i * 3 + c] = (float)clamp01(levels(member(channels, CHANNELS[c + 1]), v));
        }
        return true;
    }
    if (type == "curves") {
        // Photoshop applies each channel's curve, then the composite's on its result
        const std::vector<float> composite = decodeCurve(member(channels, "rgb")).sample(n);
        for (int c = 0; c < 3; c++) {
            const std::vector<float> own = decodeCurve(member(channels, CHANNELS[c + 1])).sample(n);
            for (int i = 0; i < n; i++) rgb[(size_t)i * 3 + c] = (float)lutAt(composite, own[(size_t)i]);
        }
        return true;
    }
    return false;
}

TitleLutData titleart::titleToneLut(const Json& adjustment, ColorFormat format) {
    std::vector<float> rgb;
    bakeTitleToneCurve(adjustment, rgb);
    TitleLutData data;
    data.format = format == ColorFormat::RGBA16F ? ColorFormat::RGBA16F : ColorFormat::RGBA;
    const int n = TITLE_LUT_SIZE;
    if (data.format == ColorFormat::RGBA16F) {
        data.bytes.resize((size_t)n * 8);
        for (int i = 0; i < n; i++) {
            const uint16_t texel[4] = {floatToHalf(rgb[(size_t)i * 3]), floatToHalf(rgb[(size_t)i * 3 + 1]), floatToHalf(rgb[(size_t)i * 3 + 2]),
                                       floatToHalf(1.0f)};
            std::memcpy(&data.bytes[(size_t)i * 8], texel, sizeof(texel));
        }
    } else {
        data.bytes.resize((size_t)n * 4);
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 3; c++) data.bytes[(size_t)i * 4 + c] = (uint8_t)std::lround(clamp01(rgb[(size_t)i * 3 + c]) * 255.0);
            data.bytes[(size_t)i * 4 + 3] = 255;
        }
    }
    return data;
}

bool titleart::applyTitleAdjustment(EffectContext& ctx, const TargetRef& src, const Json& a, TargetRef& dst) {
    const TitleAdjustOp op = titleAdjustOp(a);
    if (op == TitleAdjustOp::None) return fx::copy(ctx, src, dst);
    TitlePass pass = fx::makePass(ctx, TitleProgram::Adjust, "adjust", dst);
    pass.textures[0].target = &src;
    pass.set(2, (float)op);
    auto colorOf = [&](const char* key) {
        TitleColor c;
        parseTitleColor(a.value(key, Json("#000000")), c);
        return c;
    };
    switch (op) {
        case TitleAdjustOp::Lut: {
            const TitleLut* lut = ctx.runner.lut(titleToneLut(a, ctx.runner.caps().format(TitleBuffer::ToneLut).format));
            if (!lut) return false;
            pass.textures[1].lut = lut;
            break;
        }
        case TitleAdjustOp::Exposure:
            pass.set(3, (float)num(a, "exposure"), (float)num(a, "offset"), (float)std::max(0.01, num(a, "gamma", 1)));
            break;
        case TitleAdjustOp::Vibrance:
            pass.set(3, (float)(num(a, "vibrance") / 100.0), (float)(num(a, "saturation") / 100.0));
            break;
        case TitleAdjustOp::HueSaturation: {
            const bool colorize = flagOf(a, "colorize");
            const double hue = std::fmod(std::fmod(num(a, "hue") / 360.0, 1.0) + 1.0, 1.0);
            // Colorize's saturation is Photoshop's 0..100 colour saturation; otherwise a -1..1 change
            const double sat = colorize ? clamp01(num(a, "saturation") / 100.0) : num(a, "saturation") / 100.0;
            pass.set(3, (float)hue, (float)sat, (float)(num(a, "lightness") / 100.0), colorize ? 1.0f : 0.0f);
            break;
        }
        case TitleAdjustOp::ColorBalance: {
            auto range = [&](int index, const char* key, float w) {
                const Json& r = member(a, key);
                pass.set(index, (float)(num(r, "cyan_red") / 100.0), (float)(num(r, "magenta_green") / 100.0),
                         (float)(num(r, "yellow_blue") / 100.0), w);
            };
            range(3, "shadows", flagOf(a, "preserve_luminosity") ? 1.0f : 0.0f);
            range(4, "midtones", 0.0f);
            range(5, "highlights", 0.0f);
            break;
        }
        case TitleAdjustOp::BlackWhite: {
            const TitleColor tint = colorOf("tint_color");
            pass.set(3, (float)(num(a, "reds", 40) / 100.0), (float)(num(a, "yellows", 60) / 100.0), (float)(num(a, "greens", 40) / 100.0),
                     (float)(num(a, "cyans", 60) / 100.0));
            pass.set(4, (float)(num(a, "blues", 20) / 100.0), (float)(num(a, "magentas", 80) / 100.0), flagOf(a, "tint") ? 1.0f : 0.0f);
            // sRGB-encoded, like the colour the shader works on
            pass.set(5, tint.r, tint.g, tint.b);
            break;
        }
        case TitleAdjustOp::PhotoFilter: {
            const TitleColor c = colorOf("color");
            pass.set(3, c.r, c.g, c.b, (float)clamp01(num(a, "density", 25) / 100.0));
            pass.set(4, flagOf(a, "preserve_luminosity") ? 1.0f : 0.0f);
            break;
        }
        case TitleAdjustOp::GradientMap: {
            const TitleLut* lut = ctx.runner.lut(fx::gradientLut(decodeGradient(a.value("gradient", Json::object()))));
            if (!lut) return false;
            pass.textures[1].lut = lut;
            pass.set(3, flagOf(a, "reverse") ? 1.0f : 0.0f);
            break;
        }
        case TitleAdjustOp::Posterize:
            pass.set(3, (float)std::min(255.0, std::max(2.0, std::round(num(a, "levels", 4)))));
            break;
        case TitleAdjustOp::Threshold:
            pass.set(3, (float)std::min(255.0, std::max(1.0, std::round(num(a, "level", 128)))));
            break;
        case TitleAdjustOp::Invert:
        case TitleAdjustOp::None:
            break;
    }
    return ctx.runner.run(pass);
}
