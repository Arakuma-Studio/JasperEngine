// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureCooker.h"

#include "texture/Ktx2.h"
#include "texture/TextureData.h"
#include "texture/TextureDecode.h"
#include "texture/TextureFallback.h"

#include "htmlui/compat/Hash.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    double psnrOf(double mse){
        return mse <= 1e-12 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    }

    // a cube's RGBA8 copy at <= 256 px (what IBLEnvironment reads), one level, every face
    std::vector<uint8_t> makeCpuCopy(const PreparedTexture& prepared, std::string& error){
        int level = 0;
        while (level + 1 < prepared.levelCount() && std::max(prepared.faces[0][level].width, prepared.faces[0][level].height) > 256)
            level++;
        const MipLevel& sample = prepared.faces[0][level];
        std::vector<uint8_t> faces;
        for (const std::vector<MipLevel>& face : prepared.faces){
            const MipLevel& m = face[level];
            const size_t n = (size_t)m.width * (size_t)m.height;
            for (size_t i = 0; i < n; i++){
                uint8_t rgba[4] = {0, 0, 0, 255};
                switch (prepared.layout){
                    case PreparedTexture::Layout::RGBA8: std::memcpy(rgba, &m.pixels[i * 4], 4); break;
                    case PreparedTexture::Layout::RG8: rgba[0] = m.pixels[i * 2]; rgba[1] = m.pixels[i * 2 + 1]; rgba[2] = 255; break;
                    case PreparedTexture::Layout::R8: rgba[0] = rgba[1] = rgba[2] = m.pixels[i]; break;
                    case PreparedTexture::Layout::R16: rgba[0] = rgba[1] = rgba[2] = m.pixels[i * 2 + 1]; break;
                    case PreparedTexture::Layout::RGBAF: {
                        const float* v = reinterpret_cast<const float*>(m.pixels.data()) + i * 4;
                        for (int c = 0; c < 4; c++)
                            rgba[c] = (uint8_t)std::clamp((int)std::lround(v[c] * 255.0f), 0, 255);
                        break;
                    }
                }
                faces.insert(faces.end(), rgba, rgba + 4);
            }
        }
        Ktx2::WriteDesc desc;
        desc.format = GpuTextureFormat::RGBA8;
        desc.srgb = true;
        desc.width = (uint32_t)sample.width;
        desc.height = (uint32_t)sample.height;
        desc.faces = (uint32_t)prepared.faces.size();
        desc.levels.push_back(faces);
        desc.keyValues["JE.type"] = "default";
        desc.keyValues["JE.flags"] = "cpu,cube";
        std::vector<uint8_t> out;
        if (!Ktx2::write(desc, out, error, 9, zstdCompress))
            out.clear();
        return out;
    }

    // what the sources table remembers a step's analysis under (the key can be long)
    std::string stepVariant(const TextureCookStep& step){
        return htmlui::compat::sha256Hex(step.key);
    }

    // A step's output for these pixels, from its memo when the memo holds them
    TextureCookStep::Outcome runMemoizedStep(const TextureCookStep& step, const uint8_t* pixels, int width, int height,
                                             const std::string& sourceSha256){
        TextureCookStep::Memo& memo = *step.memo;
        std::lock_guard<std::mutex> lock(memo.mutex);
        if (!memo.filled || memo.key != step.key || memo.sourceSha256 != sourceSha256 || memo.width != width || memo.height != height){
            memo.outcome = step.run(pixels, width, height, sourceSha256);
            memo.filled = true;
            memo.key = step.key;
            memo.sourceSha256 = sourceSha256;
            memo.width = width;
            memo.height = height;
        }
        return memo.outcome;
    }

    // Runs a request's step on a decoded 2D source: `image` is replaced by the step's output when
    // it applies. `analysis` (when given) notes how it ended.
    void runCookStep(const TextureCookStep& step, TextureData& image, const std::string& sourceSha256, TextureSourceAnalysis* analysis,
                     std::vector<std::string>& warnings){
        std::vector<uint8_t> rgba;
        const uint8_t* pixels = nullptr;
        if (image.getColorFormat() == ColorFormat::RGBA && image.getChannels() == 4){
            pixels = (const uint8_t*)image.getData();
        }else if (image.toRgba8(rgba)){
            pixels = rgba.data();
        }
        TextureCookStep::Outcome outcome;
        if (!pixels){
            outcome.error = "its pixels cannot be read as RGBA8";
        }else if (step.run){
            outcome = step.memo ? runMemoizedStep(step, pixels, image.getWidth(), image.getHeight(), sourceSha256)
                                : step.run(pixels, image.getWidth(), image.getHeight(), sourceSha256);
        }
        if (outcome.applied){
            void* copy = malloc(outcome.rgba.size());
            std::memcpy(copy, outcome.rgba.data(), outcome.rgba.size());
            TextureData fixed(outcome.width, outcome.height, (unsigned int)outcome.rgba.size(), ColorFormat::RGBA, 4, copy);
            image.releaseImageData();
            image = fixed;
            image.setDataOwned(true);
        }
        if (analysis){
            analysis->stepState = outcome.applied ? "applied" : outcome.guard.empty() ? "failed" : "guarded";
            analysis->stepVerdict = outcome.verdict;
            analysis->stepGuard = outcome.applied ? std::string() : outcome.guard.empty() ? outcome.error : outcome.guard;
        }
        if (!outcome.applied){
            const std::string why = outcome.guard.empty() ? outcome.error : "needs a person (" + outcome.guard + ")";
            warnings.push_back("Pixel Art Cleanup did not run: " + why + ". The source is cooked as it is.");
        }
    }

}

std::string TextureCooker::flagsText(uint32_t flags){
    std::string text;
    auto add = [&](uint32_t flag, const char* name){
        if (flags & flag){
            if (!text.empty()) text += ",";
            text += name;
        }
    };
    add(TEXTURE_FLAG_ALPHA, "alpha");
    add(TEXTURE_FLAG_NORMAL_RG, "normalRG");
    add(TEXTURE_FLAG_COVERAGE, "coverage");
    add(TEXTURE_FLAG_DISPLAY_ENCODED, "displayEncoded");
    add(TEXTURE_FLAG_CPU_READABLE, "cpu");
    add(TEXTURE_FLAG_STREAMABLE, "streamable");
    add(TEXTURE_FLAG_CUBE, "cube");
    return text;
}

int TextureCooker::decodeSource(const fs::path& path, bool cube, std::array<TextureData, 6>& faces, std::string& error){
    TextureData image;
    if (!image.loadTextureFromFile(path.generic_string().c_str()) || !image.getData()){
        error = "cannot decode " + path.filename().generic_string();
        return 0;
    }
    // stb hands the pixels over: this object frees them
    image.setDataOwned(true);
    if (!cube){
        faces[0] = image;
        image.setDataOwned(false);
        faces[0].setDataOwned(true);
        return 1;
    }
    if (image.getColorFormat() == ColorFormat::RGBA32F || image.getColorFormat() == ColorFormat::RED16){
        // splitCubeMapCross copies 8-bit texels: wider ones go to RGBA8 first
        std::vector<uint8_t> rgba;
        if (!image.toRgba8(rgba)){
            error = "cannot read the cubemap's texels";
            return 0;
        }
        uint8_t* copy = (uint8_t*)malloc(rgba.size());
        std::memcpy(copy, rgba.data(), rgba.size());
        TextureData narrowed(image.getWidth(), image.getHeight(), (unsigned int)rgba.size(), ColorFormat::RGBA, 4, copy);
        image.releaseImageData();
        image = narrowed;
    }else{
        image.setDataOwned(false);
    }
    if (!TextureData::splitCubeMapCross(image, faces)){
        error = path.filename().generic_string() + " is not a 4:3 or 3:4 cubemap cross";
        return 0;
    }
    for (TextureData& face : faces)
        face.setDataOwned(true);
    return 6;
}

void TextureCooker::analyze(const std::array<TextureData, 6>& faces, int count, TextureSourceAnalysis& out){
    out = TextureSourceAnalysis();
    TextureData& first = const_cast<TextureData&>(faces[0]);
    out.width = first.getWidth();
    out.height = first.getHeight();
    out.faces = count;
    out.floatSource = first.getColorFormat() == ColorFormat::RGBA32F;
    out.sixteenBit = first.getColorFormat() == ColorFormat::RED16;
    out.singleChannel = first.getChannels() == 1;
    for (int f = 0; f < count; f++){
        TextureData& face = const_cast<TextureData&>(faces[f]);
        const size_t n = (size_t)face.getWidth() * (size_t)face.getHeight();
        if (face.getColorFormat() == ColorFormat::RGBA){
            const uint8_t* p = (const uint8_t*)face.getData();
            for (size_t i = 0; i < n && !(out.inputAlpha && out.grayAlpha); i++){
                out.inputAlpha = out.inputAlpha || p[i * 4 + 3] != 255;
                const int luma = (int)std::floor(0.2126f * p[i * 4] + 0.7152f * p[i * 4 + 1] + 0.0722f * p[i * 4 + 2] + 0.5f);
                out.grayAlpha = out.grayAlpha || luma < 255;
            }
        }else if (face.getColorFormat() == ColorFormat::RGBA32F){
            const float* p = (const float*)face.getData();
            for (size_t i = 0; i < n && !out.inputAlpha; i++)
                out.inputAlpha = p[i * 4 + 3] < 1.0f;
            out.grayAlpha = true;
        }else if (face.getColorFormat() == ColorFormat::RED){
            const uint8_t* p = (const uint8_t*)face.getData();
            for (size_t i = 0; i < n && !out.grayAlpha; i++)
                out.grayAlpha = p[i] < 255;
        }else{
            out.grayAlpha = true;
        }
    }
}

uint64_t TextureCooker::workingBytes(const TextureSourceAnalysis& analysis, const ResolvedTextureSettings& resolved){
    const uint64_t texels = (uint64_t)analysis.width * (uint64_t)analysis.height * (uint64_t)std::max(1, analysis.faces);
    const uint64_t decoded = analysis.floatSource ? 16 : 4;
    // the decode, the prepared levels (float for HDR and normal maps) and the encoder's copies
    const bool floatLevels = resolved.type == TextureImportType::HDR || resolved.type == TextureImportType::Normal ||
                             (resolved.type == TextureImportType::Lightmap && analysis.floatSource);
    return texels * (decoded + (floatLevels ? 16 : 4) * 2 + 8);
}

bool TextureCooker::plan(TextureCookCache& cache, const TextureCookRequest& request, TextureCookPlan& out, std::string& error){
    out = TextureCookPlan();
    out.resolved = request.settings.resolve(request.family, request.project);
    out.resolved.cpuReadable = request.cpuReadable || out.resolved.readable;
    out.resolved.cpuReason = request.cpuReadable ? request.cpuReason : (out.resolved.readable ? "Read/Write" : "");
    const bool cube = out.resolved.shape == TextureShape::Cube;
    // a step of a 2D source: its output's analysis is what gets selected for (S46 R33)
    const bool step = !request.step.key.empty() && !cube;
    if (!cache.sourceHash(request.source, out.sourceSha256, out.analysis, [&](TextureSourceAnalysis& analysis){
            std::array<TextureData, 6> faces;
            const int count = decodeSource(request.source, cube, faces, error);
            if (count == 0)
                return false;
            TextureSourceAnalysis stepResult;
            if (step){
                std::vector<std::string> ignored;
                runCookStep(request.step, faces[0], out.sourceSha256, &stepResult, ignored);
            }
            analyze(faces, count, analysis);
            analysis.stepState = stepResult.stepState;
            analysis.stepVerdict = stepResult.stepVerdict;
            analysis.stepGuard = stepResult.stepGuard;
            for (TextureData& face : faces)
                face.releaseImageData();
            return true;
        }, step ? stepVariant(request.step) : std::string())){
        if (error.empty())
            error = "cannot read " + request.assetPath;
        return false;
    }
    // the analysis was of another shape: decode again for this one
    if ((out.analysis.faces == 6) != cube){
        std::array<TextureData, 6> faces;
        const int count = decodeSource(request.source, cube, faces, error);
        if (count == 0)
            return false;
        // (cube was just requested: no step reaches here, a step's plan is never a cube's)
        analyze(faces, count, out.analysis);
        for (TextureData& face : faces)
            face.releaseImageData();
    }
    TextureProcessing processing = out.resolved.processing();
    int width = 0, height = 0;
    TexturePrepare::targetSize(out.analysis.width, out.analysis.height, processing, width, height);
    TextureFormatInputs inputs;
    inputs.family = request.family;
    inputs.settings = out.resolved;
    inputs.hasAlpha = out.analysis.hasAlpha(out.resolved.alphaSource);
    inputs.floatSource = out.analysis.floatSource;
    inputs.sixteenBitSource = out.analysis.sixteenBit;
    inputs.singleChannelSource = out.analysis.singleChannel;
    inputs.width = width;
    inputs.height = height;
    out.choice = TextureFormatSelector::select(inputs);
    if (request.settings.maxSize && std::max(out.analysis.width, out.analysis.height) > out.resolved.maxSize)
        out.choice.warnings.push_back(std::to_string(out.analysis.width) + "x" + std::to_string(out.analysis.height) +
                                      " is clamped to " + std::to_string(width) + "x" + std::to_string(height) + " by Max Size");
    if (step && out.analysis.stepState != "applied"){
        const std::string why = out.analysis.stepState == "guarded" ? "needs a person (" + out.analysis.stepGuard + ")" : out.analysis.stepGuard;
        out.choice.warnings.push_back("Pixel Art Cleanup did not run: " + why + ". The source is cooked as it is.");
    }
    out.key = TextureCookCache::key(out.sourceSha256, out.resolved, request.family, out.choice, step ? request.step.key : std::string());
    return true;
}

TextureCookResult TextureCooker::cook(TextureCookCache& cache, const TextureCookRequest& given, const std::function<bool()>& cancelled){
    TextureCookResult result;
    const auto start = std::chrono::steady_clock::now();
    // a step's plan and cook in one call run it once
    TextureCookRequest withMemo;
    if (!given.step.key.empty() && !given.step.memo){
        withMemo = given;
        withMemo.step.memo = std::make_shared<TextureCookStep::Memo>();
    }
    const TextureCookRequest& request = withMemo.step.memo ? withMemo : given;
    if (!plan(cache, request, result.plan, result.error))
        return result;
    const TextureCookPlan& plan = result.plan;
    if (cache.has(plan.key) && cache.load(plan.key, result.record)){
        result.ok = true;
        result.cacheHit = true;
        return result;
    }

    std::array<TextureData, 6> faces;
    const bool cube = plan.resolved.shape == TextureShape::Cube;
    const int count = decodeSource(request.source, cube, faces, result.error);
    if (count == 0)
        return result;
    if (!request.step.key.empty() && !cube){
        std::vector<std::string> warnings;
        runCookStep(request.step, faces[0], plan.sourceSha256, nullptr, warnings);
    }
    TextureProcessing processing = plan.resolved.processing();
    processing.sizeMultiple = plan.choice.sizeMultiple;
    std::vector<const TextureData*> pointers;
    for (int f = 0; f < count; f++)
        pointers.push_back(&faces[f]);
    PreparedTexture prepared;
    const bool preparedOk = TexturePrepare::prepare(pointers, processing, prepared, result.error);
    for (TextureData& face : faces)
        face.releaseImageData();
    if (!preparedOk)
        return result;
    if (cancelled && cancelled()){
        result.cancelled = true;
        result.error = "cancelled";
        return result;
    }

    uint32_t flags = 0;
    if (prepared.hasAlpha) flags |= TEXTURE_FLAG_ALPHA;
    if (prepared.layout == PreparedTexture::Layout::RG8) flags |= TEXTURE_FLAG_NORMAL_RG;
    if (plan.resolved.preserveCoverage) flags |= TEXTURE_FLAG_COVERAGE;
    if (prepared.layout == PreparedTexture::Layout::RGBAF) flags |= TEXTURE_FLAG_DISPLAY_ENCODED;
    if (plan.resolved.cpuReadable) flags |= TEXTURE_FLAG_CPU_READABLE;
    if (plan.resolved.streamable && !plan.resolved.cpuReadable && prepared.levelCount() > 1) flags |= TEXTURE_FLAG_STREAMABLE;
    if (cube) flags |= TEXTURE_FLAG_CUBE;

    TextureEncodeRequest encode;
    encode.prepared = &prepared;
    encode.choice = plan.choice;
    encode.effort = plan.resolved.effort;
    encode.type = plan.resolved.type;
    encode.cancelled = cancelled;
    encode.keyValues["KTXwriter"] = "JasperEngine texture cook " + std::to_string(kTextureCookVersion);
    encode.keyValues["JE.logical"] = std::to_string(prepared.logicalWidth) + "," + std::to_string(prepared.logicalHeight);
    encode.keyValues["JE.type"] = textureImportTypeName(plan.resolved.type);
    encode.keyValues["JE.flags"] = flagsText(flags);
    const ITextureEncoder& encoder = textureEncoderFor(plan.choice);
    std::vector<uint8_t> ktx2;
    if (!encoder.encode(encode, ktx2, result.error)){
        result.cancelled = cancelled && cancelled();
        return result;
    }
    std::vector<uint8_t> cpuCopy;
    const bool needsCpuCopy = cube && (gpuFormatInfo(plan.choice.format).compressed || plan.choice.codec != TextureCodec::Raw ||
                                       prepared.layout != PreparedTexture::Layout::RGBA8);
    if (needsCpuCopy){
        cpuCopy = makeCpuCopy(prepared, result.error);
        if (cpuCopy.empty())
            return result;
    }

    TextureCookRecord& record = result.record;
    record.key = plan.key;
    record.family = textureFamilyName(request.family);
    record.format = plan.choice.codec == TextureCodec::Raw ? gpuFormatName(plan.choice.format) : textureCodecName(plan.choice.codec);
    record.label = plan.choice.label();
    record.width = prepared.width;
    record.height = prepared.height;
    record.levels = prepared.levelCount();
    record.faces = (int)prepared.faces.size();
    record.logicalWidth = prepared.logicalWidth;
    record.logicalHeight = prepared.logicalHeight;
    record.gpuBytes = gpuBytes(ktx2);
    record.diskBytes = ktx2.size();
    record.sourceBytes = plan.analysis.bytes;
    record.tailBytes = tailBytes(ktx2);
    record.cpuBytes = cpuCopy.size();
    record.flags = flags;
    record.warnings = plan.choice.warnings;
    record.encoders.push_back(encoder.version() + (encoder.isaInvariant() ? std::string() : std::string(" (") + textureEncoderIsa() + ")"));
    measure(ktx2, prepared, record);
    record.cookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!cache.store(plan.key, ktx2, cpuCopy.empty() ? nullptr : &cpuCopy, record, result.error))
        return result;
    result.ok = true;
    return result;
}

uint64_t TextureCooker::gpuBytes(const std::vector<uint8_t>& ktx2){
    Ktx2Info info;
    std::string error;
    if (!Ktx2::parseInfo(ktx2, ktx2.size(), info, error))
        return 0;
    GpuTextureFormat format = info.format;
    if (info.codec != Ktx2Codec::Raw){
        // a desktop browser: BC, no ETC2 or ASTC
        TextureFormatSupport desktop;
        for (size_t f = 0; f < kGpuTextureFormatCount; f++){
            const GpuTextureFormat candidate = (GpuTextureFormat)f;
            desktop.set(candidate, candidate < GpuTextureFormat::ETC2_RGB8);
        }
        format = TextureFallback::plan(info, desktop, false).upload;
    }
    uint64_t total = 0;
    for (uint32_t level = 0; level < info.levelCount; level++)
        total += (uint64_t)gpuFormatLevelBytes(format, (int)info.levelWidth(level), (int)info.levelHeight(level)) * info.faces;
    return total;
}

uint64_t TextureCooker::tailBytes(const std::vector<uint8_t>& ktx2){
    Ktx2Info info;
    std::string error;
    if (!Ktx2::parseInfo(ktx2, ktx2.size(), info, error))
        return 0;
    uint64_t end = 0;
    for (uint32_t level = 0; level < info.levelCount; level++){
        if (std::max(info.levelWidth(level), info.levelHeight(level)) <= 64)
            end = std::max(end, info.levels[level].byteOffset + info.levels[level].byteLength);
    }
    // the whole file is the tail: no second read
    return end >= ktx2.size() ? 0 : end;
}

void TextureCooker::measure(const std::vector<uint8_t>& ktx2, const PreparedTexture& prepared, TextureCookRecord& record){
    GpuTextureFormat format;
    int width = 0, height = 0, faces = 0;
    std::vector<uint8_t> decoded;
    std::string error;
    if (!decodeKtx2ForPreview(ktx2, 0, format, width, height, faces, decoded, error) || width != prepared.width || height != prepared.height)
        return;
    const size_t faceTexels = (size_t)width * (size_t)height;
    double squared = 0.0, squaredAlpha = 0.0;
    size_t samples = 0;
    std::vector<double> angles;
    double hdrError = 0.0, hdrReference = 0.0;
    for (size_t f = 0; f < prepared.faces.size() && f < (size_t)faces; f++){
        const MipLevel& reference = prepared.faces[f][0];
        for (size_t i = 0; i < faceTexels; i++){
            const size_t at = f * faceTexels + i;
            switch (prepared.layout){
                case PreparedTexture::Layout::RGBA8: {
                    if (format != GpuTextureFormat::RGBA8) return;
                    for (int c = 0; c < 3; c++){
                        const double d = (double)decoded[at * 4 + c] - reference.pixels[i * 4 + c];
                        squared += d * d;
                    }
                    const double a = (double)decoded[at * 4 + 3] - reference.pixels[i * 4 + 3];
                    squaredAlpha += a * a;
                    samples += 3;
                    break;
                }
                case PreparedTexture::Layout::R8: {
                    const size_t stride = format == GpuTextureFormat::R8 ? 1 : format == GpuTextureFormat::RG8 ? 2 : 4;
                    const double d = (double)decoded[at * stride] - reference.pixels[i];
                    squared += d * d;
                    samples += 1;
                    break;
                }
                case PreparedTexture::Layout::RG8: {
                    const size_t stride = format == GpuTextureFormat::RG8 ? 2 : 4;
                    auto unit = [](double x, double y, double out[3]){
                        x = x / 255.0 * 2.0 - 1.0;
                        y = y / 255.0 * 2.0 - 1.0;
                        out[0] = x; out[1] = y; out[2] = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
                        const double length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
                        for (int c = 0; c < 3; c++) out[c] /= length;
                    };
                    double a[3], b[3];
                    unit(decoded[at * stride], decoded[at * stride + 1], a);
                    unit(reference.pixels[i * 2], reference.pixels[i * 2 + 1], b);
                    const double dot = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0);
                    angles.push_back(std::acos(dot) * 180.0 / 3.14159265358979323846);
                    for (int c = 0; c < 2; c++){
                        const double d = (double)decoded[at * stride + c] - reference.pixels[i * 2 + c];
                        squared += d * d;
                    }
                    samples += 2;
                    break;
                }
                case PreparedTexture::Layout::RGBAF: {
                    if (format != GpuTextureFormat::RGBA16F) return;
                    const float* r = reinterpret_cast<const float*>(reference.pixels.data()) + i * 4;
                    for (int c = 0; c < 3; c++){
                        uint16_t half;
                        std::memcpy(&half, &decoded[(at * 4 + c) * 2], 2);
                        // compared in linear light, where HDR error matters, on [0, 16] (AC4)
                        const double value = std::min(16.0, std::pow((double)std::max(0.0f, halfToFloat(half)), 2.2));
                        const double expected = std::min(16.0, std::pow((double)std::max(0.0f, r[c]), 2.2));
                        hdrError += (value - expected) * (value - expected);
                        hdrReference += expected * expected;
                    }
                    break;
                }
                case PreparedTexture::Layout::R16: {
                    if (format != GpuTextureFormat::R16) return;
                    uint16_t value, expected;
                    std::memcpy(&value, &decoded[at * 2], 2);
                    std::memcpy(&expected, &reference.pixels[i * 2], 2);
                    const double d = ((double)value - expected) / 257.0;
                    squared += d * d;
                    samples += 1;
                    break;
                }
            }
        }
    }
    if (samples > 0)
        record.psnr = psnrOf(squared / (double)samples);
    if (prepared.layout == PreparedTexture::Layout::RGBA8 && prepared.hasAlpha && samples > 0)
        record.psnrAlpha = psnrOf(squaredAlpha / (double)(samples / 3));
    if (!angles.empty()){
        double sum = 0.0;
        for (double angle : angles) sum += angle;
        record.normalMeanError = sum / (double)angles.size();
        std::sort(angles.begin(), angles.end());
        record.normalP99Error = angles[std::min(angles.size() - 1, (size_t)std::floor(0.99 * (double)angles.size()))];
    }
    if (hdrReference > 0.0)
        record.hdrRelativeRmse = std::sqrt(hdrError / hdrReference);
}
