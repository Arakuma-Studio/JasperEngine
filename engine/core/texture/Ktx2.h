// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The KTX2 container of cooked textures (spec S14 D2): header, level index, the DFD subset we
// write (one basic block), key/values and Zstandard supercompression. Raw payloads (BC, ASTC,
// ETC2/EAC, uncompressed) are read and written here; Basis files (ETC1S, UASTC, UASTC HDR) are
// parsed here and handed to BasisTranscode. KTX2 stores the levels smallest first, so a loader
// that skips the top mips never reads the end of the file.

#ifndef KTX2_H
#define KTX2_H

#include "Export.h"
#include "texture/TextureFormat.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace jasperengine {

    enum class Ktx2Codec : uint8_t {
        Raw,        // vkFormat names the GPU format of the payload
        ETC1S,      // BasisLZ supercompressed
        UASTC,      // UASTC LDR 4x4 (usually Zstandard supercompressed)
        UASTC_HDR   // UASTC HDR 4x4 (vkFormat ASTC_4x4_SFLOAT)
    };

    enum class Ktx2Supercompression : uint32_t { None = 0, BasisLZ = 1, Zstd = 2, Zlib = 3 };

    struct Ktx2LevelIndex {
        uint64_t byteOffset = 0;
        uint64_t byteLength = 0;
        uint64_t uncompressedByteLength = 0;
    };

    struct Ktx2Info {
        uint32_t vkFormat = 0;
        uint32_t typeSize = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t faces = 1;
        uint32_t levelCount = 1;
        Ktx2Supercompression supercompression = Ktx2Supercompression::None;
        Ktx2Codec codec = Ktx2Codec::Raw;
        GpuTextureFormat format = GpuTextureFormat::RGBA8;   // Raw only
        bool srgb = false;            // the DFD's transfer function
        bool premultiplied = false;
        uint32_t dfdColorModel = 0;
        std::vector<Ktx2LevelIndex> levels;               // level 0 (largest) first
        std::map<std::string, std::string> keyValues;
        uint64_t dfdOffset = 0, dfdLength = 0;
        uint64_t kvdOffset = 0, kvdLength = 0;
        uint64_t sgdOffset = 0, sgdLength = 0;
        // where the level data starts: header, index, DFD, key/values and global data before it
        uint64_t metadataEnd = 0;
        uint64_t fileSize = 0;

        uint32_t levelWidth(uint32_t level) const { return (uint32_t)mipDimension((int)width, (int)level); }
        uint32_t levelHeight(uint32_t level) const { return (uint32_t)mipDimension((int)height, (int)level); }
    };

    // Where a reader gets bytes: a file range, a memory block. The tests count the ranges.
    class JASPERENGINE_API Ktx2ByteSource {
    public:
        virtual ~Ktx2ByteSource() = default;
        virtual uint64_t size() const = 0;
        // exactly `size` bytes at `offset`; false when they are not there
        virtual bool read(uint64_t offset, size_t size, std::vector<uint8_t>& out) = 0;
    };

    class JASPERENGINE_API Ktx2MemorySource : public Ktx2ByteSource {
    public:
        explicit Ktx2MemorySource(std::span<const uint8_t> bytes): bytes(bytes){}
        uint64_t size() const override { return bytes.size(); }
        bool read(uint64_t offset, size_t size, std::vector<uint8_t>& out) override;
    private:
        std::span<const uint8_t> bytes;
    };

    // The first bytes of a file of `fileSize` bytes (what a ranged read brought): a read past
    // them fails, so the levels a load needs must be inside
    class JASPERENGINE_API Ktx2PrefixSource : public Ktx2ByteSource {
    public:
        Ktx2PrefixSource(std::span<const uint8_t> bytes, uint64_t fileSize): bytes(bytes), fileSize(fileSize){}
        uint64_t size() const override { return fileSize; }
        bool read(uint64_t offset, size_t size, std::vector<uint8_t>& out) override;
    private:
        std::span<const uint8_t> bytes;
        uint64_t fileSize;
    };

    // A KTX2 file's levels as the loader keeps them: `firstLevel` and down, each level's faces
    // contiguous, Zstandard inflated (Basis payloads stay as they are)
    struct Ktx2Image {
        Ktx2Info info;
        uint32_t firstLevel = 0;
        std::vector<std::vector<uint8_t>> levels;         // levels[i] is mip firstLevel + i
        std::vector<uint8_t> globalData;                  // BasisLZ only
        std::vector<uint8_t> metadata;                    // the file's first info.metadataEnd bytes
    };

    class JASPERENGINE_API Ktx2 {
    public:
        static constexpr size_t kHeaderSize = 80;
        static constexpr size_t kLevelIndexEntrySize = 24;
        // header, index and the largest DFD + key/values we accept
        static constexpr uint64_t kMaxMetadataSize = 1u << 20;

        static bool isKtx2(std::span<const uint8_t> bytes);

        // Parses the header, the level index, the DFD and the key/values from `prefix` (the
        // file's first bytes). With fewer bytes than the metadata needs, fails and sets
        // `needed` to the prefix size that would do (0 when the file is malformed).
        static bool parseInfo(std::span<const uint8_t> prefix, uint64_t fileSize, Ktx2Info& info, std::string& error,
                              uint64_t* needed = nullptr);
        // Reads the metadata, then levels firstLevel..levelCount-1 (lastLevel < 0: all of them),
        // touching only their byte ranges; firstLevel past the end clamps to the last level
        static bool read(Ktx2ByteSource& source, uint32_t firstLevel, Ktx2Image& image, std::string& error,
                         int lastLevel = -1);
        // the metadata alone; `metadata` gets the bytes it was parsed from (at least metadataEnd)
        static bool readInfo(Ktx2ByteSource& source, Ktx2Info& info, std::string& error, std::vector<uint8_t>* metadata = nullptr);
        // A level's bytes as stored, Zstandard inflated to its uncompressed length
        static bool inflateLevel(const Ktx2Info& info, uint32_t level, std::vector<uint8_t>& stored,
                                 std::vector<uint8_t>& out, std::string& error);

        // A raw-payload file. levels[i] holds every face of mip i (uncompressed bytes); with
        // zstdLevel > 0, `compress` supercompresses each one (the writer itself carries no
        // encoder: games link only the decoder). The DFD gets srgb as its transfer function.
        // UASTC LDR's DFD channel ids: what the blocks hold (the transcoder's alpha and RG cues)
        enum class UastcChannels : uint8_t { RGB = 0, RGBA = 3, RRR = 4, RRRG = 5, RG = 6 };

        struct WriteDesc {
            // Raw payloads, or UASTC LDR 4x4 blocks (16 bytes each, vkFormat UNDEFINED)
            Ktx2Codec codec = Ktx2Codec::Raw;
            UastcChannels uastcChannels = UastcChannels::RGBA;
            GpuTextureFormat format = GpuTextureFormat::RGBA8;
            bool srgb = false;
            bool alpha = true;          // BC1: the RGBA variant
            uint32_t width = 0, height = 0, faces = 1;
            std::vector<std::span<const uint8_t>> levels;
            std::map<std::string, std::string> keyValues;
        };
        using Compressor = bool (*)(std::span<const uint8_t> in, std::vector<uint8_t>& out, int level, void* user);
        static bool write(const WriteDesc& desc, std::vector<uint8_t>& out, std::string& error,
                          int zstdLevel = 0, Compressor compress = nullptr, void* user = nullptr);

        // Replaces (or adds) key/values of a parsed file: the cook stamps JE.* on basisu's output
        static bool rewriteKeyValues(std::span<const uint8_t> file, const std::map<std::string, std::string>& keyValues,
                                     std::vector<uint8_t>& out, std::string& error);

        // the DFD block of a raw format (exposed for the tests); supercompressed payloads say 0 bytes per plane
        static std::vector<uint8_t> makeDfd(GpuTextureFormat format, bool srgb, bool alpha, bool supercompressed = false);
        static std::vector<uint8_t> makeUastcDfd(UastcChannels channels, bool srgb, bool supercompressed);
    };

}

#endif // KTX2_H
