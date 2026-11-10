// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R7-R17, R29, R30, R44, §6.4, §6.5, D1, D4)
// - Purpose: the one apply module of the library. The ops `apply_look`, `apply_color_scheme`,
//   `apply_template`, `save_look`, `save_color_scheme`, `save_template`, `copy_library_item` and
//   `delete_library_item` (S80's reserved names, registered here into TitleArtOps) are what the
//   Title Designer, MCP's edit_title, the previews, the sheets, the thumbnail baker and the gate
//   all run; nothing applies a library item another way.
// - The keep-and-replace contract (§6.4) is the code of applyLook / applyTemplate below and of
//   titleart::applyColorScheme (the engine half). Applying copies data into the document and
//   records `look {id, color_scheme, owner}`; there is no live link (D4).
// - Px parameters of effects and generators scale by k = layer size / ref_size; Vec2 params with a
//   point gizmo are stored relative to the layer origin and land at position + k x value.
// - Files: a pack font or environment the document needs is planned as a TitleOpFile of the
//   batch's own undo step (TitleLibraryImports); a project library item likewise; a personal one is
//   written after the step (outside the project). A preview (R11) runs the same ops on a copy and
//   writes nothing: the pack files it reads are listed for a TransientAssetResolver.
// - Determinism (R10): new ids come from the batch's id generator; nothing renders from them.
// - Thread-safety: main thread.

#pragma once

#include "TitleLibrary.h"
#include "titleart/TitleArtOps.h"

#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::titlelibrary {

    // registers the eight ops and the batch finisher keeping `color_schemes` (R12); idempotent
    void registerTitleLibraryOps();

    // R11: `doc` with `ops` (one op or a batch) applied, through TitleArtOps, nothing imported or
    // written; `transient` receives assets-relative path -> pack file for the fonts and maps the
    // copy names but the project lacks. False with `error` when the batch fails.
    bool preview(const titleart::TitleArtDoc& doc, const Json& ops, titleart::TitleArtDoc& out,
                 std::map<std::string, std::string>* transient = nullptr, std::string* error = nullptr);

    struct CaptureOptions {
        LocalizedText name;
        std::string id;                         // the item's ref ("project/<slug>", "user/<slug>")
        std::string family = "custom";
        std::vector<std::string> tags;
        bool includeFont = true;
    };
    // a referenced asset a captured look carries in <slug>.files/
    struct CapturedAsset {
        std::string key;                        // "a1", ...
        std::string assetPath;                  // the project's assets-relative path
        std::string file;                       // the name in <slug>.files/
    };
    // §6.5.4: the look of layer `layerId` (the inverse of apply_look); false with `error`
    bool captureLook(const titleart::TitleArtDoc& doc, const std::string& layerId, const CaptureOptions& options, TitleLook& out,
                     std::vector<CapturedAsset>* assets, std::string* error);
    // S87's Photoshop styles: what a captured look holds, from a converted style instead of a layer
    struct StyleSource {
        Json style = Json::object();                    // the 2D style
        Json effects = Json::array();                   // S80's effect entries
        Json assets = Json::array();                    // {key, file, sha256}: files in <slug>.files/
        Json provenance;                                // the look's `provenance` (null: none)
        LocalizedText description;
    };
    // §6.5.4's look from a style (the identity of captureLook, canonical and sparse)
    bool lookFromStyle(const StyleSource& style, double refSize, const CaptureOptions& options, TitleLook& out, std::string* error);
    // R14: a scheme from a layer's colours by role (or the document's first styled layer)
    bool extractColorScheme(const titleart::TitleArtDoc& doc, const std::string& layerId, const LocalizedText& name,
                            const std::string& id, titleart::ColorScheme& out, std::string* error);

    // the ops a Library tile commits for an item (S82's provider): one apply op per target layer
    Json applyOpsFor(LibraryKind kind, const std::string& ref, const std::vector<std::string>& layers, bool applyFont, bool keepColorScheme);

    // R44: a batch of library ops only (save_*, copy_library_item, delete_library_item needs no
    // document): run against an empty document, its project files written (or removed) as one
    // undo step named by the batch, its personal files after it
    bool isLibraryOnlyBatch(const Json& ops);
    // the project library batches commit into (the open project, kept by the project hook)
    void setCurrentProject(Project* project);
    Project* currentProject();
    TitleOpsResult runLibraryBatch(Project* project, const Json& ops);

    // tests (AC3's red control): apply_look loses the layer's masks
    void setDropMasksForTesting(bool drop);

}
