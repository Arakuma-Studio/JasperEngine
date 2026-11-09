// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Palette import and export (docs/specs/S43-palettes.md R5, R6, AC2). The fixtures under
// editor/tests/fixtures/palettes/ are laid out as GIMP, Aseprite, Pixelorama, Paint.NET, Photoshop
// and Windows write them, plus Lospec's own downloads of Endesga 32.

#include "pixelart/PaletteFormats.h"

#include "stb_image.h"
#include "stb_image_write.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace paletteformats_test {

    namespace px = jasperengine::pixelart;
    using namespace jasperengine::editor::pixelart;

    const std::vector<std::string> kEndesga32 = {
        "#be4a2f", "#d77643", "#ead4aa", "#e4a672", "#b86f50", "#733e39", "#3e2731", "#a22633", "#e43b44", "#f77622", "#feae34",
        "#fee761", "#63c74d", "#3e8948", "#265c42", "#193c3e", "#124e89", "#0099db", "#2ce8f5", "#ffffff", "#c0cbdc", "#8b9bb4",
        "#5a6988", "#3a4466", "#262b44", "#181425", "#ff0044", "#68386c", "#b55088", "#f6757a", "#e8b796", "#c28569"};
    const std::vector<std::string> kSweetie16 = {"#1a1c2c", "#5d275d", "#b13e53", "#ef7d57", "#ffcd75", "#a7f070", "#38b764", "#257179",
                                                 "#29366f", "#3b5dc9", "#41a6f6", "#73eff7", "#f4f4f4", "#94b0c2", "#566c86", "#333c57"};
    const std::vector<std::string> kPico8 = {"#000000", "#1d2b53", "#7e2553", "#008751", "#ab5236", "#5f574f", "#c2c3c7", "#fff1e8",
                                             "#ff004d", "#ffa300", "#ffec27", "#00e436", "#29adff", "#83769c", "#ff77a8", "#ffccaa"};

    std::vector<uint8_t> fixtureBytes(const std::string& name) {
        std::ifstream file(std::filesystem::path(JASPER_TEST_FIXTURES_DIR) / "palettes" / name, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::vector<uint8_t> textBytes(const std::string& text) {
        return std::vector<uint8_t>(text.begin(), text.end());
    }

    ImportResult importFixture(const std::string& name) {
        const std::vector<uint8_t> bytes = fixtureBytes(name);
        EXPECT_FALSE(bytes.empty()) << name;
        return importPalette(name, bytes);
    }

    std::vector<std::string> hexesOf(const px::Palette& palette) {
        std::vector<std::string> hexes;
        for (const px::Rgba8& color : palette.colors) hexes.push_back(px::toHex(color));
        return hexes;
    }

    px::Palette paletteOf(const std::vector<std::string>& hexes) {
        px::Palette palette;
        palette.name = "Round Trip";
        for (const std::string& hex : hexes) {
            px::Rgba8 color;
            EXPECT_TRUE(px::parseHex(hex, color)) << hex;
            palette.colors.push_back(color);
        }
        return palette;
    }

    bool hasWarning(const ImportResult& result, const std::string& kind) {
        for (const PixelProblem& warning : result.warnings) {
            if (warning.kind == kind) return true;
        }
        return false;
    }

    std::vector<uint8_t> exported(const px::Palette& palette, PaletteFormat format, std::vector<std::string>& warnings, int swatchSize = 1) {
        auto result = exportPalette(palette, format, warnings, swatchSize);
        EXPECT_TRUE(std::holds_alternative<std::vector<uint8_t>>(result)) << paletteFormatName(format);
        return std::holds_alternative<std::vector<uint8_t>>(result) ? std::get<std::vector<uint8_t>>(result) : std::vector<uint8_t>();
    }

    std::vector<uint8_t> encodeTestPng(int width, int height, const std::vector<px::Rgba8>& pixels) {
        std::vector<uint8_t> png;
        auto append = [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<uint8_t>*>(context);
            out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        };
        EXPECT_TRUE(stbi_write_png_to_func(append, &png, width, height, 4, pixels.data(), width * 4));
        return png;
    }

    TEST(PaletteFormats, GimpExport) {
        const ImportResult result = importFixture("gimp-sweetie-16.gpl");
        ASSERT_TRUE(result.ok()) << result.error.message;
        EXPECT_EQ(result.format, PaletteFormat::Gpl);
        EXPECT_EQ(hexesOf(*result.palette), kSweetie16);
        EXPECT_EQ(result.palette->name, "Sweetie 16");
        EXPECT_EQ(result.palette->columns, 8);
        // GIMP's placeholder "Untitled" is not a name
        const std::map<int, std::string> names = {{0, "Night"}, {2, "Dark Red"}, {12, "Off White"}};
        EXPECT_EQ(result.palette->names, names);
        EXPECT_EQ(result.palette->source.kind, "file");
        EXPECT_EQ(result.palette->source.file, "gimp-sweetie-16.gpl");
    }

    TEST(PaletteFormats, AsepriteGplExports) {
        const ImportResult plain = importFixture("aseprite-pico-8.gpl");
        ASSERT_TRUE(plain.ok()) << plain.error.message;
        EXPECT_EQ(hexesOf(*plain.palette), kPico8);
        EXPECT_EQ(plain.palette->name, "aseprite-pico-8");   // no Name: line, so the file stem
        EXPECT_TRUE(plain.palette->names.empty());

        // Aseprite's "Channels: RGBA" adds an alpha column
        const ImportResult alpha = importFixture("aseprite-alpha.gpl");
        ASSERT_TRUE(alpha.ok()) << alpha.error.message;
        EXPECT_EQ(hexesOf(*alpha.palette), (std::vector<std::string>{"#00000000", "#ff004d", "#29adff80", "#fff1e8"}));
    }

    TEST(PaletteFormats, PixeloramaEmptySlotsKeepTheirIndices) {
        const ImportResult result = importFixture("pixelorama-slots.gpl");
        ASSERT_TRUE(result.ok()) << result.error.message;
        EXPECT_EQ(hexesOf(*result.palette), (std::vector<std::string>{"#1a1c2c", "#00000000", "#b13e53", "#ef7d57", "#00000000", "#ffcd75"}));
        EXPECT_EQ(result.palette->name, "Slots");
        EXPECT_EQ(result.palette->description, "two empty slots");
        EXPECT_EQ(result.palette->columns, 3);
        // Pixelorama names every colour by its hex: nothing worth keeping
        EXPECT_TRUE(result.palette->names.empty());
    }

    TEST(PaletteFormats, LospecDownloadsOfEveryFormat) {
        const std::pair<const char*, PaletteFormat> files[] = {
            {"lospec-endesga-32.gpl", PaletteFormat::Gpl}, {"lospec-endesga-32.hex", PaletteFormat::Hex},
            {"lospec-endesga-32.pal", PaletteFormat::JascPal}, {"lospec-endesga-32.txt", PaletteFormat::PaintNetTxt},
            {"lospec-endesga-32.ase", PaletteFormat::AdobeAse}, {"lospec-endesga-32-1x.png", PaletteFormat::Image},
            {"lospec-endesga-32-8x.png", PaletteFormat::Image}};
        for (const auto& [name, format] : files) {
            const ImportResult result = importFixture(name);
            ASSERT_TRUE(result.ok()) << name << ": " << result.error.message;
            EXPECT_EQ(result.format, format) << name;
            EXPECT_EQ(hexesOf(*result.palette), kEndesga32) << name;
            EXPECT_TRUE(result.palette->names.empty()) << name;
        }
        EXPECT_EQ(importFixture("lospec-endesga-32.gpl").palette->name, "Endesga 32");
        EXPECT_EQ(importFixture("lospec-endesga-32.txt").palette->name, "Endesga 32");
        EXPECT_EQ(importFixture("lospec-endesga-32.hex").palette->name, "lospec-endesga-32");
    }

    TEST(PaletteFormats, PaintNetKeepsAlphaAndItsWhiteFill) {
        const ImportResult result = importFixture("paintnet.txt");
        ASSERT_TRUE(result.ok()) << result.error.message;
        EXPECT_EQ(result.format, PaletteFormat::PaintNetTxt);
        ASSERT_EQ(result.palette->count(), 96);
        const std::vector<std::string> first = {"#000000", "#404040", "#ff0000", "#ff6a00", "#ffd800", "#b6ff00",
                                                "#4cff00", "#00ff21", "#00ff90", "#00ffff", "#0094ff", "#0026ff80"};
        const std::vector<std::string> hexes = hexesOf(*result.palette);
        EXPECT_EQ(std::vector<std::string>(hexes.begin(), hexes.begin() + 12), first);
        for (int i = 12; i < 96; i++) EXPECT_EQ(hexes[i], "#ffffff") << i;
        EXPECT_TRUE(hasWarning(result, "duplicate_color"));
    }

    TEST(PaletteFormats, PhotoshopAseGroupsBecomeRamps) {
        const ImportResult result = importFixture("photoshop-skin.ase");
        ASSERT_TRUE(result.ok()) << result.error.message;
        EXPECT_EQ(result.format, PaletteFormat::AdobeAse);
        EXPECT_EQ(hexesOf(*result.palette), (std::vector<std::string>{"#ffccaa", "#e29273", "#ab5236", "#ff0000", "#808080", "#257179"}));
        ASSERT_EQ(result.palette->ramps.size(), 1u);
        EXPECT_EQ(result.palette->ramps[0].name, "Skin");
        EXPECT_EQ(result.palette->ramps[0].colors, (std::vector<uint16_t>{0, 1, 2}));
        EXPECT_EQ(result.palette->colorName(0), "R=255 G=204 B=170");
        EXPECT_EQ(result.palette->colorName(5), "Teal");
        EXPECT_TRUE(hasWarning(result, "cmyk_converted"));
    }

    TEST(PaletteFormats, WindowsRiffPalette) {
        const ImportResult result = importFixture("windows-vga-16.pal");
        ASSERT_TRUE(result.ok()) << result.error.message;
        EXPECT_EQ(result.format, PaletteFormat::RiffPal);
        EXPECT_EQ(hexesOf(*result.palette),
                  (std::vector<std::string>{"#000000", "#800000", "#008000", "#808000", "#000080", "#800080", "#008080", "#c0c0c0",
                                            "#808080", "#ff0000", "#00ff00", "#ffff00", "#0000ff", "#ff00ff", "#00ffff", "#ffffff"}));
    }

    TEST(PaletteFormats, SwatchLibraryInEngineLinear) {
        const ImportResult result = importFixture("swatches.colors");
        ASSERT_TRUE(result.ok()) << result.error.message;
        EXPECT_EQ(result.format, PaletteFormat::SwatchLibrary);
        // linear 0.5 and 0.2 are gamma-2.2 bytes 186 and 123; alpha 0.4 is 102; the HDR glow clamps
        EXPECT_EQ(hexesOf(*result.palette), (std::vector<std::string>{"#ffffff", "#000000", "#ba7b0066", "#ffffba"}));
        EXPECT_EQ(result.palette->name, "Test Swatches");
        EXPECT_EQ(result.palette->colorName(3), "Glow");
        EXPECT_TRUE(hasWarning(result, "hdr_clamped"));
    }

    // Aseprite files saved by Aseprite 1.3.18.6 (make-aseprite-palettes.lua): the frame 0 palette
    // through S44's codec, alpha kept; an indexed sprite's transparent index stays see-through
    TEST(PaletteFormats, AsepriteFilesImportTheirPalette) {
        const ImportResult rgba = importFixture("aseprite-palette-rgba.aseprite");
        ASSERT_TRUE(rgba.ok()) << rgba.error.message;
        EXPECT_EQ(rgba.format, PaletteFormat::Aseprite);
        EXPECT_EQ(hexesOf(*rgba.palette), (std::vector<std::string>{"#1a1c2c", "#5d275d", "#b13e53", "#ef7d57", "#ffcd7580", "#a7f070"}));
        EXPECT_EQ(rgba.palette->transparent, -1);
        EXPECT_EQ(rgba.palette->source.kind, "aseprite");

        const ImportResult indexed = importFixture("aseprite-palette-indexed.aseprite");
        ASSERT_TRUE(indexed.ok()) << indexed.error.message;
        EXPECT_EQ(hexesOf(*indexed.palette), (std::vector<std::string>{"#000000", "#ffffff", "#ff00ff", "#29adff"}));
        EXPECT_EQ(indexed.palette->transparent, 2);

        // a file with the magic that does not read is a parse error naming the place
        std::vector<uint8_t> broken = fixtureBytes("aseprite-palette-rgba.aseprite");
        broken.resize(64);
        const ImportResult truncated = importPalette("broken.aseprite", broken);
        EXPECT_FALSE(truncated.ok());
        EXPECT_EQ(truncated.error.kind, "parse_error");
    }

    TEST(PaletteFormats, AdobeAseAndAsepriteAreToldApartByMagic) {
        const ImportResult aseprite = importFixture("aseprite-palette-rgba.aseprite");
        ASSERT_TRUE(aseprite.ok()) << aseprite.error.message;
        EXPECT_EQ(aseprite.format, PaletteFormat::Aseprite);

        // the content decides, whatever the extension says
        const ImportResult adobe = importPalette("swatches.aseprite", fixtureBytes("lospec-endesga-32.ase"));
        ASSERT_TRUE(adobe.ok()) << adobe.error.message;
        EXPECT_EQ(adobe.format, PaletteFormat::AdobeAse);
        EXPECT_EQ(importPalette("sprite.png", fixtureBytes("aseprite-palette-rgba.aseprite")).format, PaletteFormat::Aseprite);
        EXPECT_EQ(importPalette("colours.txt", fixtureBytes("gimp-sweetie-16.gpl")).format, PaletteFormat::Gpl);
        EXPECT_EQ(importPalette("colours.pal", fixtureBytes("lospec-endesga-32.hex")).format, PaletteFormat::Hex);
    }

    TEST(PaletteFormats, UnsupportedFormatListsTheFormats) {
        const ImportResult text = importPalette("notes.doc", textBytes("Dear diary,\nno colours here.\n"));
        EXPECT_EQ(text.error.kind, "format_unsupported");
        EXPECT_NE(text.error.message.find(".gpl"), std::string::npos) << text.error.message;
        EXPECT_NE(text.error.message.find(".colors"), std::string::npos) << text.error.message;
        const std::vector<uint8_t> binary = {0x00, 0x01, 0x02, 0x03, 0xff, 0x00, 0x10, 0x20};
        EXPECT_EQ(importPalette("blob.bin", binary).error.kind, "format_unsupported");
    }

    TEST(PaletteFormats, TooManyColors) {
        std::string text;
        for (int i = 0; i < 300; i++) {
            char line[16];
            std::snprintf(line, sizeof(line), "%02x%02x%02x\n", i & 255, i >> 8, 7);
            text += line;
        }
        const ImportResult result = importPalette("big.hex", textBytes(text));
        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.error.kind, "too_many_colors");
        EXPECT_NE(result.error.message.find("300"), std::string::npos) << result.error.message;
        EXPECT_EQ(result.error.fix, "Reduce Colours… or Import the first 256");

        const ImportResult first = importPalette("big.hex", textBytes(text), true);
        ASSERT_TRUE(first.ok()) << first.error.message;
        EXPECT_EQ(first.palette->count(), 256);
        EXPECT_EQ(px::toHex(first.palette->colors[255]), "#ff0007");
        EXPECT_TRUE(hasWarning(first, "truncated"));
    }

    TEST(PaletteFormats, ParseErrorsSayWhere) {
        const ImportResult gpl = importPalette("bad.gpl", textBytes("GIMP Palette\nName: Bad\n#\n 12 abc   7\tUntitled\n"));
        EXPECT_EQ(gpl.error.kind, "parse_error");
        EXPECT_NE(gpl.error.message.find("line 4"), std::string::npos) << gpl.error.message;

        const ImportResult hex = importPalette("bad.hex", textBytes("ff0000\n00ff00\nzz00ff\n"));
        EXPECT_EQ(hex.error.kind, "parse_error");
        EXPECT_NE(hex.error.message.find("line 3"), std::string::npos) << hex.error.message;

        const ImportResult jasc = importPalette("short.pal", textBytes("JASC-PAL\r\n0100\r\n5\r\n0 0 0\r\n1 2 3\r\n4 5 6\r\n"));
        EXPECT_EQ(jasc.error.kind, "parse_error");
        EXPECT_NE(jasc.error.message.find("3 of its 5"), std::string::npos) << jasc.error.message;

        const ImportResult header = importPalette("header.gpl", textBytes("Not a palette\n1 2 3\n"));
        EXPECT_EQ(header.error.kind, "parse_error");
        EXPECT_NE(header.error.message.find("line 1"), std::string::npos) << header.error.message;

        // truncated binaries name the byte offset
        std::vector<uint8_t> riff = fixtureBytes("windows-vga-16.pal");
        riff.resize(riff.size() - 10);
        const ImportResult cutRiff = importPalette("cut.pal", riff);
        EXPECT_EQ(cutRiff.error.kind, "parse_error");
        EXPECT_NE(cutRiff.error.message.find("byte"), std::string::npos) << cutRiff.error.message;
        std::vector<uint8_t> ase = fixtureBytes("photoshop-skin.ase");
        ase.resize(40);
        const ImportResult cutAse = importPalette("cut.ase", ase);
        EXPECT_EQ(cutAse.error.kind, "parse_error");
        EXPECT_NE(cutAse.error.message.find("byte"), std::string::npos) << cutAse.error.message;

        EXPECT_EQ(importPalette("empty.gpl", std::vector<uint8_t>()).error.kind, "parse_error");
    }

    TEST(PaletteFormats, BomAndCrlfReadLikeTheirPlainText) {
        const std::string text = "GIMP Palette\nName: Two\nColumns: 2\n#\n255   0   0\tRed\n  0   0 255\tBlue\n";
        std::string windows = "\xEF\xBB\xBF";
        for (char c : text) windows += c == '\n' ? std::string("\r\n") : std::string(1, c);
        const ImportResult plain = importPalette("two.gpl", textBytes(text));
        const ImportResult crlf = importPalette("two.gpl", textBytes(windows));
        ASSERT_TRUE(plain.ok() && crlf.ok()) << crlf.error.message;
        EXPECT_EQ(*plain.palette, *crlf.palette);
        EXPECT_EQ(hexesOf(*crlf.palette), (std::vector<std::string>{"#ff0000", "#0000ff"}));
        EXPECT_EQ(crlf.palette->colorName(1), "Blue");
    }

    TEST(PaletteFormats, EveryWriterRoundTripsThroughItsReader) {
        px::Palette opaque = paletteOf(kSweetie16);
        opaque.names = {{0, "Night"}, {5, "Lime"}};
        const std::pair<PaletteFormat, int> formats[] = {
            {PaletteFormat::Gpl, 1}, {PaletteFormat::Hex, 1}, {PaletteFormat::JascPal, 1}, {PaletteFormat::RiffPal, 1},
            {PaletteFormat::PaintNetTxt, 1}, {PaletteFormat::AdobeAse, 1}, {PaletteFormat::SwatchLibrary, 1},
            {PaletteFormat::PngStrip, 1}, {PaletteFormat::PngStrip, 4}};
        for (const auto& [format, swatchSize] : formats) {
            std::vector<std::string> warnings;
            const std::vector<uint8_t> bytes = exported(opaque, format, warnings, swatchSize);
            EXPECT_TRUE(warnings.empty()) << paletteFormatName(format) << ": " << (warnings.empty() ? "" : warnings[0]);
            const ImportResult back = importPalette(std::string("round-trip") + paletteFormatExtension(format), bytes);
            ASSERT_TRUE(back.ok()) << paletteFormatName(format) << ": " << back.error.message;
            EXPECT_EQ(hexesOf(*back.palette), kSweetie16) << paletteFormatName(format);
            if (format == PaletteFormat::Gpl || format == PaletteFormat::AdobeAse || format == PaletteFormat::SwatchLibrary) {
                EXPECT_EQ(back.palette->names, opaque.names) << paletteFormatName(format);
            }
            // the same palette gives the same bytes
            std::vector<std::string> again;
            EXPECT_EQ(exported(opaque, format, again, swatchSize), bytes) << paletteFormatName(format);
        }

        // the formats that hold alpha keep it; GIMP keeps a transparent slot as an empty slot
        const px::Palette alpha = paletteOf({"#00000000", "#ff004d80", "#29adff", "#12345601"});
        for (PaletteFormat format : {PaletteFormat::Hex, PaletteFormat::PaintNetTxt, PaletteFormat::SwatchLibrary, PaletteFormat::PngStrip}) {
            std::vector<std::string> warnings;
            const ImportResult back = importPalette(std::string("alpha") + paletteFormatExtension(format), exported(alpha, format, warnings));
            ASSERT_TRUE(back.ok()) << paletteFormatName(format) << ": " << back.error.message;
            EXPECT_EQ(hexesOf(*back.palette), hexesOf(alpha)) << paletteFormatName(format);
            EXPECT_TRUE(warnings.empty()) << paletteFormatName(format);
        }
        const px::Palette slots = paletteOf({"#1a1c2c", "#00000000", "#b13e53"});
        std::vector<std::string> slotWarnings;
        const ImportResult slotsBack = importPalette("slots.gpl", exported(slots, PaletteFormat::Gpl, slotWarnings));
        ASSERT_TRUE(slotsBack.ok()) << slotsBack.error.message;
        EXPECT_EQ(hexesOf(*slotsBack.palette), hexesOf(slots));
        EXPECT_TRUE(slotWarnings.empty());

        // the others say what they drop
        for (PaletteFormat format : {PaletteFormat::Gpl, PaletteFormat::JascPal, PaletteFormat::RiffPal, PaletteFormat::AdobeAse}) {
            std::vector<std::string> warnings;
            exported(alpha, format, warnings);
            EXPECT_FALSE(warnings.empty()) << paletteFormatName(format);
        }
    }

    TEST(PaletteFormats, AseGroupsCarryRunRamps) {
        px::Palette palette = paletteOf(kSweetie16);
        palette.ramps.push_back(px::PaletteRamp{"warm", {2, 3, 4}, 0.0f});
        palette.ramps.push_back(px::PaletteRamp{"scattered", {0, 9}, 0.0f});
        std::vector<std::string> warnings;
        const ImportResult back = importPalette("ramps.ase", exported(palette, PaletteFormat::AdobeAse, warnings));
        ASSERT_TRUE(back.ok()) << back.error.message;
        EXPECT_EQ(hexesOf(*back.palette), kSweetie16);
        ASSERT_EQ(back.palette->ramps.size(), 1u);
        EXPECT_EQ(back.palette->ramps[0].name, "warm");
        EXPECT_EQ(back.palette->ramps[0].colors, (std::vector<uint16_t>{2, 3, 4}));
        // the scattered ramp is not a run, so it is not written
        EXPECT_EQ(warnings.size(), 1u);
    }

    TEST(PaletteFormats, PngStripSizeAndDuplicates) {
        const px::Palette palette = paletteOf({"#ff0000", "#00ff00", "#ff0000"});
        std::vector<std::string> warnings;
        const std::vector<uint8_t> png = exported(palette, PaletteFormat::PngStrip, warnings, 8);
        int width = 0, height = 0, components = 0;
        ASSERT_TRUE(stbi_info_from_memory(png.data(), (int)png.size(), &width, &height, &components));
        EXPECT_EQ(width, 24);
        EXPECT_EQ(height, 8);
        // a strip keeps every cell, duplicates included, so indices stay the cells'
        const ImportResult back = importPalette("strip.png", png);
        ASSERT_TRUE(back.ok()) << back.error.message;
        EXPECT_EQ(hexesOf(*back.palette), hexesOf(palette));

        auto bad = exportPalette(palette, PaletteFormat::PngStrip, warnings, 0);
        ASSERT_TRUE(std::holds_alternative<PaletteError>(bad));
        EXPECT_EQ(std::get<PaletteError>(bad).kind, "invalid_value");
    }

    TEST(PaletteFormats, ImageUniqueColoursInScanOrder) {
        const px::Rgba8 red{255, 0, 0, 255}, blue{0, 0, 255, 255}, clear{9, 9, 9, 0}, glass{0, 255, 0, 128};
        // 5x3 is no strip: its unique visible colours, first seen first
        const std::vector<px::Rgba8> pixels = {red, red, blue, clear, red, glass, blue, blue, red, clear, red, red, red, glass, blue};
        const ImportResult result = importPalette("art.png", encodeTestPng(5, 3, pixels));
        ASSERT_TRUE(result.ok()) << result.error.message;
        EXPECT_EQ(result.format, PaletteFormat::Image);
        EXPECT_EQ(hexesOf(*result.palette), (std::vector<std::string>{"#ff0000", "#0000ff", "#00ff0080"}));

        // 8x4 with a two-coloured cell is not a strip either
        std::vector<px::Rgba8> cells(32, red);
        for (int y = 0; y < 4; y++) {
            for (int x = 4; x < 8; x++) cells[(size_t)(y * 8 + x)] = blue;
        }
        cells[0] = blue;
        const ImportResult mixed = importPalette("mixed.png", encodeTestPng(8, 4, cells));
        ASSERT_TRUE(mixed.ok()) << mixed.error.message;
        EXPECT_EQ(hexesOf(*mixed.palette), (std::vector<std::string>{"#0000ff", "#ff0000"}));

        // 300 unique colours
        std::vector<px::Rgba8> many;
        for (int i = 0; i < 300; i++) many.push_back(px::Rgba8{(uint8_t)(i & 255), (uint8_t)(i >> 8), 50, 255});
        const ImportResult tooMany = importPalette("many.png", encodeTestPng(20, 15, many));
        EXPECT_EQ(tooMany.error.kind, "too_many_colors");
        // the count stops one past what a palette holds (a photo's millions are never all kept)
        EXPECT_NE(tooMany.error.message.find("more than 256"), std::string::npos) << tooMany.error.message;
        const ImportResult first = importPalette("many.png", encodeTestPng(20, 15, many), true);
        ASSERT_TRUE(first.ok()) << first.error.message;
        ASSERT_EQ(first.palette->count(), 256);
        EXPECT_EQ(first.palette->colors[255], many[255]);
        ASSERT_EQ(first.warnings.size(), 1u);
        EXPECT_EQ(first.warnings[0].kind, "truncated");

        // a 1024 x 1024 image of 1M colours: the import stops counting at 257
        std::vector<px::Rgba8> photo;
        photo.reserve(1024 * 1024);
        for (int i = 0; i < 1024 * 1024; i++) photo.push_back(px::Rgba8{(uint8_t)(i & 255), (uint8_t)((i >> 8) & 255), (uint8_t)(i >> 16), 255});
        const ImportResult big = importPalette("photo.png", encodeTestPng(1024, 1024, photo));
        EXPECT_EQ(big.error.kind, "too_many_colors");
        EXPECT_NE(big.error.message.find("more than 256"), std::string::npos) << big.error.message;
    }

    TEST(PaletteFormats, ExportLimits) {
        std::vector<std::string> warnings;
        const px::Palette palette = paletteOf(kPico8);
        for (PaletteFormat format : {PaletteFormat::Aseprite, PaletteFormat::Image}) {
            auto result = exportPalette(palette, format, warnings);
            ASSERT_TRUE(std::holds_alternative<PaletteError>(result)) << paletteFormatName(format);
            EXPECT_EQ(std::get<PaletteError>(result).kind, "format_unsupported");
            EXPECT_FALSE(canExportPalette(format));
        }

        std::vector<std::string> hexes;
        for (int i = 0; i < 100; i++) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", i, 255 - i, 40);
            hexes.push_back(hex);
        }
        const px::Palette hundred = paletteOf(hexes);
        std::vector<std::string> swatchWarnings;
        const ImportResult library = importPalette("many.colors", exported(hundred, PaletteFormat::SwatchLibrary, swatchWarnings));
        ASSERT_TRUE(library.ok()) << library.error.message;
        EXPECT_EQ(library.palette->count(), 76);
        EXPECT_EQ(hexesOf(*library.palette), std::vector<std::string>(hexes.begin(), hexes.begin() + 76));
        EXPECT_EQ(swatchWarnings.size(), 1u);
        std::vector<std::string> paintNetWarnings;
        exported(hundred, PaletteFormat::PaintNetTxt, paintNetWarnings);
        EXPECT_EQ(paintNetWarnings.size(), 1u);
    }

    TEST(PaletteFormats, FormatNames) {
        const char* const names[] = {"gpl", "hex", "pal", "pal_riff", "txt", "ase", "aseprite", "image", "colors", "png"};
        for (const char* name : names) {
            PaletteFormat format = PaletteFormat::Gpl;
            ASSERT_TRUE(parsePaletteFormat(name, format)) << name;
            EXPECT_STREQ(paletteFormatName(format), name);
        }
        PaletteFormat format = PaletteFormat::Gpl;
        EXPECT_FALSE(parsePaletteFormat("act", format));
    }

}
