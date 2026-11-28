// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R9, R131: reading the .vcolors file (layout in VertexColorFile.h).
// The writer is VertexColorFileWrite.cpp: a game that only reads links no zstd compressor.

#include "vertexcolor/VertexColorFile.h"
#include "vertexcolor/VertexColorFileFormat.h"

#include "texture/TexturePrepare.h"

#include "zstd.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace jasperengine::vertexcolor {

    namespace {

        using namespace fileformat;
        constexpr size_t VCOLORS_HEADER_BYTES = 12;
        constexpr uint8_t VCOLORS_KNOWN_FLAGS = VCOLORS_FLAG_EDITOR | VCOLORS_FLAG_ZSTD;
        // a decompressed body past this is a corrupt size field, not a file to allocate for
        constexpr unsigned long long VCOLORS_MAX_BODY = 1ull << 31;

        class VcolorsReader {
        public:
            VcolorsReader(const uint8_t* data, size_t size): data(data), size(size) {}
            bool has(uint64_t count) const { return at <= size && (uint64_t)(size - at) >= count; }
            size_t left() const { return size - at; }
            uint8_t u8() { return data[at++]; }
            uint16_t u16() {
                const uint16_t v = (uint16_t)(data[at] | ((uint16_t)data[at + 1] << 8));
                at += 2;
                return v;
            }
            uint32_t u32() {
                uint32_t v = 0;
                for (int i = 0; i < 4; i++) v |= (uint32_t)data[at + (size_t)i] << (8 * i);
                at += 4;
                return v;
            }
            uint64_t u64() {
                uint64_t v = 0;
                for (int i = 0; i < 8; i++) v |= (uint64_t)data[at + (size_t)i] << (8 * i);
                at += 8;
                return v;
            }
            int32_t i32() { return (int32_t)u32(); }
            float f32() {
                const uint32_t bits = u32();
                float v = 0.0f;
                std::memcpy(&v, &bits, 4);
                return v;
            }
            Vector4 value(Format format) {
                float c[4];
                for (float& channel : c) channel = format == Format::RGBA8 ? (float)u8() / 255.0f : halfToFloat(u16());
                return Vector4(c[0], c[1], c[2], c[3]);
            }
            const uint8_t* data;
            size_t size;
            size_t at = 0;
        };

        size_t vcolorsValueBytes(Format format) { return format == Format::RGBA8 ? 4u : 8u; }

        bool decodeVcolorsBody(VcolorsReader& r, Format format, bool hasEditor, uint32_t version, Data& out, std::string& error) {
            const size_t valueBytes = vcolorsValueBytes(format);
            if (!r.has(4)) {
                error = "truncated: no target count";
                return false;
            }
            const uint32_t targetCount = r.u32();
            // a target takes at least its fixed fields: a count past the bytes is corrupt, not an allocation
            constexpr size_t TARGET_MIN_BYTES = 12 + 24 + 4 + 4;
            if ((uint64_t)targetCount * TARGET_MIN_BYTES > r.left()) {
                error = "corrupt target count " + std::to_string(targetCount) + " (the file holds " + std::to_string(r.left()) + " more bytes)";
                return false;
            }
            out.targets.resize(targetCount);
            for (uint32_t t = 0; t < targetCount; t++) {
                TargetData& target = out.targets[t];
                const std::string where = "target " + std::to_string(t) + ": ";
                if (!r.has(12 + 24 + 4)) {
                    error = where + "truncated header";
                    return false;
                }
                target.key.node = r.i32();
                target.key.primitive = r.i32();
                target.key.submesh = r.i32();
                target.fingerprint.vertexCount = r.u32();
                target.fingerprint.indexCount = r.u32();
                target.fingerprint.topologyHash = r.u64();
                target.fingerprint.positionHash = r.u64();
                if (t > 0 && !(out.targets[t - 1].key < target.key)) {
                    error = where + "targets not sorted by key or repeated (node " + std::to_string(target.key.node) + " primitive " +
                            std::to_string(target.key.primitive) + " submesh " + std::to_string(target.key.submesh) + ")";
                    return false;
                }
                const uint32_t vertexCount = r.u32();
                if (!r.has((uint64_t)vertexCount * valueBytes)) {
                    error = where + "corrupt vertex count " + std::to_string(vertexCount) + " (truncated values)";
                    return false;
                }
                target.base.resize(vertexCount);
                for (Vector4& v : target.base) v = r.value(format);
                if (!r.has(4)) {
                    error = where + "truncated: no override count";
                    return false;
                }
                const uint32_t overrideCount = r.u32();
                if (!r.has((uint64_t)overrideCount * (4 + valueBytes))) {
                    error = where + "corrupt override count " + std::to_string(overrideCount) + " (truncated overrides)";
                    return false;
                }
                target.corners.resize(overrideCount);
                for (uint32_t i = 0; i < overrideCount; i++) {
                    target.corners[i].corner = r.u32();
                    target.corners[i].value = r.value(format);
                    if (i > 0 && target.corners[i].corner <= target.corners[i - 1].corner) {
                        error = where + "corner overrides not sorted or repeated (corner " + std::to_string(target.corners[i].corner) + ")";
                        return false;
                    }
                }
                if (hasEditor) {
                    if (!r.has(4)) {
                        error = where + "truncated: no snapshot count";
                        return false;
                    }
                    const uint32_t snapshotCount = r.u32();
                    if (!r.has((uint64_t)snapshotCount * 24)) {
                        error = where + "corrupt snapshot count " + std::to_string(snapshotCount) + " (truncated snapshot)";
                        return false;
                    }
                    target.snapshotPositions.resize(snapshotCount);
                    target.snapshotNormals.resize(snapshotCount);
                    for (uint32_t i = 0; i < snapshotCount; i++) {
                        Vector3& p = target.snapshotPositions[i];
                        Vector3& n = target.snapshotNormals[i];
                        p.x = r.f32();
                        p.y = r.f32();
                        p.z = r.f32();
                        n.x = r.f32();
                        n.y = r.f32();
                        n.z = r.f32();
                    }
                    // version 1 kept no snapshot indices: its targets transfer per vertex only
                    if (version >= 2) {
                        if (!r.has(4)) {
                            error = where + "truncated: no snapshot index count";
                            return false;
                        }
                        const uint32_t indexCount = r.u32();
                        if (indexCount % 3 != 0 || !r.has((uint64_t)indexCount * 4)) {
                            error = where + "corrupt snapshot index count " + std::to_string(indexCount);
                            return false;
                        }
                        target.snapshotIndices.resize(indexCount);
                        for (uint32_t& index : target.snapshotIndices) {
                            index = r.u32();
                            if (index >= snapshotCount) {
                                error = where + "snapshot index " + std::to_string(index) + " past its " + std::to_string(snapshotCount) + " vertices";
                                return false;
                            }
                        }
                    }
                }
            }
            if (r.left() != 0) {
                error = "corrupt: " + std::to_string(r.left()) + " bytes after the last target";
                return false;
            }
            return true;
        }

    }

    bool decode(const uint8_t* bytes, size_t size, Data& out, std::string& error) {
        if (bytes == nullptr || size < VCOLORS_HEADER_BYTES) {
            error = "truncated: " + std::to_string(size) + " bytes, the header needs " + std::to_string(VCOLORS_HEADER_BYTES);
            return false;
        }
        if (std::memcmp(bytes, VCOLORS_MAGIC, 4) != 0) {
            error = "not a .vcolors file (bad magic)";
            return false;
        }
        VcolorsReader header(bytes, size);
        header.at = 4;
        const uint32_t version = header.u32();
        if (version > VCOLORS_VERSION) {
            error = "made by a newer JasperEngine (version " + std::to_string(version) + "); this editor reads up to " + std::to_string(VCOLORS_VERSION);
            return false;
        }
        if (version == 0) {
            error = "corrupt: version 0";
            return false;
        }
        const uint8_t formatByte = header.u8();
        if (formatByte > (uint8_t)Format::RGBA16F) {
            error = "corrupt: unknown format " + std::to_string(formatByte);
            return false;
        }
        const uint8_t flags = header.u8();
        if (flags & ~VCOLORS_KNOWN_FLAGS) {
            error = "corrupt: unknown flags " + std::to_string(flags);
            return false;
        }
        header.u16();   // reserved

        const uint8_t* body = bytes + VCOLORS_HEADER_BYTES;
        size_t bodySize = size - VCOLORS_HEADER_BYTES;
        std::vector<uint8_t> unpacked;
        if (flags & VCOLORS_FLAG_ZSTD) {
            const unsigned long long rawSize = ZSTD_getFrameContentSize(body, bodySize);
            if (rawSize == ZSTD_CONTENTSIZE_ERROR || rawSize == ZSTD_CONTENTSIZE_UNKNOWN || rawSize > VCOLORS_MAX_BODY) {
                error = "corrupt: the compressed body has no valid size";
                return false;
            }
            unpacked.resize((size_t)rawSize);
            const size_t produced = ZSTD_decompress(unpacked.data(), unpacked.size(), body, bodySize);
            if (ZSTD_isError(produced) || produced != unpacked.size()) {
                error = "corrupt: the compressed body does not decompress";
                return false;
            }
            body = unpacked.data();
            bodySize = unpacked.size();
        }

        Data data;
        data.format = (Format)formatByte;
        VcolorsReader r(body, bodySize);
        if (!decodeVcolorsBody(r, data.format, (flags & VCOLORS_FLAG_EDITOR) != 0, version, data, error)) return false;
        out = std::move(data);
        return true;
    }

    bool readFile(const std::filesystem::path& path, Data& out, std::string& error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open '" + path.generic_string() + "'";
            return false;
        }
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) {
            error = "cannot read '" + path.generic_string() + "'";
            return false;
        }
        std::string reason;
        if (!decode(bytes.data(), bytes.size(), out, reason)) {
            error = "'" + path.generic_string() + "': " + reason;
            return false;
        }
        return true;
    }

}
