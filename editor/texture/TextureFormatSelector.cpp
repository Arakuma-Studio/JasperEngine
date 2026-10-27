// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureFormatSelector.h"

#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using F = GpuTextureFormat;

    enum class Kind : uint8_t { ColorOpaque, ColorAlpha, Normal, SpriteOpaque, SpriteAlpha, Single, Hdr };

    struct Cell {
        TextureCodec codec;
        GpuTextureFormat format;
        float lambda;       // BC7 / UASTC RDO lambda; ETC1S quality level
        bool opaqueBc7;
    };

    constexpr Cell raw(GpuTextureFormat format, float lambda = 0.0f, bool opaque = false){
        return {TextureCodec::Raw, format, lambda, opaque};
    }
    constexpr Cell etc1s(float level){ return {TextureCodec::ETC1S, F::ETC2_RGB8, level, false}; }
    constexpr Cell uastc(float lambda){ return {TextureCodec::UASTC, F::ASTC_4x4, lambda, false}; }
    constexpr Cell uastcHdr(){ return {TextureCodec::UASTC_HDR, F::BC6H, 0.0f, false}; }

    // R5, [family][kind][Low, Normal, High]
    const Cell table[kTextureFamilyCount][7][3] = {
        // desktop (BC)
        {
            {raw(F::BC1), raw(F::BC1), raw(F::BC7, 0.0f, true)},
            {raw(F::BC7, 2.0f), raw(F::BC7, 1.0f), raw(F::BC7)},
            {raw(F::BC5), raw(F::BC5), raw(F::BC5)},
            {raw(F::BC7, 2.0f, true), raw(F::BC7, 0.5f, true), raw(F::BC7, 0.0f, true)},
            {raw(F::BC7, 2.0f), raw(F::BC7, 0.5f), raw(F::BC7)},
            {raw(F::BC4), raw(F::BC4), raw(F::BC4)},
            {raw(F::BC6H), raw(F::BC6H), raw(F::BC6H)},
        },
        // mobile (ASTC)
        {
            {raw(F::ASTC_8x8), raw(F::ASTC_6x6), raw(F::ASTC_4x4)},
            {raw(F::ASTC_8x8), raw(F::ASTC_6x6), raw(F::ASTC_4x4)},
            {raw(F::ASTC_8x8), raw(F::ASTC_6x6), raw(F::ASTC_5x5)},
            {raw(F::ASTC_8x8), raw(F::ASTC_6x6), raw(F::ASTC_4x4)},
            {raw(F::ASTC_8x8), raw(F::ASTC_6x6), raw(F::ASTC_4x4)},
            {raw(F::ASTC_10x10), raw(F::ASTC_8x8), raw(F::ASTC_6x6)},
            {raw(F::RGB9E5), raw(F::RGB9E5), raw(F::RGBA16F)},
        },
        // android_etc2 (ETC2 / EAC, through UASTC)
        {
            {raw(F::ETC2_RGB8), raw(F::ETC2_RGB8), raw(F::ETC2_RGB8)},
            {raw(F::ETC2_RGBA8), raw(F::ETC2_RGBA8), raw(F::ETC2_RGBA8)},
            {raw(F::EAC_RG11), raw(F::EAC_RG11), raw(F::EAC_RG11)},
            {raw(F::ETC2_RGB8), raw(F::ETC2_RGB8), raw(F::ETC2_RGB8)},
            {raw(F::ETC2_RGBA8), raw(F::ETC2_RGBA8), raw(F::ETC2_RGBA8)},
            {raw(F::EAC_R11), raw(F::EAC_R11), raw(F::EAC_R11)},
            {raw(F::RGB9E5), raw(F::RGB9E5), raw(F::RGB9E5)},
        },
        // web (KTX2 Basis, transcoded at load)
        {
            {etc1s(64), etc1s(128), uastc(1.0f)},
            {etc1s(64), etc1s(128), uastc(1.0f)},
            {uastc(2.0f), uastc(1.0f), uastc(0.0f)},
            {etc1s(128), uastc(1.0f), uastc(0.0f)},
            {etc1s(128), uastc(1.0f), uastc(0.0f)},
            {etc1s(128), etc1s(255), uastc(1.0f)},
            {uastcHdr(), uastcHdr(), uastcHdr()},
        },
    };

    Kind kindOf(const TextureFormatInputs& in){
        const TextureImportType type = in.settings.type;
        if (type == TextureImportType::HDR || (type == TextureImportType::Lightmap && in.floatSource))
            return Kind::Hdr;
        if (type == TextureImportType::Normal)
            return Kind::Normal;
        if (type == TextureImportType::SingleChannel)
            return Kind::Single;
        if (type == TextureImportType::Sprite || type == TextureImportType::UI)
            return in.hasAlpha ? Kind::SpriteAlpha : Kind::SpriteOpaque;
        return in.hasAlpha ? Kind::ColorAlpha : Kind::ColorOpaque;
    }

    GpuTextureFormat uncompressedOf(Kind kind){
        switch (kind){
            case Kind::Normal: return F::RG8;
            case Kind::Single: return F::R8;
            case Kind::Hdr: return F::RGBA16F;
            default: return F::RGBA8;
        }
    }

    bool isBcOrEtc(GpuTextureFormat format){
        return (format >= F::BC1 && format <= F::BC7) || (format >= F::ETC2_RGB8 && format <= F::EAC_RG11);
    }

    std::string lambdaText(float lambda){
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "%g", lambda);
        return buffer;
    }

}

const char* editor::textureCodecName(TextureCodec codec){
    switch (codec){
        case TextureCodec::ETC1S: return "ETC1S";
        case TextureCodec::UASTC: return "UASTC";
        case TextureCodec::UASTC_HDR: return "UASTC_HDR";
        case TextureCodec::Raw: default: return "raw";
    }
}

std::string TextureFormatChoice::label() const{
    std::string text;
    switch (codec){
        case TextureCodec::ETC1S: text = "ETC1S q" + std::to_string(etc1sQuality); break;
        case TextureCodec::UASTC: text = rdoLambda > 0.0f ? "UASTC RDO " + lambdaText(rdoLambda) : "UASTC"; break;
        case TextureCodec::UASTC_HDR: text = "UASTC HDR 4x4"; break;
        case TextureCodec::Raw: {
            std::string name = gpuFormatName(format);
            for (char& c : name) if (c == '_') c = ' ';
            text = name;
            if (format == F::BC7 && rdoLambda > 0.0f) text += " RDO " + lambdaText(rdoLambda);
            break;
        }
    }
    if (zstdLevel > 0)
        text += " + Zstd";
    return text;
}

int TextureFormatSelector::zstdLevel(TextureEffort effort){
    switch (effort){
        case TextureEffort::Fast: return 9;
        case TextureEffort::Best: return 19;
        case TextureEffort::Normal: default: return 15;
    }
}

bool TextureFormatSelector::isFormatName(const std::string& name){
    GpuTextureFormat format;
    return name == "ETC1S" || name == "UASTC" || name == "UASTC_HDR" || gpuFormatFromName(name, format);
}

std::vector<std::string> TextureFormatSelector::formatNames(){
    std::vector<std::string> names;
    for (size_t f = 0; f < kGpuTextureFormatCount; f++)
        names.push_back(gpuFormatName((GpuTextureFormat)f));
    names.push_back("ETC1S");
    names.push_back("UASTC");
    names.push_back("UASTC_HDR");
    return names;
}

bool TextureFormatSelector::needsMultipleOf4(TextureFamily family, const TextureFormatChoice& choice){
    if (choice.uncompressed)
        return false;
    if (choice.codec != TextureCodec::Raw)
        return true;        // Basis transcodes to BC / ETC2 at load
    return isBcOrEtc(choice.format);
}

TextureFormatChoice TextureFormatSelector::select(const TextureFormatInputs& in){
    TextureFormatChoice choice;
    const ResolvedTextureSettings& s = in.settings;
    const Kind kind = kindOf(in);
    const int zstd = zstdLevel(s.effort);

    // R3 / D13: CPU-readable textures are never block-compressed (R16, R8 or RGBA8 + Zstd)
    if (s.cpuReadable){
        choice.uncompressed = true;
        choice.format = in.sixteenBitSource ? F::R16 : in.singleChannelSource ? F::R8 : in.floatSource ? F::RGBA16F : F::RGBA8;
        choice.zstdLevel = zstd;
        if ((int64_t)in.width * in.height > 1024 * 1024)
            choice.warnings.push_back("Read/Write keeps " + std::to_string(in.width) + "x" + std::to_string(in.height) +
                                      " uncompressed in memory; consider a smaller Max Size");
        return choice;
    }

    if (!s.format.empty()){
        // an explicit format
        GpuTextureFormat format;
        if (s.format == "ETC1S"){
            choice.codec = TextureCodec::ETC1S;
            choice.etc1sQuality = s.rdo >= 1.0f ? (int)s.rdo : 128;
        }else if (s.format == "UASTC"){
            choice.codec = TextureCodec::UASTC;
            choice.format = F::ASTC_4x4;
            choice.rdoLambda = s.rdo >= 0.0f ? s.rdo : 1.0f;
            choice.zstdLevel = zstd;
        }else if (s.format == "UASTC_HDR"){
            choice.codec = TextureCodec::UASTC_HDR;
            choice.format = F::BC6H;
            choice.zstdLevel = zstd;
        }else if (gpuFormatFromName(s.format, format)){
            choice.format = format;
            choice.uncompressed = !gpuFormatInfo(format).compressed;
            choice.rdoLambda = (format == F::BC7 && s.rdo > 0.0f) ? s.rdo : 0.0f;
            choice.opaqueBc7 = format == F::BC7 && !in.hasAlpha;
            choice.zstdLevel = zstd;
            choice.viaUastc = (format >= F::ETC2_RGB8 && format <= F::EAC_RG11) || format == F::BC6H;
            const bool hdrFormat = gpuFormatInfo(format).hdr;
            if (hdrFormat != (kind == Kind::Hdr)){
                choice.warnings.push_back(std::string(gpuFormatName(format)) + (hdrFormat ? " is an HDR format for LDR data" : " cannot hold HDR data"));
                choice.problems.push_back(choice.warnings.back());
            }
            const bool astc = format >= F::ASTC_4x4;
            const bool bc = format >= F::BC1 && format <= F::BC7;
            if ((in.family == TextureFamily::Desktop && astc) || (in.family != TextureFamily::Desktop && in.family != TextureFamily::Web && bc)){
                choice.warnings.push_back(std::string(gpuFormatName(format)) + " is not sampled by most " + textureFamilyLabel(in.family) +
                                          " GPUs: they will decode it on the CPU or fail");
                choice.problems.push_back(choice.warnings.back());
            }
        }
    }else if (s.quality == TextureCookQuality::None){
        choice.uncompressed = true;
        choice.format = uncompressedOf(kind);
        choice.zstdLevel = zstd;
    }else{
        const int q = s.quality == TextureCookQuality::Low ? 0 : s.quality == TextureCookQuality::Normal ? 1 : 2;
        const Cell& cell = table[(size_t)in.family][(size_t)kind][q];
        choice.codec = cell.codec;
        choice.format = cell.format;
        choice.opaqueBc7 = cell.opaqueBc7;
        if (cell.codec == TextureCodec::ETC1S){
            choice.etc1sQuality = s.rdo >= 1.0f ? (int)s.rdo : (int)cell.lambda;
        }else{
            choice.rdoLambda = s.rdo >= 0.0f ? s.rdo : cell.lambda;
            choice.zstdLevel = zstd;
        }
        choice.viaUastc = cell.codec == TextureCodec::Raw &&
                          ((cell.format >= F::ETC2_RGB8 && cell.format <= F::EAC_RG11) || cell.format == F::BC6H);
    }

    // R6: block formats that need multiples of 4 at mip 0 (BC, ETC2; the web transcodes to them)
    if (needsMultipleOf4(in.family, choice) && (in.width % 4 != 0 || in.height % 4 != 0)){
        const TextureImportType type = s.type;
        if (type == TextureImportType::Sprite || type == TextureImportType::UI){
            choice.codec = TextureCodec::Raw;
            choice.format = uncompressedOf(kind);
            choice.uncompressed = true;
            choice.rdoLambda = 0.0f;
            choice.opaqueBc7 = false;
            choice.viaUastc = false;
            choice.zstdLevel = zstd;
            choice.warnings.push_back(std::to_string(in.width) + "x" + std::to_string(in.height) +
                                      " is not a multiple of 4: this " + std::string(textureImportTypeName(type)) +
                                      " texture stays uncompressed on " + textureFamilyLabel(in.family));
            choice.problems.push_back(choice.warnings.back());
        }else{
            choice.sizeMultiple = 4;
            const int w = std::max(4, ((in.width + 2) / 4) * 4), h = std::max(4, ((in.height + 2) / 4) * 4);
            choice.warnings.push_back(std::to_string(in.width) + "x" + std::to_string(in.height) + " is rescaled to " +
                                      std::to_string(w) + "x" + std::to_string(h) + " for block compression on " +
                                      textureFamilyLabel(in.family));
        }
    }
    return choice;
}
