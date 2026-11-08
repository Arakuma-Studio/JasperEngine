// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Shared by the Sprite Atlas tests (S45): member textures written to a scratch folder, and an
// atlas build input made from them without a project (what SpriteAtlasBuilder::gather resolves).

#pragma once

#include "sprite/SpriteAtlasBuilder.h"
#include "sprite/SpriteSheetBuilder.h"

#include "command/type/SetAssetMetaCmd.h"
#include "pixelart/PixelImage.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace jasperengine::editor::sprite::test {

    struct ScratchDir {
        std::filesystem::path root;
        explicit ScratchDir(const std::string& name) {
            std::random_device rd;
            root = std::filesystem::temp_directory_path() / ("je-atlas-" + name + "-" + std::to_string(rd()));
            std::filesystem::create_directories(root / "assets");
        }
        ~ScratchDir() {
            std::error_code ec;
            if (!std::getenv("JASPER_TEST_KEEP_TEMP")) std::filesystem::remove_all(root, ec);
        }
        std::filesystem::path assets() const { return root / "assets"; }
    };

    // w x h, transparent but for an opaque box whose pixels differ by position and seed (and a
    // transparent texel's colour that a lossless page keeps)
    inline pixelart::PixelImage memberImage(int w, int h, pixelart::IntRect opaque, int seed) {
        pixelart::PixelImage image(w, h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const bool in = opaque.contains(x, y);
                image.set(x, y, {static_cast<uint8_t>((x * 37 + seed * 11) & 0xFF), static_cast<uint8_t>((y * 53 + seed * 7) & 0xFF),
                                 static_cast<uint8_t>((seed * 29 + x + y) & 0xFF), static_cast<uint8_t>(in ? 255 - ((x + y + seed) % 3) : 0)});
            }
        }
        return image;
    }

    inline AtlasMemberInput textureMember(const ScratchDir& dir, const std::string& path, const pixelart::PixelImage& image) {
        const std::filesystem::path file = dir.assets() / path;
        std::filesystem::create_directories(file.parent_path());
        image.savePng(file);
        AtlasMemberInput m;
        m.path = path;
        m.absolute = file;
        m.kind = "texture";
        m.stamp = path + std::to_string(image.hash());
        m.pixelArt = true;
        return m;
    }

    // a texture's Sprite Mode written to its meta (as SetAssetMetaCmd would)
    inline void writeSpriteMode(const std::filesystem::path& texture, const TextureSheetSettings& settings) {
        YAML::Node meta;
        meta["guid"] = "0123456789abcdef0123456789abcdef";
        meta["import"]["spriteSheet"] = sheetSettingsToYaml(settings);
        std::ofstream out(SetAssetMetaCmd::metaPathOf(texture));
        out << YAML::Dump(meta) << "\n";
    }

    inline AtlasBuildInput atlasInput(const ScratchDir& dir, std::vector<AtlasMemberInput> members, const std::string& platform = "desktop",
                                      bool write = true) {
        AtlasBuildInput input;
        input.atlasPath = "ui/test.spriteatlas";
        input.doc.guid = Guid::parse("7c0e5d1f3a2b4c6d8e9f0a1b2c3d4e5f");
        for (const AtlasMemberInput& m : members) {
            SpriteAtlasMember member;
            member.path = m.path;
            input.doc.members.push_back(member);
        }
        std::sort(members.begin(), members.end(), [](const AtlasMemberInput& a, const AtlasMemberInput& b) { return a.path < b.path; });
        input.members = std::move(members);
        input.platform = platform;
        input.compression = "lossless";
        input.filter = "point";
        if (write) input.outDir = dir.root / ".jasperengine" / "cache" / "atlas" / "v1" / input.doc.guid.str() / platform;
        return input;
    }

}
