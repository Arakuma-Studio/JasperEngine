// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The content-addressed cache of cooked models (spec S53 R12), laid out as the texture cache:
//   .jasperengine/cache/models/v1/<k[0:2]>/<k>.glb       the cooked file
//   .jasperengine/cache/models/v1/<k[0:2]>/<k>.json      its record (the measured numbers)
//   .jasperengine/cache/models/v1/sources.json           path, size, mtime -> SHA-256 of its bytes
// k = SHA-256 of the source's SHA-256 (the main file and its external buffers), the resolved
// settings, the family, the submeshes drawn blended, kModelCookVersion, meshoptimizer's version
// and the LOD recipe. A rename never recooks and duplicates share one artifact. Writes are atomic
// (S03), the record last; an LRU sweep keeps the cache under its cap (use = the files' mtime).

#pragma once

#include "model/ModelImportSettings.h"

#include "mcp/McpJson.h"
#include "subsystem/GltfNormalize.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    using mcp::Json;

    // bumped when the cook's output changes for the same inputs (a pass, the layout it writes,
    // the codec parameters): every model cooks again through the cache key
    constexpr int kModelCookVersion = 3;

    // geometry as the GPU holds it
    struct ModelGeometryStats {
        long long triangles = 0;
        long long vertices = 0;
        uint64_t vertexBytes = 0;
        uint64_t indexBytes = 0;
        double acmr = 0.0;          // transformed vertices per triangle (16-entry FIFO); 0 when unknown
        double overdraw = 0.0;      // shaded / covered pixels from six axis views; 0 when unknown

        uint64_t memoryBytes() const { return vertexBytes + indexBytes; }
        Json toJson() const;
        static ModelGeometryStats fromJson(const Json& json);
    };

    // one baked level, over every primitive that has it
    struct ModelLodLevelStats {
        int level = 1;
        long long triangles = 0;
        double share = 0.0;         // of LOD 0's triangles, over the primitives with levels
        double error = 0.0;         // the simplifier's largest error, in mesh units (what selection uses)
        double deviation = 0.0;     // the largest sampled distance from the source surface, as a share of the model's size

        Json toJson() const;
        static ModelLodLevelStats fromJson(const Json& json);
    };

    // A cooked artifact's record (<k>.json): every number the Inspector, MCP and the Build Report show
    struct ModelCookRecord {
        std::string key;
        std::string family;
        std::string compression;            // "lossless"
        uint64_t sourceBytes = 0;           // the source file and its external buffers
        uint64_t diskBytes = 0;             // the cooked .glb
        ModelGeometryStats before, after;
        double duplicatePercent = 0.0;      // share of the source's vertices that were exact duplicates
        int meshes = 0;
        int primitives = 0;
        int lodPrimitives = 0;              // primitives with baked levels
        std::vector<ModelLodLevelStats> lods;
        double modelSize = 0.0;             // the diagonal of the model's bounds, in mesh units
        double positionError = 0.0;         // the largest position change of Mesh Compression, mesh units
        bool passThrough = false;           // the file ships as it is (its reason is a warning)
        std::vector<std::string> requiredExtensions;    // of the cooked file
        std::vector<std::string> sourceExtensions;      // the source's required ones
        std::vector<std::string> keptOrder;             // "Body/1 (blended)": triangle order kept
        std::vector<std::string> warnings;
        // what the loader finds in the source (S53 R5), and the editor-only extensions the cook
        // took out of it (R6: Draco, WebP, a Basis texture's PNG source)
        std::vector<GltfDiagnostic> diagnostics;
        std::vector<std::string> transcoded;
        int authoredLodPrimitives = 0;                  // primitives whose levels are MSFT_lod's (R4)
        double cookMs = 0.0;

        // "None" or "Up to 0.02% of the model's size"
        std::string visualChange() const;
        Json toJson() const;
        static bool fromJson(const Json& json, ModelCookRecord& out);
    };

    // a load finding as the records and MCP write it (severity "error", "warning" or "info")
    Json modelDiagnosticJson(const GltfDiagnostic& diagnostic);
    bool modelDiagnosticFromJson(const Json& json, GltfDiagnostic& out);

    class ModelCookCache {
    public:
        // `directory` is <project>/.jasperengine/cache/models/v1
        void open(const std::filesystem::path& directory, uint64_t capBytes);
        void close();
        bool isOpen() const;
        const std::filesystem::path& directory() const { return dir; }

        // SHA-256 of a source's bytes (the main file, then `extra` files in order), from the
        // sources table while their sizes and mtimes stay; false when one cannot be read
        bool sourceHash(const std::filesystem::path& absolute, const std::vector<std::filesystem::path>& extra, std::string& sha256);
        void saveSources();

        static std::string key(const std::string& sourceSha256, const ResolvedModelSettings& settings, ModelFamily family,
                               const std::set<std::pair<int, int>>& blended);

        std::filesystem::path artifactPath(const std::string& key, const char* suffix = ".glb") const;
        bool has(const std::string& key) const;
        // the record of a cooked key; touches its files (LRU)
        bool load(const std::string& key, ModelCookRecord& record);
        bool readArtifact(const std::string& key, std::vector<uint8_t>& bytes) const;
        bool store(const std::string& key, const std::vector<uint8_t>& glb, const ModelCookRecord& record, std::string& error);

        uint64_t totalBytes() const;
        // drops least recently used artifacts until the cache fits its cap; returns bytes freed
        uint64_t sweep();
        // Project Settings > Mesh > Clear Mesh Cache
        void clear();
        void setCap(uint64_t bytes);

    private:
        struct SourceEntry {
            std::vector<std::pair<uint64_t, int64_t>> files;    // size, mtime of each file read
            std::string sha256;
        };
        mutable std::mutex mutex;
        std::filesystem::path dir;
        uint64_t capBytes = 0;
        std::unordered_map<std::string, SourceEntry> sources;
        bool sourcesDirty = false;
        bool opened = false;
        uint64_t writesSinceSweep = 0;
    };

}
