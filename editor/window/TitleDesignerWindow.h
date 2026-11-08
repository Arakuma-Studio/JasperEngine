// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Title Designer (docs/specs/S82-title-designer.md): the window where people author a
// `.titleart`. One document at a time, in one window with internal regions (D1): a toolbar, the
// Layers and History tabs, the canvas, Properties with its live search, and the Library strip
// (S84's data through TitleDesignerHooks' provider). Everything it changes is an S80 op batch
// through TitleDesignerState (one named project undo step per gesture, nothing ever unsaved);
// selection, view and previews are view state, never undo steps (D8).
//
// Every button, menu, shortcut, drop and MCP call goes through the scripted API below (the
// SpriteNormalsWindow model); TitleDesignerWindowTestAccess (editor/tests/) drives the rest.
//
// Closed, it costs nothing (R65): show() returns at its first line, and no canvas view, renderer
// job, preview request, font listing or timer exists. Opening makes them; closing destroys them
// within the frame.

#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtHash.h"
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitleDesignerPrefs.h"
#include "titleart/designer/TitleDesignerState.h"
#include "titleart/designer/TitleLetterGizmo.h"
#include "titleart/designer/TitlePages.h"
#include "titleart/designer/TitlePropertyIndex.h"
#include "titleart/designer/TitleRows.h"
#include "titleart/designer/TransientAssetResolver.h"
#include "titleart/geometry/TitleGeometry.h"
#include "titleart/text/TitleTextLayout.h"

#include "ecs/Entity.h"
#include "texture/Texture.h"

#include "imgui.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    namespace titledesigner {
        class TitleCanvasView;
        class TitleLayerThumbnails;
        class TitleStarterLibrary;
        class TitleTextEdit;
        class TitleGizmo2D;
        class TitleGizmo3D;
        class TitleHandles;
        struct Gizmo3DTarget;
        struct CanvasRenderRequest;
    }

    namespace titlefonts {
        class FontCatalog;
        class FontPreviewRaster;
    }

    // what S81's Title Art Renderer gives its Edit button (R1, R30 In Context)
    struct TitleInContext {
        std::filesystem::path document;
        uint32_t sceneId = 0;
        Entity instance = NULL_ENTITY;
        // the instance's screen rectangle in the scene's game resolution (pixels, y down), and that
        // resolution; the snapshot of its scene without it is the window's to render
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        int screenWidth = 0, screenHeight = 0;
        std::vector<uint8_t> snapshotRgba;          // RGBA8 rows top first, screenWidth x screenHeight
    };

    class TitleDesignerWindow : public titledesigner::PropertyHost {
        friend struct TitleDesignerWindowTestAccess;

    public:
        static constexpr const char* WINDOW_NAME = "Title Designer";
        // the document's JSON (an editor-wide `Json` is nlohmann's unordered one)
        using Json = jasperengine::titleart::Json;

        explicit TitleDesignerWindow(Project* project);
        ~TitleDesignerWindow() override;

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }
        bool isFocused() const { return focused; }
        // the canvas has the keyboard (arrows nudge; elsewhere they move in lists)
        bool canvasHasKeys() const { return focused && canvasFocused; }
        // a text field, the text editor or the search takes the keys
        bool typing() const;
        // a file of a newer version is open: S80 renders nothing of it and its tree is not normalized
        bool newerVersion() const { return doc.isOpen() && !doc.document()->versionError().empty(); }
        // the ImGui rect of the window last frame (MCP's focus_window / screenshot)
        bool windowRect(ImVec2& min, ImVec2& max) const;
        void requestFocus() { focusRequested = true; }
        // the shared preview budget per frame (S82 R67: 1..16 ms, 8 on a software renderer)
        float previewBudgetMs() const { return settings.previewBudgetMs; }
        // a render, a settle or a preview is in progress: the editor keeps drawing frames
        bool needsFrames() const;
        void resetForProjectSwitch();
        // the editor settings were read: reopen as the window was (R64)
        void restoreFromSettings();

        // ---- the scripted API: every UI entry and MCP call goes through these ----
        bool open(const std::filesystem::path& file, std::string* error = nullptr);
        bool openInContext(const TitleInContext& context, std::string* error = nullptr);
        bool select(const std::vector<std::string>& paths, std::string* error = nullptr);
        bool runOps(const titledesigner::Json& ops, const std::string& stepName, std::string* error = nullptr);
        void setTool(titledesigner::Tool tool);
        titledesigner::Tool tool() const { return currentTool; }
        bool setPreviewAs(const std::string& mode, std::string* error = nullptr);
        bool setBackground(const titledesigner::BackdropChoice& backdrop, std::string* error = nullptr);
        bool setDevice(const titledesigner::DeviceChoice& device, std::string* error = nullptr);
        void setZoom(float zoom);
        void fit();
        void frameSelection();
        void setSearch(const std::string& query);
        bool setPanel(const std::string& panel, std::string* error = nullptr);
        bool setLibrary(const std::string& tab, const std::string& family, const std::string& query, std::string* error = nullptr);
        bool previewItem(const titledesigner::PreviewItem& item, std::string* error = nullptr);
        void clearPreview();
        bool commitPreview(std::string* error = nullptr);
        void bake();
        // Background > Game Scene: renders the scene once through its main camera (R30)
        bool snapshotScene(std::string* error = nullptr);
        // focus_window's `state` and get_title's `designer` (R78, R80)
        titledesigner::Json state() const;
        // the shown document (empty when none)
        std::filesystem::path documentPath() const;
        // the design state (MCP, tests)
        titledesigner::TitleDesignerState& designer() { return doc; }

        // the shortcut commands (§6.5): true when the window took the chord
        bool runCommand(const std::string& id);
        // drag and drop: a Project window or OS drop onto the tree / canvas
        bool dropFiles(const std::vector<std::string>& files, const std::string& underLayer, std::string* error = nullptr);

        // ---- PropertyHost ----
        titledesigner::TitleDesignerState& designerState() override { return doc; }
        bool sectionOpen(const std::string& key, bool defaultOpen) override;
        void setSectionOpen(const std::string& key, bool open) override;
        std::string flashingRow() const override;
        std::string scrollToRow() const override { return pendingScrollRow; }
        void rowScrolled() override { pendingScrollRow.clear(); }
        void openFontPicker(const std::vector<std::string>& layers) override;
        void openAddPalette(const std::string& filter) override;
        void exposeField(const titledesigner::FieldRef& target, const std::string& label) override;
        void selectNode(const titledesigner::NodeRef& node) override;
        void drawFontRow(const std::vector<std::string>& layers, bool readOnly) override;
        const jasperengine::titleart::AssetResolver* assets() override;
        void reportError(const std::string& message) override;
        bool selectedCharacters(std::string& layer, std::vector<int>& indexes) override;

    private:
        Project* project = nullptr;
        titledesigner::TitleDesignerState doc;
        titledesigner::TitleDesignerSettings settings;
        titledesigner::TitleDesignerWorkspace workspace;
        std::filesystem::path workspaceProject;
        bool windowOpen = false;
        bool focused = false;
        bool canvasFocused = false;
        bool focusRequested = false;
        bool firstShow = true;
        ImVec2 rectMin = ImVec2(0, 0), rectMax = ImVec2(0, 0);
        bool rectValid = false;
        std::string inContextPath;
        std::optional<TitleInContext> inContext;

        // live only while open (R65)
        std::unique_ptr<titledesigner::TitleCanvasView> canvas;
        std::unique_ptr<titledesigner::TitleLayerThumbnails> thumbnails;
        std::unique_ptr<titledesigner::TitleLayerThumbnails> libraryTiles;     // the starter Library's tiles
        std::unique_ptr<titledesigner::TitleStarterLibrary> starterLibrary;    // while no provider is registered
        std::unique_ptr<titledesigner::TitleTextEdit> textEdit;
        titledesigner::TitleLetterGizmo letterGizmo;       // the selected characters' gizmos (R38)
        std::unique_ptr<titledesigner::TitleGizmo2D> gizmo2d;
        std::unique_ptr<titledesigner::TitleGizmo3D> gizmo3d;
        std::unique_ptr<titledesigner::TitleHandles> handles;
        std::unique_ptr<titlefonts::FontCatalog> fontCatalog;
        std::unique_ptr<titlefonts::FontPreviewRaster> fontStrips;
        titledesigner::TransientAssetResolver transient;
        std::unique_ptr<jasperengine::titleart::geometry::TitleGeometry> geometry;
        std::unique_ptr<jasperengine::titleart::TitleTextLayout> textLayout;
        std::string bakedKey;                      // Preview As Baked: the PNG shown
        Texture bakedTexture;
        std::vector<uint8_t> bakedPixels;
        jasperengine::titleart::TitleRectI bakedFrame;
        bool bakedStale = false;
        std::map<std::string, std::pair<uint64_t, std::vector<uint8_t>>> pickCache;   // generator / adjustment alpha at 1/4 (R29)
        std::map<std::string, jasperengine::titleart::TitleRectI> pickFrames;
        std::string snapshotKey;                   // Game Scene / In Context backdrop pixels
        std::vector<uint8_t> snapshotRgba;
        int snapshotWidth = 0, snapshotHeight = 0;

        // ---- toolbar and view ----
        titledesigner::Tool currentTool = titledesigner::Tool::Rect;
        bool pivotMode = true;                      // Pivot (true) / Center (Z)
        std::map<std::string, titledesigner::TitleVec2> pivotOverride;     // per layer, the session's
        titledesigner::CanvasXform xform;
        // document space of the title on screen: xform, or the title placed on a Device's screen (R31)
        titledesigner::CanvasXform titleXform;
        ImVec2 canvasMin = ImVec2(0, 0), canvasMax = ImVec2(0, 0);
        double lastViewChange = 0.0;               // the 150 ms settle (R29)
        bool interacting = false;                  // a gesture or a view change in progress
        bool panning = false;
        ImVec2 cursorDoc = ImVec2(0, 0);
        bool cursorInCanvas = false;
        float displayScale = 1.0f;
        bool softwareRenderer = false;
        std::string notice;                        // the status bar's one-line notice
        double noticeUntil = 0.0;
        std::vector<std::string> toasts;
        double toastUntil = 0.0;
        std::string errorText;
        double errorUntil = 0.0;

        // ---- regions (R2, R3) ----
        std::string sideTab = "layers";            // layers | history (left), and the narrow column's tab
        std::string panelFocus;                    // focus_window's `panel`
        bool libraryOverlay = false;
        bool narrowPanelsOpen = false;

        // ---- Layers ----
        std::string renaming;                      // a layer id being renamed
        std::string renameBuffer;
        bool renameFocus = false;
        std::string rangeAnchor;
        std::vector<titledesigner::NodeRef> shownRows;    // the rows drawn this frame (Shift-click ranges, keys)
        std::string dragLayer;
        std::set<std::string> dragSet;

        // ---- Properties and search (R26) ----
        std::string searchText;
        bool searchFocus = false;
        bool searchEveryLayer = false;
        titledesigner::PropertyIndex index;
        uint64_t indexChange = ~0ull;
        std::string indexSelectionKey;
        double indexDirtyAt = -1.0;
        double indexIdleSince = -1.0;       // the index stale and the document still since
        // the index builds a few pages per frame (R69); the previous index serves meanwhile
        static constexpr double INDEX_FRAME_BUDGET_MS = 2.0;
        struct IndexBuild {
            bool active = false;
            std::vector<titledesigner::CollectUnit> units;
            size_t next = 0;
            uint64_t change = 0;
            std::string selection;
            titledesigner::PropertyIndex pending;
            double ms = 0.0, frameMsMax = 0.0;
            int frames = 0;
            uint64_t generation = 0;
        };
        IndexBuild indexBuild;
        int indexUnitsPerFrame = 1 << 30;   // tests cap it
        int indexUnitsLastFrame = 0;
        int indexFrames = 0;                // frames the last complete build spanned
        double indexFrameMsMax = 0.0;       // its costliest frame
        titledesigner::PropertyIndex mergedIndex;   // old + collected so far, while searching mid-build
        uint64_t mergedKey = ~0ull;
        // starts (urgent: now; else once the document settled for 300 ms) or continues the build
        void stepIndexBuild(bool urgent);
        // the index a search reads: the built one, or mid-build the new rows plus the old others
        const titledesigner::PropertyIndex& searchIndex();
        std::string indexSelection() const;
        bool indexStale() const;
        std::vector<std::string> stableOrder;      // while a hit is edited the list keeps its order
        std::string editingHit;
        double editingHitAt = 0.0;
        int highlightedHit = 0;
        size_t resultLimit = 30;
        std::string flashRow;
        double flashUntil = 0.0;
        std::string pendingScrollRow;
        std::vector<std::string> forceOpenSections;   // a landing opens the hit's sections

        // ---- canvas interaction ----
        enum class Drag : uint8_t { None, Gizmo, Gizmo3D, Handle, Marquee, Guide, NewGuide, Pan, TextSelect, Letters };
        Drag drag = Drag::None;
        ImVec2 dragStart = ImVec2(0, 0);
        int guideIndex = -1;
        bool guideVertical = true;
        std::string clickLayer;                    // root-then-drill (R29)
        ImVec2 clickSpot = ImVec2(-1e9f, -1e9f);
        int clickDepth = 0;
        double nudgeAt = -1.0;
        std::string nudgeKey;
        std::unique_ptr<titledesigner::Gizmo3DTarget> gizmo3dTarget;

        // ---- text editing (R36-R39) ----
        std::string renamePromptOld, renamePromptNew;
        double renamePromptUntil = 0.0;

        // ---- Add palette (R17) ----
        bool addOpen = false;
        bool addOpenRequested = false;
        std::string addFilter;                     // "" | "layers" | "generators" | "effects"
        std::string addQuery;
        std::string addCategory;
        int addHighlight = 0;
        bool addToParts = true;
        std::string addHovered;
        double addHoverSince = 0.0;
        ImVec2 addHoverMouse = ImVec2(0, 0);

        // ---- Library (R40-R49) ----
        std::string libraryHover;                  // hovered tile id
        double libraryHoverSince = 0.0;
        ImVec2 libraryHoverMouse = ImVec2(-1e9f, -1e9f);
        bool libraryHoverMoved = false;
        int libraryHighlight = -1;
        double lastLibraryClick = 0.0;
        std::string lastLibraryClickId;
        std::string targetKey;                     // the content key of the Library's target layer
        double targetKeySince = 0.0;
        std::string pendingTemplate;               // the "Replace the whole title?" question
        bool saveAsOpen = false;
        titledesigner::LibraryTab saveAsTab = titledesigner::LibraryTab::Looks;
        std::string randomPick;

        // ---- History (R50-R52) ----
        uint64_t historyHover = 0;
        uint64_t historyJumpEntry = 0;
        std::vector<std::string> historyInterleaved;

        // ---- lint (R53) ----
        std::vector<titledesigner::CheckFinding> findings;
        size_t checksRun = 0;
        double lintDirtyAt = -1.0;
        uint64_t lintChange = ~0ull;
        bool lintPopup = false;
        bool pixelChecksRun = false;

        // ---- external changes (R74) ----
        std::string chipText;
        uint64_t chipEntry = 0;
        double chipUntil = 0.0;

        // ---- font picker (R54-R60) ----
        bool fontPickerOpen = false;
        std::vector<std::string> fontPickerLayers;
        std::string fontQuery;
        bool fontCoversText = true;
        std::string fontCategory;
        std::string fontHovered;
        double fontHoverSince = 0.0;
        std::string fontPending;                   // a system font with a restricted licence: the question
        std::string fontPendingLayer;

        // ---- expose dialog ----
        bool exposeOpen = false;
        titledesigner::FieldRef exposeTarget;
        std::string exposeName;
        std::string exposeLabel;

        // ---- stats (R72) ----
        bool gpuTimingHeld = false;

        // ---- what the window's own work costs (R69; Stats, state().timings, the AC31 bench) ----
        double frameMs = 0.0;               // show()'s CPU time, last frame
        double frameMsSum = 0.0;            // over the last FRAME_SAMPLES frames
        double frameMsMax = 0.0;
        std::vector<double> frameSamples;
        size_t frameSampleNext = 0;
        double indexMs = 0.0;               // the last search index build
        // whole-document passes the window made (a hash, the bake status, a layer's bounds, the
        // index, the lint...): an idle frame makes none, at any layer count (R69's regression test)
        mutable uint64_t documentPasses = 0;
        double queryMs = 0.0;               // the last search query
        void showContents();

        // what the shown document's layers derive, made again when it changes, not per frame
        // (R69: a text layer's bounds lay its text out)
        struct DerivedKey {
            const void* shown = nullptr;
            uint64_t change = ~0ull;
            uint64_t assets = ~0ull;
            bool operator==(const DerivedKey& o) const { return shown == o.shown && change == o.change && assets == o.assets; }
        };
        DerivedKey derivedKey() const;
        mutable DerivedKey boundsKey;
        mutable std::map<std::string, titledesigner::TitleRect> boundsCache;
        DerivedKey hashesKey;
        jasperengine::titleart::TitleArtHashes hashesCache;
        // the outputs' bake status (the toolbar's Bake button, the Baked preview): it hashes the
        // document and reads every output's recipe, so it is made again only when the document,
        // its assets, a bake in progress or the sidecar's time change
        std::string bakeStatusKey;
        TitleBakeStatus bakeStatusCache;
        const TitleBakeStatus& bakeStatus();
        struct EmptyText {
            double x, y, size;
        };
        DerivedKey emptyTextsKey;
        std::vector<EmptyText> emptyTexts;      // visible text layers with no text (the canvas's dashed boxes)
        mutable DerivedKey contentEmptyKey;
        mutable bool contentEmptyCache = true;
        mutable std::pair<const void*, uint64_t> committedHashesKey{nullptr, ~0ull};
        mutable jasperengine::titleart::TitleArtHashes committedHashes;

        // ---- keyboard regions (R76): Tab cycles toolbar, Layers, canvas, Properties, Library ----
        enum Region : int { RegionToolbar = 0, RegionLayers, RegionCanvas, RegionProperties, RegionLibrary, RegionCount };
        int pendingRegionFocus = -1;        // Tab asked for it; its region takes the focus when drawn
        int keyboardRegion = -1;            // the region Tab reached (outlined while it has the focus)
        int focusedRegion = -1;             // last frame's region with the keyboard focus
        int focusedRegionNow = -1;
        bool regionAvailable[RegionCount] = {true, true, true, true, false};
        std::string narrowTabRequest;       // the side column's tab to select (narrow windows)

        // ---- the Library strip (R3, R40) ----
        bool libraryNarrowOpen = false;     // narrow: opened from its header bar
        bool libraryShown = false;          // its tiles were drawn this frame
        // a starter tile's document (the committed one with the item applied), made again when the
        // document or the target changes
        struct StarterTileSource {
            std::string key;
            std::shared_ptr<const jasperengine::titleart::TitleArtDoc> doc;
            std::string layer;
            jasperengine::titleart::TitleRect bounds;
            uint64_t content = 0;
            std::string failed;
        };
        std::map<std::string, StarterTileSource> starterTileSources;

        // ---- no system CJK font (the editor's fields would show boxes) ----
        const void* cjkScanDocument = nullptr;
        uint64_t cjkScanChange = ~0ull;
        bool cjkInDocument = false;
        bool cjkNoticeShown = false;        // this frame

        // test hooks
        std::function<double()> clockOverride;
        std::function<int()> systemCjkOverride;     // -1 unknown, 0 none, 1 found
        bool commitPerFrameForTesting = false;

        double now() const;
        void openFirstTime();
        void createLive();
        void destroyLive();
        void loadWorkspace();
        void saveView();
        titledesigner::DocumentView& view();
        const titledesigner::DocumentView* viewOrNull() const;
        void restoreSelection();
        void rememberSelection();
        void tick();
        void handleExternalChange();

        // regions
        void drawToolbar();
        void drawReadOnlyBanner();
        void drawStatusBar();
        void drawSideColumn(float width, float height);
        void drawLayers();
        void drawLayerRow(const titledesigner::Json& layer, int depth, const std::string& parentId);
        void drawLayerHeader();
        void drawLayerFooter();
        void layerContextMenu(const std::string& layerId);
        void drawHistory();
        void drawProperties();
        void drawPropertiesHeader();
        void drawSearch();
        void drawSearchResults();
        void drawCanvas(float width, float height);
        void drawCanvasOverlays(ImDrawList* drawList);
        void drawEmptyState(ImDrawList* drawList);
        void drawRulers(ImDrawList* drawList);
        void drawLibrary(float width, float height);
        void drawAddPalette();
        void drawFontPicker();
        void drawLintPopup();
        void drawExposeDialog();
        void drawStats();
        void drawNotices();

        // canvas
        void updateView(float width, float height);
        void requestCanvasRender();
        void handleCanvasInput();
        std::string pickLayer(ImVec2 screen, bool drill);
        titledesigner::TitleRect layerBounds(const std::string& layerId) const;
        // the shown document's per-layer hashes, made once per change (Layers, Library keys)
        const jasperengine::titleart::TitleArtHashes& shownHashes();
        void beginTextEdit(const std::string& layerId, bool selectAll);
        void endTextEdit(bool commit);
        bool textEditing() const;
        bool contentEmpty() const;
        void zoomAbout(float factor, ImVec2 screen);
        titledesigner::TitleRect stageRect() const;

        // actions (§6.3)
        bool addLayer(const std::string& type, const titledesigner::Json& values, const std::string& extra = {});
        bool deleteSelection();
        bool duplicateSelection();
        bool groupSelection();
        bool ungroupSelection();
        bool toggleClip();
        bool arrange(const std::string& how);
        bool toggleFlag(const char* member);
        bool moveLayers(const std::vector<std::string>& layers, const std::string& target, const std::string& where);
        bool copySelection(bool cut);
        bool paste();
        void nudge(double dx, double dy);
        bool applyLibraryItem(titledesigner::LibraryTab tab, const titledesigner::LibraryItem& item, bool preview);
        titledesigner::LibraryApplyOptions libraryOptions(titledesigner::LibraryTab tab) const;
        void refreshLint(bool pixels);
        void rebuildIndexIfNeeded();
        void jumpToHit(size_t i);
        void commitNamed(const titledesigner::Json& ops, const std::string& name);
        std::string libraryTargetKey() const;
        // S84's provider when registered, else the starter Library (TitleStarterLibrary)
        titledesigner::TitleLibraryProvider* library() const;
        titledesigner::LibraryTile starterTile(titledesigner::LibraryTab tab, const titledesigner::LibraryItem& item, float width, float height,
                                               bool visible);
        void drawCjkFontNotice();
        void cycleRegion(int direction);
        // at the top of a region's window: takes a focus Tab asked for, notes a focus, outlines it
        void regionFocus(int region);
        static const char* regionName(int region);
        // the Letters gizmo's drag: staged in the text edit, committed as one step on release (R38)
        void stageLetters(const titledesigner::Json& values);
        void commitLetters(const titledesigner::Json& values, const std::string& stepName);
        std::vector<std::string> fontTextOfSelection() const;
    };

}
