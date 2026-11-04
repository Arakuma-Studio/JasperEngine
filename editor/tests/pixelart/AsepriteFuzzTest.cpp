// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Hostile input (docs/specs/S44-aseprite-import.md R6, AC6): seeded byte mutations, truncations
// and size-field inflations of the corpus. The reader never crashes, never asks for an unbounded
// allocation (a decompressed size beyond zlib's ratio is refused before it is allocated) and every
// failure is a typed error. Run under ASan/UBSan with the sanitizer presets (docs/testing.md).

#include "AsepriteTestSupport.h"
#include "support/AllocationProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::aseprite;
using namespace jasperengine::editor::pixelart::testing;

namespace {

    const std::set<std::string> kKinds = {"not_aseprite", "truncated", "corrupt_chunk", "too_large", "unsupported_depth"};

    void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
        if (at + 4 > b.size()) return;
        for (int i = 0; i < 4; i++) b[at + static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    }

    void put16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
        if (at + 2 > b.size()) return;
        b[at] = static_cast<uint8_t>(v);
        b[at + 1] = static_cast<uint8_t>(v >> 8);
    }

    // the offsets of every chunk header of a valid file (its size field), to aim inflations at
    std::vector<size_t> chunkOffsets(const std::vector<uint8_t>& file) {
        std::vector<size_t> out;
        size_t pos = 128;
        const int frames = file.size() >= 8 ? (file[6] | (file[7] << 8)) : 0;
        for (int f = 0; f < frames && pos + 16 <= file.size(); f++) {
            const uint32_t size = file[pos] | (file[pos + 1] << 8) | (file[pos + 2] << 16) | (static_cast<uint32_t>(file[pos + 3]) << 24);
            const uint32_t chunks = file[pos + 12] | (file[pos + 13] << 8) | (file[pos + 14] << 16) | (static_cast<uint32_t>(file[pos + 15]) << 24);
            size_t cp = pos + 16;
            for (uint32_t c = 0; c < chunks && cp + 6 <= file.size(); c++) {
                out.push_back(cp);
                const uint32_t cs = file[cp] | (file[cp + 1] << 8) | (file[cp + 2] << 16) | (static_cast<uint32_t>(file[cp + 3]) << 24);
                if (cs < 6) break;
                cp += cs;
            }
            if (size < 16) break;
            pos += size;
        }
        return out;
    }

}

TEST(AsepriteFuzzTest, Mutations) {
    std::vector<std::vector<uint8_t>> corpus;
    std::vector<std::vector<size_t>> chunks;
    for (const auto& path : asepriteCorpus()) {
        std::vector<uint8_t> bytes = readBytes(path);
        if (bytes.size() > 64 * 1024) continue;   // the 512 KiB blend file would only slow the loop
        chunks.push_back(chunkOffsets(bytes));
        corpus.push_back(std::move(bytes));
    }
    ASSERT_GE(corpus.size(), 40u);

    std::mt19937 rng(0x5EED44);
    int ok = 0, failed = 0;
    size_t maxLargest = 0;
    for (int iteration = 0; iteration < 20000; iteration++) {
        const size_t pick = rng() % corpus.size();
        std::vector<uint8_t> bytes = corpus[pick];
        const int kind = static_cast<int>(rng() % 4);
        if (kind == 0) {
            // random byte flips
            const int n = 1 + static_cast<int>(rng() % 8);
            for (int i = 0; i < n; i++) bytes[rng() % bytes.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
        } else if (kind == 1) {
            bytes.resize(rng() % bytes.size());
        } else if (kind == 2 && !chunks[pick].empty()) {
            // a chunk's size (or a count right after its header) inflated
            const size_t at = chunks[pick][rng() % chunks[pick].size()];
            const uint32_t values[] = {0, 5, 0xffffffffu, 0x7fffffffu, 0x10000u, static_cast<uint32_t>(rng())};
            put32(bytes, at + ((rng() % 2) ? 0 : 6 + 4 * (rng() % 4)), values[rng() % 6]);
        } else {
            // header fields: frames, size, depth, palette size
            const size_t fields[] = {6, 8, 10, 12, 32};
            put16(bytes, fields[rng() % 5], static_cast<uint32_t>(rng()));
        }
        // R6: no single allocation beyond what the bytes can decompress to (zlib's 1032:1) or a
        // full 65,536-entry palette, and no more in all than the 1 GiB + 64 MiB budgets
        const jasperengine::editor::test::ThreadAllocations before = jasperengine::editor::test::threadAllocations();
        jasperengine::editor::test::resetLargestAllocation();
        ReadResult r = read(bytes);
        const jasperengine::editor::test::ThreadAllocations after = jasperengine::editor::test::threadAllocations();
        EXPECT_LE(after.largest, std::max<size_t>(8u << 20, 1100 * bytes.size())) << "iteration " << iteration;
        EXPECT_LE(after.bytes - before.bytes, (kMaxDecodedPixelBytes + kMaxPaletteBytes) * 2) << "iteration " << iteration;
        maxLargest = std::max(maxLargest, after.largest);
        if (r.ok()) {
            ok++;
            // what was read writes and renders (small canvases only: render is not the hostile surface)
            std::vector<uint8_t> out;
            CodecError error;
            if (!write(*r.document, out, &error)) {
                EXPECT_EQ(error.kind, "invalid_document") << "iteration " << iteration;
            }
            if (static_cast<int64_t>(r.document->width) * r.document->height <= 512 * 512) {
                for (int f = 0; f < std::min(r.document->frameCount(), 4); f++) renderFrame(*r.document, f);
            }
            for (const CodecError& w : r.warnings) EXPECT_FALSE(w.kind.empty());
        } else {
            failed++;
            EXPECT_TRUE(kKinds.count(r.error.kind)) << "iteration " << iteration << ": kind '" << r.error.kind << "' " << r.error.message;
            EXPECT_FALSE(r.error.message.empty());
            EXPECT_FALSE(r.error.fix.empty()) << r.error.kind;
        }
    }
    // both outcomes happen: the mutations reach past the header checks
    EXPECT_GT(ok, 1000);
    EXPECT_GT(failed, 1000);
    std::printf("[fuzz] the largest allocation of one read: %zu bytes\n", maxLargest);
}

// R6: a palette that changes on every frame keeps a copy per change; a file of many changes to a
// 65,536-entry palette stops with too_large once they pass 64 MiB, having allocated about that
TEST(AsepriteFuzzTest, PaletteChangesAreBounded) {
    Document doc;
    doc.width = 4;
    doc.height = 4;
    doc.mode = ColorMode::Rgba;
    doc.frameDurations.assign(60, 100);
    doc.palette.assign(65536, PaletteEntry{});
    Layer layer;
    layer.name = "Layer";
    doc.layers.push_back(layer);
    doc.rebuildTreeFromLevels();
    for (int f = 1; f < 60; f++) {
        std::vector<PaletteEntry> changed = doc.palette;
        changed[static_cast<size_t>(f)].color = {static_cast<uint8_t>(f), 0, 0, 255};
        doc.paletteChanges[f] = changed;
    }
    std::vector<uint8_t> bytes;
    CodecError error;
    ASSERT_TRUE(write(doc, bytes, &error)) << error.message;
    const jasperengine::editor::test::ThreadAllocations before = jasperengine::editor::test::threadAllocations();
    ReadResult r = read(bytes);
    const jasperengine::editor::test::ThreadAllocations after = jasperengine::editor::test::threadAllocations();
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error.kind, "too_large") << r.error.message;
    EXPECT_LT(after.bytes - before.bytes, static_cast<unsigned long long>(kMaxPaletteBytes) * 2 + bytes.size() * 4);
}
