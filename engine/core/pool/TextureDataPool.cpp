// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureDataPool.h"

#include "Engine.h"
#include "Log.h"
#include "io/FileData.h"
#include "thread/ResourceProgress.h"
#include "io/AssetIOService.h"

#include <atomic>
#include <filesystem>
#include <vector>

using namespace jasperengine;

std::unordered_map<std::string, TextureDataPool::PendingTexture> TextureDataPool::pendingBuilds;
std::unordered_map<std::string, std::function<TextureData()>> TextureDataPool::generators;
std::mutex TextureDataPool::cacheMutex;
std::atomic<bool> TextureDataPool::shutdownRequested{false};
std::atomic<uint32_t> TextureDataPool::replaceEpoch{0};

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

bool TextureDataPool::hasTexturePixels(const std::shared_ptr<std::array<TextureData,6>>& data, size_t numFaces) {
    if (!data) {
        return false;
    }

    for (size_t f = 0; f < numFaces; f++) {
        TextureData& face = data->at(f);
        if (!face.getData() || face.getSize() == 0) {
            return false;
        }
    }

    return true;
}

size_t TextureDataPool::getTextureFaces(std::array<TextureData,6>& data) {
    for (size_t f = 1; f < 6; f++) {
        if (data.at(f).getData() && data.at(f).getSize() > 0) {
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
            jobs::Task<std::array<TextureData,6>>& task = it->second.task;
            if (!task.isReady()) {
                result.state = ResourceLoadState::Loading;
                return result;
            }
            try {
                shared = std::make_shared<std::array<TextureData,6>>(task.get());
                pendingBuilds.erase(it);

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

        pendingBuilds[id] = startAsyncLoad(id, paths, numFaces, fixAlphaBorder);

    } else {
        try {
            std::array<TextureData,6> data = loadTextureInternal(id, paths, numFaces, fixAlphaBorder, false);
            shared = std::make_shared<std::array<TextureData,6>>(data);

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

void TextureDataPool::prepareFace(TextureData& face, size_t numFaces, bool fixAlphaBorder) {
    if (fixAlphaBorder) {
        face.fixAlphaBorder();
    }

    if (Engine::getTextureStrategy() == TextureStrategy::FIT) {
        face.fitPowerOfTwo();
    } else if (Engine::getTextureStrategy() == TextureStrategy::RESIZE) {
        face.resizePowerOfTwo();
    }

    // Cubemap faces must be square; resize to the larger dimension if needed
    if (numFaces == 6) {
        face.resizeToSquare();
    }
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
        build.task = jobs::submit([make]() -> std::array<TextureData, 6> {
            std::array<TextureData, 6> faces;
            faces[0] = make();
            return faces;
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
        std::array<TextureData, 6> faces = pending->second.task.get();
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
            failed.task = jobs::submit([f]() -> std::array<TextureData,6> {
                throw std::runtime_error("Texture is missing texture for face " + std::to_string(f));
            });
            return failed;
        }
    }

    // one read per file: a reader thread reads it, a Heavy Background job decodes it; the
    // faces of one texture share a token, so a cancel covers the whole load
    jobs::CancelToken token;
    auto decoded = std::make_shared<std::atomic<int>>(0);
    std::vector<jobs::Task<std::array<TextureData,6>>> faces;
    for (size_t f = 0; f < files; f++) {
        AssetIOService::ReadRequest request;
        request.path = TextureData::parseSvgScalePath(paths[f]);
        request.priority = IOPriority::Normal;
        request.gateClass = jobs::GateClass::Heavy;
        request.name = "texture-decode";
        request.cancel = token;
        const std::string path = paths[f];
        request.onFailure = [buildId, path](const std::string& error) {
            ResourceProgress::failBuild(buildId);
            if (!AssetIOService::isCancellation(error))
                Log::error("Failed to load texture from file: %s (%s)", path.c_str(), error.c_str());
        };
        faces.push_back(AssetIOService::instance().submitRead(request,
            [path, f, files, numFaces, fixAlphaBorder, isSingleFileCube, buildId, decoded](std::vector<uint8_t>&& bytes) {
                std::array<TextureData,6> data;
                if (shutdownRequested.load()) {
                    ResourceProgress::failBuild(buildId);
                    throw std::runtime_error("Shutdown requested");
                }
                if (isSingleFileCube) {
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
                    prepareFace(data[f], numFaces, fixAlphaBorder);
                }
                const int done = decoded->fetch_add(1) + 1;
                ResourceProgress::updateProgress(buildId, 0.9f * (float)done / (float)files);
                return data;
            }));
    }

    // the faces' results are combined (and a cubemap validated) once all are decoded
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "texture-combine";
    options.tags = jobs::Tag::Asset;
    options.cancel = &token;
    std::vector<jobs::TaskRef> deps(faces.begin(), faces.end());
    PendingTexture pending;
    pending.cancel = token;
    pending.task = jobs::submit([faces, files, numFaces, buildId]() {
        std::array<TextureData,6> data;
        for (size_t f = 0; f < files; f++) {
            const std::array<TextureData,6>& face = faces[f].get();
            if (files == 1)
                data = face;
            else
                data[f] = face[f];
        }
        std::string validationError = validateTextureFaces(data, numFaces);
        if (!validationError.empty()) {
            ResourceProgress::failBuild(buildId);
            Log::error("%s", validationError.c_str());
            throw std::runtime_error(validationError);
        }
        ResourceProgress::updateProgress(buildId, 1.0f);
        ResourceProgress::completeBuild(buildId);
        return data;
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

        prepareFace(data[f], numFaces, fixAlphaBorder);

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

void TextureDataPool::quiescePending(bool cancel) {
    for (auto& [id, pending] : pendingBuilds) {
        if (cancel) {
            // the queued reads go, a decode in progress sees the token
            pending.cancel.cancel();
            if (pending.task.cancel())
                ResourceProgress::failBuild(std::hash<std::string>{}(id));
        }
        pending.task.wait();
    }
    pendingBuilds.clear();
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
        auto pendingIt = pendingBuilds.find(id);
        if (pendingIt != pendingBuilds.end()) {
            // a load not started never runs; a running one is waited for, so it cannot
            // overlap a replacement request with this id
            pendingIt->second.cancel.cancel();
            if (pendingIt->second.task.cancel()) {
                ResourceProgress::failBuild(std::hash<std::string>{}(id));
            }
            pendingIt->second.task.wait();
            pendingBuilds.erase(pendingIt);
            removedPending = true;
        }
    }

    auto& map = getMap();
    auto it = map.find(id);
    if (it != map.end()){
        if (!it->second || it->second.use_count() <= 1){
            map.erase(it);
            std::lock_guard<std::mutex> lock(cacheMutex);
            generators.erase(id);
        }
	}else if (!removedPending){
		if (Engine::isViewLoaded()){
			Log::debug("Trying to destroy a non existent texture data: %s", id.c_str());
		}
	}
}

void TextureDataPool::clear(){
    {
		std::lock_guard<std::mutex> lock(cacheMutex);
		quiescePending(false);
		generators.clear();
	}
	getMap().clear();
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
