// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A model's import settings (spec S53 R8): the `import.modelImporter` section of its S04 meta
// sidecar, next to `import.textureImporter`, in Unity's Model Importer names. Absent fields
// resolve to the project's per-platform defaults (Project Settings > Mesh), then to the built-in
// ones; a platform block that overrides wins for its platform. The section is sparse: nothing
// is written for a model nobody edited, so opening an old project changes no file. Unknown keys
// survive a round trip.
//
//   import:
//     modelImporter:
//       version: 1
//       meshCompression: medium
//       generateLods: on
//       platforms:
//         mobile: {override: true, lodCount: 2, lodReduction: smallest}

#pragma once

#include "yaml-cpp/yaml.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // The cook targets of models. Geometry has no per-GPU format, so the texture families'
    // Android ETC2 split does not exist here: one mobile cook serves Android and iOS.
    enum class ModelFamily : uint8_t { Desktop, Mobile, Web };
    constexpr size_t kModelFamilyCount = 3;

    enum class OptimizeMesh : uint8_t { Everything, PolygonOrder, VertexOrder, Nothing };
    enum class MeshCompression : uint8_t { Off, Lossless, Low, Medium, High };
    enum class IndexFormat : uint8_t { Auto, Bits16, Bits32 };
    enum class ModelReadWrite : uint8_t { Automatic, On };
    enum class ModelNormals : uint8_t { Import, Calculate };
    enum class GenerateLods : uint8_t { Automatic, On, Off };
    enum class LodReduction : uint8_t { PreserveDetail, Balanced, Smallest };

    const char* modelFamilyName(ModelFamily family);              // "desktop"
    bool modelFamilyFromName(const std::string& name, ModelFamily& family);
    const char* modelFamilyLabel(ModelFamily family);             // "Desktop"
    const char* optimizeMeshName(OptimizeMesh value);             // "polygon_order"
    const char* meshCompressionName(MeshCompression value);       // "lossless"
    const char* indexFormatName(IndexFormat value);               // "auto", "u16", "u32"
    const char* modelReadWriteName(ModelReadWrite value);
    const char* modelNormalsName(ModelNormals value);
    const char* generateLodsName(GenerateLods value);             // "automatic", "on", "off"
    const char* lodReductionName(LodReduction value);             // "preserve_detail"
    // the Inspector's words ("Polygon Order", "16 bit", "Preserve Detail")
    const char* optimizeMeshLabel(OptimizeMesh value);
    const char* meshCompressionLabel(MeshCompression value);
    const char* indexFormatLabel(IndexFormat value);
    const char* lodReductionLabel(LodReduction value);

    // The allowed values of an enum setting (its meta key), for errors that list them
    const std::vector<std::string>& modelEnumValues(const std::string& field);

    // Every setting of R8, each optional: what a model, a platform block or a project default
    // sets (absent: the next level decides)
    struct ModelSettingValues {
        std::optional<OptimizeMesh> optimizeMesh;
        std::optional<MeshCompression> meshCompression;
        std::optional<bool> weldVertices;
        std::optional<IndexFormat> indexFormat;
        std::optional<ModelReadWrite> readWrite;
        std::optional<ModelNormals> normals;
        std::optional<float> smoothingAngle;       // degrees, 0-180
        std::optional<GenerateLods> generateLods;
        std::optional<int> lodCount;               // levels after LOD 0, 1-3
        std::optional<LodReduction> lodReduction;
        std::optional<bool> shipSource;

        bool empty() const;
        bool operator==(const ModelSettingValues&) const = default;
    };

    struct ModelPlatformSettings {
        bool override = false;
        ModelSettingValues values;

        bool empty() const { return !override && values.empty(); }
        bool operator==(const ModelPlatformSettings&) const = default;
    };

    // When a model keeps its CPU copy for scripts (R11, the risk default of S53 §9): automatic
    // follows the project-wide scan of the scripts' mesh-reading calls
    enum class ScriptMeshData : uint8_t { Automatic, Keep, Release };
    const char* scriptMeshDataName(ScriptMeshData value);

    // Project Settings > Animation > Animation Entities (S74 R38): whether a model placed in a scene
    // makes an Animation entity per clip. Always is what every project did before; Auto, which new
    // projects get, places a model of kAutoAnimationEntitiesClips or more clips with
    // ModelComponent::animationEntities false (its clips play through an Animation Player or an
    // Animator). A missing key is Always, so an older project keeps its behaviour.
    enum class AnimationEntitiesMode : uint8_t { Always, Auto };
    const char* animationEntitiesModeName(AnimationEntitiesMode value);    // "always", "auto"
    inline constexpr int kAutoAnimationEntitiesClips = 5;

    // A platform's render budget (R25): 0 is no limit for that metric
    struct RenderBudget {
        long long triangles = 0;        // the main view's, as the Game view draws a frame
        long long drawCalls = 0;
        double meshMemoryMB = 0.0;      // the scene's models, as their cooks hold them on the GPU
        double downloadMB = 0.0;        // the scene's cooked model files

        bool empty() const { return triangles == 0 && drawCalls == 0 && meshMemoryMB <= 0.0 && downloadMB <= 0.0; }
        bool operator==(const RenderBudget&) const = default;
    };

    // Project Settings > Mesh (R19), Quality's render budgets (R25) and Animation's Animation
    // Entities (S74 R38): project.yaml's `mesh:`
    struct ModelProjectSettings {
        // the defaults of every model setting a model does not set, per platform
        std::array<ModelSettingValues, kModelFamilyCount> defaults;
        int cacheSizeMB = 2048;
        ScriptMeshData scriptMeshData = ScriptMeshData::Automatic;
        std::array<RenderBudget, kModelFamilyCount> renderBudgets;
        AnimationEntitiesMode animationEntities = AnimationEntitiesMode::Always;

        bool operator==(const ModelProjectSettings&) const = default;
    };

    // Everything one cook uses, every value set: hashed into the cache key (Read/Write only
    // decides what a player releases, so it is not part of the key)
    struct ResolvedModelSettings {
        OptimizeMesh optimizeMesh = OptimizeMesh::Everything;
        MeshCompression meshCompression = MeshCompression::Lossless;
        bool weldVertices = true;
        IndexFormat indexFormat = IndexFormat::Auto;
        ModelReadWrite readWrite = ModelReadWrite::Automatic;
        ModelNormals normals = ModelNormals::Import;
        float smoothingAngle = 60.0f;
        GenerateLods generateLods = GenerateLods::Automatic;
        int lodCount = 3;
        LodReduction lodReduction = LodReduction::Balanced;
        bool shipSource = false;

        // stable text of every field the cook reads, for the cache key
        std::string canonical() const;
        bool operator==(const ResolvedModelSettings&) const = default;
    };

    class ModelImportSettings {
    public:
        static constexpr int currentVersion = 1;
        static constexpr const char* metaKey = "import.modelImporter";

        int version = currentVersion;
        ModelSettingValues values;
        std::array<ModelPlatformSettings, kModelFamilyCount> platforms;
        // keys this version does not know, kept as they were
        YAML::Node unknown;

        // false with a reason on a malformed section (a wrong type, an unknown enum value); a
        // section of an older version is read as this one (version 1 is the first)
        static bool parse(const YAML::Node& node, ModelImportSettings& out, std::string* error = nullptr);
        // the sparse section: `version` and what is set
        YAML::Node serialize() const;
        bool operator==(const ModelImportSettings& other) const;
        // nothing set (the section can go)
        bool isDefault() const;

        // The meta section of an asset (absolute path), and whether there was one
        static bool load(const std::string& absoluteAsset, ModelImportSettings& out, std::string* error = nullptr);

        ResolvedModelSettings resolve(ModelFamily family, const ModelProjectSettings& project) const;
    };

    // One setting checked alone (MCP, the importer): `field` is its meta key ("lodCount"); the
    // error names the field and its allowed values
    bool validateModelSettingsField(const std::string& field, const YAML::Node& value, std::string& error);
    // the meta keys of R8's settings, in the Inspector's order
    const std::vector<std::string>& modelSettingKeys();

    // project.yaml's `mesh:`: only what differs from the defaults is written (an empty map when
    // nothing does); decoding clamps out-of-range values and skips unknown names
    YAML::Node encodeModelProjectSettings(const ModelProjectSettings& settings);
    void decodeModelProjectSettings(const YAML::Node& node, ModelProjectSettings& settings);

}
