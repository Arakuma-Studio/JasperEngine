// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Fixtures and comparisons shared by the pixel-art codec tests (editor/tests/fixtures/aseprite:
// files Aseprite wrote, with the PNGs Aseprite exported from them; README.md there).

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/aseprite/AsepriteDocument.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::testing {

    inline std::filesystem::path asepriteFixtures() {
        return std::filesystem::path(JASPER_EDITOR_TEST_FIXTURES) / "aseprite";
    }

    inline std::vector<uint8_t> readBytes(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // every .aseprite / .ase file of the fixture corpus, sorted
    inline std::vector<std::filesystem::path> asepriteCorpus() {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(asepriteFixtures())) {
            const std::string ext = entry.path().extension().string();
            if (entry.is_regular_file() && (ext == ".aseprite" || ext == ".ase")) files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    inline aseprite::Document loadDocument(const std::filesystem::path& path, const aseprite::ReadOptions& options = {}) {
        aseprite::ReadResult r = aseprite::readFile(path, options);
        EXPECT_TRUE(r.ok()) << path << ": " << r.error.kind << ": " << r.error.message;
        return r.ok() ? std::move(*r.document) : aseprite::Document();
    }

    inline PixelImage loadPng(const std::filesystem::path& path) {
        std::string error;
        std::optional<PixelImage> image = PixelImage::loadPng(path, &error);
        EXPECT_TRUE(image.has_value()) << path << ": " << error;
        return image ? *image : PixelImage();
    }

    // Aseprite's PNGs and ours agree when alpha is equal everywhere and RGB where alpha > 0
    // (a fully transparent pixel's colour is not part of the picture). The message names the
    // first difference.
    inline ::testing::AssertionResult samePicture(const PixelImage& got, const PixelImage& expected) {
        if (got.width() != expected.width() || got.height() != expected.height()) {
            return ::testing::AssertionFailure() << "size " << got.width() << "x" << got.height() << ", expected " << expected.width() << "x"
                                                 << expected.height();
        }
        int differing = 0;
        std::string first;
        for (int y = 0; y < got.height(); y++) {
            for (int x = 0; x < got.width(); x++) {
                const PixelRgba a = got.at(x, y), b = expected.at(x, y);
                const bool same = a[3] == b[3] && (b[3] == 0 || (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]));
                if (!same) {
                    if (differing++ == 0) {
                        first = "(" + std::to_string(x) + ", " + std::to_string(y) + ") got " + std::to_string(a[0]) + "," + std::to_string(a[1]) +
                                "," + std::to_string(a[2]) + "," + std::to_string(a[3]) + " expected " + std::to_string(b[0]) + "," +
                                std::to_string(b[1]) + "," + std::to_string(b[2]) + "," + std::to_string(b[3]);
                    }
                }
            }
        }
        if (differing == 0) return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << differing << " differing pixels, first at " << first;
    }

    // The frames side by side, as Aseprite's `--sheet-type horizontal` export lays them out
    inline PixelImage horizontalSheet(const aseprite::Document& doc, const aseprite::RenderOptions& options = {}) {
        PixelImage sheet(doc.width * doc.frameCount(), doc.height);
        for (int f = 0; f < doc.frameCount(); f++) {
            sheet.blit(aseprite::renderFrame(doc, f, options), f * doc.width, 0);
        }
        return sheet;
    }

}
