// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Texture sprite sheets (docs/specs/S45-sprite-atlas.md R10, R11, §6.4): a texture's Sprite Mode
// in its meta (`import.spriteSheet`, beside S14's `textureImporter`), the "name@WxH" file name
// convention, the Aseprite json-hash import (Slice... > From JSON), and the editor's answer to
// "<texture>#sheet" (SpriteSheetPool's texture-sheet source reads it through ImportedAssets).
//
//   .jasperengine/cache/imported/v1/sheets/<k[0:2]>/<k>.json   the jasperengine.spritesheet record
//
// k = SHA-256 of (the texture's bytes, its asset path, the resolved settings, the sheet version):
// the record is a pure function of them. Settings: the meta when it has them, else the
// "name@WxH" grid (nothing written), else Single (no record: the pool's one frame).
// Thread model: the settings and build functions are pure (any thread); the provider's memo
// takes its mutex (pool loads ask from job threads).

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"

#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetSources.h"

#include "json.hpp"
#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::sprite {

    constexpr const char* kSpriteSheetKey = "import.spriteSheet";

    const char* spriteModeName(TextureSpriteMode mode);   // "single", "grid", "multiple"
    bool spriteModeFromName(const std::string& name, TextureSpriteMode& out);
    // "bottom_center"..., "frame" is not a texture pivot
    const char* pivotPresetName(PivotPreset preset);
    bool pivotPresetFromName(const std::string& name, PivotPreset& out);

    // The meta form (§6.4); false for a newer version (read-only, problem `invalid_settings`) or a
    // value that is not a map. Bad entries are problems and are left out.
    bool sheetSettingsFromYaml(const YAML::Node& node, TextureSheetSettings& out, std::vector<pixelart::PixelProblem>& problems);
    YAML::Node sheetSettingsToYaml(const TextureSheetSettings& settings);
    // The MCP form: the same keys in snake_case (skip_empty); `base` holds what the JSON leaves out
    bool sheetSettingsFromJson(const nlohmann::json& json, const TextureSheetSettings& base, TextureSheetSettings& out,
                               std::vector<pixelart::PixelProblem>& problems);
    nlohmann::json sheetSettingsToJson(const TextureSheetSettings& settings);

    // Aseprite's --data JSON (json-hash: frames an object; json-array: a list of {filename, frame}):
    // the frames' names and rects in order, the 9-patch slices' centres as borders and their pivots
    bool framesFromAsepriteJson(const std::string& text, std::vector<TextureSheetFrame>& out, pixelart::PixelProblem& problem);

    struct TextureSheetInfo {
        enum class Source { Default, Meta, FileName };
        TextureSheetSettings settings;
        Source source = Source::Default;
        bool readOnly = false;           // a newer meta version
        int cellWidth = 0, cellHeight = 0;   // the file name's grid, when it has one
        std::vector<pixelart::PixelProblem> problems;

        bool isSheet() const { return settings.mode != TextureSpriteMode::SINGLE; }
        const char* sourceName() const { return source == Source::Meta ? "meta" : source == Source::FileName ? "file_name" : "default"; }
    };
    TextureSheetInfo textureSheetSettings(const std::filesystem::path& absoluteTexture);

    struct TextureSheetBuild {
        bool ok = false;
        SpriteSheetData sheet;
        std::vector<std::string> warnings;
        pixelart::PixelProblem error;
        int width = 0, height = 0;
    };
    // The sheet of a texture with these settings (the image read unless given): frames on the
    // texture itself, page texture `assetPath`
    TextureSheetBuild buildTextureSheet(const std::filesystem::path& absoluteTexture, const std::string& assetPath,
                                        const TextureSheetSettings& settings, const pixelart::PixelImage* image = nullptr);

    class SpriteSheetBuilder {
    public:
        static SpriteSheetBuilder& get();

        void attach(Project* project);
        Project* getProject() const { return project; }

        // "<texture>#sheet" of a texture with a Sprite Mode: its record's file (any thread)
        bool resolve(const std::string& assetPath, std::string& fileOut);
        // the texture or its settings changed: its record is made again on the next ask, and the
        // pool's sheet dropped (main thread)
        void invalidate(const std::string& assetPath);

        static constexpr const char* kCacheDir = "sheets";

    private:
        SpriteSheetBuilder() = default;

        struct Memo {
            std::filesystem::file_time_type textureTime, metaTime;
            uintmax_t textureSize = 0, metaSize = 0;
            bool hasMeta = false;
            std::string file;     // "" when the texture is not a sheet
        };

        Project* project = nullptr;
        std::filesystem::path assetsRoot, cacheRoot;
        std::mutex mutex;
        std::map<std::string, Memo> memos;
    };

}
