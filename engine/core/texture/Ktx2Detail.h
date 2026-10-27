// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The KTX2 container's byte-level helpers and DFD constants shared by the reader (Ktx2.cpp, in
// every game) and the writer (Ktx2Write.cpp, which only the editor and the tests link).

#ifndef KTX2DETAIL_H
#define KTX2DETAIL_H

#include "Ktx2.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::ktx2detail {
    inline constexpr uint8_t ktxIdentifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

    // DFD (Khronos Data Format 1.3) values we read and write
    inline constexpr uint32_t DF_MODEL_RGBSDA = 1;
    inline constexpr uint32_t DF_MODEL_BC1A = 128;
    inline constexpr uint32_t DF_MODEL_BC3 = 130;
    inline constexpr uint32_t DF_MODEL_BC4 = 131;
    inline constexpr uint32_t DF_MODEL_BC5 = 132;
    inline constexpr uint32_t DF_MODEL_BC6H = 133;
    inline constexpr uint32_t DF_MODEL_BC7 = 134;
    inline constexpr uint32_t DF_MODEL_ETC2 = 161;
    inline constexpr uint32_t DF_MODEL_ASTC = 162;
    inline constexpr uint32_t DF_MODEL_ETC1S = 163;
    inline constexpr uint32_t DF_MODEL_UASTC = 166;
    inline constexpr uint32_t DF_MODEL_UASTC_HDR = 167;
    inline constexpr uint32_t DF_PRIMARIES_BT709 = 1;
    inline constexpr uint32_t DF_TRANSFER_LINEAR = 1;
    inline constexpr uint32_t DF_TRANSFER_SRGB = 2;
    inline constexpr uint32_t DF_FLAG_PREMULTIPLIED = 1;
    inline constexpr uint32_t DF_CHANNEL_RED = 0;
    inline constexpr uint32_t DF_CHANNEL_GREEN = 1;
    inline constexpr uint32_t DF_CHANNEL_BLUE = 2;
    inline constexpr uint32_t DF_CHANNEL_COLOR_ETC2 = 2;
    inline constexpr uint32_t DF_CHANNEL_ALPHA = 15;
    inline constexpr uint32_t DF_QUALIFIER_LINEAR = 0x10;
    inline constexpr uint32_t DF_QUALIFIER_EXPONENT = 0x20;
    inline constexpr uint32_t DF_QUALIFIER_SIGNED = 0x40;
    inline constexpr uint32_t DF_QUALIFIER_FLOAT = 0x80;
    inline constexpr uint32_t kKtxVkAstc4x4SFloat = 1000066000;

    inline uint32_t ktxRead32(const uint8_t* p){
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    inline uint64_t ktxRead64(const uint8_t* p){
        return (uint64_t)ktxRead32(p) | ((uint64_t)ktxRead32(p + 4) << 32);
    }
    inline void ktxPut32(std::vector<uint8_t>& out, uint32_t v){
        for (int i = 0; i < 4; i++) out.push_back((uint8_t)(v >> (8 * i)));
    }
    inline void ktxPut64(std::vector<uint8_t>& out, uint64_t v){
        ktxPut32(out, (uint32_t)v);
        ktxPut32(out, (uint32_t)(v >> 32));
    }
    inline void ktxSet32(std::vector<uint8_t>& out, size_t at, uint32_t v){
        for (int i = 0; i < 4; i++) out[at + i] = (uint8_t)(v >> (8 * i));
    }
    inline void ktxSet64(std::vector<uint8_t>& out, size_t at, uint64_t v){
        ktxSet32(out, at, (uint32_t)v);
        ktxSet32(out, at + 4, (uint32_t)(v >> 32));
    }
    inline void ktxPad(std::vector<uint8_t>& out, size_t alignment){
        while (out.size() % alignment)
            out.push_back(0);
    }
    // a + b <= limit without overflow
    inline bool ktxFits(uint64_t offset, uint64_t length, uint64_t limit){
        return offset <= limit && length <= limit - offset;
    }

    inline size_t ktxLevelAlignment(GpuTextureFormat format){
        const size_t block = gpuFormatInfo(format).bytesPerBlock;
        size_t a = 4;
        while (a % block) a += 4;   // lcm(block, 4), block is 1, 2, 4, 8 or 16
        return a;
    }

    inline uint32_t ktxTypeSize(GpuTextureFormat format){
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

    inline void ktxParseKeyValues(const uint8_t* p, uint64_t length, std::map<std::string, std::string>& out){
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

    inline std::vector<uint8_t> ktxMakeKeyValues(const std::map<std::string, std::string>& keyValues){
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

#endif // KTX2DETAIL_H
