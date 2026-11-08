// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What an .aseprite file imports to (docs/specs/S44-aseprite-import.md R12-R19, §6.4-§6.7):
// the import settings of its meta (`import.asepriteImporter`) and the pure function from a
// document and those settings to the outputs: the sprite sheet record with its pages, one clip
// per tag, the slices, the user data, the palette and the tilesets handed to S43 and S50, and
// the sub-asset table. No Project, ImGui or GPU: it runs on a job (AsepriteImporter) and in the
// CLI. Output is deterministic.

#pragma once

#include "pixelart/PixelImage.h"
#include "pixelart/PixelProblem.h"
#include "pixelart/aseprite/AsepriteDocument.h"

#include "asset/Guid.h"
#include "asset/ModelSubAssets.h"
#include "asset/SubAssetKeys.h"
#include "component/SpriteComponent.h"
#include "sprite/SpriteSheet.h"

#include "json.hpp"
#include "yaml-cpp/yaml.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart::aseprite {

    // bumped whenever the same file and settings import to different outputs (part of the cache key)
    inline constexpr int kAsepriteImporterVersion = 1;

    enum class ImportMode { SpriteSheet, LayersAsSprites };     // "sprite_sheet" (Unity's Merge Frames), "layers_as_sprites" (Individual Layers)
    enum class PivotSource { Preset, Custom, Slice };
    enum class SheetLayout { Packed, Grid };
    enum class SheetCompression { None, Project };

    // `import.asepriteImporter` (§6.4), every field optional with these defaults
    struct AsepriteImportSettings {
        static constexpr int kVersion = 1;
        int version = kVersion;
        ImportMode importMode = ImportMode::SpriteSheet;
        bool includeHidden = false;
        std::vector<std::string> ignorePrefixes;
        std::vector<std::string> include;       // layer names or paths ("Body/Arm")
        std::vector<std::string> exclude;
        std::optional<int> pixelsPerUnit;        // nullopt: the project's (S41)
        PivotSource pivotSource = PivotSource::Slice;
        std::string pivotSlice;                  // "": a slice named "pivot", else the first with a pivot
        PivotPreset pivotPreset = PivotPreset::BOTTOM_CENTER;
        float customPivot[2] = {0.5f, 0.0f};     // normalized, y from the bottom
        std::string borderSlice;                 // "": the first 9-patch slice; "none": no border
        bool trim = true;
        bool generateClips = true;
        std::vector<std::string> clipTags;       // empty: every tag
        std::string eventsLayer = "events";
        SheetLayout layout = SheetLayout::Packed;
        int padding = 1, extrude = 1, maxSize = 2048;
        SheetCompression compression = SheetCompression::None;
        bool paletteAsset = true;
        // the section as read: unknown keys survive a write (R41)
        YAML::Node raw;

        // a newer version is read with the keys this editor understands, and is not written
        bool readOnly() const { return version > kVersion; }

        // the meta's section (absent: the defaults); false with problems naming the key
        static bool fromYaml(const YAML::Node& node, AsepriteImportSettings& out, std::vector<PixelProblem>& problems);
        YAML::Node toYaml() const;
        // the MCP/CLI form: snake_case keys, the same structure (§6.4)
        nlohmann::json toJson() const;
        // a partial update in MCP form; every bad key or value is a problem (invalid_settings, field = the key)
        bool applyJson(const nlohmann::json& patch, std::vector<PixelProblem>& problems);
        // canonical text of what affects the outputs (the cache key's part)
        std::string fingerprint() const;
    };

    struct AsepriteImportContext {
        std::string sourcePath;      // asset-relative ("sprites/hero.aseprite")
        Guid sourceGuid;
        int projectPixelsPerUnit = 1;
    };

    struct ClipOutput {
        std::string key;             // "animation:walk", "animation#0"
        std::string name;            // the reference's clip part ("walk"; the untagged clip: the file stem)
        int tag = -1;                // index into the sheet's tags (-1: the untagged clip)
        std::string text;            // canonical .anim (anim::writeClip)
        float length = 0.0f;
        std::string wrap;            // "loop" | "once"
        size_t keys = 0;
    };

    struct SubAssetOutput {
        std::string key;
        std::string name;
        SubAssetKind kind = SubAssetKind::Frame;
    };

    // S50's hand-off: a tileset as tiles and its per-tile user data
    struct TilesetOutput {
        std::string key;             // "tileset:<name>" (or "tileset#<i>")
        std::string name;
        uint32_t id = 0;
        int tileWidth = 0, tileHeight = 0, baseIndex = 1, tileCount = 0;
        PixelImage tiles;            // tileWidth x (tileHeight * tileCount), tile 0 first
        std::vector<nlohmann::json> tileUserData;
    };

    // S50's hand-off: one tilemap cel
    struct TilemapCelOutput {
        std::string layer;           // the layer's path
        int frame = 0;
        int x = 0, y = 0;            // pixels
        uint32_t tileset = 0;        // the tileset id
        Tilemap tilemap;
    };

    class AsepriteImport {
    public:
        static AsepriteImport run(const Document& document, const AsepriteImportSettings& settings, const AsepriteImportContext& context);

        bool ok() const { return error.empty(); }
        PixelProblem error;                      // import_failed, unknown_layer (nothing left), ...
        std::vector<PixelProblem> warnings;

        SpriteSheetData sheet;
        std::vector<PixelImage> pages;           // RGBA8, sheet.pages order
        std::vector<ClipOutput> clips;
        std::vector<SubAssetOutput> subAssets;   // every output, in a stable order
        std::vector<PaletteEntry> palette;       // the palette#0 hand-off (empty when not exposed)
        int transparentIndex = -1;
        std::vector<int> layers;                 // the layers imported (§6.7)

        const std::vector<TilesetOutput>& tilesets() const { return tilesetOutputs; }
        const std::vector<TilemapCelOutput>& tilemapCels() const { return tilemapOutputs; }

        // the summary line of the inspector and the Console ("4 tags · 24 frames · 1 page 128x64 · 3 slices · 2 warnings")
        std::string summary() const;
        // {frames, pages: [{size}], clips: [{name, key, length, wrap}], slices, tags, warnings: [...]}
        nlohmann::json toJson() const;

        std::vector<TilesetOutput> tilesetOutputs;
        std::vector<TilemapCelOutput> tilemapOutputs;
    };

    // §6.7: the image and tilemap layers the settings select, by index (warnings name missing layers)
    std::vector<int> selectLayers(const Document& document, const AsepriteImportSettings& settings, std::vector<PixelProblem>& warnings);

    // The sub-assets the asset database keeps for an .aseprite file: the named ones of its tag
    // clips, slices and tilesets (from the file's structure, no pixels). False when it cannot be read.
    bool scanAsepriteSubAssets(const std::filesystem::path& file, std::vector<ModelSubAsset>& out);

    // "onFootstep(\"left\", 1)" -> function and args (.anim's event: a Lua identifier and JSON scalars);
    // false when the text is not a call
    bool parseEventText(const std::string& text, std::string& function, nlohmann::json& args);

}
