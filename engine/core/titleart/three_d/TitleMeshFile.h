// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R38, R65, §6.3)
// - Purpose: `.titlemesh`, the cooked mesh of a Realtime 3D title whose text cannot change, so the
//   player draws it without the mesher (JASPERENGINE_TITLE_ART_MESHER OFF).
// - Format, little-endian: magic "JTM1", version u32, tier u8, source hash u64,
//   TITLE_3D_VERSION u32; then sections, each a u32 tag and a u32 byte length: vertices (24 B
//   each), wrap (u16 pairs), indices (a u8 width flag, then u16 or u32), slots, spans (float32),
//   parts, rigs, bounds and statistics, pivot. A reader refuses an unknown version.
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "titleart/three_d/TitleMesh.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    constexpr uint32_t TITLE_MESH_FILE_VERSION = 1;
    constexpr const char* TITLE_MESH_EXTENSION = ".titlemesh";

    JASPERENGINE_API std::vector<uint8_t> writeTitleMesh(const TitleMesh& mesh);
    // null with `error` ("unknown_version: ..." for a newer file, "corrupt: ..." otherwise)
    JASPERENGINE_API std::shared_ptr<TitleMesh> readTitleMesh(const std::vector<uint8_t>& bytes, std::string* error);

}
