// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "math/Ray.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    enum class FluidPreviewState { STOPPED, PLAYING, PAUSED };

    // The editor's live tools of a FluidSimulation in edit mode, shared by the Inspector (its
    // preview bar and Paint Dye toggle), the Scene view's input (SceneWindow) and its overlay
    // (SceneRender):
    //
    // - Preview: a stopped scene keeps its fluids still (Project::pauseEngineScene); Play steps
    //   one through FluidSystem::setPreview and keeps its Scene view redrawing, Pause holds it
    //   (queued splats still paint), Stop restarts it (needReset). Previews end when the scene
    //   plays, the GameObject or its component goes, or the Inspector closes.
    // - Paint Dye: armed on one fluid whose output is drawn on its Sprite, a left drag over the
    //   Sprite in the Scene view is the fluid's pointer (FluidSimulator::pointerDown/Move/Up in
    //   fluid coordinates from the ray hit on the quad), previewing it. Transient simulation
    //   state: no undo step and nothing saved. Edit mode only (in Play the game's interactive
    //   input drives the fluid); disarmed by Esc, the toggle, another selection or Play.
    //
    // Main thread, editor-global (one brush, like the terrain brush).
    class FluidEditorTools {
    public:
        // ---- preview
        static FluidPreviewState getPreviewState(uint32_t sceneId, Entity entity);
        static void playPreview(Project* project, uint32_t sceneId, Entity entity);
        static void pausePreview(Project* project, uint32_t sceneId, Entity entity);
        // the fluid restarts (seed, start burst) and holds still again
        static void stopPreview(Project* project, uint32_t sceneId, Entity entity);
        // every preview and the brush (play starts, the Inspector closes, a save)
        static void stopAll(Project* project);
        // the scene is gone: forget its previews without touching it
        static void forgetScene(uint32_t sceneId);
        // once a frame: ends what is no longer valid, keeps previewing Scene views redrawing
        static void update(Project* project);

        // ---- Paint Dye brush
        // false (with why) when this fluid cannot be painted now
        static bool canPaint(Project* project, uint32_t sceneId, Entity entity, std::string* reason = nullptr);
        static bool arm(Project* project, uint32_t sceneId, Entity entity);
        static void disarm();
        static bool isArmed(uint32_t sceneId, Entity entity);
        // the brush is armed on a fluid of this scene (SceneRender, SceneWindow: the Scene
        // view's left button paints instead of selecting)
        static bool isPainting(const Scene* scene);
        // a press over the fluid's Sprite starts a stroke (and the preview); false: missed it
        static bool beginStroke(Scene* scene, const Ray& ray);
        static void paintStroke(Scene* scene, const Ray& ray);
        static void endStroke();
        // `[` / `]`: splatRadius x factor, one undo step for a run of presses
        static void adjustRadius(float factor);
        // the brush circle (world points, a closed loop) under the ray on the fluid's quad: the
        // Scene view draws it over its image (SceneWindow), so no scene object hides it
        static bool cursorCircle(const Scene* scene, const Ray& ray, std::vector<Vector3>& points);

        // the world corners of an entity's Sprite quad (bottom-left, bottom-right, top-right,
        // top-left in its local space); false without a Sprite
        static bool spriteCorners(Scene* scene, Entity entity, std::vector<Vector3>& corners);

        // A ray on an entity's Sprite quad: the point in fluid coordinates ((0,0) bottom-left,
        // (1,1) top-right of the image the Sprite shows) and the world point; false when the
        // ray misses the quad's plane (or the quad itself with requireInside)
        static bool rayToFluid(Scene* scene, Entity entity, const Ray& ray, bool requireInside, Vector2& fluidPos, Vector3* worldPos = nullptr);

        // the editor brush's pointer id (FluidSimulator pointers: 0 is the game's mouse)
        static constexpr int BRUSH_POINTER = 0x7ffffff0;
    };

}
