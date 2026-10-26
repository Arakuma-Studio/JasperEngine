// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// BC1 / BC4 / BC5 (rgbcx) and BC7 (bc7enc, RDO through ert) of the desktop family, banded.

#include "TextureEncoderParts.h"

#include "BC7TransparentEndpointRepair.h"

#include "texture/Ktx2.h"
#include "jobs/JobSystem.h"

#include "bc7decomp.h"
#include "bc7enc.h"
#include "ert.h"
#include "rgbcx.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    void initBcEncoders(){
        static std::once_flag once;
        std::call_once(once, []{
            rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
            bc7enc_compress_block_init();
        });
    }

    bool unpackBc7Block(const void* block, ert::color_rgba* pixels, uint32_t, void*){
        return bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(pixels));
    }

    uint32_t bc7UberLevel(TextureEffort effort){
        switch (effort){
            case TextureEffort::Fast: return 0;
            case TextureEffort::Best: return 3;
            case TextureEffort::Normal: default: return 1;
        }
    }

    uint32_t bc1Level(TextureEffort effort){
        switch (effort){
            case TextureEffort::Fast: return 4;
            case TextureEffort::Best: return 16;
            case TextureEffort::Normal: default: return 10;
        }
    }

    class BcEncoder final : public ITextureEncoder {
    public:
        const char* name() const override { return "bc7enc_rdo"; }
        std::string version() const override { return "bc7enc_rdo b9438627 bands16"; }
        // scalar floating point, not proven identical across CPUs (D11)
        bool isaInvariant() const override { return false; }

        bool encode(const TextureEncodeRequest& request, std::vector<uint8_t>& ktx2, std::string& error) const override{
            initBcEncoders();
            const PreparedTexture& prepared = *request.prepared;
            const TextureFormatChoice& choice = request.choice;
            const GpuTextureFormat format = choice.format;
            const int channels = (int)prepared.texelBytes();
            // 8-bit levels of any layout: a block is gathered as RGBA (BC4 reads R, BC5 R and G)
            if (prepared.layout == PreparedTexture::Layout::RGBAF || prepared.layout == PreparedTexture::Layout::R16){
                error = std::string("bc7enc_rdo encodes 8-bit levels, not these as ") + gpuFormatName(format);
                return false;
            }
            bc7enc_compress_block_params bc7;
            bc7enc_compress_block_params_init(&bc7);
            bc7.m_uber_level = std::min<uint32_t>(BC7ENC_MAX_UBER_LEVEL, bc7UberLevel(request.effort));
            bc7.m_max_partitions = request.effort == TextureEffort::Fast ? 16 : BC7ENC_MAX_PARTITIONS;
            const bool rdo = format == GpuTextureFormat::BC7 && choice.rdoLambda > 0.0f;
            // perceptual weights for sRGB colour (D7); the RDO pass measures plain RGBA error, so
            // its encode does too (as bc7enc_rdo's own tool does)
            if (!prepared.srgb || rdo)
                bc7enc_compress_block_params_init_linear_weights(&bc7);
            if (rdo){
                bc7.m_mode17_partition_estimation_filterbank = false;
                if (prepared.hasAlpha){
                    bc7.m_mode5_error_weight = 0.7f;
                    bc7.m_mode6_error_weight = 0.6f;
                }else{
                    bc7.m_mode6_error_weight = 0.4f;
                }
            }

            std::vector<std::vector<uint8_t>> levels(prepared.levelCount());
            for (int level = 0; level < prepared.levelCount(); level++){
                for (const std::vector<MipLevel>& face : prepared.faces){
                    const MipLevel& image = face[level];
                    std::vector<uint8_t> blocks;
                    if (!encodeImage(image, channels, format, bc7, choice, request, blocks, error))
                        return false;
                    levels[level].insert(levels[level].end(), blocks.begin(), blocks.end());
                }
            }
            return writeRawKtx2(request, format, levels, ktx2, error);
        }

    private:
        bool encodeImage(const MipLevel& image, int channels, GpuTextureFormat format, const bc7enc_compress_block_params& bc7,
                         const TextureFormatChoice& choice, const TextureEncodeRequest& request, std::vector<uint8_t>& out,
                         std::string& error) const{
            const int blocksX = (image.width + 3) / 4;
            const int blocksY = (image.height + 3) / 4;
            const size_t blockBytes = gpuFormatInfo(format).bytesPerBlock;
            out.assign((size_t)blocksX * (size_t)blocksY * blockBytes, 0);
            const int bands = (blocksY + kTextureBandRows - 1) / kTextureBandRows;
            const bool opaque = choice.opaqueBc7;
            const float lambda = choice.rdoLambda;
            std::atomic<bool> stopped{false};

            auto encodeBand = [&](size_t band) -> bool {
                if (request.cancelled && request.cancelled()){
                    stopped = true;
                    return false;
                }
                const int firstRow = (int)band * kTextureBandRows;
                const int lastRow = std::min(blocksY, firstRow + kTextureBandRows);
                std::vector<uint8_t> pixels((size_t)(lastRow - firstRow) * (size_t)blocksX * 64);
                for (int by = firstRow; by < lastRow; by++){
                    for (int bx = 0; bx < blocksX; bx++){
                        uint8_t* rgba = &pixels[((size_t)(by - firstRow) * blocksX + bx) * 64];
                        gatherBlockRgba(image, channels, bx, by, rgba);
                        if (opaque)
                            for (int t = 0; t < 16; t++) rgba[t * 4 + 3] = 255;
                        uint8_t* dst = &out[((size_t)by * blocksX + bx) * blockBytes];
                        switch (format){
                            case GpuTextureFormat::BC1:
                                rgbcx::encode_bc1(bc1Level(request.effort), dst, rgba, false, false);
                                break;
                            case GpuTextureFormat::BC4:
                                if (request.effort == TextureEffort::Fast)
                                    rgbcx::encode_bc4(dst, rgba, 4);
                                else
                                    rgbcx::encode_bc4_hq(dst, rgba, 4);
                                break;
                            case GpuTextureFormat::BC5:
                                if (request.effort == TextureEffort::Fast)
                                    rgbcx::encode_bc5(dst, rgba, 0, 1, 4);
                                else
                                    rgbcx::encode_bc5_hq(dst, rgba, 0, 1, 4);
                                break;
                            case GpuTextureFormat::BC7:
                            default:
                                bc7enc_compress_block(dst, rgba, &bc7);
                                break;
                        }
                    }
                }
                if (format == GpuTextureFormat::BC7 && lambda > 0.0f){
                    // RDO within the band: the band size is fixed, so the bytes are too
                    ert::reduce_entropy_params params;
                    params.m_lambda = lambda;
                    params.m_lookback_window_size = 256;
                    params.m_smooth_block_max_mse_scale = 15.0f + (50.0f - 15.0f) * std::min(1.0f, lambda / 4.0f);
                    uint32_t modified = 0;
                    ert::reduce_entropy(&out[(size_t)firstRow * blocksX * blockBytes], (uint32_t)((lastRow - firstRow) * blocksX),
                                        16, 16, 4, 4, 4, reinterpret_cast<const ert::color_rgba*>(pixels.data()), params, modified,
                                        unpackBc7Block, nullptr);
                }
                return true;
            };

            jobs::ParallelForOptions options;
            options.lane = jobs::Lane::Background;
            jobs::system().parallelFor((size_t)bands, encodeBand, options);
            if (stopped || (request.cancelled && request.cancelled())){
                error = "cancelled";
                return false;
            }
            // transparent texels stay at alpha 0 (OpenEngine's p-bit repair)
            if (format == GpuTextureFormat::BC7 && channels == 4 && !opaque)
                RepairBC7TransparentEndpoints(image.pixels.data(), (uint32_t)image.width, (uint32_t)image.height, out);
            return true;
        }
    };

}

const ITextureEncoder& editor::bcTextureEncoder(){
    static const BcEncoder encoder;
    return encoder;
}
