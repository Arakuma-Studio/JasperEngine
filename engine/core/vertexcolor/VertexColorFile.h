// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R8, R9, R92, R131: the .vcolors file. Little-endian binary:
//   magic "JEVC", u32 version, u8 format, u8 flags (bit0: has editor section), u16 reserved,
//   u32 targetCount, then per target: key (3 x i32), fingerprint (2 x u32, 2 x u64),
//   u32 vertexCount, values (RGBA8: 4 bytes, RGBA16F: 4 halves), u32 overrideCount,
//   overrides (u32 corner + value), and, with the editor flag, u32 snapshotCount,
//   positions (3 x f32) and normals (3 x f32) per snapshot vertex, then (version 2) u32
//   snapshotIndexCount and that many u32 indices of the snapshot's triangles (0 for a target
//   without overrides). Version 1 has no snapshot indices and still reads.
//   The body after the header is zstd-compressed when that is smaller (flags bit1).
// A future version is refused with a message and the bytes are left alone (S03).

#include "Export.h"
#include "vertexcolor/VertexColorTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::vertexcolor {

    constexpr uint32_t VCOLORS_VERSION = 2;
    constexpr const char* VCOLORS_EXTENSION = ".vcolors";

    // Compresses a body into `packed` (zstd, level 3); false when it cannot. The engine links only zstd's
    // decoder: the editor sets it once at start (editor/vertexcolor/VertexColorCompressor.cpp);
    // without one (a game) bodies are written raw
    using BodyCompressor = bool (*)(const std::vector<uint8_t>& body, std::vector<uint8_t>& packed);
    JASPERENGINE_API void setBodyCompressor(BodyCompressor compressor);

    // includeEditor: the snapshot section (R9); exports pass false (R92). The body is compressed when
    // a compressor is set and that is smaller
    JASPERENGINE_API std::vector<uint8_t> encode(const Data& data, bool includeEditor);
    JASPERENGINE_API bool decode(const uint8_t* bytes, size_t size, Data& out, std::string& error);

    // through engine/core/io/AtomicFile.h (R8)
    JASPERENGINE_API bool writeFile(const std::filesystem::path& path, const Data& data, bool includeEditor, std::string& error);
    JASPERENGINE_API bool readFile(const std::filesystem::path& path, Data& out, std::string& error);

}
