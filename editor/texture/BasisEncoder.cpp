// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Basis Universal of the web family (ETC1S, UASTC with RDO, UASTC HDR 4x4), and the formats
// that come from UASTC transcodes (D1): ETC2 / EAC for android_etc2, BC6H for desktop HDR.
// UASTC LDR is encoded block by block in fixed bands (encode_uastc, then uastc_rdo per band) on
// the job system; ETC1S and UASTC HDR go through basis_compressor on one thread of its own pool
// (its global codebooks are per file), which keeps their bytes independent of the thread count.

#include "TextureEncoderParts.h"

#include "texture/Ktx2.h"
#include "texture/TextureFallback.h"
#include "jobs/JobSystem.h"

#include "encoder/basisu_comp.h"
#include "encoder/basisu_enc.h"
#include "encoder/basisu_gpu_texture.h"
#include "encoder/basisu_uastc_enc.h"
#include "transcoder/basisu_transcoder.h"

#include <algorithm>
#include <atomic>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    void initBasis(){
        static std::once_flag once;
        std::call_once(once, []{
            basisu::basisu_encoder_init();
            basist::basisu_transcoder_init();
        });
    }

    uint32_t uastcLevel(TextureEffort effort){
        switch (effort){
            case TextureEffort::Fast: return basisu::cPackUASTCLevelFaster;
            case TextureEffort::Best: return basisu::cPackUASTCLevelSlower;
            case TextureEffort::Normal: default: return basisu::cPackUASTCLevelDefault;
        }
    }

    Ktx2::UastcChannels uastcChannels(const PreparedTexture& prepared){
        switch (prepared.layout){
            case PreparedTexture::Layout::RG8: return Ktx2::UastcChannels::RG;
            case PreparedTexture::Layout::R8: return Ktx2::UastcChannels::RRR;
            default: return prepared.hasAlpha ? Ktx2::UastcChannels::RGBA : Ktx2::UastcChannels::RGB;
        }
    }

    // one face of a level, UASTC blocks of fixed bands, RDO within each band
    bool encodeUastcImage(const MipLevel& image, int channels, uint32_t flags, float lambda, const std::function<bool()>& cancelled,
                          std::vector<uint8_t>& out){
        const int blocksX = (image.width + 3) / 4;
        const int blocksY = (image.height + 3) / 4;
        out.assign((size_t)blocksX * (size_t)blocksY * 16, 0);
        const int bands = (blocksY + kTextureBandRows - 1) / kTextureBandRows;
        std::atomic<bool> stopped{false};
        auto encodeBand = [&](size_t band) -> bool {
            if (cancelled && cancelled()){
                stopped = true;
                return false;
            }
            const int firstRow = (int)band * kTextureBandRows;
            const int lastRow = std::min(blocksY, firstRow + kTextureBandRows);
            const size_t count = (size_t)(lastRow - firstRow) * (size_t)blocksX;
            std::vector<basisu::color_rgba> pixels(count * 16);
            basist::uastc_block* blocks = reinterpret_cast<basist::uastc_block*>(&out[(size_t)firstRow * blocksX * 16]);
            for (int by = firstRow; by < lastRow; by++){
                for (int bx = 0; bx < blocksX; bx++){
                    const size_t index = (size_t)(by - firstRow) * blocksX + bx;
                    uint8_t* rgba = reinterpret_cast<uint8_t*>(&pixels[index * 16]);
                    gatherBlockRgba(image, channels, bx, by, rgba);
                    // a normal map's Y also in alpha: the transcodes to BC5 and EAC RG11 read R and A
                    // (basisu's UASTC layout for two channels), the others R and G as the shaders do
                    if (channels == 2)
                        for (int t = 0; t < 16; t++)
                            rgba[t * 4 + 3] = rgba[t * 4 + 1];
                    basisu::encode_uastc(rgba, blocks[index], flags);
                }
            }
            if (lambda > 0.0f){
                basisu::uastc_rdo_params params;
                params.m_lambda = lambda;
                // twice basisu's default window: more matches for zstd within a band
                params.m_lz_dict_size = 8192;
                basisu::uastc_rdo((uint32_t)count, blocks, pixels.data(), params, flags, nullptr, 0);
            }
            return true;
        };
        jobs::ParallelForOptions options;
        options.lane = jobs::Lane::Background;
        jobs::system().parallelFor((size_t)bands, encodeBand, options);
        return !stopped && !(cancelled && cancelled());
    }

    bool encodeUastcLevels(const TextureEncodeRequest& request, float lambda, std::vector<std::vector<uint8_t>>& levels){
        const PreparedTexture& prepared = *request.prepared;
        const int channels = (int)prepared.texelBytes();
        const uint32_t flags = uastcLevel(request.effort);
        levels.assign(prepared.levelCount(), {});
        for (int level = 0; level < prepared.levelCount(); level++){
            for (const std::vector<MipLevel>& face : prepared.faces){
                std::vector<uint8_t> blocks;
                if (!encodeUastcImage(face[level], channels, flags, lambda, request.cancelled, blocks))
                    return false;
                levels[level].insert(levels[level].end(), blocks.begin(), blocks.end());
            }
        }
        return true;
    }

    bool writeUastcKtx2(const TextureEncodeRequest& request, const std::vector<std::vector<uint8_t>>& levels, int zstdLevel,
                        std::vector<uint8_t>& ktx2, std::string& error){
        const PreparedTexture& prepared = *request.prepared;
        Ktx2::WriteDesc desc;
        desc.codec = Ktx2Codec::UASTC;
        desc.uastcChannels = uastcChannels(prepared);
        desc.srgb = prepared.srgb;
        desc.width = (uint32_t)prepared.width;
        desc.height = (uint32_t)prepared.height;
        desc.faces = (uint32_t)prepared.faces.size();
        for (const auto& level : levels)
            desc.levels.push_back(level);
        desc.keyValues = request.keyValues;
        return Ktx2::write(desc, ktx2, error, zstdLevel, zstdCompress);
    }

    basisu::image toBasisImage(const MipLevel& level, int channels){
        basisu::image out(level.width, level.height);
        for (int y = 0; y < level.height; y++){
            for (int x = 0; x < level.width; x++){
                const uint8_t* p = &level.pixels[((size_t)y * level.width + x) * channels];
                basisu::color_rgba& c = out(x, y);
                switch (channels){
                    case 1: c.set(p[0], p[0], p[0], 255); break;
                    case 2: c.set(p[0], p[1], 0, 255); break;
                    default: c.set(p[0], p[1], p[2], p[3]); break;
                }
            }
        }
        return out;
    }

    basisu::imagef toBasisImageHdr(const MipLevel& level){
        basisu::imagef out(level.width, level.height);
        const float* p = reinterpret_cast<const float*>(level.pixels.data());
        for (int y = 0; y < level.height; y++){
            for (int x = 0; x < level.width; x++){
                const float* t = p + ((size_t)y * level.width + x) * 4;
                out(x, y) = basisu::vec4F(t[0], t[1], t[2], 1.0f);
            }
        }
        return out;
    }

    // ETC1S and UASTC HDR: basis_compressor, levels given (no mip generation of its own)
    bool compressWithBasisu(const TextureEncodeRequest& request, bool hdr, std::vector<uint8_t>& ktx2, std::string& error){
        const PreparedTexture& prepared = *request.prepared;
        basisu::basis_compressor_params params;
        params.m_status_output = false;
        params.m_print_stats = false;
        params.m_compute_stats = false;
        params.m_debug = false;
        params.m_multithreading = false;
        params.m_mip_gen = false;
        params.m_create_ktx2_file = true;
        params.m_read_source_images = false;
        params.m_write_output_basis_or_ktx2_files = false;
        params.m_tex_type = prepared.faces.size() == 6 ? basist::cBASISTexTypeCubemapArray : basist::cBASISTexType2D;
        const int channels = (int)prepared.texelBytes();
        if (hdr){
            params.set_format_mode(basist::basis_tex_format::cUASTC_HDR_4x4);
            const int level = request.effort == TextureEffort::Fast ? 0 : request.effort == TextureEffort::Best ? 3 : 1;
            params.m_uastc_hdr_4x4_options.set_quality_level(level);
            params.m_ktx2_uastc_supercompression = basist::KTX2_SS_ZSTANDARD;
            params.m_ktx2_zstd_supercompression_level = std::max(1, request.choice.zstdLevel);
            params.m_ktx2_and_basis_srgb_transfer_function = false;
            params.m_perceptual = false;
            for (const std::vector<MipLevel>& face : prepared.faces){
                params.m_source_images_hdr.push_back(toBasisImageHdr(face[0]));
                basisu::vector<basisu::imagef> mips;
                for (size_t level = 1; level < face.size(); level++)
                    mips.push_back(toBasisImageHdr(face[level]));
                params.m_source_mipmap_images_hdr.push_back(mips);
            }
        }else{
            params.set_format_mode(basist::basis_tex_format::cETC1S);
            params.m_quality_level = std::clamp(request.choice.etc1sQuality, 1, 255);
            params.m_etc1s_compression_level = request.effort == TextureEffort::Fast ? 1 : request.effort == TextureEffort::Best ? 4 : 2;
            params.set_srgb_options(prepared.srgb);
            params.m_check_for_alpha = false;
            params.m_force_alpha = prepared.hasAlpha;
            for (const std::vector<MipLevel>& face : prepared.faces){
                params.m_source_images.push_back(toBasisImage(face[0], channels));
                basisu::vector<basisu::image> mips;
                for (size_t level = 1; level < face.size(); level++)
                    mips.push_back(toBasisImage(face[level], channels));
                params.m_source_mipmap_images.push_back(mips);
            }
        }
        // one thread: the encode is a Background job of the cook already, and a fixed pool
        // keeps ETC1S's codebook search independent of the machine's core count
        basisu::job_pool pool(1);
        params.m_pJob_pool = &pool;
        basisu::basis_compressor compressor;
        if (!compressor.init(params)){
            error = "basisu rejected the parameters";
            return false;
        }
        if (request.cancelled && request.cancelled()){
            error = "cancelled";
            return false;
        }
        const basisu::basis_compressor::error_code result = compressor.process();
        if (result != basisu::basis_compressor::cECSuccess){
            error = "basisu failed to encode (error " + std::to_string((int)result) + ")";
            return false;
        }
        const basisu::uint8_vec& file = compressor.get_output_ktx2_file();
        return Ktx2::rewriteKeyValues(std::span<const uint8_t>(file.data(), file.size()), request.keyValues, ktx2, error);
    }

    class BasisEncoder final : public ITextureEncoder {
    public:
        const char* name() const override { return "basisu"; }
        std::string version() const override { return std::string("basisu ") + BASISU_LIB_VERSION_STRING + " bands16 rg-alpha dict8k"; }
        // SSE off, but not proven identical across CPUs (D11)
        bool isaInvariant() const override { return false; }

        bool encode(const TextureEncodeRequest& request, std::vector<uint8_t>& ktx2, std::string& error) const override{
            initBasis();
            const PreparedTexture& prepared = *request.prepared;
            const TextureFormatChoice& choice = request.choice;
            const bool hdrPixels = prepared.layout == PreparedTexture::Layout::RGBAF;
            switch (choice.codec){
                case TextureCodec::ETC1S:
                    if (hdrPixels || prepared.layout == PreparedTexture::Layout::R16){
                        error = "ETC1S encodes LDR 8-bit levels";
                        return false;
                    }
                    return compressWithBasisu(request, false, ktx2, error);
                case TextureCodec::UASTC: {
                    if (hdrPixels || prepared.layout == PreparedTexture::Layout::R16){
                        error = "UASTC LDR encodes LDR 8-bit levels";
                        return false;
                    }
                    std::vector<std::vector<uint8_t>> levels;
                    if (!encodeUastcLevels(request, choice.rdoLambda, levels)){
                        error = "cancelled";
                        return false;
                    }
                    return writeUastcKtx2(request, levels, choice.zstdLevel, ktx2, error);
                }
                case TextureCodec::UASTC_HDR:
                    if (!hdrPixels){
                        error = "UASTC HDR encodes float levels";
                        return false;
                    }
                    return compressWithBasisu(request, true, ktx2, error);
                case TextureCodec::Raw:
                default:
                    break;
            }
            // raw ETC2 / EAC and BC6H: a UASTC (HDR) encode, transcoded here at the highest quality
            std::vector<uint8_t> intermediate;
            if (choice.format == GpuTextureFormat::BC6H){
                if (!hdrPixels){
                    error = "BC6H encodes float levels";
                    return false;
                }
                if (!compressWithBasisu(request, true, intermediate, error))
                    return false;
            }else{
                if (hdrPixels){
                    error = std::string(gpuFormatName(choice.format)) + " cannot hold float levels";
                    return false;
                }
                std::vector<std::vector<uint8_t>> levels;
                if (!encodeUastcLevels(request, 0.0f, levels)){
                    error = "cancelled";
                    return false;
                }
                if (!writeUastcKtx2(request, levels, 0, intermediate, error))
                    return false;
            }
            std::vector<std::vector<uint8_t>> transcoded;
            if (!transcodeBasisLevels(intermediate, choice.format, transcoded, error))
                return false;
            return writeRawKtx2(request, choice.format, transcoded, ktx2, error);
        }
    };

    bool toTranscoderFormat(GpuTextureFormat format, basist::transcoder_texture_format& out){
        using T = basist::transcoder_texture_format;
        switch (format){
            case GpuTextureFormat::ETC2_RGB8: out = T::cTFETC1_RGB; return true;
            case GpuTextureFormat::ETC2_RGBA8: out = T::cTFETC2_RGBA; return true;
            case GpuTextureFormat::EAC_R11: out = T::cTFETC2_EAC_R11; return true;
            case GpuTextureFormat::EAC_RG11: out = T::cTFETC2_EAC_RG11; return true;
            case GpuTextureFormat::BC1: out = T::cTFBC1_RGB; return true;
            case GpuTextureFormat::BC3: out = T::cTFBC3_RGBA; return true;
            case GpuTextureFormat::BC4: out = T::cTFBC4_R; return true;
            case GpuTextureFormat::BC5: out = T::cTFBC5_RG; return true;
            case GpuTextureFormat::BC6H: out = T::cTFBC6H; return true;
            case GpuTextureFormat::BC7: out = T::cTFBC7_RGBA; return true;
            case GpuTextureFormat::ASTC_4x4: out = T::cTFASTC_LDR_4x4_RGBA; return true;
            case GpuTextureFormat::RGBA8: case GpuTextureFormat::RG8: case GpuTextureFormat::R8: out = T::cTFRGBA32; return true;
            case GpuTextureFormat::RGB565: out = T::cTFRGB565; return true;
            case GpuTextureFormat::RGBA4: out = T::cTFRGBA4444; return true;
            case GpuTextureFormat::RGBA16F: out = T::cTFRGBA_HALF; return true;
            case GpuTextureFormat::RGB9E5: out = T::cTFRGB_9E5; return true;
            default: return false;
        }
    }

}

const ITextureEncoder& editor::basisTextureEncoder(){
    static const BasisEncoder encoder;
    return encoder;
}

bool editor::transcodeBasisLevels(const std::vector<uint8_t>& file, GpuTextureFormat target, std::vector<std::vector<uint8_t>>& levels,
                                  std::string& error){
    initBasis();
    Ktx2Info info;
    if (!Ktx2::parseInfo(file, file.size(), info, error))
        return false;
    const TextureContent content = TextureFallback::contentOf(info);
    basist::transcoder_texture_format format;
    if (!toTranscoderFormat(target, format)){
        error = std::string("Basis files cannot be transcoded to ") + gpuFormatName(target);
        return false;
    }
    basist::ktx2_transcoder transcoder;
    if (!transcoder.init(file.data(), (uint32_t)file.size()) || !transcoder.start_transcoding()){
        error = "the Basis transcoder cannot read the file";
        return false;
    }
    levels.assign(transcoder.get_levels(), {});
    for (uint32_t level = 0; level < transcoder.get_levels(); level++){
        for (uint32_t face = 0; face < transcoder.get_faces(); face++){
            basist::ktx2_image_level_info levelInfo;
            if (!transcoder.get_image_level_info(levelInfo, level, 0, face)){
                error = "no such Basis level";
                return false;
            }
            const bool uncompressed = basist::basis_transcoder_format_is_uncompressed(format);
            const uint32_t units = uncompressed ? levelInfo.m_orig_width * levelInfo.m_orig_height : levelInfo.m_total_blocks;
            std::vector<uint8_t> out((size_t)units * basist::basis_get_bytes_per_block_or_pixel(format));
            uint32_t flags = basist::cDecodeFlagsHighQuality;
            if (target == GpuTextureFormat::BC1)
                flags |= basist::cDecodeFlagsBC1ForbidThreeColorBlocks;
            const int channel0 = content == TextureContent::Normal ? 0 : -1;
            const int channel1 = content == TextureContent::Normal ? 1 : -1;
            if (!transcoder.transcode_image_level(level, 0, face, out.data(), units, format, flags, 0, 0, channel0, channel1)){
                error = std::string("the Basis transcode to ") + gpuFormatName(target) + " failed";
                return false;
            }
            if (target == GpuTextureFormat::R8 || target == GpuTextureFormat::RG8){
                const size_t c = target == GpuTextureFormat::R8 ? 1 : 2;
                std::vector<uint8_t> reduced((size_t)units * c);
                for (size_t i = 0; i < units; i++)
                    for (size_t k = 0; k < c; k++)
                        reduced[i * c + k] = out[i * 4 + k];
                out = std::move(reduced);
            }
            levels[level].insert(levels[level].end(), out.begin(), out.end());
        }
    }
    return true;
}

bool editor::decodeEtcLevel(GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size, std::vector<uint8_t>& out){
    basisu::texture_format blockFormat;
    int channels = 4;
    switch (format){
        case GpuTextureFormat::ETC2_RGB8: blockFormat = basisu::texture_format::cETC1; break;
        case GpuTextureFormat::ETC2_RGBA8: blockFormat = basisu::texture_format::cETC2_RGBA; break;
        case GpuTextureFormat::EAC_R11: blockFormat = basisu::texture_format::cETC2_R11_EAC; channels = 1; break;
        case GpuTextureFormat::EAC_RG11: blockFormat = basisu::texture_format::cETC2_RG11_EAC; channels = 2; break;
        default: return false;
    }
    const size_t blockBytes = gpuFormatInfo(format).bytesPerBlock;
    const int blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
    if (size < (size_t)blocksX * blocksY * blockBytes)
        return false;
    out.assign((size_t)width * (size_t)height * channels, 0);
    for (int by = 0; by < blocksY; by++){
        for (int bx = 0; bx < blocksX; bx++){
            basisu::color_rgba pixels[16];
            if (!basisu::unpack_block(blockFormat, data + ((size_t)by * blocksX + bx) * blockBytes, pixels, false))
                return false;
            for (int y = 0; y < 4; y++){
                for (int x = 0; x < 4; x++){
                    const int px = bx * 4 + x, py = by * 4 + y;
                    if (px >= width || py >= height)
                        continue;
                    const basisu::color_rgba& c = pixels[y * 4 + x];
                    uint8_t* o = &out[((size_t)py * width + px) * channels];
                    for (int k = 0; k < channels; k++)
                        o[k] = c[k];
                }
            }
        }
    }
    return true;
}

bool editor::decodeExr(const unsigned char* bytes, size_t length, std::vector<float>& rgba, int& width, int& height, std::string& error){
    initBasis();
    basisu::imagef image;
    if (!basisu::read_exr(bytes, length, image)){
        error = "not a readable OpenEXR image";
        return false;
    }
    width = (int)image.get_width();
    height = (int)image.get_height();
    rgba.resize((size_t)width * (size_t)height * 4);
    for (int y = 0; y < height; y++){
        for (int x = 0; x < width; x++){
            const basisu::vec4F& v = image(x, y);
            float* o = &rgba[((size_t)y * width + x) * 4];
            for (int c = 0; c < 4; c++)
                o[c] = v[c];
        }
    }
    return true;
}
