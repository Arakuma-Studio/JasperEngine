// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PostProcessingPresets.h"

#include "pool/ShaderPool.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <cctype>

namespace jasperengine::editor {

namespace {

std::string lowerCase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

std::vector<PostProcessingPreset> makePresets() {
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
        s.lift = Vector3(0.02f, 0.0f, 0.04f);
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
    static const std::vector<PostProcessingPreset> presets = makePresets();
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
    return nullptr;
}

const PostProcessingPreset* matchPostProcessingPreset(const PostProcessingSettings& settings) {
    for (const PostProcessingPreset& preset : getPostProcessingPresets()) {
        if (preset.settings == settings) return &preset;
    }
    return nullptr;
}

PostProcessingCostTier getPostProcessingCostTier(const PostProcessingSettings& onlyThisEffect) {
    const PostProcessingTiers tiers = RenderSystem::getPostProcessingTiers(onlyThisEffect);
    if (tiers.uberFeatures & (UBER_BLOOM | UBER_BLUR)) return PostProcessingCostTier::CHAIN;
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
            return "T2, the bloom chain: 7 small passes from a quarter of the resolution (about 0.17 of a screen) "
                   "plus the uber pass that composites it.";
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

PostProcessingCost estimatePostProcessingCost(const PostProcessingSettings& settings, int userPasses, bool fixedResolution) {
    PostProcessingCost cost;
    const PostProcessingTiers tiers = RenderSystem::getPostProcessingTiers(settings);
    const float scale = tiers.uber ? tiers.renderScale : 1.0f;
    const float area = scale * scale;
    const bool scaled = scale < 0.999f;
    const bool bloom = (tiers.uberFeatures & UBER_BLOOM) != 0;
    const bool blur = (tiers.uberFeatures & UBER_BLUR) != 0;
    cost.inShader = tiers.tailColor;
    cost.uber = tiers.uber;
    cost.chain = bloom || blur;
    cost.sceneShading = area;
    userPasses = std::max(userPasses, 0);

    // ---- this engine: the user chain and the chains at the scene's size, then the uber pass
    PostProcessingCostSide& engine = cost.engine;
    if (userPasses > 0) engine.passes.push_back({"user pass", userPasses, area * userPasses, !scaled && !fixedResolution});
    if (bloom) addChain(engine, "bloom", area);
    if (blur) addChain(engine, "blur", area, RenderSystem::getBlurLevels(settings.blurRadius));
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
        cost.notes.push_back("Each user pass is one more pass at the scene's resolution: on mobile, prefer the built-in effects.");
    }
    if (!cost.urpCustom.empty()) {
        std::string names;
        for (const std::string& name : cost.urpCustom) names += (names.empty() ? "" : ", ") + name;
        cost.notes.push_back("URP has no built-in " + names + ": counted as one Full Screen Pass Renderer Feature each.");
    }
    return cost;
}

}
