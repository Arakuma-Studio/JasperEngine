// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R26, R65, AC14)
// - Purpose: the cook of a title environment for a Realtime tier and a platform family: the
//   builder's CPU twin (buildTitleEnvironmentCpu) at the tier's face size with every prefiltered
//   level, encoded by the texture cook's HDR row (TextureFormatSelector: BC6H desktop, RGB9E5
//   mobile and android_etc2, UASTC HDR web) into a KTX2 cube, stamped with JE.logical
//   "title_environment" and JE.titleart.environment / .face / .builder. What the exporter writes
//   for each environment a Realtime title can show, and what the player hands the renderer
//   (RenderRequest::cookedEnvironments3D).
// - Cache: by (preset, face, family, builder version, encoder version) in memory; a hit encodes
//   nothing (titleEnvironmentCookStats).
// - Thread-safety: any thread (the CPU twin runs on the job system); the cache is locked.

#pragma once

#include "texture/TextureImportSettings.h"
#include "titleart/three_d/TitleMesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    struct TitleEnvironmentCookStats {
        uint64_t encodes = 0;
        uint64_t hits = 0;
    };

    // the KTX2 cube of environment `preset` (a procedural, softbox or photo preset) for `tier` on
    // `family`; false with `error` (an unknown preset, a photo whose HDRI is not here)
    bool cookTitleEnvironment(const std::string& preset, titleart::TitleTier tier, TextureFamily family, std::vector<uint8_t>& ktx2,
                              std::string& error);
    TitleEnvironmentCookStats titleEnvironmentCookStats();

}
