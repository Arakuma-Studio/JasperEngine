// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIImageCache.h"

#include "HtmlUIClock.h"
#include "HtmlUIDependencies.h"
#include "compat/VirtualFiles.h"
#include "effects/EffectRuntime.h"

#include "Engine.h"
#include "HtmlUI.h"
#include "Log.h"
#include "io/Data.h"
#include "io/File.h"
#include "io/FileData.h"
#include "io/ResourcePack.h"
#include "jobs/JobSystem.h"
#include "math/Rect.h"
#include "profiler/Profile.h"
#include "render/TextureRender.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"
#include "texture/CookedTextures.h"
#include "texture/Ktx2.h"
#include "texture/TextureData.h"

#include "stb_image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>

#if defined(__APPLE__)
    #include <TargetConditionals.h>
#endif

using namespace jasperengine;

namespace {

    constexpr size_t imageCacheMiB = 1024 * 1024;

    std::atomic<int> budgetMiBSetting{-1};
    std::atomic<bool> synchronousForTesting{false};

    // premultiplied RGBA8 of a whole file, row 0 at the top
    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
        std::string error;
    };

    // the decode of LoadTexture before S93, on any thread: float images (.hdr, .exr) display-encoded to
    // 8 bits like every texture, gray and gray-alpha widened, the alpha premultiplied (the engine's
    // TextureAlphaBorder fix is never applied: premultiplied colours need none)
    std::shared_ptr<const DecodedImage> decodeImageFile(const std::string& path){
        JE_PROFILE_SCOPE("htmlui-image-decode");
        auto result = std::make_shared<DecodedImage>();
        TextureData data;
        // (a TextureData does not own what it loads: its pixels go once copied out)
        struct ReleaseData {
            TextureData& data;
            ~ReleaseData(){ data.releaseImageData(); }
        } releaseData{data};
        bool loaded = false;
        if (htmlui::compat::VirtualFiles::isVirtual(path)){
            // compat's generated files (data: URIs); a "?svgScale=" suffix stays meaningful for virtual SVGs
            float svgScale = 1.0f;
            const std::string file = TextureData::parseSvgScalePath(path, &svgScale);
            const std::shared_ptr<const std::string> content = htmlui::compat::VirtualFiles::find(file);
            if (content && !content->empty()){
                data.setSVGScale(svgScale);
                loaded = data.loadTextureFromMemory(reinterpret_cast<unsigned char*>(const_cast<char*>(content->data())), (unsigned int)content->size());
            }
        }else{
            loaded = !path.empty() && data.loadTextureFromFile(path.c_str());
        }
        if (!loaded){
            result->error = "could not load image '" + path + "'";
            return result;
        }
        const int width = data.getWidth();
        const int height = data.getHeight();
        int channels = data.getChannels();
        const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
        std::vector<uint8_t> converted;
        if (data.getColorFormat() == ColorFormat::RGBA32F && data.toRgba8(converted)){
            pixels = converted.data();
            channels = 4;
        }
        if (data.getColorFormat() == ColorFormat::RED16 || !pixels || width <= 0 || height <= 0 || channels < 1 || channels > 4){
            result->error = "unsupported image format '" + path + "'";
            return result;
        }
        const size_t count = (size_t)width * (size_t)height;
        result->rgba.resize(count * 4);
        for (size_t i = 0; i < count; i++){
            const unsigned char* p = pixels + i * channels;
            uint8_t r, g, b, a;
            if (channels == 1){
                r = g = b = p[0]; a = 255;
            }else if (channels == 2){
                r = g = b = p[0]; a = p[1];
            }else if (channels == 3){
                r = p[0]; g = p[1]; b = p[2]; a = 255;
            }else{
                r = p[0]; g = p[1]; b = p[2]; a = p[3];
            }
            uint8_t* out = &result->rgba[i * 4];
            out[0] = (uint8_t)((r * a + 127) / 255);
            out[1] = (uint8_t)((g * a + 127) / 255);
            out[2] = (uint8_t)((b * a + 127) / 255);
            out[3] = a;
        }
        result->width = width;
        result->height = height;
        return result;
    }

    // the file without "?svgScale=" (the part a stamp and a sprite sheet know)
    std::string filePartOf(const std::string& path){
        return path.substr(0, path.find('?'));
    }

    // what a file was when its images were made: a rewrite (hot reload, another tool) is another key
    std::string stampOf(const std::string& path){
        // a virtual file is replaced, never rewritten: its content is the stamp
        if (htmlui::compat::VirtualFiles::isVirtual(path)){
            const std::shared_ptr<const std::string> content = htmlui::compat::VirtualFiles::find(TextureData::parseSvgScalePath(path));
            return content ? std::to_string((uintptr_t)content.get()) + ":" + std::to_string(content->size()) : "0";
        }
        std::error_code error;
        const std::filesystem::path systemPath(FileData::getSystemPath(filePartOf(path)));
        const auto size = std::filesystem::file_size(systemPath, error);
        if (error)
            return "0";
        const auto time = std::filesystem::last_write_time(systemPath, error);
        if (error)
            return std::to_string((unsigned long long)size);
        return std::to_string((unsigned long long)size) + ":" + std::to_string((long long)time.time_since_epoch().count());
    }

    // The dimensions from the file's first bytes (PNG, JPEG, BMP, TGA, GIF, PSD, HDR, KTX2) or its
    // cook's record; false when only a decode can tell (SVG, EXR, a format stb cannot probe)
    bool probeImageSize(const std::string& path, int& width, int& height){
        const std::string file = filePartOf(path);
        if (path.find('?') != std::string::npos || TextureData::hasSvgExtension(file.c_str()) || htmlui::compat::VirtualFiles::isVirtual(file))
            return false;
        // a packed resource is read whole (it has no file handle), a file only its first bytes
        std::unique_ptr<FileData> data;
        if (ResourcePack::contains(file)){
            data = std::make_unique<Data>(file.c_str());
        }else{
            auto handle = std::make_unique<File>();
            if (handle->open(file.c_str()) == FileErrors::FILEDATA_OK)
                data = std::move(handle);
        }
        const unsigned int length = data ? data->length() : 0;
        if (length == 0){
            // a cooked export ships no source: its cook knows the size
            CookedTextureEntry cooked;
            if (CookedTextures::find(file, cooked) && cooked.width > 0 && cooked.height > 0){
                width = cooked.width;
                height = cooked.height;
                return true;
            }
            return false;
        }
        // 64 KiB holds a PNG's IHDR and a JPEG's frame header after its EXIF block
        std::vector<unsigned char> prefix(std::min<unsigned int>(length, 64 * 1024));
        data->seek(0);
        prefix.resize(data->read(prefix.data(), (unsigned int)prefix.size()));
        if (Ktx2::isKtx2(std::span<const uint8_t>(prefix.data(), prefix.size()))){
            if (prefix.size() < 28)
                return false;
            uint32_t w = 0, h = 0;
            std::memcpy(&w, prefix.data() + 20, 4);
            std::memcpy(&h, prefix.data() + 24, 4);
            width = (int)w;
            height = (int)h;
            return width > 0 && height > 0;
        }
        int channels = 0;
        if (!stbi_info_from_memory(prefix.data(), (int)prefix.size(), &width, &height, &channels))
            return false;
        return width > 0 && height > 0;
    }

    int percentDecodeDigit(char c){
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::string percentDecoded(const std::string& text){
        std::string out;
        for (size_t i = 0; i < text.size(); i++){
            if (text[i] == '%' && i + 2 < text.size() && percentDecodeDigit(text[i + 1]) >= 0 && percentDecodeDigit(text[i + 2]) >= 0){
                out += (char)(percentDecodeDigit(text[i + 1]) * 16 + percentDecodeDigit(text[i + 2]));
                i += 2;
            }else{
                out += text[i];
            }
        }
        return out;
    }

    std::string numberText(float value){
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%g", value);
        return buffer;
    }

}

// ---------------------------------------------------------------- region

std::string HtmlUIImageRegion::keyText() const{
    switch (kind){
        case Kind::NONE: return std::string();
        case Kind::PIXELS: return "xywh=" + numberText(x) + "," + numberText(y) + "," + numberText(width) + "," + numberText(height);
        case Kind::PERCENT: return "xywh=percent:" + numberText(x) + "," + numberText(y) + "," + numberText(width) + "," + numberText(height);
        case Kind::SPRITE: return "sprite=" + sprite;
    }
    return std::string();
}

bool HtmlUIImageRegion::parse(const std::string& input, HtmlUIImageRegion& out, std::string* error){
    out = HtmlUIImageRegion();
    std::string fragment = (!input.empty() && input[0] == '#') ? input.substr(1) : input;
    if (fragment.empty())
        return true;
    auto fail = [&](const std::string& message){
        if (error)
            *error = message;
        out = HtmlUIImageRegion();
        return false;
    };
    if (fragment.compare(0, 7, "sprite=") == 0){
        out.kind = Kind::SPRITE;
        out.sprite = percentDecoded(fragment.substr(7));
        if (out.sprite.empty())
            return fail("'#" + fragment + "' names no sprite");
        return true;
    }
    if (fragment.compare(0, 5, "xywh=") != 0)
        return true;
    std::string values = fragment.substr(5);
    out.kind = Kind::PIXELS;
    if (values.compare(0, 6, "pixel:") == 0){
        values = values.substr(6);
    }else if (values.compare(0, 8, "percent:") == 0){
        out.kind = Kind::PERCENT;
        values = values.substr(8);
    }
    float numbers[4] = {0, 0, 0, 0};
    size_t start = 0;
    for (int i = 0; i < 4; i++){
        const size_t end = (i < 3) ? values.find(',', start) : values.size();
        if (end == std::string::npos)
            return fail("'#" + fragment + "' needs four numbers: x,y,w,h");
        const std::string item = values.substr(start, end - start);
        char* parsed = nullptr;
        numbers[i] = std::strtof(item.c_str(), &parsed);
        if (item.empty() || !parsed || *parsed != '\0' || !std::isfinite(numbers[i]))
            return fail("'#" + fragment + "': '" + item + "' is not a number");
        start = end + 1;
    }
    out.x = numbers[0];
    out.y = numbers[1];
    out.width = numbers[2];
    out.height = numbers[3];
    if (out.x < 0.0f || out.y < 0.0f || out.width <= 0.0f || out.height <= 0.0f)
        return fail("'#" + fragment + "': x and y must not be negative, w and h must be positive");
    return true;
}

// ---------------------------------------------------------------- entries

struct HtmlUIImageCache::Source {
    std::string path;
    jobs::Task<std::shared_ptr<const DecodedImage>> task;
    std::shared_ptr<const DecodedImage> pixels;   // once taken from the task (or decoded at once)
    uint64_t usedAt = 0;

    // the decode, waited for when `wait`; nullptr while it runs
    const DecodedImage* result(bool wait){
        if (!pixels){
            if (!task.isValid())
                return nullptr;
            if (wait)
                task.wait();
            if (!task.isReady())
                return nullptr;
            try{
                pixels = task.get();
            }catch(...){
                auto failed = std::make_shared<DecodedImage>();
                failed->error = "the decode of image '" + path + "' was cancelled";
                pixels = failed;
            }
            task = jobs::Task<std::shared_ptr<const DecodedImage>>();
        }
        return pixels.get();
    }

    size_t bytes() const{
        return pixels ? pixels->rgba.size() : 0;
    }
};

struct HtmlUIImageCache::Entry {
    std::string key;
    std::string fileKey;
    std::string path;
    // the texels taken from the file (the whole file, or a region)
    int x = 0, y = 0, width = 0, height = 0;
    bool region = false;
    TextureFilter filter = TextureFilter::LINEAR;
    TextureWrap wrapU = TextureWrap::CLAMP_TO_EDGE, wrapV = TextureWrap::CLAMP_TO_EDGE;
    std::shared_ptr<Source> source;     // until uploaded
    std::unique_ptr<TextureRender> texture;
    bool failed = false;
    bool cached = true;                 // in the cache's map (false once invalidated)
    size_t bytes = 0;
    int holders = 0;
    uint64_t releasedAt = 0;

    ~Entry(){
        if (texture)
            texture->destroyTexture();
    }
};

// ---------------------------------------------------------------- references

HtmlUIImageCache::Reference::Reference(std::shared_ptr<Entry> e): entry(std::move(e)){
    entry->holders++;
}

HtmlUIImageCache::Reference::~Reference(){
    if (--entry->holders == 0)
        HtmlUIImageCache::get().onReleased(*entry);
}

int HtmlUIImageCache::Reference::getWidth() const{
    return entry->width;
}

int HtmlUIImageCache::Reference::getHeight() const{
    return entry->height;
}

bool HtmlUIImageCache::Reference::pending() const{
    return !entry->texture && !entry->failed;
}

bool HtmlUIImageCache::Reference::decoding() const{
    return !entry->texture && !entry->failed && entry->source && !entry->source->pixels && !entry->source->task.isReady();
}

bool HtmlUIImageCache::Reference::failed() const{
    return entry->failed;
}

void HtmlUIImageCache::Reference::finish(){
    if (entry->source)
        entry->source->result(true);
}

TextureRender* HtmlUIImageCache::Reference::texture(){
    Entry& e = *entry;
    if (e.texture || e.failed)
        return e.texture.get();
    if (!e.source || !Engine::isViewLoaded())
        return nullptr;
    const DecodedImage* decoded = e.source->result(false);
    if (!decoded)
        return nullptr;
    HtmlUIImageCache& cache = HtmlUIImageCache::get();
    if (!decoded->error.empty() || decoded->rgba.empty()){
        e.failed = true;
        Log::error("[HTML UI] %s", decoded->error.empty() ? ("could not load image '" + e.path + "'").c_str() : decoded->error.c_str());
        e.source.reset();
        return nullptr;
    }

    // the region's texels in their own texture (whole images upload the decode as it is)
    const uint8_t* texels = decoded->rgba.data();
    std::vector<uint8_t> cropped;
    int width = decoded->width, height = decoded->height;
    if (e.region){
        if (e.x + e.width > decoded->width || e.y + e.height > decoded->height){
            e.failed = true;
            Log::error("[HTML UI] the region of image '%s' is outside its %d x %d pixels", e.path.c_str(), decoded->width, decoded->height);
            e.source.reset();
            return nullptr;
        }
        cropped.resize((size_t)e.width * (size_t)e.height * 4);
        for (int row = 0; row < e.height; row++){
            std::memcpy(&cropped[(size_t)row * e.width * 4], &decoded->rgba[((size_t)(e.y + row) * decoded->width + e.x) * 4], (size_t)e.width * 4);
        }
        texels = cropped.data();
        width = e.width;
        height = e.height;
    }
    {
        JE_PROFILE_SCOPE("htmlui-image-upload");
        std::unique_ptr<TextureRender> texture = std::make_unique<TextureRender>();
        void* data[1] = {const_cast<uint8_t*>(texels)};
        size_t size[1] = {(size_t)width * (size_t)height * 4};
        // LINEAR (or NEAREST) min filter: no mipmaps
        if (!texture->createTexture("htmlui-image", width, height, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size, e.filter,
                                    e.filter, e.wrapU, e.wrapV)){
            texture->destroyTexture();
            e.failed = true;
            Log::error("[HTML UI] could not create the texture of image '%s'", e.path.c_str());
            e.source.reset();
            return nullptr;
        }
        e.texture = std::move(texture);
    }
    e.width = width;
    e.height = height;
    e.bytes = (size_t)width * (size_t)height * 4;
    cache.counters.uploads++;
    cache.watchViewLoss();
    // an atlas keeps its decode for the next regions cut from it (while the budget has room)
    if (e.region && getBudgetBytes() > 0){
        e.source->usedAt = ++cache.releaseTick;
        cache.keptSources[e.fileKey] = e.source;
    }
    e.source.reset();
    cache.trim();
    return e.texture.get();
}

// ---------------------------------------------------------------- cache

HtmlUIImageCache::HtmlUIImageCache() = default;

HtmlUIImageCache::~HtmlUIImageCache() = default;

HtmlUIImageCache& HtmlUIImageCache::get(){
    // never destroyed: its textures must not outlive the GPU device at exit (clear() frees them)
    static HtmlUIImageCache* cache = new HtmlUIImageCache();
    return *cache;
}

void HtmlUIImageCache::setBudgetMiB(int mib){
    budgetMiBSetting = mib;
    get().trim();
}

int HtmlUIImageCache::defaultBudgetMiB(){
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    return 32;
#else
    return 64;
#endif
}

size_t HtmlUIImageCache::getBudgetBytes(){
    const int mib = budgetMiBSetting;
    return (size_t)(mib < 0 ? defaultBudgetMiB() : mib) * imageCacheMiB;
}

void HtmlUIImageCache::setSynchronousForTesting(bool synchronous){
    synchronousForTesting = synchronous;
}

bool HtmlUIImageCache::decodesSynchronously(){
    // the rule of distance glyphs (RmlFontEngine): a frame's pixels depend on the frames before it
    // only, not on the background lane's timing; without workers nothing would run before the pump
    return synchronousForTesting || Engine::getFixedDeltatime() > 0.0 || HtmlUIClock::getFixedTime().has_value() ||
           htmlui::EffectClock::isFixed() || jobs::system().isSingleThreaded();
}

std::shared_ptr<HtmlUIImageCache::Reference> HtmlUIImageCache::acquire(const Request& request, std::string* error, AcquireInfo* info){
    auto fail = [error](const std::string& message) -> std::shared_ptr<Reference> {
        if (error)
            *error = message;
        return nullptr;
    };
    if (request.path.empty())
        return fail("could not load image ''");

    const std::string fileKey = normalizeHtmlUIPath(request.path) + "|" + stampOf(request.path);
    const std::string key = fileKey + "|" + std::to_string((int)request.filter) + (request.repeatX ? "x" : "-") + (request.repeatY ? "y" : "-") +
                            "|" + request.region.keyText();
    const bool sync = request.sync || decodesSynchronously();
    auto found = entries.find(key);
    if (found != entries.end() && !found->second->failed){
        counters.hits++;
        if (info)
            info->cacheHit = true;
        auto reference = std::make_shared<Reference>(found->second);
        if (sync)
            reference->finish();
        return reference;
    }

    // the decode of the file: a running one (another region, filter or wrap of it), a kept one, or a new one
    std::shared_ptr<Source> source;
    auto kept = keptSources.find(fileKey);
    if (kept != keptSources.end()){
        source = kept->second;
        source->usedAt = ++releaseTick;
    }else{
        auto running = sources.find(fileKey);
        if (running != sources.end())
            source = running->second.lock();
    }
    int fileWidth = 0, fileHeight = 0;
    if (source && source->pixels && source->pixels->error.empty()){
        fileWidth = source->pixels->width;
        fileHeight = source->pixels->height;
    }else if (source && !source->pixels && probeImageSize(request.path, fileWidth, fileHeight)){
        // (a running decode: its header gives the size)
    }else{
        source.reset();
    }
    if (!source){
        source = std::make_shared<Source>();
        source->path = request.path;
        counters.decodes++;
        if (info)
            info->decoded = true;
        if (!sync && probeImageSize(request.path, fileWidth, fileHeight)){
            const std::string path = request.path;
            source->task = jobs::submit([path](){ return decodeImageFile(path); },
                                        jobs::SubmitOptions{jobs::Lane::Background, jobs::GateClass::Heavy, "htmlui-image-decode", jobs::Tag::Asset});
        }else{
            // decoding="sync", a deterministic clock, no worker threads, or a file only a decode can size
            source->pixels = decodeImageFile(request.path);
            if (!source->pixels->error.empty())
                return fail(source->pixels->error);
            fileWidth = source->pixels->width;
            fileHeight = source->pixels->height;
        }
        sources[fileKey] = source;
    }

    auto entry = std::make_shared<Entry>();
    entry->key = key;
    entry->fileKey = fileKey;
    entry->path = request.path;
    entry->filter = request.filter;
    entry->wrapU = request.repeatX ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE;
    entry->wrapV = request.repeatY ? TextureWrap::REPEAT : TextureWrap::CLAMP_TO_EDGE;
    entry->width = fileWidth;
    entry->height = fileHeight;

    // the region in the file's pixels, checked against its size
    const HtmlUIImageRegion& region = request.region;
    if (!region.empty()){
        float rx = region.x, ry = region.y, rw = region.width, rh = region.height;
        if (region.kind == HtmlUIImageRegion::Kind::PERCENT){
            rx = region.x * (float)fileWidth / 100.0f;
            ry = region.y * (float)fileHeight / 100.0f;
            rw = region.width * (float)fileWidth / 100.0f;
            rh = region.height * (float)fileHeight / 100.0f;
        }else if (region.kind == HtmlUIImageRegion::Kind::SPRITE){
            const std::string file = normalizeHtmlUIPath(filePartOf(request.path));
            const std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(file);
            const SpriteFrame* frame = nullptr;
            if (sheet){
                for (const SpriteFrame& candidate : sheet->frames){
                    if (candidate.name == region.sprite){
                        frame = &candidate;
                        break;
                    }
                }
            }
            Rect place;
            if (info)
                info->regionFailed = true;
            if (!frame)
                return fail("image '" + file + "' has no sprite '" + region.sprite + "' (its Sprite Mode sheet names the frames)");
            if (!SpriteSheetSources::frameTextureRect(*frame, place)){
                if (sheet->frames.size() != 1)
                    return fail("sprite '" + region.sprite + "' of image '" + file + "' has no place in the texture");
                place = Rect(0.0f, 0.0f, (float)fileWidth, (float)fileHeight);
            }
            rx = place.getX();
            ry = place.getY();
            rw = std::fabs(place.getWidth());
            rh = std::fabs(place.getHeight());
        }
        const int x0 = (int)std::lround(rx), y0 = (int)std::lround(ry);
        const int x1 = (int)std::lround(rx + rw), y1 = (int)std::lround(ry + rh);
        if (x0 < 0 || y0 < 0 || x1 > fileWidth || y1 > fileHeight || x1 <= x0 || y1 <= y0){
            if (info)
                info->regionFailed = true;
            return fail("the region '#" + region.keyText() + "' of image '" + normalizeHtmlUIPath(filePartOf(request.path)) + "' is outside its " +
                        std::to_string(fileWidth) + " x " + std::to_string(fileHeight) + " pixels");
        }
        if (info)
            info->regionFailed = false;
        entry->region = true;
        entry->x = x0;
        entry->y = y0;
        entry->width = x1 - x0;
        entry->height = y1 - y0;
    }
    entry->source = source;
    entries[key] = entry;
    auto reference = std::make_shared<Reference>(entry);
    if (sync)
        reference->finish();
    return reference;
}

void HtmlUIImageCache::onReleased(Entry& entry){
    entry.releasedAt = ++releaseTick;
    if (!entry.cached)
        return;
    // a failed image or one not decoded yet is not kept: nothing to save
    if (entry.failed || !entry.texture || getBudgetBytes() == 0){
        forget(entry);
        return;
    }
    trim();
}

void HtmlUIImageCache::forget(Entry& entry){
    entry.cached = false;
    // (erasing may destroy the entry: the key is copied)
    const std::string key = entry.key;
    entries.erase(key);
}

size_t HtmlUIImageCache::getReleasedBytes() const{
    size_t bytes = 0;
    for (const auto& [key, entry] : entries){
        if (entry->holders == 0)
            bytes += entry->bytes;
    }
    for (const auto& [key, source] : keptSources)
        bytes += source->bytes();
    return bytes;
}

void HtmlUIImageCache::trim(){
    const size_t budget = getBudgetBytes();
    size_t released = getReleasedBytes();
    while (released > budget){
        // the least recently released image or kept decode goes
        Entry* oldest = nullptr;
        for (const auto& [key, entry] : entries){
            if (entry->holders == 0 && entry->bytes > 0 && (!oldest || entry->releasedAt < oldest->releasedAt))
                oldest = entry.get();
        }
        auto oldestSource = keptSources.end();
        for (auto it = keptSources.begin(); it != keptSources.end(); ++it){
            if (oldestSource == keptSources.end() || it->second->usedAt < oldestSource->second->usedAt)
                oldestSource = it;
        }
        if (oldestSource != keptSources.end() && (!oldest || oldestSource->second->usedAt < oldest->releasedAt)){
            released -= oldestSource->second->bytes();
            keptSources.erase(oldestSource);
        }else if (oldest){
            released -= oldest->bytes;
            forget(*oldest);
        }else{
            break;
        }
    }
    for (auto it = sources.begin(); it != sources.end();){
        if (it->second.expired())
            it = sources.erase(it);
        else
            ++it;
    }
}

void HtmlUIImageCache::invalidate(const std::string& path){
    const std::string file = normalizeHtmlUIPath(filePartOf(path)) + "|";
    for (auto it = entries.begin(); it != entries.end();){
        if (it->second->fileKey.compare(0, file.size(), file) == 0){
            it->second->cached = false;
            it = entries.erase(it);
        }else{
            ++it;
        }
    }
    for (auto it = keptSources.begin(); it != keptSources.end();){
        if (it->first.compare(0, file.size(), file) == 0)
            it = keptSources.erase(it);
        else
            ++it;
    }
    for (auto it = sources.begin(); it != sources.end();){
        if (it->first.compare(0, file.size(), file) == 0)
            it = sources.erase(it);
        else
            ++it;
    }
}

void HtmlUIImageCache::watchViewLoss(){
    // HtmlUI drops the textures with its contexts' while it is up (in that order: the renderers hold handles onto these);
    // the cache outlives HtmlUI, so it hears the view go by itself too: a texture of a dead view would be reused by the
    // next document. (add() replaces the subscription: every upload and every HtmlUI shutdown puts it back, as
    // Engine::clearAllSubscriptions may have dropped it.)
    Engine::onViewDestroyed.add(viewLossTag, std::function<void()>([](){
        if (!HtmlUI::isInitialized())
            HtmlUIImageCache::get().releaseGpuResources();
    }));
}

void HtmlUIImageCache::releaseGpuResources(){
    std::vector<std::shared_ptr<Entry>> held;
    for (auto& [key, entry] : entries){
        if (entry->holders > 0)
            held.push_back(entry);
    }
    // the textures went with the view: released images go, held ones are decoded again
    for (auto& [key, entry] : entries){
        if (entry->texture){
            entry->texture->destroyTexture();
            entry->texture.reset();
            entry->bytes = 0;
        }
        entry->cached = false;
    }
    entries.clear();
    for (const std::shared_ptr<Entry>& entry : held){
        if (entry->failed)
            continue;
        entry->cached = true;
        if (!entry->source){
            auto source = std::make_shared<Source>();
            source->path = entry->path;
            const std::string path = entry->path;
            counters.decodes++;
            if (decodesSynchronously())
                source->pixels = decodeImageFile(path);
            else
                source->task = jobs::submit([path](){ return decodeImageFile(path); },
                                            jobs::SubmitOptions{jobs::Lane::Background, jobs::GateClass::Heavy, "htmlui-image-decode", jobs::Tag::Asset});
            entry->source = source;
        }
        entries[entry->key] = entry;
    }
}

void HtmlUIImageCache::clear(){
    for (auto it = entries.begin(); it != entries.end();){
        if (it->second->holders == 0){
            it->second->cached = false;
            it = entries.erase(it);
        }else{
            ++it;
        }
    }
    keptSources.clear();
    sources.clear();
}

size_t HtmlUIImageCache::getEntryCount() const{
    return entries.size();
}

size_t HtmlUIImageCache::getDecodingCount() const{
    size_t decoding = 0;
    for (const auto& [key, entry] : entries){
        if (entry->source && !entry->source->pixels && !entry->source->task.isReady())
            decoding++;
    }
    return decoding;
}

void HtmlUIImageCache::getGpuHandles(std::vector<uint32_t>& images) const{
    images.clear();
    for (const auto& [key, entry] : entries){
        if (entry->texture){
            const uint32_t id = entry->texture->backend.get().id;
            if (id != 0)
                images.push_back(id);
        }
    }
}

// ---------------------------------------------------------------- prewarm

std::shared_ptr<HtmlUIImageCache::PrewarmJob> HtmlUIImageCache::prewarm(const std::vector<std::string>& paths){
    auto job = std::make_shared<PrewarmJob>();
    for (const std::string& path : paths){
        Request request;
        const size_t hash = path.find('#');
        request.path = path.substr(0, hash);
        std::string error;
        if (hash != std::string::npos && !HtmlUIImageRegion::parse(path.substr(hash), request.region, &error)){
            Log::warn("[HTML UI] prewarm: %s", error.c_str());
            continue;
        }
        std::shared_ptr<Reference> reference = acquire(request, &error);
        if (!reference){
            Log::warn("[HTML UI] prewarm: %s", error.c_str());
            continue;
        }
        job->images.push_back(std::move(reference));
    }
    job->total = job->images.size();
    const bool idle = prewarms.empty();
    prewarms.push_back(job);
    // the jobs' main-thread part runs at the start of every frame until they are done
    if (idle)
        jobs::postToMain([](){ HtmlUIImageCache::get().scheduleTick(); });
    return job;
}

void HtmlUIImageCache::scheduleTick(){
    update();
    if (!prewarms.empty())
        jobs::postToMain([](){ HtmlUIImageCache::get().scheduleTick(); });
}

void HtmlUIImageCache::update(double budgetMs){
    if (prewarms.empty())
        return;
    const auto start = std::chrono::steady_clock::now();
    bool uploaded = false;
    std::vector<std::shared_ptr<PrewarmJob>> finished;
    for (const std::shared_ptr<PrewarmJob>& job : prewarms){
        size_t completed = 0;
        for (const std::shared_ptr<Reference>& image : job->images){
            if (image->pending()){
                // the first upload of a frame always runs, later ones within the budget
                const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                if (uploaded && elapsed >= budgetMs)
                    continue;
                if (image->texture())
                    uploaded = true;
            }
            if (!image->pending())
                completed++;
        }
        job->completed = completed;
        if (completed == job->total){
            job->done = true;
            finished.push_back(job);
        }
    }
    for (const std::shared_ptr<PrewarmJob>& job : finished){
        prewarms.erase(std::remove(prewarms.begin(), prewarms.end(), job), prewarms.end());
        // (its function goes with the call: a Lua one is not kept past the job)
        if (job->onDone){
            std::function<void()> onDone = std::move(job->onDone);
            job->onDone = nullptr;
            onDone();
        }
    }
}
