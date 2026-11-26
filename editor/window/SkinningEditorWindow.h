// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > 2D > Skinning Editor (docs/specs/S94-rendering-anim-gaps.md F49, Unity's Skinning
// Editor of 2D Animation): the skin of the selected Sprite's frame, drawn over its pixels.
// - Create Bone: drag from the head to the tail (the selected bone is the parent).
// - Auto Geometry: the triangle grid over the opaque pixels (Cell Size, Alpha Tolerance).
// - Auto Weights, Bind to Bone (100%), and the Weight Brush (Add, Subtract, Smooth; Size,
//   Strength) painting the selected bone; the vertices show their weights in the bones' colours.
// - Preview Pose: drag a bone to turn it; the mesh bends here only (nothing is written), Reset
//   Pose puts it back.
// Every edit is one undo step (a brush stroke on release): the skin goes into the source's meta
// and the Sprite's bone GameObjects and Sprite Skin follow (sprite::applySkinEdit).

#include "pixelart/PixelImage.h"
#include "sprite/SpriteSkinEdit.h"

#include "ecs/Entity.h"
#include "texture/Texture.h"

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class SkinningEditorWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Skinning Editor";

        explicit SkinningEditorWindow(Project* project);

        void show();
        void open();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        // another project: the target and its pixels go
        void resetForProjectSwitch();
        // the Sprite Skin Inspector's Open Skinning Editor: opened on that Sprite on the next show
        static void requestOpen(uint32_t sceneId, Entity entity);

    private:
        enum class Tool { CreateBone, Geometry, WeightBrush, PreviewPose };

        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;

        // the Sprite shown: followed from the selection unless locked
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        bool locked = false;
        sprite::SkinTarget target;
        bool hasTarget = false;
        std::string targetError;
        std::string frameKey;                 // source | frame name: a change loads the skin again
        std::filesystem::file_time_type metaTime;
        SpriteSkinGeometry skin;      // the stored skin, or the stroke being painted

        // the frame's page on the GPU
        Texture pageTexture;
        std::string pageKey;
        int pageWidth = 0, pageHeight = 0;
        std::vector<Texture> retired;

        Tool tool = Tool::CreateBone;
        int selectedBone = -1;
        float zoom = 4.0f;
        bool fitRequested = true;
        bool showMesh = true;
        bool showWeights = true;
        sprite::AutoGeometrySettings geometry;
        sprite::WeightBrushMode brushMode = sprite::WeightBrushMode::Add;
        float brushSize = 12.0f;
        float brushStrength = 0.2f;
        bool stroking = false;                // a brush stroke in progress (written on release)
        bool draggingBone = false;            // Create Bone: from dragStart to the mouse
        Vector2 dragStart;
        int turningBone = -1;                 // Preview Pose: the bone being turned
        std::vector<float> previewAngles;     // degrees per bone, preview only
        std::string statusMessage;
        bool statusIsError = false;

        void followSelection();
        void refreshTarget();
        void uploadPage();
        void commit(const SpriteSkinGeometry& edited, const std::vector<std::string>& removed, const std::string& what);
        // the vertices as Preview Pose bends them (frame pixels)
        std::vector<Vector2> posedVertices() const;
        // a bone's head and tail as Preview Pose places them (frame pixels)
        void posedBone(int bone, Vector2& head, Vector2& tail) const;

        void drawToolbar();
        void drawSidePanel();
        void drawCanvas();
        void setStatus(const std::string& message, bool error);
    };

}
