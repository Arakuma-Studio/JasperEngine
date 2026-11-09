// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A texture's import settings (spec S14 R1, R30): the `import.textureImporter` section of its
// S04 meta sidecar, Unity's Texture Importer in our names. Absent fields resolve to the type's
// defaults, then the project's per-family defaults; a platform tab that overrides wins for its
// family. Unknown keys survive a round trip.
//
//   import:
//     textureImporter:
//       version: 1
//       type: normal
//       inferredFrom: "glTF normalTexture of models/hero.gltf"
//       maxSize: 1024
//       platforms:
//         mobile: {override: true, quality: low}

#pragma once

#include "texture/MipChain.h"
#include "texture/TextureFilterMode.h"
#include "texture/TexturePrepare.h"

#include "yaml-cpp/yaml.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // the cook targets (D3): one desktop cook serves GL, D3D11, Vulkan and Metal on macOS
    enum class TextureFamily : uint8_t { Desktop, Mobile, AndroidEtc2, Web };
    constexpr size_t kTextureFamilyCount = 4;
    enum class TextureCookQuality : uint8_t { None, Low, Normal, High };
    enum class TextureEffort : uint8_t { Fast, Normal, Best };
    // Unity's Texture Shape: a single file holds a 2D texture or a cubemap (a 4:3 / 3:4 cross)
    enum class TextureShape : uint8_t { Tex2D, Cube };

    // a number as the cache keys print it: fixed, six decimals at most, locale-free ("0.5", "-1")
    std::string textureCanonicalNumber(float value);
    const char* textureFamilyName(TextureFamily family);          // "desktop", "android_etc2"
    bool textureFamilyFromName(const std::string& name, TextureFamily& family);
    const char* textureFamilyLabel(TextureFamily family);         // "Desktop", "Android ETC2"
    const char* textureQualityName(TextureCookQuality quality);
    const char* textureEffortName(TextureEffort effort);
    const char* mipFilterName(MipFilter filter);
    const char* alphaSourceName(TextureAlphaSource source);
    const char* textureChannelName(TextureChannel channel);
    const char* textureShapeName(TextureShape shape);

    // The allowed values of each enum field, for errors that list them (MCP, CLI)
    const std::vector<std::string>& textureEnumValues(const std::string& field);

    struct TexturePlatformSettings {
        bool override = false;
        std::optional<int> maxSize;
        std::optional<std::string> format;       // a GpuTextureFormat name, "ETC1S", "UASTC" or "UASTC_HDR"
        std::optional<TextureCookQuality> quality;
        std::optional<TextureEffort> effort;
        std::optional<float> rdo;                // the RDO lambda (BC7, UASTC), the ETC1S quality level

        bool empty() const { return !override && !maxSize && !format && !quality && !effort && !rdo; }
        bool operator==(const TexturePlatformSettings&) const = default;
    };

    // Project Settings > Texture (R24): what the families default to
    struct TextureProjectSettings {
        std::array<TextureCookQuality, kTextureFamilyCount> defaultQuality = {TextureCookQuality::Normal, TextureCookQuality::Normal,
                                                                              TextureCookQuality::Normal, TextureCookQuality::Normal};
        std::array<int, kTextureFamilyCount> defaultMaxSize = {2048, 2048, 2048, 2048};
        bool androidEtc2Fallback = false;
        // RGB565 / RGBA4 last resort (R11): desktop, mobile, web
        std::array<bool, 3> lowMemoryFallback = {false, false, true};
        // runtime quality (0 full ... 3 an eighth) and memory budget (MB, 0 = none): desktop, mobile, web
        std::array<int, 3> textureQuality = {0, 0, 0};
        std::array<int, 3> memoryBudgetMB = {0, 512, 384};
        int cacheSizeMB = 4096;
        std::string globalCacheDir;     // shared by every project (keys are content addresses)
        // the project's power-of-two strategy (Engine::getTextureStrategy) at cook time
        TextureSizeStrategy strategy = TextureSizeStrategy::None;

        bool operator==(const TextureProjectSettings&) const = default;
    };

    // Everything a cook of one family uses, every value set: hashed into the cache key
    struct ResolvedTextureSettings {
        TextureImportType type = TextureImportType::Default;
        TextureShape shape = TextureShape::Tex2D;
        bool srgb = true;
        TextureAlphaSource alphaSource = TextureAlphaSource::Input;
        bool alphaIsTransparency = false;
        bool readable = false;
        int maxSize = 2048;
        MipFilter resizeFilter = MipFilter::Mitchell;
        bool mipmaps = true;
        MipFilter mipFilter = MipFilter::Mitchell;
        int mipLimit = 0;
        bool preserveCoverage = false;
        float alphaCutoff = 0.5f;
        bool alphaClip = false;         // S42 R6: binary alpha at alphaCutoff
        bool flipGreen = false;
        TextureChannel channel = TextureChannel::R;
        TextureCookQuality quality = TextureCookQuality::Normal;
        TextureEffort effort = TextureEffort::Normal;
        bool streamable = true;
        std::string format;             // an explicit format, empty = the R5 table
        float rdo = -1.0f;              // an explicit lambda / ETC1S level, < 0 = the table's
        // from the references at cook time (R3): never block-compressed, CPU-addressable
        bool cpuReadable = false;
        // why it is CPU-readable ("TerrainComponent.heightMap of scenes/main.scene")
        std::string cpuReason;
        TextureSizeStrategy strategy = TextureSizeStrategy::None;

        // the processing of the levels (the raw path takes the same)
        TextureProcessing processing() const;
        // stable text of every field, for the cache key
        std::string canonical() const;
    };

    class TextureImportSettings {
    public:
        static constexpr int currentVersion = 1;

        int version = currentVersion;
        std::optional<TextureImportType> type;
        std::string inferredFrom;
        std::optional<TextureShape> shape;
        std::optional<bool> srgb;
        std::optional<TextureAlphaSource> alphaSource;
        std::optional<bool> alphaIsTransparency;
        std::optional<bool> readable;
        std::optional<int> maxSize;
        std::optional<MipFilter> resizeFilter;
        std::optional<bool> mipmaps;
        std::optional<MipFilter> mipFilter;
        std::optional<int> mipLimit;
        std::optional<bool> preserveCoverage;
        std::optional<float> alphaCutoff;
        std::optional<bool> flipGreen;
        std::optional<TextureChannel> channel;
        std::optional<TextureCookQuality> quality;
        std::optional<TextureEffort> effort;
        std::optional<bool> streamable;
        std::optional<bool> shipSource;
        // S42 R4, R6, R7: the Pixel Art switch (absent fields resolve to the Pixel Art defaults),
        // binary alpha, and what an assignment copies onto a reference (Filter Mode) and onto a
        // Sprite or Tilemap (Pixels Per Unit, 1...256)
        std::optional<bool> pixelArt;
        std::optional<bool> alphaClip;
        std::optional<TextureFilterMode> filterMode;
        std::optional<int> pixelsPerUnit;
        std::array<TexturePlatformSettings, kTextureFamilyCount> platforms;
        // keys this version does not know, kept as they were
        YAML::Node unknown;

        // false with a reason on a malformed section (a wrong type, an unknown enum value)
        static bool parse(const YAML::Node& node, TextureImportSettings& out, std::string* error = nullptr);
        YAML::Node serialize() const;
        bool operator==(const TextureImportSettings& other) const;

        // The meta section of an asset (absolute path), and whether there was one
        static bool load(const std::string& absoluteAsset, TextureImportSettings& out, std::string* error = nullptr);
        static constexpr const char* metaKey = "import.textureImporter";

        TextureImportType resolvedType() const;
        ResolvedTextureSettings resolve(TextureFamily family, const TextureProjectSettings& project) const;
        bool resolvedPixelArt() const { return pixelArt.value_or(false); }
        // absent: Point under Pixel Art, else the project's Default Filter Mode
        TextureFilterMode resolvedFilterMode(TextureFilterMode projectDefault) const;
        // absent: the project's Pixels Per Unit
        int resolvedPixelsPerUnit(int projectPixelsPerUnit) const { return pixelsPerUnit.value_or(projectPixelsPerUnit); }
        // the type's own default of a field (what an absent value means)
        static bool defaultSrgb(TextureImportType type);
        static bool defaultMipmaps(TextureImportType type);
        static bool defaultAlphaIsTransparency(TextureImportType type);
    };

    // Range and enum checks of a partial update (MCP set_texture_import_settings): the error
    // names the field and its allowed values
    // project.yaml's `texture:` (Project Settings > Texture): only what differs from the
    // defaults is written (an empty map when nothing does); decoding clamps out-of-range values
    YAML::Node encodeTextureProjectSettings(const TextureProjectSettings& settings);
    void decodeTextureProjectSettings(const YAML::Node& node, TextureProjectSettings& settings);
    // the runtime platform groups of quality, budget and the low-memory fallback
    const char* texturePlatformGroupName(size_t group);     // "desktop", "mobile", "web"

    bool validateTextureSettingsField(const std::string& field, const YAML::Node& value, std::string& error);

}
