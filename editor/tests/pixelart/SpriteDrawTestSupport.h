// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shared by the S47 drawing tests: small in-memory sprites and batches run on them.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/draw/GridCodec.h"
#include "pixelart/draw/SpriteDoc.h"
#include "pixelart/draw/SpriteEdit.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::s47test {

    namespace ase = aseprite;
    using nlohmann::json;

    // 0 is transparent; 1-5 are distinct colours
    inline std::vector<ase::PaletteEntry> drawPalette(int size = 6) {
        static const uint8_t kColors[][3] = {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}, {128, 128, 128}, {59, 93, 201}};
        std::vector<ase::PaletteEntry> out;
        for (int i = 0; i < size && i < 9; i++) out.push_back({{kColors[i][0], kColors[i][1], kColors[i][2], static_cast<uint8_t>(i == 0 ? 0 : 255)}, std::nullopt});
        return out;
    }

    inline draw::SpriteDoc makeSprite(int w, int h, ase::ColorMode mode = ase::ColorMode::Indexed, int frames = 1, std::vector<std::string> layers = {"A"}) {
        draw::SpriteDoc sprite;
        ase::Document& doc = sprite.doc;
        doc.width = w;
        doc.height = h;
        doc.mode = mode;
        doc.frameDurations.assign(static_cast<size_t>(frames), 100);
        doc.palette = drawPalette();
        for (const std::string& name : layers) {
            ase::Layer layer;
            layer.name = name;
            doc.layers.push_back(layer);
        }
        return sprite;
    }

    struct Run {
        bool ok = false;
        draw::EditResult result;
        PixelProblem problem;
        int opIndex = -1;
    };

    inline Run apply(draw::SpriteDoc& sprite, json ops, json symmetry = nullptr, json frame = nullptr, json layer = nullptr, const draw::EditEnv& env = {}) {
        draw::EditRequest request;
        request.ops = std::move(ops);
        request.symmetry = std::move(symmetry);
        request.frame = std::move(frame);
        request.layer = std::move(layer);
        Run run;
        run.ok = draw::applyOps(sprite, request, env, run.result, run.problem, run.opIndex);
        return run;
    }

#define S47_APPLY(sprite, ...)                                                  \
    do {                                                                        \
        const ::jasperengine::editor::pixelart::s47test::Run s47Run =           \
            ::jasperengine::editor::pixelart::s47test::apply((sprite), __VA_ARGS__); \
        ASSERT_TRUE(s47Run.ok) << s47Run.problem.kind << ": " << s47Run.problem.message << " fix: " << s47Run.problem.fix; \
    } while (0)

    // the canvas as text: the alphabet for indexed sprites, '#' for any opaque pixel otherwise, '.' clear
    inline std::vector<std::string> rowsOf(const PixelImage& image) {
        std::vector<std::string> rows;
        for (int y = 0; y < image.height(); y++) {
            std::string row;
            for (int x = 0; x < image.width(); x++) {
                if (image.hasIndices()) row.push_back(image.at(x, y)[3] == 0 ? '.' : draw::alphabetChar(image.indexAt(x, y)));
                else row.push_back(image.at(x, y)[3] == 0 ? '.' : '#');
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // ---- hand-built PNGs (no zlib needed: stored deflate blocks; nothing checks CRCs of test bytes)
    inline void appendBe32(std::vector<uint8_t>& out, uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(v >> s));
    }

    inline void appendChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
        appendBe32(out, static_cast<uint32_t>(data.size()));
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        appendBe32(out, 0);
    }

    inline std::vector<uint8_t> storedZlib(const std::vector<uint8_t>& raw) {
        std::vector<uint8_t> out = {0x78, 0x01, 1};
        const uint16_t len = static_cast<uint16_t>(raw.size());
        out.push_back(static_cast<uint8_t>(len & 0xff));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(~len & 0xff));
        out.push_back(static_cast<uint8_t>((~len >> 8) & 0xff));
        out.insert(out.end(), raw.begin(), raw.end());
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw) {
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
        appendBe32(out, (b << 16) | a);
        return out;
    }

    inline std::vector<uint8_t> pngSignature() { return {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}; }

    inline std::vector<uint8_t> ihdrBytes(int w, int h, int colorType) {
        std::vector<uint8_t> d;
        appendBe32(d, static_cast<uint32_t>(w));
        appendBe32(d, static_cast<uint32_t>(h));
        d.insert(d.end(), {8, static_cast<uint8_t>(colorType), 0, 0, 0});
        return d;
    }

    // rows of pixel bytes, filter 0
    inline std::vector<uint8_t> filterNone(int stride, int height, const std::vector<uint8_t>& pixels) {
        std::vector<uint8_t> raw;
        for (int y = 0; y < height; y++) {
            raw.push_back(0);
            raw.insert(raw.end(), pixels.begin() + y * stride, pixels.begin() + (y + 1) * stride);
        }
        return raw;
    }

    // a one-channel-per-pixel 8-bit palette PNG, tRNS optional, one extra chunk between PLTE and IDAT
    inline std::vector<uint8_t> palettePng(int w, int h, const std::vector<uint8_t>& indices, const std::vector<std::array<uint8_t, 3>>& colors,
                                           const std::vector<uint8_t>& alphas = {}, const std::string& text = "") {
        std::vector<uint8_t> file = pngSignature();
        appendChunk(file, "IHDR", ihdrBytes(w, h, 3));
        std::vector<uint8_t> plte;
        for (const std::array<uint8_t, 3>& c : colors) plte.insert(plte.end(), c.begin(), c.end());
        appendChunk(file, "PLTE", plte);
        if (!alphas.empty()) appendChunk(file, "tRNS", alphas);
        if (!text.empty()) appendChunk(file, "tEXt", std::vector<uint8_t>(text.begin(), text.end()));
        appendChunk(file, "IDAT", storedZlib(filterNone(w, h, indices)));
        appendChunk(file, "IEND", {});
        return file;
    }

    inline std::vector<std::string> canvasRows(const draw::SpriteDoc& sprite, int layer = 0, int frame = 0) {
        return rowsOf(draw::celCanvas(sprite.doc, layer, frame));
    }

}
