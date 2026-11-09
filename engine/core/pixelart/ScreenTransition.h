// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_SCREENTRANSITION_H
#define PIXELART_SCREENTRANSITION_H

// Screen transitions (docs/specs/S48-pixel-fx.md R12-R17, D7, D8): one engine overlay that covers
// the screen in an ink colour and reveals it again, with hard art-pixel edges and rims coloured
// from a ramp. It belongs to the engine, not to a scene: it survives scene changes, covers the
// layer scenes (HUD) and the loading scene, and costs nothing while idle (no pass, no draw, no
// upload; the program is made on first use and kept). The shader is shaderlib/transition.frag
// (ShaderType::TRANSITION); evaluate() is its CPU mirror for the edge lint and the tests.

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector2.h"
#include "math/Vector4.h"
#include "pixelart/Rgba8.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine {
    class Framebuffer;
    class Scene;
}

namespace jasperengine::pixelart {

    enum class TransitionKind { IRIS, BLINDS, SLASH, PIXEL_DISSOLVE, CRT_OFF, BURN_AWAY, FADE };
    enum class IrisShape { CIRCLE, DIAMOND, SQUARE };
    enum class TransitionEase { LINEAR, IN, OUT, IN_OUT };
    enum class TransitionPhase { NONE, COVER, HOLD, REVEAL };

    constexpr int kTransitionKindCount = 7;
    constexpr int kTransitionMaxRampSteps = 16;
    constexpr int kTransitionMaxRim = 8;

    // "iris", "blinds", "slash", "pixel_dissolve", "crt_off", "burn_away", "fade"
    JASPERENGINE_API const char* transitionKindName(TransitionKind kind);
    JASPERENGINE_API bool transitionKindFromName(const std::string& name, TransitionKind& kind);
    JASPERENGINE_API const char* transitionEaseName(TransitionEase ease);
    JASPERENGINE_API bool transitionEaseFromName(const std::string& name, TransitionEase& ease);
    JASPERENGINE_API const char* irisShapeName(IrisShape shape);
    JASPERENGINE_API bool irisShapeFromName(const std::string& name, IrisShape& shape);
    // 0..1 eased (in: cubic in, out: cubic out, in_out: cubic in-out)
    JASPERENGINE_API float applyTransitionEase(TransitionEase ease, float t);

    // The project's defaults (Project Settings > Graphics > Screen Transitions, R41): what a
    // transition takes for what it is not given
    struct JASPERENGINE_API TransitionDefaults {
        TransitionKind kind = TransitionKind::IRIS;
        float duration = 0.6f;                              // seconds
        Vector4 color{11.0f / 255.0f, 10.0f / 255.0f, 16.0f / 255.0f, 1.0f}; // the ink, sRGB #0b0a10
        std::string ramp;                                   // "<palette>.palette#ramp:<name>" ("": the built-in ember ramp)
        int rim = 2;                                        // art pixels 0..8
        int pixelSize = 0;                                  // 0: the Pixel Perfect Camera's grid, else screen pixels per art pixel
        bool coverUI = true;                                // over every scene; false: after the main scene, under the layer scenes

        bool operator==(const TransitionDefaults& other) const;
        bool operator!=(const TransitionDefaults& other) const { return !(*this == other); }
    };

    struct JASPERENGINE_API TransitionOptions {
        Vector4 ink{11.0f / 255.0f, 10.0f / 255.0f, 16.0f / 255.0f, 1.0f};   // sRGB, as picked
        // the rim's ramp: "<palette>.palette#ramp:<name>", or a palette path in `palette` and
        // the ramp's name here; "" both: the built-in ember ramp. rampColors wins when set.
        std::string palette;
        std::string ramp;
        std::vector<Rgba8> rampColors;
        int rim = 2;                    // art pixels 0..8
        float glow = 0.4f;              // 0..1: light just outside the edge, in ramp steps
        int pixelSize = 0;              // 0: the S41 grid's scale (else 1); else screen pixels per art pixel
        uint32_t seed = 1;
        TransitionEase ease = TransitionEase::IN_OUT;
        float hold = -1.0f;             // play(): seconds covered (< 0: 10 % of the duration)
        // iris
        IrisShape shape = IrisShape::CIRCLE;
        Vector2 center{0.5f, 0.5f};     // normalized over the screen from its top-left
        Scene* centerScene = nullptr;   // a GameObject the iris closes on (its screen position)
        Entity centerObject = NULL_ENTITY;
        // slash
        float angle = 30.0f;            // degrees
        // blinds
        int count = 8;
        bool vertical = false;
        // pixel dissolve
        int cell = 16;                  // art pixels per cell
        // fade
        bool throughRamp = false;

        // the project's defaults (ink, ramp, rim, pixel size)
        static TransitionOptions fromDefaults();
    };

    struct JASPERENGINE_API TransitionHandle {
        uint32_t id = 0;
        bool isValid() const { return id != 0; }
        // the run ended (or another transition replaced it)
        bool isDone() const;
        // 0..1 of the run (1 once done)
        float getProgress() const;
        // ends the transition if it is still this run's (the screen uncovered at once)
        void stop() const;
    };

    // The art grid and the screen a transition is drawn on
    struct JASPERENGINE_API TransitionGrid {
        float artWidth = 0.0f, artHeight = 0.0f;   // art pixels
        float scale = 1.0f;                         // screen pixels per art pixel
        float originX = 0.0f, originY = 0.0f;       // the grid's top-left in screen pixels from the image's top-left
        float targetWidth = 0.0f, targetHeight = 0.0f;
        bool flipY = false;                         // the target's rows run bottom-up (the GL swapchain)
    };

    // the shader's parameter block (transition.frag u_fs_transition), std140 vec4s
    struct transition_fs_t {
        Vector4 art;
        Vector4 frame;
        Vector4 state;
        Vector4 params0;
        Vector4 params1;
        Vector4 rim;
        Vector4 ink;
        Vector4 ramp[kTransitionMaxRampSteps];
    };

    // A transition evaluated on the CPU, one entry per art pixel (rows from the top)
    struct JASPERENGINE_API TransitionImage {
        enum Cell : uint8_t { UNCOVERED = 0, INK = 1, RIM = 2, LIGHT = 3 };
        int width = 0, height = 0;
        std::vector<uint8_t> cells;         // Cell
        std::vector<uint8_t> rimDepth;      // RIM: 1..rim, the steps from the edge
        std::vector<Rgba8> colors;          // premultiplied output (a 0: light only, or nothing)
    };

    // The rim's width along the edge (the transition_edges lint): every edge crossing's rim run
    // measured along the field's dominant axis
    struct JASPERENGINE_API TransitionRimMeasure {
        int crossings = 0;      // edge pixels measured (0: no edge in this frame)
        int minWidth = 0, maxWidth = 0;
        int atX = -1, atY = -1; // where the widest or narrowest run differs from the asked rim
    };

    class JASPERENGINE_API ScreenTransition {
    public:
        // cover (45 %), hold (10 %, or opts.hold), reveal (45 %); onCovered at the hold
        static TransitionHandle play(TransitionKind kind, float seconds, const TransitionOptions& options = TransitionOptions::fromDefaults(),
                                     std::function<void()> onCovered = nullptr);
        // one phase: covered at the end (and held until reveal() or stop())
        static TransitionHandle cover(TransitionKind kind, float seconds, const TransitionOptions& options = TransitionOptions::fromDefaults(),
                                      std::function<void()> onCovered = nullptr);
        // one phase, from the current coverage (from covered when none runs)
        static TransitionHandle reveal(TransitionKind kind, float seconds, const TransitionOptions& options = TransitionOptions::fromDefaults());
        // covers, loads the scene stack (SceneManager, held until covered), holds while it loads,
        // reveals once it has loaded and drawn a frame; onto a shown loading scene it reveals and
        // covers again at half the duration each. False when SceneManager has no such scene.
        static TransitionHandle loadScene(const std::string& scene, TransitionKind kind, float seconds,
                                          const TransitionOptions& options = TransitionOptions::fromDefaults(), std::function<void()> onCovered = nullptr);

        static bool isActive();
        static bool isCovered();
        static float getCoverage();
        static TransitionPhase getPhase();
        static TransitionKind getKind();
        static void stop();

        // Engine::runFrame, after the scenes' update (unscaled seconds: hit-stop and the time
        // scale do not slow it). The editor drives it while no game runs (its preview).
        static void update(double unscaledDt);
        // something to draw this frame (coverage above 0, or holding)
        static bool needsDraw();
        // draws over the target (null: the swapchain) when needsDraw(); the Engine calls it
        static void draw(Framebuffer* target);
        // the Engine draws it (default); the editor turns it off while a frame is not the game's
        static void setEngineDrawEnabled(bool enabled);
        static bool isEngineDrawEnabled();

        // the editor's preview and capture_contact_sheet: a fixed coverage (and phase) drawn as if a
        // transition of that kind ran; a negative coverage ends it
        static void setPreview(TransitionKind kind, const TransitionOptions& options, float coverage, TransitionPhase phase = TransitionPhase::COVER);
        static void setPreviewProgress(float coverage);
        // Offscreen: the kind at that coverage over a default Framebuffer (RGBA8 + depth), its
        // whole size at pixel_size screen pixels per art pixel (0: 1), loading what it keeps.
        // False while the program is still being built (call again on a later frame).
        static bool renderInto(Framebuffer* target, TransitionKind kind, const TransitionOptions& options, float coverage,
                               TransitionPhase phase = TransitionPhase::COVER);

        // the CPU mirror of transition.frag on an art grid of width x height
        static void evaluate(TransitionKind kind, const TransitionOptions& options, float coverage, TransitionPhase phase,
                             int width, int height, TransitionImage& out);
        static TransitionRimMeasure measureRim(TransitionKind kind, const TransitionImage& image);
        // the parameter block for a grid (what draw() uploads)
        static transition_fs_t makeUniforms(TransitionKind kind, const TransitionOptions& options, float coverage, TransitionPhase phase,
                                            const TransitionGrid& grid);
        // the ramp's colours (display sRGB) a transition with these options uses
        static std::vector<Rgba8> resolveRamp(const TransitionOptions& options);
        // the built-in ember ramp (pixelfx's 'ember'), darkest first
        static const std::vector<Rgba8>& emberRamp();

        static void setDefaults(const TransitionDefaults& defaults);
        static const TransitionDefaults& getDefaults();
        static bool isCoverUI();

        // Play stop / shutdown: ends the transition and drops its callbacks (they may hold Lua)
        static void reset();
        // the view's GPU objects are gone (the program is made again on the next use)
        static void releaseGpu();
        // true once the program exists (R17: never before the first transition)
        static bool isProgramCreated();
    };

}

#endif // PIXELART_SCREENTRANSITION_H
