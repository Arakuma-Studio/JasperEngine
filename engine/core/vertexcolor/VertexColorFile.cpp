// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R8, R9, R92, R131: the .vcolors file (layout in VertexColorFile.h)

#include "vertexcolor/VertexColorFile.h"

#include "io/AtomicFile.h"
#include "texture/TexturePrepare.h"

#include "zstd.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

namespace jasperengine::vertexcolor {

    namespace {

        constexpr uint8_t VCOLORS_MAGIC[4] = {'J', 'E', 'V', 'C'};
        constexpr size_t VCOLORS_HEADER_BYTES = 12;
        constexpr uint8_t VCOLORS_FLAG_EDITOR = 1;
        constexpr uint8_t VCOLORS_FLAG_ZSTD = 2;
        constexpr uint8_t VCOLORS_KNOWN_FLAGS = VCOLORS_FLAG_EDITOR | VCOLORS_FLAG_ZSTD;
        // a decompressed body past this is a corrupt size field, not a file to allocate for
        constexpr unsigned long long VCOLORS_MAX_BODY = 1ull << 31;

        // Explicit little-endian bytes, never a struct dump (the file reads the same on every target)
        class VcolorsWriter {
        public:
            explicit VcolorsWriter(std::vector<uint8_t>& out): out(out) {}
            void u8(uint8_t v) { out.push_back(v); }
            void u16(uint16_t v) {
                out.push_back((uint8_t)v);
                out.push_back((uint8_t)(v >> 8));
            }
            void u32(uint32_t v) {
                for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i)));
            }
            void u64(uint64_t v) {
                for (int i = 0; i < 8; i++) out.push_back((uint8_t)(v >> (8 * i)));
            }
            void i32(int32_t v) { u32((uint32_t)v); }
            void f32(float v) {
                uint32_t bits = 0;
                std::memcpy(&bits, &v, 4);
                u32(bits);
            }
            void value(const Vector4& v, Format format) {
                const float c[4] = {v.x, v.y, v.z, v.w};
                for (float channel : c) {
                    if (format == Format::RGBA8) {
                        if (!(channel > 0.0f)) channel = 0.0f;   // NaN too
                        if (channel > 1.0f) channel = 1.0f;
                        u8((uint8_t)std::floor(channel * 255.0f + 0.5f));
                    } else {
                        u16(floatToHalf(channel));
                    }
                }
            }
            std::vector<uint8_t>& out;
        };

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

        void encodeVcolorsBody(const Data& data, bool includeEditor, std::vector<uint8_t>& body) {
            VcolorsWriter w(body);
            w.u32((uint32_t)data.targets.size());
            for (const TargetData& target : data.targets) {
                w.i32(target.key.node);
                w.i32(target.key.primitive);
                w.i32(target.key.submesh);
                w.u32(target.fingerprint.vertexCount);
                w.u32(target.fingerprint.indexCount);
                w.u64(target.fingerprint.topologyHash);
                w.u64(target.fingerprint.positionHash);
                w.u32((uint32_t)target.base.size());
                for (const Vector4& v : target.base) w.value(v, data.format);
                w.u32((uint32_t)target.corners.size());
                for (const CornerValue& c : target.corners) {
                    w.u32(c.corner);
                    w.value(c.value, data.format);
                }
                if (includeEditor) {
                    // normals missing for a snapshot position are written as zero (no normal filter)
                    w.u32((uint32_t)target.snapshotPositions.size());
                    for (size_t i = 0; i < target.snapshotPositions.size(); i++) {
                        const Vector3& p = target.snapshotPositions[i];
                        const Vector3 n = i < target.snapshotNormals.size() ? target.snapshotNormals[i] : Vector3();
                        w.f32(p.x);
                        w.f32(p.y);
                        w.f32(p.z);
                        w.f32(n.x);
                        w.f32(n.y);
                        w.f32(n.z);
                    }
                }
            }
        }

        bool decodeVcolorsBody(VcolorsReader& r, Format format, bool hasEditor, Data& out, std::string& error) {
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
                }
            }
            if (r.left() != 0) {
                error = "corrupt: " + std::to_string(r.left()) + " bytes after the last target";
                return false;
            }
            return true;
        }

    }

    std::vector<uint8_t> encode(const Data& data, bool includeEditor) {
        std::vector<uint8_t> body;
        encodeVcolorsBody(data, includeEditor, body);

        uint8_t flags = includeEditor ? VCOLORS_FLAG_EDITOR : 0;
        std::vector<uint8_t> packed;
        if (!body.empty()) {
            packed.resize(ZSTD_compressBound(body.size()));
            const size_t size = ZSTD_compress(packed.data(), packed.size(), body.data(), body.size(), 3);
            if (!ZSTD_isError(size) && size < body.size()) {
                packed.resize(size);
                flags |= VCOLORS_FLAG_ZSTD;
            }
        }

        std::vector<uint8_t> out;
        VcolorsWriter w(out);
        for (uint8_t m : VCOLORS_MAGIC) w.u8(m);
        w.u32(VCOLORS_VERSION);
        w.u8((uint8_t)data.format);
        w.u8(flags);
        w.u16(0);
        const std::vector<uint8_t>& payload = (flags & VCOLORS_FLAG_ZSTD) ? packed : body;
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
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
        if (!decodeVcolorsBody(r, data.format, (flags & VCOLORS_FLAG_EDITOR) != 0, data, error)) return false;
        out = std::move(data);
        return true;
    }

    bool writeFile(const std::filesystem::path& path, const Data& data, bool includeEditor, std::string& error) {
        const std::vector<uint8_t> bytes = encode(data, includeEditor);
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Document;
        options.skipIfUnchanged = true;
        const io::WriteResult result = io::writeFileAtomic(path, std::string_view((const char*)bytes.data(), bytes.size()), options);
        if (!result.ok()) {
            error = result.message.empty() ? io::describeWriteError(result) : result.message;
            return false;
        }
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
