// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The exporter ships PNGs without the editor's own text chunks (util/PngEditorChunks.h,
// Exporter::copyPngWithoutEditorChunks): the sprite normals recipe and paint layer and the texture
// synthesis recipe go, every other chunk stays byte for byte, the pixels decode the same; PNGs
// without such a chunk and damaged streams are copied as they are (each file read once); other
// files are left to the exporter's plain copy.

#include <gtest/gtest.h>

#include "Exporter.h"
#include "texsynth/SynthImage.h"
#include "util/PngEditorChunks.h"

#include "support/PosixCompat.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine::editor;
namespace fs = std::filesystem;
namespace ts = jasperengine::editor::texsynth;

namespace {

    uint32_t crc32(const uint8_t* data, size_t size) {
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; i++) {
            c ^= data[i];
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
        return c ^ 0xFFFFFFFFu;
    }

    std::vector<uint8_t> chunk(const char* type, const std::string& data) {
        std::vector<uint8_t> out;
        const uint32_t n = static_cast<uint32_t>(data.size());
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(n >> s));
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        const uint32_t crc = crc32(out.data() + 4, out.size() - 4);
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(crc >> s));
        return out;
    }

    // a text chunk's data: keyword, `zeros` NUL bytes (tEXt: the separator; iTXt: separator,
    // compression flag and method, empty language and translated keyword), text
    std::string text(const char* keyword, int zeros, const char* body) {
        return std::string(keyword) + std::string(static_cast<size_t>(zeros), '\0') + body;
    }

    // `png` with `extra` inserted right after IHDR (8 + 25 bytes)
    std::vector<uint8_t> withChunks(const std::vector<uint8_t>& png, const std::vector<std::vector<uint8_t>>& extra) {
        std::vector<uint8_t> out(png.begin(), png.begin() + 33);
        for (const std::vector<uint8_t>& c : extra) out.insert(out.end(), c.begin(), c.end());
        out.insert(out.end(), png.begin() + 33, png.end());
        return out;
    }

    bool contains(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& part) {
        return std::search(bytes.begin(), bytes.end(), part.begin(), part.end()) != bytes.end();
    }

    std::vector<uint8_t> plainPng() {
        ts::Image image = ts::Image::make(8, 6, 4, 8);
        for (size_t i = 0; i < image.data.size(); i++) image.data[i] = static_cast<uint8_t>(i * 7);
        std::vector<uint8_t> png;
        EXPECT_TRUE(ts::encodePng(image, png));
        return png;
    }

}

TEST(PngEditorChunks, DropsOnlyTheEditorTextChunks) {
    const std::vector<uint8_t> png = plainPng();
    const std::vector<uint8_t> recipe = chunk("iTXt", text("jasper.spritenormals", 5, "{\"version\":1}"));
    const std::vector<uint8_t> paint = chunk("iTXt", text("jasper.spritenormals.paint", 5, "{}"));
    const std::vector<uint8_t> synthesis = chunk("tEXt", text("jasper.synthesis", 1, "{}"));
    const std::vector<uint8_t> comment = chunk("tEXt", text("Comment", 1, "made by hand"));
    const std::vector<uint8_t> foreign = chunk("iTXt", text("jasperish", 5, "x"));
    const std::vector<uint8_t> tagged = withChunks(png, {recipe, comment, paint, synthesis, foreign});

    std::vector<uint8_t> stripped;
    ASSERT_TRUE(pngchunks::stripEditorChunks(tagged, stripped));
    EXPECT_FALSE(contains(stripped, recipe));
    EXPECT_FALSE(contains(stripped, paint));
    EXPECT_FALSE(contains(stripped, synthesis));
    EXPECT_TRUE(contains(stripped, comment)) << "another tool's chunk stays, CRC included";
    EXPECT_TRUE(contains(stripped, foreign)) << "only the \"jasper.\" prefix goes";
    EXPECT_EQ(stripped, withChunks(png, {comment, foreign})) << "the rest byte for byte";
    ts::Image a, b;
    std::string error;
    ASSERT_TRUE(ts::loadImageFromMemory(tagged.data(), tagged.size(), a, error)) << error;
    ASSERT_TRUE(ts::loadImageFromMemory(stripped.data(), stripped.size(), b, error)) << error;
    EXPECT_EQ(a.data, b.data);

    // nothing to drop, not a PNG, a damaged stream: left to a plain copy
    std::vector<uint8_t> untouched = {1, 2, 3};
    EXPECT_FALSE(pngchunks::stripEditorChunks(png, untouched));
    EXPECT_EQ(untouched, (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_FALSE(pngchunks::stripEditorChunks(std::vector<uint8_t>(64, 0), untouched));
    std::vector<uint8_t> truncated(tagged.begin(), tagged.begin() + 60);
    EXPECT_FALSE(pngchunks::stripEditorChunks(truncated, untouched));
}

TEST(PngEditorChunks, TheExporterCopiesPngsWithoutThem) {
    const fs::path dir = fs::temp_directory_path() / ("jasper-png-chunks-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::vector<uint8_t> png = plainPng();
    const std::vector<uint8_t> recipe = chunk("iTXt", text("jasper.spritenormals", 5, "{}"));
    auto write = [](const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    write(dir / "hero_normal.PNG", withChunks(png, {recipe}));
    write(dir / "plain.png", png);
    write(dir / "notes.txt", recipe);
    EXPECT_TRUE(Exporter::copyPngWithoutEditorChunks(dir / "hero_normal.PNG", dir / "out.png"));
    EXPECT_EQ(read(dir / "out.png"), png);
    EXPECT_TRUE(Exporter::copyPngWithoutEditorChunks(dir / "plain.png", dir / "out2.png")) << "nothing to strip: the bytes as read";
    EXPECT_EQ(read(dir / "out2.png"), png);
    write(dir / "broken.png", std::vector<uint8_t>(png.begin(), png.begin() + 40));
    EXPECT_TRUE(Exporter::copyPngWithoutEditorChunks(dir / "broken.png", dir / "out4.png"));
    EXPECT_EQ(read(dir / "out4.png"), std::vector<uint8_t>(png.begin(), png.begin() + 40)) << "a damaged PNG as it is";
    EXPECT_FALSE(Exporter::copyPngWithoutEditorChunks(dir / "notes.txt", dir / "out3.txt"));
    fs::remove_all(dir);
}
