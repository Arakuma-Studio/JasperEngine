// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The content-addressed cache of cooked textures (spec S14 R7, D4):
//   .jasperengine/cache/textures/v1/<k[0:2]>/<k>.ktx2       the cooked file
//   .jasperengine/cache/textures/v1/<k[0:2]>/<k>.json       its record (format, sizes, PSNR, ms)
//   .jasperengine/cache/textures/v1/<k[0:2]>/<k>.cpu.ktx2   a cube's RGBA8 copy for IBL
//   .jasperengine/cache/textures/v1/sources.json            path, size, mtime -> SHA-256 + analysis
// k = SHA-256 of the source's SHA-256, the resolved per-family settings, the family, the format,
// kTextureCookVersion, the encoder's version and (encoders not proven ISA-invariant) the host
// ISA. Renaming a file never recooks it and duplicates share one artifact; a user-global
// directory can be shared by every project. Writes are atomic (S03); an LRU sweep keeps the
// cache under its cap (most recent use = the files' mtime).

#pragma once

#include "texture/TextureFormatSelector.h"
#include "texture/TextureImportSettings.h"

#include "mcp/McpJson.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace jasperengine::editor {

    using mcp::Json;

    // bumped when the cook's output changes for the same inputs (MipChain, TexturePrepare, a
    // selector rule, an encoder's parameters)
    constexpr int kTextureCookVersion = 1;

    // What the format selection needs to know of a source without decoding it again
    struct TextureSourceAnalysis {
        int width = 0, height = 0;          // as decoded (a cube: one face)
        int faces = 1;
        bool inputAlpha = false;            // some decoded alpha < 255
        bool grayAlpha = false;             // some grey (luma) < 255: alpha from gray has alpha
        bool floatSource = false;
        bool sixteenBit = false;
        bool singleChannel = false;
        uint64_t bytes = 0;                 // the source file
        // a request with a TextureCookStep (S46 R33): the analysis is of the step's output when
        // applied, else of the source; "" no step, "applied", "guarded", or "failed" with why
        std::string stepState;
        std::string stepGuard;
        std::string stepVerdict;            // "clean", "fixable", "needs_person"; "" when it did not run

        bool hasAlpha(TextureAlphaSource source) const;
        Json toJson() const;
        static bool fromJson(const Json& json, TextureSourceAnalysis& out);
    };

    // A cooked artifact's record (<k>.json)
    struct TextureCookRecord {
        std::string key;
        std::string family;
        std::string format;                 // "BC7", "ETC1S", "UASTC"...
        std::string label;                  // "BC7 RDO 1 + Zstd"
        int width = 0, height = 0, levels = 0, faces = 1;
        int logicalWidth = 0, logicalHeight = 0;
        uint64_t gpuBytes = 0;              // VRAM of the levels as the family's devices hold them
        uint64_t diskBytes = 0;             // the .ktx2
        uint64_t sourceBytes = 0;
        uint64_t tailBytes = 0;             // metadata + levels <= 64 px (the progressive read)
        uint64_t cpuBytes = 0;              // the .cpu.ktx2, when there is one
        double psnr = 0.0, psnrAlpha = 0.0;   // dB (RGB, alpha) against the processed source, mip 0
        double normalMeanError = 0.0, normalP99Error = 0.0;   // degrees (normal maps)
        double hdrRelativeRmse = 0.0;
        double cookMs = 0.0;
        uint32_t flags = 0;                 // TextureDataFlags
        std::vector<std::string> warnings;
        std::vector<std::string> encoders;  // "basisu 2.50 bands16 (x86_64)"

        Json toJson() const;
        static bool fromJson(const Json& json, TextureCookRecord& out);
    };

    class TextureCookCache {
    public:
        // `projectCache` is <project>/.jasperengine/cache/textures/v1; `globalDir` (Project
        // Settings > Texture) replaces it when set
        void open(const std::filesystem::path& projectCache, const std::string& globalDir, uint64_t capBytes);
        void close();
        bool isOpen() const;
        const std::filesystem::path& directory() const { return dir; }

        // SHA-256 of a source and its analysis, from the sources table while its size and
        // mtime stay; `analyze` decodes it otherwise (false: the file cannot be read). `variant`
        // names a step's analysis of the same file (S46 R33), remembered apart ("": the plain one)
        bool sourceHash(const std::filesystem::path& absolute, std::string& sha256, TextureSourceAnalysis& analysis,
                        const std::function<bool(TextureSourceAnalysis&)>& analyze, const std::string& variant = std::string());
        void saveSources();

        // `stepKey` (TextureCookStep::key, "" none) joins the hash only when there is one, so a
        // texture without a step keeps the key it always had
        static std::string key(const std::string& sourceSha256, const ResolvedTextureSettings& settings, TextureFamily family,
                               const TextureFormatChoice& choice, const std::string& stepKey = std::string());

        std::filesystem::path artifactPath(const std::string& key, const char* suffix = ".ktx2") const;
        bool has(const std::string& key) const;
        // the record of a cooked key; touches its files (LRU)
        bool load(const std::string& key, TextureCookRecord& record);
        bool readArtifact(const std::string& key, std::vector<uint8_t>& bytes, const char* suffix = ".ktx2") const;
        bool store(const std::string& key, const std::vector<uint8_t>& ktx2, const std::vector<uint8_t>* cpuCopy,
                   const TextureCookRecord& record, std::string& error);

        uint64_t totalBytes();
        // drops least recently used artifacts until the cache fits its cap; returns bytes freed
        uint64_t sweep();
        // Project Settings > Texture > Clear Texture Cache
        void clear();
        void setCap(uint64_t bytes) { capBytes = bytes; }

    private:
        struct SourceEntry {
            uint64_t size = 0;
            int64_t mtime = 0;
            std::string sha256;
            TextureSourceAnalysis analysis;
        };
        mutable std::mutex mutex;
        std::filesystem::path dir;
        std::filesystem::path sourcesFile;
        uint64_t capBytes = 0;
        std::unordered_map<std::string, SourceEntry> sources;
        bool sourcesDirty = false;
        bool opened = false;
        uint64_t writesSinceSweep = 0;
    };

}
