// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Where a texture's cooked file is (spec S14 R9, R10). An exported game reads
// assets/je-cooked/<family>/index.json, which maps each asset-relative source path to its KTX2
// file, logical size, format and flags; the editor installs a provider that answers from its
// cook cache instead (and simulates a family in the Game view). Paths not in the index load raw.

#ifndef COOKEDTEXTURES_H
#define COOKEDTEXTURES_H

#include "Export.h"
#include "texture/TextureFormat.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine {

    struct CookedTextureEntry {
        std::string file;               // the KTX2: a path FileData opens
        int width = 0, height = 0;      // logical (the source's size)
        std::string format;             // "BC7", "ETC1S", "UASTC"...
        uint32_t flags = 0;             // TextureDataFlags
        int levels = 1;
        int faces = 1;
        uint64_t bytes = 0;             // file size
        uint64_t tailBytes = 0;         // the file's first bytes: metadata and every level <= 64 px
        std::string cpuFile;            // a 256 px RGBA8 copy for CPU readers (cubemaps: IBL), or empty
    };

    class JASPERENGINE_API CookedTextures {
    public:
        static constexpr const char* kCookedDir = "je-cooked";

        // desktop, mobile, android_etc2 or web: what this build loads
        static std::string activeFamily();
        static std::string defaultFamily();

        // The cooked file of a texture source path (asset-relative, "asset://" or an absolute
        // path under the assets directory)
        static bool find(const std::string& sourcePath, CookedTextureEntry& out);

        // the editor's cook cache; null goes back to the export's index.json
        using Provider = std::function<bool(const std::string& assetPath, CookedTextureEntry& out)>;
        static void setProvider(Provider provider, const std::string& family);
        static bool hasProvider();

        // A texture the engine reads on the CPU (a terrain's heights, a foliage layer's density)
        // whose cook is GPU-only: the editor's handler cooks it again CPU-readable and swaps that
        // in (an export cooks such maps CPU-readable already). Any thread; a game has no handler.
        using CpuReadHandler = std::function<void(const std::string& assetPath)>;
        static void setCpuReadHandler(CpuReadHandler handler);
        static bool hasCpuReadHandler();
        static void requireCpuReadable(const std::string& sourcePath);
        // why a CPU-readable cook of it cannot be had (the editor's word: a failed cook), or
        // empty; an empty message clears it
        static void setCpuReadFailure(const std::string& assetPath, const std::string& message);
        static std::string getCpuReadFailure(const std::string& sourcePath);

        // "asset://a/../b.png", "/project/assets/b.png" -> "b.png" ('/' separated)
        static std::string normalize(const std::string& path);
        static void reset();
    };

}

#endif // COOKEDTEXTURES_H
