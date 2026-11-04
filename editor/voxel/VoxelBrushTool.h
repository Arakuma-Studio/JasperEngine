// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The armed Voxel Volume brush (docs/specs/T6-voxel-volumes.md R60): the state the panel, the Scene view
// and the shortcuts share, and what a stroke does to the volume. Brushes raycast level 0, so they hit
// where editing happens (R53). No ImGui: the Scene view passes rays and the modifiers it read.

#include "VoxelBrush.h"

#include "command/CommandHistory.h"
#include "ecs/Entity.h"
#include "math/Ray.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
    class VoxelSystem;
}

namespace jasperengine::editor {

    class Project;
    struct SceneProject;

    struct VoxelBrushModifiers {
        bool ctrl = false;
        bool shift = false;
    };

    // The live info strip (R60), like T1's Terrain Info
    struct VoxelCursorInfo {
        bool valid = false;
        voxel::Vec3d position;        // metres in the volume's frame
        double value = 0.0;           // signed distance, metres
        voxel::MaterialSample material;
        int64_t stampVoxels = 0;      // the voxels the stamp would touch
        bool stampEstimated = false;  // too big a shape to ask each frame: its box
        int64_t memoryBytes = 0;
    };

    class VoxelBrushTool {
    public:
        static VoxelBrushTool& get();

        // Keeps away the target and the stroke; settings stay
        void reset();

        void setTarget(Project* project, uint32_t sceneId, Entity owner);
        uint32_t getTargetScene() const { return sceneId; }
        Entity getTargetEntity() const { return owner; }
        Project* getProject() const { return project; }

        void arm(VoxelTool tool);
        void disarm();
        bool isArmed() const { return armed; }
        VoxelBrushSettings& settings() { return brush; }
        const VoxelBrushSettings& settings() const { return brush; }
        void adjustSize(float factor);

        // Armed, nothing playing, and the target in this scene with its store loaded
        bool isEditingScene(Scene* scene) const;

        // ---- the mouse. A stroke's effect is decided by the modifiers now and held to its end (T1 R56).
        bool beginStroke(Scene* scene, const Ray& ray, VoxelBrushModifiers modifiers);
        bool paintStroke(Scene* scene, const Ray& ray);
        void endStroke();
        // Esc: the stroke's edits go, as they do when the history cancels the interactive edit itself
        void cancelStroke();
        bool isStrokeActive() const { return stroke.active; }
        VoxelTool strokeTool() const { return stroke.effective; }

        // ---- Alt on the armed brush (T1 DC12)
        void altPress(float x, float y) { alt.press(x, y); }
        // True from the moment it is an orbit
        bool altMove(float x, float y, float threshold) { return alt.move(x, y, threshold); }
        bool altActive() const { return alt.isActive(); }
        // True while the camera must wait: a press not yet past the threshold
        bool altHolding() const { return alt.isActive() && !alt.isOrbit(); }
        // The release. A click picks the material under the ray and returns true.
        bool altRelease(Scene* scene, const Ray& ray);

        // The material under the ray becomes the brush's. False when the ray hits nothing.
        bool pick(Scene* scene, const Ray& ray);

        // ---- the decal and the info strip
        // Follows the pointer; hit tests the volume and rebuilds the decal
        void updateCursor(Scene* scene, const Ray& ray);
        // Each frame: the colour follows a modifier as soon as it is pressed, before the click (T1 R55)
        void tick(VoxelBrushModifiers modifiers, bool cameraMoving);
        void hideCursor();
        const VoxelDecal& decal() const { return cursorDecal; }
        bool decalVisible() const { return decalShown; }
        float decalAlpha() const { return alpha; }
        const VoxelCursorInfo& cursorInfo() const { return info; }
        // Bumped when the decal's lines or its colour change
        uint64_t decalRevision() const { return decalRev; }

        // The reason the last stroke did nothing ("" when it did): the panel shows it
        const std::string& lastError() const { return error; }
        void clearError() { error.clear(); }

        // ---- no mouse: one stroke through points in the volume's frame, the same stamps as a drag
        // through them, one undo step. Leaves the armed state and the target alone. False with `why`.
        static bool scriptedStroke(Project* project, uint32_t sceneId, Entity owner, const VoxelBrushSettings& settings, VoxelTool effective,
                                   const std::vector<voxel::Vec3d>& points, std::string& why);

        // The ray into the volume's frame, against level 0 of the store, or of `surface` when given. An Add
        // over nothing lands on the floor (y = 0), since an empty volume has no surface to start from.
        static bool hitVolume(VoxelSystem& system, Entity owner, const Ray& ray, bool floorFallback, voxel::RaycastHit& hit, voxel::Store* surface = nullptr);

    private:
        struct Stroke {
            bool active = false;
            InteractiveEdit edit;
            uint32_t sceneId = 0;
            Entity owner = NULL_ENTITY;
            VoxelTool effective = VoxelTool::Dig;
            VoxelBrushShape shape = VoxelBrushShape::Sphere;
            VoxelBrushSettings settings;
            std::vector<voxel::Vec3d> stamps;
            VoxelStrokeStamper stamper{1.0};
            // The surface as it was at the press, what the stroke's rays hit (T1's height reference). On
            // the live one each stamp would sink the next, and holding still would dig on forever.
            std::unique_ptr<voxel::Store> surface;
            voxel::Vec3d capsuleStart;
            voxel::Vec3d capsuleEnd;
        };

        bool strokeCancelled();
        void openStroke(uint32_t scene, Entity volume, const VoxelBrushSettings& settings, VoxelTool effective, double spacing);
        bool applyStamp(const voxel::Vec3d& center, const voxel::Vec3d& end);
        void applyStamps();
        void finishStroke();

        Project* project = nullptr;
        uint32_t sceneId = 0;
        Entity owner = NULL_ENTITY;
        bool armed = false;
        VoxelBrushSettings brush;
        VoxelTool toolBeforePick = VoxelTool::Paint;
        Stroke stroke;
        VoxelAltGesture alt;

        // what the decal and the strip were last built from
        struct CursorKey {
            bool valid = false;
            Vector3 origin;
            Vector3 direction;
            uint64_t version = 0;
            VoxelTool tool = VoxelTool::Dig;
            float radius = 0.0f;
            VoxelBrushShape shape = VoxelBrushShape::Sphere;
        };
        CursorKey cursor;
        bool hasCursorHit = false;
        voxel::RaycastHit cursorHit;
        VoxelDecal cursorDecal;
        bool decalShown = false;
        float alpha = 0.0f;
        VoxelTool liveEffective = VoxelTool::Dig;
        uint64_t decalRev = 1;
        std::chrono::steady_clock::time_point pointerMoved;
        VoxelCursorInfo info;
        std::string error;
    };

}
