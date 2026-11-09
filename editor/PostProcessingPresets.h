// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "SceneSettings.h"
#include "util/CubeLut.h"
#include "subsystem/RenderSystem.h"

#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // One-click post-processing setups (shader library phase 4, section 2.1): a whole
    // PostProcessingSettings each, applied as one scene property command ("post_processing")
    // so a preset is one undo step. The inspector's Post-processing section and the MCP tools
    // list_post_processing_presets / apply_post_processing_preset use them.
    struct PostProcessingPreset {
        std::string id, name, description;
        PostProcessingSettings settings;
    };

    // the presets, in menu order ("none" first)
    const std::vector<PostProcessingPreset>& getPostProcessingPresets();
    // the preset with this id (or name, any case), or null
    const PostProcessingPreset* findPostProcessingPreset(const std::string& idOrName);
    // the preset equal to these settings, or null (custom settings). With a project, a preset's
    // built-in palette ("builtin:<id>") matches the project's copy of it
    const PostProcessingPreset* matchPostProcessingPreset(const PostProcessingSettings& settings, const Project* project = nullptr);
    // The settings a preset applies in a project (S43 R7, R22): a built-in palette it names
    // becomes the project's copy (its assets-relative path), written first when there is none
    // (one step of the project history: wroteCopy; link it with the scene's step). False with
    // `error` when the copy cannot be written.
    bool resolvePostProcessingPreset(Project& project, const PostProcessingPreset& preset, PostProcessingSettings& settings,
                                     bool& wroteCopy, std::string& error);

    // The cost tier of an effect (RenderSystem::getPostProcessingTiers of a set with only it on)
    enum class PostProcessingCostTier {
        NONE,       // neutral: draws nothing
        IN_SHADER,  // T0: in the shaders' colour tail, no pass
        UBER,       // T1: the uber pass (one fullscreen pass shared by every T1 effect)
        CHAIN       // T2: the quarter-resolution bloom/blur chain plus the uber pass
    };
    PostProcessingCostTier getPostProcessingCostTier(const PostProcessingSettings& onlyThisEffect);
    // "T0, 0 passes: ...", ... for tooltips
    const char* getPostProcessingCostTierLabel(PostProcessingCostTier tier);

    // One render pass of a cost estimate
    struct PostProcessingCostPass {
        std::string label;
        int count = 1;              // passes of this kind
        float area = 1.0f;          // the target's area, fraction of the output (all of them)
        bool fullResolution = false;
    };

    // An estimate of what the post-processing of the main camera costs after the scene is drawn,
    // and of what Unity URP 14 costs for the same effects. From the research's cost model
    // (fast-postprocessing.md section 11: passes counted from source, never measured on a device):
    // "fullscreen" = pixels shaded by the post passes in units of the output's pixel count.
    struct PostProcessingCostSide {
        std::vector<PostProcessingCostPass> passes;
        int passCount = 0;
        int fullResolutionPasses = 0;
        float fullscreen = 0.0f;
    };
    struct PostProcessingCost {
        PostProcessingCostSide engine;
        PostProcessingCostSide urp;
        // the tiers the engine runs: in-shader colour (T0), the uber pass (T1), the chain (T2)
        bool inShader = false;
        bool uber = false;
        bool chain = false;
        // the 3D scene's shading at the render scale (renderScale^2 of the native cost)
        float sceneShading = 1.0f;
        // effects URP has no built-in for (counted as one full-screen renderer feature pass each)
        std::vector<std::string> urpCustom;
        // remarks for the readout (fixed resolution, render scale, user passes, MSAA...)
        std::vector<std::string> notes;
    };
    // passes: the scene's user post-process chain (SceneSettings::postProcess; the enabled ones
    // count, each at its scale); fixedResolution: the scene renders at a fixed resolution (the
    // uber pass replaces its blit). The engine side counts what an exported game draws to the
    // screen (the chain's final blit, MSAA's resolve blit when nothing else is offscreen).
    PostProcessingCost estimatePostProcessingCost(const PostProcessingSettings& settings, const std::vector<PostProcessPass>& passes,
                                                  bool fixedResolution);

    // the uber pass features as words ("FXAA, grade LUT, vignette"), from the variant bit names
    std::string describeUberFeatures(uint32_t uberFeatures);

    // The inspector's rule for a post_processing field row (S13): whether it shows (the LUT
    // Input Encoding with a .cube LUT only; the grain's Size, Colored and Frame Rate with the
    // types that use them; Palette Quantize's Dither Strength with a dither), and why it does
    // nothing now ("" when it does): FXAA and Sharpen while VHS is on, Halation's Threshold while
    // bloom is on, Dynamic Resolution while the scene renders at a fixed resolution, Palette
    // Quantize without a palette, the anti-banding Dither while Palette Quantize is on
    struct PostProcessingRowState {
        bool shown = true;
        std::string inactiveReason;
        std::string inactiveTag;    // the reason in a few words ("inactive while VHS is on")
    };
    PostProcessingRowState getPostProcessingRowState(const PostProcessingSettings& settings, const std::string& snakeName,
                                                     bool fixedResolution);

    // whether a LUT path is a .cube file (the other LUTs are image strips)
    bool isCubeLutPath(const std::string& path);

    // what a .cube LUT's input encoding resolves to, for the inspector's LUT Input Encoding row:
    // "Auto: ACEScct (the title names ACEScct)", "sRGB", or the parse error
    std::string describeLutInputEncoding(LutInputEncoding setting, const CubeLut* lut, const std::string& error);
    // the encoding's display name ("ACEScct", "DaVinci Intermediate")
    const char* getLutInputEncodingName(LutInputEncoding encoding);

    class Command;

    // One undo step that replaces a scene's whole post_processing, named by the label in the
    // undo history ("Apply Film Preset (Main)"); the plain property command without a label
    Command* makePostProcessingCommand(Project* project, uint32_t sceneId, const PostProcessingSettings& settings,
                                       const std::string& label);

    // A colour LUT file in words, for the Project window's tooltip: "Color LUT", its title, table
    // sizes, input range, the encoding Auto proposes and its stage, or why it does not load
    std::string describeColorLut(const CubeLut* lut, const std::string& error);

    // The "Applied now" line of dynamic resolution while playing (R16): "Dynamic resolution 0.75
    // (GPU 14.2 / 16.7 ms)", "(frame law: probing in 4 s)"; empty when it is off. warnings gets
    // the saturation and ineffective-scale warnings
    std::string describeDynamicResolution(const DynamicResolutionStatus& status, std::vector<std::string>& warnings);

}
