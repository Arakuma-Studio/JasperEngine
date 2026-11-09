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
    const char* const paletteDitherNames[] = {"none", "bayer2", "bayer4", "bayer8", nullptr};
    const char* const paletteDitherValues[] = {"NONE", "BAYER2", "BAYER4", "BAYER8", nullptr};
    const char* const paletteDistanceNames[] = {"oklab", "rgb", nullptr};
    const char* const paletteDistanceValues[] = {"OKLAB", "RGB", nullptr};
    const char* const palettePrecisionNames[] = {"normal", "high", nullptr};
    const char* const palettePrecisionValues[] = {"NORMAL", "HIGH", nullptr};
    const char* const filmGrainTypeNames[] = {"classic", "lens", "filmic", nullptr};
    const char* const filmGrainTypeValues[] = {"CLASSIC", "LENS", "FILMIC", nullptr};
    const char* const filmGateMaskNames[] = {"none", "rounded", "academy_137", "widescreen_185", "anamorphic_239", nullptr};
    const char* const filmGateMaskValues[] = {"NONE", "ROUNDED", "ACADEMY_137", "WIDESCREEN_185", "ANAMORPHIC_239", nullptr};
    const char* const lutEncodingNames[] = {"auto", "srgb", "linear", "logc3", "acescct", "cineon", "davinci_intermediate", nullptr};
    const char* const lutEncodingValues[] = {"AUTO", "SRGB", "LINEAR", "LOGC3", "ACESCCT", "CINEON", "DAVINCI_INTERMEDIATE", nullptr};

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
        PP_FIELD(lutTexture, "lut_texture", T::Asset, 0.0f, 0.0f, "project LUT: an image strip (N*N x N, display colour) or a .cube file (\"\" none)"),
        PP_FIELD(lutContribution, "lut_contribution", T::Float, 0.0f, 1.0f, "how much of the LUT applies (0..1)"),
        PP_ENUM(lutInputEncoding, "lut_input_encoding", lutEncodingNames, "LutInputEncoding", lutEncodingValues,
                "a .cube LUT's input: auto (from its title and range), srgb, linear (display), logc3, acescct, cineon, davinci_intermediate (scene, before the tone mapping)"),
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
        PP_ENUM(filmGrainType, "film_grain_type", filmGrainTypeNames, "FilmGrainType", filmGrainTypeValues,
                "grain pattern: classic (per pixel), lens (FidelityFX Lens cells), filmic (5x5 particle lattice, desktop / high-end)"),
        PP_FIELD(filmGrainSize, "film_grain_size", T::Float, 0.25f, 8.0f, "grain size in pixels (lens, filmic)"),
        PP_FIELD(filmGrainColored, "film_grain_colored", T::Bool, 0.0f, 1.0f, "one grain per colour channel (filmic)"),
        PP_FIELD(filmFrameRate, "film_frame_rate", T::Float, 1.0f, 120.0f, "film frames per second: how often the lens / filmic grain and the film damage change"),
        PP_FIELD(vhsEnabled, "vhs_enabled", T::Bool, 0.0f, 1.0f, "VHS tape look (uber pass; FXAA and sharpen do not run with it)"),
        PP_FIELD(vhsIntensity, "vhs_intensity", T::Float, 0.0f, 1.0f, "how much of the VHS look applies (0..1)"),
        PP_FIELD(vhsWobble, "vhs_wobble", T::Float, 0.0f, 2.0f, "line wobble of the tape's wow and flutter (0..2)"),
        PP_FIELD(vhsTracking, "vhs_tracking", T::Float, 0.0f, 2.0f, "tracking error bands rolling down the picture (0..2)"),
        PP_FIELD(vhsColorBleed, "vhs_color_bleed", T::Float, 0.0f, 2.0f, "horizontal colour bleed of the tape's narrow chroma (0..2)"),
        PP_FIELD(vhsTapeNoise, "vhs_tape_noise", T::Float, 0.0f, 2.0f, "tape noise (0..2)"),
        PP_FIELD(vhsChromaStreaks, "vhs_chroma_streaks", T::Float, 0.0f, 2.0f, "coloured streaks along the lines (0..2)"),
        PP_FIELD(vhsDropouts, "vhs_dropouts", T::Float, 0.0f, 2.0f, "white dropout specks of worn tape (0..2)"),
        PP_FIELD(vhsScanlines, "vhs_scanlines", T::Float, 0.0f, 2.0f, "scanline darkening (0..2)"),
        PP_FIELD(vhsInterference, "vhs_interference", T::Float, 0.0f, 2.0f, "rolling interference bars (0..2)"),
        PP_FIELD(vhsGlitches, "vhs_glitches", T::Float, 0.0f, 2.0f, "signal glitches: line tears and colour bursts (0..2)"),
        PP_FIELD(vhsSpeed, "vhs_speed", T::Float, 0.0f, 4.0f, "animation speed of the tape (0..4)"),
        PP_FIELD(filmDamageEnabled, "film_damage_enabled", T::Bool, 0.0f, 1.0f, "film damage: hairs, scratches, dust, gate weave and mask (uber pass)"),
        PP_FIELD(filmDamageIntensity, "film_damage_intensity", T::Float, 0.0f, 4.0f, "strength of the hairs, scratches and dust (0..4)"),
        PP_FIELD(filmHairAmount, "film_hair_amount", T::Float, 0.0f, 8.0f, "hair candidates per film frame (0..8, 0 none)"),
        PP_FIELD(filmScratchAmount, "film_scratch_amount", T::Float, 0.0f, 8.0f, "scratch candidates per film frame (0..8, 0 none)"),
        PP_FIELD(filmDustAmount, "film_dust_amount", T::Float, 0.0f, 8.0f, "dust candidates per film frame (0..8, 0 none)"),
        PP_FIELD(filmDustSize, "film_dust_size", T::Float, 0.5f, 24.0f, "dust size in pixels"),
        PP_FIELD(filmGateWeave, "film_gate_weave", T::Float, 0.0f, 1.0f, "the gate's weave, 0..1 of a projector's (0.75 / 0.5 px, 0.08 degrees)"),
        PP_ENUM(filmGateMask, "film_gate_mask", filmGateMaskNames, "FilmGateMask", filmGateMaskValues,
                "film gate frame: none, rounded, academy_137, widescreen_185, anamorphic_239"),
        PP_FIELD(filmGateMaskFeather, "film_gate_mask_feather", T::Float, 0.0f, 256.0f, "feather of the gate mask's edge in pixels"),
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
        PP_FIELD(halationEnabled, "halation_enabled", T::Bool, 0.0f, 1.0f, "halation: film's red glow around highlights (bloom's chain, or 2 small passes without bloom)"),
        PP_FIELD(halationIntensity, "halation_intensity", T::Float, 0.0f, 2.0f, "halation strength (0..2)"),
        PP_FIELD(halationThreshold, "halation_threshold", T::Float, 0.0f, 1.0f, "display brightness where halation starts (0..1; bloom's threshold while bloom is on)"),
        PP_FIELD(halationRadius, "halation_radius", T::Float, 0.0f, 1.0f, "halation radius: mix of the 1/4 and 1/8 levels (0..1)"),
        PP_FIELD(halationTint, "halation_tint", T::Vector3, 0.0f, 1.0f, "halation colour [r,g,b] (sRGB)"),
        PP_FIELD(renderScale, "render_scale", T::Float, 0.5f, 1.0f, "3D scene resolution, 0.5..1 of the output (the main camera's UI too; a layer scene's HUD stays native)"),
        PP_FIELD(msaaSamples, "msaa_samples", T::Int, 1.0f, 4.0f, "multisampling of the scene target: 1, 2 or 4"),
        PP_FIELD(dynamicResolutionEnabled, "dynamic_resolution_enabled", T::Bool, 0.0f, 1.0f,
                 "dynamic resolution: in Play and exported games the render scale moves between the min scale and render_scale to hold the target frame rate"),
        PP_FIELD(dynamicResolutionMinScale, "dynamic_resolution_min_scale", T::Float, 0.5f, 1.0f, "lowest render scale dynamic resolution goes to (0.5..1)"),
        PP_FIELD(dynamicResolutionTargetFps, "dynamic_resolution_target_fps", T::Int, 0.0f, 240.0f, "frame rate dynamic resolution holds (0: the display's refresh rate, else 60)"),
        // Palette Quantize (S43)
        PP_FIELD(paletteEnabled, "palette_enabled", T::Bool, 0.0f, 1.0f, "Palette Quantize: every pixel of the frame onto the palette (after the grade, before CRT and grain)"),
        PP_FIELD(palette, "palette", T::Asset, 0.0f, 0.0f, "the .palette Palette Quantize uses (\"\" none: the effect is off)"),
        PP_ENUM(paletteDither, "palette_dither", paletteDitherNames, "PaletteDither", paletteDitherValues,
                "ordered dither between a colour's two nearest palette colours, per art pixel: none, bayer2, bayer4, bayer8"),
        PP_FIELD(paletteDitherStrength, "palette_dither_strength", T::Float, 0.0f, 1.0f, "how far the dither mixes (0 nearest colour only .. 1)"),
        PP_ENUM(paletteDistance, "palette_distance", paletteDistanceNames, "PaletteDistance", paletteDistanceValues,
                "how the nearest palette colour is chosen: oklab (perceptual), rgb"),
        PP_ENUM(palettePrecision, "palette_precision", palettePrecisionNames, "PalettePrecision", palettePrecisionValues,
                "the colour table's grid: normal (32^3), high (64^3: slower to bake, for many close shades)"),
        // Glitch (S48)
        PP_FIELD(glitchEnabled, "glitch_enabled", T::Bool, 0.0f, 1.0f, "Glitch: digital corruption in bursts (torn rows, RGB split, interlace) in source texels; at intensity 0 only PixelFX.glitch bursts show"),
        PP_FIELD(glitchIntensity, "glitch_intensity", T::Float, 0.0f, 1.0f, "how hard the scheduled bursts corrupt the frame (0..1)"),
        PP_FIELD(glitchRgbSplit, "glitch_rgb_split", T::Float, 0.0f, 8.0f, "red and blue shifted apart in a burst, source texels (0..8)"),
        PP_FIELD(glitchTears, "glitch_tears", T::Int, 0.0f, 8.0f, "rows torn sideways per burst frame (0..8)"),
        PP_FIELD(glitchTearShift, "glitch_tear_shift", T::Float, 0.0f, 32.0f, "how far a torn row moves, source texels (0..32)"),
        PP_FIELD(glitchInterlace, "glitch_interlace", T::Float, 0.0f, 1.0f, "chance per burst frame that every other row goes dark (0..1)"),
        PP_FIELD(glitchBurstRate, "glitch_burst_rate", T::Float, 0.0f, 8.0f, "bursts per second (0..8; 0: corrupted all the time at the intensity)"),
        PP_FIELD(glitchBurstLength, "glitch_burst_length", T::Float, 0.05f, 1.0f, "seconds a burst lasts (0.05..1)"),
        PP_FIELD(glitchFps, "glitch_fps", T::Float, 1.0f, 60.0f, "the corruption's frame rate (1..60): it changes on these steps only"),
        PP_FIELD(glitchSeed, "glitch_seed", T::Int, 0.0f, 65535.0f, "the burst schedule's seed (0..65535)"),
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
    dynamicResolutionTargetFps = std::min(std::max(dynamicResolutionTargetFps, 0), 240);
    glitchTears = std::min(std::max(glitchTears, 0), 8);
    glitchSeed = std::min(std::max(glitchSeed, 0), 65535);
    if ((int)tonemap < 0 || (int)tonemap > (int)TonemapMode::AGX) tonemap = TonemapMode::NONE;
    if ((int)monochromeMode < 0 || (int)monochromeMode > (int)MonochromeMode::SEPIA) monochromeMode = MonochromeMode::GRAYSCALE;
    if ((int)filmGrainType < 0 || (int)filmGrainType > (int)FilmGrainType::FILMIC) filmGrainType = FilmGrainType::CLASSIC;
    if ((int)filmGateMask < 0 || (int)filmGateMask > (int)FilmGateMask::ANAMORPHIC_239) filmGateMask = FilmGateMask::NONE;
    if ((int)lutInputEncoding < 0 || (int)lutInputEncoding > (int)LutInputEncoding::DAVINCI_INTERMEDIATE) lutInputEncoding = LutInputEncoding::AUTO;
    if ((int)paletteDither < 0 || (int)paletteDither > (int)PaletteDither::BAYER8) paletteDither = PaletteDither::BAYER4;
    if ((int)paletteDistance < 0 || (int)paletteDistance > (int)PaletteDistance::RGB) paletteDistance = PaletteDistance::OKLAB;
    if ((int)palettePrecision < 0 || (int)palettePrecision > (int)PalettePrecision::HIGH) palettePrecision = PalettePrecision::NORMAL;
}

bool PostProcessPass::setTexture(const std::string& uniform, const std::string& path){
    return ShaderLibraryValues::setTexture(textures, uniform, path);
}

std::string PostProcessPass::getTexture(const std::string& uniform) const{
    return ShaderLibraryValues::getTexture(textures, uniform);
}
