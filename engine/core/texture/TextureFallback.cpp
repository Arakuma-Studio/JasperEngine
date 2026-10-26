// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureFallback.h"

#include "TextureDecode.h"

#include "Log.h"

#include <cstring>
#include <mutex>

using namespace jasperengine;

namespace {
    void appendLastResort(std::vector<GpuTextureFormat>& list, TextureContent content, bool lowMemoryFallback){
        switch (content){
            case TextureContent::Normal: list.push_back(GpuTextureFormat::RG8); return;
            case TextureContent::Single: list.push_back(GpuTextureFormat::R8); return;
            case TextureContent::Hdr: list.push_back(GpuTextureFormat::RGBA16F); return;
            case TextureContent::ColorOpaque:
                if (lowMemoryFallback)
                    list.push_back(GpuTextureFormat::RGB565);
                break;
            case TextureContent::ColorAlpha:
                if (lowMemoryFallback)
                    list.push_back(GpuTextureFormat::RGBA4);
                break;
        }
        list.push_back(GpuTextureFormat::RGBA8);
    }

    std::string unsupportedReason(GpuTextureFormat format){
        return std::string(gpuFormatName(format)) + " is not supported by this GPU";
    }
}

bool TextureFallback::hasFlag(const Ktx2Info& info, const char* flag){
    auto it = info.keyValues.find("JE.flags");
    if (it == info.keyValues.end())
        return false;
    const std::string& flags = it->second;
    const size_t length = std::strlen(flag);
    size_t at = 0;
    while (at <= flags.size()){
        size_t end = flags.find(',', at);
        if (end == std::string::npos)
            end = flags.size();
        if (end - at == length && flags.compare(at, length, flag) == 0)
            return true;
        at = end + 1;
    }
    return false;
}

TextureContent TextureFallback::contentOf(const Ktx2Info& info){
    if (info.codec == Ktx2Codec::UASTC_HDR || (info.codec == Ktx2Codec::Raw && gpuFormatInfo(info.format).hdr))
        return TextureContent::Hdr;
    if (hasFlag(info, "normalRG"))
        return TextureContent::Normal;
    auto type = info.keyValues.find("JE.type");
    if (type != info.keyValues.end() && type->second == "single_channel")
        return TextureContent::Single;
    if (info.codec == Ktx2Codec::Raw){
        const uint8_t channels = gpuFormatInfo(info.format).channels;
        if (channels == 1)
            return TextureContent::Single;
        if (channels == 2)
            return TextureContent::Normal;
        if (!gpuFormatInfo(info.format).alpha)
            return TextureContent::ColorOpaque;
    }
    // without the cook's flag a Basis file is taken as having alpha (the safe transcode)
    if (type == info.keyValues.end() && info.keyValues.find("JE.flags") == info.keyValues.end())
        return TextureContent::ColorAlpha;
    return hasFlag(info, "alpha") ? TextureContent::ColorAlpha : TextureContent::ColorOpaque;
}

std::vector<GpuTextureFormat> TextureFallback::transcodeCandidates(Ktx2Codec codec, TextureContent content, bool lowMemoryFallback){
    using F = GpuTextureFormat;
    std::vector<F> list;
    switch (content){
        case TextureContent::Normal: list = {F::BC5, F::EAC_RG11, F::ASTC_4x4}; break;
        case TextureContent::Single: list = {F::BC4, F::EAC_R11, F::ASTC_4x4}; break;
        case TextureContent::Hdr: list = {F::BC6H, F::RGB9E5}; break;
        case TextureContent::ColorOpaque:
            if (codec == Ktx2Codec::ETC1S)
                list = {F::ETC2_RGB8, F::BC1, F::ASTC_4x4, F::BC7};
            else
                list = {F::ASTC_4x4, F::BC7, F::ETC2_RGB8, F::BC1};
            break;
        case TextureContent::ColorAlpha:
            if (codec == Ktx2Codec::ETC1S)
                list = {F::ETC2_RGBA8, F::BC3, F::ASTC_4x4, F::BC7};
            else
                list = {F::ASTC_4x4, F::BC7, F::ETC2_RGBA8, F::BC3};
            break;
    }
    appendLastResort(list, content, lowMemoryFallback);
    return list;
}

TextureUploadPlan TextureFallback::planRaw(GpuTextureFormat stored, TextureContent content, const TextureFormatSupport& support,
                                           bool lowMemoryFallback){
    TextureUploadPlan plan;
    plan.upload = stored;
    plan.decoded = stored;
    if (support(stored)){
        plan.path = TextureUploadPlan::Path::Native;
        return plan;
    }
    if (!TextureDecode::canDecode(stored)){
        plan.path = TextureUploadPlan::Path::Unavailable;
        plan.reason = unsupportedReason(stored) + " and cannot be decoded on the CPU";
        return plan;
    }
    plan.path = TextureUploadPlan::Path::CpuDecode;
    plan.decoded = TextureDecode::decodedFormat(stored);
    plan.upload = plan.decoded;
    if (plan.decoded == GpuTextureFormat::RGBA16F && !support(GpuTextureFormat::RGBA16F))
        plan.upload = GpuTextureFormat::RGBA8;
    if (plan.upload == GpuTextureFormat::RGBA8 && lowMemoryFallback &&
        (content == TextureContent::ColorOpaque || content == TextureContent::ColorAlpha)){
        const GpuTextureFormat small = content == TextureContent::ColorOpaque ? GpuTextureFormat::RGB565 : GpuTextureFormat::RGBA4;
        if (support(small))
            plan.upload = small;
    }
    plan.reason = unsupportedReason(stored) + ", decoded to " + gpuFormatName(plan.upload) + " on the CPU";
    return plan;
}

TextureUploadPlan TextureFallback::plan(const Ktx2Info& info, const TextureFormatSupport& support, bool lowMemoryFallback){
    const TextureContent content = contentOf(info);
    if (info.codec == Ktx2Codec::Raw)
        return planRaw(info.format, content, support, lowMemoryFallback);

    TextureUploadPlan plan;
    plan.path = TextureUploadPlan::Path::Transcode;
    const std::vector<GpuTextureFormat> candidates = transcodeCandidates(info.codec, content, lowMemoryFallback);
    for (GpuTextureFormat candidate : candidates){
        // R8 and RG8 come from an RGBA transcode on the CPU: every device samples them
        const bool cpuSide = candidate == GpuTextureFormat::R8 || candidate == GpuTextureFormat::RG8;
        if (cpuSide || support(candidate)){
            plan.upload = candidate;
            plan.decoded = candidate;
            if (candidate != candidates.front())
                plan.reason = unsupportedReason(candidates.front()) + ", transcoded to " + gpuFormatName(candidate);
            return plan;
        }
    }
    plan.path = TextureUploadPlan::Path::Unavailable;
    plan.reason = "no format of the transcode list is supported by this GPU";
    return plan;
}

namespace {
    std::mutex& logOnceMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }
    bool loggedFallbacks[kGpuTextureFormatCount + 1] = {};
}

void TextureFallback::logOnce(GpuTextureFormat stored, bool basis, const std::string& reason){
    std::lock_guard<std::mutex> lock(logOnceMutex());
    bool& done = loggedFallbacks[basis ? kGpuTextureFormatCount : (size_t)stored];
    if (!done){
        done = true;
        Log::print("Texture fallback: %s", reason.c_str());
    }
}

void TextureFallback::resetLogOnce(){
    std::lock_guard<std::mutex> lock(logOnceMutex());
    for (bool& done : loggedFallbacks)
        done = false;
}
