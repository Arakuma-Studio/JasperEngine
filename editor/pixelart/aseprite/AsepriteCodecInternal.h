// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shared by AsepriteRead.cpp and AsepriteWrite.cpp only: the byte cursor (explicit little
// endian, every read bounds-checked), zlib through miniz, the chunk references and the
// provenance the writer uses to re-emit unchanged chunks byte for byte.
//
// A chunk's fingerprint is the XXH64 of what this codec's encoder makes of the document part the
// chunk holds, with each image's pixels replaced by their content hash. The reader computes it
// from the decoded document, the writer from the current one: equal fingerprints mean the
// chunk's meaning did not change, so its original bytes are written back.

#pragma once

#include "AsepriteDocument.h"

#include <cstring>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor::pixelart::aseprite::detail {

    // ---- errors

    struct Failure : std::exception {
        CodecError error;
        explicit Failure(CodecError e): error(std::move(e)) {}
        const char* what() const noexcept override { return error.message.c_str(); }
    };

    [[noreturn]] void fail(const char* kind, std::string message, std::string fix, std::string field, size_t offset);
    std::string chunkField(uint16_t type, size_t offset);   // "chunk 0x2005 @ 1234"
    std::string hex16(uint16_t value);                      // "0x2005"

    // ---- little-endian cursor over a byte span

    class Cursor {
    public:
        Cursor(std::span<const uint8_t> bytes, size_t begin, size_t end, uint16_t chunkType)
            : data(bytes), pos(begin), limit(end), type(chunkType), start(begin) {}

        size_t offset() const { return pos; }
        size_t remaining() const { return limit - pos; }
        bool atEnd() const { return pos >= limit; }
        void seek(size_t to) { pos = to; }

        uint8_t u8() { need(1); return data[pos++]; }
        uint16_t u16() {
            need(2);
            const uint16_t v = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
            pos += 2;
            return v;
        }
        int16_t i16() { return static_cast<int16_t>(u16()); }
        uint32_t u32() {
            need(4);
            const uint32_t v = static_cast<uint32_t>(data[pos]) | (static_cast<uint32_t>(data[pos + 1]) << 8) |
                               (static_cast<uint32_t>(data[pos + 2]) << 16) | (static_cast<uint32_t>(data[pos + 3]) << 24);
            pos += 4;
            return v;
        }
        int32_t i32() { return static_cast<int32_t>(u32()); }
        uint64_t u64() {
            const uint64_t lo = u32();
            const uint64_t hi = u32();
            return lo | (hi << 32);
        }
        std::span<const uint8_t> bytes(size_t n) {
            need(n);
            std::span<const uint8_t> s = data.subspan(pos, n);
            pos += n;
            return s;
        }
        void skip(size_t n) { need(n); pos += n; }
        std::string string() {
            const uint16_t n = u16();
            std::span<const uint8_t> s = bytes(n);
            return std::string(s.begin(), s.end());
        }
        // a count of items of at least `itemBytes` each: more than the bytes left can hold is corrupt
        uint32_t count(uint32_t value, size_t itemBytes, const char* what);

        void need(size_t n) const;

    private:
        std::span<const uint8_t> data;
        size_t pos;
        size_t limit;
        uint16_t type;     // 0: the header or a frame header
        size_t start;
    };

    // ---- growing little-endian buffer

    class Out {
    public:
        std::vector<uint8_t> bytes;

        void u8(uint32_t v) { bytes.push_back(static_cast<uint8_t>(v)); }
        void u16(uint32_t v) { u8(v & 0xff); u8((v >> 8) & 0xff); }
        void u32(uint32_t v) { u16(v & 0xffff); u16(v >> 16); }
        void u64(uint64_t v) { u32(static_cast<uint32_t>(v)); u32(static_cast<uint32_t>(v >> 32)); }
        void zeros(size_t n) { bytes.insert(bytes.end(), n, 0); }
        void raw(std::span<const uint8_t> s) { bytes.insert(bytes.end(), s.begin(), s.end()); }
        void string(const std::string& s) {
            const size_t n = std::min<size_t>(s.size(), 0xffff);
            u16(static_cast<uint32_t>(n));
            bytes.insert(bytes.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        }
        void patch32(size_t at, uint32_t v) {
            for (int i = 0; i < 4; i++) bytes[at + i] = static_cast<uint8_t>(v >> (8 * i));
        }
        size_t size() const { return bytes.size(); }
    };

    // ---- zlib

    // Inflates exactly `expected` bytes (fewer when the stream ends early: `produced` says how
    // many); false (with a message) when the stream is not zlib
    bool inflateBounded(std::span<const uint8_t> compressed, uint8_t* out, size_t expected, size_t& produced, std::string& error);
    // zlib stream at miniz level 6: deterministic
    // false (out empty) when miniz cannot compress it
    bool deflateBytes(std::span<const uint8_t> raw, std::vector<uint8_t>& out);

    // ---- pixels in file form

    int bytesPerPixel(ColorMode mode);
    // the file bytes of an image: RGBA, value+alpha, or indices (an indexed image without its
    // plane maps its RGBA to exact palette matches; false with the first colour that has none)
    bool imageFileBytes(const PixelImage& image, ColorMode mode, const std::vector<PaletteEntry>& palette, int transparentIndex,
                        std::vector<uint8_t>& out, std::string* error);
    // the image of a cel's file bytes (indexed: with its plane and palette lookup)
    PixelImage imageFromFileBytes(int w, int h, ColorMode mode, std::span<const uint8_t> raw, std::span<const uint8_t> paletteRgba,
                                  int transparentIndex);

    // ---- chunk references

    struct ChunkRef {
        enum class Kind : uint8_t {
            Palette,        // a = frame, b = chunk type (0x2019, 0x0004, 0x000B or 0x0011)
            Layer,          // a = layer
            Cel,            // a = layer, b = frame
            CelExtra,       // a = layer, b = frame
            ColorProfile,
            ExternalFiles,
            Mask,           // a = mask index
            Tags,
            Slice,          // a = slice (0x2022)
            OldSlices,      // a = first slice, b = count (the deprecated 0x2021)
            Tileset,        // a = tileset
            UserData,       // b = owner kind (UserDataOwner), a/c = owner indices
            Opaque,         // a = opaque chunk index
        };
        Kind kind = Kind::Opaque;
        int a = -1, b = -1, c = -1;

        bool operator==(const ChunkRef& o) const { return kind == o.kind && a == o.a && b == o.b && c == o.c; }
    };

    enum class UserDataOwner : int { Sprite, Layer, Cel, Tag, Slice, Tileset, Tile };

    inline ChunkRef userDataRef(UserDataOwner owner, int a = -1, int c = -1) {
        return {ChunkRef::Kind::UserData, a, static_cast<int>(owner), c};
    }

    struct ChunkRefHash {
        size_t operator()(const ChunkRef& r) const {
            return (static_cast<size_t>(r.kind) * 0x9E3779B97F4A7C15ull) ^ (static_cast<size_t>(r.a + 1) * 0x85EBCA77ull) ^
                   (static_cast<size_t>(r.b + 1) * 0xC2B2AE3Dull) ^ (static_cast<size_t>(r.c + 1) * 0x27D4EB2Full);
        }
    };

    // ---- provenance

    struct ChunkRecord {
        ChunkRef ref;
        uint16_t type = 0;
        size_t offset = 0;        // of the chunk's size field in the file
        size_t size = 0;          // the whole chunk, header included
        uint64_t fingerprint = 0;
    };

    struct FrameRecord {
        size_t offset = 0;        // of the frame header
        size_t size = 0;          // the frame's declared size (header, chunks and any slack)
        uint16_t rawDuration = 0;
        std::vector<ChunkRecord> chunks;
    };

    // the header fields as read, raw and decoded, so a field whose meaning did not change keeps its bytes
    struct HeaderSnapshot {
        uint16_t frames = 0, width = 0, height = 0, depth = 0;
        uint32_t flags = 0;
        uint16_t speed = 0;
        uint8_t transparentIndex = 0;      // decoded (0 unless indexed)
        uint16_t paletteSize = 0;          // decoded (0 in the file: 256)
        uint8_t pixelWidth = 1, pixelHeight = 1;   // decoded (0 in the file: 1)
        int16_t gridX = 0, gridY = 0;
        uint16_t gridW = 0, gridH = 0;
    };

}

namespace jasperengine::editor::pixelart::aseprite {

    struct Provenance {
        std::shared_ptr<const std::vector<uint8_t>> bytes;   // the whole file as read
        detail::HeaderSnapshot header;
        std::vector<detail::FrameRecord> frames;
        size_t framesEnd = 0;                                // where the last frame ended; bytes after it are kept
        // the content hash of each image the read produced; the images stay alive here, so a
        // pointer seen again at write time is the same, unchanged image
        std::unordered_map<const void*, uint64_t> imageHashes;
        std::vector<std::shared_ptr<const void>> keepAlive;
    };

}

namespace jasperengine::editor::pixelart::aseprite::detail {

    // the content hash of a cel's pixels in file form (cached through the provenance's table)
    struct HashCache {
        const Provenance* provenance = nullptr;
        std::unordered_map<const void*, uint64_t> computed;
        // image cels' compressed pixels, made ahead on several threads by the writer (looked up
        // by the cel record, never iterated: the output does not depend on the map's order)
        std::unordered_map<const void*, std::vector<uint8_t>> compressedCels;
        uint64_t imageHash(const Document& doc, const Cel& cel);
        uint64_t tilemapHash(const Tilemap& tilemap);
    };

    // The chunk body (after the 6-byte header) this codec writes for a reference. With
    // `fingerprint` the pixel data is the content hash instead of the compressed stream.
    // False (with *error) when the document cannot be encoded.
    bool encodeBody(const Document& doc, const ChunkRef& ref, uint16_t type, HashCache& hashes, bool fingerprint,
                    std::vector<uint8_t>& body, CodecError* error);
    uint64_t fingerprintOf(const Document& doc, const ChunkRef& ref, uint16_t type, HashCache& hashes);

    // the type of chunk a reference is written as (palette references carry theirs)
    uint16_t chunkTypeOf(const Document& doc, const ChunkRef& ref);

}
