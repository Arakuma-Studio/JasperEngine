// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/test/cleanup.test.ts):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// S46 AC5 (R10): a doubtful speck merges, a clear one stays, a close colour merges, two-pixel eyes
// stay until strength 1 and never when locked, a diagonal is never a speck, a kept speck is left
// alone, and size 0 does nothing.

#include "pixelart/cleanup/CleanupSpecks.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace jasperengine::editor::pixelart::cleanup;
using jasperengine::pixelart::packRgb;

namespace {

    const std::string specksKeys = "CGBKS";

    std::vector<Rgba8> specksPalette() {
        return {Rgba8{252, 240, 232, 255}, Rgba8{196, 196, 202, 255}, Rgba8{90, 170, 250, 255}, Rgba8{4, 4, 10, 255}, Rgba8{246, 206, 176, 255}};
    }

    struct SpecksScene {
        PixelArt art;
        CellSamples samples;
    };

    // A fixed picture from rows of letters, and the pixels each cell was read from: four of its own
    // colour plus doubt[i] letters for cell i (votes for another colour)
    SpecksScene specksScene(const std::vector<std::string>& rows, const std::map<int, std::string>& doubt = {}) {
        SpecksScene out;
        const std::vector<Rgba8> palette = specksPalette();
        out.art.width = out.samples.width = int(rows[0].size());
        out.art.height = out.samples.height = int(rows.size());
        out.art.palette = palette;
        for (const std::string& row : rows) {
            for (char letter : row) {
                const int i = int(out.art.indices.size());
                out.art.indices.push_back(int32_t(specksKeys.find(letter)));
                std::string read(4, letter);
                const auto found = doubt.find(i);
                if (found != doubt.end()) read += found->second;
                Cell cell;
                for (char key : read) cell.colors.push_back(packRgb(palette[specksKeys.find(key)]));
                out.samples.cells.push_back(std::move(cell));
            }
        }
        return out;
    }

    CleanupOptions specksOptions(double size, double strength, std::vector<int> protect = {}, std::vector<int> keep = {}) {
        CleanupOptions options;
        options.size = size;
        options.strength = strength;
        options.protect = std::move(protect);
        options.keep = std::move(keep);
        return options;
    }

    std::vector<std::string> specksShow(const PixelArt& art) {
        std::vector<std::string> out;
        for (int y = 0; y < art.height; y++) {
            std::string row;
            for (int x = 0; x < art.width; x++) row += specksKeys[size_t(art.indices[size_t(y) * art.width + x])];
            out.push_back(row);
        }
        return out;
    }

    using Rows = std::vector<std::string>;

}

TEST(CleanupSpecks, MergesASpeckThatNearlyVotedForItsSurroundingsKeepsOneDrawnClearly) {
    // (1, 1) is a blue speck whose cell half voted cream; (3, 1) is a blue speck drawn clearly
    const SpecksScene scene = specksScene({"CCCCC", "CBCBC", "CCCCC"}, {{6, "CCC"}});
    const Cleaned cleaned = cleanUp(scene.art, scene.samples, specksOptions(1, 0.5));
    EXPECT_EQ(specksShow(cleaned.art), (Rows{"CCCCC", "CCCBC", "CCCCC"}));
    EXPECT_EQ(cleaned.specks, (std::vector<std::vector<int>>{{6}}));
    EXPECT_EQ(specksShow(cleanUp(scene.art, scene.samples, specksOptions(1, 1)).art), (Rows{"CCCCC", "CCCCC", "CCCCC"}));
}

TEST(CleanupSpecks, MergesASpeckCloseInColourEvenWhenDrawnClearly) {
    const SpecksScene scene = specksScene({"CCC", "CGC", "CCC"});
    EXPECT_EQ(specksShow(cleanUp(scene.art, scene.samples, specksOptions(1, 0.5)).art), (Rows{"CCC", "CCC", "CCC"}));
    EXPECT_EQ(specksShow(cleanUp(scene.art, scene.samples, specksOptions(1, 0)).art), (Rows{"CCC", "CGC", "CCC"}));
}

TEST(CleanupSpecks, KeepsTwoPixelEyesUntilToldToMergeEverythingAndNeverWhenProtected) {
    const SpecksScene scene = specksScene({"SSSSSS", "SKSSKS", "SKSSKS", "SSSSSS"});
    EXPECT_TRUE(cleanUp(scene.art, scene.samples, specksOptions(1, 1)).specks.empty());
    EXPECT_TRUE(cleanUp(scene.art, scene.samples, specksOptions(2, 0.5)).specks.empty());
    EXPECT_EQ(cleanUp(scene.art, scene.samples, specksOptions(2, 1)).specks.size(), 2u);
    EXPECT_TRUE(cleanUp(scene.art, scene.samples, specksOptions(2, 1, {int(specksKeys.find('K'))})).specks.empty());
}

TEST(CleanupSpecks, NeverTakesAOnePixelDiagonalOutlineForASpeck) {
    const SpecksScene scene = specksScene({"KCCC", "CKCC", "CCKC", "CCCK"});
    EXPECT_TRUE(cleanUp(scene.art, scene.samples, specksOptions(3, 1)).specks.empty());
}

TEST(CleanupSpecks, LeavesASpeckBroughtBackByHand) {
    const SpecksScene scene = specksScene({"CCCCC", "CGCGC", "CCCCC"});
    const Cleaned cleaned = cleanUp(scene.art, scene.samples, specksOptions(1, 1, {}, {8}));
    EXPECT_EQ(specksShow(cleaned.art), (Rows{"CCCCC", "CCCGC", "CCCCC"}));
}

TEST(CleanupSpecks, DoesNothingWhenOff) {
    const SpecksScene scene = specksScene({"CCC", "CGC", "CCC"});
    const Cleaned cleaned = cleanUp(scene.art, scene.samples, specksOptions(0, 1));
    EXPECT_EQ(cleaned.art, scene.art);
    EXPECT_TRUE(cleaned.specks.empty());
}
