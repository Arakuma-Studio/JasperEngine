// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S81-title-art-runtime.md R11, R32, R52, R53, R55)
// - Purpose: what an export ships of Title Art at runtime. The plan reads the shipped scenes and
//   prefabs (ModuleUsageDetector's contents): every enabled Title Art Renderer, its mode, its
//   instance values and how large its companion shows it. Baked titles ship their PNGs and
//   sidecar; Realtime and Glyph Atlas titles also ship their .titleart (TitleArtExport::isNeeded)
//   and, with Prebake Initial State, one image per distinct initial state, rendered here in the
//   editor and deduplicated by its key (titleArtPrebakePath).
// - The prebake key is the runtime's (TitleArtRuntime::prebakeKeyOf): the document's content,
//   the instance values, the resolution (from the companion's size in reference units, R11) and
//   the bake frame; a game whose first frame has that state shows the image at once.
// - Thread-safety: main thread (renders through TitleArtRenderHost).

#pragma once

#include "component/TitleArtRendererComponent.h"
#include "subsystem/TitleArtRuntime.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/TitleArtSettings.h"
#include "util/TitleArtBakeInfo.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    struct TitleArtGlyphPrebakeWork;

    struct TitleArtShippedInstance {
        std::string where;                  // "scene Main", "prefab Coin"
        std::string object;                 // the GameObject's name
        std::string title;                  // project-relative .titleart
        TitleArtRenderMode mode = TitleArtRenderMode::BAKED;
        std::vector<TitleArtOverride> overrides;
        std::string additionalCharacters;
        bool prebake = true;
        bool glow = false;
        int atlasPageSize = 1024;           // Glyph Atlas
        int atlasMaxPages = 4;
        float resolution = 1.0f;            // as TitleArtSystem::displayResolution gives it
        TitleArtRectI region;               // the bake frame
        std::string qualityTier = "auto";   // S86 R37 (Realtime 3D)
        std::vector<std::string> runtimeEnvironments;   // S86 R26: environments the export cooks besides the resolved ones
    };

    struct TitleArtExportPlan {
        std::vector<TitleArtShippedInstance> instances;
        std::set<std::string> runtimeTitles;    // project-relative: Realtime or Glyph Atlas somewhere
        std::set<std::string> bakedTitles;      // project-relative: Baked only
    };

    // the shipped titles of a project (its scenes and prefabs)
    TitleArtExportPlan planTitleArtExport(Project* project);

    // the project's settings a build needs (TITLE_ART_RUNTIME_SETTINGS_PATH's text): Frame Budget,
    // Release Delay, Cache Budget and Reference Height. The editor-only ones stay out (Edit mode's
    // budget, Simulate Export Characters, Auto Bake, Stale Bakes On Export)
    std::string titleArtRuntimeSettingsText(const TitleArtProjectSettings& settings);
    // writes it into the build's assets when a Realtime or Glyph Atlas title ships; false with `error`
    bool writeTitleArtRuntimeSettings(Project* project, const TitleArtExportPlan& plan, const std::filesystem::path& assetsDst, std::string& error);

    struct TitleArtPrebakeResult {
        bool ok = true;
        std::string error;
        std::vector<std::string> written;       // project-relative files written into the build's assets
        std::vector<std::string> warnings;      // W-TITLEART-PREBAKE-COUNT
        size_t bytes = 0;
        size_t glyphCells = 0;                  // cells of the prebaked glyph atlases (R32)
        // a render met a Title Art program still being built: call again on a later frame with this
        // result, which renders only what is left (S80 Deviation 61)
        bool notReady = false;
        std::map<uint64_t, std::string> keys;   // state key -> file written (dedup by state, R11)
        std::set<size_t> glyphsDone;            // instances whose glyph atlas was prebaked
        std::set<size_t> instancesDone;
        // the glyph atlases being prebaked, by instance: a slice per call (drop them on the main thread)
        std::map<size_t, std::shared_ptr<TitleArtGlyphPrebakeWork>> glyphWork;
    };
    // renders and writes the prebaked initial states of the plan's Realtime and Glyph Atlas
    // instances into `assetsDst` (the build's assets root, mirroring the project's paths below
    // `assetsPrefix`), and the Glyph Atlas instances' atlases (R32). The keys come from the build's
    // copies of the documents (the player hashes those: run it after the fonts' relink). A 3D
    // title prebakes once per S86 tier it renders at on `platforms` (its state hash holds the tier)
    void writeTitleArtPrebakes(Project* project, const TitleArtExportPlan& plan, const std::filesystem::path& assetsDst,
                               const std::vector<std::string>& platforms, TitleArtPrebakeResult& result);
    // R32 for one Glyph Atlas instance (the plan's `index`): its initial text and each digit at
    // every digit place in it render in the editor (at most 1,024 cells); the atlas's pages, plates
    // and manifest ship as <title>.glyphs-<style>.json beside the title. Each call renders a slice
    // and returns false while there is more (call again on a later frame), true once it is written
    // or a warning says why not
    bool writeGlyphPrebake(Project* project, size_t index, const TitleArtShippedInstance& instance, std::shared_ptr<const titleart::TitleArtDoc> doc,
                           uint64_t shippedHash, const titleart::TitleArtLiveState& state, const std::filesystem::path& assetsDst,
                           const std::string& assetsPrefix, TitleArtPrebakeResult& result);

}
