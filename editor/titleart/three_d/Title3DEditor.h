// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R2, R52, R58, R59, R61)
// - Purpose: S86's editor registrations: the `title_art` ops (set_dimension, add_band,
//   remove_band, move_band, apply_material, fit_to_tier) through TitleArtOps::registerOp, so the
//   Title Designer and edit_title share them, and the 3d_* checks of check_title through
//   TitleArtChecks::registerCheck. Idempotent; called with S80's registrations.
// - Undo names (one step per op): "Set Dimension 3D", "Add Band", "Move Band", "Remove Band",
//   "Apply Material Gold", "Fit To Tier Low".
// - Band presets of Add Band: bright_ring, dark_step, plate, tube_rim.
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/three_d/TitleMesh.h"

#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::titleart {
    class AssetResolver;
    struct TitleHdri;
}

namespace jasperengine::editor {

    void registerTitle3DEditor();
    // a bundled HDRI of `dir` (its manifest.json names the slug's file), linear half floats
    std::shared_ptr<const titleart::TitleHdri> loadBundledTitleHdri(const std::string& slug, const std::filesystem::path& dir);

    // the presets add_band takes, and their band values
    const std::vector<std::string>& title3DBandPresetNames();
    titleart::Json title3DBandPreset(const std::string& name, double letterDepth);

    // R61's reductions of a layer for a tier, as `set` values (path -> value) with why
    struct Title3DFix {
        std::string path;
        titleart::Json value;
        std::string why;
    };
    std::vector<Title3DFix> title3DTierFixes(const titleart::Json& layer, const std::string& tier);

    // stats_3d (R60, R61): per 3D layer of `doc` its mesh at `tier` (triangles, vertices, solids,
    // build_ms, k-independent) and whether it fits each Realtime tier; an empty array without 3D
    titleart::Json title3DStats(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets, titleart::TitleTier tier);
    // get_title's 3D view of a layer: its effective style_3d and the lighting it renders with,
    // each value with its source (document, layer, part)
    titleart::Json title3DLayerInfo(const titleart::Json& layer, const titleart::TitleArtDoc& doc);
    // R38: the cooked meshes (.titlemesh bytes) of `doc`'s 3D layers at a Realtime tier, by layer
    // id: what the exporter writes for a Realtime 3D title whose text cannot change, so the player
    // draws them without the mesher (RenderRequest::cookedMeshes3D); built as a render at `pxScale`
    // builds them (the layer's placement scale times it: bevel segments and inflate steps follow
    // the output scale); `layers` limits them to those ids (null: every 3D layer); false with `error`
    bool cookTitleMeshes(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets, titleart::TitleTier tier, double pxScale,
                         const std::set<std::string>* layers, std::map<std::string, std::vector<uint8_t>>& out, std::string& error);

}
