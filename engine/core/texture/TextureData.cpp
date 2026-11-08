// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureData.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#include "stb_image.h"
#include "util/LunaSVGRuntime.h"
#include "Log.h"
#include "Texture.h"
#include "texture/BasisTranscode.h"
#include "texture/CookedTextures.h"
#include "texture/Ktx2.h"
#include "texture/MipChain.h"
#include "texture/TextureDecode.h"
#include "texture/TextureFallback.h"
#include "texture/TexturePrepare.h"

#include <map>
#include <mutex>

using namespace jasperengine;

bool TextureData::hasSvgExtension(const char* filename) {
    if (!filename) {
        return false;
    }

    std::string path(filename);
    std::string extension;
    const size_t dotPos = path.find_last_of('.');
    if (dotPos != std::string::npos) {
        extension = path.substr(dotPos);
    }
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".svg";
}

static const std::string SVG_SCALE_MARKER = "?svgScale=";

std::string TextureData::parseSvgScalePath(const std::string& path, float* outScale) {
    const size_t pos = path.find(SVG_SCALE_MARKER);
    if (pos == std::string::npos) {
        return path;
    }

    if (outScale) {
        try {
            *outScale = std::stof(path.substr(pos + SVG_SCALE_MARKER.size()));
        } catch (...) {
            // Malformed suffix: leave the caller's scale untouched.
        }
    }
    return path.substr(0, pos);
}

std::string TextureData::buildSvgScalePath(const std::string& cleanPath, float scale) {
    // No suffix for the default scale (or invalid values): keep the path clean.
    if (!(scale > 0.0f) || std::fabs(scale - 1.0f) < 1e-4f) {
        return cleanPath;
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%g", scale);
    return cleanPath + SVG_SCALE_MARKER + buffer;
}

bool TextureData::looksLikeSvg(Data* filedata) {
    if (!filedata || !filedata->getMemPtr() || filedata->length() == 0) {
        return false;
    }

    const unsigned int inspectLength = std::min<unsigned int>(filedata->length(), 1024);
    unsigned char* mem = filedata->getMemPtr();

    // Skip an optional UTF-8 BOM so it doesn't hide the first real character.
    unsigned int start = 0;
    if (inspectLength >= 3 && mem[0] == 0xEF && mem[1] == 0xBB && mem[2] == 0xBF) {
        start = 3;
    }
    // Skip leading whitespace.
    while (start < inspectLength && std::isspace(mem[start])) {
        start++;
    }

    // A valid SVG/XML document always begins with '<' (e.g. "<?xml", "<!--",
    // "<!DOCTYPE", or "<svg"). Requiring this avoids misrouting a binary raster
    // image to the SVG decoder just because the bytes "<svg" appear in its metadata.
    if (start >= inspectLength || mem[start] != '<') {
        return false;
    }

    std::string header;
    header.reserve(inspectLength - start);
    for (unsigned int i = start; i < inspectLength; i++) {
        header.push_back(static_cast<char>(std::tolower(mem[i])));
    }

    return header.find("<svg") != std::string::npos;
}

TextureData::TextureData() {
    this->width = 0;
    this->height = 0;
    this->originalWidth = 0;
    this->originalHeight = 0;
    this->size = 0;
    this->color_format = ColorFormat::RGBA;
    this->channels = 0;
    this->data = NULL;

    this->transparent = false;

    this->dataOwned = false;

    this->svgScale = 1.0f;

    this->gpuFormat = GpuTextureFormat::RGBA8;
    this->srgb = true;
    this->faceCount = 1;
    this->flags = 0;
    this->droppedLevels = 0;
    this->fullLevelCount = 1;
}

TextureData::TextureData(int width, int height, unsigned int size, ColorFormat color_format, int channels, void* data){
    this->width = width;
    this->height = height;
    this->originalWidth = width;
    this->originalHeight = height;
    this->size = size;
    this->color_format = color_format;
    this->channels = channels;
    this->data = data;

    this->transparent = false;

    this->dataOwned = false;

    this->svgScale = 1.0f;

    this->gpuFormat = color_format == ColorFormat::RED ? GpuTextureFormat::R8
                    : color_format == ColorFormat::RED16 ? GpuTextureFormat::R16 : GpuTextureFormat::RGBA8;
    this->srgb = color_format == ColorFormat::RGBA;
    this->faceCount = 1;
    this->flags = 0;
    this->droppedLevels = 0;
    this->fullLevelCount = 1;
}

TextureData::TextureData(const char* filename) : TextureData(){
    loadTextureFromFile(filename);
}

TextureData::TextureData(unsigned char* data, unsigned int dataLength) : TextureData(){
    loadTextureFromMemory(data, dataLength);
}

TextureData::TextureData(const TextureData& v){
    this->copy(v);
}

TextureData& TextureData::operator = ( const TextureData& v ){
    this->copy(v);

    return *this;
}

bool TextureData::operator == ( const TextureData& v ) const{
    return (
        v.width == width &&
        v.height == height &&
        v.originalWidth == originalWidth &&
        v.originalHeight == originalHeight &&
        v.size == size &&
        v.color_format == color_format &&
        v.channels == channels &&
        v.data == data &&
        v.transparent == transparent &&
        v.dataOwned == dataOwned &&
        v.levelStorage == levelStorage &&
        v.gpuFormat == gpuFormat &&
        v.droppedLevels == droppedLevels
    );
}

bool TextureData::operator != ( const TextureData& v ) const{
    return !(*this == v);
}

bool TextureData::loadTexture(Data* filedata) {
    filedata->seek(0);

    if (dataOwned && data)
        releaseImageData();
    levels.clear();
    levelStorage.reset();
    cpuCopy.reset();
    flags = 0;
    droppedLevels = 0;
    fullLevelCount = 1;

    if (Ktx2::isKtx2(std::span<const uint8_t>(filedata->getMemPtr(), filedata->length()))) {
        // CPU readers want pixels: compressed levels are decoded (level 0)
        std::string error;
        if (!loadKtx2Bytes(filedata->getMemPtr(), filedata->length(), true, 0, nullptr, false, &error)) {
            Log::error("Error loading KTX2 texture: %s", error.c_str());
            return false;
        }
        return true;
    }

    if (stbi_is_hdr_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length())) {
        // HDR (.hdr) stays float: the importer's hdr type cooks it to BC6H / RGB9E5 / RGBA16F
        float* pixels = stbi_loadf_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length(), &width, &height, &channels, 4);
        if (!pixels){
            Log::error("Error loading texture: %s", stbi_failure_reason());
            return false;
        }
        data = pixels;
        channels = 4;
        color_format = ColorFormat::RGBA32F;
        size = (unsigned int)((size_t)width * (size_t)height * 4 * sizeof(float));
        originalWidth = width;
        originalHeight = height;
        gpuFormat = GpuTextureFormat::RGBA16F;
        srgb = false;
        transparent = hasAlpha();
        return true;
    }

    if (looksLikeSvg(filedata)) {
        if (!loadSvgTexture(filedata)) {
            Log::error("Error loading SVG texture");
            return false;
        }
        return true;
    }
    
    //----- Start std_image read texture
    stbi_info_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length(), &width, &height, &channels);

    //Renders (not GL) only support 8bpp and 32bpp
    int desired_channels = 1;
    color_format = ColorFormat::RED;

    if (channels != 1){
        desired_channels = 4;
        color_format = ColorFormat::RGBA;
    }

    // Single-channel 16-bit images (e.g. terrain heightmaps) are kept at full precision
    // as RED16. Multi-channel images are still downconverted to 8-bit RGBA.
    const bool is16Bit = desired_channels == 1 &&
                         stbi_is_16_bit_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length());

    if (is16Bit){
        color_format = ColorFormat::RED16;
        data = stbi_load_16_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length(), &width, &height, &channels, desired_channels);
    }else{
        data = stbi_load_from_memory((stbi_uc const *)filedata->getMemPtr(), filedata->length(), &width, &height, &channels, desired_channels);
    }

    if (!data){
        Log::error("Error loading texture: %s", stbi_failure_reason());
        return false;
    }

    channels = desired_channels;

    size = width * height * channels * (is16Bit ? 2 : 1); //in bytes
    //----- End std_image read texture

    originalWidth = width;
    originalHeight = height;
    gpuFormat = color_format == ColorFormat::RED ? GpuTextureFormat::R8
              : color_format == ColorFormat::RED16 ? GpuTextureFormat::R16 : GpuTextureFormat::RGBA8;
    srgb = color_format == ColorFormat::RGBA;

    transparent = hasAlpha();
    
    return true;
}

namespace {
    std::mutex& floatDecoderMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    std::map<std::string, TextureData::FloatDecoder>& floatDecoders(){
        static auto* decoders = new std::map<std::string, TextureData::FloatDecoder>();
        return *decoders;
    }

    std::string textureLowerExtension(const std::string& path){
        const size_t dot = path.find_last_of('.');
        std::string extension = dot == std::string::npos ? std::string() : path.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension;
    }

    TextureData::FloatDecoder floatDecoderFor(const std::string& path){
        std::lock_guard<std::mutex> lock(floatDecoderMutex());
        auto it = floatDecoders().find(textureLowerExtension(path));
        return it == floatDecoders().end() ? nullptr : it->second;
    }

    uint32_t flagsFromKeyValues(const Ktx2Info& info){
        uint32_t flags = 0;
        if (TextureFallback::hasFlag(info, "alpha")) flags |= TEXTURE_FLAG_ALPHA;
        if (TextureFallback::hasFlag(info, "normalRG")) flags |= TEXTURE_FLAG_NORMAL_RG;
        if (TextureFallback::hasFlag(info, "coverage")) flags |= TEXTURE_FLAG_COVERAGE;
        if (TextureFallback::hasFlag(info, "displayEncoded")) flags |= TEXTURE_FLAG_DISPLAY_ENCODED;
        if (TextureFallback::hasFlag(info, "cpu")) flags |= TEXTURE_FLAG_CPU_READABLE;
        if (TextureFallback::hasFlag(info, "streamable")) flags |= TEXTURE_FLAG_STREAMABLE;
        if (info.faces == 6) flags |= TEXTURE_FLAG_CUBE;
        return flags;
    }

}

void TextureData::registerFloatDecoder(const std::string& extension, FloatDecoder decoder){
    std::lock_guard<std::mutex> lock(floatDecoderMutex());
    floatDecoders()[extension] = decoder;
}

bool TextureData::hasFloatDecoder(const std::string& extension){
    std::lock_guard<std::mutex> lock(floatDecoderMutex());
    return floatDecoders().count(extension) > 0;
}

bool TextureData::loadKtx2(const unsigned char* bytes, unsigned int length, uint32_t firstLevel,
                           const TextureFormatSupport* support, bool lowMemoryFallback, std::string* error){
    if (dataOwned && data)
        releaseImageData();
    return loadKtx2Bytes(bytes, length, false, firstLevel, support, lowMemoryFallback, error);
}

bool TextureData::loadKtx2Bytes(const unsigned char* bytes, unsigned int length, bool decodeToPixels, uint32_t firstLevel,
                                const TextureFormatSupport* support, bool lowMemoryFallback, std::string* error){
    std::string message;
    std::string& err = error ? *error : message;
    Ktx2MemorySource source(std::span<const uint8_t>(bytes, length));
    Ktx2Image image;
    if (!Ktx2::read(source, firstLevel, image, err))
        return false;
    return loadKtx2Image(image, decodeToPixels, support, lowMemoryFallback, err);
}

bool TextureData::loadKtx2Image(Ktx2Image& image, bool decodeToPixels, const TextureFormatSupport* support,
                                bool lowMemoryFallback, std::string& err){
    if (dataOwned && data && !levelStorage)
        releaseImageData();
    const Ktx2Info& info = image.info;

    int logicalWidth = (int)info.width;
    int logicalHeight = (int)info.height;
    auto logical = info.keyValues.find("JE.logical");
    if (logical != info.keyValues.end()){
        int w = 0, h = 0;
        if (sscanf(logical->second.c_str(), "%d,%d", &w, &h) == 2 && w > 0 && h > 0){
            logicalWidth = w;
            logicalHeight = h;
        }
    }
    const int levelWidth = (int)info.levelWidth(image.firstLevel);
    const int levelHeight = (int)info.levelHeight(image.firstLevel);
    const int faces = (int)info.faces;
    const int firstLevel = (int)image.firstLevel;
    const int fullLevels = (int)info.levelCount;
    const TextureContent content = TextureFallback::contentOf(info);

    GpuTextureFormat format = info.format;
    bool isSrgb = info.srgb;
    std::vector<std::vector<uint8_t>> out;

    if (info.codec != Ktx2Codec::Raw){
        // Basis: transcoded here, on the loader, to what the device samples (RGBA8 for CPU readers)
        TextureUploadPlan plan;
        if (decodeToPixels || !support){
            plan.path = TextureUploadPlan::Path::Transcode;
            plan.upload = content == TextureContent::Hdr ? GpuTextureFormat::RGBA16F
                        : content == TextureContent::Single ? GpuTextureFormat::R8 : GpuTextureFormat::RGBA8;
        }else{
            plan = TextureFallback::plan(info, *support, lowMemoryFallback);
        }
        if (plan.path == TextureUploadPlan::Path::Unavailable){
            err = plan.reason;
            return false;
        }
        if (!plan.reason.empty())
            TextureFallback::logOnce(format, true, plan.reason);
        // the session takes the levels (the file's first level and count are kept above)
        BasisTranscode::Session session;
        if (!session.open(std::move(image), err))
            return false;
        const uint32_t count = decodeToPixels ? 1u : session.levelCount();
        out.resize(count);
        for (uint32_t level = 0; level < count; level++){
            if (!session.transcodeLevel(level, plan.upload, content, out[level], err))
                return false;
        }
        format = plan.upload;
    }else{
        out = std::move(image.levels);
        const bool cpuDecode = decodeToPixels ? gpuFormatInfo(format).compressed || format == GpuTextureFormat::RGB9E5 : false;
        TextureUploadPlan plan;
        if (decodeToPixels){
            plan.path = cpuDecode ? TextureUploadPlan::Path::CpuDecode : TextureUploadPlan::Path::Native;
            plan.decoded = TextureDecode::decodedFormat(format);
            plan.upload = plan.decoded;
        }else if (support){
            plan = TextureFallback::planRaw(format, content, *support, lowMemoryFallback);
        }
        if (plan.path == TextureUploadPlan::Path::Unavailable){
            err = plan.reason;
            return false;
        }
        if (plan.path == TextureUploadPlan::Path::CpuDecode){
            if (!decodeToPixels)
                TextureFallback::logOnce(format, false, plan.reason);
            const size_t count = decodeToPixels ? 1 : out.size();
            std::vector<std::vector<uint8_t>> decoded(count);
            for (size_t level = 0; level < count; level++){
                const int w = mipDimension(levelWidth, (int)level);
                const int h = mipDimension(levelHeight, (int)level);
                if (!TextureDecode::decodeFaces(format, w, h, faces, out[level].data(), out[level].size(), decoded[level])){
                    err = std::string(gpuFormatName(format)) + " cannot be decoded on the CPU";
                    return false;
                }
                if (plan.upload != plan.decoded){
                    std::vector<uint8_t> converted;
                    std::vector<uint8_t> faceOut;
                    const size_t faceBytes = gpuFormatLevelBytes(plan.decoded, w, h);
                    for (int f = 0; f < faces; f++){
                        if (!TextureDecode::convert(plan.decoded, plan.upload, w, h, decoded[level].data() + faceBytes * f, faceBytes, faceOut)){
                            err = std::string("cannot convert ") + gpuFormatName(plan.decoded) + " to " + gpuFormatName(plan.upload);
                            return false;
                        }
                        converted.insert(converted.end(), faceOut.begin(), faceOut.end());
                    }
                    decoded[level] = std::move(converted);
                }
            }
            out = std::move(decoded);
            format = plan.upload;
        }
    }

    // CPU readers know RGBA8, R8 and R16: the others are widened for them
    if (decodeToPixels && format != GpuTextureFormat::RGBA8 && format != GpuTextureFormat::R8 && format != GpuTextureFormat::R16){
        const int w = levelWidth, h = levelHeight;
        const size_t n = (size_t)w * (size_t)h * (size_t)faces;
        std::vector<uint8_t> rgba(n * 4);
        const std::vector<uint8_t>& src = out[0];
        for (size_t i = 0; i < n; i++){
            uint8_t* o = &rgba[i * 4];
            if (format == GpuTextureFormat::RG8){
                const float x = src[i * 2] / 255.0f * 2.0f - 1.0f, y = src[i * 2 + 1] / 255.0f * 2.0f - 1.0f;
                const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
                o[0] = src[i * 2]; o[1] = src[i * 2 + 1]; o[2] = (uint8_t)std::lround((z * 0.5f + 0.5f) * 255.0f); o[3] = 255;
            }else if (format == GpuTextureFormat::RGBA16F){
                for (int c = 0; c < 4; c++){
                    uint16_t half;
                    std::memcpy(&half, &src[(i * 4 + c) * 2], 2);
                    o[c] = (uint8_t)std::clamp((int)std::lround(halfToFloat(half) * 255.0f), 0, 255);
                }
            }else{
                err = std::string(gpuFormatName(format)) + " pixels cannot be read on the CPU";
                return false;
            }
        }
        out.assign(1, std::move(rgba));
        format = GpuTextureFormat::RGBA8;
    }

    setLevels(format, isSrgb, faces, levelWidth, levelHeight, std::move(out));
    setLogicalSize(logicalWidth, logicalHeight);
    flags = flagsFromKeyValues(info);
    addFloatCpuView();
    droppedLevels = firstLevel;
    fullLevelCount = fullLevels;
    transparent = content == TextureContent::ColorAlpha;
    return true;
}

bool TextureData::loadSvgTexture(Data* filedata) {
    if (!filedata || !filedata->getMemPtr() || filedata->length() == 0) {
        return false;
    }

    // LunaSVG (the engine's SVG rasteriser, also behind HTML UI's <svg>): intrinsic size in
    // CSS px (96 dpi) x svgScale, rounded up and capped at 16384, straight alpha like every
    // other engine texture. A non-positive or non-finite scale means the intrinsic size.
    std::vector<uint8_t> pixels;
    int rasterWidth = 0;
    int rasterHeight = 0;
    if (!LunaSVGRuntime::rasterize(reinterpret_cast<const char*>(filedata->getMemPtr()), filedata->length(),
                                   svgScale, false, pixels, rasterWidth, rasterHeight)) {
        return false;
    }

    const size_t rasterSize = pixels.size();
    unsigned char* rasterData = static_cast<unsigned char*>(malloc(rasterSize));
    if (!rasterData) {
        return false;
    }
    memcpy(rasterData, pixels.data(), rasterSize);

    data = rasterData;
    width = rasterWidth;
    height = rasterHeight;
    originalWidth = rasterWidth;
    originalHeight = rasterHeight;
    channels = 4;
    color_format = ColorFormat::RGBA;
    size = static_cast<unsigned int>(rasterSize);
    transparent = hasAlpha();

    return true;
}

bool TextureData::loadTextureFromFile(const char* filename) {
    // A texture path may carry a rasterization scale as "<path>?svgScale=<value>".
    // Split it off here: load from the clean path, and let an embedded scale
    // override the current svgScale setting.
    float parsedScale = svgScale;
    std::string cleanPath = parseSvgScalePath(filename ? filename : "", &parsedScale);
    svgScale = parsedScale;

    Data filedata;

    int res = filedata.open(cleanPath.c_str());

    if (res==FileErrors::FILE_NOT_FOUND){
        // a cooked export ships no source: its cooked file stands in (decoded for the CPU)
        CookedTextureEntry cooked;
        if (CookedTextures::find(cleanPath, cooked) && filedata.open(cooked.file.c_str()) == FileErrors::FILEDATA_OK){
            if (!loadTexture(&filedata)){
                Log::error("Texture file not loaded: %s (cooked %s)", cleanPath.c_str(), cooked.file.c_str());
                return false;
            }
            return true;
        }
        Log::error("Texture file not found: %s", cleanPath.c_str());
        return false;
    }
    if (res==FileErrors::INVALID_PARAMETER){
        Log::error("Texture file path is invalid: %s", cleanPath.c_str());
        return false;
    }

    bool result = false;
    if (hasSvgExtension(cleanPath.c_str())) {
        if (dataOwned && data)
            releaseImageData();
        result = loadSvgTexture(&filedata);
    } else if (FloatDecoder decoder = floatDecoderFor(cleanPath)) {
        result = loadFloatTexture(decoder, filedata.getMemPtr(), filedata.length());
    } else {
        result = loadTexture(&filedata);
    }

    if (!result){
        Log::error("Texture file not loaded: %s", cleanPath.c_str());
    }

    return result;
}

bool TextureData::loadFloatTexture(FloatDecoder decoder, const unsigned char* bytes, size_t length){
    if (dataOwned && data)
        releaseImageData();
    levels.clear();
    levelStorage.reset();
    cpuCopy.reset();
    std::vector<float> rgba;
    int w = 0, h = 0;
    std::string error;
    if (!decoder(bytes, length, rgba, w, h, error) || w <= 0 || h <= 0 || rgba.size() < (size_t)w * (size_t)h * 4){
        Log::error("Error loading texture: %s", error.c_str());
        return false;
    }
    float* pixels = static_cast<float*>(malloc((size_t)w * (size_t)h * 4 * sizeof(float)));
    if (!pixels)
        return false;
    std::memcpy(pixels, rgba.data(), (size_t)w * (size_t)h * 4 * sizeof(float));
    data = pixels;
    width = originalWidth = w;
    height = originalHeight = h;
    channels = 4;
    color_format = ColorFormat::RGBA32F;
    size = (unsigned int)((size_t)w * (size_t)h * 4 * sizeof(float));
    gpuFormat = GpuTextureFormat::RGBA16F;
    srgb = false;
    flags = 0;
    transparent = hasAlpha();
    return true;
}

bool TextureData::loadTextureFromMemory(unsigned char* data, unsigned int dataLength){
    Data filedata;

    filedata.open(data, dataLength, false, false);

    bool result = loadTexture(&filedata);

    if (!result){
        Log::error("Texture from memory not loaded");
    }

    return result;
}

bool TextureData::loadTextureFromFileData(const char* filename, unsigned char* bytes, unsigned int dataLength){
    float parsedScale = svgScale;
    std::string cleanPath = parseSvgScalePath(filename ? filename : "", &parsedScale);
    svgScale = parsedScale;

    Data filedata;
    filedata.open(bytes, dataLength, false, false);

    bool result = false;
    if (hasSvgExtension(cleanPath.c_str())) {
        if (dataOwned && data)
            releaseImageData();
        result = loadSvgTexture(&filedata);
    } else if (FloatDecoder decoder = floatDecoderFor(cleanPath)) {
        result = loadFloatTexture(decoder, bytes, dataLength);
    } else {
        result = loadTexture(&filedata);
    }

    if (!result){
        Log::error("Texture file not loaded: %s", cleanPath.c_str());
    }

    return result;
}

bool TextureData::loadCubeMapFromSingleFile(const char* filename, std::array<TextureData, 6>& data){
    TextureData texture;
    if (!texture.loadTextureFromFile(filename))
        return false;
    return splitCubeMapCross(texture, data);
}

bool TextureData::loadCubeMapFromSingleFileData(const char* filename, unsigned char* bytes, unsigned int length, std::array<TextureData, 6>& data){
    TextureData texture;
    if (!texture.loadTextureFromFileData(filename, bytes, length))
        return false;
    return splitCubeMapCross(texture, data);
}

bool TextureData::splitCubeMapCross(TextureData& texture, std::array<TextureData, 6>& data){
    int tileW = 0;
    int tileH = 0;

    int right[2];
    int left[2];
    int top[2];
    int bottom[2];
    int front[2];
    int back[2];

    uint64_t ratio = texture.getWidth() * 100 / texture.getHeight();

    // Horizontal Cross
    if (ratio == 133){ // 4:3
        tileW = texture.getWidth() / 4;
        tileH = texture.getHeight() / 3;

        right[0] = 2; right[1] = 1;
        left[0] = 0; left[1] = 1;
        top[0] = 1; top[1] = 0;
        bottom[0] = 1; bottom[1] = 2;
        front[0] = 1; front[1] = 1;
        back[0] = 3; back[1] = 1;

    // Vertical Cross
    }else if (ratio == 75){ // 3:4
        tileW = texture.getWidth() / 3;
        tileH = texture.getHeight() / 4;

        right[0] = 2; right[1] = 1;
        left[0] = 0; left[1] = 1;
        top[0] = 1; top[1] = 0;
        bottom[0] = 1; bottom[1] = 2;
        front[0] = 1; front[1] = 1;
        back[0] = 1; back[1] = 3;
    }else{
        Log::error("Failed to load cubemap: format not supported");
        texture.releaseImageData();
        return false;
    }

    // Face indices follow the backend cubemap convention (sokol/OpenGL style):
    // 0=+X (Right), 1=-X (Left), 2=+Y (Top), 3=-Y (Bottom), 4=+Z, 5=-Z.
    // This loader uses OpenGL-style naming: Front = +Z (4), Back = -Z (5).

    // IMPORTANT:
    // Do not assign/copy 'texture' into each face and then call crop(), because TextureData::copy()
    // is shallow and would make all faces share the same underlying pixel pointer. The first crop()
    // frees that buffer, and subsequent crops crash.
    //
    // Instead, extract each face into its own buffer.

    auto extractFace = [&](int faceIndex, int xTile, int yTile){
        const int xOffset = xTile * tileW;
        const int yOffset = yTile * tileH;

        unsigned char* src = (unsigned char*)texture.getData();
        const int srcW = texture.getWidth();
        const int srcChannels = texture.getChannels();
        const ColorFormat srcFormat = texture.getColorFormat();

        // whole texels: RED16 has two bytes per channel
        const int srcPixelSize = srcChannels * TextureData::getBytesPerChannel(srcFormat);
        const int rowBytes = tileW * srcPixelSize;
        const int bufsize = tileW * tileH * srcPixelSize;
        unsigned char* faceData = (unsigned char*)malloc(bufsize * sizeof(unsigned char));
        if (!faceData)
            throw std::runtime_error("Out of memory while extracting cubemap face");

        for (int y = 0; y < tileH; y++){
            const size_t srcOffset = ((size_t)(yOffset + y) * (size_t)srcW + (size_t)xOffset) * (size_t)srcPixelSize;
            const size_t dstOffset = (size_t)y * (size_t)rowBytes;
            memcpy(faceData + dstOffset, src + srcOffset, (size_t)rowBytes);
        }

        data[faceIndex] = TextureData(tileW, tileH, (unsigned int)bufsize, srcFormat, srcChannels, faceData);
    };

    try {
        // Using JASPERENGINE convention:
        // Right (+X)
        extractFace(0, right[0], right[1]);
        // Left (-X)
        extractFace(1, left[0], left[1]);
        // Top (+Y)
        extractFace(2, top[0], top[1]);
        // Bottom (-Y)
        extractFace(3, bottom[0], bottom[1]);
        // Front (+Z)
        extractFace(4, front[0], front[1]);
        // Back (-Z)
        extractFace(5, back[0], back[1]);
    } catch (...) {
        texture.releaseImageData();
        throw;
    }

    // Release temporary source image data (it is not stored in the returned faces)
    texture.releaseImageData();

    return true;
}

void TextureData::copy ( const TextureData& v ){
    this->width = v.width;
    this->height = v.height;
    this->originalWidth = v.originalWidth;
    this->originalHeight = v.originalHeight;
    this->size = v.size;
    this->color_format = v.color_format;
    this->channels = v.channels;

    this->dataOwned = v.dataOwned;

    this->transparent = v.transparent;

    this->svgScale = v.svgScale;

    this->data = v.data;

    this->gpuFormat = v.gpuFormat;
    this->srgb = v.srgb;
    this->faceCount = v.faceCount;
    this->levels = v.levels;
    this->levelStorage = v.levelStorage;
    this->flags = v.flags;
    this->droppedLevels = v.droppedLevels;
    this->fullLevelCount = v.fullLevelCount;
    this->cpuCopy = v.cpuCopy;
}

TextureData::~TextureData() {
    if (dataOwned)
        releaseImageData();
}

void TextureData::releaseImageData(){
    cpuCopy.reset();
    if (levelStorage || !levels.empty()){
        // `data` is level 0 inside the storage: the storage goes when its last copy lets it go
        levels.clear();
        levelStorage.reset();
        data = NULL;
        return;
    }
    if (data){
        stbi_image_free(data);
        data = NULL;
    }
}

int TextureData::getBytesPerChannel(ColorFormat format){
    switch (format){
        case ColorFormat::RED16:
        case ColorFormat::RGBA16F:
        case ColorFormat::RG16F:
        case ColorFormat::R16F:
            return 2;
        case ColorFormat::RGBA32F:
        case ColorFormat::R32F:
            return 4;
        default:
            return 1;
    }
}

bool TextureData::isCodeFloatData() const{
    return isFloatFormat(color_format) && levels.empty() && gpuFormat != GpuTextureFormat::RGBA16F;
}

bool TextureData::isFloatFormat(ColorFormat format){
    return getFloatFormatChannels(format) > 0;
}

int TextureData::getFloatFormatChannels(ColorFormat format){
    switch (format){
        case ColorFormat::RGBA16F:
        case ColorFormat::RGBA32F:
            return 4;
        case ColorFormat::RG16F:
            return 2;
        case ColorFormat::R32F:
        case ColorFormat::R16F:
            return 1;
        default:
            return 0;
    }
}

const char* TextureData::getColorFormatName(ColorFormat format){
    switch (format){
        case ColorFormat::RED:     return "RED";
        case ColorFormat::RGBA:    return "RGBA";
        case ColorFormat::RED16:   return "RED16";
        case ColorFormat::SRGB8A8: return "SRGB8A8";
        case ColorFormat::RGBA16F: return "RGBA16F";
        case ColorFormat::RGBA32F: return "RGBA32F";
        case ColorFormat::R32F:    return "R32F";
        case ColorFormat::RG16F:   return "RG16F";
        case ColorFormat::R16F:    return "R16F";
    }
    return "unknown";
}

int TextureData::getWalkablePixelSize(const char* operation) const{
    if (!data){
        Log::error("Texture data %s: no data", operation);
        return 0;
    }
    const int floatChannels = getFloatFormatChannels(color_format);
    if (floatChannels > 0 && channels != floatChannels){
        Log::error("Texture data %s: colour format %s has %i channels, the data says %i",
                   operation, getColorFormatName(color_format), floatChannels, channels);
        return 0;
    }
    return channels * getBytesPerChannel(color_format);
}

int TextureData::getNearestPowerOfTwo(int size){
    int i;
    for(i = 31; i >= 0; i--)
        if ((size - 1) & (1<<i))
            break;
    return (1<<(i + 1));
}

bool TextureData::hasAlpha(){
    if (!data && !levels.empty())
        return (flags & TEXTURE_FLAG_ALPHA) != 0;
    if (!data)
        return false;
    // float alpha: below 1 (an HDR alpha past 1 is opaque)
    if (channels == 4 && color_format == ColorFormat::RGBA16F){
        // half 1.0 is 0x3C00; the positive halves order as their bits, negatives have the sign bit
        const uint16_t* texels = (const uint16_t*)data;
        for (size_t i = 0; i < (size_t)width * (size_t)height; i++){
            const uint16_t alpha = texels[i * 4 + 3];
            if ((alpha & 0x8000) || alpha < 0x3C00)
                return true;
        }
        return false;
    }
    if (channels == 4 && color_format == ColorFormat::RGBA32F){
        const float* texels = (const float*)data;
        for (size_t i = 0; i < (size_t)width * (size_t)height; i++){
            if (texels[i * 4 + 3] < 1.0f)
                return true;
        }
        return false;
    }
    if (channels == 4 && getBytesPerChannel(color_format) == 1){
        for(int y = 0; y < height; y++){
            for(int x = 0; x < width; x++){
                int pixel = (y * (width * channels)) + (x * channels);
                if (((unsigned char*)data)[pixel + 3] != 255)
                    return true;
            }
        }
    }
    return false;
}

void TextureData::crop(int xOffset, int yOffset, int newWidth, int newHeight){
    detachLevels();
    if (!data)
        return;

    // whole texels (RED16 and the float formats have several bytes per channel)
    const int pixelSize = getWalkablePixelSize("crop");
    if (pixelSize == 0)
        return;
    if (xOffset < 0 || yOffset < 0 || newWidth <= 0 || newHeight <= 0 ||
        xOffset + newWidth > width || yOffset + newHeight > height){
        Log::error("Texture data crop: %i x %i at (%i, %i) is outside the %i x %i image", newWidth, newHeight, xOffset, yOffset, width, height);
        return;
    }
    
    int rowsize = width * pixelSize;
    int newRowsize = newWidth * pixelSize;
    int bufsize = newWidth * newHeight * pixelSize;
    
    unsigned char* newData = (unsigned char*) malloc(bufsize*sizeof(unsigned char));
    
    int row_cnt;
    long off1 = 0;
    long off2 = 0;
    long off3 = xOffset*pixelSize;
    
    for (row_cnt=yOffset;row_cnt<yOffset+(newHeight);row_cnt++) {
        off1=row_cnt*rowsize;
        off2=(row_cnt-yOffset)*newRowsize;
        
        memcpy(newData+off2,(unsigned char*)data+off1+off3,newRowsize);
    }

    stbi_image_free(data);
    
    width = newWidth;
    height = newHeight;
    size = bufsize;
    data = newData;
}

void TextureData::resizePowerOfTwo(){
    resize(getNearestPowerOfTwo(width), getNearestPowerOfTwo(height));
}

void TextureData::resizeToSquare(){
    if (width != height){
        int side = std::max(width, height);
        resize(side, side);
    }
}

void TextureData::resize(int newWidth, int newHeight){
    detachLevels();
    if (!data)
        return;

    if ((newWidth != width) || (newHeight != height)){

        // RED16 and the float formats carry several bytes per channel, so a pixel is not always
        // one byte per channel
        int pixelSize = getWalkablePixelSize("resize");
        if (pixelSize == 0 || newWidth <= 0 || newHeight <= 0)
            return;
        int bufsize = newWidth * newHeight * pixelSize;
        unsigned char* newData = (unsigned char*) malloc(bufsize*sizeof(unsigned char));

        double scaleWidth =  (double)newWidth / (double)width;
        double scaleHeight = (double)newHeight / (double)height;

        for(int cy = 0; cy < newHeight; cy++){
            for(int cx = 0; cx < newWidth; cx++){
                
                int pixel = (cy * (newWidth*pixelSize)) + (cx*pixelSize);
                int nearestMatch =  (((int)(cy / scaleHeight) * (width*pixelSize)) + ((int)(cx / scaleWidth) * pixelSize) );
            
                for (int b = 0; b < pixelSize; b++){
                    newData[pixel + b] =  ((unsigned char*)data)[nearestMatch + b];
                }
            
            }
        }

        stbi_image_free(data);
    
        width = newWidth;
        height = newHeight;
        size = bufsize;
        data = newData;
        
    }

}

void TextureData::fitPowerOfTwo(){
    fitSize(0, 0, getNearestPowerOfTwo(width), getNearestPowerOfTwo(height));
}

void TextureData::fitSize(int xOffset, int yOffset, int newWidth, int newHeight){
    detachLevels();
    if (!data)
        return;
    
    if ((newWidth != width) || (newHeight != height)){

        // whole texels (RED16 and the float formats have several bytes per channel)
        const int pixelSize = getWalkablePixelSize("fitSize");
        if (pixelSize == 0)
            return;
        if (xOffset < 0 || yOffset < 0 || xOffset + width > newWidth || yOffset + height > newHeight){
            Log::error("Texture data fitSize: the %i x %i image at (%i, %i) doesn't fit %i x %i", width, height, xOffset, yOffset, newWidth, newHeight);
            return;
        }

        int bufsize = newWidth * newHeight * pixelSize;
        unsigned char* newData = (unsigned char*) malloc(bufsize*sizeof(unsigned char));
        
        for( unsigned int i = 0; i < bufsize; ++i )
        {
            newData[i] = 0;
        }
        
        for( unsigned int i = 0; i < height; ++i )
        {
            memcpy( (unsigned char*)newData + pixelSize * ( newWidth * ( yOffset + i ) + xOffset ), (unsigned char*)data + width * i * pixelSize, width * pixelSize );
        }
        
        stbi_image_free(data);
        
        width = newWidth;
        height = newHeight;
        size = bufsize;
        data = newData;
        
    }
    
}

void TextureData::fixAlphaBorder(){
    detachLevels();
    if (!data || channels != 4 || getBytesPerChannel(color_format) != 1)
        return;
    // rings of transparent pixels colored around visible ones, enough for a few mip levels
    bleedAlphaBorder((unsigned char*)data, width, height, 4);
}

void TextureData::flipVertical(){
    detachLevels();
    if (!data)
        return;

    // whole texels (RED16 and the float formats have several bytes per channel)
    const int pixelSize = getWalkablePixelSize("flipVertical");
    if (pixelSize == 0)
        return;
    
    int bufsize = width * pixelSize;
    
    unsigned char* tb1 = new unsigned char[bufsize];
    
    int row_cnt;
    long off1 = 0;
    long off2 = 0;
    
    for (row_cnt=0;row_cnt<height/2;row_cnt++) {
        off1=row_cnt*bufsize;
        off2=((height-1)-row_cnt)*bufsize;
        
        memcpy(tb1,(unsigned char*)data+off1,bufsize);
        memcpy((unsigned char*)data+off1,(unsigned char*)data+off2,bufsize);
        memcpy((unsigned char*)data+off2,tb1,bufsize);
    }
    
    delete [] tb1;
    
}

unsigned char TextureData::getColorComponent(int x, int y, int color){
    if (getBytesPerChannel(color_format) != 1){
        Log::error("Texture data getColorComponent: colour format %s is not 8-bit", getColorFormatName(color_format));
        return 0;
    }
    return ((unsigned char*)data)[((x + y*width)*channels)+color];
}

void TextureData::setDataOwned(bool dataOwned){
    this->dataOwned = dataOwned;
}

bool TextureData::getDataOwned() const{
    return this->dataOwned;
}

void TextureData::setSVGScale(float svgScale){
    this->svgScale = svgScale;
}

float TextureData::getSVGScale() const{
    return this->svgScale;
}

int TextureData::getWidth(){
    return width;
}

int TextureData::getHeight(){
    return height;
}

int TextureData::getOriginalWidth(){
    return originalWidth;
}

int TextureData::getOriginalHeight(){
    return originalHeight;
}

unsigned int TextureData::getSize(){
    return size;
}

ColorFormat TextureData::getColorFormat(){
    return color_format;
}

int TextureData::getChannels(){
    return channels;
}

void* TextureData::getData(){
    return data;
}

bool TextureData::isTransparent(){
    return transparent;
}

int TextureData::getMinNearestPowerOfTwo(){
    return getNearestPowerOfTwo(std::min(width, height));
}

void TextureData::detachLevels(){
    if (levels.empty() && !levelStorage)
        return;
    if (!data || levels.empty()){
        // block-compressed: no pixels to edit
        levels.clear();
        levelStorage.reset();
        data = NULL;
        return;
    }
    // (a float cook's CPU view is its own pixels, not level 0: addFloatCpuView)
    const bool view = (const uint8_t*)data != levels[0].data;
    const size_t bytes = view ? (size_t)size : gpuFormatLevelBytes(gpuFormat, levels[0].width, levels[0].height) * (size_t)faceCount;
    void* copy = malloc(bytes);
    if (copy)
        memcpy(copy, data, bytes);
    levels.clear();
    levelStorage.reset();
    data = copy;
    size = (unsigned int)bytes;
    faceCount = 1;
    droppedLevels = 0;
}

void TextureData::setLevels(GpuTextureFormat format, bool srgb, int faces, int width, int height, std::vector<std::vector<uint8_t>> levelBytes){
    // decoded pixels this object owns go; shared ones stay with whoever frees them
    if (dataOwned && data && !levelStorage)
        releaseImageData();
    data = NULL;
    size_t total = 0;
    for (const std::vector<uint8_t>& level : levelBytes)
        total += level.size();
    auto storage = std::make_shared<std::vector<uint8_t>>();
    storage->reserve(total);
    std::vector<size_t> offsets;
    for (const std::vector<uint8_t>& level : levelBytes){
        offsets.push_back(storage->size());
        storage->insert(storage->end(), level.begin(), level.end());
    }
    levels.clear();
    for (size_t i = 0; i < levelBytes.size(); i++){
        TextureLevel level;
        level.width = mipDimension(width, (int)i);
        level.height = mipDimension(height, (int)i);
        level.data = storage->data() + offsets[i];
        level.size = levelBytes[i].size();
        levels.push_back(level);
    }
    levelStorage = std::move(storage);
    gpuFormat = format;
    this->srgb = srgb;
    faceCount = std::max(1, faces);
    this->width = width;
    this->height = height;
    originalWidth = width;
    originalHeight = height;
    // the CPU readers' view of level 0
    if (format == GpuTextureFormat::RGBA8 || format == GpuTextureFormat::R8 || format == GpuTextureFormat::R16){
        data = levels.empty() ? NULL : (void*)levels[0].data;
        size = levels.empty() ? 0 : (unsigned int)levels[0].size;
        channels = format == GpuTextureFormat::RGBA8 ? 4 : 1;
        color_format = format == GpuTextureFormat::RGBA8 ? ColorFormat::RGBA : (format == GpuTextureFormat::R8 ? ColorFormat::RED : ColorFormat::RED16);
    }else{
        size = levels.empty() ? 0 : (unsigned int)levels[0].size;
        channels = gpuFormatInfo(format).channels;
        color_format = ColorFormat::RGBA;
    }
    dataOwned = false;
}

// A CPU-readable float cook (a terrain's .exr or .hdr height or density map): level 0 as linear
// RGBA32F for the CPU readers, kept in the levels' storage so copies share and free it with them
void TextureData::addFloatCpuView(){
    if (gpuFormat != GpuTextureFormat::RGBA16F || !(flags & TEXTURE_FLAG_CPU_READABLE) || levels.empty() || faceCount != 1 || !levelStorage)
        return;
    const size_t texels = (size_t)levels[0].width * (size_t)levels[0].height;
    if (levels[0].size < texels * 4 * sizeof(uint16_t))
        return;
    auto storage = std::make_shared<std::vector<uint8_t>>(*levelStorage);
    const size_t viewOffset = storage->size();
    storage->resize(viewOffset + texels * 4 * sizeof(float));
    const uint8_t* half = storage->data() + (levels[0].data - levelStorage->data());
    float* view = reinterpret_cast<float*>(storage->data() + viewOffset);
    const bool encoded = (flags & TEXTURE_FLAG_DISPLAY_ENCODED) != 0;
    for (size_t i = 0; i < texels * 4; i++){
        uint16_t bits;
        std::memcpy(&bits, half + i * sizeof(uint16_t), sizeof(bits));
        const float value = halfToFloat(bits);
        // the colour channels are stored pow(1/2.2), as the shaders decode them
        view[i] = (encoded && i % 4 != 3) ? std::pow(std::max(value, 0.0f), 2.2f) : value;
    }
    for (TextureLevel& level : levels)
        level.data = storage->data() + (level.data - levelStorage->data());
    levelStorage = std::move(storage);
    data = view;
    size = (unsigned int)(texels * 4 * sizeof(float));
    channels = 4;
    color_format = ColorFormat::RGBA32F;
}

bool TextureData::hasLevels() const{
    return !levels.empty();
}

const std::vector<TextureLevel>& TextureData::getLevels() const{
    return levels;
}

GpuTextureFormat TextureData::getGpuFormat() const{
    return gpuFormat;
}

bool TextureData::isSrgb() const{
    return srgb;
}

int TextureData::getFaceCount() const{
    return faceCount;
}

bool TextureData::isCompressed() const{
    return gpuFormatInfo(gpuFormat).compressed && !levels.empty();
}

bool TextureData::hasPixels() const{
    return !levels.empty() || (data != NULL && size > 0);
}

uint32_t TextureData::getFlags() const{
    return flags;
}

void TextureData::setFlags(uint32_t flags){
    this->flags = flags;
}

void TextureData::setLogicalSize(int width, int height){
    originalWidth = width;
    originalHeight = height;
}

int TextureData::getDroppedLevels() const{
    return droppedLevels;
}

void TextureData::setDroppedLevels(int dropped, int fullLevelCount){
    droppedLevels = dropped;
    this->fullLevelCount = fullLevelCount;
}

int TextureData::getFullLevelCount() const{
    return fullLevelCount;
}

uint64_t TextureData::getLevelBytes() const{
    uint64_t bytes = 0;
    for (const TextureLevel& level : levels)
        bytes += level.size;
    if (levels.empty() && data)
        bytes = size;
    return bytes;
}

void TextureData::setCpuCopy(std::shared_ptr<std::array<TextureData, 6>> copy){
    cpuCopy = std::move(copy);
}

const std::shared_ptr<std::array<TextureData, 6>>& TextureData::getCpuCopy() const{
    return cpuCopy;
}

bool TextureData::toRgba8(std::vector<uint8_t>& out) const{
    if (!data || width <= 0 || height <= 0)
        return false;
    const size_t n = (size_t)width * (size_t)height;
    out.resize(n * 4);
    if (color_format == ColorFormat::RGBA32F){
        const float* src = (const float*)data;
        for (size_t i = 0; i < n; i++){
            for (int c = 0; c < 3; c++)
                out[i * 4 + c] = (uint8_t)std::clamp((int)std::lround(std::pow(std::max(src[i * 4 + c], 0.0f), 1.0f / 2.2f) * 255.0f), 0, 255);
            out[i * 4 + 3] = (uint8_t)std::clamp((int)std::lround(src[i * 4 + 3] * 255.0f), 0, 255);
        }
        return true;
    }
    const unsigned char* src = (const unsigned char*)data;
    for (size_t i = 0; i < n; i++){
        if (color_format == ColorFormat::RED16){
            uint16_t v;
            memcpy(&v, src + i * 2, 2);
            out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = (uint8_t)(((uint32_t)v * 255u + 32767u) / 65535u);
            out[i * 4 + 3] = 255;
        }else if (channels == 1){
            out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = src[i];
            out[i * 4 + 3] = 255;
        }else if (channels == 4){
            memcpy(&out[i * 4], src + i * 4, 4);
        }else{
            return false;
        }
    }
    return true;
}

void TextureData::cleanupTexture(void* data){
    TextureData* texData = (TextureData*)data;
    texData->releaseImageData();
}
