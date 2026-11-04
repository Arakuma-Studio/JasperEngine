// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PostProcessingPresets.h"

#include "Project.h"
#include "command/CommandGroup.h"
#include "command/CommandNames.h"
#include "command/type/ScenePropertyCmd.h"
#include "pixelart/BuiltinPalettes.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteUse.h"
#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace jasperengine::editor {

namespace {

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

std::vector<PostProcessingPreset> makePostProcessingPresets() {
    std::vector<PostProcessingPreset> presets;

    presets.push_back({"none", "None", "Everything off: what the engine draws without post-processing (no pass, no shader work).",
                       PostProcessingSettings()});

    // the cheapest set that still looks finished: the colour in the shaders, FXAA in the one
    // uber pass (which then takes the grade too) and a short, faint bloom
    {
        PostProcessingSettings s;
        s.tonemap = TonemapMode::NEUTRAL;
        s.colorGradingEnabled = true;
        s.contrast = 1.08f;
        s.saturation = 1.1f;
        s.lutSize = 16;
        s.fxaaEnabled = true;
        s.bloomEnabled = true;
        s.bloomThreshold = 0.9f;
        s.bloomKnee = 0.5f;
        s.bloomIntensity = 0.6f;
        s.bloomScatter = 0.5f;
        presets.push_back({"mobile", "Mobile",
                           "Phones and tablets: neutral tone mapping and a light grade (16^3 LUT), FXAA, faint bloom. "
                           "One uber pass plus the quarter-resolution bloom chain.", s});

        // the same, with the render scale following the frame cost
        s.dynamicResolutionEnabled = true;
        s.dynamicResolutionMinScale = 0.6f;
        presets.push_back({"mobile_adaptive", "Mobile Adaptive",
                           "Mobile, plus dynamic resolution: in the game the render scale drops to as low as 0.6 to hold "
                           "the frame rate, and climbs back when there is headroom.", s});
    }

    {
        PostProcessingSettings s;
        s.tonemap = TonemapMode::ACES;
        s.colorGradingEnabled = true;
        s.temperature = 0.05f;
        s.contrast = 1.1f;
        s.saturation = 1.05f;
        s.vignetteEnabled = true;
        s.vignetteIntensity = 0.3f;
        s.ditherEnabled = true;
        s.fxaaEnabled = true;
        s.bloomEnabled = true;
        s.chromaticAberrationEnabled = true;
        s.chromaticAberrationIntensity = 0.15f;
        s.filmGrainEnabled = true;
        s.filmGrainIntensity = 0.12f;
        presets.push_back({"console", "Console",
                           "Desktop and consoles: ACES, grade, vignette, dither, FXAA, bloom, subtle chromatic aberration "
                           "and film grain. One uber pass plus the bloom chain.", s});
    }

    // flat, chunky colour: few distinct levels read as posterized once the pixels are large
    {
        PostProcessingSettings s;
        s.colorGradingEnabled = true;
        s.contrast = 1.45f;
        s.saturation = 1.4f;
        // warm: red and a little green lift the shadows (amber), the gain warms the highlights
        s.lift = Vector3(0.04f, 0.02f, 0.0f);
        s.gain = Vector3(1.05f, 1.0f, 0.95f);
        s.vignetteEnabled = true;
        // gentle: clipped highlights stay bright towards the edges
        s.vignetteIntensity = 0.3f;
        s.vignetteSmoothness = 0.6f;
        s.pixelateEnabled = true;
        s.pixelateSize = 4.0f;
        presets.push_back({"stylised", "Stylised",
                           "Pixelated with a punchy, posterized-looking grade and a vignette. One uber pass.", s});
    }

    {
        PostProcessingSettings s;
        s.colorGradingEnabled = true;
        s.temperature = 0.1f;
        s.contrast = 1.1f;
        s.saturation = 1.15f;
        s.crtEnabled = true;
        s.crtScanlines = 0.55f;
        s.crtMask = 0.35f;
        s.crtCurvature = 0.3f;
        s.crtVignette = 0.4f;
        s.pixelateEnabled = true;
        s.pixelateSize = 3.0f;
        s.chromaticAberrationEnabled = true;
        s.chromaticAberrationIntensity = 0.25f;
        s.filmGrainEnabled = true;
        s.filmGrainIntensity = 0.15f;
        presets.push_back({"crt_retro", "CRT Retro",
                           "An old television: curved tube, scanlines, aperture mask, low-resolution pixels, colour fringes "
                           "and grain. One uber pass.", s});
    }

    // 35 mm film: a soft curve, a warm grade, film grain cells, the red glow of halation
    // around highlights, a little dust and gate weave
    {
        PostProcessingSettings s;
        s.tonemap = TonemapMode::NEUTRAL;
        s.colorGradingEnabled = true;
        s.temperature = 0.12f;
        s.contrast = 1.06f;
        s.saturation = 0.92f;
        s.lift = Vector3(0.02f, 0.015f, 0.0f);
        s.gain = Vector3(1.03f, 1.0f, 0.96f);
        s.vignetteEnabled = true;
        s.vignetteIntensity = 0.25f;
        s.vignetteSmoothness = 0.6f;
        s.filmGrainEnabled = true;
        s.filmGrainIntensity = 0.25f;
        s.filmGrainType = FilmGrainType::LENS;
        s.filmGrainSize = 1.5f;
        s.halationEnabled = true;
        s.halationIntensity = 0.5f;
        s.filmDamageEnabled = true;
        s.filmDamageIntensity = 0.8f;
        s.filmHairAmount = 0.1f;
        s.filmScratchAmount = 0.2f;
        s.filmDustAmount = 1.5f;
        s.filmGateWeave = 0.3f;
        presets.push_back({"film", "Film",
                           "35 mm film: neutral tone mapping, a warm grade, Lens film grain, halation, light damage (dust, "
                           "hairs, gate weave) and a vignette. One uber pass plus halation's two small passes.", s});
    }

    // a worn tape: the VHS look with its defaults, light grain and a slightly faded grade
    {
        PostProcessingSettings s;
        s.colorGradingEnabled = true;
        s.contrast = 0.95f;
        s.saturation = 0.85f;
        s.vhsEnabled = true;
        s.filmGrainEnabled = true;
        s.filmGrainIntensity = 0.1f;
        presets.push_back({"vhs", "VHS Tape",
                           "A played tape: colour bleed, wobble, tracking bands, tape noise, dropouts and scanlines, with "
                           "light grain and a faded grade. One uber pass.", s});
    }

    // an exact palette over the whole frame (S43 R22): the built-in is copied into the project
    // when the preset is applied (resolvePostProcessingPreset), so the scene stores the copy
    {
        PostProcessingSettings s;
        s.paletteEnabled = true;
        s.palette = std::string(pixelart::kBuiltinPrefix) + "game-boy";
        s.paletteDither = PaletteDither::BAYER4;
        presets.push_back({"handheld_4", "Handheld (4 Shades)",
                           "The four greens of the Game Boy (DMG) palette over the frame, Bayer 4x4 dithered per art pixel "
                           "(Palette Quantize). One uber pass.", s});

        s.palette = std::string(pixelart::kBuiltinPrefix) + "pico-8";
        s.paletteDither = PaletteDither::NONE;
        presets.push_back({"pico_8", "PICO-8",
                           "Every pixel onto the 16 colours of PICO-8, the nearest one without a dither (Palette Quantize). "
                           "One uber pass.", s});
    }

    return presets;
}

// one chain of the bloom/blur kind: a 4-tap prefilter from the scene to a quarter of its size,
// three dual-filter downsamples and three upsamples (phase 4 spec section 2.4)
// the chain as RenderSystem::renderBloomChain / renderBlurChain draw it: a prefilter to 1/4,
// then `levels` dual-filter downsamples (1/8, 1/16, 1/32) and as many upsamples back
void addChain(PostProcessingCostSide& side, const std::string& name, float sceneArea, int levels = 3) {
    side.passes.push_back({name + " prefilter", 1, sceneArea / 16.0f, false});
    float down = 0.0f;
    float up = 0.0f;
    for (int level = 1; level <= levels; level++) {
        down += sceneArea / (float)(16 << (2 * level));
        up += sceneArea / (float)(16 << (2 * (level - 1)));
    }
    if (levels > 0) {
        side.passes.push_back({name + " down", levels, down, false});
        side.passes.push_back({name + " up", levels, up, false});
    }
}

void total(PostProcessingCostSide& side) {
    side.passCount = 0;
    side.fullResolutionPasses = 0;
    side.fullscreen = 0.0f;
    for (const PostProcessingCostPass& pass : side.passes) {
        side.passCount += pass.count;
        if (pass.fullResolution) side.fullResolutionPasses += pass.count;
        side.fullscreen += pass.area;
    }
}

} // namespace

const std::vector<PostProcessingPreset>& getPostProcessingPresets() {
    static const std::vector<PostProcessingPreset> presets = makePostProcessingPresets();
    return presets;
}

const PostProcessingPreset* findPostProcessingPreset(const std::string& idOrName) {
    const std::string wanted = lowerCase(idOrName);
    for (const PostProcessingPreset& preset : getPostProcessingPresets()) {
        if (preset.id == wanted || lowerCase(preset.name) == wanted) return &preset;
    }
    // "stylized", "crt", "crt retro"
    if (wanted == "stylized") return findPostProcessingPreset("stylised");
    if (wanted == "crt" || wanted == "crt-retro") return findPostProcessingPreset("crt_retro");
    if (wanted == "vhs tape" || wanted == "vhs_tape") return findPostProcessingPreset("vhs");
    if (wanted == "mobile-adaptive" || wanted == "adaptive") return findPostProcessingPreset("mobile_adaptive");
    return nullptr;
}

const PostProcessingPreset* matchPostProcessingPreset(const PostProcessingSettings& settings, const Project* project) {
    for (const PostProcessingPreset& preset : getPostProcessingPresets()) {
        if (preset.settings == settings) return &preset;
        const std::string& named = preset.settings.palette.path;
        if (!project || settings.palette.empty() || named.rfind(pixelart::kBuiltinPrefix, 0) != 0) continue;
        // everything else first: the palette's file is only read for a preset that otherwise matches
        PostProcessingSettings asPreset = settings;
        asPreset.palette = preset.settings.palette;
        if (!(asPreset == preset.settings)) continue;
        // the project's copy of the built-in names its origin (R7: source {kind: builtin, id})
        std::shared_ptr<const jasperengine::pixelart::Palette> palette = jasperengine::pixelart::PaletteLibrary::get(settings.palette.path);
        if (palette && palette->source.kind == "builtin" && palette->source.id == named.substr(std::char_traits<char>::length(pixelart::kBuiltinPrefix)))
            return &preset;
    }
    return nullptr;
}

bool resolvePostProcessingPreset(Project& project, const PostProcessingPreset& preset, PostProcessingSettings& settings,
                                 bool& wroteCopy, std::string& error) {
    settings = preset.settings;
    wroteCopy = false;
    if (settings.palette.path.rfind(pixelart::kBuiltinPrefix, 0) != 0) return true;
    pixelart::ResolvedPalette resolved;
    if (!pixelart::usePalette(project, settings.palette.path, resolved, wroteCopy, error)) return false;
    settings.palette = resolved.assetPath;
    return true;
}

PostProcessingCostTier getPostProcessingCostTier(const PostProcessingSettings& onlyThisEffect) {
    const PostProcessingTiers tiers = RenderSystem::getPostProcessingTiers(onlyThisEffect);
    if (tiers.uberFeatures & (UBER_BLOOM | UBER_BLUR | UBER_HALATION)) return PostProcessingCostTier::CHAIN;
    if (tiers.uber) return PostProcessingCostTier::UBER;
    if (tiers.tailColor) return PostProcessingCostTier::IN_SHADER;
    return PostProcessingCostTier::NONE;
}

const char* getPostProcessingCostTierLabel(PostProcessingCostTier tier) {
    switch (tier) {
        case PostProcessingCostTier::IN_SHADER:
            return "T0, 0 passes: computed at the end of the scene's shaders (a LUT tap per shaded pixel). "
                   "With the uber pass on it moves there, still without a pass of its own.";
        case PostProcessingCostTier::UBER:
            return "T1, the uber pass: one fullscreen pass shared by every T1 effect (with fixed resolution it is "
                   "the upscale blit drawn anyway).";
        case PostProcessingCostTier::CHAIN:
            return "T2, a chain from a quarter of the resolution plus the uber pass that composites it: bloom is 7 small "
                   "passes (about 0.17 of a screen); blur 1 + 2 x ceil(radius) (3 at radius 1, 5 to radius 2, 7 above); "
                   "halation reads bloom's chain (no pass) or, without bloom, runs 2 small passes (about 0.08 of a screen).";
        case PostProcessingCostTier::NONE:
            break;
    }
    return "No cost: draws nothing at this value.";
}

std::string describeUberFeatures(uint32_t uberFeatures) {
    std::string text;
    const int count = ShaderPool::getShaderPropertyCount(ShaderType::BLIT);
    for (int bit = 0; bit < count && bit < 32; bit++) {
        if (uberFeatures & (1u << bit)) {
            text += (text.empty() ? "" : ", ") + ShaderPool::getShaderPropertyName(ShaderType::BLIT, bit, false);
        }
    }
    return text;
}

bool isCubeLutPath(const std::string& path) {
    return path.size() > 5 && lowerCase(path.substr(path.size() - 5)) == ".cube";
}

PostProcessingRowState getPostProcessingRowState(const PostProcessingSettings& settings, const std::string& snakeName,
                                                 bool fixedResolution) {
    PostProcessingRowState state;
    const bool vhs = settings.vhsEnabled && settings.vhsIntensity > 0.0f;
    if (snakeName == "lut_input_encoding") {
        state.shown = isCubeLutPath(settings.lutTexture);
    } else if (snakeName == "film_grain_size") {
        state.shown = settings.filmGrainType != FilmGrainType::CLASSIC;
    } else if (snakeName == "film_grain_colored") {
        state.shown = settings.filmGrainType == FilmGrainType::FILMIC;
    } else if (snakeName == "fxaa_enabled" || snakeName == "sharpen_enabled" || snakeName == "sharpen_intensity") {
        if (vhs) {
            state.inactiveReason = "Inactive while VHS is on: the tape's colour bleed already filters the image horizontally.";
            state.inactiveTag = "inactive while VHS is on";
        }
    } else if (snakeName == "halation_threshold") {
        if (settings.bloomEnabled && settings.bloomIntensity > 0.0f) {
            state.inactiveReason = "Bloom is on: halation reuses the bloom chain's levels, so Bloom's Threshold applies.";
            state.inactiveTag = "Bloom's threshold applies";
        }
    } else if (snakeName == "dynamic_resolution_enabled" || snakeName == "dynamic_resolution_min_scale" ||
               snakeName == "dynamic_resolution_target_fps") {
        if (fixedResolution) {
            state.inactiveReason = "Inactive while Fixed Resolution is on: the scene renders at its fixed size.";
            state.inactiveTag = "inactive with Fixed Resolution";
        }
    } else if (snakeName == "palette_dither_strength") {
        state.shown = settings.paletteDither != PaletteDither::NONE;
    } else if (snakeName == "dither_enabled") {
        if (settings.paletteEnabled && !settings.palette.empty()) {
            state.inactiveReason = "Inactive while Palette Quantize is on: its own dither between palette colours replaces the "
                                   "anti-banding noise.";
            state.inactiveTag = "inactive with Palette Quantize";
        }
    }
    // Palette Quantize does nothing without a palette (RenderSystem's paletteMissing)
    if (snakeName == "palette_enabled" && settings.paletteEnabled && settings.palette.empty()) {
        state.inactiveReason = "Palette Quantize is on without a palette: it stays off until one is set.";
        state.inactiveTag = "no palette: off";
    }
    return state;
}

const char* getLutInputEncodingName(LutInputEncoding encoding) {
    switch (encoding) {
        case LutInputEncoding::AUTO: return "Auto";
        case LutInputEncoding::SRGB: return "sRGB";
        case LutInputEncoding::LINEAR: return "Linear";
        case LutInputEncoding::LOGC3: return "LogC3";
        case LutInputEncoding::ACESCCT: return "ACEScct";
        case LutInputEncoding::CINEON: return "Cineon";
        case LutInputEncoding::DAVINCI_INTERMEDIATE: return "DaVinci Intermediate";
    }
    return "Auto";
}

std::string describeLutInputEncoding(LutInputEncoding setting, const CubeLut* lut, const std::string& error) {
    if (!lut) return error.empty() ? std::string("No LUT loaded") : "Not loaded: " + error;
    std::string reason;
    const LutInputEncoding resolved = resolveEncoding(setting, *lut, &reason);
    const char* stage = stageOf(resolved, *lut) == LutStage::SCENE ? "scene stage, before the tone mapping" : "display stage";
    if (setting == LutInputEncoding::AUTO)
        return std::string("Auto: ") + getLutInputEncodingName(resolved) + " (" + reason + "; " + stage + ")";
    return std::string(getLutInputEncodingName(resolved)) + " (" + stage + ")";
}

Command* makePostProcessingCommand(Project* project, uint32_t sceneId, const PostProcessingSettings& settings, const std::string& label) {
    Command* command = new ScenePropertyCmd<PostProcessingSettings>(project, sceneId, "post_processing", settings);
    if (label.empty()) return command;
    std::vector<std::unique_ptr<Command>> commands;
    commands.emplace_back(command);
    return new CommandGroup(std::move(commands), label + " (" + CommandNames::scene(project, sceneId) + ")", false);
}

std::string describeColorLut(const CubeLut* lut, const std::string& error) {
    if (!lut) return "Color LUT: " + (error.empty() ? std::string("not loaded") : "does not load: " + error);
    std::string text = lut->fromStrip ? "Color LUT (image strip)" : "Color LUT (.cube)";
    if (!lut->title.empty()) text += "\nTitle: " + lut->title;
    char line[160];
    if (lut->size1D > 0) {
        std::snprintf(line, sizeof(line), "\n1D table: %d entries, input %g to %g", lut->size1D, lut->in1D[0], lut->in1D[1]);
        text += line;
    }
    if (lut->size3D > 0) {
        std::snprintf(line, sizeof(line), "\n3D table: %d^3, input %g to %g", lut->size3D, lut->in3D[0], lut->in3D[1]);
        text += line;
    }
    text += "\nInput encoding: " + describeLutInputEncoding(LutInputEncoding::AUTO, lut, error);
    return text;
}

std::string describeDynamicResolution(const DynamicResolutionStatus& status, std::vector<std::string>& warnings) {
    if (!status.enabled && !status.overridden) return std::string();
    char text[160];
    if (status.overridden) {
        std::snprintf(text, sizeof(text), "Dynamic resolution %.2f (pinned)", status.scale);
        return text;
    }
    if (!status.running) return std::string("Dynamic resolution idle: ") + status.idle;
    const std::string source = status.source;
    if (source == "gpu") {
        std::snprintf(text, sizeof(text), "Dynamic resolution %.2f (GPU %.1f / %.1f ms)", status.scale, status.smoothedMs, status.targetMs);
    } else if (source == "frame") {
        if (status.probeSeconds > 0.0f)
            std::snprintf(text, sizeof(text), "Dynamic resolution %.2f (frame law: probing in %.0f s)", status.scale, std::ceil(status.probeSeconds));
        else
            std::snprintf(text, sizeof(text), "Dynamic resolution %.2f (frame law: %.1f / %.1f ms)", status.scale, status.smoothedMs, status.targetMs);
    } else {
        std::snprintf(text, sizeof(text), "Dynamic resolution %.2f (measuring)", status.scale);
    }
    if (status.saturatedLow)
        warnings.push_back("Dynamic resolution is at Min Scale and still over the target: the cost the scale does not move "
                           "(CPU, geometry, post chain) misses the frame alone.");
    if (status.ineffective)
        warnings.push_back("Dynamic resolution judged the scale ineffective for this content (it saves too little): it holds "
                           "Render Scale until the cost changes.");
    return text;
}

PostProcessingCost estimatePostProcessingCost(const PostProcessingSettings& settings, const std::vector<PostProcessPass>& passes,
                                              bool fixedResolution) {
    PostProcessingCost cost;
    const PostProcessingTiers tiers = RenderSystem::getPostProcessingTiers(settings);
    const float scale = tiers.uber ? tiers.renderScale : 1.0f;
    const float area = scale * scale;
    const bool scaled = scale < 0.999f;
    const bool bloom = (tiers.uberFeatures & UBER_BLOOM) != 0;
    const bool blur = (tiers.uberFeatures & UBER_BLUR) != 0;
    const bool halation = (tiers.uberFeatures & UBER_HALATION) != 0;
    cost.inShader = tiers.tailColor;
    cost.uber = tiers.uber;
    cost.chain = bloom || blur || halation;
    cost.sceneShading = area;

    // the enabled user passes and their resolution scales (PostProcessPass::getAppliedScale)
    std::vector<float> passScales;
    for (const PostProcessPass& pass : passes)
        if (pass.enabled) passScales.push_back(pass.getAppliedScale());
    const int userPasses = (int)passScales.size();

    // ---- this engine (an exported game drawing to the screen): the user chain at the scene's
    // size (each pass at its scale), the chains, then the uber pass (RenderSystem::renderPostProcess,
    // renderCameraPass)
    PostProcessingCostSide& engine = cost.engine;
    for (int i = 0; i < userPasses; i++) {
        const float passScale = passScales[i];
        const bool full = passScale >= 0.999f;
        std::string label = "user pass " + std::to_string(i + 1);
        if (!full) {
            char text[32];
            std::snprintf(text, sizeof(text), " (scale %.2f)", passScale);
            label += text;
        }
        engine.passes.push_back({label, 1, area * passScale * passScale, full && !scaled && !fixedResolution});
    }
    if (userPasses > 0 && !tiers.uber) {
        // the uber pass reads the chain's last output as it is; without it the chain ends in a
        // copy to the fixed-resolution target (only after a last pass below full size), or in
        // the blit to the screen (always: the chain never draws to the swapchain itself)
        const bool lastScaled = passScales.back() < 0.999f;
        if (fixedResolution) {
            if (lastScaled) engine.passes.push_back({"user chain copy", 1, area, false});
        } else {
            engine.passes.push_back({"user chain blit to the screen", 1, 1.0f, true});
        }
    }
    // MSAA resolves inside the scene pass into the offscreen target the scene draws to; with
    // nothing offscreen (no uber pass, user pass or fixed resolution) that target is made for
    // it and blitted to the screen
    const bool msaaBlit = tiers.msaaSamples > 1 && !tiers.uber && !fixedResolution && userPasses == 0;
    if (msaaBlit) engine.passes.push_back({"MSAA resolve blit to the screen", 1, 1.0f, true});
    if (bloom) addChain(engine, "bloom", area);
    if (blur) addChain(engine, "blur", area, RenderSystem::getBlurLevels(settings.blurRadius));
    // halation without bloom: its threshold prefilter to 1/4 and one level down to 1/8 (with
    // bloom it reads bloom's levels)
    if (tiers.halationChain) {
        engine.passes.push_back({"halation prefilter", 1, area / 16.0f, false});
        engine.passes.push_back({"halation down", 1, area / 64.0f, false});
    }
    if (tiers.uber) engine.passes.push_back({"uber", 1, 1.0f, true});
    total(engine);

    // ---- Unity URP 14 for the same effects (bloom Half, 6 iterations, not HQ; FXAA as the
    // camera's anti-aliasing): the LUT is baked every frame, the uber pass reads the camera's
    // intermediate texture, FXAA and upscaling run a final pass at the output's resolution
    PostProcessingCostSide& urp = cost.urp;
    const bool colour = settings.exposure != 0.0f || settings.tonemap != TonemapMode::NONE || settings.colorGradingEnabled ||
                        settings.monochromeEnabled || settings.vignetteEnabled || settings.ditherEnabled;
    const bool lens = (tiers.uberFeatures & (UBER_CHROMATIC | UBER_GRAIN)) != 0;
    const bool fxaa = (tiers.uberFeatures & UBER_FXAA) != 0;
    const bool post = colour || lens || bloom || fxaa;
    if (post) urp.passes.push_back({"color grading LUT 1024x32", 1, (1024.0f * 32.0f) / (1920.0f * 1080.0f), false});
    if (bloom) urp.passes.push_back({"bloom (half resolution, 6 mips, separable)", 16, 0.75f * area, false});
    // effects URP has no built-in for: one Full Screen Pass Renderer Feature each
    auto custom = [&](uint32_t bit, const char* name) {
        if (tiers.uberFeatures & bit) {
            urp.passes.push_back({std::string(name) + " (renderer feature)", 1, area, !scaled});
            cost.urpCustom.push_back(name);
        }
    };
    custom(UBER_PIXELATE, "pixelate");
    custom(UBER_CRT, "CRT");
    custom(UBER_EDGE, "edge detect");
    custom(UBER_SHARPEN, "sharpen");
    custom(UBER_BLUR, "blur");
    custom(UBER_VHS, "VHS");
    custom(UBER_FILM_DAMAGE, "film damage");
    custom(UBER_HALATION, "halation");
    if (userPasses > 0) urp.passes.push_back({"user pass (renderer feature)", userPasses, area * userPasses, !scaled});
    if (post) urp.passes.push_back({"uber", 1, area, !scaled});
    if (fxaa && scaled) urp.passes.push_back({"FXAA scaling setup", 1, area, false});
    if (fxaa || scaled) urp.passes.push_back({fxaa ? "final post (FXAA)" : "final blit (upscale)", 1, 1.0f, true});
    total(urp);

    // ---- remarks
    if (!cost.inShader && !cost.uber && userPasses == 0) {
        cost.notes.push_back("Nothing is on: no pass and no shader work.");
    }
    if (cost.inShader) {
        cost.notes.push_back(tiers.tailGrade
            ? "In the shaders (T0): exposure, curve, grade, vignette and dither cost a LUT tap per shaded pixel, no pass."
            : "In the shaders (T0): exposure and the curve cost a LUT tap per shaded pixel; the rest is in the uber pass.");
    }
    if (tiers.uber && fixedResolution) {
        cost.notes.push_back("Fixed resolution: the uber pass replaces the upscale blit drawn anyway, so it adds no pass.");
    }
    if (scaled) {
        char text[160];
        std::snprintf(text, sizeof(text), "Render scale %.2f: the 3D scene shades %.0f%% of the pixels; the uber pass upscales it.",
                      scale, area * 100.0f);
        cost.notes.push_back(text);
    }
    if (userPasses > 0) {
        cost.notes.push_back(std::string("Each user pass is one more pass at the scene's resolution (times its scale squared): on "
                                         "mobile, prefer the built-in effects.") +
                             (tiers.uber ? "" : fixedResolution ? " A last pass below full size is copied to the fixed-resolution target."
                                                                : " Without the uber pass the chain ends with a blit to the screen "
                                                                  "(the editor's Game view draws its last full-size pass straight to the view)."));
    }
    if (tiers.msaaSamples > 1) {
        cost.notes.push_back(msaaBlit
            ? "MSAA: the samples resolve in tile memory at the end of the scene pass; with nothing else offscreen the exported "
              "game resolves into a target blitted to the screen (one full-resolution pass, counted). Desktop GPUs pay the "
              "multisampled target's memory and bandwidth."
            : "MSAA: the samples resolve in tile memory at the end of the scene pass, into the target the effects read (no "
              "pass of its own). Desktop GPUs pay the multisampled target's memory and bandwidth.");
    }
    if (settings.colorGradingEnabled && settings.lutContribution > 0.0f && isCubeLutPath(settings.lutTexture)) {
        cost.notes.push_back("The .cube LUT is baked into the colour LUT on the CPU (URP's Color Lookup): no pass and no GPU cost.");
    }
    if (tiers.dynamicResolution) {
        const float minScale = std::min(std::max(settings.dynamicResolutionMinScale, 0.5f), tiers.renderScale);
        char text[200];
        std::snprintf(text, sizeof(text), "Dynamic resolution: scene shading varies between min^2 and max^2 of native (%.0f%% to %.0f%%) "
                      "with the frame cost, in the game only.", minScale * minScale * 100.0f, tiers.renderScale * tiers.renderScale * 100.0f);
        cost.notes.push_back(text);
    }
    if (!cost.urpCustom.empty()) {
        std::string names;
        for (const std::string& name : cost.urpCustom) names += (names.empty() ? "" : ", ") + name;
        cost.notes.push_back("URP has no built-in " + names + ": counted as one Full Screen Pass Renderer Feature each.");
    }
    return cost;
}

}
