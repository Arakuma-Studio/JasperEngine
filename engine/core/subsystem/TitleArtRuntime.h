// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TITLE_ART_RUNTIME_H
#define TITLE_ART_RUNTIME_H

// Contract (docs/specs/S81-title-art-runtime.md R9-R32, R51)
// - Purpose: the seam between the always-built part of Title Art (TitleArtSystem, the component,
//   the Lua class) and the `titleart` module (JASPERENGINE_TITLE_ART): Realtime and Glyph Atlas
//   rendering. The module implements it in engine/core/titleart/runtime/TitleArtRuntime.cpp;
//   without the module engine/core/subsystem/TitleArtRuntimeNone.cpp does (isBuilt() false, every
//   title shows its bake: R51), the UIDocumentSystemNone precedent.
// - One engine-wide scheduler (R13) serves every scene: TitleArtSystem hands each Realtime or
//   Glyph Atlas title's current state to its TitleArtLive once a frame (setState: cheap, it
//   compares a hash), RenderSystem runs the frame's steps before the camera passes (runFrame),
//   and the system lends the front display image to the companion (display()).
// - Thread-safety: main thread.

#include "Export.h"
#include "component/TitleArtRendererComponent.h"
#include "util/TitleArtBakeInfo.h"

#include <cstdint>
#include <memory>

namespace jasperengine::titleanim{
    class TitleAnimPlayer;
    struct TitleAnimLibrary;
}

#include <string>
#include <vector>

namespace jasperengine{

    class TextureRender;
    class Framebuffer;

    namespace titleart{

        // the module's state of one Realtime or Glyph Atlas title (scheduler entry, double
        // buffer, glyph atlas); opaque here
        class TitleArtLive;

        // one Exposed parameter of a document (S80 §6.1), as the runtime and Lua see it
        struct JASPERENGINE_API TitleArtExposedParam{
            std::string name;
            std::string label;
            std::string type;               // text color number bool font image color_scheme
            bool hasMin = false, hasMax = false;
            double min = 0.0, max = 0.0;
            std::vector<std::string> options;   // allowed values (color schemes)
            // the document's value (R5's last resort): what an override replaces
            TitleArtOverride defaultValue;
            bool hasDefault = false;
            // every target names a field of a 2D text layer's `text`: a Glyph Atlas text (R25)
            bool targetsText = false;
        };

        // what a title must show now (R9's state)
        struct JASPERENGINE_API TitleArtLiveState{
            std::vector<TitleArtOverride> overrides;    // the effective values (R5) that differ from the document
            float resolution = 1.0f;        // display scale x resolutionScale, rounded to 1/8
            // the canvas region the image covers (image space): the bake's frame, so a Realtime
            // image and the bake show the same rectangle (R10); empty = the whole canvas
            TitleArtRectI region;
            int priority = 0;
            bool visible = true;
            bool renderWhenHidden = false;
            bool prebake = true;
            TitleArtTransition transition = TitleArtTransition::CUT;
            float transitionDuration = 0.15f;
            int atlasPageSize = 1024;
            int atlasMaxPages = 4;
            std::string qualityTier = "auto";
            // the S86 tier a Realtime 3D title renders at (TitleArtRuntime::tierFor: 0 bake quality,
            // 1 high, 2 medium, 3 low); 0 for a document without 3D layers
            int tier = 0;
            std::string label;              // "TitleArt/<GameObject>" (VRAM owner, pass labels)
            uint32_t testFlags = 0;         // the instance's test state (setDisabledFeaturesForTesting)
            bool glowOutput = false;        // Glow Pulse is on: render the glow output too (R35)
            std::string characters;         // R55: the characters the title may show ("" = no limit)
            // S83's FxTier on this device, resolved by the module from S56's platform group and tier
            // (TitleArtRuntime::setState)
            int fxTier = 0;
        };

        // S86 R63: the mesh of a 3D layer the last render drew (Lua getMeshStats)
        struct JASPERENGINE_API TitleArtMeshStats{
            std::string layerId;
            uint32_t triangles = 0, vertices = 0, solids = 0;
            float buildMs = 0.0f;           // 0 for a cooked mesh (no build ran)
            std::string tier;               // "bake" "high" "medium" "low": the tier it was made for
            bool overBudget = false;        // over its tier's limits (S86 §6.7)
            bool cooked = false;            // the export's .titlemesh (no mesher ran)
        };

        // what a title's live state reports (R18, R19, R69)
        struct JASPERENGINE_API TitleArtLiveStatus{
            bool ready = false;             // the displayed image is the state's
            bool hasImage = false;          // something to show (an older state's image counts)
            std::string pending;            // "" queued hidden shaders "rendering n/m" failed
            bool failed = false;
            std::string error;              // one sentence: the cause and the fix
            std::string pixelPath;          // "float" | "rgba8"
            uint64_t stateHash = 0;
            uint64_t displayedHash = 0;
            int displayWidth = 0, displayHeight = 0;
            size_t displayBytes = 0;
            size_t poolBytes = 0;
            // the last finished render (R60 Status, R69 last_render)
            int lastFrames = 0;
            double lastGpuMsEstimate = 0.0;
            double lastCpuMs = 0.0;
            int overBudgetSteps = 0;
            uint64_t steps = 0;             // steps run since the instance was made (tests: 0 = no render)
            uint64_t readbacks = 0;         // anchor readbacks (R36)
            // the frame the display image is cut from (bake frame semantics) and its anchors
            TitleArtBakeInfo frame;
            bool anchorsChanged = false;
            // R55: characters of the state its fonts or the Characters set cannot show
            std::string missingCharacters;
            // R31: effects Glyph Atlas skips or approximates; R25/R30 fallback reason
            std::vector<std::string> glyphUnsafe, glyphApproximate;
            std::string glyphFallback;      // "" or the glyph_atlas_unsupported reason
            uint32_t atlasPages = 0;
            // R9: full renders and live composes swapped in since the instance was made, and why
            // the last change of a live parameter re-rendered ("" when it composed live)
            uint64_t renders = 0;
            uint64_t liveComposes = 0;
            std::string liveFallback;
            size_t liveBytes = 0;           // the inputs kept for the live compose
            // S86 R63: the meshes of the displayed image's Dimension 3D layers
            std::vector<TitleArtMeshStats> meshes;
        };

        class JASPERENGINE_API TitleArtRuntime{
        public:
            // the module is in this build (R51)
            static bool isBuilt();

            // A title's live state for the document at `titlePath` (project-relative) in `mode`;
            // null with `error` without the module or when the document cannot be read
            static std::shared_ptr<TitleArtLive> create(const std::string& titlePath, TitleArtRenderMode mode, std::string* error);
            // the state it must show; a new hash queues a request (R12)
            static void setState(TitleArtLive& live, const TitleArtLiveState& state);
            static TitleArtLiveStatus status(const TitleArtLive& live);
            // the front display image (null while there is none) and, while a crossfade runs,
            // the previous one and the blend 0..1 (R17)
            static TextureRender* display(TitleArtLive& live);
            static Framebuffer* displayFramebuffer(TitleArtLive& live);
            static Framebuffer* previousFramebuffer(TitleArtLive& live, float* blend);
            // the glow output of the displayed image (R35), null when none
            static Framebuffer* glowFramebuffer(TitleArtLive& live);
            // re-render now (Lua refresh(), render_title {rerender})
            static void invalidate(TitleArtLive& live);
            // R11: how many output pixels per canvas pixel a UI title renders at for the project's
            // Reference Height (height / canvas height; 1 without one or without the module)
            static float referenceScale();
            // S86 R37: the tier a title renders at for its component's Quality Tier ("auto", "high",
            // "medium", "low"): a named tier, else the device's S56 tier where one applies, else the
            // project's Realtime 3D Quality for this platform; High draws as Medium on GLES3/WebGL2.
            // 0 for a document without 3D layers and in builds without 3D titles
            static int tierFor(const TitleArtLive& live, const std::string& qualityTier);
            // S86 R63: the mesh stats of the 3D layer at `layerPath` (names joined by '/', "#N" for
            // the Nth same-named sibling after the first, or the layer's id) from the last render;
            // false with `error` when the path names no 3D layer, true with `found` false while no
            // render has drawn it yet
            static bool meshStats(const TitleArtLive& live, const std::string& layerPath, TitleArtMeshStats& out, bool& found,
                                  std::string* error);
            // a prebaked initial-state image shipped with the build (R11, R32), by state hash
            static void setPrebaked(TitleArtLive& live, const std::string& texturePath, uint64_t stateHash);

            // S85: the document's animations compiled (every track; the text layout gives glyph
            // tracks their units); null with `error` without the module or a readable document
            static std::shared_ptr<titleanim::TitleAnimLibrary> compileAnimations(const std::string& titlePath, std::string* error);
            // S85 R14, R15, R18: what plays of the player's animations beyond the companion: the
            // re-render values sampled at the animation's rerender_fps (a request per sample time,
            // under the title's share of the frame budget) and the glyph-cell poses; called each
            // frame before setState (nothing when nothing plays)
            static void setAnimation(TitleArtLive& live, const titleanim::TitleAnimPlayer& player, int animatingTitles);
            // R18: a `prerender` animation's sample times rendered ahead into the title's frame cache;
            // false when they do not fit the Cache Budget (it then plays live, warned once)
            static bool prepareAnimation(TitleArtLive& live, const titleanim::TitleAnimPlayer& player, const std::string& name);
            static bool isPrerendering(const TitleArtLive& live);

            // the document's Exposed parameters with their values (R5); false with `error`
            static bool readExposed(const std::string& titlePath, std::vector<TitleArtExposedParam>& out, std::string* error);
            // the hash of the state (document content + effective values + resolution + path)
            // the prebake and the scheduler share (R11)
            static uint64_t stateHashOf(TitleArtLive& live, const TitleArtLiveState& state);
            // the key of a state's prebaked image (R11): the state hash without the pixel path,
            // so a device of either path finds the image the export rendered once
            static uint64_t prebakeKeyOf(TitleArtLive& live, const TitleArtLiveState& state);

            // RenderSystem, before the camera passes: runs the frame's steps of every queued
            // render within the frame budget, then swaps finished images in (R13, R15, R17)
            static void runFrame(double dt, bool editorEditMode);
            // drops the GPU side of every title (device loss, the end of the engine)
            static void releaseAll();

            // tests and the editor's settings: the frame budget in ms (0.25..16, R15); <= 0
            // goes back to the platform's
            static void setFrameBudget(double ms);
            static double getFrameBudget(bool editorEditMode = false);
        };

    }

}

#endif //TITLE_ART_RUNTIME_H
