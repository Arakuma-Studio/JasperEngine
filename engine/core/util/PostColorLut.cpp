// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PostColorLut.h"

#include "Log.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{
    const float GAMMA = 2.2f;

    float saturate(float v){
        return std::min(std::max(v, 0.0f), 1.0f);
    }

    Vector3 saturate(const Vector3& v){
        return Vector3(saturate(v.x), saturate(v.y), saturate(v.z));
    }

    Vector3 powv(const Vector3& v, float e){
        return Vector3(std::pow(std::max(v.x, 0.0f), e), std::pow(std::max(v.y, 0.0f), e), std::pow(std::max(v.z, 0.0f), e));
    }

    Vector3 mixv(const Vector3& a, const Vector3& b, float t){
        return a + (b - a) * t;
    }

    // rows of a 3x3 matrix times a vector
    Vector3 mul(const float m[9], const Vector3& v){
        return Vector3(m[0] * v.x + m[1] * v.y + m[2] * v.z,
                       m[3] * v.x + m[4] * v.y + m[5] * v.z,
                       m[6] * v.x + m[7] * v.y + m[8] * v.z);
    }

    // linear sRGB primaries <-> LMS (CAT02 cone space): white balance scales the cones
    const float LINEAR_TO_LMS[9] = {
        0.390405f, 0.549941f, 0.00892632f,
        0.0708416f, 0.963172f, 0.00135775f,
        0.0231082f, 0.128021f, 0.936245f
    };
    const float LMS_TO_LINEAR[9] = {
        2.85847f, -1.62879f, -0.024891f,
        -0.210182f, 1.1582f, 0.000324281f,
        -0.041812f, -0.118169f, 1.06867f
    };

    // the cone scales that move the white of a temperature / tint shift (-1..1) back to D65:
    // the shifted white is a point of the daylight locus (x from the temperature, y from the
    // locus plus the tint) converted to LMS
    Vector3 whiteBalanceScales(float temperature, float tint){
        const float t1 = temperature * 100.0f / 65.0f;
        const float t2 = tint * 100.0f / 65.0f;
        const float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
        const float locusY = 2.87f * x - 3.0f * x * x - 0.27509507f;
        const float y = locusY + t2 * 0.05f;
        // xyY (Y = 1) to XYZ to LMS
        const float X = x / y;
        const float Z = (1.0f - x - y) / y;
        const Vector3 lms(0.7328f * X + 0.4296f - 0.1624f * Z,
                          -0.7036f * X + 1.6975f + 0.0061f * Z,
                          0.0030f * X + 0.0136f + 0.9834f * Z);
        const Vector3 d65(0.949237f, 1.03542f, 1.08728f);
        return Vector3(d65.x / lms.x, d65.y / lms.y, d65.z / lms.z);
    }

    float luma(const Vector3& c){
        return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    }

    // AgX, the minimal approximation (Wrensch): the sRGB colour into AgX's inset space, a
    // log2 encoding of -12.47..4.03 stops, a sixth-degree fit of the default contrast curve,
    // then back out of the inset space and decoded with gamma 2.2
    Vector3 agx(const Vector3& color){
        static const float INSET[9] = {
            0.842479062253094f, 0.0784335999999992f, 0.0792237451477643f,
            0.0423282422610123f, 0.878468636469772f, 0.0791661274605434f,
            0.0423756549057051f, 0.0784336f, 0.879142973793104f
        };
        static const float OUTSET[9] = {
            1.19687900512017f, -0.0980208811401368f, -0.0990297440797205f,
            -0.0528968517574562f, 1.15190312990417f, -0.0989611768448433f,
            -0.0529716355144438f, -0.0980434501171241f, 1.15107367264116f
        };
        const float minEv = -12.47393f;
        const float maxEv = 4.026069f;
        Vector3 v = mul(INSET, color);
        for (int i = 0; i < 3; i++){
            float e = std::log2(std::max(v[i], 1e-10f));
            e = (std::min(std::max(e, minEv), maxEv) - minEv) / (maxEv - minEv);
            const float x2 = e * e;
            const float x4 = x2 * x2;
            v[i] = 15.5f * x4 * x2 - 40.14f * x4 * e + 31.96f * x4 - 6.868f * x2 * e + 0.4298f * x2 + 0.1191f * e - 0.00232f;
        }
        v = mul(OUTSET, v);
        return powv(v, GAMMA);
    }

    bool hasMonochrome(const PostProcessingSettings& s){
        return s.monochromeEnabled && s.monochromeIntensity > 0.0f;
    }

    // the grading fields away from neutral (the user LUT aside)
    bool hasParametricGrade(const PostProcessingSettings& s){
        if (!s.colorGradingEnabled)
            return false;
        const PostProcessingSettings d;
        return s.temperature != d.temperature || s.tint != d.tint || s.contrast != d.contrast || s.saturation != d.saturation ||
               s.lift != d.lift || s.gamma != d.gamma || s.gain != d.gain;
    }

    // what the colour transform needs of the settings and their LUT, worked out once per bake
    // (or grade call) instead of per texel
    struct Grader{
        const PostProcessingSettings& s;
        bool whiteBalance = false;
        Vector3 whiteScales;
        Vector3 inverseGamma;
        const CubeLut* lut = nullptr;   // the user LUT when one applies, at lutStage
        LutStage lutStage = LutStage::DISPLAY;
        LutInputEncoding lutEncoding = LutInputEncoding::SRGB;
        float lutMix = 0.0f;

        Grader(const PostProcessingSettings& settings, const CubeLut* userLut): s(settings){
            whiteBalance = s.colorGradingEnabled && (s.temperature != 0.0f || s.tint != 0.0f);
            if (whiteBalance)
                whiteScales = whiteBalanceScales(s.temperature, s.tint);
            for (int i = 0; i < 3; i++)
                inverseGamma[i] = 1.0f / std::max(s.gamma[i], 0.01f);
            if (PostColorLut::getLutStage(settings, userLut, lutStage)){
                lut = userLut;
                lutEncoding = resolveEncoding(s.lutInputEncoding, *lut);
                lutMix = saturate(s.lutContribution);
            }
        }

        // the exposed linear scene colour with a scene-stage LUT mixed in (the curve's input)
        Vector3 scene(const Vector3& exposed) const{
            if (!lut || lutStage != LutStage::SCENE)
                return exposed;
            return mixv(exposed, applySceneStage(*lut, lutEncoding, exposed), lutMix);
        }

        // a linear display colour white balanced, then display-encoded
        Vector3 encode(const Vector3& linearDisplay) const{
            Vector3 c = linearDisplay;
            if (whiteBalance){
                Vector3 lms = mul(LINEAR_TO_LMS, c);
                lms = Vector3(lms.x * whiteScales.x, lms.y * whiteScales.y, lms.z * whiteScales.z);
                c = mul(LMS_TO_LINEAR, lms);
            }
            return powv(c, 1.0f / GAMMA);
        }

        // the grade of a display-encoded colour from the contrast on, clamped
        Vector3 grade(Vector3 e) const{
            if (s.colorGradingEnabled){
                e = (e - Vector3(0.5f, 0.5f, 0.5f)) * s.contrast + Vector3(0.5f, 0.5f, 0.5f);
                for (int i = 0; i < 3; i++){
                    const float v = std::max(s.gain[i] * (e[i] + s.lift[i] * (1.0f - e[i])), 0.0f);
                    e[i] = s.gamma[i] != 1.0f ? std::pow(v, inverseGamma[i]) : v;
                }
                const float l = luma(e);
                e = Vector3(l, l, l) + (e - Vector3(l, l, l)) * s.saturation;
            }
            if (s.monochromeEnabled){
                Vector3 mono;
                if (s.monochromeMode == MonochromeMode::SEPIA){
                    static const float SEPIA[9] = {
                        0.393f, 0.769f, 0.189f,
                        0.349f, 0.686f, 0.168f,
                        0.272f, 0.534f, 0.131f
                    };
                    mono = mul(SEPIA, e);
                }else{
                    const float l = luma(e);
                    mono = Vector3(l, l, l);
                }
                e = mixv(e, mono, saturate(s.monochromeIntensity));
            }
            if (lut && lutStage == LutStage::DISPLAY)
                e = mixv(e, applyDisplayStage(*lut, lutEncoding, saturate(e)), lutMix);
            return saturate(e);
        }
    };
}

Vector3 PostColorLut::toneMap(TonemapMode mode, const Vector3& input){
    const Vector3 c(std::max(input.x, 0.0f), std::max(input.y, 0.0f), std::max(input.z, 0.0f));
    switch (mode){
        case TonemapMode::ACES: {
            // Narkowicz's fit of the ACES RRT + ODT, its 0.6 exposure applied
            Vector3 out;
            for (int i = 0; i < 3; i++){
                const float x = c[i] * 0.6f;
                out[i] = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
            }
            return saturate(out);
        }
        case TonemapMode::NEUTRAL: {
            // Khronos PBR Neutral: identity (minus a small toe offset) up to 0.76, then the peak
            // compressed towards 1 and desaturated towards white
            const float startCompression = 0.8f - 0.04f;
            const float desaturation = 0.15f;
            Vector3 color = c;
            const float x = std::min(color.x, std::min(color.y, color.z));
            const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
            color = color - Vector3(offset, offset, offset);
            const float peak = std::max(color.x, std::max(color.y, color.z));
            if (peak < startCompression)
                return saturate(color);
            const float d = 1.0f - startCompression;
            const float newPeak = 1.0f - d * d / (peak + d - startCompression);
            color = color * (newPeak / peak);
            const float g = 1.0f - 1.0f / (desaturation * (peak - newPeak) + 1.0f);
            return saturate(mixv(color, Vector3(newPeak, newPeak, newPeak), g));
        }
        case TonemapMode::REINHARD:
            return Vector3(c.x / (1.0f + c.x), c.y / (1.0f + c.y), c.z / (1.0f + c.z));
        case TonemapMode::AGX:
            return saturate(agx(c));
        case TonemapMode::NONE:
        default:
            return saturate(c);
    }
}

bool PostColorLut::getLutStage(const PostProcessingSettings& settings, const CubeLut* lut, LutStage& stage){
    if (!settings.colorGradingEnabled || settings.lutTexture.empty() || settings.lutContribution <= 0.0f || !lut || lut->empty())
        return false;
    stage = stageOf(settings.lutInputEncoding, *lut);
    return true;
}

bool PostColorLut::hasGrade(const PostProcessingSettings& s){
    return hasMonochrome(s) || hasParametricGrade(s) || (s.colorGradingEnabled && !s.lutTexture.empty() && s.lutContribution > 0.0f);
}

bool PostColorLut::hasGrade(const PostProcessingSettings& s, const CubeLut* lut){
    LutStage stage = LutStage::DISPLAY;
    return hasMonochrome(s) || hasParametricGrade(s) || (getLutStage(s, lut, stage) && stage == LutStage::DISPLAY);
}

bool PostColorLut::hasCurve(const PostProcessingSettings& s){
    return s.exposure != 0.0f || s.tonemap != TonemapMode::NONE;
}

bool PostColorLut::hasCurve(const PostProcessingSettings& s, const CubeLut* lut){
    LutStage stage = LutStage::DISPLAY;
    return hasCurve(s) || (getLutStage(s, lut, stage) && stage == LutStage::SCENE);
}

Vector3 PostColorLut::grade(const PostProcessingSettings& s, const CubeLut* lut, const Vector3& linearDisplay){
    const Grader grader(s, lut);
    return grader.grade(grader.encode(linearDisplay));
}

void PostColorLut::bake(const PostProcessingSettings& settings, const CubeLut* lut, Stage stage, int n, std::vector<uint8_t>& rgba,
                        float shaper){
    rgba.resize((size_t)n * n * n * 4);
    const Grader grader(settings, lut);
    const float exposure = std::exp2(settings.exposure);
    const size_t width = (size_t)n * n;
    // per axis value: the texel's coordinate, and what it decodes to (the display domain's
    // linear colour, or the shaper's inverse: s = sqrt(x / (x + k)), x = k s^2 / (1 - s^2))
    std::vector<float> coords((size_t)n);
    std::vector<float> decoded((size_t)n);
    for (int i = 0; i < n; i++){
        const float c = (float)i / (n - 1);
        coords[i] = c;
        if (stage == Stage::GRADE){
            decoded[i] = std::pow(c, GAMMA);
        }else{
            const float s2 = c * c;
            decoded[i] = s2 < 1.0f ? shaper * s2 / (1.0f - s2) : 65504.0f;
        }
    }
    for (int b = 0; b < n; b++){
        for (int g = 0; g < n; g++){
            for (int r = 0; r < n; r++){
                const Vector3 value(decoded[r], decoded[g], decoded[b]);
                Vector3 out;
                if (stage == Stage::GRADE){
                    // without white balance the encoding undoes the decoding: the coordinate itself
                    out = grader.grade(grader.whiteBalance ? grader.encode(value) : Vector3(coords[r], coords[g], coords[b]));
                }else{
                    const Vector3 display = toneMap(settings.tonemap, grader.scene(value * exposure));
                    out = (stage == Stage::ALL) ? grader.grade(grader.encode(display)) : saturate(powv(display, 1.0f / GAMMA));
                }
                const size_t i = ((size_t)g * width + (size_t)b * n + (size_t)r) * 4;
                rgba[i + 0] = (uint8_t)std::lround(saturate(out.x) * 255.0f);
                rgba[i + 1] = (uint8_t)std::lround(saturate(out.y) * 255.0f);
                rgba[i + 2] = (uint8_t)std::lround(saturate(out.z) * 255.0f);
                rgba[i + 3] = 255;
            }
        }
    }
}

Vector3 PostColorLut::evaluate(const PostProcessingSettings& settings, const CubeLut* lut, Stage stage, const Vector3& code, float shaper){
    const Grader grader(settings, lut);
    const float exposure = std::exp2(settings.exposure);
    auto decode = [shaper](float c){
        const float s2 = saturate(c) * saturate(c);
        return s2 < 1.0f ? shaper * s2 / (1.0f - s2) : 65504.0f;
    };
    const Vector3 value(decode(code.x), decode(code.y), decode(code.z));
    const Vector3 display = toneMap(settings.tonemap, grader.scene(value * exposure));
    const Vector3 out = (stage == Stage::ALL) ? grader.grade(grader.encode(display)) : saturate(powv(display, 1.0f / GAMMA));
    return saturate(out);
}

Vector3 PostColorLut::invert(const PostProcessingSettings& settings, const CubeLut* lut, Stage stage, const Vector3& display, float shaper){
    Vector3 code = saturate(display);
    for (int sweep = 0; sweep < 8; sweep++){
        for (unsigned c = 0; c < 3; c++){
            float lo = 0.0f;
            float hi = 0.9995f;
            for (int step = 0; step < 24; step++){
                Vector3 probe = code;
                probe[c] = 0.5f * (lo + hi);
                if (evaluate(settings, lut, stage, probe, shaper)[c] < display[c])
                    lo = probe[c];
                else
                    hi = probe[c];
            }
            code[c] = 0.5f * (lo + hi);
        }
    }
    return code;
}

bool PostColorLut::loadStrip(const std::string& path, Strip& strip, std::string* error){
    strip = Strip();
    if (path.empty()){
        if (error)
            *error = "No LUT path.";
        return false;
    }
    TextureData data;
    if (!data.loadTextureFromFile(path.c_str())){
        if (error)
            *error = "The image could not be loaded.";
        return false;
    }
    const int width = data.getWidth();
    const int height = data.getHeight();
    if (height < 2 || width != height * height || data.getColorFormat() != ColorFormat::RGBA){
        Log::error("Post-processing LUT %s must be an RGB(A) strip of N*N x N pixels (got %d x %d)", path.c_str(), width, height);
        if (error)
            *error = "The image must be an RGB(A) strip of N*N x N pixels (got " + std::to_string(width) + " x " + std::to_string(height) + ").";
        return false;
    }
    strip.size = height;
    strip.width = width;
    strip.rgb.resize((size_t)width * height * 3);
    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            const size_t i = ((size_t)y * width + x) * 3;
            strip.rgb[i + 0] = data.getColorComponent(x, y, 0);
            strip.rgb[i + 1] = data.getColorComponent(x, y, 1);
            strip.rgb[i + 2] = data.getColorComponent(x, y, 2);
        }
    }
    return true;
}

void PostColorLut::stripToCubeLut(const Strip& strip, CubeLut& lut){
    lut = CubeLut();
    lut.fromStrip = true;
    const int n = strip.size;
    if (n < 2 || strip.width != n * n || strip.rgb.size() < (size_t)strip.width * n * 3)
        return;
    lut.size3D = n;
    lut.lut3D.resize((size_t)n * n * n * 3);
    for (int b = 0; b < n; b++){
        for (int g = 0; g < n; g++){
            // green 0 is the bottom row (Unity's strips)
            const size_t row = (size_t)(n - 1 - g) * strip.width;
            for (int r = 0; r < n; r++){
                const uint8_t* src = &strip.rgb[(row + (size_t)b * n + r) * 3];
                float* dst = &lut.lut3D[(((size_t)b * n + g) * n + r) * 3];
                dst[0] = src[0] / 255.0f;
                dst[1] = src[1] / 255.0f;
                dst[2] = src[2] / 255.0f;
            }
        }
    }
}
