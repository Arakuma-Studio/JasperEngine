// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BasisTranscode.h"

#if defined(JASPERENGINE_BASIS_TRANSCODER)
#include "basisu_transcoder.h"

#include <algorithm>
#include <mutex>
#endif

using namespace jasperengine;

bool BasisTranscode::isAvailable(){
#if defined(JASPERENGINE_BASIS_TRANSCODER)
    return true;
#else
    return false;
#endif
}

#if defined(JASPERENGINE_BASIS_TRANSCODER)

struct BasisTranscode::Session::State {
    Ktx2Image image;
    bool hasAlpha = false;
    // ETC1S: the codebooks of the file and an image description per (level, face) of the file
    basist::basisu_lowlevel_etc1s_transcoder etc1s;
    basist::basisu_transcoder_state etc1sState;
    std::vector<uint8_t> imageDescs;
    basist::basisu_lowlevel_uastc_ldr_4x4_transcoder uastc;
    basist::basisu_lowlevel_uastc_hdr_4x4_transcoder uastcHdr;

    uint32_t width(uint32_t level) const { return image.info.levelWidth(image.firstLevel + level); }
    uint32_t height(uint32_t level) const { return image.info.levelHeight(image.firstLevel + level); }
};

namespace {
    uint32_t basisRead32(const uint8_t* p){
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    // BasisLZ global data: a 20-byte header (endpoint and selector counts, the byte lengths of the
    // endpoints, selectors, tables and extended data), one 20-byte image description per image
    // (level-major, then face: flags, RGB slice offset and length, alpha slice offset and length,
    // within the level), then the codebooks and tables
    constexpr size_t kEtc1sHeaderSize = 20;
    constexpr size_t kEtc1sImageDescSize = 20;
    // the DFD of UASTC LDR: its first sample's channel id (KTX2's KHR_DF_CHANNEL_UASTC_*)
    constexpr size_t kDfdFirstSampleOffset = 28;
    constexpr uint32_t kUastcChannelRgba = 3, kUastcChannelRrrg = 5;
    // an ETC1S file with an alpha slice has a DFD of two samples
    constexpr uint64_t kEtc1sAlphaDfdLength = 60;

    void basisInitTranscoder(){
        static std::once_flag once;
        std::call_once(once, []{ basist::basisu_transcoder_init(); });
    }

    bool toBasisFormat(GpuTextureFormat format, basist::transcoder_texture_format& out){
        using T = basist::transcoder_texture_format;
        switch (format){
            case GpuTextureFormat::ETC2_RGB8: out = T::cTFETC1_RGB; return true;
            case GpuTextureFormat::ETC2_RGBA8: out = T::cTFETC2_RGBA; return true;
            case GpuTextureFormat::BC1: out = T::cTFBC1_RGB; return true;
            case GpuTextureFormat::BC3: out = T::cTFBC3_RGBA; return true;
            case GpuTextureFormat::BC4: out = T::cTFBC4_R; return true;
            case GpuTextureFormat::BC5: out = T::cTFBC5_RG; return true;
            case GpuTextureFormat::BC6H: out = T::cTFBC6H; return true;
            case GpuTextureFormat::BC7: out = T::cTFBC7_RGBA; return true;
            case GpuTextureFormat::EAC_R11: out = T::cTFETC2_EAC_R11; return true;
            case GpuTextureFormat::EAC_RG11: out = T::cTFETC2_EAC_RG11; return true;
            case GpuTextureFormat::ASTC_4x4: out = T::cTFASTC_LDR_4x4_RGBA; return true;
            case GpuTextureFormat::RGBA8: case GpuTextureFormat::R8: case GpuTextureFormat::RG8: out = T::cTFRGBA32; return true;
            case GpuTextureFormat::RGB565: out = T::cTFRGB565; return true;
            case GpuTextureFormat::RGBA4: out = T::cTFRGBA4444; return true;
            case GpuTextureFormat::RGBA16F: out = T::cTFRGBA_HALF; return true;
            case GpuTextureFormat::RGB9E5: out = T::cTFRGB_9E5; return true;
            default: return false;
        }
    }

    bool basisTarget(GpuTextureFormat target, basist::transcoder_texture_format& format, std::string& error){
        if (!toBasisFormat(target, format)){
            error = std::string("Basis files cannot be transcoded to ") + gpuFormatName(target);
            return false;
        }
        return true;
    }

    uint32_t basisFlags(GpuTextureFormat target){
        uint32_t flags = basist::cDecodeFlagsHighQuality;
        if (target == GpuTextureFormat::BC1)
            flags |= basist::cDecodeFlagsBC1ForbidThreeColorBlocks;   // sampled as BC1 RGBA: no transparent black
        return flags;
    }

    // R8 and RG8 keep the first channels of an RGBA transcode
    size_t reducedChannels(GpuTextureFormat target){
        return target == GpuTextureFormat::R8 ? 1 : target == GpuTextureFormat::RG8 ? 2 : 0;
    }

    void reduceRgba(const std::vector<uint8_t>& rgba, size_t texels, size_t channels, uint8_t* out){
        for (size_t i = 0; i < texels; i++)
            for (size_t c = 0; c < channels; c++)
                out[i * channels + c] = rgba[i * 4 + c];
    }

    // the bytes of one face of a level: blocks, or pixels (one or two channels for R8 / RG8)
    size_t basisFaceBytes(basist::transcoder_texture_format format, GpuTextureFormat target, uint32_t width, uint32_t height){
        const size_t reduced = reducedChannels(target);
        if (reduced)
            return (size_t)width * height * reduced;
        if (basist::basis_transcoder_format_is_uncompressed(format))
            return (size_t)width * height * basist::basis_get_bytes_per_block_or_pixel(format);
        return (size_t)((width + 3) / 4) * ((height + 3) / 4) * basist::basis_get_bytes_per_block_or_pixel(format);
    }
}

void BasisTranscode::warmUp(){
    basisInitTranscoder();
}

BasisTranscode::Session::Session(): state(new State()){}

BasisTranscode::Session::~Session(){
    delete state;
}

bool BasisTranscode::Session::open(Ktx2Image image, std::string& error){
    basisInitTranscoder();
    State& s = *state;
    s.image = std::move(image);
    const Ktx2Info& info = s.image.info;
    if (info.codec == Ktx2Codec::Raw){
        error = "not a Basis KTX2 file";
        return false;
    }
    if (s.image.levels.empty() || s.image.firstLevel + s.image.levels.size() > info.levelCount || (info.faces != 1 && info.faces != 6) ||
        s.image.metadata.size() < info.dfdOffset + kDfdFirstSampleOffset + 4){
        error = "incomplete KTX2 image";
        return false;
    }
    if (info.codec == Ktx2Codec::UASTC){
        const uint32_t channel = (basisRead32(s.image.metadata.data() + info.dfdOffset + kDfdFirstSampleOffset) >> 24) & 15;
        s.hasAlpha = channel == kUastcChannelRgba || channel == kUastcChannelRrrg;
    }
    if (info.codec != Ktx2Codec::ETC1S)
        return true;

    s.hasAlpha = info.dfdLength == kEtc1sAlphaDfdLength;
    const std::vector<uint8_t>& g = s.image.globalData;
    const size_t images = (size_t)info.levelCount * info.faces;
    if (g.size() < kEtc1sHeaderSize + images * kEtc1sImageDescSize){
        error = "bad BasisLZ global data";
        return false;
    }
    const uint32_t endpointCount = (uint32_t)g[0] | ((uint32_t)g[1] << 8);
    const uint32_t selectorCount = (uint32_t)g[2] | ((uint32_t)g[3] << 8);
    const uint32_t endpointsBytes = basisRead32(&g[4]), selectorsBytes = basisRead32(&g[8]), tablesBytes = basisRead32(&g[12]);
    const size_t codebooksAt = kEtc1sHeaderSize + images * kEtc1sImageDescSize;
    if (!endpointCount || !selectorCount || !endpointsBytes || !selectorsBytes || !tablesBytes ||
        codebooksAt + (uint64_t)endpointsBytes + selectorsBytes + tablesBytes > g.size()){
        error = "bad BasisLZ global data";
        return false;
    }
    s.imageDescs.assign(g.begin() + kEtc1sHeaderSize, g.begin() + (ptrdiff_t)codebooksAt);
    const uint8_t* endpoints = g.data() + codebooksAt;
    if (!s.etc1s.decode_tables(endpoints + endpointsBytes + selectorsBytes, tablesBytes) ||
        !s.etc1s.decode_palettes(endpointCount, endpoints, endpointsBytes, selectorCount, endpoints + endpointsBytes, selectorsBytes)){
        error = "the Basis transcoder cannot read this file's ETC1S codebooks";
        return false;
    }
    return true;
}

uint32_t BasisTranscode::Session::levelCount() const{
    return (uint32_t)state->image.levels.size();
}

uint32_t BasisTranscode::Session::faces() const{
    return state->image.info.faces;
}

bool BasisTranscode::Session::transcodesInRows() const{
    return state->image.info.codec == Ktx2Codec::UASTC || state->image.info.codec == Ktx2Codec::UASTC_HDR;
}

uint32_t BasisTranscode::Session::blockRows(uint32_t level) const{
    return (state->height(level) + 3) / 4;
}

size_t BasisTranscode::Session::levelBytes(uint32_t level, GpuTextureFormat target) const{
    basist::transcoder_texture_format format;
    if (!toBasisFormat(target, format))
        return 0;
    return basisFaceBytes(format, target, state->width(level), state->height(level)) * faces();
}

bool BasisTranscode::Session::transcodeLevel(uint32_t level, GpuTextureFormat target, TextureContent content,
                                             std::vector<uint8_t>& out, std::string& error){
    if (level >= levelCount()){
        error = "no such Basis level";
        return false;
    }
    if (transcodesInRows()){
        out.assign(levelBytes(level, target), 0);
        return transcodeRows(level, 0, blockRows(level), target, content, out, error);
    }

    // ETC1S: a slice is one entropy-coded stream, transcoded whole
    State& s = *state;
    basist::transcoder_texture_format format;
    if (!basisTarget(target, format, error))
        return false;
    const uint32_t width = s.width(level), height = s.height(level);
    const uint32_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
    const bool uncompressed = basist::basis_transcoder_format_is_uncompressed(format);
    const uint32_t units = uncompressed ? width * height : blocksX * blocksY;
    const size_t reduced = reducedChannels(target);
    const std::vector<uint8_t>& data = s.image.levels[level];
    const uint32_t fileLevel = s.image.firstLevel + level;
    std::vector<uint8_t> faceOut;
    out.clear();
    for (uint32_t face = 0; face < faces(); face++){
        const uint8_t* desc = s.imageDescs.data() + ((size_t)fileLevel * faces() + face) * kEtc1sImageDescSize;
        const uint32_t rgbOffset = basisRead32(desc + 4), rgbLength = basisRead32(desc + 8);
        const uint32_t alphaOffset = basisRead32(desc + 12), alphaLength = basisRead32(desc + 16);
        faceOut.assign((size_t)units * basist::basis_get_bytes_per_block_or_pixel(format), 0);
        if (!s.etc1s.transcode_image(format, faceOut.data(), units, data.data(), (uint32_t)data.size(), blocksX, blocksY, width, height,
                                     fileLevel, rgbOffset, rgbLength, alphaLength ? alphaOffset : 0, alphaLength, basisFlags(target),
                                     s.hasAlpha, false, 0, &s.etc1sState)){
            error = std::string("the Basis transcode to ") + gpuFormatName(target) + " failed";
            return false;
        }
        if (reduced){
            const size_t at = out.size();
            out.resize(at + (size_t)width * height * reduced);
            reduceRgba(faceOut, (size_t)width * height, reduced, out.data() + at);
        }else{
            out.insert(out.end(), faceOut.begin(), faceOut.end());
        }
    }
    return true;
}

bool BasisTranscode::Session::transcodeRows(uint32_t level, uint32_t firstRow, uint32_t rows, GpuTextureFormat target, TextureContent content,
                                            std::vector<uint8_t>& out, std::string& error){
    State& s = *state;
    basist::transcoder_texture_format format;
    if (!basisTarget(target, format, error))
        return false;
    if (!transcodesInRows() || level >= levelCount() || firstRow + rows > blockRows(level) || out.size() != levelBytes(level, target)){
        error = "bad Basis row transcode";
        return false;
    }
    if (rows == 0)
        return true;
    const uint32_t width = s.width(level), height = s.height(level);
    const uint32_t blocksX = (width + 3) / 4, blocksY = blockRows(level);
    constexpr size_t kBlockBytes = 16;
    const std::vector<uint8_t>& data = s.image.levels[level];
    if (data.size() < (size_t)blocksX * blocksY * kBlockBytes * faces()){
        error = "short Basis level";
        return false;
    }
    const bool uncompressed = basist::basis_transcoder_format_is_uncompressed(format);
    const uint32_t unitBytes = basist::basis_get_bytes_per_block_or_pixel(format);
    const size_t reduced = reducedChannels(target);
    const size_t faceBytes = basisFaceBytes(format, target, width, height);
    // the band: rows of blocks, and the pixel rows they cover (the last band may be short)
    const uint32_t bandHeight = std::min(rows * 4, height - firstRow * 4);
    const uint32_t units = uncompressed ? width * bandHeight : blocksX * rows;
    // normal maps are stored RG (D8): their two channels are R and G, not basisu's R and A
    const int channel0 = content == TextureContent::Normal ? 0 : -1;
    const int channel1 = content == TextureContent::Normal ? 1 : -1;
    const uint32_t fileLevel = s.image.firstLevel + level;
    std::vector<uint8_t> rgba;
    for (uint32_t face = 0; face < faces(); face++){
        const size_t sourceAt = ((size_t)face * blocksY + firstRow) * blocksX * kBlockBytes;
        const uint32_t sourceBytes = (uint32_t)((size_t)rows * blocksX * kBlockBytes);
        const size_t bandAt = uncompressed ? (size_t)firstRow * 4 * width * (reduced ? reduced : unitBytes)
                                           : (size_t)firstRow * blocksX * unitBytes;
        uint8_t* destination = out.data() + face * faceBytes + bandAt;
        if (reduced){
            rgba.assign((size_t)units * unitBytes, 0);
            destination = rgba.data();
        }
        const bool ok = s.image.info.codec == Ktx2Codec::UASTC_HDR
            ? s.uastcHdr.transcode_image(format, destination, units, data.data() + sourceAt, sourceBytes, blocksX, rows, width, bandHeight,
                                         fileLevel, 0, sourceBytes, basisFlags(target), false, false, 0, nullptr, 0, channel0, channel1)
            : s.uastc.transcode_image(format, destination, units, data.data() + sourceAt, sourceBytes, blocksX, rows, width, bandHeight,
                                      fileLevel, 0, sourceBytes, basisFlags(target), s.hasAlpha, false, 0, nullptr, 0, channel0, channel1);
        if (!ok){
            error = std::string("the Basis transcode to ") + gpuFormatName(target) + " failed";
            return false;
        }
        if (reduced)
            reduceRgba(rgba, (size_t)width * bandHeight, reduced, out.data() + face * faceBytes + bandAt);
    }
    return true;
}

#else

void BasisTranscode::warmUp(){}

struct BasisTranscode::Session::State {};

BasisTranscode::Session::Session(): state(new State()){}

BasisTranscode::Session::~Session(){
    delete state;
}

bool BasisTranscode::Session::open(Ktx2Image, std::string& error){
    error = "Basis Universal textures are not supported by this build";
    return false;
}

uint32_t BasisTranscode::Session::levelCount() const{
    return 0;
}

uint32_t BasisTranscode::Session::faces() const{
    return 0;
}

bool BasisTranscode::Session::transcodeLevel(uint32_t, GpuTextureFormat, TextureContent, std::vector<uint8_t>&, std::string& error){
    error = "Basis Universal textures are not supported by this build";
    return false;
}

bool BasisTranscode::Session::transcodesInRows() const{
    return false;
}

uint32_t BasisTranscode::Session::blockRows(uint32_t) const{
    return 0;
}

size_t BasisTranscode::Session::levelBytes(uint32_t, GpuTextureFormat) const{
    return 0;
}

bool BasisTranscode::Session::transcodeRows(uint32_t, uint32_t, uint32_t, GpuTextureFormat, TextureContent, std::vector<uint8_t>&,
                                            std::string& error){
    error = "Basis Universal textures are not supported by this build";
    return false;
}

#endif
