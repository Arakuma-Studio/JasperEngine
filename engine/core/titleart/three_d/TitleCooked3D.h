// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R26, R37, R38, R65)
// - Purpose: what an export cooks for the Realtime 3D titles it ships and how a game finds it.
//   The export writes assets/je-titleart/index.json (version 1):
//     {"version": 1,
//      "realtime_3d_quality": {"windows": "high", "linux": "high", "macos": "high", "ios": "medium",
//                              "android": "low", "web": "low"},
//      "environments": {"<texture family>": {"<preset>": "environments/<family>/<preset>.ktx2"}},
//      "titles": {"<title path>": {"meshes": {"<layer id>": {"high": "meshes/<file>.titlemesh",
//                                                             "medium": ..., "low": ...}},
//                                  "environments": ["studio", ...]}}}
//   (paths inside are relative to je-titleart/; a title path is the component's, as
//   TitleArtRuntime::create receives it). Meshes are written for the 3D layers whose geometry
//   cannot change at runtime (titleArt3DFixedGeometry), one per Realtime tier, so whatever tier a
//   device resolves to draws without the mesher; environments for each family the export ships,
//   the cube of the best tier any shipped title asks of that family (a cube serves every tier:
//   the roughness-to-level mapping does not depend on its face).
// - A game reads the index once, at its first 3D title; the editor has none (its titles mesh and
//   build their cubes). A title's source loads its meshes per tier on first use and shares the
//   environment cubes with every other title (one copy each).
// - Thread-safety: main thread.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/three_d/TitleMesh.h"

#include <memory>
#include <string>

namespace jasperengine::titleart {

    constexpr const char* TITLE_COOKED_DIR = "je-titleart";
    constexpr const char* TITLE_COOKED_INDEX_FILE = "index.json";
    constexpr int TITLE_COOKED_INDEX_VERSION = 1;

    // the files an export writes, relative to je-titleart/
    JASPERENGINE_API std::string titleCookedMeshFile(const std::string& titlePath, const std::string& layerId, TitleTier tier);
    JASPERENGINE_API std::string titleCookedEnvironmentFile(const std::string& family, const std::string& preset);

    // A 3D layer whose geometry no runtime value can change: no Exposed parameter targets its text,
    // font or any field the mesher reads (only materials, lighting, transforms, skew, glint, AO,
    // opacity and the like, and the per-letter transforms of letters already rigged), so the
    // export's cooked mesh is the one a mesher would build. False with `why` (one phrase naming the
    // parameter) otherwise. ModuleUsage's _MESHER switch and the exporter's cook share it.
    JASPERENGINE_API bool titleArt3DFixedGeometry(const Json& root, const Json& layer, std::string* why);

    // the build's cooked data of the title at `titlePath`, null when its export cooked none for it
    JASPERENGINE_API std::shared_ptr<TitleCooked3DSource> titleCooked3DFor(const std::string& titlePath);
    // the project's Realtime 3D Quality for `platform` (Engine::Platform as an int) as the export
    // wrote it; false without an index
    JASPERENGINE_API bool titleCookedProjectTier(int platform, TitleTier& out);
    // tests: read the index again at the next use, from `root`/je-titleart ("" : the assets')
    JASPERENGINE_API void resetTitleCookedIndexForTesting(const std::string& root = std::string());

}
