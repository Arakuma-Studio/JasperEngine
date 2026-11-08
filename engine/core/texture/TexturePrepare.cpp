// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TexturePrepare.h"

#include "TextureData.h"
#include "TextureDecode.h"
#include "stb_image_resize2.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

// Compiled with -ffp-contract=off (engine/CMakeLists.txt): the same bytes on every CPU.

namespace {
    // the engine's display encoding: shaders decode colour with pow(c, 2.2) (srgb.glsl), and
    // stb_image turns .hdr into 8 bits with the same gamma
    constexpr float kPrepDisplayGamma = 2.2f;

    const char* importTypeNames[] = {"default", "normal", "sprite", "ui", "lightmap", "single_channel", "hdr"};

    uint8_t prepToByte(float v){
        return (uint8_t)std::clamp((int)std::floor(v * 255.0f + 0.5f), 0, 255);
    }

    uint8_t prepLuma(const uint8_t* rgb){
        // Rec. 709 weights on the stored values (what Unity's Alpha from Grayscale uses)
        return (uint8_t)std::clamp((int)std::floor(0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2] + 0.5f), 0, 255);
    }

    bool prepIsFloat(const TextureData& data){
        return const_cast<TextureData&>(data).getColorFormat() == ColorFormat::RGBA32F;
    }

    // a decoded face as RGBA8 (grey expanded, 16-bit and float reduced the way stb_image would)
    std::vector<uint8_t> prepToRgba8(const TextureData& face){
        TextureData& data = const_cast<TextureData&>(face);
        const size_t texels = (size_t)data.getWidth() * (size_t)data.getHeight();
        std::vector<uint8_t> out(texels * 4);
        const ColorFormat format = data.getColorFormat();
        if (format == ColorFormat::RGBA){
            std::memcpy(out.data(), data.getData(), texels * 4);
        }else if (format == ColorFormat::RED){
            const uint8_t* src = (const uint8_t*)data.getData();
            for (size_t i = 0; i < texels; i++){
                out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = src[i];
                out[i * 4 + 3] = 255;
            }
        }else if (format == ColorFormat::RED16){
            const uint16_t* src = (const uint16_t*)data.getData();
            for (size_t i = 0; i < texels; i++){
                const uint8_t v = (uint8_t)(((uint32_t)src[i] * 255u + 32767u) / 65535u);
                out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = v;
                out[i * 4 + 3] = 255;
            }
        }else if (format == ColorFormat::RGBA32F){
            const float* src = (const float*)data.getData();
            for (size_t i = 0; i < texels; i++){
                for (int c = 0; c < 3; c++)
                    out[i * 4 + c] = prepToByte(std::pow(std::max(src[i * 4 + c], 0.0f), 1.0f / kPrepDisplayGamma));
                out[i * 4 + 3] = prepToByte(src[i * 4 + 3]);
            }
        }
        return out;
    }

    // a decoded face as linear float RGBA
    std::vector<float> prepToLinearFloat(const TextureData& face){
        TextureData& data = const_cast<TextureData&>(face);
        const size_t texels = (size_t)data.getWidth() * (size_t)data.getHeight();
        std::vector<float> out(texels * 4);
        if (data.getColorFormat() == ColorFormat::RGBA32F){
            std::memcpy(out.data(), data.getData(), texels * 4 * sizeof(float));
            return out;
        }
        const std::vector<uint8_t> rgba = prepToRgba8(face);
        float table[256];
        for (int v = 0; v < 256; v++)
            table[v] = std::pow((float)v / 255.0f, kPrepDisplayGamma);
        for (size_t i = 0; i < texels; i++){
            for (int c = 0; c < 3; c++)
                out[i * 4 + c] = table[rgba[i * 4 + c]];
            out[i * 4 + 3] = (float)rgba[i * 4 + 3] / 255.0f;
        }
        return out;
    }

    MipChainOptions prepChainOptions(PreparedTexture::Layout layout, const TextureProcessing& p){
        MipChainOptions options;
        options.filter = p.mipFilter;
        options.wrap = p.type == TextureImportType::Default || p.type == TextureImportType::Normal || p.type == TextureImportType::SingleChannel;
        options.maxLevels = p.mipmaps ? p.mipLimit : 1;
        switch (layout){
            case PreparedTexture::Layout::RGBA8:
                options.channels = 4;
                options.type = MipPixelType::U8;
                options.srgb = p.srgb;
                options.alphaWeighted = p.srgb;
                options.preserveCoverage = p.preserveCoverage;
                options.alphaCutoff = p.alphaCutoff;
                options.bleedAlpha = p.alphaIsTransparency;
                break;
            case PreparedTexture::Layout::RG8:
                // filtered as unit vectors, stored as XY afterwards
                options.channels = 3;
                options.type = MipPixelType::F32;
                options.normalMap = true;
                break;
            case PreparedTexture::Layout::R8:
                options.channels = 1;
                options.type = MipPixelType::U8;
                options.srgb = p.srgb && p.type != TextureImportType::SingleChannel;
                break;
            case PreparedTexture::Layout::R16:
                options.channels = 1;
                options.type = MipPixelType::U16;
                break;
            case PreparedTexture::Layout::RGBAF:
                options.channels = 4;
                options.type = MipPixelType::F32;
                break;
        }
        return options;
    }

    void prepResizeLevel(MipLevel& level, int width, int height, const MipChainOptions& options, MipFilter filter){
        if (level.width == width && level.height == height)
            return;
        const size_t texel = MipChain::texelBytes(options);
        MipLevel out;
        out.width = width;
        out.height = height;
        out.pixels.resize((size_t)width * (size_t)height * texel);
        if (filter == MipFilter::Nearest){
            nearestResize(level.pixels.data(), level.width, level.height, out.pixels.data(), width, height, texel);
            level = std::move(out);
            return;
        }
        stbir_pixel_layout layout = STBIR_4CHANNEL;
        if (options.channels == 1) layout = STBIR_1CHANNEL;
        else if (options.channels == 2) layout = STBIR_2CHANNEL;
        else if (options.channels == 3) layout = STBIR_RGB;
        else if (options.alphaWeighted) layout = STBIR_RGBA;
        else if (options.srgb && options.type == MipPixelType::U8) layout = STBIR_RGBA_NO_AW;
        stbir_datatype type = options.type == MipPixelType::F32 ? STBIR_TYPE_FLOAT
                            : (options.type == MipPixelType::U16 ? STBIR_TYPE_UINT16 : (options.srgb ? STBIR_TYPE_UINT8_SRGB : STBIR_TYPE_UINT8));
        stbir_filter stbFilter = STBIR_FILTER_MITCHELL;
        if (filter == MipFilter::Box) stbFilter = STBIR_FILTER_BOX;
        else if (filter == MipFilter::Triangle) stbFilter = STBIR_FILTER_TRIANGLE;
        else if (filter == MipFilter::CatmullRom) stbFilter = STBIR_FILTER_CATMULLROM;
        stbir_resize(level.pixels.data(), level.width, level.height, (int)(level.width * texel),
                     out.pixels.data(), width, height, (int)(width * texel),
                     layout, type, options.wrap ? STBIR_EDGE_WRAP : STBIR_EDGE_CLAMP, stbFilter);
        level = std::move(out);
    }

    // the image in the top-left corner of a zero-filled canvas (TextureStrategy::FIT)
    void prepPadLevel(MipLevel& level, int width, int height, size_t texel){
        if (level.width == width && level.height == height)
            return;
        MipLevel out;
        out.width = width;
        out.height = height;
        out.pixels.assign((size_t)width * (size_t)height * texel, 0);
        const int copyWidth = std::min(width, level.width);
        for (int y = 0; y < std::min(height, level.height); y++)
            std::memcpy(out.pixels.data() + (size_t)y * width * texel, level.pixels.data() + (size_t)y * level.width * texel, copyWidth * texel);
        level = std::move(out);
    }

    int prepNextPowerOfTwo(int v){
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }
}

const char* jasperengine::textureImportTypeName(TextureImportType type){
    const size_t index = (size_t)type;
    return index < sizeof(importTypeNames) / sizeof(importTypeNames[0]) ? importTypeNames[index] : "default";
}

bool jasperengine::textureImportTypeFromName(const std::string& name, TextureImportType& type){
    for (size_t i = 0; i < sizeof(importTypeNames) / sizeof(importTypeNames[0]); i++){
        if (name == importTypeNames[i]){
            type = (TextureImportType)i;
            return true;
        }
    }
    return false;
}

TextureProcessing TextureProcessing::defaultFor(const TextureData& decoded, bool fixAlphaBorder, bool cube){
    TextureProcessing processing;
    TextureData& data = const_cast<TextureData&>(decoded);
    processing.keepSourceChannels = true;
    processing.srgb = data.getColorFormat() == ColorFormat::RGBA;
    processing.alphaIsTransparency = fixAlphaBorder;
    processing.cube = cube;
    if (data.getColorFormat() == ColorFormat::RGBA32F)
        processing.type = TextureImportType::HDR;
    return processing;
}

size_t PreparedTexture::texelBytes() const{
    switch (layout){
        case Layout::RGBA8: return 4;
        case Layout::RG8: return 2;
        case Layout::R8: return 1;
        case Layout::R16: return 2;
        case Layout::RGBAF: return 16;
    }
    return 4;
}

void TexturePrepare::targetSize(int width, int height, const TextureProcessing& processing, int& outWidth, int& outHeight){
    int w = std::max(1, width);
    int h = std::max(1, height);
    if (processing.strategy != TextureSizeStrategy::None){
        w = prepNextPowerOfTwo(w);
        h = prepNextPowerOfTwo(h);
    }
    const int maxSize = std::max(1, processing.maxSize);
    if (w > maxSize || h > maxSize){
        // the longer side becomes maxSize, the other keeps the aspect (at least 1)
        if (w >= h){
            h = std::max(1, (int)std::floor((double)h * maxSize / w + 0.5));
            w = maxSize;
        }else{
            w = std::max(1, (int)std::floor((double)w * maxSize / h + 0.5));
            h = maxSize;
        }
        if (processing.strategy != TextureSizeStrategy::None){
            // a power of two stays one: halve until it fits
            w = std::min(w, maxSize);
            h = std::min(h, maxSize);
        }
    }
    if (processing.cube){
        w = h = std::max(w, h);
    }
    if (processing.sizeMultiple > 1){
        const int m = processing.sizeMultiple;
        auto nearest = [m](int v){ return std::max(m, ((v + m / 2) / m) * m); };
        w = nearest(w);
        h = nearest(h);
    }
    outWidth = w;
    outHeight = h;
}

bool TexturePrepare::prepare(const std::vector<const TextureData*>& faces, const TextureProcessing& processing,
                             PreparedTexture& out, std::string& error){
    out = PreparedTexture();
    if (faces.empty() || !faces[0] || !const_cast<TextureData*>(faces[0])->getData()){
        error = "no pixels to prepare";
        return false;
    }
    TextureData& first = const_cast<TextureData&>(*faces[0]);
    for (const TextureData* face : faces){
        TextureData& f = const_cast<TextureData&>(*face);
        if (!f.getData() || f.getWidth() != first.getWidth() || f.getHeight() != first.getHeight() ||
            f.getColorFormat() != first.getColorFormat()){
            error = "the faces differ in size or format";
            return false;
        }
    }

    using Layout = PreparedTexture::Layout;
    const ColorFormat source = first.getColorFormat();
    Layout layout = Layout::RGBA8;
    if (processing.keepSourceChannels){
        layout = source == ColorFormat::RED ? Layout::R8 : source == ColorFormat::RED16 ? Layout::R16
               : source == ColorFormat::RGBA32F ? Layout::RGBAF : Layout::RGBA8;
    }else{
        switch (processing.type){
            case TextureImportType::Normal: layout = Layout::RG8; break;
            case TextureImportType::SingleChannel: layout = Layout::R8; break;
            case TextureImportType::HDR: layout = Layout::RGBAF; break;
            case TextureImportType::Lightmap: layout = prepIsFloat(first) ? Layout::RGBAF : Layout::RGBA8; break;
            default: layout = Layout::RGBA8; break;
        }
    }
    out.layout = layout;
    out.srgb = (layout == Layout::RGBA8 || layout == Layout::R8) ? processing.srgb && processing.type != TextureImportType::SingleChannel
                                                                 : layout == Layout::RGBAF;
    if (processing.keepSourceChannels && layout == Layout::R8)
        out.srgb = false;
    out.logicalWidth = first.getOriginalWidth() > 0 ? first.getOriginalWidth() : first.getWidth();
    out.logicalHeight = first.getOriginalHeight() > 0 ? first.getOriginalHeight() : first.getHeight();

    const MipChainOptions options = prepChainOptions(layout, processing);
    int targetWidth = 0, targetHeight = 0;
    targetSize(first.getWidth(), first.getHeight(), processing, targetWidth, targetHeight);
    // FIT pads to the power of two before the other size rules shrink the result
    int fitWidth = first.getWidth(), fitHeight = first.getHeight();
    if (processing.strategy == TextureSizeStrategy::Fit){
        fitWidth = prepNextPowerOfTwo(fitWidth);
        fitHeight = prepNextPowerOfTwo(fitHeight);
    }

    const size_t texels = (size_t)first.getWidth() * (size_t)first.getHeight();
    for (const TextureData* face : faces){
        TextureData& data = const_cast<TextureData&>(*face);
        MipLevel base;
        base.width = data.getWidth();
        base.height = data.getHeight();
        switch (layout){
            case Layout::RGBA8: {
                base.pixels = prepToRgba8(data);
                // the byte an alpha test at alphaCutoff starts passing at (a >= cutoff * 255)
                const int clipByte = std::clamp((int)std::ceil(processing.alphaCutoff * 255.0f - 1e-4f), 0, 255);
                for (size_t i = 0; i < texels; i++){
                    if (processing.alphaSource == TextureAlphaSource::FromGray)
                        base.pixels[i * 4 + 3] = prepLuma(&base.pixels[i * 4]);
                    else if (processing.alphaSource == TextureAlphaSource::None)
                        base.pixels[i * 4 + 3] = 255;
                    if (processing.alphaClip)
                        base.pixels[i * 4 + 3] = base.pixels[i * 4 + 3] >= clipByte ? 255 : 0;
                    out.hasAlpha = out.hasAlpha || base.pixels[i * 4 + 3] != 255;
                }
                break;
            }
            case Layout::RG8: {
                const std::vector<uint8_t> rgba = prepToRgba8(data);
                base.pixels.resize(texels * 3 * sizeof(float));
                float* v = reinterpret_cast<float*>(base.pixels.data());
                for (size_t i = 0; i < texels; i++){
                    const uint8_t green = processing.flipGreen ? (uint8_t)(255 - rgba[i * 4 + 1]) : rgba[i * 4 + 1];
                    const float x = rgba[i * 4 + 0] / 255.0f * 2.0f - 1.0f;
                    const float y = green / 255.0f * 2.0f - 1.0f;
                    float z = rgba[i * 4 + 2] / 255.0f * 2.0f - 1.0f;
                    // tangent-space normals face outwards: a two-channel source (blue 0) gets its Z back
                    if (z < 0.0f)
                        z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                    v[i * 3 + 0] = x;
                    v[i * 3 + 1] = y;
                    v[i * 3 + 2] = z;
                }
                break;
            }
            case Layout::R8: {
                if (source == ColorFormat::RED && (processing.keepSourceChannels || processing.channel == TextureChannel::R ||
                                                   processing.channel == TextureChannel::G || processing.channel == TextureChannel::B ||
                                                   processing.channel == TextureChannel::Luma)){
                    base.pixels.assign((const uint8_t*)data.getData(), (const uint8_t*)data.getData() + texels);
                    break;
                }
                const std::vector<uint8_t> rgba = prepToRgba8(data);
                base.pixels.resize(texels);
                for (size_t i = 0; i < texels; i++){
                    const uint8_t* t = &rgba[i * 4];
                    switch (processing.channel){
                        case TextureChannel::R: base.pixels[i] = t[0]; break;
                        case TextureChannel::G: base.pixels[i] = t[1]; break;
                        case TextureChannel::B: base.pixels[i] = t[2]; break;
                        case TextureChannel::A: base.pixels[i] = t[3]; break;
                        case TextureChannel::Luma: base.pixels[i] = prepLuma(t); break;
                    }
                }
                break;
            }
            case Layout::R16:
                base.pixels.assign((const uint8_t*)data.getData(), (const uint8_t*)data.getData() + texels * 2);
                break;
            case Layout::RGBAF: {
                const std::vector<float> linear = prepToLinearFloat(data);
                base.pixels.resize(texels * 4 * sizeof(float));
                std::memcpy(base.pixels.data(), linear.data(), base.pixels.size());
                float* v = reinterpret_cast<float*>(base.pixels.data());
                for (size_t i = 0; i < texels; i++){
                    if (processing.alphaSource == TextureAlphaSource::None)
                        v[i * 4 + 3] = 1.0f;
                    out.hasAlpha = out.hasAlpha || v[i * 4 + 3] < 1.0f;
                }
                break;
            }
        }

        if (processing.strategy == TextureSizeStrategy::Fit)
            prepPadLevel(base, fitWidth, fitHeight, MipChain::texelBytes(options));
        prepResizeLevel(base, targetWidth, targetHeight, options, processing.resizeFilter);

        std::vector<MipLevel> levels;
        levels.push_back(std::move(base));
        MipChain::build(levels, options);

        if (layout == Layout::RG8){
            for (MipLevel& level : levels){
                const size_t n = (size_t)level.width * (size_t)level.height;
                const float* v = reinterpret_cast<const float*>(level.pixels.data());
                std::vector<uint8_t> rg(n * 2);
                for (size_t i = 0; i < n; i++){
                    rg[i * 2 + 0] = prepToByte(v[i * 3 + 0] * 0.5f + 0.5f);
                    rg[i * 2 + 1] = prepToByte(v[i * 3 + 1] * 0.5f + 0.5f);
                }
                level.pixels = std::move(rg);
            }
        }else if (layout == Layout::RGBAF){
            // stored display-encoded, like every colour the shaders decode with pow 2.2
            for (MipLevel& level : levels){
                float* v = reinterpret_cast<float*>(level.pixels.data());
                const size_t n = (size_t)level.width * (size_t)level.height;
                for (size_t i = 0; i < n; i++)
                    for (int c = 0; c < 3; c++)
                        v[i * 4 + c] = std::pow(std::max(v[i * 4 + c], 0.0f), 1.0f / kPrepDisplayGamma);
            }
        }
        out.faces.push_back(std::move(levels));
    }
    out.width = out.faces[0][0].width;
    out.height = out.faces[0][0].height;
    return true;
}

GpuTextureFormat TexturePrepare::uncompressedFormat(PreparedTexture::Layout layout){
    switch (layout){
        case PreparedTexture::Layout::RG8: return GpuTextureFormat::RG8;
        case PreparedTexture::Layout::R8: return GpuTextureFormat::R8;
        case PreparedTexture::Layout::R16: return GpuTextureFormat::R16;
        case PreparedTexture::Layout::RGBAF: return GpuTextureFormat::RGBA16F;
        case PreparedTexture::Layout::RGBA8: default: return GpuTextureFormat::RGBA8;
    }
}

bool TexturePrepare::toUncompressed(const PreparedTexture& prepared, GpuTextureFormat format,
                                    std::vector<std::vector<uint8_t>>& levels, std::string& error){
    using Layout = PreparedTexture::Layout;
    const Layout layout = prepared.layout;
    const bool ok =
        (layout == Layout::RGBA8 && (format == GpuTextureFormat::RGBA8 || format == GpuTextureFormat::RGB565 || format == GpuTextureFormat::RGBA4)) ||
        (layout == Layout::RG8 && format == GpuTextureFormat::RG8) ||
        (layout == Layout::R8 && format == GpuTextureFormat::R8) ||
        (layout == Layout::R16 && format == GpuTextureFormat::R16) ||
        (layout == Layout::RGBAF && (format == GpuTextureFormat::RGBA16F || format == GpuTextureFormat::RGB9E5));
    if (!ok){
        error = std::string("the prepared levels cannot be stored as ") + gpuFormatName(format);
        return false;
    }
    const int levelCount = prepared.levelCount();
    levels.assign(levelCount, {});
    const size_t outTexel = gpuFormatInfo(format).bytesPerBlock;
    for (int level = 0; level < levelCount; level++){
        std::vector<uint8_t>& dst = levels[level];
        for (const std::vector<MipLevel>& face : prepared.faces){
            const MipLevel& src = face[level];
            const size_t n = (size_t)src.width * (size_t)src.height;
            const size_t at = dst.size();
            dst.resize(at + n * outTexel);
            uint8_t* o = dst.data() + at;
            if (format == GpuTextureFormat::RGB565 || format == GpuTextureFormat::RGBA4){
                std::vector<uint8_t> packed;
                TextureDecode::convert(GpuTextureFormat::RGBA8, format, src.width, src.height, src.pixels.data(), src.pixels.size(), packed);
                std::memcpy(o, packed.data(), packed.size());
            }else if (format == GpuTextureFormat::RGBA16F){
                const float* v = reinterpret_cast<const float*>(src.pixels.data());
                for (size_t i = 0; i < n * 4; i++){
                    const uint16_t h = floatToHalf(v[i]);
                    std::memcpy(o + i * 2, &h, 2);
                }
            }else if (format == GpuTextureFormat::RGB9E5){
                const float* v = reinterpret_cast<const float*>(src.pixels.data());
                for (size_t i = 0; i < n; i++){
                    const uint32_t packed = packRGB9E5(v[i * 4], v[i * 4 + 1], v[i * 4 + 2]);
                    std::memcpy(o + i * 4, &packed, 4);
                }
            }else{
                std::memcpy(o, src.pixels.data(), n * outTexel);
            }
        }
    }
    return true;
}

uint16_t jasperengine::floatToHalf(float value){
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    uint32_t magnitude = bits & 0x7FFFFFFFu;
    if (magnitude >= 0x7F800000u)                       // inf, nan
        return (uint16_t)(sign | 0x7C00u | (magnitude > 0x7F800000u ? 0x200u : 0u));
    if (magnitude >= 0x477FF000u)                       // rounds past the largest half
        return (uint16_t)(sign | 0x7C00u);
    if (magnitude < 0x38800000u){                       // a half subnormal (or zero)
        if (magnitude < 0x33000000u)
            return (uint16_t)sign;
        const uint32_t exponent = magnitude >> 23;
        const uint32_t mantissa = (magnitude & 0x7FFFFFu) | 0x800000u;
        const uint32_t shift = 126 - exponent;          // 14..24
        uint32_t half = mantissa >> shift;
        const uint32_t remainder = mantissa & ((1u << shift) - 1);
        const uint32_t halfway = 1u << (shift - 1);
        if (remainder > halfway || (remainder == halfway && (half & 1u)))
            half++;
        return (uint16_t)(sign | half);
    }
    // normal: rebias, round to nearest even
    magnitude -= 0x38000000u;
    uint32_t half = magnitude >> 13;
    const uint32_t remainder = magnitude & 0x1FFFu;
    if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u)))
        half++;
    return (uint16_t)(sign | half);
}

float jasperengine::halfToFloat(uint16_t value){
    const uint32_t sign = (uint32_t)(value & 0x8000u) << 16;
    const uint32_t exponent = (value >> 10) & 0x1Fu;
    uint32_t mantissa = value & 0x3FFu;
    uint32_t bits;
    if (exponent == 0){
        if (mantissa == 0){
            bits = sign;
        }else{
            int e = -1;
            do { e++; mantissa <<= 1; } while ((mantissa & 0x400u) == 0);
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((mantissa & 0x3FFu) << 13);
        }
    }else if (exponent == 31){
        bits = sign | 0x7F800000u | (mantissa << 13);
    }else{
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

// EXT_texture_shared_exponent's encoding
uint32_t jasperengine::packRGB9E5(float r, float g, float b){
    constexpr int kMantissaBits = 9, kBias = 15, kMaxExponent = 31;
    constexpr float kMax = (float)((1 << kMantissaBits) - 1) / (float)(1 << kMantissaBits) * (float)(1 << (kMaxExponent - kBias));
    const float rc = std::clamp(std::isnan(r) ? 0.0f : r, 0.0f, kMax);
    const float gc = std::clamp(std::isnan(g) ? 0.0f : g, 0.0f, kMax);
    const float bc = std::clamp(std::isnan(b) ? 0.0f : b, 0.0f, kMax);
    const float maxc = std::max(rc, std::max(gc, bc));
    int exponent = std::max(-kBias - 1, (int)std::floor(std::log2(std::max(maxc, 1e-30f)))) + 1 + kBias;
    double denominator = std::pow(2.0, exponent - kBias - kMantissaBits);
    const int maxm = (int)std::floor(maxc / denominator + 0.5);
    if (maxm == (1 << kMantissaBits)){
        denominator *= 2.0;
        exponent++;
    }
    const uint32_t rm = (uint32_t)std::floor(rc / denominator + 0.5);
    const uint32_t gm = (uint32_t)std::floor(gc / denominator + 0.5);
    const uint32_t bm = (uint32_t)std::floor(bc / denominator + 0.5);
    return (rm & 0x1FFu) | ((gm & 0x1FFu) << 9) | ((bm & 0x1FFu) << 18) | ((uint32_t)exponent << 27);
}

void jasperengine::unpackRGB9E5(uint32_t packed, float& r, float& g, float& b){
    const int exponent = (int)(packed >> 27) - 15 - 9;
    const float scale = std::ldexp(1.0f, exponent);
    r = (float)(packed & 0x1FFu) * scale;
    g = (float)((packed >> 9) & 0x1FFu) * scale;
    b = (float)((packed >> 18) & 0x1FFu) * scale;
}
