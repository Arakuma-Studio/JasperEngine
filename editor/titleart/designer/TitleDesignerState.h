// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md D3, D8, R8, R42, R44, R74, R75, §6.2, §6.4)
// - Purpose: the Title Designer's documents and selection. Three layers of one `.titleart`:
//   `committed` (what the file says: the shared TitleArtDocument), `working` (committed plus the
//   staged gesture's ops) and `preview` (working plus the hovered item's ops: a Look, an effect
//   of the Add palette, a font, a history version). The canvas, thumbnails and Properties show
//   shown() = preview, else working. Nothing else is ever unsaved: a gesture's last staged batch
//   is committed once on release (one named project undo step), Esc drops it.
// - Every write is a TitleArtOps batch (or a whole text for Restore This Version) through
//   TitleArtDocument, so the window, MCP's edit_title and Lua produce the same bytes (R77).
//   Previews run the same ops through TitleArtOps::apply on the working copy with the id seed
//   their commit will use, so a committed preview equals the previewed document (R44).
// - Staging replaces: stage(ops) applies `ops` to `committed`, so gestures compute absolute
//   values from their start and Esc needs no inverse.
// - Outside changes (an agent's edit_title, undo/redo, a VCS pull) reload `committed`; a staged
//   gesture is cancelled with a notice, the selection is kept by id, the preview recomputed.
// - The edit router (S85's Record mode, §6.8) rewrites ops before staging and commit.
// - Thread-safety: main thread.

#pragma once

#include "TitleDesignerTypes.h"

#include "titleart/TitleArtDocument.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::titledesigner {

    using jasperengine::titleart::TitleArtDoc;

    // something shown on the canvas without a step (R17, R42, R52, R54)
    struct PreviewItem {
        enum class Kind : uint8_t { Look, ColorScheme, Template, Effect, Generator, Layer, Font, HistoryVersion };
        Kind kind = Kind::Effect;
        std::string id;             // the item ("outer_glow", a Look id, a font path)
        std::string label;          // its name, for the step a commit adds
        Json ops = Json::array();   // the op batch (every kind but HistoryVersion)
        std::string text;           // HistoryVersion: the version's whole document text
        uint64_t historyEntry = 0;  // HistoryVersion: the undo entry it comes from
        std::string stepName;       // the step a commit adds ("Add Effect Outer Glow to Title")
    };
    const char* previewKindName(PreviewItem::Kind kind);

    // what an outside change did, for the window's chip and notices (R74)
    struct ExternalChange {
        TitleArtChangeOrigin origin = TitleArtChangeOrigin::Disk;
        bool cancelledGesture = false;
        uint64_t revision = 0;
    };

    class TitleDesignerState {
    public:
        explicit TitleDesignerState(Project* project);
        ~TitleDesignerState();
        TitleDesignerState(const TitleDesignerState&) = delete;
        TitleDesignerState& operator=(const TitleDesignerState&) = delete;

        bool open(const std::filesystem::path& file, std::string* error);
        void close();
        bool isOpen() const { return doc != nullptr; }
        const std::shared_ptr<TitleArtDocument>& document() const { return doc; }
        Project* project() const { return owner; }
        // false for a document S80 refuses to edit (R6); readOnlyReason() says why
        bool editable() const;
        std::string readOnlyReason() const;

        const TitleArtDoc& committed() const;
        const TitleArtDoc& working() const { return workingDoc; }
        const TitleArtDoc& shown() const { return previewDoc ? *previewDoc : workingDoc; }
        // an immutable snapshot of shown() for renders, made once per change
        std::shared_ptr<const TitleArtDoc> shownSnapshot() const;
        // rises with every change of shown()
        uint64_t changeCount() const { return changes; }

        // ---- gestures (D3) ----
        void beginGesture(const std::string& stepName, const std::string& mergeKey = {});
        bool gestureActive() const { return gesture; }
        const std::string& gestureName() const { return gestureStep; }
        void setGestureName(const std::string& name) { gestureStep = name; }
        // applies `ops` to committed as the gesture's batch (replacing the previous one)
        bool stage(const Json& ops, std::string* error);
        bool hasStaged() const { return !staged.empty(); }
        // commits the staged batch as one step (nothing staged: no step); ends the gesture
        bool endGesture(std::string* error);
        // Esc: working = committed, no step
        void cancelGesture();

        // ---- immediate commits ----
        bool commit(const Json& ops, const std::string& stepName, std::string* error, const std::string& mergeKey = {}, uint64_t idSeed = 0);
        bool commitText(const std::string& text, const std::string& stepName, std::string* error);
        // the last commit's result (new ids, changes, problems)
        const TitleOpsResult& lastResult() const { return result; }

        // ---- previews (R42) ----
        bool setPreview(const PreviewItem& item, std::string* error);
        void clearPreview();
        bool previewActive() const { return previewItem.has_value(); }
        const PreviewItem* preview() const { return previewItem ? &*previewItem : nullptr; }
        // commits the preview's ops (or text) as its step, with the seed the preview used
        bool commitPreview(std::string* error);

        // ---- S85's Record mode (§6.8) ----
        void setEditRouter(std::function<Json(const Json&)> router) { editRouter = std::move(router); }

        // ---- selection (view state, D8) ----
        const std::vector<NodeRef>& selection() const { return selected; }
        // the node Properties shows (the last selected), the document when nothing is
        NodeRef primary() const;
        void select(std::vector<NodeRef> nodes);
        void selectOne(const NodeRef& node) { select({node}); }
        void toggle(const NodeRef& node);
        bool isSelected(const NodeRef& node) const;
        // the layers of the selection (a part, effect or mask counts its layer), in selection order
        std::vector<std::string> selectedLayers() const;
        // Esc: effect -> part -> layer -> group -> document (R12)
        void selectParent();

        // an outside change since the last call (R74)
        std::optional<ExternalChange> takeExternalChange();

        // test hooks
        void setCommitPerStageForTesting(bool on) { commitPerStage = on; }

    private:
        Project* owner = nullptr;
        std::shared_ptr<TitleArtDocument> doc;
        int listener = 0;
        TitleArtDoc emptyDoc;
        TitleArtDoc workingDoc;
        std::shared_ptr<TitleArtDoc> previewDoc;
        std::optional<PreviewItem> previewItem;
        uint64_t previewSeed = 0;
        mutable std::shared_ptr<const TitleArtDoc> snapshot;
        mutable uint64_t snapshotChange = ~0ull;
        uint64_t changes = 1;
        bool gesture = false;
        std::string gestureStep;
        std::string gestureMergeKey;
        Json staged = Json::array();
        bool committing = false;
        bool commitPerStage = false;
        TitleOpsResult result;
        std::function<Json(const Json&)> editRouter;
        std::vector<NodeRef> selected;
        std::optional<ExternalChange> external;

        void documentChanged(TitleArtChangeOrigin origin);
        void rebuild();
        void rebuildPreview();
        void pruneSelection();
        Json routed(const Json& ops) const;
    };

    // ---- paths (S80 §6.2 addressing; focus_window's `select`) ----

    // "Logo/Title", "Spark#2" for a layer id ("" when missing)
    std::string layerPathOf(const TitleArtDoc& doc, const std::string& layerId);
    // a node's path: "" (document), "Title", "Title/rim#1", "Title/face", "Title/fx:outer_glow",
    // "Title/rim#0/fx:outer_glow", "Title/mask#0"
    std::string nodePath(const TitleArtDoc& doc, const NodeRef& node);
    // the node of a path; false with `error` naming what failed and the valid names there
    bool nodeFromPath(const TitleArtDoc& doc, const std::string& path, NodeRef& out, std::string* error);
    // the layer object of an id (null when missing)
    const Json* layerById(const TitleArtDoc& doc, const std::string& layerId);
    // the parent layer id of a layer ("" at the top level)
    std::string parentLayerOf(const TitleArtDoc& doc, const std::string& layerId);
    // the part key of rim i of a layer ("rim:<id>")
    std::string rimPartKey(const Json& layer, size_t index);
    // a part key's label: "Face", "Rim 2", "Bevel", "Side", "Band 1"
    std::string partLabel(const Json& layer, const std::string& partKey);
    // the long field-path prefix of an effect node ("effects[2]" or "part_effects.rim:ab12cd34[0]"),
    // "" when it is missing
    std::string effectPathOf(const Json& layer, const NodeRef& node);
    // the effect entry of a node (null when missing)
    const Json* effectOf(const Json& layer, const NodeRef& node);
    // a mask node's index (-1 when missing)
    int maskIndexOf(const Json& layer, const std::string& maskId);

}
