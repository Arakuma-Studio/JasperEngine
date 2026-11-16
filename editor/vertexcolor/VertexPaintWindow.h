// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R101: the Vertex Paint window (the Terrain window's layout: a tool
// strip, then property tables): Color, Brush, Masks, Selection, Colors on this mesh, Generate & Bake,
// View. Its generator dialogs build ops and run them through the facade (applyEdit), as jobs with
// progress and Cancel (R97); Export as Model and Bake to Texture call the facade's exports.

#include "vertexcolor/VertexPaintTool.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class CommandCatalog;
    class Project;
}

namespace jasperengine::editor::vertexcolor {

    class VertexPaintWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Vertex Paint";

        static VertexPaintWindow& get();

        void setProject(Project* project);
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        // each frame, open or not: the cursor's fade and a running generator's job
        void show();
        // the VertexPaint context's commands (Table 3) run here
        void bindCommands(CommandCatalog& catalog);
        // R100: the facade's paintToolHook (what every Paint button runs after ensureComponent)
        // selects the entity, opens this window and arms Paint
        static void installPaintToolHook();

        // ---- R101 Generate & Bake
        enum class Dialog { None, FromTexture, AmbientOcclusion, Curvature, Thickness, Gradient, Noise, Direction, TransferFrom, Adjust, Channels,
                            ExportModel, BakeTexture };
        static const char* dialogTitle(Dialog dialog);   // "Ambient Occlusion"
        void openDialog(Dialog dialog);
        Dialog currentDialog() const { return dialog; }

        // what the dialogs edit (public: the tests fill them as a person would)
        struct Fields {
            // From Texture (R41)
            bool fromMaterial = true;
            int materialSlot = 0;                 // MaterialSlot
            std::string texturePath;
            bool colorData = true;
            int uvSet = 0;
            int samples = 16;
            bool keepSeams = true;
            // AO / Thickness (R42)
            int rays = 64;
            float distance = 1.0f;
            bool sceneScope = false;
            int seed = 0;
            // Curvature (R43)
            int curvatureBlur = 1;
            // Gradient (R45)
            int gradientType = 2;                 // vc::GradientType: Height by default
            Vector3 gradientFrom = Vector3(0.0f, 0.0f, 0.0f);
            Vector3 gradientTo = Vector3(0.0f, 1.0f, 0.0f);
            Vector3 axis = Vector3(0.0f, 1.0f, 0.0f);
            int interpolation = 1;                // vc::Interpolation: OKLab
            int extend = 3;                       // vc::Extend: Both
            int gradientElement = 0;              // vc::Element
            // Noise (R46)
            int noiseKind = 1;                    // vc::NoiseKind: Simplex
            float noiseScale = 1.0f;
            int octaves = 3;
            bool noiseWorld = true;
            // Direction (R44)
            Vector3 direction = Vector3(0.0f, 1.0f, 0.0f);
            float rangeMin = 0.0f, rangeMax = 1.0f;
            // Transfer From (R60)
            Entity transferSource = NULL_ENTITY;
            float maxDistance = 0.1f;
            float normalAngle = 60.0f;
            bool unmatchedNearest = false;
            // Adjust (R47)
            int adjustKind = 0;                   // vc::AdjustKind
            float inBlack = 0.0f, inWhite = 1.0f, gamma = 1.0f, outBlack = 0.0f, outWhite = 1.0f;
            float brightness = 0.0f, contrast = 0.0f;
            float hueShift = 0.0f, saturationShift = 0.0f, valueShift = 0.0f;
            // Channels (R48)
            int channelKind = 0;                  // vc::ChannelOpKind
            int sourceChannel = 0, destChannel = 1;
            int packSource[4] = {0, 1, 2, 3};     // -1: the constant
            float packConstant[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            // Export as Model (R61)
            std::string exportPath;
            bool replaceInScene = false;
            bool splitHardEdges = true;
            // Bake to Texture (R62)
            int bakeTarget = 0;
            std::string bakePath;
            int bakeSize = 1024;
            int bakePadding = 4;
            int bakeUvSet = 0;
            bool bakeSrgb = true;
        };
        Fields fields;

        // the ops the open dialog's fields make for the acting targets (perTarget: a material slot's
        // image is each target's own); false with `error`
        bool dialogOps(Dialog dialog, std::vector<std::vector<vc::Op>>& perTarget, std::vector<TargetRef>& targets, std::string& undoName,
                       std::string& error);
        // the dialog's Apply / Export / Bake
        bool runDialog();
        bool isJobRunning() const { return job.running; }
        float jobProgress() const { return job.progress.load(); }
        void cancelJob();
        const std::string& dialogMessage() const { return dialogNote; }

        // R102: the window draws in the Scene view what the tools show
        Project* project() const { return projectPtr; }

    private:
        void drawToolStrip(uint32_t sceneId);
        void drawTargetRow(uint32_t sceneId);
        void drawMaterialWarnings(uint32_t sceneId);
        void drawColor(uint32_t sceneId);
        void drawBrush();
        void drawMasks(uint32_t sceneId);
        void drawSelection(uint32_t sceneId);
        void drawColorsOnMesh(uint32_t sceneId);
        void drawGenerate(uint32_t sceneId);
        void drawView();
        void drawDialog(uint32_t sceneId);
        bool dialogFields(uint32_t sceneId);

        Project* projectPtr = nullptr;
        bool windowOpen = false;
        bool focusRequested = false;
        Dialog dialog = Dialog::None;
        std::string dialogNote;
        std::string paletteAsset;
        // the Colors on this mesh list, built again after each edit
        struct MeshColors {
            uint32_t sceneId = 0;
            size_t historyPosition = (size_t)-1;
            std::vector<TargetRef> targets;
            std::vector<vc::PaletteEntry> entries;
        };
        MeshColors meshColors;
        struct Job {
            bool running = false;
            std::atomic<bool> cancel{false};
            std::atomic<float> progress{0.0f};
        };
        Job job;
    };

}
