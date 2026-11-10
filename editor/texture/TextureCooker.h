// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// One texture, one family: decode -> TexturePrepare (MipChain) -> the selected encoder -> KTX2,
// through the content-addressed cache (spec S14 R7, D10). A cache hit needs no decode: the
// cache's source table keeps what the format selection needs to know of the pixels. Cubemaps
// also get a 256 px RGBA8 copy for IBL (R3). Every result carries its record: format, sizes,
// PSNR and the time it took.

#pragma once

#include "texture/TextureCookCache.h"
#include "texture/TextureEncoders.h"
#include "texture/TextureImportSettings.h"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine {
    class TextureData;
}

namespace jasperengine::editor {

    // A step run on a 2D source's decoded pixels before they are prepared (S46 R33, the pixel-art
    // cleanup of import.pixelCleanup). The cooker only knows this shape: what the step does is
    // the hook's, so no code linked here depends on it. A request without a key runs no step and
    // cooks exactly as it did.
    struct TextureCookStep {
        // the step's identity in the cache key ("" no step): whatever changes its output
        std::string key;
        struct Outcome {
            bool applied = false;           // `rgba` holds the output, else the source is cooked as it is
            int width = 0, height = 0;
            std::vector<uint8_t> rgba;      // straight RGBA8, rows top first, when applied
            std::string guard;              // not applied: why a person must look (a guard's id)
            std::string error;              // not applied: the step could not run (a broken recipe)
            std::string verdict;            // what the step found ("clean", "fixable", "needs_person"), "" when it did not run
        };
        // the source's straight RGBA8 pixels, rows top first, and its file's SHA-256
        std::function<Outcome(const uint8_t* rgba, int width, int height, const std::string& sourceSha256)> run;
        // The output of one run, shared by the requests that cook one source: its plan and cook
        // stages, every family. Whoever builds those requests owns it, so the output lives only
        // while one of them may still ask; null runs the step each time it is needed.
        struct Memo {
            std::mutex mutex;               // held through a run: a second family waits for the first's
            bool filled = false;
            std::string key, sourceSha256;  // what `outcome` is of, with the source's size
            int width = 0, height = 0;
            Outcome outcome;
        };
        std::shared_ptr<Memo> memo;
    };

    struct TextureCookRequest {
        std::filesystem::path source;       // absolute
        std::string assetPath;              // asset-relative ("textures/rock.png"), for messages
        TextureImportSettings settings;
        TextureProjectSettings project;
        TextureFamily family = TextureFamily::Desktop;
        // a reference reads its pixels on the CPU (R3): terrain height and density maps,
        // post-processing LUTs, spot light masks
        bool cpuReadable = false;
        std::string cpuReason;
        // optional (S46 R33)
        TextureCookStep step;
    };

    struct TextureCookPlan {
        ResolvedTextureSettings resolved;
        TextureFormatChoice choice;
        TextureSourceAnalysis analysis;
        std::string sourceSha256;
        std::string key;
    };

    struct TextureCookResult {
        bool ok = false;
        bool cacheHit = false;
        bool cancelled = false;
        std::string error;
        TextureCookPlan plan;
        TextureCookRecord record;
    };

    class TextureCooker {
    public:
        // the plan of a cook (settings, format, key) without cooking: the inspector's table, the
        // report, a cache lookup. Decodes only when the source changed since it was last hashed.
        static bool plan(TextureCookCache& cache, const TextureCookRequest& request, TextureCookPlan& out, std::string& error);
        // plans again, then cooks on a cache miss; a step without a memo gets one for the call
        static TextureCookResult cook(TextureCookCache& cache, const TextureCookRequest& request, const std::function<bool()>& cancelled);

        // The source as the cook reads it: float for .hdr / .exr, a 4:3 or 3:4 cross split
        // into six faces when cube. Returns the face count.
        static int decodeSource(const std::filesystem::path& path, bool cube, std::array<TextureData, 6>& faces, std::string& error);
        static void analyze(const std::array<TextureData, 6>& faces, int count, TextureSourceAnalysis& out);
        // the bytes a family's devices hold for a cooked file (Basis: the first transcode target
        // of a desktop browser)
        static uint64_t gpuBytes(const std::vector<uint8_t>& ktx2);
        // the end of the levels of at most 64 px (the progressive read of R12)
        static uint64_t tailBytes(const std::vector<uint8_t>& ktx2);
        // PSNR / angular error / relative RMSE of a cooked file against the prepared levels (mip 0)
        static void measure(const std::vector<uint8_t>& ktx2, const PreparedTexture& prepared, TextureCookRecord& record);
        // the working memory a cook of these dimensions needs (the queue's budget, R8)
        static uint64_t workingBytes(const TextureSourceAnalysis& analysis, const ResolvedTextureSettings& resolved);
        // JE.flags of a cook
        static std::string flagsText(uint32_t flags);
    };

}
