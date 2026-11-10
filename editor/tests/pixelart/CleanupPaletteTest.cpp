// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from ritmo (packages/tools/test/features.test.ts and core.test.ts "palettes", "exports"):
// MIT License, Copyright (c) 2026 Sandro Maglione.

// S46 AC7 (R9, R13-R18): palette extraction and proposals, lost colours, background and shadow
// removal, hand edits, the crop and RGBA output, the alpha cutoff, and the pipeline's order, lines
// dragged by hand and cache.

#include "pixelart/cleanup/CleanupBackground.h"
#include "pixelart/cleanup/CleanupEdits.h"
#include "pixelart/cleanup/CleanupGrid.h"
#include "pixelart/cleanup/CleanupLost.h"
#include "pixelart/cleanup/CleanupPalette.h"
#include "pixelart/cleanup/CleanupPipeline.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSynthetic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine::editor::pixelart;
using namespace jasperengine::editor::pixelart::cleanup;
using jasperengine::pixelart::ColorMetric;
using jasperengine::pixelart::ciede2000;
using jasperengine::pixelart::packRgb;
using jasperengine::pixelart::toCielab;
using jasperengine::pixelart::toHex;

namespace {

    using Rows = std::vector<std::string>;

    const std::string featureKeys = "WPKBSG";

    Rgba8 featureColor(char key) {
        switch (key) {
            case 'W': return {255, 255, 255, 255};
            case 'P': return {246, 214, 236, 255};
            case 'K': return {10, 8, 20, 255};
            case 'B': return {120, 150, 230, 255};
            case 'S': return {240, 200, 170, 255};
            default: return {200, 200, 200, 255};   // G
        }
    }

    std::vector<Rgba8> featurePalette() {
        std::vector<Rgba8> palette;
        for (char key : featureKeys) palette.push_back(featureColor(key));
        return palette;
    }

    PixelArt featureArt(const Rows& rows) {
        PixelArt art;
        art.width = int(rows[0].size());
        art.height = int(rows.size());
        art.palette = featurePalette();
        for (const std::string& row : rows) {
            for (char letter : row) art.indices.push_back(letter == '.' ? TRANSPARENT : int32_t(featureKeys.find(letter)));
        }
        return art;
    }

    Rows featureLetters(const PixelArt& picture) {
        const std::vector<Rgba8> palette = featurePalette();
        Rows out;
        for (int y = 0; y < picture.height; y++) {
            std::string row;
            for (int x = 0; x < picture.width; x++) {
                const int32_t index = picture.indices[size_t(y) * picture.width + x];
                if (index == TRANSPARENT) {
                    row += '.';
                    continue;
                }
                const auto it = std::find(palette.begin(), palette.end(), picture.palette[size_t(index)]);
                row += featureKeys[size_t(it - palette.begin())];
            }
            out.push_back(row);
        }
        return out;
    }

    // A fake checkerboard behind a black outline, with a white highlight and a white gap inside it
    PixelArt checkerScene() {
        return featureArt({"WWPPWWPPWW", "WWPKKKKPWW", "PPKBBWBKPP", "PPKBKKBKPP", "WWKBKWKKWW", "WWKBKKBKWW", "PPKSSSSKPP", "PPPKKKKPPP"});
    }

    BackgroundOptions bgOptions(double tolerance, bool enclosed, bool specks, bool shadow) {
        BackgroundOptions options;
        options.tolerance = tolerance;
        options.enclosed = enclosed;
        options.specks = specks;
        options.shadow = shadow;
        return options;
    }

    std::vector<uint8_t> floodMask(const PixelArt& art) { return findBackground(art, backgroundColors(art), bgOptions(0.02, false, false, false)); }

    Edit editAt(int x, int y, EditTool tool, bool area, Rgba8 color = {}) {
        Edit edit;
        edit.x = x;
        edit.y = y;
        edit.tool = tool;
        edit.area = area;
        edit.color = color;
        return edit;
    }

    // features.test.ts "lost colors"
    const Rgba8 lostWhite{240, 244, 250, 255}, lostBlack{6, 6, 20, 255}, lostBlue{120, 144, 200, 255}, lostSkin{240, 220, 204, 255},
        lostDeepBlue{50, 70, 170, 255}, lostGrey{140, 140, 140, 255};

    CellSamples lostSamples() {
        const std::string text = "WWWWWWWBBDDWWBDDBWWKKGGWWKSSGWWWWWWW";
        CellSamples samples;
        samples.width = samples.height = 6;
        for (size_t i = 0; i < text.size(); i++) {
            Rgba8 color;
            switch (text[i]) {
                case 'W': color = lostWhite; break;
                case 'K': color = lostBlack; break;
                case 'B': color = lostBlue; break;
                case 'S': color = lostSkin; break;
                case 'D': color = lostDeepBlue; break;
                default: color = lostGrey; break;
            }
            const int jitter = int(i % 3) - 1;
            Cell cell;
            for (int d : {0, jitter, -jitter}) {
                cell.colors.push_back(packRgb(Rgba8{uint8_t(color.r + d), uint8_t(color.g + d), uint8_t(color.b + d), 255}));
            }
            samples.cells.push_back(std::move(cell));
        }
        return samples;
    }

    PixelArt lostSnap(const CellSamples& samples, std::vector<Rgba8> palette) {
        QuantizeOptions snap;
        snap.palette = std::move(palette);
        snap.ditherStrength = 0;
        return quantize(samples, snap);
    }

    std::vector<size_t> lostCounts(const std::vector<LostColor>& lost) {
        std::vector<size_t> out;
        for (const LostColor& entry : lost) out.push_back(entry.cells.size());
        return out;
    }

    std::vector<Rgba8> repeated(const std::vector<std::pair<Rgba8, int>>& counts) {
        std::vector<Rgba8> out;
        for (const auto& [color, n] : counts) out.insert(out.end(), size_t(n), color);
        return out;
    }

    std::vector<Rgba8> proposalSample() {
        return repeated({{featureColor('W'), 400}, {Rgba8{250, 250, 250, 255}, 300}, {featureColor('B'), 200}, {featureColor('K'), 95}, {Rgba8{230, 20, 40, 255}, 5}});
    }

    Recipe pipelineRecipe(double cell) {
        Recipe recipe;
        recipe.grid.kind = GridKind::Cells;
        recipe.grid.spec = {cell, cell, 0, 0};
        return recipe;
    }

}

// ---- background (features.test.ts)

TEST(CleanupBackground, FindsBothColoursOfAFakeCheckerboardOnTheBorder) {
    const PixelArt scene = checkerScene();
    std::string found;
    for (int index : backgroundColors(scene)) found += featureKeys[size_t(index)];
    std::sort(found.begin(), found.end());
    EXPECT_EQ(found, "PW");
}

TEST(CleanupBackground, FloodsInFromTheBorderAndStopsAtTheOutline) {
    const PixelArt scene = checkerScene();
    EXPECT_EQ(featureLetters(clearMasked(scene, floodMask(scene))),
              (Rows{"..........", "...KKKK...", "..KBBWBK..", "..KBKKBK..", "..KBKWKK..", "..KBKKBK..", "..KSSSSK..", "...KKKK..."}));
}

TEST(CleanupBackground, RemovesEnclosedBackgroundColouredAreasOnlyWhenAsked) {
    const PixelArt scene = checkerScene();
    const Rows cleared = featureLetters(clearMasked(scene, findBackground(scene, backgroundColors(scene), bgOptions(0.02, true, false, false))));
    EXPECT_EQ(cleared[2], "..KBB.BK..");
    EXPECT_EQ(cleared[4], "..KBK.KK..");
}

TEST(CleanupBackground, ErasesAndRestoresByHandAPixelOrAWholeAreaInOrder) {
    const PixelArt scene = checkerScene();
    const Edited edited = applyEdits(scene, floodMask(scene),
                                     {editAt(5, 4, EditTool::Erase, false), editAt(3, 2, EditTool::Erase, true), editAt(0, 0, EditTool::Restore, true), editAt(9, 7, EditTool::Restore, false)},
                                     ColorMetric::OkLab);
    const Rows rows = featureLetters(clearMasked(scene, edited.mask));
    EXPECT_EQ(rows[0], "WW........");
    EXPECT_EQ(rows[1], "WW.KKKK...");
    EXPECT_EQ(rows[2], "..K..WBK..");
    EXPECT_EQ(rows[4], "..K.K.KK..");
    EXPECT_EQ(rows[7], "...KKKK..P");
}

TEST(CleanupBackground, PaintsOnePixelOrAWholeAreaWithTheNearestPaletteColourInOrderWithErasing) {
    const PixelArt scene = checkerScene();
    const Edited edited = applyEdits(scene, floodMask(scene),
                                     {// A near-black off the palette snaps to K
                                      editAt(5, 2, EditTool::Paint, false, Rgba8{20, 18, 30, 255}),
                                      editAt(3, 6, EditTool::Paint, true, featureColor('B')),
                                      // Painting shows an erased pixel again; erasing it after takes it away
                                      editAt(0, 3, EditTool::Paint, false, featureColor('S')),
                                      editAt(0, 4, EditTool::Paint, false, featureColor('S')),
                                      editAt(0, 4, EditTool::Erase, false)},
                                     ColorMetric::OkLab);
    const Rows cleared = featureLetters(clearMasked(edited.art, edited.mask));
    EXPECT_EQ(cleared[2], "..KBBKBK..");
    EXPECT_EQ(cleared[3], "S.KBKKBK..");
    EXPECT_EQ(cleared[4], "..KBKWKK..");
    EXPECT_EQ(cleared[6], "..KBBBBK..");
    EXPECT_EQ(edited.painted, (std::vector<int>{25, 30, 63, 64, 65, 66}));
}

TEST(CleanupBackground, RemovesADropShadowUnderTheSubjectButNotTheSubjectsOwnGreyAndOnlyWhenAsked) {
    // G is the white background made darker: a shadow against the background below the feet, and a grey patch inside the outline
    const PixelArt shadowed = featureArt({"WWWWWWWWWW", "WWWKKKKWWW", "WWKBGGBKWW", "WWKBGGBKWW", "WWKBBBBKWW", "WWWKBBKWWW", "WGGGKKGGGW", "WWGGGGGGWW", "WWWWWWWWWW"});
    const std::vector<int> colors = backgroundColors(shadowed);
    EXPECT_EQ(featureLetters(clearMasked(shadowed, findBackground(shadowed, colors, bgOptions(0.02, false, false, true)))),
              (Rows{"..........", "...KKKK...", "..KBGGBK..", "..KBGGBK..", "..KBBBBK..", "...KBBK...", "....KK....", "..........", ".........."}));
    EXPECT_EQ(featureLetters(clearMasked(shadowed, findBackground(shadowed, colors, bgOptions(0.02, false, false, false))))[6], ".GGGKKGGG.");
}

TEST(CleanupBackground, LeavesAGreyPartOfTheSubjectThatTouchesTheBackgroundButSitsHighUp) {
    // A grey wing against the background at the top: not under the subject, so not a shadow
    const PixelArt winged = featureArt({"WWWWWWWWWW", "WGGGKKKWWW", "WGGKBBBKWW", "WWWKBBBKWW", "WWWKBBBKWW", "WWWWKKKWWW", "WWWWWWWWWW"});
    EXPECT_EQ(featureLetters(clearMasked(winged, findBackground(winged, backgroundColors(winged), bgOptions(0.02, false, false, true))))[1], ".GGGKKK...");
}

TEST(CleanupBackground, RemovesSpecksLeftAloneInTheBackgroundAndOnlyWhenAsked) {
    const PixelArt specked = featureArt({"WWWWWWWW", "WPWWKKWW", "WWWKBBKW", "WWWKBBKW", "WWWWKKWW", "WWWWWWWW"});
    const std::vector<int> colors = backgroundColors(specked);
    EXPECT_EQ(featureLetters(clearMasked(specked, findBackground(specked, colors, bgOptions(0.02, false, false, false))))[1], ".P..KK..");
    EXPECT_EQ(featureLetters(clearMasked(specked, findBackground(specked, colors, bgOptions(0.02, false, true, false)))),
              (Rows{"........", "....KK..", "...KBBK.", "...KBBK.", "....KK..", "........"}));
}

TEST(CleanupBackground, TreatsPixelsThatAreAlreadyTransparentAsBackground) {
    const PixelArt picture = featureArt({"....", ".KK.", ".KK.", "...."});
    EXPECT_TRUE(backgroundColors(picture).empty());
    EXPECT_EQ(findBackground(picture, {}, bgOptions(0.08, false, false, false)), (std::vector<uint8_t>{1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1}));
}

TEST(CleanupBackground, RemovesAFakeCheckerboardFromABlurryUpscaledPicture) {
    const PixelArt scene = checkerScene();
    Upscale options;
    options.scale = 9.4;
    options.offset = 3;
    options.blur = 1;
    options.noise = 3;
    options.seed = 2;
    const RgbaImage image = upscale(scene, options);
    const std::optional<Detection> detection = detectGrid(image);
    ASSERT_TRUE(detection);
    QuantizeOptions plain;
    plain.ditherStrength = 0;
    const PixelArt fixed = quantize(sampleCells(image, uniformGrid(image.width(), image.height(), detection->spec), {0.25, 128}), plain);
    ASSERT_EQ(fixed.width, scene.width);
    EXPECT_EQ(findBackground(fixed, backgroundColors(fixed), bgOptions(0.08, false, false, false)), floodMask(scene));
}

TEST(CleanupBackground, KeepsAnEnclosedEyeOfTheBackgroundColourWithoutEnclosed) {
    // AC7: W inside the outline at row 2 and row 4 stays
    const PixelArt scene = checkerScene();
    const Rows cleared = featureLetters(clearMasked(scene, floodMask(scene)));
    EXPECT_EQ(cleared[2][5], 'W');
    EXPECT_EQ(cleared[4][5], 'W');
}

TEST(CleanupBackground, KeepsTheOutlineWhenThePaletteHasNoColourForTheBackground) {
    // A grey background whose nearest palette colour is the dark outline: found in the palette's
    // colours, the outline would go with it
    const Rgba8 grey{60, 70, 68, 255}, outline{18, 43, 52, 255}, ice{162, 221, 249, 255};
    const std::string text = "GGGGGGGGGGGTTGGGGGTIITGGGTIIIITGGGTIITGGGGGTTGGGGGGGGGGG";
    PixelArt sprite;
    sprite.width = 8;
    sprite.height = 7;
    sprite.palette = {grey, outline, ice};
    for (char letter : text) sprite.indices.push_back(int32_t(std::string("GTI").find(letter)));
    Upscale options;
    options.scale = 9.4;
    options.offset = 3;
    options.blur = 1;
    options.noise = 3;
    options.seed = 2;
    Recipe recipe;
    RecipePalette palette;
    palette.name = "Dark";
    palette.colors = {outline, ice};
    recipe.palette = palette;
    recipe.removeBackground = true;
    const Outcome fixed = *runRecipe(upscale(sprite, options), recipe);
    std::string kept;
    for (int32_t index : fixed.art.indices) kept += index == TRANSPARENT ? '.' : "TI"[size_t(index)];
    std::string expected = text;
    std::replace(expected.begin(), expected.end(), 'G', '.');
    EXPECT_EQ(kept, expected);
}

// ---- crop and RGBA (features.test.ts, core.test.ts "exports")

TEST(CleanupExport, CropKeepsOnlyTheBoxAroundTheOpaquePixels) {
    const PixelArt cropped = cropToContent(featureArt({"......", "..K...", ".KBK..", "..K...", "......"}));
    EXPECT_EQ(cropped.width, 3);
    EXPECT_EQ(cropped.height, 3);
    EXPECT_EQ(featureLetters(cropped), (Rows{".K.", "KBK", ".K."}));
}

TEST(CleanupExport, CropLeavesAPictureWithNothingToCropOrNothingOpaqueAsItIs) {
    const PixelArt full = featureArt({"KB", "BK"});
    EXPECT_EQ(cropToContent(full), full);
    const PixelArt empty = featureArt({"..", ".."});
    EXPECT_EQ(cropToContent(empty), empty);
}

TEST(CleanupExport, ScalesPixelsByWholeFactors) {
    PixelArt art;
    art.width = 3;
    art.height = 2;
    art.palette = {Rgba8{255, 0, 0, 255}, Rgba8{0, 0, 255, 255}};
    art.indices = {0, 0, 1, 0, 0, TRANSPARENT};
    const RgbaImage image = toRgba(art, 2);
    EXPECT_EQ(image.width(), 6);
    EXPECT_EQ(image.height(), 4);
    EXPECT_EQ(image.at(0, 0), (PixelRgba{255, 0, 0, 255}));
    EXPECT_EQ(image.at(5, 3), (PixelRgba{0, 0, 0, 0}));
}

// ---- palette extraction and proposals (core.test.ts "palettes", features.test.ts)

TEST(CleanupPalette, ExtractsTheColoursAPictureIsMadeOfKeepingLockedOnes) {
    std::vector<Rgba8> real = sweetie16();
    real.resize(6);
    const PixelArt sprite = makeSprite(30, 30, real, 4);
    Upscale options;
    options.scale = 4;
    options.noise = 3;
    const RgbaImage image = upscale(sprite, options);
    const Grid grid = uniformGrid(image.width(), image.height(), GridSpec{4, 4, 0, 0});
    const std::vector<Rgba8> colors = cellColors(sampleCells(image, grid, {0.25, 128}));
    const std::vector<Rgba8> locked{Rgba8{255, 0, 255, 255}};
    const std::vector<Rgba8> extracted = extractPalette(colors, 7, locked);
    ASSERT_EQ(extracted.size(), 7u);
    EXPECT_EQ(extracted[0], locked[0]);
    // Each real colour has an extracted colour within a few steps of it
    for (const Rgba8& color : real) {
        double nearest = 1e9;
        for (const Rgba8& other : extracted) nearest = std::min<double>(nearest, ciede2000(toCielab(color), toCielab(other)));
        EXPECT_LT(nearest, 3.0) << toHex(color);
    }
    EXPECT_EQ(extractPalette(colors, 7, locked), extracted);
}

TEST(CleanupPalette, DominantTakesTheMostUsedColoursWithNearDuplicatesMergedAndTheirShares) {
    const std::vector<Proposal> proposals = proposePalette(proposalSample(), 3, ProposeMethod::Dominant);
    ASSERT_EQ(proposals.size(), 3u);
    EXPECT_EQ(proposals[0].color, featureColor('W'));
    EXPECT_EQ(proposals[1].color, featureColor('B'));
    EXPECT_EQ(proposals[2].color, featureColor('K'));
    EXPECT_LT(std::abs(proposals[0].share - 0.7), 5e-6);
}

TEST(CleanupPalette, DistinctKeepsASmallAccentTheDominantColoursWouldDrop) {
    const Rgba8 accent{230, 20, 40, 255};
    auto has = [&](const std::vector<Proposal>& list) {
        return std::any_of(list.begin(), list.end(), [&](const Proposal& p) { return p.color == accent; });
    };
    EXPECT_TRUE(has(proposePalette(proposalSample(), 3, ProposeMethod::Distinct)));
    EXPECT_FALSE(has(proposePalette(proposalSample(), 3, ProposeMethod::Dominant)));
}

TEST(CleanupPalette, NeverProposesNearDuplicatesEvenWhenAskedForMoreColoursThanThePictureHas) {
    for (ProposeMethod method : {ProposeMethod::Dominant, ProposeMethod::Balanced, ProposeMethod::Distinct}) {
        EXPECT_EQ(proposePalette(proposalSample(), 10, method).size(), 4u) << int(method);
    }
}

TEST(CleanupPalette, BalancedGivesKMeansColoursWhoseSharesAddUpToTheWholePicture) {
    const std::vector<Proposal> proposals = proposePalette(proposalSample(), 4, ProposeMethod::Balanced);
    ASSERT_EQ(proposals.size(), 4u);
    double sum = 0;
    for (const Proposal& proposal : proposals) sum += proposal.share;
    EXPECT_LT(std::abs(sum - 1.0), 5e-6);
    for (size_t i = 1; i < proposals.size(); i++) EXPECT_LE(proposals[i].share, proposals[i - 1].share);
}

// ---- lost colours (features.test.ts)

TEST(CleanupLost, ListsTheColoursAPaletteHasNoMatchForMostCellsFirstWithWhatTheyBecame) {
    const CellSamples samples = lostSamples();
    const std::vector<LostColor> found = lostColors(samples, lostSnap(samples, {lostWhite, lostBlack, lostBlue, lostSkin}), defaultLostThreshold);
    EXPECT_EQ(lostCounts(found), (std::vector<size_t>{4, 3}));
    ASSERT_EQ(found.size(), 2u);
    EXPECT_EQ(found[0].became, lostBlue);
    EXPECT_EQ(found[0].cells, (std::vector<int>{9, 10, 14, 15}));
    EXPECT_EQ(found[1].cells, (std::vector<int>{21, 22, 28}));
}

TEST(CleanupLost, ForgetsALostColourOnceThePaletteHasItAndBringsEveryCellBack) {
    const CellSamples samples = lostSamples();
    const std::vector<LostColor> found = lostColors(samples, lostSnap(samples, {lostWhite, lostBlack, lostBlue, lostSkin}), defaultLostThreshold);
    ASSERT_FALSE(found.empty());
    // AC7: adding the colour brings back every one of its cells
    const PixelArt full = lostSnap(samples, {lostWhite, lostBlack, lostBlue, lostSkin, lostDeepBlue, lostGrey});
    EXPECT_TRUE(lostColors(samples, full, defaultLostThreshold).empty());
    for (int cell : found[0].cells) EXPECT_EQ(full.palette[size_t(full.indices[size_t(cell)])], lostDeepBlue) << "cell " << cell;
    for (int cell : found[1].cells) EXPECT_EQ(full.palette[size_t(full.indices[size_t(cell)])], lostGrey) << "cell " << cell;
}

TEST(CleanupLost, LeavesOutSkippedCellsAndColoursLostInTooFewCells) {
    const CellSamples samples = lostSamples();
    const PixelArt art = lostSnap(samples, {lostWhite, lostBlack, lostBlue, lostSkin});
    std::vector<uint8_t> skip(36, 0);
    for (int i : {9, 10, 14}) skip[size_t(i)] = 1;
    EXPECT_EQ(lostCounts(lostColors(samples, art, defaultLostThreshold, &skip)), (std::vector<size_t>{3}));
    EXPECT_EQ(lostCounts(lostColors(samples, art, defaultLostThreshold, nullptr, 4)), (std::vector<size_t>{4}));
}

// ---- the pipeline's S46 steps (R9, R14, R16, R18, R4 moved lines)

TEST(CleanupPipeline, CutsPartialAlphaToOpaqueOrTransparentAndCountsIt) {
    RgbaImage image(4, 1);
    image.set(0, 0, {200, 10, 10, 255});
    image.set(1, 0, {200, 10, 10, 200});
    image.set(2, 0, {200, 10, 10, 100});
    image.set(3, 0, {0, 0, 0, 0});
    Recipe recipe = pipelineRecipe(1);
    recipe.sample.inset = 0;
    const Outcome fixed = runWithSpec(image, recipe, recipe.grid.spec, std::nullopt);
    EXPECT_EQ(fixed.partialAlpha, 2);
    const RgbaImage output = toRgba(fixed.exported);
    EXPECT_EQ(output.at(0, 0)[3], 255);
    EXPECT_EQ(output.at(1, 0)[3], 255);
    EXPECT_EQ(output.at(2, 0)[3], 0);
    EXPECT_EQ(output.at(3, 0)[3], 0);
    // The cutoff decides: 200 is below a cutoff of 220
    recipe.alphaCutoff = 220;
    const RgbaImage strict = toRgba(runWithSpec(image, recipe, recipe.grid.spec, std::nullopt).exported);
    EXPECT_EQ(strict.at(1, 0)[3], 0);
    EXPECT_EQ(strict.at(0, 0)[3], 255);
}

TEST(CleanupPipeline, ProposesAnAutoPaletteFromTheCellsAndSnapsToIt) {
    const PixelArt sprite = makeSprite(16, 16, sweetie16(), 3);
    Upscale options;
    options.scale = 6;
    const RgbaImage image = upscale(sprite, options);
    Recipe recipe = pipelineRecipe(6);
    AutoPalette automatic;
    automatic.count = 4;
    automatic.method = ProposeMethod::Dominant;
    recipe.autoPalette = automatic;
    const Outcome fixed = *runRecipe(image, recipe);
    ASSERT_TRUE(fixed.palette);
    EXPECT_EQ(fixed.palette->colors.size(), 4u);
    EXPECT_EQ(fixed.art.palette, fixed.palette->colors);
}

TEST(CleanupPipeline, AppliesLinesDraggedByHandLastAndABentLineMovesAsAWhole) {
    const PixelArt sprite = makeSprite(8, 8, sweetie16(), 2);
    Upscale options;
    options.scale = 10;
    const RgbaImage image = upscale(sprite, options);
    Recipe recipe = pipelineRecipe(10);
    PipelineCache cache;
    recipe.moved = {{"x:3", 32.5}, {"y:2", 19.0}, {"x:99", 1.0}};
    const Grid moved = buildGrid(image, recipe.grid.spec, recipe, cache);
    EXPECT_EQ(moved.xs[3], 32.5);
    EXPECT_EQ(moved.ys[2], 19.0);
    recipe.follow = Follow::Bend;
    recipe.moved.clear();
    const Grid bent = buildGrid(image, recipe.grid.spec, recipe, cache);
    ASSERT_TRUE(bent.bend);
    recipe.moved = {{"x:3", bent.xs[3] + 2.0}};
    const Grid dragged = buildGrid(image, recipe.grid.spec, recipe, cache);
    ASSERT_TRUE(dragged.bend);
    for (size_t j = 0; j < bent.bend->xs[3].size(); j++) EXPECT_DOUBLE_EQ(dragged.bend->xs[3][j], bent.bend->xs[3][j] + 2.0);
}

TEST(CleanupPipeline, DropsHandEditsAndKeptSpecksRecordedAtAnotherSize) {
    const PixelArt sprite = makeSprite(8, 8, sweetie16(), 2);
    Upscale options;
    options.scale = 4;
    const RgbaImage image = upscale(sprite, options);
    Recipe recipe = pipelineRecipe(4);
    recipe.edits.size = "9x9";
    recipe.edits.strokes = {{editAt(0, 0, EditTool::Erase, false)}};
    recipe.cleanKeep.size = "9x9";
    recipe.cleanKeep.cells = {1};
    const Outcome stale = *runRecipe(image, recipe);
    EXPECT_TRUE(stale.editsStale);
    EXPECT_TRUE(stale.cleanKeepStale);
    EXPECT_NE(stale.art.indices[0], TRANSPARENT);
    recipe.edits.size = "8x8";
    const Outcome applied = *runRecipe(image, recipe);
    EXPECT_FALSE(applied.editsStale);
    EXPECT_EQ(applied.art.indices[0], TRANSPARENT);
}

TEST(CleanupPipeline, ACachedRunEqualsAColdRunAndFollowsTheNewPicture) {
    Upscale options;
    options.scale = 6;
    options.blur = 1;
    options.noise = 4;
    const RgbaImage first = upscale(makeSprite(16, 16, sweetie16(), 3), options);
    const RgbaImage second = upscale(makeSprite(16, 16, sweetie16(), 4), options);
    Recipe recipe = pipelineRecipe(6);
    PipelineCache cache;
    (void)runRecipe(first, recipe, &cache);
    recipe.cleanup.size = 2;     // a slider moved: the grid and the samples are reused
    EXPECT_EQ(runRecipe(first, recipe, &cache)->art, runRecipe(first, recipe)->art);
    // another picture must not read the cached mask, grid or samples
    EXPECT_EQ(runRecipe(second, recipe, &cache)->art, runRecipe(second, recipe)->art);
}
