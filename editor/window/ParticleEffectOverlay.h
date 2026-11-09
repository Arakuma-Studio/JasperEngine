// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's "Particle Effect" panel (docs/specs/S20-particles.md R28): with a Particle
// System selected in a stopped scene, its effect plays in Edit mode (Play/Pause, Restart, Stop,
// Playback Speed), the Playback Time slider scrubs it (a seek from the nearest checkpoint,
// spread over frames at <= 4 ms each, R29), and it shows the live count and the simulation and
// render times. "Simulate Layers" previews the selection only or every system of the scene.
// The gizmo toggles (bounds, colliders, force fields) are the Scene view's, for every scene.

#include "ecs/Entity.h"

#include "imgui.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    class ParticleEffectOverlay {
    public:
        struct Gizmos {
            bool bounds = false;
            bool colliders = true;
            bool forceFields = true;
            bool pixelGrid = false;     // the base-pixel grid around the emitters when zoomed in 4x or more (S49 R34)
        };
        static Gizmos& gizmos();

        // Each editor frame, for a scene: the preview advances its systems (nothing while it plays)
        void update(Project* project, SceneProject* sceneProject, double dt);
        // the panel over the canvas's bottom-right corner
        void draw(Project* project, SceneProject* sceneProject, const ImVec2& canvasMin, const ImVec2& canvasSize);
        void clear(uint32_t sceneId);

        // The panel's controls, also for tools (the MCP control_particles in Edit mode)
        void play(uint32_t sceneId);
        void pause(uint32_t sceneId);
        void restart(Project* project, SceneProject* sceneProject);
        void stop(Project* project, SceneProject* sceneProject);
        void seek(Project* project, SceneProject* sceneProject, double time);
        void setSpeed(uint32_t sceneId, float speed);
        // a system a tool controls joins the preview (selected or not) until the scene closes
        void addTarget(uint32_t sceneId, Entity entity);
        bool isPlaying(uint32_t sceneId) const;
        // a seek still spreading its ticks over frames
        bool isSeeking(uint32_t sceneId) const;
        // the systems it previews in the scene now
        std::vector<Entity> targets(Project* project, SceneProject* sceneProject);

    private:
        struct State {
            bool playing = true;
            float speed = 1.0f;
            bool simulateAll = false;
            bool seeking = false;
            std::vector<Entity> previewed;
            std::vector<Entity> extra;
        };
        std::map<uint32_t, State> states;

        void drawPixelGrid(Project* project, SceneProject* sceneProject, const std::vector<Entity>& systems, const ImVec2& canvasMin, const ImVec2& canvasSize);
    };

}
