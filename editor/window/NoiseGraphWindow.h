// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/T2-noise-graphs.md R50, R55-R59)
// - Purpose: Window > Terrain > Noise Graph, the editor of a .noisegraph: a toolbar (Graph, Seed with a
//   dice, Preview Size, Space, Frame, Preview, Apply), the canvas, and a right panel (the selected node's
//   parameters, or the exposed variables and the Preview image with its statistics).
// - All logic is NoiseGraphDocument's; this file draws it and uploads what it asks for: at most 8 thumbnails
//   a frame, each a Texture of its own, gone when the window closes. Closed or on a hidden tab, the window
//   runs no job and holds no texture (R50).
// - Edits are project undo steps; Delete, Copy, Paste and Duplicate reach it through the catalog's edit.*
//   commands (App's claim). Graph errors show on their nodes and in a bar over the canvas, never in the
//   Console (R58).
// - Thread-safety: main thread.

#pragma once

#include "noise/NoiseGraphDocument.h"
#include "noise/TerrainGeneration.h"
#include "texture/Texture.h"
#include "window/widget/GraphCanvas.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class NoiseGraphWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Noise Graph";

        explicit NoiseGraphWindow(Project* project);
        ~NoiseGraphWindow();

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return windowFocused; }
        void requestFocus();
        // The window on a graph file (absolute, or relative to the assets)
        void openGraph(const std::filesystem::path& file);
        // A project switch: the document is of the old project
        void prepareForProjectSwitch();

        // Delete, Copy, Paste and Duplicate, through the catalog (App's claim)
        bool canDelete() const { return document.canDelete(); }
        bool canCopy() const { return document.canCopy(); }
        bool canPaste() const { return document.canPaste(); }
        bool canDuplicate() const { return document.canCopy() && document.canEdit(); }
        void deleteSelection() { document.deleteSelection(); }
        void copySelection() { document.copySelection(); }
        void paste();
        void duplicateSelection() { document.duplicateSelection(); }

        NoiseGraphDocument& getDocument() { return document; }

    private:
        void drawToolbar();
        void drawIssueBar();
        void drawCanvas();
        void drawSearchPopup();
        void drawRightPanel();
        void drawNodeParams(const std::string& nodeId);
        void drawVariablesAndPreview();
        void applyToSelectedTerrain();
        void refreshGraphList();
        // Textures live until the frame after the last draw that may use them: ImGui draws after show()
        void uploadThumbnails();
        void uploadPreviewImage();
        ImTextureID makeTexture(Texture& texture, const std::string& id, const std::vector<uint8_t>& rgba, int size);
        void releaseTextures();
        void releaseRetired();

        Project* project = nullptr;
        NoiseGraphDocument document;
        GraphCanvas canvas;
        bool windowOpen = false;
        bool windowFocused = false;
        bool focusRequest = false;
        bool frameRequest = false;

        std::map<std::string, Texture> thumbnails;
        Texture previewTexture;
        uint64_t previewTextureRevision = 0;
        std::vector<Texture> retired;
        uint64_t textureCounter = 0;

        NoisePreviewSettings settings;
        std::vector<std::filesystem::path> graphFiles;

        // the search popup: open at a canvas position, with a dragged wire's pin
        bool searchRequest = false;
        ImVec2 searchPosition;
        uint64_t searchPin = 0;
        char searchText[64] = {};
        int searchCursor = 0;

        std::shared_ptr<TerrainGeneration::Run> applyRun;
        std::string applyStatus;
    };

}
