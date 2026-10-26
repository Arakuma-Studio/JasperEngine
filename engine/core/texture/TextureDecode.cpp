// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureDecode.h"

#include "TexturePrepare.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

// BC4/BC5 decode to the exact values a GPU's UNORM interpolation gives
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

using namespace jasperengine;

namespace {
    std::mutex& textureDecoderMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    std::array<TextureDecode::Decoder, kGpuTextureFormatCount>& registeredDecoders(){
        static std::array<TextureDecode::Decoder, kGpuTextureFormatCount> decoders{};
        return decoders;
    }

    bool isBcFormat(GpuTextureFormat format){
        return format >= GpuTextureFormat::BC1 && format <= GpuTextureFormat::BC7;
    }

    // decodes whole blocks into a block-aligned buffer, then crops to width x height
    bool decodeBcLevel(GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size, std::vector<uint8_t>& out){
        const int blocksX = (width + 3) / 4;
        const int blocksY = (height + 3) / 4;
        const size_t blockBytes = gpuFormatInfo(format).bytesPerBlock;
        if (size < (size_t)blocksX * (size_t)blocksY * blockBytes)
            return false;
        const GpuTextureFormat target = TextureDecode::decodedFormat(format);
        const size_t texel = gpuFormatInfo(target).bytesPerBlock;
        const int paddedWidth = blocksX * 4;
        std::vector<uint8_t> padded((size_t)paddedWidth * (size_t)blocksY * 4 * texel);
        const int pitch = paddedWidth * (int)texel;
        for (int by = 0; by < blocksY; by++){
            for (int bx = 0; bx < blocksX; bx++){
                const uint8_t* block = data + ((size_t)by * blocksX + bx) * blockBytes;
                uint8_t* dst = padded.data() + (size_t)by * 4 * pitch + (size_t)bx * 4 * texel;
                switch (format){
                    case GpuTextureFormat::BC1: bcdec_bc1(block, dst, pitch); break;
                    case GpuTextureFormat::BC3: bcdec_bc3(block, dst, pitch); break;
                    case GpuTextureFormat::BC4: bcdec_bc4(block, dst, pitch, 0); break;
                    case GpuTextureFormat::BC5: bcdec_bc5(block, dst, pitch, 0); break;
                    case GpuTextureFormat::BC7: bcdec_bc7(block, dst, pitch); break;
                    case GpuTextureFormat::BC6H: {
                        uint16_t rgb[16 * 3];
                        bcdec_bc6h_half(block, rgb, 4 * 3, 0);
                        const uint16_t one = floatToHalf(1.0f);
                        for (int y = 0; y < 4; y++){
                            uint16_t* row = reinterpret_cast<uint16_t*>(dst + (size_t)y * pitch);
                            for (int x = 0; x < 4; x++){
                                row[x * 4 + 0] = rgb[(y * 4 + x) * 3 + 0];
                                row[x * 4 + 1] = rgb[(y * 4 + x) * 3 + 1];
                                row[x * 4 + 2] = rgb[(y * 4 + x) * 3 + 2];
                                row[x * 4 + 3] = one;
                            }
                        }
                        break;
                    }
                    default: return false;
                }
            }
        }
        out.resize((size_t)width * (size_t)height * texel);
        for (int y = 0; y < height; y++)
            std::memcpy(out.data() + (size_t)y * width * texel, padded.data() + (size_t)y * pitch, (size_t)width * texel);
        return true;
    }
}

GpuTextureFormat TextureDecode::decodedFormat(GpuTextureFormat format){
    switch (format){
        case GpuTextureFormat::BC4: case GpuTextureFormat::EAC_R11: return GpuTextureFormat::R8;
        case GpuTextureFormat::BC5: case GpuTextureFormat::EAC_RG11: return GpuTextureFormat::RG8;
        case GpuTextureFormat::BC6H: case GpuTextureFormat::RGB9E5: return GpuTextureFormat::RGBA16F;
        default:
            return gpuFormatInfo(format).compressed ? GpuTextureFormat::RGBA8 : format;
    }
}

bool TextureDecode::canDecode(GpuTextureFormat format){
    if (!gpuFormatInfo(format).compressed || isBcFormat(format) || format == GpuTextureFormat::RGB9E5)
        return true;
    std::lock_guard<std::mutex> lock(textureDecoderMutex());
    return registeredDecoders()[(size_t)format] != nullptr;
}

void TextureDecode::registerDecoder(GpuTextureFormat format, Decoder decoder){
    std::lock_guard<std::mutex> lock(textureDecoderMutex());
    registeredDecoders()[(size_t)format] = decoder;
}

bool TextureDecode::decode(GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size, std::vector<uint8_t>& out){
    if (width <= 0 || height <= 0 || !data)
        return false;
    if (format == GpuTextureFormat::RGB9E5){
        const size_t n = (size_t)width * (size_t)height;
        if (size < n * 4)
            return false;
        out.resize(n * 8);
        const uint16_t one = floatToHalf(1.0f);
        for (size_t i = 0; i < n; i++){
            uint32_t packed;
            std::memcpy(&packed, data + i * 4, 4);
            float rgb[3];
            unpackRGB9E5(packed, rgb[0], rgb[1], rgb[2]);
            uint16_t* o = reinterpret_cast<uint16_t*>(out.data() + i * 8);
            for (int c = 0; c < 3; c++)
                o[c] = floatToHalf(rgb[c]);
            o[3] = one;
        }
        return true;
    }
    if (!gpuFormatInfo(format).compressed){
        const size_t bytes = gpuFormatLevelBytes(format, width, height);
        if (size < bytes)
            return false;
        out.assign(data, data + bytes);
        return true;
    }
    if (isBcFormat(format))
        return decodeBcLevel(format, width, height, data, size, out);
    Decoder decoder = nullptr;
    {
        std::lock_guard<std::mutex> lock(textureDecoderMutex());
        decoder = registeredDecoders()[(size_t)format];
    }
    return decoder && decoder(format, width, height, data, size, out);
}

bool TextureDecode::decodeFaces(GpuTextureFormat format, int width, int height, int faces, const uint8_t* data, size_t size,
                                std::vector<uint8_t>& out){
    const size_t faceBytes = gpuFormatLevelBytes(format, width, height);
    if (faces <= 0 || size < faceBytes * (size_t)faces)
        return false;
    out.clear();
    std::vector<uint8_t> face;
    for (int f = 0; f < faces; f++){
        if (!decode(format, width, height, data + faceBytes * f, faceBytes, face))
            return false;
        out.insert(out.end(), face.begin(), face.end());
    }
    return true;
}

bool TextureDecode::convert(GpuTextureFormat from, GpuTextureFormat to, int width, int height, const uint8_t* data, size_t size,
                            std::vector<uint8_t>& out){
    const size_t n = (size_t)width * (size_t)height;
    if (size < gpuFormatLevelBytes(from, width, height))
        return false;
    if (from == to){
        out.assign(data, data + gpuFormatLevelBytes(from, width, height));
        return true;
    }
    std::vector<uint8_t> rgba;
    const uint8_t* source = data;
    if (from == GpuTextureFormat::RGBA16F){
        rgba.resize(n * 4);
        for (size_t i = 0; i < n * 4; i++){
            uint16_t h;
            std::memcpy(&h, data + i * 2, 2);
            const float v = halfToFloat(h);
            rgba[i] = (uint8_t)std::clamp((int)std::floor(v * 255.0f + 0.5f), 0, 255);
        }
        source = rgba.data();
        from = GpuTextureFormat::RGBA8;
        if (to == GpuTextureFormat::RGBA8){
            out = std::move(rgba);
            return true;
        }
    }
    if (from != GpuTextureFormat::RGBA8 || (to != GpuTextureFormat::RGB565 && to != GpuTextureFormat::RGBA4))
        return false;
    out.resize(n * 2);
    for (size_t i = 0; i < n; i++){
        const uint8_t* t = source + i * 4;
        uint16_t packed;
        if (to == GpuTextureFormat::RGB565){
            packed = (uint16_t)((((t[0] * 31 + 127) / 255) << 11) | (((t[1] * 63 + 127) / 255) << 5) | ((t[2] * 31 + 127) / 255));
        }else{
            auto q = [](uint8_t v){ return (uint16_t)((v * 15 + 127) / 255); };
            packed = (uint16_t)((q(t[0]) << 12) | (q(t[1]) << 8) | (q(t[2]) << 4) | q(t[3]));
        }
        std::memcpy(out.data() + i * 2, &packed, 2);
    }
    return true;
}
