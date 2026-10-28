// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureDataPool.h"

#include "Engine.h"
#include "Log.h"
#include "io/Data.h"
#include "io/FileData.h"
#include "thread/ResourceProgress.h"
#include "render/SystemRender.h"
#include "io/AssetIOService.h"
#include "pool/TexturePool.h"
#include "render/SystemRender.h"
#include "texture/BasisTranscode.h"
#include "texture/Ktx2.h"
#include "texture/TextureFallback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <set>
#include <vector>

using namespace jasperengine;

std::unordered_map<std::string, TextureDataPool::PendingTexture> TextureDataPool::pendingBuilds;
std::unordered_map<std::string, TextureDataPool::PendingTexture> TextureDataPool::backgroundBuilds;
std::vector<std::shared_ptr<TextureDataPool::BasisJob>> TextureDataPool::basisJobs;
std::unordered_map<std::string, std::function<TextureData()>> TextureDataPool::generators;
std::mutex TextureDataPool::cacheMutex;
std::atomic<bool> TextureDataPool::shutdownRequested{false};
std::atomic<uint32_t> TextureDataPool::replaceEpoch{0};

// A resident texture's place in the budget: its size at every drop comes from its full size
struct TextureDataPool::Resident {
    bool cooked = false;
    bool streamable = false;
    GpuTextureFormat format = GpuTextureFormat::RGBA8;
    int fullWidth = 0, fullHeight = 0;     // level 0 of the cooked file
    int logicalWidth = 0, logicalHeight = 0;
    int faces = 1;
    int fullLevels = 1;
    int loadedDrop = 0;                    // the levels the resident data skipped
    int budgetDrop = 0;                    // the budget's share of the drop
    uint64_t bytes = 0;

    uint64_t bytesAt(int drop) const{
        uint64_t total = 0;
        for (int level = drop; level < fullLevels; level++)
            total += (uint64_t)gpuFormatLevelBytes(format, mipDimension(fullWidth, level), mipDimension(fullHeight, level)) * (uint64_t)faces;
        return total;
    }
};

// A Basis transcode the main thread runs in time slices (web builds without threads): UASTC a
// band of block rows at a time, ETC1S a level at a time
struct TextureDataPool::BasisJob {
    std::string id;
    bool replacing = false;
    BasisTranscode::Session session;
    GpuTextureFormat target = GpuTextureFormat::RGBA8;
    TextureContent content = TextureContent::ColorOpaque;
    uint32_t next = 0;
    // the level `next` while its bands transcode, and the next band's first block row
    std::vector<uint8_t> current;
    uint32_t row = 0;
    std::vector<std::vector<uint8_t>> levels;
    int width = 0, height = 0, faces = 1;
    int logicalWidth = 0, logicalHeight = 0;
    bool srgb = true;
    uint32_t flags = 0;
    int dropped = 0, fullLevels = 1;
    std::shared_ptr<std::array<TextureData,6>> cpuCopy;
};

namespace {
    int poolTextureQuality = 0;
    uint64_t poolMemoryBudget = 0;
#if defined(__EMSCRIPTEN__)
    bool poolLowMemoryFallback = true;
#else
    bool poolLowMemoryFallback = false;
#endif
    bool poolBudgetDirty = false;
    // ids the budget planned a different drop for, reloaded by update()
    std::set<std::string> poolPlannedReloads;
    TextureDataPool::ProcessingProvider poolProcessingProvider;
    std::mutex poolProviderMutex;

    // the progressive tail is every level up to this size
    constexpr int kTextureTailSize = 64;

    // Main-thread Basis transcodes once the scenes have loaded (spec S14 R19, deviation 27): the
    // time a frame gives them, and the UASTC band size between two looks at the clock (a band of
    // a 2048 px level to BC7 is well under a millisecond in desktop Chrome)
    constexpr double kBasisFrameSliceMs = 4.0;
    constexpr uint32_t kBasisBandRows = 32;

    bool transcodeOnMainThread(){
        return jobs::hasSystem() && jobs::system().isSingleThreaded();
    }

    // A cooked cube's 256 px RGBA8 copy (its `cpu` file) as six faces, for IBL
    std::shared_ptr<std::array<TextureData,6>> decodeCpuCopy(const std::vector<uint8_t>& bytes, const std::string& file){
        TextureData cube;
        std::string error;
        if (!cube.loadKtx2(bytes.data(), (unsigned int)bytes.size(), 0, nullptr, false, &error) ||
            cube.getGpuFormat() != GpuTextureFormat::RGBA8 || cube.getFaceCount() != 6 || !cube.getData()){
            Log::error("Cooked texture CPU copy %s is not an RGBA8 cube: %s", file.c_str(), error.c_str());
            return nullptr;
        }
        auto faces = std::make_shared<std::array<TextureData,6>>();
        const size_t faceBytes = (size_t)cube.getWidth() * (size_t)cube.getHeight() * 4;
        const uint8_t* pixels = (const uint8_t*)cube.getData();
        for (size_t f = 0; f < 6; f++)
            (*faces)[f].setLevels(GpuTextureFormat::RGBA8, true, 1, cube.getWidth(), cube.getHeight(),
                                  {std::vector<uint8_t>(pixels + faceBytes * f, pixels + faceBytes * (f + 1))});
        return faces;
    }

    // residents are planned on the main thread; loads read their drop from any thread
    std::mutex& residentsMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }
}

bool TextureDataPool::loadCookedBytes(const std::string& id, const CookedTextureEntry& entry, const std::vector<uint8_t>& bytes, uint32_t firstLevel,
                                      bool allowTail, bool deferBasis, const TextureFormatSupport& support, bool lowMemory,
                                      CookedPiece& out, std::string& error){
    Ktx2Info info;
    const uint64_t fileSize = entry.bytes > 0 ? entry.bytes : bytes.size();
    if (!Ktx2::parseInfo(bytes, fileSize, info, error))
        return false;
    firstLevel = std::min(firstLevel, info.levelCount - 1);
    const Ktx2LevelIndex& top = info.levels[firstLevel];
    out.end = top.byteOffset + top.byteLength;
    out.firstLevel = firstLevel;
    Ktx2PrefixSource source(bytes, fileSize);
    Ktx2Image image;
    if (out.end <= bytes.size()){
        if (!Ktx2::read(source, firstLevel, image, error))
            return false;
        if (deferBasis && image.info.codec != Ktx2Codec::Raw){
            auto job = std::make_shared<TextureDataPool::BasisJob>();
            job->id = id;
            TextureUploadPlan plan = TextureFallback::plan(image.info, support, lowMemory);
            if (plan.path == TextureUploadPlan::Path::Unavailable){
                error = plan.reason;
                return false;
            }
            job->target = plan.upload;
            job->content = TextureFallback::contentOf(image.info);
            job->width = (int)image.info.levelWidth(firstLevel);
            job->height = (int)image.info.levelHeight(firstLevel);
            job->faces = (int)image.info.faces;
            job->srgb = image.info.srgb;
            job->dropped = (int)firstLevel;
            job->fullLevels = (int)image.info.levelCount;
            // the logical size and flags the TextureData would take
            int logicalW = (int)image.info.width, logicalH = (int)image.info.height;
            auto logical = image.info.keyValues.find("JE.logical");
            if (logical != image.info.keyValues.end())
                sscanf(logical->second.c_str(), "%d,%d", &logicalW, &logicalH);
            job->logicalWidth = logicalW;
            job->logicalHeight = logicalH;
            job->flags = (TextureFallback::hasFlag(image.info, "alpha") ? TEXTURE_FLAG_ALPHA : 0) |
                         (TextureFallback::hasFlag(image.info, "normalRG") ? TEXTURE_FLAG_NORMAL_RG : 0) |
                         (TextureFallback::hasFlag(image.info, "streamable") ? TEXTURE_FLAG_STREAMABLE : 0) |
                         (TextureFallback::hasFlag(image.info, "cpu") ? TEXTURE_FLAG_CPU_READABLE : 0) |
                         (image.info.faces == 6 ? TEXTURE_FLAG_CUBE : 0) | entry.flags;
            // the session takes the levels: everything of the file it needs is read above
            if (!job->session.open(std::move(image), error))
                return false;
            out.basis = job;
            out.complete = false;
            return true;
        }
        out.complete = true;
        if (!out.data.loadKtx2Image(image, false, &support, lowMemory, error))
            return false;
        // the index's flags (streamable...) are the export's word for the file
        out.data.setFlags(out.data.getFlags() | entry.flags);
        return true;
    }
    if (!allowTail){
        error = "short read of a cooked texture";
        return false;
    }
    out.complete = false;
    // the tail: the first kept level of at most 64 px whose bytes were read
    for (uint32_t level = firstLevel; level < info.levelCount; level++){
        const Ktx2LevelIndex& index = info.levels[level];
        if (std::max(info.levelWidth(level), info.levelHeight(level)) <= (uint32_t)kTextureTailSize &&
            index.byteOffset + index.byteLength <= bytes.size()){
            if (!Ktx2::read(source, level, image, error) || !out.data.loadKtx2Image(image, false, &support, lowMemory, error))
                return false;
            out.data.setFlags(out.data.getFlags() | entry.flags);
            out.hasTail = true;
            break;
        }
    }
    return true;
}

texturesdata_t& TextureDataPool::getMap(){
    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    static texturesdata_t* map = new texturesdata_t();
    return *map;
};

std::unordered_map<std::string, uint32_t>& TextureDataPool::getGenerations(){
    static auto* generations = new std::unordered_map<std::string, uint32_t>();
    return *generations;
}

std::unordered_map<std::string, TextureSource>& TextureDataPool::getSources(){
    static auto* sources = new std::unordered_map<std::string, TextureSource>();
    return *sources;
}

std::unordered_map<std::string, TextureDataPool::Resident>& TextureDataPool::getResidents(){
    static auto* residents = new std::unordered_map<std::string, Resident>();
    return *residents;
}

void TextureDataPool::replace(const std::string& id, std::shared_ptr<std::array<TextureData,6>> data){
    if (!data){
        return;
    }
    getMap()[id] = std::move(data);
    getGenerations()[id]++;
    replaceEpoch.fetch_add(1, std::memory_order_acq_rel);
}

uint32_t TextureDataPool::generation(const std::string& id){
    auto& generations = getGenerations();
    auto it = generations.find(id);
    return it == generations.end() ? 0 : it->second;
}

bool TextureDataPool::getSource(const std::string& id, TextureSource& out){
    auto& sources = getSources();
    auto it = sources.find(id);
    if (it == sources.end()){
        return false;
    }
    out = it->second;
    return true;
}

std::vector<std::string> TextureDataPool::findIdsUsingFile(const std::string& systemPath){
    const std::string wanted = std::filesystem::path(systemPath).lexically_normal().generic_string();
    std::vector<std::string> ids;
    for (const auto& [id, source] : getSources()){
        for (size_t f = 0; f < 6; f++){
            if (source.paths[f].empty()){
                continue;
            }
            const std::string file = TextureData::parseSvgScalePath(source.paths[f], nullptr);
            if (std::filesystem::path(FileData::getSystemPath(file)).lexically_normal().generic_string() == wanted){
                ids.push_back(id);
                break;
            }
        }
    }
    return ids;
}

std::array<TextureData,6> TextureDataPool::decode(const std::string& id, const TextureSource& source){
    return loadTextureInternal(id, source.paths, source.numFaces, source.fixAlphaBorder, false);
}

std::shared_ptr<std::array<TextureData,6>> TextureDataPool::shareDecoded(std::array<TextureData,6> data){
    // The faces (one pixel allocation each, see loadCubeMapFromSingleFile) go through the render
    // queue's cleanup like the release after an upload: queued after it, they are released (a
    // no-op when that release ran) and deleted once an upload queued from a loading thread and
    // its own release have run; on the main thread at once
    return std::shared_ptr<std::array<TextureData,6>>(new std::array<TextureData,6>(std::move(data)),
        [](std::array<TextureData,6>* faces){
            SystemRender::scheduleCleanup([](void* pointer){
                auto* decoded = static_cast<std::array<TextureData,6>*>(pointer);
                for (TextureData& face : *decoded){
                    face.releaseImageData();
                }
                delete decoded;
            }, faces);
        });
}

bool TextureDataPool::hasTexturePixels(const std::shared_ptr<std::array<TextureData,6>>& data, size_t numFaces) {
    if (!data) {
        return false;
    }

    // levels with every face (a cooked or prepared cube) sit in the first entry
    if (data->at(0).hasLevels() && data->at(0).getFaceCount() == (int)numFaces) {
        return true;
    }

    for (size_t f = 0; f < numFaces; f++) {
        if (!data->at(f).hasPixels()) {
            return false;
        }
    }

    return true;
}

size_t TextureDataPool::getTextureFaces(std::array<TextureData,6>& data) {
    if (data.at(0).hasLevels() && data.at(0).getFaceCount() == 6) {
        return 6;
    }
    for (size_t f = 1; f < 6; f++) {
        if (data.at(f).hasPixels()) {
            return 6;
        }
    }

    return 1;
}

std::shared_ptr<std::array<TextureData,6>> TextureDataPool::get(const std::string& id){
    auto& map = getMap();
    auto it = map.find(id);

    if (it != map.end() && it->second){
        return it->second;
    }

    return nullptr;
}

void TextureDataPool::put(const std::string& id, std::shared_ptr<std::array<TextureData,6>> data){
    if (!data){
        return;
    }

    auto& map = getMap();
    auto it = map.find(id);
    if (it != map.end() && hasTexturePixels(it->second, getTextureFaces(*it->second))){
        return; // already cached with pixels; keep the existing entry
    }

    map[id] = std::move(data);
}

std::shared_ptr<std::array<TextureData,6>> TextureDataPool::get(const std::string& id, std::array<TextureData,6> data){
    auto& map = getMap();
    auto it = map.find(id);
    std::shared_ptr<std::array<TextureData,6>> shared = (it != map.end()) ? it->second : nullptr;
    size_t numFaces = getTextureFaces(data);

    if (shared && hasTexturePixels(shared, numFaces)){
		return shared;
	}

	shared = std::make_shared<std::array<TextureData,6>>(data);
    map[id] = shared;

	return shared;
}

void TextureDataPool::setProcessingProvider(ProcessingProvider provider){
    std::lock_guard<std::mutex> lock(poolProviderMutex);
    poolProcessingProvider = std::move(provider);
}

bool TextureDataPool::findCooked(const std::array<std::string, 6>& paths, size_t numFaces, std::vector<CookedTextureEntry>& entries){
    entries.clear();
    bool singleFile = !paths[0].empty();
    for (size_t f = 1; f < 6 && singleFile; f++)
        singleFile = paths[f].empty();
    const size_t files = (numFaces == 6 && !singleFile) ? 6 : 1;
    for (size_t f = 0; f < files; f++){
        CookedTextureEntry entry;
        if (paths[f].empty() || TextureData::hasSvgExtension(TextureData::parseSvgScalePath(paths[f]).c_str()) ||
            !CookedTextures::find(paths[f], entry))
            return false;
        // a single-file cube is cooked as a cube; six files as six 2D faces
        if ((singleFile && numFaces == 6) != (entry.faces == 6))
            return false;
        entries.push_back(entry);
    }
    return !entries.empty();
}

int TextureDataPool::levelDropFor(const std::string& id, const CookedTextureEntry& entry){
    if (!(entry.flags & TEXTURE_FLAG_STREAMABLE) || entry.levels <= 1)
        return 0;
    int drop = std::clamp(poolTextureQuality, 0, 3);
    std::lock_guard<std::mutex> lock(residentsMutex());
    auto it = getResidents().find(id);
    if (it != getResidents().end())
        drop += it->second.budgetDrop;
    return std::clamp(drop, 0, entry.levels - 1);
}

void TextureDataPool::prepareLevels(std::array<TextureData,6>& data, size_t numFaces, const std::string& path, bool fixAlphaBorder){
    TextureProcessing processing = TextureProcessing::defaultFor(data[0], fixAlphaBorder, numFaces == 6);
    {
        std::lock_guard<std::mutex> lock(poolProviderMutex);
        TextureProcessing configured;
        if (poolProcessingProvider && poolProcessingProvider(TextureData::parseSvgScalePath(path), configured)){
            configured.alphaIsTransparency = configured.alphaIsTransparency || fixAlphaBorder;
            configured.cube = numFaces == 6;
            processing = configured;
        }
    }
    if (Engine::getTextureStrategy() == TextureStrategy::FIT)
        processing.strategy = TextureSizeStrategy::Fit;
    else if (Engine::getTextureStrategy() == TextureStrategy::RESIZE)
        processing.strategy = TextureSizeStrategy::Resize;

    std::vector<const TextureData*> faces;
    for (size_t f = 0; f < numFaces; f++)
        faces.push_back(&data[f]);
    PreparedTexture prepared;
    std::vector<std::vector<uint8_t>> levels;
    std::string error;
    if (!TexturePrepare::prepare(faces, processing, prepared, error) ||
        !TexturePrepare::toUncompressed(prepared, TexturePrepare::uncompressedFormat(prepared.layout), levels, error)){
        throw std::runtime_error("Texture " + path + ": " + error);
    }

    // CPU readers of a float or normal cube (IBL) read a 256 px RGBA8 copy
    std::shared_ptr<std::array<TextureData,6>> cpuCopy;
    if (numFaces == 6 && prepared.layout != PreparedTexture::Layout::RGBA8){
        cpuCopy = std::make_shared<std::array<TextureData,6>>();
        int level = 0;
        while (level + 1 < prepared.levelCount() && std::max(prepared.faces[0][level].width, prepared.faces[0][level].height) > 256)
            level++;
        PreparedTexture small = prepared;
        for (auto& face : small.faces){
            face.erase(face.begin(), face.begin() + level);
            face.resize(1);
        }
        small.width = small.faces[0][0].width;
        small.height = small.faces[0][0].height;
        for (size_t f = 0; f < 6; f++){
            const MipLevel& m = small.faces[f][0];
            std::vector<uint8_t> rgba((size_t)m.width * (size_t)m.height * 4);
            const size_t n = (size_t)m.width * (size_t)m.height;
            for (size_t i = 0; i < n; i++){
                if (prepared.layout == PreparedTexture::Layout::RGBAF){
                    const float* v = reinterpret_cast<const float*>(m.pixels.data()) + i * 4;
                    for (int c = 0; c < 4; c++)
                        rgba[i * 4 + c] = (uint8_t)std::clamp((int)std::lround(v[c] * 255.0f), 0, 255);
                }else if (prepared.layout == PreparedTexture::Layout::R8 || prepared.layout == PreparedTexture::Layout::R16){
                    const uint8_t v = prepared.layout == PreparedTexture::Layout::R8 ? m.pixels[i] : m.pixels[i * 2 + 1];
                    rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = v;
                    rgba[i * 4 + 3] = 255;
                }else{
                    rgba[i * 4 + 0] = m.pixels[i * 2];
                    rgba[i * 4 + 1] = m.pixels[i * 2 + 1];
                    rgba[i * 4 + 2] = 255;
                    rgba[i * 4 + 3] = 255;
                }
            }
            (*cpuCopy)[f].setLevels(GpuTextureFormat::RGBA8, true, 1, m.width, m.height, {std::move(rgba)});
        }
    }

    for (size_t f = 0; f < 6; f++){
        data[f].releaseImageData();
        data[f] = TextureData();
    }
    TextureData& out = data[0];
    out.setLevels(TexturePrepare::uncompressedFormat(prepared.layout), prepared.srgb, (int)numFaces, prepared.width, prepared.height, std::move(levels));
    out.setLogicalSize(prepared.logicalWidth, prepared.logicalHeight);
    uint32_t flags = (prepared.hasAlpha ? TEXTURE_FLAG_ALPHA : 0) | (numFaces == 6 ? TEXTURE_FLAG_CUBE : 0) |
                     (prepared.layout == PreparedTexture::Layout::RG8 ? TEXTURE_FLAG_NORMAL_RG : 0) |
                     (prepared.layout == PreparedTexture::Layout::RGBAF ? TEXTURE_FLAG_DISPLAY_ENCODED : 0);
    out.setFlags(flags);
    out.setDroppedLevels(0, out.getLevels().empty() ? 1 : (int)out.getLevels().size());
    if (cpuCopy)
        out.setCpuCopy(cpuCopy);
}

TextureLoadResult TextureDataPool::loadFromFile(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool fixAlphaBorder) {
    auto& shared = getMap()[id];

    TextureLoadResult result;
    result.id = id;

    // what a hot reload decodes again
    getSources()[id] = TextureSource{paths, numFaces, fixAlphaBorder};

    if (shared.use_count() > 0 && hasTexturePixels(shared, numFaces)) {
        result.state = ResourceLoadState::Finished;
        result.data = shared;
        return result;
    }

    // The pool entry may still exist while other Texture instances hold the old
    // shared_ptr, even after releaseDataAfterLoad freed the pixel buffer. In
    // that case, keep the stale object alive for existing holders but replace
    // the cached map entry with a freshly loaded copy.

    if (Engine::isAsyncLoading()) {
        std::lock_guard<std::mutex> lock(cacheMutex);

        if (shutdownRequested.load()) {
            throw std::runtime_error("Shutdown requested");
        }

        auto it = pendingBuilds.find(id);
        if (it != pendingBuilds.end()) {
            jobs::Task<LoadResult>& task = it->second.task;
            if (!task.isReady()) {
                result.state = ResourceLoadState::Loading;
                return result;
            }
            try {
                LoadResult loaded = task.get();
                pendingBuilds.erase(it);
                if (!loaded.complete){
                    // the rest reads in the background; the tail (when there is one) shows now
                    const bool hasTail = loaded.data[0].hasPixels();
                    std::array<TextureData,6> tail = loaded.data;
                    if (loaded.basis){
                        loaded.basis->replacing = hasTail;
                        basisJobs.push_back(loaded.basis);
                    }else if (!loaded.rest.empty()){
                        backgroundBuilds[id] = startRestLoad(id, std::move(loaded));
                    }
                    if (!hasTail){
                        result.state = ResourceLoadState::Loading;
                        return result;
                    }
                    shared = shareDecoded(tail);
                    noteResident(id, *shared, true);
                    result.state = ResourceLoadState::Finished;
                    result.data = shared;
                    return result;
                }
                shared = shareDecoded(std::move(loaded.data));
                noteResident(id, *shared, loaded.cooked);

                result.state = ResourceLoadState::Finished;
                result.data = shared;
                return result;
            } catch (const std::exception& e) {
                pendingBuilds.erase(it);

                result.state = ResourceLoadState::Failed;
                result.errorMessage = e.what();
                return result;
            }
        }

        // a basis job of this id already waits for the main thread
        for (const auto& job : basisJobs){
            if (job->id == id && !job->replacing){
                result.state = ResourceLoadState::Loading;
                return result;
            }
        }

        pendingBuilds[id] = startAsyncLoad(id, paths, numFaces, fixAlphaBorder);

    } else {
        try {
            shared = shareDecoded(loadTextureInternal(id, paths, numFaces, fixAlphaBorder, false));
            std::vector<CookedTextureEntry> entries;
            noteResident(id, *shared, findCooked(paths, numFaces, entries));

            result.state = ResourceLoadState::Finished;
            result.data = shared;
            return result;
        } catch (const std::exception& e) {
            result.state = ResourceLoadState::Failed;
            result.errorMessage = e.what();
            return result;
        }
    }

    result.state = ResourceLoadState::Loading;
    return result;
}

void TextureDataPool::setGenerator(const std::string& id, std::function<TextureData()> generator) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    generators[id] = std::move(generator);
}

TextureLoadResult TextureDataPool::loadGenerated(const std::string& id) {
    TextureLoadResult result;
    result.id = id;

    std::lock_guard<std::mutex> lock(cacheMutex);
    auto generator = generators.find(id);
    if (generator == generators.end())
        return result;

    auto& shared = getMap()[id];
    if (shared.use_count() > 0 && hasTexturePixels(shared, 1)) {
        result.state = ResourceLoadState::Finished;
        result.data = shared;
        return result;
    }
    if (shutdownRequested.load()) {
        result.state = ResourceLoadState::Failed;
        result.errorMessage = "Shutdown requested";
        return result;
    }

    auto pending = pendingBuilds.find(id);
    if (pending == pendingBuilds.end()) {
        PendingTexture build;
        std::function<TextureData()> make = generator->second;
        build.task = jobs::submit([make]() -> LoadResult {
            LoadResult made;
            made.data[0] = make();
            return made;
        }, {jobs::Lane::Background, jobs::GateClass::Heavy, "texture-generate", jobs::Tag::Asset});
        pendingBuilds[id] = std::move(build);
        result.state = ResourceLoadState::Loading;
        return result;
    }
    if (!pending->second.task.isReady()) {
        result.state = ResourceLoadState::Loading;
        return result;
    }
    try {
        std::array<TextureData, 6> faces = pending->second.task.get().data;
        pendingBuilds.erase(pending);
        if (!faces[0].getData() || faces[0].getSize() == 0) {
            result.state = ResourceLoadState::Failed;
            result.errorMessage = "No pixels were made for " + id;
            return result;
        }
        shared = std::make_shared<std::array<TextureData, 6>>(faces);
        // the pool's copy owns the pixels the generator handed over
        shared->at(0).setDataOwned(true);
        result.state = ResourceLoadState::Finished;
        result.data = shared;
    } catch (const std::exception& e) {
        pendingBuilds.erase(pending);
        result.state = ResourceLoadState::Failed;
        result.errorMessage = e.what();
    }
    return result;
}

TextureDataPool::PendingTexture TextureDataPool::startAsyncLoad(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool fixAlphaBorder) {
    const std::string textureName = std::filesystem::path(paths[0].empty() ? id : paths[0]).filename().string();
    const uint64_t buildId = std::hash<std::string>{}(id);

    ResourceProgress::startBuild(buildId, ResourceType::Texture, textureName);

    bool isSingleFileCube = !paths[0].empty();
    for (int f = 1; f < 6 && isSingleFileCube; f++){
        if (!paths[f].empty())
            isSingleFileCube = false;
    }
    isSingleFileCube = isSingleFileCube && numFaces == 6;
    const size_t files = (numFaces == 6 && !isSingleFileCube) ? 6 : 1;

    for (size_t f = 0; f < files; f++) {
        if (paths[f].empty()) {
            ResourceProgress::failBuild(buildId);
            Log::error("Texture is missing texture for face %zu", f);
            PendingTexture failed;
            failed.task = jobs::submit([f]() -> LoadResult {
                throw std::runtime_error("Texture is missing texture for face " + std::to_string(f));
            });
            return failed;
        }
    }

    // a cooked file reads its first bytes: the metadata and the levels <= 64 px (all of it when
    // that is the whole file); the device's formats are read here, on the main thread
    std::vector<CookedTextureEntry> cooked;
    const bool isCooked = findCooked(paths, numFaces, cooked);
    const TextureFormatSupport support = SystemRender::getTextureFormatSupport();
    const bool lowMemory = poolLowMemoryFallback;
    const bool deferBasis = transcodeOnMainThread();
    std::vector<uint32_t> firstLevels(files, 0);
    if (isCooked){
        for (size_t f = 0; f < files; f++)
            firstLevels[f] = (uint32_t)levelDropFor(id, cooked[f]);
    }

    // one read per file: a reader thread reads it, a Heavy Background job decodes it; the
    // faces of one texture share a token, so a cancel covers the whole load
    jobs::CancelToken token;
    auto decoded = std::make_shared<std::atomic<int>>(0);
    std::vector<jobs::Task<LoadResult>> faces;
    for (size_t f = 0; f < files; f++) {
        AssetIOService::ReadRequest request;
        request.priority = IOPriority::Normal;
        request.gateClass = jobs::GateClass::Heavy;
        request.name = "texture-decode";
        request.cancel = token;
        const std::string path = paths[f];
        CookedTextureEntry entry;
        if (isCooked){
            entry = cooked[f];
            request.path = entry.file;
            if (entry.tailBytes > 0 && entry.tailBytes < entry.bytes)
                request.size = (uint32_t)entry.tailBytes;
        }else{
            request.path = TextureData::parseSvgScalePath(paths[f]);
        }
        request.onFailure = [buildId, path](const std::string& error) {
            ResourceProgress::failBuild(buildId);
            if (!AssetIOService::isCancellation(error))
                Log::error("Failed to load texture from file: %s (%s)", path.c_str(), error.c_str());
        };
        const uint32_t firstLevel = firstLevels[f];
        faces.push_back(AssetIOService::instance().submitRead(request,
            [id, path, f, files, numFaces, fixAlphaBorder, isSingleFileCube, buildId, decoded, isCooked, entry, firstLevel, support, lowMemory, deferBasis](std::vector<uint8_t>&& bytes) {
                LoadResult result;
                std::array<TextureData,6>& data = result.data;
                if (shutdownRequested.load()) {
                    ResourceProgress::failBuild(buildId);
                    throw std::runtime_error("Shutdown requested");
                }
                if (isCooked) {
                    result.cooked = true;
                    CookedPiece piece;
                    std::string error;
                    // a cube of six files has no progressive tail nor main-thread transcode
                    const bool single = files == 1;
                    auto shared = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
                    if (!loadCookedBytes(id, entry, *shared, firstLevel, true, deferBasis && single, support, lowMemory, piece, error)) {
                        ResourceProgress::failBuild(buildId);
                        Log::error("Failed to load cooked texture %s for %s: %s", entry.file.c_str(), path.c_str(), error.c_str());
                        throw std::runtime_error("Failed to load cooked texture for " + path + ": " + error);
                    }
                    data[f] = piece.data;
                    if (!piece.complete) {
                        result.complete = false;
                        if (piece.basis) {
                            result.basis = piece.basis;
                        } else {
                            LoadResult::Rest rest;
                            rest.face = f;
                            rest.entry = entry;
                            rest.prefix = shared;
                            rest.end = piece.end;
                            rest.firstLevel = piece.firstLevel;
                            rest.deferBasis = deferBasis && single;
                            result.rest.push_back(rest);
                        }
                    }
                } else if (isSingleFileCube) {
                    if (!TextureData::loadCubeMapFromSingleFileData(path.c_str(), bytes.data(), (unsigned int)bytes.size(), data)) {
                        ResourceProgress::failBuild(buildId);
                        Log::error("Failed to load cube texture from file: %s", path.c_str());
                        throw std::runtime_error("Failed to load cube texture from file: " + path);
                    }
                } else {
                    if (!data[f].loadTextureFromFileData(path.c_str(), bytes.data(), (unsigned int)bytes.size())) {
                        ResourceProgress::failBuild(buildId);
                        if (numFaces == 1){
                            Log::error("Failed to load texture from file: %s", path.c_str());
                            throw std::runtime_error("Failed to load texture from file: " + path);
                        }
                        Log::error("Failed to load texture face %zu from file: %s", f, path.c_str());
                        throw std::runtime_error("Failed to load texture face " + std::to_string(f) + " from file: " + path);
                    }
                }
                const int done = decoded->fetch_add(1) + 1;
                ResourceProgress::updateProgress(buildId, 0.9f * (float)done / (float)files);
                return result;
            }));
    }

    // a cooked cube's CPU copy (IBL reads it, the GPU data is compressed or float)
    jobs::Task<std::shared_ptr<std::array<TextureData,6>>> cpuCopyTask;
    const bool hasCpuCopy = isCooked && files == 1 && !cooked[0].cpuFile.empty();
    if (hasCpuCopy){
        AssetIOService::ReadRequest request;
        request.path = cooked[0].cpuFile;
        request.priority = IOPriority::Normal;
        request.gateClass = jobs::GateClass::Heavy;
        request.name = "texture-cpu-copy";
        request.cancel = token;
        const std::string file = cooked[0].cpuFile;
        request.onFailure = [file](const std::string& error){
            if (!AssetIOService::isCancellation(error))
                Log::error("Failed to read cooked texture CPU copy %s (%s)", file.c_str(), error.c_str());
        };
        cpuCopyTask = AssetIOService::instance().submitRead(request, [file](std::vector<uint8_t>&& bytes){
            return decodeCpuCopy(bytes, file);
        });
    }

    // the faces' results are combined (and a cubemap validated and prepared) once all are decoded
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.gate = jobs::GateClass::Heavy;
    options.name = "texture-combine";
    options.tags = jobs::Tag::Asset;
    options.cancel = &token;
    std::vector<jobs::TaskRef> deps(faces.begin(), faces.end());
    if (hasCpuCopy)
        deps.push_back(cpuCopyTask);
    PendingTexture pending;
    pending.cancel = token;
    const std::string sourcePath = paths[0];
    pending.task = jobs::submit([faces, files, numFaces, buildId, isCooked, sourcePath, fixAlphaBorder, hasCpuCopy, cpuCopyTask]() {
        LoadResult combined;
        combined.cooked = isCooked;
        for (size_t f = 0; f < files; f++) {
            const LoadResult& face = faces[f].get();
            if (files == 1)
                combined.data = face.data;
            else
                combined.data[f] = face.data[f];
            combined.complete = combined.complete && face.complete;
            combined.rest.insert(combined.rest.end(), face.rest.begin(), face.rest.end());
            if (face.basis)
                combined.basis = face.basis;
        }
        if (!isCooked) {
            std::string validationError = validateTextureFaces(combined.data, numFaces);
            if (!validationError.empty()) {
                ResourceProgress::failBuild(buildId);
                Log::error("%s", validationError.c_str());
                throw std::runtime_error(validationError);
            }
            prepareLevels(combined.data, numFaces, sourcePath, fixAlphaBorder);
        } else if (!combined.complete && files > 1) {
            // a six-file cube shows when every face is in
            for (size_t f = 0; f < 6; f++)
                combined.data[f] = TextureData();
        }
        if (hasCpuCopy) {
            std::shared_ptr<std::array<TextureData,6>> copy = cpuCopyTask.get();
            combined.data[0].setCpuCopy(copy);
            if (combined.basis)
                combined.basis->cpuCopy = copy;
        }
        ResourceProgress::updateProgress(buildId, 1.0f);
        ResourceProgress::completeBuild(buildId);
        return combined;
    }, options, deps);
    return pending;
}

TextureDataPool::PendingTexture TextureDataPool::startRestLoad(const std::string& id, LoadResult&& partial){
    jobs::CancelToken token;
    const TextureFormatSupport support = SystemRender::getTextureFormatSupport();
    const bool lowMemory = poolLowMemoryFallback;
    std::vector<jobs::Task<LoadResult>> reads;
    for (const LoadResult::Rest& rest : partial.rest){
        AssetIOService::ReadRequest request;
        request.path = rest.entry.file;
        request.offset = rest.prefix->size();
        request.size = (uint32_t)(rest.end - rest.prefix->size());
        request.priority = IOPriority::Low;
        request.gateClass = jobs::GateClass::Heavy;
        request.name = "texture-mips";
        request.cancel = token;
        const std::string file = rest.entry.file;
        request.onFailure = [file](const std::string& error){
            if (!AssetIOService::isCancellation(error))
                Log::error("Failed to read the mips of cooked texture %s (%s)", file.c_str(), error.c_str());
        };
        reads.push_back(AssetIOService::instance().submitRead(request, [id, rest, support, lowMemory](std::vector<uint8_t>&& bytes){
            std::vector<uint8_t> whole = *rest.prefix;
            whole.insert(whole.end(), bytes.begin(), bytes.end());
            CookedPiece piece;
            std::string error;
            if (!loadCookedBytes(id, rest.entry, whole, rest.firstLevel, false, rest.deferBasis, support, lowMemory, piece, error))
                throw std::runtime_error("Failed to load cooked texture " + rest.entry.file + ": " + error);
            LoadResult result;
            result.cooked = true;
            result.data[rest.face] = piece.data;
            // web without threads: the levels transcode on the main thread, in time slices
            result.basis = piece.basis;
            result.complete = !piece.basis;
            return result;
        }));
    }
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "texture-mips-combine";
    options.tags = jobs::Tag::Asset;
    options.cancel = &token;
    std::vector<jobs::TaskRef> deps(reads.begin(), reads.end());
    std::vector<size_t> restFaces;
    for (const LoadResult::Rest& rest : partial.rest)
        restFaces.push_back(rest.face);
    auto base = std::make_shared<std::array<TextureData,6>>(std::move(partial.data));
    PendingTexture pending;
    pending.cancel = token;
    pending.task = jobs::submit([reads, restFaces, base]() {
        LoadResult combined;
        combined.cooked = true;
        combined.data = *base;
        const std::shared_ptr<std::array<TextureData,6>> cpuCopy = base->at(0).getCpuCopy();
        for (size_t i = 0; i < reads.size(); i++){
            const LoadResult& read = reads[i].get();
            combined.data[restFaces[i]] = read.data[restFaces[i]];
            if (read.basis){
                combined.basis = read.basis;
                combined.basis->cpuCopy = cpuCopy;
                combined.complete = false;
            }
        }
        if (cpuCopy)
            combined.data[0].setCpuCopy(cpuCopy);
        return combined;
    }, options, deps);
    return pending;
}

std::array<TextureData,6> TextureDataPool::loadTextureInternal(const std::string& id, const std::array<std::string, 6>& paths, size_t numFaces, bool fixAlphaBorder, bool trackProgress) {
    uint64_t buildId = std::hash<std::string>{}(id);

    if (trackProgress) {
        if (shutdownRequested.load()) {
            throw std::runtime_error("Shutdown requested");
        }

        // Calculate starting progress based on number of faces
        // Single face: start at 0.5, Multiple faces: start proportionally
        float startProgress = numFaces == 1 ? 0.5f : (1.0f / (numFaces * 2.0f));
        ResourceProgress::updateProgress(buildId, startProgress);
    }

    std::array<TextureData,6> data;

    // cooked: each file read whole, kept from its drop down
    std::vector<CookedTextureEntry> cooked;
    if (findCooked(paths, numFaces, cooked)) {
        const TextureFormatSupport support = SystemRender::getTextureFormatSupport();
        for (size_t f = 0; f < cooked.size(); f++) {
            Data file;
            if (file.open(cooked[f].file.c_str()) != FileErrors::FILEDATA_OK) {
                throw std::runtime_error("Cooked texture not found: " + cooked[f].file);
            }
            std::vector<uint8_t> bytes(file.getMemPtr(), file.getMemPtr() + file.length());
            CookedTextureEntry entry = cooked[f];
            entry.bytes = bytes.size();
            CookedPiece piece;
            std::string error;
            if (!loadCookedBytes(id, entry, bytes, (uint32_t)levelDropFor(id, entry), false, false, support, poolLowMemoryFallback, piece, error)) {
                Log::error("Failed to load cooked texture %s: %s", cooked[f].file.c_str(), error.c_str());
                throw std::runtime_error("Failed to load cooked texture " + cooked[f].file + ": " + error);
            }
            data[f] = piece.data;
        }
        if (cooked.size() == 1 && !cooked[0].cpuFile.empty()) {
            Data file;
            if (file.open(cooked[0].cpuFile.c_str()) == FileErrors::FILEDATA_OK) {
                std::vector<uint8_t> bytes(file.getMemPtr(), file.getMemPtr() + file.length());
                data[0].setCpuCopy(decodeCpuCopy(bytes, cooked[0].cpuFile));
            }
        }
        if (trackProgress) {
            ResourceProgress::updateProgress(buildId, 1.0f);
            ResourceProgress::completeBuild(buildId);
        }
        return data;
    }

    bool isSingleFileCube = (!paths[0].empty());
    for (int f = 1; f < 6 && isSingleFileCube; f++){
        if (!paths[f].empty())
            isSingleFileCube = false;
    }

    if (numFaces == 6 && isSingleFileCube) {
        if (!TextureData::loadCubeMapFromSingleFile(paths[0].c_str(), data)){
            if (trackProgress) {
                ResourceProgress::failBuild(buildId);
            }
            Log::error("Failed to load cube texture from file: %s", paths[0].c_str());
            throw std::runtime_error("Failed to load cube texture from file: " + paths[0]);
        }

        std::string validationError = validateTextureFaces(data, numFaces);
        if (!validationError.empty()) {
            if (trackProgress) {
                ResourceProgress::failBuild(buildId);
            }
            Log::error("%s in cubemap: %s", validationError.c_str(), paths[0].c_str());
            throw std::runtime_error(validationError + " in cubemap: " + paths[0]);
        }

        prepareLevels(data, numFaces, paths[0], fixAlphaBorder);

        if (trackProgress) {
            if (shutdownRequested.load()) {
                throw std::runtime_error("Shutdown requested");
            }
            ResourceProgress::updateProgress(buildId, 1.0f);
            ResourceProgress::completeBuild(buildId);
        }

        return data;
    }

    for (size_t f = 0; f < numFaces; f++) {
        if (paths[f].empty()) {
            if (trackProgress) {
                ResourceProgress::failBuild(buildId);
            }
            Log::error("Texture is missing texture for face %zu", f);
            throw std::runtime_error("Texture is missing texture for face " + std::to_string(f));
        }

        bool success = data[f].loadTextureFromFile(paths[f].c_str());
        if (!success) {
            if (trackProgress) {
                ResourceProgress::failBuild(buildId);
            }
            if (numFaces == 1){
                Log::error("Failed to load texture from file: %s", paths[f].c_str());
                throw std::runtime_error("Failed to load texture from file: " + paths[f]);
            }else{
                Log::error("Failed to load texture face %zu from file: %s", f, paths[f].c_str());
                throw std::runtime_error("Failed to load texture face " + std::to_string(f) + " from file: " + paths[f]);
            }
        }

        if (trackProgress) {
            if (shutdownRequested.load()) {
                throw std::runtime_error("Shutdown requested");
            }

            float faceProgress;
            if (numFaces == 1) {
                // For single face: go from 0.5 to 0.95
                faceProgress = 0.5f + (0.45f * (f + 1) / static_cast<float>(numFaces));
            } else {
                // For multiple faces: use more granular progress
                float startProgress = 1.0f / (numFaces * 2.0f);
                float workingRange = 0.9f - startProgress;
                faceProgress = startProgress + (workingRange * (f + 1) / static_cast<float>(numFaces));
            }

            ResourceProgress::updateProgress(buildId, faceProgress);
        }
    }

    std::string validationError = validateTextureFaces(data, numFaces);
    if (!validationError.empty()) {
        if (trackProgress) {
            ResourceProgress::failBuild(buildId);
        }
        Log::error("%s", validationError.c_str());
        throw std::runtime_error(validationError);
    }

    // a source .ktx2 already holds its levels
    if (!data[0].hasLevels()) {
        prepareLevels(data, numFaces, paths[0], fixAlphaBorder);
    }

    if (trackProgress) {
        if (shutdownRequested.load()) {
            throw std::runtime_error("Shutdown requested");
        }
        ResourceProgress::updateProgress(buildId, 1.0f);
        ResourceProgress::completeBuild(buildId);
    }

    return data;
}

std::string TextureDataPool::validateTextureFaces(std::array<TextureData,6>& data, size_t numFaces) {
    if (numFaces != 6) {
        return "";
    }

    const int expectedWidth = data[0].getWidth();
    const int expectedHeight = data[0].getHeight();
    const int expectedChannels = data[0].getChannels();
    const ColorFormat expectedFormat = data[0].getColorFormat();

    if (expectedWidth <= 0 || expectedHeight <= 0) {
        return "Invalid cubemap face 0: empty texture data";
    }

    if (expectedWidth != expectedHeight) {
        return "Invalid cubemap face 0: cubemap faces must be square, got "
            + std::to_string(expectedWidth) + "x" + std::to_string(expectedHeight);
    }

    for (size_t f = 1; f < numFaces; f++) {
        const int width = data[f].getWidth();
        const int height = data[f].getHeight();

        if (width <= 0 || height <= 0) {
            return "Invalid cubemap face " + std::to_string(f) + ": empty texture data";
        }

        if (width != height) {
            return "Invalid cubemap face " + std::to_string(f)
                + ": cubemap faces must be square, got "
                + std::to_string(width) + "x" + std::to_string(height);
        }

        if (width != expectedWidth || height != expectedHeight) {
            return "Invalid cubemap face " + std::to_string(f)
                + ": all cubemap faces must share the same dimensions, expected "
                + std::to_string(expectedWidth) + "x" + std::to_string(expectedHeight)
                + ", got " + std::to_string(width) + "x" + std::to_string(height);
        }

        if (data[f].getChannels() != expectedChannels || data[f].getColorFormat() != expectedFormat) {
            return "Invalid cubemap face " + std::to_string(f)
                + ": all cubemap faces must share the same pixel format";
        }
    }

    return "";
}

void TextureDataPool::noteResident(const std::string& id, const std::array<TextureData,6>& data, bool cooked){
    TextureData& first = const_cast<TextureData&>(data[0]);
    std::lock_guard<std::mutex> lock(residentsMutex());
    Resident& resident = getResidents()[id];
    const int budgetDrop = resident.budgetDrop;
    resident = Resident();
    resident.cooked = cooked;
    resident.budgetDrop = budgetDrop;
    resident.streamable = cooked && (first.getFlags() & TEXTURE_FLAG_STREAMABLE) != 0;
    resident.format = first.getGpuFormat();
    resident.faces = first.hasLevels() ? first.getFaceCount() : 1;
    resident.loadedDrop = first.getDroppedLevels();
    resident.fullLevels = std::max(1, first.getFullLevelCount());
    resident.fullWidth = first.getWidth() << resident.loadedDrop;
    resident.fullHeight = first.getHeight() << resident.loadedDrop;
    resident.logicalWidth = first.getOriginalWidth();
    resident.logicalHeight = first.getOriginalHeight();
    if (first.hasLevels()){
        resident.bytes = first.getLevelBytes();
    }else{
        for (const TextureData& face : data)
            resident.bytes += face.getLevelBytes();
    }
    poolBudgetDirty = true;
}

void TextureDataPool::forgetResident(const std::string& id){
    std::lock_guard<std::mutex> lock(residentsMutex());
    if (getResidents().erase(id) > 0)
        poolBudgetDirty = true;
}

void TextureDataPool::rebalanceBudget(){
    std::lock_guard<std::mutex> lock(residentsMutex());
    poolBudgetDirty = false;
    auto& residents = getResidents();
    const int qualityDrop = std::clamp(poolTextureQuality, 0, 3);

    // the budget's share of each streamable texture's drop, on top of the quality's
    std::unordered_map<std::string, int> planned;
    for (auto& [id, resident] : residents){
        if (resident.streamable)
            planned[id] = poolMemoryBudget > 0 ? resident.budgetDrop : 0;
    }
    auto dropOf = [&](const Resident& r, int budget){ return std::clamp(qualityDrop + budget, 0, r.fullLevels - 1); };
    // the total as planned (resident bytes follow once the reloads finish)
    uint64_t total = 0;
    for (const auto& [id, resident] : residents)
        total += resident.streamable ? resident.bytesAt(dropOf(resident, planned[id])) : resident.bytes;

    if (poolMemoryBudget > 0){
        // over budget: one more mip off the largest streamable texture, until it fits
        while (total > poolMemoryBudget){
            std::string largest;
            uint64_t largestBytes = 0;
            for (const auto& [id, resident] : residents){
                if (!resident.streamable)
                    continue;
                const int now = dropOf(resident, planned[id]);
                const int next = dropOf(resident, planned[id] + 1);
                if (next == now || std::max(mipDimension(resident.fullWidth, next), mipDimension(resident.fullHeight, next)) < kTextureTailSize)
                    continue;
                const uint64_t bytes = resident.bytesAt(now);
                if (bytes > largestBytes || (bytes == largestBytes && id < largest)){
                    largest = id;
                    largestBytes = bytes;
                }
            }
            if (largest.empty())
                break;
            const Resident& r = residents[largest];
            total -= largestBytes - r.bytesAt(dropOf(r, planned[largest] + 1));
            planned[largest]++;
        }
        // under it: mips come back, the smallest increase first, while it still fits
        while (true){
            std::string best;
            uint64_t bestIncrease = 0;
            for (const auto& [id, resident] : residents){
                if (!resident.streamable || planned[id] == 0)
                    continue;
                const uint64_t increase = resident.bytesAt(dropOf(resident, planned[id] - 1)) - resident.bytesAt(dropOf(resident, planned[id]));
                if (total + increase <= poolMemoryBudget && (best.empty() || increase < bestIncrease || (increase == bestIncrease && id < best))){
                    best = id;
                    bestIncrease = increase;
                }
            }
            if (best.empty())
                break;
            planned[best]--;
            total += bestIncrease;
        }
    }

    for (auto& [id, resident] : residents){
        if (!resident.streamable)
            continue;
        resident.budgetDrop = planned[id];
        if (dropOf(resident, resident.budgetDrop) != resident.loadedDrop)
            poolPlannedReloads.insert(id);
        else
            poolPlannedReloads.erase(id);
    }
}

void TextureDataPool::adopt(const std::string& id, LoadResult& result, bool replacing){
    auto shared = shareDecoded(result.data);
    if (replacing || getMap().count(id)){
        replace(id, shared);
        TexturePool::invalidate(id);
    }else{
        getMap()[id] = shared;
    }
    noteResident(id, *shared, result.cooked);
}

void TextureDataPool::update(){
    std::vector<std::string> finished;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        // the rest of progressive loads, and reloads
        for (auto it = backgroundBuilds.begin(); it != backgroundBuilds.end();){
            if (!it->second.task.isReady()){
                ++it;
                continue;
            }
            const std::string id = it->first;
            try {
                LoadResult loaded = it->second.task.get();
                it = backgroundBuilds.erase(it);
                if (!loaded.complete){
                    if (loaded.basis){
                        loaded.basis->replacing = true;
                        basisJobs.push_back(loaded.basis);
                    }else if (!loaded.rest.empty()){
                        backgroundBuilds[id] = startRestLoad(id, std::move(loaded));
                    }
                    continue;
                }
                adopt(id, loaded, true);
            } catch (const std::exception& e) {
                Log::error("Texture %s: %s", id.c_str(), e.what());
                it = backgroundBuilds.erase(it);
            }
        }
    }

    // main-thread Basis transcodes: everything while the scenes load, then a slice of each frame
    if (!basisJobs.empty()){
        const bool unlimited = !Engine::areScenesLoaded();
        const auto sliceStart = std::chrono::steady_clock::now();
        auto sliceLeft = [&](){
            return unlimited || std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sliceStart).count() < kBasisFrameSliceMs;
        };
        for (size_t j = 0; j < basisJobs.size() && sliceLeft();){
            BasisJob& job = *basisJobs[j];
            std::string error;
            bool failed = false;
            while (job.next < job.session.levelCount() && sliceLeft()){
                if (job.session.transcodesInRows()){
                    if (job.row == 0)
                        job.current.assign(job.session.levelBytes(job.next, job.target), 0);
                    const uint32_t rows = std::min(kBasisBandRows, job.session.blockRows(job.next) - job.row);
                    if (!job.session.transcodeRows(job.next, job.row, rows, job.target, job.content, job.current, error)){
                        failed = true;
                        break;
                    }
                    job.row += rows;
                    if (job.row < job.session.blockRows(job.next))
                        continue;
                }else if (!job.session.transcodeLevel(job.next, job.target, job.content, job.current, error)){
                    failed = true;
                    break;
                }
                job.levels.push_back(std::move(job.current));
                job.current.clear();
                job.row = 0;
                job.next++;
            }
            if (failed){
                Log::error("Texture %s: %s", job.id.c_str(), error.c_str());
                basisJobs.erase(basisJobs.begin() + (ptrdiff_t)j);
                continue;
            }
            if (job.next >= job.session.levelCount()){
                LoadResult result;
                result.cooked = true;
                TextureData& data = result.data[0];
                data.setLevels(job.target, job.srgb, job.faces, job.width, job.height, std::move(job.levels));
                data.setLogicalSize(job.logicalWidth, job.logicalHeight);
                data.setFlags(job.flags);
                data.setDroppedLevels(job.dropped, job.fullLevels);
                if (job.cpuCopy)
                    data.setCpuCopy(job.cpuCopy);
                const std::string id = job.id;
                const bool replacing = job.replacing;
                basisJobs.erase(basisJobs.begin() + (ptrdiff_t)j);
                adopt(id, result, replacing);
                continue;
            }
            j++;
        }
    }

    if (poolBudgetDirty)
        rebalanceBudget();

    // budget and quality reloads: one load each, its result replaces the resident one
    if (!poolPlannedReloads.empty()){
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto it = poolPlannedReloads.begin(); it != poolPlannedReloads.end();){
            const std::string id = *it;
            TextureSource source;
            if (backgroundBuilds.count(id) || pendingBuilds.count(id) || !getSource(id, source)){
                ++it;
                continue;
            }
            backgroundBuilds[id] = startAsyncLoad(id, source.paths, source.numFaces, source.fixAlphaBorder);
            it = poolPlannedReloads.erase(it);
        }
    }
}

void TextureDataPool::setTextureQuality(int quality){
    quality = std::clamp(quality, 0, 3);
    if (quality == poolTextureQuality)
        return;
    poolTextureQuality = quality;
    poolBudgetDirty = true;
}

int TextureDataPool::getTextureQuality(){
    return poolTextureQuality;
}

void TextureDataPool::setMemoryBudget(uint64_t bytes){
    if (bytes == poolMemoryBudget)
        return;
    poolMemoryBudget = bytes;
    poolBudgetDirty = true;
}

uint64_t TextureDataPool::getMemoryBudget(){
    return poolMemoryBudget;
}

uint64_t TextureDataPool::getResidentBytes(){
    std::lock_guard<std::mutex> lock(residentsMutex());
    uint64_t total = 0;
    for (const auto& [id, resident] : getResidents())
        total += resident.bytes;
    return total;
}

void TextureDataPool::setLowMemoryFallback(bool enabled){
    poolLowMemoryFallback = enabled;
}

bool TextureDataPool::getLowMemoryFallback(){
    return poolLowMemoryFallback;
}

std::vector<TextureResidentInfo> TextureDataPool::getResidentTextures(){
    std::lock_guard<std::mutex> lock(residentsMutex());
    std::vector<TextureResidentInfo> out;
    for (const auto& [id, resident] : getResidents()){
        TextureResidentInfo info;
        info.id = id;
        info.cooked = resident.cooked;
        info.streamable = resident.streamable;
        info.format = resident.format;
        info.logicalWidth = resident.logicalWidth;
        info.logicalHeight = resident.logicalHeight;
        info.storedWidth = mipDimension(resident.fullWidth, resident.loadedDrop);
        info.storedHeight = mipDimension(resident.fullHeight, resident.loadedDrop);
        info.levels = resident.fullLevels - resident.loadedDrop;
        info.droppedLevels = resident.loadedDrop;
        info.bytes = resident.bytes;
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const TextureResidentInfo& a, const TextureResidentInfo& b){ return a.id < b.id; });
    return out;
}

size_t TextureDataPool::pendingWorkCount(){
    std::lock_guard<std::mutex> lock(cacheMutex);
    return backgroundBuilds.size() + basisJobs.size() + poolPlannedReloads.size();
}

void TextureDataPool::quiescePending(bool cancel) {
    for (auto* builds : {&pendingBuilds, &backgroundBuilds}){
        for (auto& [id, pending] : *builds) {
            if (cancel) {
                // the queued reads go, a decode in progress sees the token
                pending.cancel.cancel();
                if (pending.task.cancel())
                    ResourceProgress::failBuild(std::hash<std::string>{}(id));
            }
            pending.task.wait();
        }
        builds->clear();
    }
    basisJobs.clear();
}

void TextureDataPool::requestShutdown() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = true;
    quiescePending(true);
}

void TextureDataPool::cancelShutdown() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    shutdownRequested = false;
}

void TextureDataPool::remove(const std::string& id){
    bool removedPending = false;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto* builds : {&pendingBuilds, &backgroundBuilds}){
            auto pendingIt = builds->find(id);
            if (pendingIt != builds->end()) {
                // a load not started never runs; a running one is waited for, so it cannot
                // overlap a replacement request with this id
                pendingIt->second.cancel.cancel();
                if (pendingIt->second.task.cancel()) {
                    ResourceProgress::failBuild(std::hash<std::string>{}(id));
                }
                pendingIt->second.task.wait();
                builds->erase(pendingIt);
                removedPending = true;
            }
        }
        basisJobs.erase(std::remove_if(basisJobs.begin(), basisJobs.end(),
                                       [&id](const std::shared_ptr<BasisJob>& job){ return job->id == id; }), basisJobs.end());
        poolPlannedReloads.erase(id);
    }

    auto& map = getMap();
    auto it = map.find(id);
    if (it != map.end()){
        if (!it->second || it->second.use_count() <= 1){
            map.erase(it);
            forgetResident(id);
            std::lock_guard<std::mutex> lock(cacheMutex);
            generators.erase(id);
        }
	}else if (!removedPending){
		if (Engine::isViewLoaded()){
			Log::debug("Trying to destroy a non existent texture data: %s", id.c_str());
		}
	}
}

void TextureDataPool::invalidate(const std::string& id){
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto pendingIt = pendingBuilds.find(id);
        if (pendingIt != pendingBuilds.end()) {
            // as remove(): a load not started never runs, a running one is waited for
            pendingIt->second.cancel.cancel();
            if (pendingIt->second.task.cancel()) {
                ResourceProgress::failBuild(std::hash<std::string>{}(id));
            }
            pendingIt->second.task.wait();
            pendingBuilds.erase(pendingIt);
        }
    }
    getMap().erase(id);
}

void TextureDataPool::clear(){
    {
		std::lock_guard<std::mutex> lock(cacheMutex);
		quiescePending(false);
		generators.clear();
	}
	getMap().clear();
    {
        std::lock_guard<std::mutex> lock(residentsMutex());
        getResidents().clear();
    }
    poolPlannedReloads.clear();
    poolBudgetDirty = false;
}

void TextureDataPool::clearUnused(){
    // Quiesce in-flight loads before sweeping the cache. The decode workers do
    // not acquire cacheMutex, while holding it prevents a replacement load from
    // being registered under the same id during the wait.
    {
		std::lock_guard<std::mutex> lock(cacheMutex);
		quiescePending(false);
	}

	std::vector<std::string> ids;
	ids.reserve(getMap().size());
	for (const auto& [id, _] : getMap()) {
		ids.push_back(id);
	}
	for (const auto& id : ids) {
		remove(id);
	}
}
