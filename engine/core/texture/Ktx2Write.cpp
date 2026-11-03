// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The KTX2 writer: the editor's cooks and the tests write files; games only read them (Ktx2.cpp),
// so this object is not linked into them (spec S14 R21).

#include "Ktx2.h"
#include "Ktx2Detail.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::ktx2detail;

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
