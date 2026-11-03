// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureEncoderParts.h"

#include "texture/Ktx2.h"
#include "texture/TextureData.h"
#include "texture/TextureDecode.h"
#include "texture/TextureFallback.h"

#include "zstd.h"

#include <algorithm>
#include <cstring>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    class RawEncoder final : public ITextureEncoder {
    public:
        const char* name() const override { return "uncompressed"; }
        std::string version() const override { return std::string("uncompressed zstd ") + ZSTD_versionString(); }
        bool isaInvariant() const override { return true; }

        bool encode(const TextureEncodeRequest& request, std::vector<uint8_t>& ktx2, std::string& error) const override{
            std::vector<std::vector<uint8_t>> levels;
            if (!TexturePrepare::toUncompressed(*request.prepared, request.choice.format, levels, error))
                return false;
            return writeRawKtx2(request, request.choice.format, levels, ktx2, error);
        }
    };

}

const ITextureEncoder& editor::rawTextureEncoder(){
    static const RawEncoder encoder;
    return encoder;
}

const ITextureEncoder& editor::textureEncoderFor(const TextureFormatChoice& choice){
    if (choice.codec != TextureCodec::Raw || choice.viaUastc)
        return basisTextureEncoder();
    if (choice.uncompressed || !gpuFormatInfo(choice.format).compressed)
        return rawTextureEncoder();
    if (choice.format >= GpuTextureFormat::ASTC_4x4)
        return astcTextureEncoder();
    return bcTextureEncoder();
}

const std::vector<const ITextureEncoder*>& editor::textureEncoders(){
    static const std::vector<const ITextureEncoder*> all = {&rawTextureEncoder(), &bcTextureEncoder(), &astcTextureEncoder(), &basisTextureEncoder()};
    return all;
}

const char* editor::textureEncoderIsa(){
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "unknown";
#endif
}

bool editor::zstdCompress(std::span<const uint8_t> in, std::vector<uint8_t>& out, int level, void*){
    out.resize(ZSTD_compressBound(in.size()));
    const size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), std::clamp(level, 1, ZSTD_maxCLevel()));
    if (ZSTD_isError(written))
        return false;
    out.resize(written);
    return true;
}

void editor::gatherBlockRgba(const MipLevel& level, int channels, int blockX, int blockY, int blockWidth, int blockHeight, uint8_t* rgba){
    for (int y = 0; y < blockHeight; y++){
        const int sy = std::min(level.height - 1, blockY * blockHeight + y);
        for (int x = 0; x < blockWidth; x++){
            const int sx = std::min(level.width - 1, blockX * blockWidth + x);
            const uint8_t* p = &level.pixels[((size_t)sy * level.width + sx) * channels];
            uint8_t* o = &rgba[((size_t)y * blockWidth + x) * 4];
            switch (channels){
                case 1: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
                case 2: o[0] = p[0]; o[1] = p[1]; o[2] = 0; o[3] = 255; break;
                default: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;
            }
        }
    }
}

void editor::gatherBlockRgba(const MipLevel& level, int channels, int blockX, int blockY, uint8_t rgba[64]){
    gatherBlockRgba(level, channels, blockX, blockY, 4, 4, rgba);
}

bool editor::writeRawKtx2(const TextureEncodeRequest& request, GpuTextureFormat format, const std::vector<std::vector<uint8_t>>& levels,
                          std::vector<uint8_t>& ktx2, std::string& error){
    const PreparedTexture& prepared = *request.prepared;
    Ktx2::WriteDesc desc;
    desc.format = format;
    desc.srgb = prepared.srgb;
    desc.alpha = prepared.hasAlpha;
    desc.width = (uint32_t)prepared.width;
    desc.height = (uint32_t)prepared.height;
    desc.faces = (uint32_t)prepared.faces.size();
    for (const auto& level : levels)
        desc.levels.push_back(level);
    desc.keyValues = request.keyValues;
    return Ktx2::write(desc, ktx2, error, request.choice.zstdLevel, zstdCompress);
}

void editor::registerEditorTextureCodecs(){
    static std::once_flag once;
    std::call_once(once, []{
        for (GpuTextureFormat format : {GpuTextureFormat::ASTC_4x4, GpuTextureFormat::ASTC_5x5, GpuTextureFormat::ASTC_6x6,
                                        GpuTextureFormat::ASTC_8x8, GpuTextureFormat::ASTC_10x10, GpuTextureFormat::ASTC_12x12})
            TextureDecode::registerDecoder(format, &decodeAstcLevel);
        for (GpuTextureFormat format : {GpuTextureFormat::ETC2_RGB8, GpuTextureFormat::ETC2_RGBA8, GpuTextureFormat::EAC_R11,
                                        GpuTextureFormat::EAC_RG11})
            TextureDecode::registerDecoder(format, &decodeEtcLevel);
        TextureData::registerFloatDecoder(".exr", &decodeExr);
    });
}

bool editor::transcodeKtx2(const std::vector<uint8_t>& file, GpuTextureFormat target, std::vector<std::vector<uint8_t>>& levels,
                           int& width, int& height, std::string& error){
    Ktx2Info info;
    if (!Ktx2::parseInfo(file, file.size(), info, error))
        return false;
    width = (int)info.width;
    height = (int)info.height;
    if (info.codec != Ktx2Codec::Raw)
        return transcodeBasisLevels(file, target, levels, error);
    // a raw payload: through the CPU decode (to its decoded format), then converted if asked
    Ktx2MemorySource source(file);
    Ktx2Image image;
    if (!Ktx2::read(source, 0, image, error))
        return false;
    levels.assign(image.levels.size(), {});
    const GpuTextureFormat decoded = TextureDecode::decodedFormat(info.format);
    for (size_t level = 0; level < image.levels.size(); level++){
        const int w = (int)info.levelWidth((uint32_t)level), h = (int)info.levelHeight((uint32_t)level);
        std::vector<uint8_t> pixels;
        if (target == info.format){
            levels[level] = image.levels[level];
            continue;
        }
        if (!TextureDecode::decodeFaces(info.format, w, h, (int)info.faces, image.levels[level].data(), image.levels[level].size(), pixels)){
            error = std::string(gpuFormatName(info.format)) + " cannot be decoded on the CPU";
            return false;
        }
        if (target != decoded){
            const size_t faceBytes = gpuFormatLevelBytes(decoded, w, h);
            for (uint32_t f = 0; f < info.faces; f++){
                std::vector<uint8_t> converted;
                if (!TextureDecode::convert(decoded, target, w, h, pixels.data() + faceBytes * f, faceBytes, converted)){
                    error = std::string(gpuFormatName(info.format)) + " decodes to " + gpuFormatName(decoded) + ", not " + gpuFormatName(target);
                    return false;
                }
                levels[level].insert(levels[level].end(), converted.begin(), converted.end());
            }
        }else{
            levels[level] = std::move(pixels);
        }
    }
    return true;
}

bool editor::decodeKtx2ForPreview(const std::vector<uint8_t>& file, uint32_t level, GpuTextureFormat& format,
                                  int& width, int& height, int& faces, std::vector<uint8_t>& pixels, std::string& error){
    registerEditorTextureCodecs();
    Ktx2Info info;
    if (!Ktx2::parseInfo(file, file.size(), info, error))
        return false;
    level = std::min(level, info.levelCount - 1);
    width = (int)info.levelWidth(level);
    height = (int)info.levelHeight(level);
    faces = (int)info.faces;
    if (info.codec != Ktx2Codec::Raw){
        const TextureContent content = TextureFallback::contentOf(info);
        format = content == TextureContent::Hdr ? GpuTextureFormat::RGBA16F : content == TextureContent::Normal ? GpuTextureFormat::RG8
               : content == TextureContent::Single ? GpuTextureFormat::R8 : GpuTextureFormat::RGBA8;
        std::vector<std::vector<uint8_t>> levels;
        if (!transcodeBasisLevels(file, format, levels, error))
            return false;
        pixels = std::move(levels[level]);
        return true;
    }
    Ktx2MemorySource source(file);
    Ktx2Image image;
    if (!Ktx2::read(source, level, image, error, (int)level))
        return false;
    format = TextureDecode::decodedFormat(info.format);
    if (!TextureDecode::decodeFaces(info.format, width, height, faces, image.levels[0].data(), image.levels[0].size(), pixels)){
        error = std::string(gpuFormatName(info.format)) + " cannot be decoded on the CPU";
        return false;
    }
    return true;
}
