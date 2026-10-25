// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SceneSettings.h"
#include "shader/ShaderManifest.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

namespace{
    const char* const tonemapNames[] = {"none", "aces", "neutral", "reinhard", "agx", nullptr};
    const char* const tonemapValues[] = {"NONE", "ACES", "NEUTRAL", "REINHARD", "AGX", nullptr};
    const char* const monochromeNames[] = {"grayscale", "sepia", nullptr};
    const char* const monochromeValues[] = {"GRAYSCALE", "SEPIA", nullptr};

    using F = PostProcessingSettings::Field;
    using T = PostProcessingSettings::FieldType;

    // one row per member: name, snake name, type, accessor, range, help
    #define PP_FIELD(member, snake, type, minV, maxV, help) \
        F{#member, snake, type, [](PostProcessingSettings& s) -> void* { return &s.member; }, nullptr, nullptr, nullptr, minV, maxV, help}
    #define PP_ENUM(member, snake, names, cppType, values, help) \
        F{#member, snake, T::Enum, [](PostProcessingSettings& s) -> void* { return &s.member; }, names, cppType, values, 0.0f, 0.0f, help}
}

const std::vector<PostProcessingSettings::Field>& PostProcessingSettings::getFields(){
    static const std::vector<Field> fields = {
        PP_FIELD(exposure, "exposure", T::Float, -10.0f, 10.0f, "exposure in stops before the tone mapping (0 none)"),
        PP_ENUM(tonemap, "tonemap", tonemapNames, "TonemapMode", tonemapValues,
                "tone mapping curve: none (clamp), aces (fitted), neutral (Khronos PBR Neutral), reinhard, agx"),
        PP_FIELD(colorGradingEnabled, "color_grading_enabled", T::Bool, 0.0f, 1.0f, "colour grade (white balance, contrast, lift/gamma/gain, saturation, LUT)"),
        PP_FIELD(temperature, "temperature", T::Float, -1.0f, 1.0f, "white balance temperature, -1 cool .. 1 warm"),
        PP_FIELD(tint, "tint", T::Float, -1.0f, 1.0f, "white balance tint, -1 green .. 1 magenta"),
        PP_FIELD(contrast, "contrast", T::Float, 0.0f, 2.0f, "contrast around mid grey (1 none)"),
        PP_FIELD(saturation, "saturation", T::Float, 0.0f, 2.0f, "saturation (1 none, 0 grey)"),
        PP_FIELD(lift, "lift", T::Vector3, -1.0f, 1.0f, "lift [r,g,b]: added to the shadows (0 none)"),
        PP_FIELD(gamma, "gamma", T::Vector3, 0.1f, 4.0f, "gamma [r,g,b]: midtones (1 none, above brightens)"),
        PP_FIELD(gain, "gain", T::Vector3, 0.0f, 4.0f, "gain [r,g,b]: scales the colour (1 none)"),
        PP_FIELD(lutTexture, "lut_texture", T::Asset, 0.0f, 0.0f, "project image of a LUT strip (N*N x N), display colour in and out (\"\" none)"),
        PP_FIELD(lutContribution, "lut_contribution", T::Float, 0.0f, 1.0f, "how much of the LUT strip applies (0..1)"),
        PP_FIELD(lutSize, "lut_size", T::Int, 16.0f, 32.0f, "side of the engine's colour LUT: 16 or 32"),
        PP_FIELD(monochromeEnabled, "monochrome_enabled", T::Bool, 0.0f, 1.0f, "grayscale or sepia"),
        PP_ENUM(monochromeMode, "monochrome_mode", monochromeNames, "MonochromeMode", monochromeValues, "monochrome look: grayscale, sepia"),
        PP_FIELD(monochromeIntensity, "monochrome_intensity", T::Float, 0.0f, 1.0f, "monochrome mix (0..1)"),
        PP_FIELD(vignetteEnabled, "vignette_enabled", T::Bool, 0.0f, 1.0f, "vignette"),
        PP_FIELD(vignetteIntensity, "vignette_intensity", T::Float, 0.0f, 1.0f, "vignette amount at the corners (0..1)"),
        PP_FIELD(vignetteSmoothness, "vignette_smoothness", T::Float, 0.01f, 1.0f, "vignette falloff width (0..1)"),
        PP_FIELD(vignetteRoundness, "vignette_roundness", T::Float, 0.0f, 1.0f, "1 a circle, 0 the screen's shape"),
        PP_FIELD(vignetteColor, "vignette_color", T::Vector3, 0.0f, 1.0f, "vignette colour [r,g,b] (sRGB)"),
        PP_FIELD(ditherEnabled, "dither_enabled", T::Bool, 0.0f, 1.0f, "dither against 8-bit banding"),
        PP_FIELD(fxaaEnabled, "fxaa_enabled", T::Bool, 0.0f, 1.0f, "FXAA anti-aliasing (uber pass)"),
        PP_FIELD(chromaticAberrationEnabled, "chromatic_aberration_enabled", T::Bool, 0.0f, 1.0f, "chromatic aberration (uber pass)"),
        PP_FIELD(chromaticAberrationIntensity, "chromatic_aberration_intensity", T::Float, 0.0f, 1.0f, "red/blue shift at the screen edge (0..1)"),
        PP_FIELD(pixelateEnabled, "pixelate_enabled", T::Bool, 0.0f, 1.0f, "pixelate (uber pass)"),
        PP_FIELD(pixelateSize, "pixelate_size", T::Float, 1.0f, 64.0f, "pixel block size in output pixels"),
        PP_FIELD(crtEnabled, "crt_enabled", T::Bool, 0.0f, 1.0f, "CRT look (uber pass)"),
        PP_FIELD(crtScanlines, "crt_scanlines", T::Float, 0.0f, 1.0f, "scanline darkening (0..1)"),
        PP_FIELD(crtMask, "crt_mask", T::Float, 0.0f, 1.0f, "aperture-grille mask (0..1)"),
        PP_FIELD(crtCurvature, "crt_curvature", T::Float, 0.0f, 1.0f, "screen curvature (0..1)"),
        PP_FIELD(crtVignette, "crt_vignette", T::Float, 0.0f, 1.0f, "tube edge darkening (0..1)"),
        PP_FIELD(edgeDetectEnabled, "edge_detect_enabled", T::Bool, 0.0f, 1.0f, "Sobel edge detect on luma (uber pass)"),
        PP_FIELD(edgeDetectThreshold, "edge_detect_threshold", T::Float, 0.0f, 1.0f, "luma gradient where edges start (0..1)"),
        PP_FIELD(edgeDetectIntensity, "edge_detect_intensity", T::Float, 0.0f, 1.0f, "edge colour mix (0..1)"),
        PP_FIELD(edgeDetectColor, "edge_detect_color", T::Vector3, 0.0f, 1.0f, "edge colour [r,g,b] (sRGB)"),
        PP_FIELD(filmGrainEnabled, "film_grain_enabled", T::Bool, 0.0f, 1.0f, "film grain (uber pass)"),
        PP_FIELD(filmGrainIntensity, "film_grain_intensity", T::Float, 0.0f, 1.0f, "grain amount (0..1)"),
        PP_FIELD(filmGrainResponse, "film_grain_response", T::Float, 0.0f, 1.0f, "less grain in bright areas (0..1)"),
        PP_FIELD(sharpenEnabled, "sharpen_enabled", T::Bool, 0.0f, 1.0f, "sharpen (uber pass)"),
        PP_FIELD(sharpenIntensity, "sharpen_intensity", T::Float, 0.0f, 1.0f, "sharpen amount (0..1)"),
        PP_FIELD(bloomEnabled, "bloom_enabled", T::Bool, 0.0f, 1.0f, "bloom (quarter-resolution chain + uber pass)"),
        PP_FIELD(bloomThreshold, "bloom_threshold", T::Float, 0.0f, 1.0f, "display brightness where bloom starts (0..1)"),
        PP_FIELD(bloomKnee, "bloom_knee", T::Float, 0.0f, 1.0f, "soft knee width of the threshold (0..1)"),
        PP_FIELD(bloomIntensity, "bloom_intensity", T::Float, 0.0f, 10.0f, "bloom strength"),
        PP_FIELD(bloomTint, "bloom_tint", T::Vector3, 0.0f, 1.0f, "bloom colour [r,g,b] (sRGB)"),
        PP_FIELD(bloomScatter, "bloom_scatter", T::Float, 0.0f, 1.0f, "bloom radius (0..1)"),
        PP_FIELD(blurEnabled, "blur_enabled", T::Bool, 0.0f, 1.0f, "blurred copy of the scene (quarter-resolution chain)"),
        PP_FIELD(blurRadius, "blur_radius", T::Float, 0.0f, 4.0f, "blur radius"),
        PP_FIELD(blurIntensity, "blur_intensity", T::Float, 0.0f, 1.0f, "how much the screen shows the blur (0 only the texture)"),
        PP_FIELD(renderScale, "render_scale", T::Float, 0.5f, 1.0f, "3D scene resolution, 0.5..1 of the output (the main camera's UI too; a layer scene's HUD stays native)"),
        PP_FIELD(msaaSamples, "msaa_samples", T::Int, 1.0f, 4.0f, "multisampling of the scene target: 1, 2 or 4"),
    };
    return fields;
}

#undef PP_FIELD
#undef PP_ENUM

const PostProcessingSettings::Field* PostProcessingSettings::findField(const std::string& name){
    for (const Field& field : getFields()){
        if (name == field.name || name == field.snakeName)
            return &field;
    }
    return nullptr;
}

bool PostProcessingSettings::operator==(const PostProcessingSettings& o) const{
    for (const Field& field : getFields()){
        const void* a = getFieldRef(field);
        const void* b = o.getFieldRef(field);
        switch (field.type){
            case FieldType::Bool: if (*(const bool*)a != *(const bool*)b) return false; break;
            case FieldType::Int:
            case FieldType::Enum: if (*(const int*)a != *(const int*)b) return false; break;
            case FieldType::Float: if (*(const float*)a != *(const float*)b) return false; break;
            case FieldType::Vector3: if (*(const Vector3*)a != *(const Vector3*)b) return false; break;
            case FieldType::String: if (*(const std::string*)a != *(const std::string*)b) return false; break;
            case FieldType::Asset: if (*(const AssetRef*)a != *(const AssetRef*)b) return false; break;
        }
    }
    return true;
}

void PostProcessingSettings::clamp(){
    for (const Field& field : getFields()){
        void* ref = field.ref(*this);
        if (field.type == FieldType::Float){
            float& v = *(float*)ref;
            v = std::isfinite(v) ? std::min(std::max(v, field.minValue), field.maxValue) : *(const float*)PostProcessingSettings().getFieldRef(field);
        }else if (field.type == FieldType::Vector3){
            Vector3& v = *(Vector3*)ref;
            const Vector3& d = *(const Vector3*)PostProcessingSettings().getFieldRef(field);
            for (int i = 0; i < 3; i++)
                v[i] = std::isfinite(v[i]) ? std::min(std::max(v[i], field.minValue), field.maxValue) : d[i];
        }
    }
    lutSize = lutSize > 16 ? 32 : 16;
    msaaSamples = msaaSamples >= 4 ? 4 : (msaaSamples >= 2 ? 2 : 1);
    if ((int)tonemap < 0 || (int)tonemap > (int)TonemapMode::AGX) tonemap = TonemapMode::NONE;
    if ((int)monochromeMode < 0 || (int)monochromeMode > (int)MonochromeMode::SEPIA) monochromeMode = MonochromeMode::GRAYSCALE;
}

bool PostProcessPass::setTexture(const std::string& uniform, const std::string& path){
    return ShaderLibraryValues::setTexture(textures, uniform, path);
}

std::string PostProcessPass::getTexture(const std::string& uniform) const{
    return ShaderLibraryValues::getTexture(textures, uniform);
}
