// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The text and geometry tests' AssetResolver: the OFL test fonts of titleart/fonts/ (their
// licences beside them, NOTICE.md), plus files, images and SVGs a test adds in memory; and the
// helpers that make a normalized document from a few members.

#pragma once

#include "TestPaths.h"

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "util/FontFace.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::titleart::test {

    inline std::string titleFontsDir() {
        return (std::filesystem::path(jasperengine::test::testPaths().sourceDir) / "titleart" / "fonts").string();
    }

    inline std::vector<uint8_t> readFileBytes(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // the test fonts by asset path ("fonts/<file>")
    constexpr const char* DELA_GOTHIC = "fonts/DelaGothicOne-Regular.ttf";      // Japanese, vert/vrt2, no palt
    constexpr const char* STICK = "fonts/Stick-Regular.ttf";                    // Japanese, palt/vpal
    constexpr const char* OSWALD = "fonts/Oswald-wght.ttf";                     // variable glyf, wght 200..700
    constexpr const char* SOURCE_SANS = "fonts/SourceSans3-Regular.otf";        // CFF
    constexpr const char* SOURCE_SANS_VF = "fonts/SourceSans3VF-Upright.otf";   // CFF2, wght 200..900

    class TestAssetResolver : public AssetResolver {
    public:
        TestAssetResolver() {
            for (const char* f : {DELA_GOTHIC, STICK, OSWALD, SOURCE_SANS, SOURCE_SANS_VF}) {
                files[f] = readFileBytes((std::filesystem::path(titleFontsDir()) / std::string(f).substr(6)).string());
            }
        }

        void addFile(const std::string& path, std::vector<uint8_t> bytes) {
            files[path] = std::move(bytes);
            revisions[path]++;
        }
        void addText(const std::string& path, const std::string& text) { addFile(path, std::vector<uint8_t>(text.begin(), text.end())); }
        void addImage(const std::string& path, TitleImageData image) {
            images[path] = std::move(image);
            revisions[path]++;
        }
        void setProjectFonts(std::vector<std::string> paths) { projectFontPaths = std::move(paths); }

        ResolvedTitleAsset resolve(const TitleAssetRef& ref, TitleAssetKind) const override {
            ResolvedTitleAsset a;
            if (!files.count(ref.path) && !images.count(ref.path)) return a;
            a.found = true;
            a.path = ref.path;
            a.assetPath = ref.path;
            a.guid = ref.guid;
            const auto r = revisions.find(ref.path);
            a.revision = 1 + (r == revisions.end() ? 0 : r->second);
            return a;
        }
        std::shared_ptr<FontFace> loadFont(const ResolvedTitleAsset& asset, int faceIndex, std::string* error) const override {
            const auto key = std::make_pair(asset.path + "#" + std::to_string(asset.revision), faceIndex);
            const auto hit = fonts.find(key);
            if (hit != fonts.end()) return hit->second;
            const auto f = files.find(asset.path);
            if (f == files.end()) {
                if (error) *error = "no such font";
                return nullptr;
            }
            // as every Title Art resolver: CFF2 faces open for shaping and outlines
            std::shared_ptr<FontFace> face = FontFace::loadMemory(f->second.data(), f->second.size(), faceIndex, true, asset.path,
                                                                  error, FontFaceRasterizer::Optional);
            if (face) fonts[key] = face;
            return face;
        }
        bool readBytes(const ResolvedTitleAsset& asset, std::vector<uint8_t>& out, std::string* error) const override {
            const auto f = files.find(asset.path);
            if (f == files.end()) {
                if (error) *error = "no such file";
                return false;
            }
            out = f->second;
            return true;
        }
        bool loadImage(const ResolvedTitleAsset& asset, TitleImageData& out, std::string* error) const override {
            const auto i = images.find(asset.path);
            if (i == images.end()) {
                if (error) *error = "no such image";
                return false;
            }
            out = i->second;
            return true;
        }
        std::shared_ptr<FontFace> builtinFont() const override { return nullptr; }
        std::vector<ResolvedTitleAsset> projectFonts() const override {
            std::vector<ResolvedTitleAsset> out;
            for (const std::string& p : projectFontPaths) out.push_back(resolve(TitleAssetRef{p, ""}, TitleAssetKind::Font));
            return out;
        }

        // loads of fonts so far (the coverage cache's test)
        size_t loadedFonts() const { return fonts.size(); }

    private:
        std::map<std::string, std::vector<uint8_t>> files;
        std::map<std::string, TitleImageData> images;
        std::map<std::string, uint64_t> revisions;
        std::vector<std::string> projectFontPaths;
        mutable std::map<std::pair<std::string, int>, std::shared_ptr<FontFace>> fonts;
    };

    inline Json fontRef(const std::string& path) { return Json{{"path", path}, {"guid", ""}}; }

    // a normalized document holding `layers` (each its members, `type` included)
    inline TitleArtDoc documentWith(const std::vector<Json>& layers) {
        Json root = Json{{"format", TITLE_ART_FORMAT}, {"version", TITLE_ART_VERSION}};
        root["layers"] = Json::array();
        for (const Json& layer : layers) root["layers"].push_back(layer);
        return parseTitleArtJson(root).doc;
    }

    // an OpenType collection (.ttc) of whole font files, as fontTools' TTCollection writes one:
    // the header, each font's table directory, then the tables (4-byte aligned) with their
    // offsets from the start of the collection
    inline std::vector<uint8_t> buildCollection(const std::vector<std::vector<uint8_t>>& fontFiles) {
        const auto be16 = [](const std::vector<uint8_t>& b, size_t o) { return (uint32_t)(b[o] << 8 | b[o + 1]); };
        const auto be32 = [](const std::vector<uint8_t>& b, size_t o) {
            return (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | (uint32_t)b[o + 3];
        };
        const auto put32 = [](std::vector<uint8_t>& b, size_t o, uint32_t v) {
            b[o] = (uint8_t)(v >> 24);
            b[o + 1] = (uint8_t)(v >> 16);
            b[o + 2] = (uint8_t)(v >> 8);
            b[o + 3] = (uint8_t)v;
        };
        std::vector<uint8_t> out(12 + 4 * fontFiles.size(), 0);
        std::memcpy(out.data(), "ttcf", 4);
        out[4] = 0;
        out[5] = 1;     // version 1.0
        put32(out, 8, (uint32_t)fontFiles.size());
        std::vector<size_t> directories;
        for (size_t f = 0; f < fontFiles.size(); f++) {
            const std::vector<uint8_t>& font = fontFiles[f];
            const size_t tables = be16(font, 4);
            const size_t size = 12 + 16 * tables;
            directories.push_back(out.size());
            put32(out, 12 + 4 * f, (uint32_t)out.size());
            out.insert(out.end(), font.begin(), font.begin() + size);
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

}
