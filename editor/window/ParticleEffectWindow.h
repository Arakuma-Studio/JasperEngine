// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Visual Effects > Particle Effect (docs/specs/S91-effect-authoring.md R17-R23, R25,
// D3, D4; docs/editor/particle-effect.md): one effect as a whole. It opens on a .particles file
// (a double-click in the Project window) or on a GameObject's subtree (Open in Particle Effect),
// and shows:
//  - the Outline: every system of the subtree and their sub-emitter children down the tree, each
//    with a preview-only eye (R18): hiding draws nothing of that node in the Preview Stage and
//    never writes a file;
//  - the timing bars, analytic (ParticleEffectTiming, the same rows MCP gets), with the
//    playhead: a drag scrubs (R19);
//  - the Preview Stage, a private scene (ParticlePreviewRender's stepped mode) with its own
//    clock: Play, Pause, Step, Speed, the seed (Reroll, a new one each loop if asked) and a loop
//    range (R20, R21); backgrounds, a moving emitter, the Overdraw view and the stats;
//  - the selected node's modules, the rows the Inspector draws (ParticleModuleRows.h, R22);
//  - Exposed Parameters, sliders and trigger buttons that drive the stage alone (R25).
//
// Frame-locked editing (R17, R20): an edit while playing or paused re-simulates the stage to the
// same time with the same seed, from the checkpoints and at most 4 ms a frame, the progress
// shown. The seed changes only on Reroll or, with Per-Loop Seed on, when the clock loops.
//
// The stage plays copies of the documents the effect library holds under keys of its own
// ("particle-effect-window/<node>.particles"), rewritten so that each node's sub-emitter
// entries name its children's copies: that is how the eye and the Overdraw view (every
// particle an additive constant heat) change the stage and never the files or an export.
// The copies follow the files (their library versions) and the inline effects (their text).
//
// Undo: an edit of a file is a ParticleEffectAssetCmd on the project history, of an inline
// effect a scene step on its GameObject (ParticleEffectEdit); a drag merges into one step, a
// no-op makes none, and Esc during a drag puts the file back with no step (App's interactive
// edits). The window's own settings (the eye, the clock, the stage) are not undo steps.
//
// Filmstrip and Checks (R24, AC15): the window hosts ParticleChecksPanel in a section under the
// Exposed Parameters, on what it shows (the file or the subtree) and with its seed, so its rows are
// preview_particle_effect's; its Fix applies a single-edit fix_hint as one ParticleEffectAssetCmd
// and its Record bakes the sprite sheet (or a PNG sequence) as one undo step.

#include "ecs/Entity.h"
#include "particles/ParticleEffect.h"
#include "render/preview/ParticlePreviewRender.h"
#include "util/ParticleEffectTiming.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Command;
    class CommandHistory;
    class ParticleChecksPanel;
    class Project;

    class ParticleEffectWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Particle Effect";
        // the library keys of the stage's copies start with it
        static constexpr const char* STAGE_KEY_PREFIX = "particle-effect-window/";

        using Background = ParticlePreviewRender::Background;

        // A node of the Outline: a system (a GameObject of the subtree, or the opened file) or a
        // sub-emitter child. Its row of timing() has its index.
        struct Node {
            std::string name;
            std::string effect;            // the .particles (assets-relative), empty for an inline effect
            uint32_t sceneId = 0;          // a system of a subtree: its GameObject
            Entity entity = NULL_ENTITY;
            int parent = -1;
            int subEmitter = -1;
            int depth = 0;
            bool missing = false;          // a child whose file does not read
        };


        explicit ParticleEffectWindow(Project* project);
        ~ParticleEffectWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return focused; }
        void requestFocus();

        // the window on a .particles file (assets-relative), opened and focused
        void openEffect(const std::string& assetPath);
        // the window on a GameObject's subtree (every Particle System at or below it)
        void openSubtree(uint32_t sceneId, Entity root);
        // what it shows: the file, or "" for a subtree (then getSubtreeRoot)
        const std::string& getEffectPath() const { return effectPath; }
        Entity getSubtreeRoot() const { return subtreeRoot; }
        void close();

        void resetForProjectSwitch();
        void clearSceneState(uint32_t sceneId);

        // ---- the Outline (what its rows do)
        const std::vector<Node>& getNodes() const { return nodes; }
        void select(int node);
        int getSelected() const { return selected; }
        void setHidden(int node, bool hidden);
        bool isHidden(int node) const { return hidden.count(nodeKey(node)) > 0; }

        // ---- the timing bars (R19): the rows of the nodes, analytic
        const std::vector<ParticleTimingRow>& timing() const { return rows; }
        // the bars' span: the rows' latest finite time, or the loop range's end
        float timelineSpan() const;

        // ---- the clock (R20, R21)
        void play();
        void pause();
        bool isPlaying() const { return playing; }
        void togglePlay() { playing ? pause() : play(); }
        void setSpeed(float speed);
        float getSpeed() const { return speed; }
        // one tick of the first system, paused
        void stepFrame();
        // back to 0, the same seed
        void restart();
        // the playhead to `time` (a scrub): re-simulated there, paused
        void seek(double time);
        double getTime() const;
        uint32_t getSeed() const { return seed; }
        void setSeed(uint32_t seed);
        // a new random seed, the same time
        void reroll();
        void setPerLoopSeed(bool on) { perLoopSeed = on; }
        // the toolbar's label of that option
        static const char* perLoopSeedLabel();
        bool isPerLoopSeed() const { return perLoopSeed; }
        // the clock loops over [start, end] (off: over the effect, a one-shot from its end to 0)
        void setLoopRange(bool enabled, float start, float end);
        bool hasLoopRange() const { return loopRange; }
        float getLoopStart() const { return loopStart; }
        float getLoopEnd() const { return loopEnd; }
        // an edit, a scrub or a loop is re-simulating the stage
        bool isResimulating() const { return stage.isSeeking(); }
        float resimulationProgress() const { return stage.seekProgress(); }

        // ---- the Preview Stage (R21)
        void setBackground(Background background);
        Background getBackground() const { return background; }
        void setMovingEmitter(bool on);
        bool isMovingEmitter() const { return moving; }
        void setOverdraw(bool on);
        bool isOverdraw() const { return overdraw; }
        ParticlePreviewRender::StageStats stageStats() { return stage.stats(); }
        ParticlePreviewRender& getStage() { return stage; }

        // ---- edits of the selected node's effect (the module rows): one undo step, merging
        // until `commit`; false when nothing changed or the node cannot be edited
        bool editSelected(const std::function<bool(particles::ParticleEffectDocument& document)>& edit, bool commit = true);
        // the selected node's document as the files hold it (false: none, or unreadable)
        bool readSelected(particles::ParticleEffectDocument& document, std::vector<particles::Diagnostic>* diagnostics = nullptr) const;

        // ---- Exposed Parameters (R25): the stage only
        struct ExposedRow {
            std::string name;
            particles::ExposedType type = particles::ExposedType::Float;
            Vector4 value;
        };
        std::vector<ExposedRow> exposedRows() const;
        void setExposedValue(const std::string& name, const Vector4& value);
        // the trigger names the effects answer (Trigger sub-emitters)
        std::vector<std::string> triggerNames() const;
        void fireTrigger(const std::string& name);

        // the clock and the stage's re-simulation for a frame of `dt` seconds (show() calls it)
        void update(double dt);

        // the hosted Filmstrip and Checks, on what the window shows and with its seed
        ParticleChecksPanel& getChecksPanel();

        // what editor_status shows of it (MCP tests read the clock and the eye through it)
        struct UiState {
            bool open = false;
            bool focused = false;
            std::string effect;
            std::string subtreeRoot;
            double time = 0.0;
            uint32_t seed = 0;
            bool playing = false;
            float speed = 1.0f;
            bool resimulating = false;
            std::vector<std::string> nodes;
            std::vector<std::string> hidden;
            int selected = -1;
            uint32_t live = 0;
            uint64_t stateHash = 0;
        };
        UiState uiState();

    private:
        Project* project;
        bool windowOpen = false;
        bool focused = false;
        bool focusRequested = false;

        std::string effectPath;
        uint32_t subtreeScene = 0;
        Entity subtreeRoot = NULL_ENTITY;

        std::vector<Node> nodes;
        std::vector<ParticleTimingRow> rows;
        int selected = 0;
        // the selection by node key: the index moves when the tree above it changes
        std::string selectedKey;
        // the eye: hidden nodes by key (their effect and place in the tree), the stage only
        std::set<std::string> hidden;

        ParticlePreviewRender stage;
        std::unique_ptr<ParticleChecksPanel> checks;
        // what the stage copies were made of: the files' versions, the inline effects' text, the eye
        uint64_t stagedSignature = 0;
        bool staged = false;

        bool playing = true;
        float speed = 1.0f;
        uint32_t seed = 1;
        bool perLoopSeed = false;
        bool loopRange = false;
        float loopStart = 0.0f;
        float loopEnd = 1.0f;
        // the clock's time while the stage seeks (the stage's own time lags it)
        double clock = 0.0;
        Background background = Background::Dark;
        bool moving = false;
        bool overdraw = false;
        bool scrubbing = false;
        // frames the stage still queues after a change while paused (D4: a still stage draws nothing)
        int stageFramesDue = 0;
        unsigned int stageWidth = 0;
        unsigned int stageHeight = 0;
        // the Curve window was opened on a curve over the emitter's time: it shows the playhead
        bool curvePlayhead = false;
        std::vector<ExposedRow> exposedOverrides;

        // the merging step of a drag in the rows (ended by commit)
        Command* editCommand = nullptr;
        CommandHistory* editHistory = nullptr;
        std::set<std::string> expanded;

        std::string nodeKey(int node) const;
        std::string stageKey(int node) const;
        // the nodes of what is opened, and their timing; false when nothing can be shown
        bool collectNodes();
        uint64_t sourceSignature() const;
        // a subtree system's place under the root (false: a file, or no such GameObject)
        bool placement(const Node& node, Vector3& position, Quaternion& rotation, Vector3& scale) const;
        void stageChanged() { stageFramesDue = 2; }
        // the stage copies written into the effect library, the stage rebuilt with the systems
        void restage();
        const particles::ParticleEffectDocument* sourceDocument(int node, particles::ParticleEffectDocument& scratch) const;
        void wrapLoop();
        // A looping effect's clock (no Loop Range): its Duration, and the time it is steady from (a
        // whole number of Durations covering the longest life plus one Duration). The clock runs to
        // steady + Duration, then goes back one Duration. False for a one-shot effect.
        bool loopingClock(double& duration, double& steady) const;
        void pushEdit(CommandHistory* history, Command* command, bool commit);

        void drawToolbar();
        void drawStage(float width, float height);
        void drawOutline();
        void drawTimingBar(int node, float x0, float x1, float y0, float y1, float span);
        void drawRuler(float span);
        void drawInspector();
        void drawExposed();
    };

}
