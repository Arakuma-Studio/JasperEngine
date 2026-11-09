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

#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::testing {

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
        const ::jasperengine::editor::pixelart::testing::Run s47Run =           \
            ::jasperengine::editor::pixelart::testing::apply((sprite), __VA_ARGS__); \
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

    inline std::vector<std::string> canvasRows(const draw::SpriteDoc& sprite, int layer = 0, int frame = 0) {
        return rowsOf(draw::celCanvas(sprite.doc, layer, frame));
    }

}
