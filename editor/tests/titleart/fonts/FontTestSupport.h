// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shared by the Title Designer's font tests (docs/specs/S82-title-designer.md R54-R60): S80's OFL
// test fonts (engine/tests/core/titleart/fonts, licences beside them), the web font fixtures
// (editor/tests/fixtures/titleart/fonts), a scratch folder, an in-memory AssetResolver for S80's
// shaper, glyph outlines through it (HarfBuzz hb-draw), and a .ttc made of whole fonts.

#pragma once

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtIO.h"
#include "titleart/geometry/Path.h"
#include "titleart/text/TitleTextLayout.h"
#include "util/FontFace.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor::titlefonts::test {

    namespace fs = std::filesystem;
    namespace ta = jasperengine::titleart;

    inline fs::path titleFontsDir() { return fs::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "titleart" / "fonts"; }
    inline fs::path webFontFixturesDir() { return fs::path(JASPER_TEST_FIXTURES_DIR) / "titleart" / "fonts"; }

    inline fs::path oswaldPath() { return titleFontsDir() / "Oswald-wght.ttf"; }               // variable glyf, wght 200..700
    inline fs::path delaGothicPath() { return titleFontsDir() / "DelaGothicOne-Regular.ttf"; }  // Japanese
    inline fs::path stickPath() { return titleFontsDir() / "Stick-Regular.ttf"; }               // Japanese
    inline fs::path sourceSansPath() { return titleFontsDir() / "SourceSans3-Regular.otf"; }    // CFF
    inline fs::path sourceSansVfPath() { return titleFontsDir() / "SourceSans3VF-Upright.otf"; } // CFF2

    inline std::vector<uint8_t> readBytes(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    inline void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    }

    // a fresh folder under the temporary directory, removed with the object
    class ScratchDir {
    public:
        explicit ScratchDir(const std::string& name) {
            static int counter = 0;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            dir = fs::temp_directory_path() / ("jasper-title-fonts-" + name + "-" + std::to_string((long long)stamp) + "-" + std::to_string(++counter));
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir);
        }
        ~ScratchDir() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
        ScratchDir(const ScratchDir&) = delete;
        ScratchDir& operator=(const ScratchDir&) = delete;
        const fs::path& path() const { return dir; }

    private:
        fs::path dir;
    };

    // big-endian helpers over font bytes
    inline uint32_t be32(const std::vector<uint8_t>& b, size_t o) {
        return (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3];
    }
    inline uint16_t be16(const std::vector<uint8_t>& b, size_t o) { return (uint16_t)(b[o] << 8 | b[o + 1]); }
    inline void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
        b[o] = (uint8_t)(v >> 24);
        b[o + 1] = (uint8_t)(v >> 16);
        b[o + 2] = (uint8_t)(v >> 8);
        b[o + 3] = (uint8_t)v;
    }
    inline void put16(std::vector<uint8_t>& b, size_t o, uint16_t v) {
        b[o] = (uint8_t)(v >> 8);
        b[o + 1] = (uint8_t)v;
    }

    // a single sfnt's tables by tag
    inline std::map<std::string, std::vector<uint8_t>> sfntTableMap(const std::vector<uint8_t>& font) {
        std::map<std::string, std::vector<uint8_t>> tables;
        if (font.size() < 12) return tables;
        const size_t count = be16(font, 4);
        for (size_t i = 0; i < count && 12 + 16 * (i + 1) <= font.size(); i++) {
            const size_t record = 12 + 16 * i;
            const std::string tag(reinterpret_cast<const char*>(&font[record]), 4);
            const uint32_t offset = be32(font, record + 8), length = be32(font, record + 12);
            if ((size_t)offset + length <= font.size()) tables[tag] = std::vector<uint8_t>(font.begin() + offset, font.begin() + offset + length);
        }
        return tables;
    }

    // the directory offset of a table of a single sfnt (0 when absent)
    inline size_t tableOffset(const std::vector<uint8_t>& font, const char* tag) {
        const size_t count = be16(font, 4);
        for (size_t i = 0; i < count; i++) {
            const size_t record = 12 + 16 * i;
            if (std::memcmp(&font[record], tag, 4) == 0) return be32(font, record + 8);
        }
        return 0;
    }

    // An OpenType collection of whole font files (the header, each font's directory, the tables
    // 4-byte aligned with offsets from the collection's start), as fontTools writes one
    inline std::vector<uint8_t> buildCollection(const std::vector<std::vector<uint8_t>>& fontFiles) {
        std::vector<uint8_t> out(12 + 4 * fontFiles.size(), 0);
        std::memcpy(out.data(), "ttcf", 4);
        out[5] = 1;
        put32(out, 8, (uint32_t)fontFiles.size());
        std::vector<size_t> directories;
        for (size_t f = 0; f < fontFiles.size(); f++) {
            const std::vector<uint8_t>& font = fontFiles[f];
            const size_t size = 12 + 16 * (size_t)be16(font, 4);
            directories.push_back(out.size());
            put32(out, 12 + 4 * f, (uint32_t)out.size());
            out.insert(out.end(), font.begin(), font.begin() + (std::ptrdiff_t)size);
        }
        for (size_t f = 0; f < fontFiles.size(); f++) {
            const std::vector<uint8_t>& font = fontFiles[f];
            const size_t tables = be16(font, 4);
            for (size_t t = 0; t < tables; t++) {
                const size_t record = 12 + 16 * t;
                const uint32_t offset = be32(font, record + 8), length = be32(font, record + 12);
                while (out.size() % 4) out.push_back(0);
                put32(out, directories[f] + record + 8, (uint32_t)out.size());
                out.insert(out.end(), font.begin() + offset, font.begin() + offset + length);
            }
        }
        return out;
    }

    // Fonts held in memory under names, for S80's shaper
    class MemoryFonts : public ta::AssetResolver {
    public:
        void add(const std::string& name, std::vector<uint8_t> bytes) { files[name] = std::move(bytes); }

        ta::ResolvedTitleAsset resolve(const ta::TitleAssetRef& ref, ta::TitleAssetKind) const override {
            ta::ResolvedTitleAsset asset;
            if (!files.count(ref.path)) return asset;
            asset.found = true;
            asset.path = ref.path;
            asset.assetPath = ref.path;
            asset.revision = 1;
            return asset;
        }
        std::shared_ptr<FontFace> loadFont(const ta::ResolvedTitleAsset& asset, int faceIndex, std::string* error) const override {
            const auto key = std::make_pair(asset.path, faceIndex);
            const auto hit = faces.find(key);
            if (hit != faces.end()) return hit->second;
            const auto file = files.find(asset.path);
            if (file == files.end()) return nullptr;
            std::shared_ptr<FontFace> face = FontFace::loadMemory(file->second.data(), file->second.size(), faceIndex, true, asset.path, error,
                                                                  FontFaceRasterizer::Optional);
            if (face) faces[key] = face;
            return face;
        }
        bool readBytes(const ta::ResolvedTitleAsset&, std::vector<uint8_t>&, std::string*) const override { return false; }
        bool loadImage(const ta::ResolvedTitleAsset&, ta::TitleImageData&, std::string*) const override { return false; }
        std::shared_ptr<FontFace> builtinFont() const override { return nullptr; }

    private:
        std::map<std::string, std::vector<uint8_t>> files;
        mutable std::map<std::pair<std::string, int>, std::shared_ptr<FontFace>> faces;
    };

    // each cluster's glyph outlines of `text` set in the font named `font` (S80's shaper and
    // hb-draw), with optional variations
    inline std::vector<ta::geometry::Path> glyphOutlines(MemoryFonts& fonts, const std::string& font, const std::string& text,
                                                         const ta::Json& variations = ta::Json::object()) {
        ta::Json layer = {{"type", "text"}, {"id", "0000000a"}, {"text", text}, {"font", ta::Json{{"path", font}, {"guid", ""}}}};
        layer["variations"] = variations;
        ta::Json root = {{"format", ta::TITLE_ART_FORMAT}, {"version", ta::TITLE_ART_VERSION}};
        root["layers"] = ta::Json::array();
        root["layers"].push_back(layer);
        const ta::TitleArtParseResult parsed = ta::parseTitleArtJson(root);
        std::vector<ta::geometry::Path> out;
        if (!parsed.ok) return out;
        ta::TitleTextLayout layout(&fonts);
        const ta::LaidOutText& laid = layout.layout(parsed.doc.layers()[0]);
        for (const ta::LaidOutCluster& cluster : laid.clusters) {
            for (const ta::LaidOutGlyph& glyph : cluster.glyphs) out.push_back(glyph.outline);
        }
        return out;
    }

    // why two outlines differ ("" when they are the same command by command, point by point)
    inline std::string outlineDifference(const ta::geometry::Path& a, const ta::geometry::Path& b) {
        if (a.verbList() != b.verbList()) return "commands differ (" + std::to_string(a.verbList().size()) + " vs " + std::to_string(b.verbList().size()) + ")";
        const auto& pa = a.pointList();
        const auto& pb = b.pointList();
        if (pa.size() != pb.size()) return "point counts differ";
        for (size_t i = 0; i < pa.size(); i++) {
            if (pa[i].x != pb[i].x || pa[i].y != pb[i].y) return "point " + std::to_string(i) + " differs";
        }
        return "";
    }

}
