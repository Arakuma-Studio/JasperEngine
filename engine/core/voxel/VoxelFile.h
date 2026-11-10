// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_FILE_H
#define VOXEL_FILE_H

// R74-R76, R79, V9: the .voxelvolume format. Little-endian, written field by field, canonical: the same
// content with the same library version gives the same bytes. Readers check every offset, so a
// truncated or flipped file gives a code and never an out-of-bounds read.

#include "Export.h"
#include "VoxelStore.h"
#include "VoxelTypes.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::voxel {

    inline constexpr uint16_t VOXEL_FILE_VERSION = 1;
    inline constexpr uint16_t VOXEL_FILE_HEADER_BYTES = 64;
    inline constexpr uint32_t VOXEL_FILE_DIRECTORY_ENTRY_BYTES = 28;

    enum VoxelFileFlags : uint32_t {
        VOXEL_FILE_DELTA = 1,
        VOXEL_FILE_COOKED = 2,
        VOXEL_FILE_OPEN_BOUNDS = 4
    };

    enum class VoxelCodec : uint8_t { Uniform = 0, Raw = 1, Zstd = 2, Drle16 = 3 };

    enum class FileError : uint8_t {
        None,
        BadMagic,
        Truncated,
        Checksum,
        NewerVersion,
        BadHeader,
        BadDirectory,
        TooLarge,
        BaseChanged,
        DimensionMismatch
    };

    JASPERENGINE_API const char* fileErrorCode(FileError error);

    // Terrain texels this volume cut (R40): rows of runs, so what a person painted stays apart
    struct TerrainCutRuns {
        int width = 0, height = 0;
        struct Run {
            int row = 0, start = 0, length = 0;
        };
        std::vector<Run> runs;
        bool empty() const { return runs.empty(); }
        std::vector<uint8_t> toMask() const;   // width * height, 1 = cut by this volume
        static TerrainCutRuns fromMask(const uint8_t* mask, int width, int height);
    };

    struct VoxelFileInfo {
        uint16_t version = 0;
        uint32_t flags = 0;
        Int3 blocks;
        int levels = 1;
        int slotCount = 0;
        float voxelSize = 1.0f;
        uint64_t baseHash = 0;
        std::string generatorJson;
        TerrainCutRuns terrainCut;
        uint32_t entryCount = 0;
        size_t fileBytes = 0;

        VolumeDesc desc() const;
    };

    struct VolumeFileResult {
        bool ok = false;
        FileError error = FileError::None;
        std::string message;
        VoxelFileInfo info;
    };

    // The compressor lives in the cook (the players' zstd is decoder only, CMakeLists): the editor passes
    // one in, and a player's delta never has one (R79).
    using VoxelCompressFn = bool (*)(const uint8_t* bytes, size_t size, std::vector<uint8_t>& out);

    struct SaveOptions {
        enum class Mode : uint8_t {
            Editor,   // the base description and the blocks that differ from it (R76)
            Cooked,   // every block evaluated, generator stripped unless kept (R78)
            Delta     // blocks changed since the load, DRLE16 only (R79)
        };
        Mode mode = Mode::Editor;
        int slotCount = 0;
        uint64_t baseHash = 0;
        std::string generatorJson;          // canonical JSON, what R33 stores
        TerrainCutRuns terrainCut;
        VoxelCompressFn compress = nullptr;
        bool keepGenerator = false;         // Cooked: R78's runtimeGeneration
    };

    // Reads and verifies the header, sections and directory. No block is decoded. maxBytes is R7's
    // budget for a file (0: none).
    JASPERENGINE_API VolumeFileResult readVolumeFileInfo(std::span<const uint8_t> bytes, int64_t maxBytes = 0);

    // Fills a store of the file's desc: uniform blocks inline, others left cold over `bytes`, which
    // the store keeps through `owner` (null: the caller guarantees they outlive it, as JPak::view does)
    JASPERENGINE_API VolumeFileResult loadVolumeFile(std::span<const uint8_t> bytes, Store& store, std::shared_ptr<const void> owner = nullptr, int64_t maxBytes = 0);

    // R79: all or nothing over the store as loaded. Refuses base_changed when the hash differs.
    JASPERENGINE_API VolumeFileResult loadDeltaFile(std::span<const uint8_t> bytes, Store& store, uint64_t expectedBaseHash);

    JASPERENGINE_API std::vector<uint8_t> saveVolumeFile(const Store& store, const SaveOptions& options);

    // The trailing check of a file's bytes, which identifies the base a delta was made over (R79)
    JASPERENGINE_API uint64_t volumeFileHash(std::span<const uint8_t> bytes);

}

#endif
