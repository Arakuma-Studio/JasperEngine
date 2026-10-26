// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BasisTranscode.h"

#include <cstring>
#include <mutex>

#if defined(JASPERENGINE_BASIS_TRANSCODER)
#include "basisu_transcoder.h"
#endif

using namespace jasperengine;

namespace {
    void basisPut32(std::vector<uint8_t>& out, size_t at, uint32_t v){
        for (int i = 0; i < 4; i++) out[at + i] = (uint8_t)(v >> (8 * i));
    }
    void basisPut64(std::vector<uint8_t>& out, size_t at, uint64_t v){
        basisPut32(out, at, (uint32_t)v);
        basisPut32(out, at + 4, (uint32_t)(v >> 32));
    }
    // BasisLZ global data: a 20-byte header, then one 20-byte image description per image
    // (level-major, then face), then the codebooks and tables
    constexpr size_t kEtc1sHeaderSize = 20;
    constexpr size_t kEtc1sImageDescSize = 20;
}

bool BasisTranscode::isAvailable(){
#if defined(JASPERENGINE_BASIS_TRANSCODER)
    return true;
#else
    return false;
#endif
}

bool BasisTranscode::rebase(const Ktx2Image& image, std::vector<uint8_t>& out, std::string& error){
    const Ktx2Info& info = image.info;
    if (info.codec == Ktx2Codec::Raw){
        error = "not a Basis KTX2 file";
        return false;
    }
    if (image.metadata.size() < info.metadataEnd || image.levels.empty()){
        error = "incomplete KTX2 image";
        return false;
    }
    const uint32_t levelCount = (uint32_t)image.levels.size();
    const uint32_t first = image.firstLevel;
    const bool etc1s = info.codec == Ktx2Codec::ETC1S;

    std::vector<uint8_t> globalData;
    if (etc1s){
        const size_t images = (size_t)info.levelCount * info.faces;
        if (image.globalData.size() < kEtc1sHeaderSize + images * kEtc1sImageDescSize){
            error = "bad BasisLZ global data";
            return false;
        }
        const uint8_t* g = image.globalData.data();
        globalData.assign(g, g + kEtc1sHeaderSize);
        const size_t keptFrom = kEtc1sHeaderSize + (size_t)first * info.faces * kEtc1sImageDescSize;
        globalData.insert(globalData.end(), g + keptFrom, g + keptFrom + (size_t)levelCount * info.faces * kEtc1sImageDescSize);
        const size_t tablesAt = kEtc1sHeaderSize + images * kEtc1sImageDescSize;
        globalData.insert(globalData.end(), g + tablesAt, g + image.globalData.size());
    }

    // header, level index, the DFD and key/values as they were (the transcoder wants them
    // adjacent), the global data, then the levels smallest first
    const size_t indexAt = Ktx2::kHeaderSize;
    out.assign(image.metadata.begin(), image.metadata.begin() + Ktx2::kHeaderSize);
    out.resize(indexAt + (size_t)levelCount * Ktx2::kLevelIndexEntrySize, 0);
    const uint64_t dfdOffset = out.size();
    out.insert(out.end(), image.metadata.begin() + (size_t)info.dfdOffset, image.metadata.begin() + (size_t)(info.dfdOffset + info.dfdLength));
    const uint64_t kvdOffset = out.size();
    if (info.kvdLength > 0)
        out.insert(out.end(), image.metadata.begin() + (size_t)info.kvdOffset, image.metadata.begin() + (size_t)(info.kvdOffset + info.kvdLength));
    uint64_t sgdOffset = 0;
    if (!globalData.empty()){
        while (out.size() % 8) out.push_back(0);
        sgdOffset = out.size();
        out.insert(out.end(), globalData.begin(), globalData.end());
    }

    basisPut32(out, 20, info.levelWidth(first));
    basisPut32(out, 24, info.levelHeight(first));
    basisPut32(out, 40, levelCount);
    // Ktx2::read inflated Zstandard levels; BasisLZ ones are kept as they are
    basisPut32(out, 44, etc1s ? (uint32_t)Ktx2Supercompression::BasisLZ : (uint32_t)Ktx2Supercompression::None);
    basisPut32(out, 48, (uint32_t)dfdOffset);
    basisPut32(out, 52, (uint32_t)info.dfdLength);
    basisPut32(out, 56, info.kvdLength > 0 ? (uint32_t)kvdOffset : 0);
    basisPut32(out, 60, (uint32_t)info.kvdLength);
    basisPut64(out, 64, sgdOffset);
    basisPut64(out, 72, globalData.size());

    for (int level = (int)levelCount - 1; level >= 0; level--){
        const std::vector<uint8_t>& bytes = image.levels[level];
        if (!etc1s){
            while (out.size() % 16) out.push_back(0);
        }
        const size_t at = indexAt + (size_t)level * Ktx2::kLevelIndexEntrySize;
        basisPut64(out, at, out.size());
        basisPut64(out, at + 8, bytes.size());
        basisPut64(out, at + 16, etc1s ? 0 : bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return true;
}

#if defined(JASPERENGINE_BASIS_TRANSCODER)

struct BasisTranscode::Session::State {
    std::vector<uint8_t> file;
    basist::ktx2_transcoder transcoder;
};

namespace {
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
}

BasisTranscode::Session::Session(): state(new State()){}

BasisTranscode::Session::~Session(){
    delete state;
}

bool BasisTranscode::Session::open(std::vector<uint8_t> file, std::string& error){
    basisInitTranscoder();
    state->file = std::move(file);
    if (!state->transcoder.init(state->file.data(), (uint32_t)state->file.size()) || !state->transcoder.start_transcoding()){
        error = "the Basis transcoder cannot read this KTX2 file";
        return false;
    }
    return true;
}

uint32_t BasisTranscode::Session::levelCount() const{
    return state->transcoder.get_levels();
}

uint32_t BasisTranscode::Session::faces() const{
    return state->transcoder.get_faces();
}

bool BasisTranscode::Session::transcodeLevel(uint32_t level, GpuTextureFormat target, TextureContent content,
                                             std::vector<uint8_t>& out, std::string& error){
    basist::transcoder_texture_format format;
    if (!toBasisFormat(target, format)){
        error = std::string("Basis files cannot be transcoded to ") + gpuFormatName(target);
        return false;
    }
    const uint32_t faceCount = faces();
    out.clear();
    for (uint32_t face = 0; face < faceCount; face++){
        basist::ktx2_image_level_info levelInfo;
        if (!state->transcoder.get_image_level_info(levelInfo, level, 0, face)){
            error = "no such Basis level";
            return false;
        }
        const uint32_t width = levelInfo.m_orig_width;
        const uint32_t height = levelInfo.m_orig_height;
        const bool blocks = basist::basis_transcoder_format_is_uncompressed(format) == false;
        const uint32_t units = blocks ? levelInfo.m_total_blocks : width * height;
        const uint32_t unitBytes = basist::basis_get_bytes_per_block_or_pixel(format);
        std::vector<uint8_t> faceOut((size_t)units * unitBytes);
        uint32_t flags = basist::cDecodeFlagsHighQuality;
        if (target == GpuTextureFormat::BC1)
            flags |= basist::cDecodeFlagsBC1ForbidThreeColorBlocks;   // sampled as BC1 RGBA: no transparent black
        // normal maps are stored RG (D8): their two channels are R and G, not basisu's R and A
        const int channel0 = content == TextureContent::Normal ? 0 : -1;
        const int channel1 = content == TextureContent::Normal ? 1 : -1;
        if (!state->transcoder.transcode_image_level(level, 0, face, faceOut.data(), units, format, flags, 0, 0, channel0, channel1)){
            error = std::string("the Basis transcode to ") + gpuFormatName(target) + " failed";
            return false;
        }
        if (target == GpuTextureFormat::R8 || target == GpuTextureFormat::RG8){
            const size_t channels = target == GpuTextureFormat::R8 ? 1 : 2;
            std::vector<uint8_t> reduced((size_t)width * height * channels);
            for (size_t i = 0; i < (size_t)width * height; i++)
                for (size_t c = 0; c < channels; c++)
                    reduced[i * channels + c] = faceOut[i * 4 + c];
            faceOut = std::move(reduced);
        }
        out.insert(out.end(), faceOut.begin(), faceOut.end());
    }
    return true;
}

#else

struct BasisTranscode::Session::State {};

BasisTranscode::Session::Session(): state(new State()){}

BasisTranscode::Session::~Session(){
    delete state;
}

bool BasisTranscode::Session::open(std::vector<uint8_t> file, std::string& error){
    error = "Basis Universal textures are not supported by this build";
    return false;
}

uint32_t BasisTranscode::Session::levelCount() const{
    return 0;
}

uint32_t BasisTranscode::Session::faces() const{
    return 0;
}

bool BasisTranscode::Session::transcodeLevel(uint32_t level, GpuTextureFormat target, TextureContent content,
                                             std::vector<uint8_t>& out, std::string& error){
    error = "Basis Universal textures are not supported by this build";
    return false;
}

#endif
