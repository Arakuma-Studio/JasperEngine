// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > 2D > Sprite Preview (docs/specs/S44-aseprite-import.md R31): a read-only look at one
// .aseprite file. Source is the file composed by the codec (every visible layer, or the layers
// ticked here); Imported is what the game draws: the import's sheet and its page PNGs. Tags play
// with their direction and repeat, frames step with the catalog's spritePreview.* keys, and the
// overlays (slices, pivot, trim, pixel grid), onion skin and Compare at 1x draw over the frame at
// an integer zoom. The one edit is Exclude from Import: the layer toggles become the import
// settings' layers, one project undo step.
//
// The file is read and the pages are decoded on a Background job; a new import record (the hot
// reload swapping one in) reads them again. GPU copies are made only for what is shown: the
// Imported pages once per import, a Source frame once per frame and layer set.

#include "pixelart/PixelImage.h"
#include "pixelart/draw/SpriteEdit.h"
#include "sprite/SpriteSheet.h"

#include "jobs/Task.h"
#include "texture/Texture.h"

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::draw {
    struct SpriteEdited;
}

namespace jasperengine::editor::pixelart::aseprite {
    struct Document;
    struct ImportRecord;
}

namespace jasperengine::editor {

    class Project;
    class ProjectWindow;
    class CommandCatalog;

    class SpritePreviewWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Sprite Preview";

        SpritePreviewWindow(Project* project, ProjectWindow* projectWindow);
        ~SpritePreviewWindow();

        void show();
        // Window > 2D > Sprite Preview: the window, with the file it had
        void open();
        // The window showing an .aseprite file (asset-relative "sprites/hero.aseprite"; an absolute
        // path under the assets directory is accepted too)
        void open(const std::string& assetPath);
        void setOpen(bool open);
        bool isOpen() const;
        bool isFocused() const;
        // Another project is being opened: the file, its pixels and the compare reference go
        void resetForProjectSwitch();

        // The spritePreview.* commands of the catalog (DefaultCommands.cpp); keysHere: the
        // window has the keys (App: it is focused)
        void bindCommands(CommandCatalog& catalog, std::function<bool()> keysHere);

        const std::string& getAssetPath() const { return assetPath; }
        int getFrame() const { return frame; }

    private:
        enum class View { Source, Imported };
        // Scene: the selected scene's Background colour, the colour the game clears to behind the sprite
        enum class Background { Checkerboard, Color, Scene };

        // What the job reads: the document, the import's pages, and the timeline and layer rows
        // derived from them (defined in the .cpp)
        struct Loaded;

        struct GpuImage {
            Texture texture;
            int width = 0;
            int height = 0;
            bool valid() const { return width > 0 && !texture.empty(); }
        };

        // One slice as the overlay draws it, from the document (Source) or the sheet (Imported)
        struct SliceOverlay {
            std::string name;
            pixelart::IntRect bounds;
            bool hasCenter = false;
            pixelart::IntRect center;
            bool hasPivot = false;
            pixelart::IntPoint pivot;
            ImU32 color = 0;
        };

        Project* project;
        ProjectWindow* projectWindow;
        bool windowOpen = false;
        bool focusRequested = false;
        bool focused = false;

        std::string assetPath;
        // the Project window's selection as last seen (a new single .aseprite selection is followed)
        std::vector<std::filesystem::path> seenSelection;

        // ---- the file and its import
        std::shared_ptr<const pixelart::aseprite::ImportRecord> record;
        std::string recordKey;
        bool recordSeen = false;
        jobs::Task<std::shared_ptr<Loaded>> loadTask;
        std::shared_ptr<Loaded> loaded;
        std::shared_ptr<const SpriteSheetData> sheet;
        uint64_t sheetGeneration = 0;

        // ---- playback
        View view = View::Source;
        int frame = 0;
        int tag = -1;                  // -1: every frame
        bool playing = false;
        bool loop = true;
        float speed = 1.0f;
        double elapsedMs = 0.0;        // in the shown frame
        int playIndex = 0;             // position in the playing sequence
        int spriteGroup = 0;           // layers_as_sprites: which sprite of the sheet

        // ---- view options
        int zoom = 4;
        bool fitRequested = true;   // the zoom follows the canvas size until a zoom or a pan
        ImVec2 pan = ImVec2(0.0f, 0.0f);
        bool onionSkin = false;
        int onionPrevious = 1;
        int onionNext = 1;
        float onionOpacity = 0.5f;
        bool showSlices = true;
        bool showPivot = true;
        bool showTrim = false;
        bool showGrid = true;
        Background background = Background::Checkerboard;
        float backgroundColor[3] = {0.19f, 0.30f, 0.47f};
        int stripLanes = 0;            // the frame strip's tag rows

        // ---- layer toggles (preview only), by layer path so a reload keeps them
        std::map<std::string, bool> layerToggles;
        uint64_t togglesRevision = 1;
        std::string statusMessage;
        bool statusIsError = false;

        // ---- Compare at 1x
        bool compare = false;
        std::string compareSheetPath;  // asset-relative
        int compareFrame = 0;
        std::filesystem::path compareStoredFor;   // the project whose choice is loaded
        std::vector<std::string> compareCandidates;
        double compareCandidatesAt = -1.0;
        std::string compareUploadedKey;   // what compareGpu holds: "<path>|<record key>|<frame>"
        std::shared_ptr<const SpriteSheetData> compareSheet;
        GpuImage compareGpu;

        // ---- GPU copies
        std::vector<GpuImage> pageGpu;             // Imported: the pages, uploaded once per import
        std::map<int, GpuImage> sourceGpu;         // Source: composed frames
        std::vector<int> sourceOrder;              // their use order (oldest first)
        uint64_t sourceGpuRevision = 0;            // togglesRevision they were composed with
        const void* sourceGpuDocument = nullptr;
        std::vector<Texture> retired;              // destroyed next frame (ImGui draws after show())

        // ---- agent edits (S47 R41): the changed rects, outlined and fading, and the status line
        struct AgentEdit {
            std::vector<pixelart::draw::DirtyRect> rects;
            std::string by, summary, revision;
            bool showLine = false;         // an agent's edit (an undo or redo only outlines)
            double at = 0.0;               // the steady clock of the edit
            uint64_t undoId = 0;           // the history step, found on the frame after it
        };
        std::optional<AgentEdit> agentEdit;
        std::string externalSaveNotice;
        void onSpriteEdited(const pixelart::draw::SpriteEdited& event);
        void adoptDocumentFromSession();
        void drawAgentEditLine();

        // ---- upkeep
        void followProjectSelection();
        void followImport();
        void startLoad();
        void adoptLoad();
        void clearFile();
        void retire(GpuImage& gpu);
        void retireAll();
        void releaseRetired();

        // ---- model
        int frameCount() const;
        int canvasWidth() const;
        int canvasHeight() const;
        uint32_t frameDuration(int index) const;
        const std::vector<SpriteTag>& tags() const;
        int spriteGroups() const;
        std::string spriteGroupName(int group) const;
        const SpriteFrame* sheetFrame(int fileFrame) const;
        std::vector<int> playSequence() const;
        void rangeOf(int& from, int& to) const;
        bool layerOn(int layer) const;
        void setLayerOn(int layer, bool on);
        std::vector<int> sourceLayers() const;

        // ---- actions (buttons and commands share them)
        void togglePlay();
        void stepFrame(int delta);
        void firstFrame();
        void lastFrame();
        void zoomBy(int steps);
        void selectTag(int index);
        void tickPlayback();
        void excludeFromImport();
        void setStatus(const std::string& message, bool error);

        // ---- GPU
        GpuImage* sourceFrameGpu(int fileFrame);
        GpuImage* pageTexture(size_t page);
        void updateCompareImage();

        // ---- compare reference, remembered per project (.jasperengine/user/sprite-preview.yaml)
        void loadCompareChoice();
        void saveCompareChoice();
        void scanCompareCandidates(bool force);

        // ---- UI
        void drawToolbar();
        void drawSidePanel();
        void drawLayers();
        void drawCompareSection();
        void drawCanvas(const ImVec2& size);
        void drawFrameStrip();
        // one frame (and its onion skin) at `origin`, zoomed; false when nothing could be drawn
        bool drawFrameImage(ImDrawList* draw, ImVec2 origin, int fileFrame, ImU32 tint);
        void drawOverlays(ImDrawList* draw, ImVec2 origin);
        // the selected scene's background colour; false when no scene is open (the checkerboard shows)
        bool sceneBackgroundColor(ImVec4& color, std::string* sceneName = nullptr) const;
        std::vector<SliceOverlay> slicesAt(int fileFrame) const;
    };

}
