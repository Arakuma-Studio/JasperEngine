// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md §6.8, §6.6, R40-R49)
// - Purpose: how later specs plug into the Title Designer without editing it: panels (S85's
//   Animation strip), Properties sections (TitlePages.h addSection: S81's Runtime, S86's 3D, S87's
//   Languages), toolbar items (S87's language switcher, S85's Record indicator), the edit router
//   (S85's Record), the time source (S85's playhead), Preview As modes (S81's Glyph Atlas, S86's
//   tiers, S85's Exact), lint check sources (S85's animation checks) and the Library provider
//   (S84's Looks, Color Schemes and Templates, its tile previews, favorites and recents).
// - A spec compiled out registers nothing: its UI does not exist and costs nothing. Without a
//   Library provider the window's Library shows S82's starter Looks and Color Schemes
//   (TitleStarterLibrary), and its empty state offers New Text instead of template cards
//   (S82 Deviations 2 and 16).
// - Registration happens at startup (the spec's editor registration function) or from tests;
//   reads are from the main thread.

#pragma once

#include "TitleDesignerTypes.h"
#include "titleart/TitleArtChecks.h"

#include "imgui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::titledesigner {

    class TitleDesignerState;
    struct Gizmo3DTarget;

    // ---- panels and toolbar ----
    struct PanelDef {
        enum class Region : uint8_t { Bottom, Right };
        std::string id;
        std::string title;
        Region region = Region::Bottom;
        std::function<void(TitleDesignerState& state)> draw;
        // shown by default for this document (S85: once it has animations)
        std::function<bool(const TitleDesignerState& state)> visibleByDefault;
    };
    void addPanel(PanelDef panel);
    void removePanel(const std::string& id);
    const std::vector<PanelDef>& panels();

    struct ToolbarItemDef {
        std::string id;
        std::function<void(TitleDesignerState& state)> draw;
        bool overflowOnly = false;
    };
    void addToolbarItem(ToolbarItemDef item);
    void removeToolbarItem(const std::string& id);
    const std::vector<ToolbarItemDef>& toolbarItems();

    // ---- edits and time (S85) ----
    using EditRouter = std::function<Json(const Json& ops)>;
    void setEditRouter(EditRouter router);
    const EditRouter& editRouter();
    using TimeSource = std::function<double()>;
    void setTimeSource(TimeSource source);
    const TimeSource& timeSource();

    // ---- the canvas pose (S85's Animation panel): while `active`, the panel draws the sampled
    // pose over the backdrop instead of the document's render (the canvas requests no render) ----
    struct CanvasPoseSource {
        std::function<bool(const TitleDesignerState& state)> active;
        std::function<void(TitleDesignerState& state, ImDrawList* drawList, ImVec2 min, ImVec2 max, const CanvasXform& xform)> draw;
    };
    void setCanvasPoseSource(CanvasPoseSource source);
    const CanvasPoseSource& canvasPoseSource();

    // ---- the 3D gizmo (R35): S86 supplies the target of a Dimension 3D layer or letter ----
    // the characters selected in the canvas text editor (R38): their indexes and the document
    // point of their em cells' centre
    struct Gizmo3DLetters {
        std::vector<int> indexes;
        TitleVec2 centre;
    };
    // `letters` null: the layer's target; else the target of those characters of the layer
    using Gizmo3DProvider = std::function<bool(const TitleDesignerState& state, const std::string& layerId, const Gizmo3DLetters* letters,
                                               Gizmo3DTarget& out)>;
    void setGizmo3DProvider(Gizmo3DProvider provider);

    // drawn over the canvas every frame, for a highlight the document does not store (S86 R52's
    // band outline)
    using CanvasOverlay = std::function<void(const TitleDesignerState&, ImDrawList*, const CanvasXform&)>;
    void addCanvasOverlay(const std::string& id, CanvasOverlay overlay);
    void runCanvasOverlays(const TitleDesignerState& state, ImDrawList* drawList, const CanvasXform& xform);
    const Gizmo3DProvider& gizmo3DProvider();

    // ---- Preview As (R4, R28) ----
    struct PreviewModeDef {
        std::string id;                 // "glyph_atlas", "realtime_tier_mobile", "exact"
        std::string label;              // "Glyph Atlas"
        std::string note;               // under the canvas ("Cross-glyph effects are drawn per glyph")
        // draws the canvas image for the committed document into the view's rectangle; returns
        // false while it has nothing to show (the canvas shows the checker and a spinner)
        std::function<bool(TitleDesignerState& state, ImDrawList* drawList, ImVec2 min, ImVec2 max, const CanvasXform& xform)> draw;
        std::function<void()> close;    // the mode was left (free its renderer)
        // a mode without `draw`: the canvas renders the working document as usual, at this
        // RenderRequest::tier (S86's Realtime tiers: TitleTier High 1, Medium 2, Low 3)
        int renderTier = 0;
    };
    void addPreviewMode(PreviewModeDef mode);
    void removePreviewMode(const std::string& id);
    const std::vector<PreviewModeDef>& previewModes();

    // ---- work in the engine's render phase ----
    // a mode's work that must run in the engine's render phase this frame (S81's Glyph Atlas title
    // runs its scheduler there while no scene draws); one-shot, drained by the canvas view
    void requestEngineFrame(std::function<void()> work);
    void runEngineFrameRequests();
    // a request waits or the last drain ran one: the editor keeps drawing, or work asked for from
    // a window would wait for the next mouse move
    bool engineFramesWanted();

    // ---- lint (R53) ----
    struct CheckFinding {
        std::string id;                 // the check
        std::string status;             // "WARN", "FAIL"
        std::string message;
        std::string rule;               // the rule's name and its cited source (the ⓘ)
        std::string measured, threshold;
        std::string layer;              // the layer it is about ("" for the document)
        jasperengine::titleart::TitleRectI at;   // the crop to look at (canvas px), empty when none
        Json fix;                       // ops that fix it (null when none)
        std::string fixName;            // "Style Fix: Contrast"
    };
    using CheckSource = std::function<std::vector<CheckFinding>(const TitleDesignerState& state)>;
    void addCheckSource(const std::string& id, CheckSource source);
    void removeCheckSource(const std::string& id);
    std::vector<CheckFinding> runCheckSources(const TitleDesignerState& state);

    // ---- the Library (S84) ----
    enum class LibraryTab : uint8_t { Looks, ColorSchemes, Templates };
    const char* libraryTabName(LibraryTab tab);         // "looks", "color_schemes", "templates"
    bool libraryTabFromName(const std::string& name, LibraryTab& out);

    struct LibraryItem {
        std::string id;
        std::string name;
        std::string family;             // one of S84 R23's families
        std::string dimension;          // "2d", "3d" or "" (both)
        bool user = false;              // a project or personal item (the "user" badge)
        bool featured = false;          // templates: the empty state's six
        std::string convertsTo;         // "3d" / "2d" when applying converts the layer (S86)
        std::string disabledReason;     // the target cannot take it
        std::vector<std::string> terms; // searchable words (English and Japanese)
    };

    struct LibraryApplyOptions {
        std::vector<std::string> layers;    // the targets (empty: the whole title / a new text)
        bool applyFont = false;             // S84 R42
        bool keepColorScheme = true;
        bool wholeTitle = false;            // Color Schemes: Whole Title (R45)
    };

    // a tile's picture: a texture ImGui draws, or why there is none yet
    struct LibraryTile {
        ImTextureID texture = 0;
        float width = 0.0f, height = 0.0f; // the picture's size in pixels
        bool live = false;              // false: S84's baked first-paint thumbnail (drawn at 50 %, R47)
        bool pending = false;           // a render is on its way (spinner)
        std::string failed;             // "Render failed" reason (not retried for this key)
    };

    class TitleLibraryProvider {
    public:
        virtual ~TitleLibraryProvider() = default;
        virtual std::vector<LibraryItem> items(LibraryTab tab) = 0;
        virtual std::vector<std::string> families() = 0;
        // the ops a click commits and a hover previews (the same S84 function: R44)
        virtual Json applyOps(LibraryTab tab, const LibraryItem& item, const LibraryApplyOptions& options) = 0;
        // "Apply Look Chrome Gold", "Apply Color Scheme Red to Whole Title"
        virtual std::string stepName(LibraryTab tab, const LibraryItem& item, const LibraryApplyOptions& options) = 0;
        // the tile of an item for the target's content key (S84's TitleLibraryPreviews); asked
        // every frame for visible tiles and the next screen only (R46)
        virtual LibraryTile tile(LibraryTab tab, const LibraryItem& item, const std::string& targetKey, float width, float height, bool visible) = 0;
        // after a template: the layer of its `title` placeholder (R5), "" when none
        virtual std::string titleLayer(const jasperengine::titleart::TitleArtDoc& doc) { (void)doc; return {}; }
        // favorites and recents (S84 R18: personal editor settings)
        virtual bool isFavorite(LibraryTab tab, const std::string& id) { (void)tab; (void)id; return false; }
        virtual void setFavorite(LibraryTab tab, const std::string& id, bool favorite) { (void)tab; (void)id; (void)favorite; }
        virtual std::vector<std::string> recents(LibraryTab tab) { (void)tab; return {}; }
        virtual void recordApply(LibraryTab tab, const std::string& id) { (void)tab; (void)id; }
        // a toast naming the structure an apply added ("added a Group and 2 Generator layers", R75)
        virtual std::string structureNote(const jasperengine::titleart::TitleArtDoc& before, const jasperengine::titleart::TitleArtDoc& after) {
            (void)before;
            (void)after;
            return {};
        }
        // whether the menu offers Save As Look / Color Scheme / Template (S84's provider does)
        virtual bool savesItems() const { return true; }
        // the menu's Save As dialogs (S84 R42): draws the dialog's fields, true when it closed
        virtual bool drawSaveAs(LibraryTab tab, TitleDesignerState& state) { (void)tab; (void)state; return true; }
        // user items: rename and delete (S84 R16-R17)
        virtual bool renameItem(LibraryTab tab, const std::string& id, const std::string& name, std::string* error) {
            (void)tab; (void)id; (void)name;
            if (error) *error = "not supported";
            return false;
        }
        virtual bool deleteItem(LibraryTab tab, const std::string& id, std::string* error) {
            (void)tab; (void)id;
            if (error) *error = "not supported";
            return false;
        }
        // library files dropped on the panel (.titlelook, .titlecolors, .titletemplate)
        virtual bool importFiles(const std::vector<std::string>& files, std::string* error) {
            (void)files;
            if (error) *error = "not supported";
            return false;
        }
        // drop every request of a window (closed) and re-request after a GPU loss (R48)
        virtual void cancelAll() {}
        virtual void deviceLost() {}
    };
    void setLibraryProvider(std::shared_ptr<TitleLibraryProvider> provider);
    TitleLibraryProvider* libraryProvider();

    // ---- the editor's own extensions ----
    // registers the hooks of the title specs built into this editor (S81's Runtime section and
    // Glyph Atlas mode, S83's checks, S86's 3D rows, pickers, tiers and gizmo target); the window
    // calls it once (TitleDesignerExtensions.cpp)
    void registerTitleDesignerExtensions();

}
