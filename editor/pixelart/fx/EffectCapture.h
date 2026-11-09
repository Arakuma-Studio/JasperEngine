// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Captures a target at fixed times (docs/specs/S48-pixel-fx.md R27, D12) for contact sheets and
// the effect lints:
// - isolated: a copy of the target alone in a scratch scene of the editor (never saved, not
//   undoable), stepped from t = 0 at a fixed 1/60 s with its random seeds pinned and the shader
//   time pinned at each frame, drawn by an orthographic camera at one framebuffer pixel per art
//   pixel (1 / pixels per unit) over the target's bounds and twice the margin, on transparent black;
// - scene: the target's whole scene through its camera. Stopped, a copy of the scene stepped as
//   above; playing, the paused game stepped at a fixed 1/60 s (Project::step) and its scenes drawn.
// The lints' extra captures (spill's seeds 2, 3 and 7, two fresh captures of 0.37 x duration for
// determinism, the frame at t = duration, the frame cost with the target shown and hidden) are
// more passes of the same machinery. Main thread: the editor drives every run from its frame
// (processFrame), the way ParticleEffectPreview runs.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/Rgba8.h"
#include "pixelart/fx/ContactSheet.h"
#include "pixelart/lint/EffectLints.h"

#include "ecs/Entity.h"
#include "pixelart/PixelFxPlayer.h"
#include "pixelart/ScreenTransition.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Framebuffer;
    class Scene;
}

namespace jasperengine::editor {
    class Project;
    struct SceneProject;
}

namespace jasperengine::editor::pixelart::fx {

    enum class CaptureMode { Isolated, Scene };

    // What a capture draws. The extension point for more kinds of target: a subclass builds its
    // copy into a scratch scene and moves its own clocks at each time (a GameObject with a PixelFX
    // effect seeks the effect player there; a transition draws its overlay pass itself).
    class CaptureTarget {
    public:
        virtual ~CaptureTarget() = default;

        // "Player (GameObject)", "assets/fx/fire.particles": for texts
        virtual std::string label() const = 0;
        // used when the call gives no duration / loop
        virtual double defaultDuration() const { return 1.0; }
        virtual bool defaultLoop() const { return false; }
        // the scene a scene-mode capture shows; null: none (scene mode is refused)
        virtual SceneProject* sourceScene() const { return nullptr; }

        // Isolated mode: puts a fresh copy into `scene` (an empty scratch scene). `runSeed` pins its
        // randomness (1 for the sheet, 2, 3 and 7 for spill). `made` gets the copies' roots, which
        // frame_cost hides. False + error when it cannot.
        virtual bool build(Scene* scene, uint32_t runSeed, std::vector<Entity>& made, std::string& error) = 0;
        // After the scene was stepped to `time`: the target's own clocks (default: none)
        virtual void seek(Scene* scene, const std::vector<Entity>& made, double time) {
            (void)scene;
            (void)made;
            (void)time;
        }
        // The lint inputs only the target knows (a transition: transition and rimColors); the
        // palette of the copies' materials is filled before this
        virtual void describeLints(lint::EffectLintInput& input) const { (void)input; }
        // A target drawn over an empty scene of its own (a transition): its frame's size in
        // framebuffer pixels, the whole frame its bounds; false: the frame follows the copies'
        // bounds
        virtual bool fixedFrame(int& width, int& height) const {
            (void)width;
            (void)height;
            return false;
        }
        // After the scene's draw of the frame at `time`, before the read-back: draws over the
        // capture's target (a transition's overlay pass). False, with what it waits for, to try
        // again on a later frame.
        virtual bool drawOver(Framebuffer& target, double time, std::string& waitingFor) {
            (void)target;
            (void)time;
            (void)waitingFor;
            return true;
        }
    };

    // A PixelFX effect a GameObject target plays from t = 0 (its time moved by the capture)
    struct CaptureEffect {
        bool active = false;
        jasperengine::pixelart::PixelFxEffect effect = jasperengine::pixelart::PixelFxEffect::FLASH;
        float duration = 1.0f;
        jasperengine::pixelart::PixelFxOptions options;
    };

    // How a transition target runs over the capture's duration: cover (coverage 0 to 1), reveal
    // (1 to 0) or play (cover 45 %, hold 10 % or options.hold, reveal 45 %, as Transition.play)
    enum class CaptureTransitionPhase { Play, Cover, Reveal };

    // A GameObject and its children (their animations, particles and materials' time); with
    // `clip` (a project-relative .anim) the copy's root plays that clip on an Animation Player
    std::unique_ptr<CaptureTarget> makeEntityTarget(Project* project, SceneProject* scene, Entity entity, const std::string& clip,
                                                    std::string& error, const CaptureEffect& effect = CaptureEffect());
    // A screen transition of `kind` on an art grid of artWidth x artHeight at options.pixelSize
    // (0: 1) screen pixels per art pixel, over transparent black; `duration` its run's length
    std::unique_ptr<CaptureTarget> makeTransitionTarget(jasperengine::pixelart::TransitionKind kind,
                                                        const jasperengine::pixelart::TransitionOptions& options, CaptureTransitionPhase phase,
                                                        double duration, int artWidth, int artHeight);
    // A .particles asset (assets-relative) on a Particle System of its own at the origin
    std::unique_ptr<CaptureTarget> makeParticlesTarget(Project* project, const std::string& effect, std::string& error);

    // A capture's size limits (R27). Only a transition's art grid times its pixel size is the
    // caller's to pick (an isolated frame is cropped to maxSide, a scene frame fits sceneMaxWidth),
    // and 1024 art pixels x pixel size 16 x 64 frames would be 64 GiB of frames.
    constexpr int kCaptureMaxSide = 4096;
    constexpr int64_t kCaptureMaxPixels = int64_t(1) << 25;   // 128 MiB of RGBA8, over all a sheet's frames

    // Whether a capture of `frames` frames of `target` fits the limits; false, with what to change, when not
    bool captureFitsLimits(const CaptureTarget& target, int frames, std::string& error);

    struct CaptureSettings {
        CaptureMode mode = CaptureMode::Isolated;
        std::vector<double> times;          // in order, each in [0, duration]
        double duration = 1.0;
        bool loop = false;
        int margin = 24;                    // art pixels around the target's bounds (pixelfx's M)
        bool lints = false;                 // make the lints' extra captures
        std::vector<std::string> lintIds;   // empty: every lint
        int pixelsPerUnit = 100;
        int maxSide = 512;                  // art pixels of an isolated frame, each way
        int sceneMaxWidth = 480;            // a scene-mode frame: the canvas divided down to this
        uint32_t sceneId = 0;               // scene mode while playing: the played scene
        Project* project = nullptr;
        // scene mode while playing: lets an auto_paused game step for as long as the result lives
        // (the MCP's play auto_pause hold, w3::GameAdvance)
        std::function<std::shared_ptr<void>()> acquireAdvance;
    };

    struct CaptureOutput {
        std::vector<PixelImage> frames;     // premultiplied on transparent black (isolated) or opaque (scene)
        std::vector<double> times;
        int width = 0, height = 0;
        bool opaque = false;                // scene mode: the frames are the whole view
        IntRect bounds;                     // the target at t = 0, in frame pixels (rows top first)
        int groundY = -1;                   // the target's feet: the "ground" backdrop's first row
        jasperengine::pixelart::Rgba8 sceneColor{21, 19, 29, 255};
        // the lints' inputs (when settings.lints)
        lint::EffectLintInput lintInput;
        std::vector<std::vector<PixelImage>> spillRuns;
        PixelImage last;                    // t = duration of a one-shot
        PixelImage determinism[2];
    };

    // A finished capture's sheet (the output's ground row, scene colour and opaque frames applied
    // to `settings`) and its lints (settings.lintIds of the capture; empty report without lints)
    PixelImage composeCapture(const CaptureOutput& output, SheetSettings settings, SheetLayout* layout = nullptr);
    lint::LintReport lintCapture(const CaptureOutput& output, const CaptureSettings& settings);

    class EffectCapture {
    public:
        static constexpr double kStep = 1.0 / 60.0;
        static constexpr double kFrameBudgetMs = 12.0;
        static constexpr double kTimeoutSeconds = 120.0;

        EffectCapture(CaptureSettings settings, std::unique_ptr<CaptureTarget> target);
        ~EffectCapture();
        EffectCapture(const EffectCapture&) = delete;
        EffectCapture& operator=(const EffectCapture&) = delete;

        // Starts it: its work runs in the editor's frames from now on
        void start();
        void cancel();
        bool isFinished() const { return finished; }
        bool isFailed() const { return !error.empty(); }
        const std::string& getError() const { return error; }
        double getProgress() const;
        std::string getStage() const;
        // the result (complete once finished without an error); its lint input points into it
        const CaptureOutput& getOutput() const { return output; }
        const CaptureSettings& getSettings() const { return settings; }
        const CaptureTarget& getTarget() const { return *target; }
        // called once when the run ends (ok or not), from processFrame
        void setOnFinished(std::function<void()> done) { onFinished = std::move(done); }

        // True while a run has started and not ended. A second capture is refused while one runs: captures
        // share the capture view and a Play capture holds the game's fixed step and pause.
        static bool anyRunning();

        // The editor calls these from its frame (App::engineRender); true while a run still works
        static bool processFrame();
        static void setFrameRequester(std::function<void()> requester);
        // the graphics view goes away (editor shutdown): every run fails, the GPU objects go
        static void releaseGpu();

        struct Impl;

    private:
        // one slice of work within the budget; true when the run ended
        bool advance(double budgetMs);
        void finish(const std::string& failure);

        CaptureSettings settings;
        std::unique_ptr<CaptureTarget> target;
        std::unique_ptr<Impl> impl;
        CaptureOutput output;
        std::string error;
        bool started = false;
        bool finished = false;
        std::function<void()> onFinished;
    };

}
