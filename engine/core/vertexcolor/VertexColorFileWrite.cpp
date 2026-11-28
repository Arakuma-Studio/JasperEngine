// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R8, R92, R131: writing the .vcolors file (layout in VertexColorFile.h).
// The engine links only zstd's decoder (player size): the body is compressed by the compressor the
// editor sets (editor/vertexcolor/VertexColorCompressor.cpp), else written raw, which every reader takes.

#include "vertexcolor/VertexColorFile.h"
#include "vertexcolor/VertexColorFileFormat.h"

#include "io/AtomicFile.h"
#include "texture/TexturePrepare.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <string_view>

namespace jasperengine::vertexcolor {

    namespace {


        // the editor's zstd (setBodyCompressor); none in a game, which links only zstd's decoder
        std::atomic<BodyCompressor> vcolorsCompressor{nullptr};

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
                    // only overrides need the triangles to transfer: a smooth target's vertices say it all
                    // (a list that does not fit its snapshot is left out, never written to fail the read)
                    bool indices = target.hasOverrides() && target.snapshotIndices.size() % 3 == 0;
                    for (size_t i = 0; indices && i < target.snapshotIndices.size(); i++) indices = target.snapshotIndices[i] < target.snapshotPositions.size();
                    w.u32(indices ? (uint32_t)target.snapshotIndices.size() : 0u);
                    if (indices) {
                        for (uint32_t index : target.snapshotIndices) w.u32(index);
                    }
                }
            }
        }

    }

    void setBodyCompressor(BodyCompressor compressor) {
        vcolorsCompressor.store(compressor);
    }

    std::vector<uint8_t> encode(const Data& data, bool includeEditor) {
        std::vector<uint8_t> body;
        encodeVcolorsBody(data, includeEditor, body);

        uint8_t flags = includeEditor ? vertexcolor::fileformat::VCOLORS_FLAG_EDITOR : 0;
        std::vector<uint8_t> packed;
        const BodyCompressor compressor = vcolorsCompressor.load();
        if (!body.empty() && compressor && compressor(body, packed) && packed.size() < body.size())
            flags |= vertexcolor::fileformat::VCOLORS_FLAG_ZSTD;

        std::vector<uint8_t> out;
        VcolorsWriter w(out);
        for (uint8_t m : vertexcolor::fileformat::VCOLORS_MAGIC) w.u8(m);
        w.u32(VCOLORS_VERSION);
        w.u8((uint8_t)data.format);
        w.u8(flags);
        w.u16(0);
        const std::vector<uint8_t>& payload = (flags & vertexcolor::fileformat::VCOLORS_FLAG_ZSTD) ? packed : body;
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
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

}
