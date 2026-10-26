// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ASTC 4x4-12x12 (astcenc) of the mobile family: the invariant build, so the bytes are the same
// on every CPU (D11) and do not depend on how many jobs share an image.

#include "TextureEncoderParts.h"

#include "texture/Ktx2.h"
#include "jobs/JobSystem.h"

#include "astcenc.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    float astcQuality(TextureEffort effort){
        switch (effort){
            case TextureEffort::Fast: return ASTCENC_PRE_FAST;
            case TextureEffort::Best: return ASTCENC_PRE_THOROUGH;
            case TextureEffort::Normal: default: return ASTCENC_PRE_MEDIUM;
        }
    }

    struct ContextDeleter {
        void operator()(astcenc_context* context) const { astcenc_context_free(context); }
    };
    using Context = std::unique_ptr<astcenc_context, ContextDeleter>;

    // the 8-bit level as the RGBA texels astcenc reads
    std::vector<uint8_t> toRgba(const MipLevel& level, int channels){
        const size_t texels = (size_t)level.width * (size_t)level.height;
        std::vector<uint8_t> rgba(texels * 4);
        for (size_t i = 0; i < texels; i++){
            uint8_t* o = &rgba[i * 4];
            const uint8_t* p = &level.pixels[i * channels];
            switch (channels){
                case 1: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
                case 2: o[0] = p[0]; o[1] = p[1]; o[2] = 0; o[3] = 255; break;
                default: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;
            }
        }
        return rgba;
    }

    class AstcEncoder final : public ITextureEncoder {
    public:
        const char* name() const override { return "astcenc"; }
        std::string version() const override { return "astcenc 5.7.0 invariant"; }
        bool isaInvariant() const override { return true; }

        bool encode(const TextureEncodeRequest& request, std::vector<uint8_t>& ktx2, std::string& error) const override{
            const PreparedTexture& prepared = *request.prepared;
            const GpuTextureFormat format = request.choice.format;
            const GpuFormatInfo& info = gpuFormatInfo(format);
            if (prepared.layout == PreparedTexture::Layout::RGBAF || prepared.layout == PreparedTexture::Layout::R16){
                error = "astcenc encodes LDR 8-bit levels";
                return false;
            }
            const int channels = (int)prepared.texelBytes();
            const bool normal = prepared.layout == PreparedTexture::Layout::RG8;
            const bool single = prepared.layout == PreparedTexture::Layout::R8;

            unsigned int flags = 0;
            if (prepared.layout == PreparedTexture::Layout::RGBA8 && prepared.hasAlpha)
                flags |= ASTCENC_FLG_USE_ALPHA_WEIGHT;
            astcenc_config config;
            // the linear LDR profile even for sRGB colour: the engine samples UNORM and decodes sRGB
            // in the shader (D7), and ASTC's sRGB decode mode expands endpoints differently
            const astcenc_profile profile = ASTCENC_PRF_LDR;
            if (astcenc_config_init(profile, info.blockDim, info.blockDim, 1, astcQuality(request.effort), flags, &config) != ASTCENC_SUCCESS){
                error = "astcenc rejected the configuration";
                return false;
            }
            // normal XY (D8: RG everywhere) and single channels: the other channels carry nothing
            if (normal){
                config.cw_b_weight = 0.0f;
                config.cw_a_weight = 0.0f;
            }else if (single){
                config.cw_g_weight = 0.0f;
                config.cw_b_weight = 0.0f;
                config.cw_a_weight = 0.0f;
            }else if (!prepared.hasAlpha){
                config.cw_a_weight = 0.0f;
            }
            const unsigned int threads = (unsigned int)std::max(1, jobs::system().getWorkerCount() + 1);
            astcenc_context* raw = nullptr;
            if (astcenc_context_alloc(&config, threads, &raw, nullptr) != ASTCENC_SUCCESS){
                error = "astcenc could not allocate a context";
                return false;
            }
            Context context(raw);
            astcenc_swizzle swizzle;
            if (normal)
                swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_0, ASTCENC_SWZ_1};
            else if (single)
                swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_1};
            else if (!prepared.hasAlpha)
                swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_1};
            else
                swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

            std::vector<std::vector<uint8_t>> levels(prepared.levelCount());
            for (int level = 0; level < prepared.levelCount(); level++){
                for (const std::vector<MipLevel>& face : prepared.faces){
                    const MipLevel& image = face[level];
                    std::vector<uint8_t> rgba = toRgba(image, channels);
                    void* slices[1] = {rgba.data()};
                    astcenc_image input{(unsigned int)image.width, (unsigned int)image.height, 1, ASTCENC_TYPE_U8, slices};
                    std::vector<uint8_t> blocks(gpuFormatLevelBytes(format, image.width, image.height));
                    std::atomic<int> failures{0};
                    // every job takes blocks from astcenc's shared queue until none is left
                    auto work = [&](size_t thread) -> bool {
                        if (request.cancelled && request.cancelled()){
                            astcenc_compress_cancel(context.get());
                            return false;
                        }
                        if (astcenc_compress_image(context.get(), &input, &swizzle, blocks.data(), blocks.size(), (unsigned int)thread) != ASTCENC_SUCCESS)
                            failures++;
                        return true;
                    };
                    jobs::ParallelForOptions options;
                    options.lane = jobs::Lane::Background;
                    jobs::system().parallelFor(threads, work, options);
                    astcenc_compress_reset(context.get());
                    if (request.cancelled && request.cancelled()){
                        error = "cancelled";
                        return false;
                    }
                    if (failures > 0){
                        error = "astcenc failed to compress a level";
                        return false;
                    }
                    levels[level].insert(levels[level].end(), blocks.begin(), blocks.end());
                }
            }
            return writeRawKtx2(request, format, levels, ktx2, error);
        }
    };

}

const ITextureEncoder& editor::astcTextureEncoder(){
    static const AstcEncoder encoder;
    return encoder;
}

bool editor::decodeAstcLevel(GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size, std::vector<uint8_t>& out){
    const GpuFormatInfo& info = gpuFormatInfo(format);
    if (format < GpuTextureFormat::ASTC_4x4 || size < gpuFormatLevelBytes(format, width, height))
        return false;
    // one context per block size, decode only; LDR (the engine samples UNORM, D7)
    static std::mutex mutex;
    static astcenc_context* contexts[kGpuTextureFormatCount] = {};
    std::lock_guard<std::mutex> lock(mutex);
    astcenc_context*& context = contexts[(size_t)format];
    if (!context){
        astcenc_config config;
        if (astcenc_config_init(ASTCENC_PRF_LDR, info.blockDim, info.blockDim, 1, ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &config) != ASTCENC_SUCCESS ||
            astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS)
            return false;
    }
    out.resize((size_t)width * (size_t)height * 4);
    void* slices[1] = {out.data()};
    astcenc_image image{(unsigned int)width, (unsigned int)height, 1, ASTCENC_TYPE_U8, slices};
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    const bool ok = astcenc_decompress_image(context, data, size, &image, &swizzle, 0) == ASTCENC_SUCCESS;
    astcenc_decompress_reset(context);
    return ok;
}
