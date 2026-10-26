// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Ktx2.h"

#include "zstd.h"

#include <algorithm>
#include <cstring>
#include <limits>

using namespace jasperengine;

namespace {
    const uint8_t ktxIdentifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

    // DFD (Khronos Data Format 1.3) values we read and write
    constexpr uint32_t DF_MODEL_RGBSDA = 1;
    constexpr uint32_t DF_MODEL_BC1A = 128;
    constexpr uint32_t DF_MODEL_BC3 = 130;
    constexpr uint32_t DF_MODEL_BC4 = 131;
    constexpr uint32_t DF_MODEL_BC5 = 132;
    constexpr uint32_t DF_MODEL_BC6H = 133;
    constexpr uint32_t DF_MODEL_BC7 = 134;
    constexpr uint32_t DF_MODEL_ETC2 = 161;
    constexpr uint32_t DF_MODEL_ASTC = 162;
    constexpr uint32_t DF_MODEL_ETC1S = 163;
    constexpr uint32_t DF_MODEL_UASTC = 166;
    constexpr uint32_t DF_MODEL_UASTC_HDR = 167;
    constexpr uint32_t DF_PRIMARIES_BT709 = 1;
    constexpr uint32_t DF_TRANSFER_LINEAR = 1;
    constexpr uint32_t DF_TRANSFER_SRGB = 2;
    constexpr uint32_t DF_FLAG_PREMULTIPLIED = 1;
    constexpr uint32_t DF_CHANNEL_RED = 0;
    constexpr uint32_t DF_CHANNEL_GREEN = 1;
    constexpr uint32_t DF_CHANNEL_BLUE = 2;
    constexpr uint32_t DF_CHANNEL_COLOR_ETC2 = 2;
    constexpr uint32_t DF_CHANNEL_ALPHA = 15;
    constexpr uint32_t DF_QUALIFIER_LINEAR = 0x10;
    constexpr uint32_t DF_QUALIFIER_EXPONENT = 0x20;
    constexpr uint32_t DF_QUALIFIER_SIGNED = 0x40;
    constexpr uint32_t DF_QUALIFIER_FLOAT = 0x80;
    const uint32_t kKtxVkAstc4x4SFloat = 1000066000;

    uint32_t ktxRead32(const uint8_t* p){
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    uint64_t ktxRead64(const uint8_t* p){
        return (uint64_t)ktxRead32(p) | ((uint64_t)ktxRead32(p + 4) << 32);
    }
    void ktxPut32(std::vector<uint8_t>& out, uint32_t v){
        for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i)));
    }
    void ktxPut64(std::vector<uint8_t>& out, uint64_t v){
        ktxPut32(out, (uint32_t)v);
        ktxPut32(out, (uint32_t)(v >> 32));
    }
    void ktxSet32(std::vector<uint8_t>& out, size_t at, uint32_t v){
        for (int i = 0; i < 4; i++) out[at + i] = (uint8_t)(v >> (8 * i));
    }
    void ktxSet64(std::vector<uint8_t>& out, size_t at, uint64_t v){
        ktxSet32(out, at, (uint32_t)v);
        ktxSet32(out, at + 4, (uint32_t)(v >> 32));
    }
    void ktxPad(std::vector<uint8_t>& out, size_t alignment){
        while (out.size() % alignment)
            out.push_back(0);
    }
    // a + b <= limit without overflow
    bool ktxFits(uint64_t offset, uint64_t length, uint64_t limit){
        return offset <= limit && length <= limit - offset;
    }

    size_t ktxLevelAlignment(GpuTextureFormat format){
        const size_t block = gpuFormatInfo(format).bytesPerBlock;
        size_t a = 4;
        while (a % block) a += 4;   // lcm(block, 4), block is 1, 2, 4, 8 or 16
        return a;
    }

    uint32_t ktxTypeSize(GpuTextureFormat format){
        switch (format){
            case GpuTextureFormat::R16: case GpuTextureFormat::RGB565: case GpuTextureFormat::RGBA4:
            case GpuTextureFormat::RGBA16F: return 2;
            case GpuTextureFormat::RGB9E5: return 4;
            default: return 1;
        }
    }

    struct KtxSample {
        uint16_t bitOffset;
        uint8_t bitLength;      // bits
        uint8_t channel;        // channel id with qualifiers
        uint32_t lower, upper;
    };

    void ktxParseKeyValues(const uint8_t* p, uint64_t length, std::map<std::string, std::string>& out){
        uint64_t at = 0;
        while (at + 4 <= length){
            const uint32_t entry = ktxRead32(p + at);
            at += 4;
            if (entry > length - at)
                return;
            const char* text = (const char*)(p + at);
            const char* end = (const char*)std::memchr(text, 0, entry);
            if (end){
                std::string key(text, end);
                std::string value(end + 1, text + entry);
                while (!value.empty() && value.back() == '\0')
                    value.pop_back();
                out[key] = value;
            }
            at += entry;
            at = (at + 3) & ~(uint64_t)3;
        }
    }

    std::vector<uint8_t> ktxMakeKeyValues(const std::map<std::string, std::string>& keyValues){
        std::vector<uint8_t> out;
        // std::map orders keys by byte value, as KTX2 requires
        for (const auto& [key, value] : keyValues){
            ktxPut32(out, (uint32_t)(key.size() + 1 + value.size() + 1));
            out.insert(out.end(), key.begin(), key.end());
            out.push_back(0);
            out.insert(out.end(), value.begin(), value.end());
            out.push_back(0);
            ktxPad(out, 4);
        }
        return out;
    }
}

bool Ktx2MemorySource::read(uint64_t offset, size_t size, std::vector<uint8_t>& out){
    if (!ktxFits(offset, size, bytes.size()))
        return false;
    out.assign(bytes.begin() + (size_t)offset, bytes.begin() + (size_t)(offset + size));
    return true;
}

bool Ktx2PrefixSource::read(uint64_t offset, size_t size, std::vector<uint8_t>& out){
    if (!ktxFits(offset, size, bytes.size()))
        return false;
    out.assign(bytes.begin() + (size_t)offset, bytes.begin() + (size_t)(offset + size));
    return true;
}

bool Ktx2::isKtx2(std::span<const uint8_t> bytes){
    return bytes.size() >= sizeof(ktxIdentifier) && std::memcmp(bytes.data(), ktxIdentifier, sizeof(ktxIdentifier)) == 0;
}

bool Ktx2::parseInfo(std::span<const uint8_t> prefix, uint64_t fileSize, Ktx2Info& info, std::string& error, uint64_t* needed){
    if (needed) *needed = 0;
    info = Ktx2Info();
    info.fileSize = fileSize;
    if (prefix.size() < kHeaderSize){
        if (fileSize < kHeaderSize){
            error = "truncated KTX2 header";
            return false;
        }
        if (needed) *needed = kHeaderSize;
        error = "KTX2 header not read";
        return false;
    }
    const uint8_t* p = prefix.data();
    if (!isKtx2(prefix)){
        error = "not a KTX2 file";
        return false;
    }
    info.vkFormat = ktxRead32(p + 12);
    info.typeSize = ktxRead32(p + 16);
    info.width = ktxRead32(p + 20);
    info.height = ktxRead32(p + 24);
    const uint32_t depth = ktxRead32(p + 28);
    const uint32_t layers = ktxRead32(p + 32);
    info.faces = ktxRead32(p + 36);
    info.levelCount = std::max<uint32_t>(1, ktxRead32(p + 40));
    info.supercompression = (Ktx2Supercompression)ktxRead32(p + 44);
    info.dfdOffset = ktxRead32(p + 48);
    info.dfdLength = ktxRead32(p + 52);
    info.kvdOffset = ktxRead32(p + 56);
    info.kvdLength = ktxRead32(p + 60);
    info.sgdOffset = ktxRead64(p + 64);
    info.sgdLength = ktxRead64(p + 72);

    if (info.width == 0 || info.height == 0 || info.width > 65536 || info.height > 65536 || depth > 1 || layers > 1){
        error = "unsupported KTX2 dimensions (2D and cube textures only)";
        return false;
    }
    if (info.faces != 1 && info.faces != 6){
        error = "bad KTX2 face count";
        return false;
    }
    if (info.levelCount > (uint32_t)fullMipCount((int)info.width, (int)info.height)){
        error = "bad KTX2 level count";
        return false;
    }
    if (info.supercompression != Ktx2Supercompression::None && info.supercompression != Ktx2Supercompression::BasisLZ &&
        info.supercompression != Ktx2Supercompression::Zstd){
        error = "unsupported KTX2 supercompression";
        return false;
    }

    const uint64_t indexEnd = kHeaderSize + (uint64_t)info.levelCount * kLevelIndexEntrySize;
    uint64_t metadataEnd = indexEnd;
    for (const auto& [offset, length] : {std::pair{info.dfdOffset, info.dfdLength}, std::pair{info.kvdOffset, info.kvdLength},
                                         std::pair{info.sgdOffset, info.sgdLength}}){
        if (length == 0)
            continue;
        if (offset < indexEnd || !ktxFits(offset, length, fileSize) || offset + length > kMaxMetadataSize){
            error = "bad KTX2 metadata range";
            return false;
        }
        metadataEnd = std::max(metadataEnd, offset + length);
    }
    if (info.dfdLength < 28){
        error = "missing KTX2 data format descriptor";
        return false;
    }
    if (metadataEnd > fileSize){
        error = "truncated KTX2 metadata";
        return false;
    }
    info.metadataEnd = metadataEnd;
    if (prefix.size() < metadataEnd){
        if (needed) *needed = metadataEnd;
        error = "KTX2 metadata not read";
        return false;
    }

    // the DFD's first (basic) block
    const uint8_t* dfd = p + info.dfdOffset;
    const uint32_t totalSize = ktxRead32(dfd);
    const uint32_t vendorType = ktxRead32(dfd + 4);
    const uint32_t blockSize = ktxRead32(dfd + 8) >> 16;
    if (totalSize != info.dfdLength || vendorType != 0 || blockSize < 24 || 4 + (uint64_t)blockSize > info.dfdLength){
        error = "bad KTX2 data format descriptor";
        return false;
    }
    const uint32_t modelWord = ktxRead32(dfd + 12);
    info.dfdColorModel = modelWord & 0xFF;
    const uint32_t transfer = (modelWord >> 16) & 0xFF;
    info.premultiplied = ((modelWord >> 24) & DF_FLAG_PREMULTIPLIED) != 0;
    info.srgb = transfer == DF_TRANSFER_SRGB;

    if (info.vkFormat == 0 && info.dfdColorModel == DF_MODEL_ETC1S){
        info.codec = Ktx2Codec::ETC1S;
        if (info.supercompression != Ktx2Supercompression::BasisLZ || info.sgdLength == 0){
            error = "ETC1S KTX2 without BasisLZ global data";
            return false;
        }
    }else if (info.vkFormat == 0 && info.dfdColorModel == DF_MODEL_UASTC){
        info.codec = Ktx2Codec::UASTC;
    }else if (info.vkFormat == kKtxVkAstc4x4SFloat && info.dfdColorModel == DF_MODEL_UASTC_HDR){
        info.codec = Ktx2Codec::UASTC_HDR;
    }else{
        bool vkSrgb = false;
        if (!gpuFormatFromVkFormat(info.vkFormat, info.format, vkSrgb)){
            error = "unsupported KTX2 vkFormat " + std::to_string(info.vkFormat);
            return false;
        }
        info.codec = Ktx2Codec::Raw;
        info.srgb = info.srgb || vkSrgb;
        if (info.supercompression == Ktx2Supercompression::BasisLZ){
            error = "BasisLZ on a raw KTX2 payload";
            return false;
        }
    }

    info.levels.resize(info.levelCount);
    for (uint32_t level = 0; level < info.levelCount; level++){
        const uint8_t* entry = p + kHeaderSize + level * kLevelIndexEntrySize;
        Ktx2LevelIndex& index = info.levels[level];
        index.byteOffset = ktxRead64(entry);
        index.byteLength = ktxRead64(entry + 8);
        index.uncompressedByteLength = ktxRead64(entry + 16);
        if (index.byteLength == 0 || index.byteOffset < metadataEnd || !ktxFits(index.byteOffset, index.byteLength, fileSize)){
            error = "bad KTX2 level index (level " + std::to_string(level) + ")";
            return false;
        }
        if (info.codec == Ktx2Codec::Raw){
            const uint64_t expected = (uint64_t)gpuFormatLevelBytes(info.format, (int)info.levelWidth(level), (int)info.levelHeight(level)) * info.faces;
            const uint64_t stored = info.supercompression == Ktx2Supercompression::None ? index.byteLength : index.uncompressedByteLength;
            if (stored != expected || (info.supercompression == Ktx2Supercompression::None && index.uncompressedByteLength != expected)){
                error = "KTX2 level " + std::to_string(level) + " has " + std::to_string(stored) + " bytes, its format needs " + std::to_string(expected);
                return false;
            }
        }else if (info.codec != Ktx2Codec::ETC1S){
            // UASTC: 16-byte blocks, 4x4
            const uint64_t blocks = (uint64_t)((info.levelWidth(level) + 3) / 4) * ((info.levelHeight(level) + 3) / 4) * info.faces;
            const uint64_t stored = info.supercompression == Ktx2Supercompression::None ? index.byteLength : index.uncompressedByteLength;
            if (stored != blocks * 16){
                error = "bad KTX2 UASTC level size (level " + std::to_string(level) + ")";
                return false;
            }
        }
    }

    if (info.kvdLength > 0)
        ktxParseKeyValues(p + info.kvdOffset, info.kvdLength, info.keyValues);
    return true;
}

bool Ktx2::readInfo(Ktx2ByteSource& source, Ktx2Info& info, std::string& error, std::vector<uint8_t>* metadata){
    // the header first, then exactly the metadata it points at: no level byte is read here
    const uint64_t fileSize = source.size();
    std::vector<uint8_t> prefix;
    if (fileSize < kHeaderSize || !source.read(0, kHeaderSize, prefix)){
        error = "truncated KTX2 header";
        return false;
    }
    uint64_t needed = 0;
    if (!parseInfo(prefix, fileSize, info, error, &needed)){
        if (needed <= kHeaderSize)
            return false;
        std::vector<uint8_t> rest;
        if (!source.read(kHeaderSize, (size_t)(needed - kHeaderSize), rest)){
            error = "truncated KTX2 metadata";
            return false;
        }
        prefix.insert(prefix.end(), rest.begin(), rest.end());
        if (!parseInfo(prefix, fileSize, info, error))
            return false;
    }
    if (metadata)
        *metadata = std::move(prefix);
    return true;
}

bool Ktx2::inflateLevel(const Ktx2Info& info, uint32_t level, std::vector<uint8_t>& stored, std::vector<uint8_t>& out, std::string& error){
    if (level >= info.levelCount){
        error = "no such KTX2 level";
        return false;
    }
    if (info.supercompression != Ktx2Supercompression::Zstd){
        out = std::move(stored);
        return true;
    }
    const uint64_t length = info.levels[level].uncompressedByteLength;
    if (length == 0 || length > (uint64_t)std::numeric_limits<size_t>::max() / 2){
        error = "bad KTX2 uncompressed level size";
        return false;
    }
    out.resize((size_t)length);
    const size_t result = ZSTD_decompress(out.data(), out.size(), stored.data(), stored.size());
    if (ZSTD_isError(result) || result != out.size()){
        error = std::string("KTX2 level ") + std::to_string(level) + ": " +
                (ZSTD_isError(result) ? ZSTD_getErrorName(result) : "inflated size mismatch");
        out.clear();
        return false;
    }
    return true;
}

bool Ktx2::read(Ktx2ByteSource& source, uint32_t firstLevel, Ktx2Image& image, std::string& error, int lastLevel){
    image = Ktx2Image();
    if (!readInfo(source, image.info, error, &image.metadata))
        return false;
    const Ktx2Info& info = image.info;
    image.firstLevel = std::min(firstLevel, info.levelCount - 1);
    const uint32_t last = (lastLevel < 0 || (uint32_t)lastLevel >= info.levelCount) ? info.levelCount - 1 : (uint32_t)std::max<int>(lastLevel, (int)image.firstLevel);
    if (info.sgdLength > 0)
        image.globalData.assign(image.metadata.begin() + (size_t)info.sgdOffset, image.metadata.begin() + (size_t)(info.sgdOffset + info.sgdLength));
    image.levels.resize(last - image.firstLevel + 1);
    std::vector<uint8_t> stored;
    for (uint32_t level = image.firstLevel; level <= last; level++){
        const Ktx2LevelIndex& index = info.levels[level];
        if (!source.read(index.byteOffset, (size_t)index.byteLength, stored)){
            error = "cannot read KTX2 level " + std::to_string(level);
            return false;
        }
        if (info.codec == Ktx2Codec::ETC1S){
            image.levels[level - image.firstLevel] = std::move(stored);
        }else if (!inflateLevel(info, level, stored, image.levels[level - image.firstLevel], error)){
            return false;
        }
        stored.clear();
    }
    return true;
}

std::vector<uint8_t> Ktx2::makeUastcDfd(UastcChannels channels, bool srgb, bool supercompressed){
    const uint32_t blockBytes = 24 + 16;
    std::vector<uint8_t> out;
    ktxPut32(out, 4 + blockBytes);
    ktxPut32(out, 0);
    ktxPut32(out, 2u | (blockBytes << 16));
    ktxPut32(out, DF_MODEL_UASTC | (DF_PRIMARIES_BT709 << 8) | ((srgb ? DF_TRANSFER_SRGB : DF_TRANSFER_LINEAR) << 16));
    ktxPut32(out, 3u | (3u << 8));
    ktxPut32(out, supercompressed ? 0u : 16u);
    ktxPut32(out, 0);
    ktxPut32(out, 0u | (127u << 16) | ((uint32_t)channels << 24));
    ktxPut32(out, 0);
    ktxPut32(out, 0);
    ktxPut32(out, 0xFFFFFFFFu);
    return out;
}

std::vector<uint8_t> Ktx2::makeDfd(GpuTextureFormat format, bool srgb, bool alpha, bool supercompressed){
    const GpuFormatInfo& info = gpuFormatInfo(format);
    std::vector<KtxSample> samples;
    uint32_t model = DF_MODEL_RGBSDA;
    const uint8_t alphaQualifier = srgb ? DF_QUALIFIER_LINEAR : 0;
    auto unorm = [](uint16_t offset, uint8_t bits, uint8_t channel){
        return KtxSample{offset, bits, channel, 0, bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1};
    };
    auto block = [](uint16_t offset, uint8_t bits, uint8_t channel){
        return KtxSample{offset, bits, channel, 0, 0xFFFFFFFFu};
    };
    switch (format){
        case GpuTextureFormat::R8: samples = {unorm(0, 8, DF_CHANNEL_RED)}; break;
        case GpuTextureFormat::RG8: samples = {unorm(0, 8, DF_CHANNEL_RED), unorm(8, 8, DF_CHANNEL_GREEN)}; break;
        case GpuTextureFormat::RGBA8:
            samples = {unorm(0, 8, DF_CHANNEL_RED), unorm(8, 8, DF_CHANNEL_GREEN), unorm(16, 8, DF_CHANNEL_BLUE),
                       unorm(24, 8, (uint8_t)(DF_CHANNEL_ALPHA | alphaQualifier))};
            break;
        case GpuTextureFormat::R16: samples = {unorm(0, 16, DF_CHANNEL_RED)}; break;
        case GpuTextureFormat::RGB565:
            samples = {unorm(0, 5, DF_CHANNEL_BLUE), unorm(5, 6, DF_CHANNEL_GREEN), unorm(11, 5, DF_CHANNEL_RED)};
            break;
        case GpuTextureFormat::RGBA4:
            samples = {unorm(0, 4, (uint8_t)(DF_CHANNEL_ALPHA | alphaQualifier)), unorm(4, 4, DF_CHANNEL_BLUE),
                       unorm(8, 4, DF_CHANNEL_GREEN), unorm(12, 4, DF_CHANNEL_RED)};
            break;
        case GpuTextureFormat::RGB9E5: {
            const uint8_t ef = DF_QUALIFIER_EXPONENT;
            samples = {{0, 9, DF_CHANNEL_RED, 0, 8448}, {27, 5, (uint8_t)(DF_CHANNEL_RED | ef), 15, 31},
                       {9, 9, DF_CHANNEL_GREEN, 0, 8448}, {27, 5, (uint8_t)(DF_CHANNEL_GREEN | ef), 15, 31},
                       {18, 9, DF_CHANNEL_BLUE, 0, 8448}, {27, 5, (uint8_t)(DF_CHANNEL_BLUE | ef), 15, 31}};
            break;
        }
        case GpuTextureFormat::RGBA16F: {
            const uint8_t f = DF_QUALIFIER_FLOAT | DF_QUALIFIER_SIGNED;
            for (uint8_t c = 0; c < 4; c++)
                samples.push_back({(uint16_t)(16 * c), 16, (uint8_t)((c == 3 ? DF_CHANNEL_ALPHA : c) | f), 0xBF800000u, 0x3F800000u});
            break;
        }
        case GpuTextureFormat::BC1: model = DF_MODEL_BC1A; samples = {block(0, 64, alpha ? DF_CHANNEL_ALPHA : 0)}; break;
        case GpuTextureFormat::BC3: model = DF_MODEL_BC3; samples = {block(0, 64, DF_CHANNEL_ALPHA), block(64, 64, 0)}; break;
        case GpuTextureFormat::BC4: model = DF_MODEL_BC4; samples = {block(0, 64, 0)}; break;
        case GpuTextureFormat::BC5: model = DF_MODEL_BC5; samples = {block(0, 64, DF_CHANNEL_RED), block(64, 64, DF_CHANNEL_GREEN)}; break;
        case GpuTextureFormat::BC6H: model = DF_MODEL_BC6H; samples = {{0, 128, DF_QUALIFIER_FLOAT, 0, 0x7F800000u}}; break;
        case GpuTextureFormat::BC7: model = DF_MODEL_BC7; samples = {block(0, 128, 0)}; break;
        case GpuTextureFormat::ETC2_RGB8: model = DF_MODEL_ETC2; samples = {block(0, 64, DF_CHANNEL_COLOR_ETC2)}; break;
        case GpuTextureFormat::ETC2_RGBA8:
            model = DF_MODEL_ETC2;
            samples = {block(0, 64, DF_CHANNEL_ALPHA), block(64, 64, DF_CHANNEL_COLOR_ETC2)};
            break;
        case GpuTextureFormat::EAC_R11: model = DF_MODEL_ETC2; samples = {block(0, 64, DF_CHANNEL_RED)}; break;
        case GpuTextureFormat::EAC_RG11: model = DF_MODEL_ETC2; samples = {block(0, 64, DF_CHANNEL_RED), block(64, 64, DF_CHANNEL_GREEN)}; break;
        default: model = DF_MODEL_ASTC; samples = {block(0, 128, 0)}; break;
    }
    // sRGB is only meaningful for colour data; linear formats always say linear
    const bool colour = !info.hdr && format != GpuTextureFormat::R16 && format != GpuTextureFormat::BC4 &&
                        format != GpuTextureFormat::BC5 && format != GpuTextureFormat::EAC_R11 && format != GpuTextureFormat::EAC_RG11;
    const uint32_t transfer = (srgb && colour) ? DF_TRANSFER_SRGB : DF_TRANSFER_LINEAR;
    const uint32_t blockBytes = 24 + 16 * (uint32_t)samples.size();
    std::vector<uint8_t> out;
    ktxPut32(out, 4 + blockBytes);
    ktxPut32(out, 0);                                   // Khronos vendor, basic descriptor type
    ktxPut32(out, 2u | (blockBytes << 16));             // KDF 1.3
    ktxPut32(out, model | (DF_PRIMARIES_BT709 << 8) | (transfer << 16));
    const uint32_t dim = info.blockDim - 1u;
    ktxPut32(out, dim | (dim << 8));
    ktxPut32(out, supercompressed ? 0u : info.bytesPerBlock);
    ktxPut32(out, 0);
    for (const KtxSample& s : samples){
        ktxPut32(out, (uint32_t)s.bitOffset | ((uint32_t)(s.bitLength - 1) << 16) | ((uint32_t)s.channel << 24));
        ktxPut32(out, 0);
        ktxPut32(out, s.lower);
        ktxPut32(out, s.upper);
    }
    return out;
}

bool Ktx2::write(const WriteDesc& desc, std::vector<uint8_t>& out, std::string& error, int zstdLevel, Compressor compress, void* user){
    out.clear();
    const uint32_t levelCount = (uint32_t)desc.levels.size();
    if (desc.width == 0 || desc.height == 0 || levelCount == 0 || (desc.faces != 1 && desc.faces != 6) ||
        levelCount > (uint32_t)fullMipCount((int)desc.width, (int)desc.height)){
        error = "bad KTX2 description";
        return false;
    }
    const bool zstd = zstdLevel > 0;
    if (zstd && !compress){
        error = "KTX2 Zstandard supercompression needs a compressor";
        return false;
    }
    const bool uastc = desc.codec == Ktx2Codec::UASTC;
    if (desc.codec != Ktx2Codec::Raw && !uastc){
        error = "the KTX2 writer writes raw and UASTC payloads (ETC1S and UASTC HDR come from basisu)";
        return false;
    }
    // UASTC LDR blocks are 16 bytes per 4x4, like ASTC 4x4
    const GpuTextureFormat layoutFormat = uastc ? GpuTextureFormat::ASTC_4x4 : desc.format;
    std::vector<std::vector<uint8_t>> payloads(levelCount);
    for (uint32_t level = 0; level < levelCount; level++){
        const size_t expected = gpuFormatLevelBytes(layoutFormat, mipDimension((int)desc.width, (int)level), mipDimension((int)desc.height, (int)level)) * desc.faces;
        if (desc.levels[level].size() != expected){
            error = "KTX2 level " + std::to_string(level) + " has " + std::to_string(desc.levels[level].size()) + " bytes, expected " + std::to_string(expected);
            return false;
        }
        if (zstd && !compress(desc.levels[level], payloads[level], zstdLevel, user)){
            error = "Zstandard compression failed";
            return false;
        }
    }

    const std::vector<uint8_t> dfd = uastc ? makeUastcDfd(desc.uastcChannels, desc.srgb, zstd) : makeDfd(desc.format, desc.srgb, desc.alpha, zstd);
    const std::vector<uint8_t> kvd = ktxMakeKeyValues(desc.keyValues);
    const uint64_t dfdOffset = kHeaderSize + (uint64_t)levelCount * kLevelIndexEntrySize;
    const uint64_t kvdOffset = dfdOffset + dfd.size();

    out.insert(out.end(), ktxIdentifier, ktxIdentifier + sizeof(ktxIdentifier));
    ktxPut32(out, uastc ? 0u : gpuFormatToVkFormat(desc.format, desc.srgb, desc.alpha));
    ktxPut32(out, uastc ? 1u : ktxTypeSize(desc.format));
    ktxPut32(out, desc.width);
    ktxPut32(out, desc.height);
    ktxPut32(out, 0);             // depth
    ktxPut32(out, 0);             // layers
    ktxPut32(out, desc.faces);
    ktxPut32(out, levelCount);
    ktxPut32(out, zstd ? (uint32_t)Ktx2Supercompression::Zstd : 0);
    ktxPut32(out, (uint32_t)dfdOffset);
    ktxPut32(out, (uint32_t)dfd.size());
    ktxPut32(out, kvd.empty() ? 0 : (uint32_t)kvdOffset);
    ktxPut32(out, (uint32_t)kvd.size());
    ktxPut64(out, 0);
    ktxPut64(out, 0);
    const size_t indexAt = out.size();
    out.resize(out.size() + (size_t)levelCount * kLevelIndexEntrySize, 0);
    out.insert(out.end(), dfd.begin(), dfd.end());
    out.insert(out.end(), kvd.begin(), kvd.end());

    // smallest level first; raw levels aligned to lcm(block size, 4)
    const size_t alignment = zstd ? 1 : ktxLevelAlignment(layoutFormat);
    for (int level = (int)levelCount - 1; level >= 0; level--){
        ktxPad(out, alignment);
        const std::span<const uint8_t> bytes = zstd ? std::span<const uint8_t>(payloads[level]) : desc.levels[level];
        const size_t at = indexAt + (size_t)level * kLevelIndexEntrySize;
        ktxSet64(out, at, out.size());
        ktxSet64(out, at + 8, bytes.size());
        ktxSet64(out, at + 16, desc.levels[level].size());
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return true;
}

bool Ktx2::rewriteKeyValues(std::span<const uint8_t> file, const std::map<std::string, std::string>& keyValues,
                            std::vector<uint8_t>& out, std::string& error){
    Ktx2Info info;
    if (!parseInfo(file, file.size(), info, error))
        return false;
    std::map<std::string, std::string> merged = info.keyValues;
    for (const auto& [key, value] : keyValues)
        merged[key] = value;
    const std::vector<uint8_t> kvd = ktxMakeKeyValues(merged);

    out.assign(file.begin(), file.begin() + kHeaderSize + (size_t)info.levelCount * kLevelIndexEntrySize);
    const uint64_t dfdOffset = out.size();
    out.insert(out.end(), file.begin() + (size_t)info.dfdOffset, file.begin() + (size_t)(info.dfdOffset + info.dfdLength));
    const uint64_t kvdOffset = out.size();
    out.insert(out.end(), kvd.begin(), kvd.end());
    uint64_t sgdOffset = 0;
    if (info.sgdLength > 0){
        ktxPad(out, 8);
        sgdOffset = out.size();
        out.insert(out.end(), file.begin() + (size_t)info.sgdOffset, file.begin() + (size_t)(info.sgdOffset + info.sgdLength));
    }
    ktxSet32(out, 48, (uint32_t)dfdOffset);
    ktxSet32(out, 52, (uint32_t)info.dfdLength);
    ktxSet32(out, 56, kvd.empty() ? 0 : (uint32_t)kvdOffset);
    ktxSet32(out, 60, (uint32_t)kvd.size());
    ktxSet64(out, 64, sgdOffset);
    ktxSet64(out, 72, info.sgdLength);

    const size_t alignment = (info.codec == Ktx2Codec::Raw && info.supercompression == Ktx2Supercompression::None)
        ? ktxLevelAlignment(info.format) : (info.supercompression == Ktx2Supercompression::None ? 16 : 1);
    for (int level = (int)info.levelCount - 1; level >= 0; level--){
        const Ktx2LevelIndex& index = info.levels[level];
        ktxPad(out, alignment);
        ktxSet64(out, kHeaderSize + (size_t)level * kLevelIndexEntrySize, out.size());
        out.insert(out.end(), file.begin() + (size_t)index.byteOffset, file.begin() + (size_t)(index.byteOffset + index.byteLength));
    }
    return true;
}
