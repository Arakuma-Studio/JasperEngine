// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Ktx2.h"
#include "Ktx2Detail.h"

#include "zstd.h"

#include <algorithm>
#include <cstring>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::ktx2detail;

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
