// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R26, R37, R38, R65; S81 R51, R52)
// - Purpose: what an export ships of its Realtime 3D titles beside their documents
//   (three_d/TitleCooked3D.h names the files and the index): for every 3D layer whose geometry no
//   runtime value can change, its .titlemesh at each Realtime tier (High, Medium, Low: the tier
//   resolves on the device, through S56's tiers, Lua's qualityTier and the backend), built at the
//   largest resolution its instances show it; for every texture family the export ships, the KTX2
//   cube of each environment its titles can show (their resolved environments and the components'
//   Runtime Environments) at the best tier any of them asks of that family; and the project's
//   Realtime 3D Quality per platform. The player hands them to the renderer
//   (RenderRequest::cookedMeshes3D, cookedEnvironments3D), so a build without the mesher draws
//   fixed 3D text and no player needs an HDRI.
// - The tier a title renders at on a platform (the prebakes' too): its component's Quality Tier,
//   else the project's for the platform; High draws as Medium on WebGL2 (titleArt3DTierFor).
// - Thread-safety: main thread (meshes are built through the editor's resolver).

#pragma once

#include "texture/TextureImportSettings.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/runtime/TitleArtRuntimeExport.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct Title3DCookResult {
        bool ok = true;
        std::string error;
        std::vector<std::string> written;       // build-relative files (je-titleart/...)
        std::vector<std::string> warnings;      // W-TITLEART-3D-ENV, W-TITLEART-3D-MESH
        size_t bytes = 0;
        size_t meshes = 0, environments = 0;
    };

    // writes je-titleart/ into `assetsDst` (the build's assets root) for the plan's Realtime 3D
    // titles on `platforms` (buildPlatformName's names) and `families`; removes it when no shipped
    // title is 3D
    Title3DCookResult cookTitleArt3D(Project* project, const TitleArtExportPlan& plan, const std::filesystem::path& assetsDst,
                                     const std::vector<std::string>& platforms, const std::vector<TextureFamily>& families);

    // the S86 tier (1 high, 2 medium, 3 low) `qualityTier` renders at on `platform` with the
    // project's Realtime 3D Quality
    int titleArt3DTierFor(Project* project, const std::string& qualityTier, const std::string& platform);
    // the distinct tiers an instance renders at on the export's platforms: {0} for a document
    // without 3D layers (its prebake has no tier)
    std::vector<int> titleArt3DPrebakeTiers(Project* project, const titleart::TitleArtDoc& doc, const std::string& qualityTier,
                                            const std::vector<std::string>& platforms);
    // the environment presets a document's 3D layers light with (scene, layers, parts), sorted
    std::vector<std::string> titleArt3DEnvironments(const titleart::TitleArtDoc& doc);

}
