// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelCpuData.h"

#include "Log.h"
#include "Scene.h"
#include "System.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "model/CookedModels.h"
#include "pool/ModelPool.h"
#include "subsystem/MeshSystem.h"

#include "tiny_gltf.h"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

using namespace jasperengine;

namespace {

    struct Entry {
        std::weak_ptr<tinygltf::Model> parse;
        std::string filename;
        std::string poolKey;
        bool released = false;
        uint64_t bytes = 0;
    };

    std::mutex& stateMutex(){
        static std::mutex* mutex = new std::mutex();
        return *mutex;
    }

    std::map<const tinygltf::Model*, Entry>& entries(){
        static std::map<const tinygltf::Model*, Entry>* map = new std::map<const tinygltf::Model*, Entry>();
        return *map;
    }

    // a load, a sub-clip's read and a mesh's upload never see half-read bytes
    std::recursive_mutex& bytesMutex(){
        return MeshSystem::getPooledGLTFMutex();
    }

    std::string heldBack;
    std::atomic<size_t> pendingCount{0};
    std::atomic<size_t> releasedCount{0};
    std::atomic<uint64_t> releasedBytes{0};

    // an entry whose parse is gone leaves the counts (restored ones are in neither)
    void forget(const Entry& entry){
        if (entry.released){
            releasedCount--;
            releasedBytes -= entry.bytes;
        }else if (entry.bytes == 0){
            pendingCount--;
        }
    }

    bool isFileBuffer(const tinygltf::Buffer& buffer){
        return buffer.name != MeshSystem::kSyntheticBufferName;
    }

    // the file's bytes again, in the parse's own buffers (the same parse and normalise give the
    // same buffers in the same order: a load's synthetic ones come after them)
    bool restore(tinygltf::Model& parse, const std::string& filename){
        tinygltf::Model again;
        std::string error;
        if (!MeshSystem::parseGLTFFile(filename, again, &error)){
            Log::error("Cannot read %s again for its mesh data: %s", filename.c_str(), error.c_str());
            return false;
        }
        for (size_t b = 0; b < again.buffers.size() && b < parse.buffers.size(); b++){
            if (parse.buffers[b].data.empty())
                parse.buffers[b].data = std::move(again.buffers[b].data);
        }
        return true;
    }

}

void ModelCpuData::markReleasable(const std::shared_ptr<tinygltf::Model>& parse, const std::string& filename, const std::string& poolKey){
    if (!parse || System::instance().isEditor())
        return;
    std::lock_guard<std::mutex> lock(stateMutex());
    auto [entry, added] = entries().try_emplace(parse.get());
    // (at an address of a parse that is gone, its entry is stale; a kept parse stays kept)
    if (!added && !entry->second.parse.expired())
        return;
    if (!added)
        forget(entry->second);
    entry->second = Entry();
    entry->second.parse = parse;
    entry->second.filename = filename;
    entry->second.poolKey = poolKey;
    pendingCount++;
}

void ModelCpuData::update(const std::vector<Scene*>& scenes, bool force){
    if (pendingCount.load() == 0)
        return;
    static unsigned int calls = 0;
    if (!force && ++calls % 30 != 0)
        return;

    std::lock_guard<std::recursive_mutex> bytesLock(bytesMutex());
    heldBack.clear();
    std::vector<std::pair<std::shared_ptr<tinygltf::Model>, Entry*>> pending;
    {
        std::lock_guard<std::mutex> lock(stateMutex());
        for (auto it = entries().begin(); it != entries().end();){
            std::shared_ptr<tinygltf::Model> parse = it->second.parse.lock();
            if (!parse){
                forget(it->second);
                it = entries().erase(it);
                continue;
            }
            if (!it->second.released && it->second.bytes == 0)
                pending.emplace_back(parse, &it->second);
            ++it;
        }
    }

    for (auto& [parse, entry] : pending){
        long holders = 1; // `pending` above
        if (ModelPool::peekGLTF(entry->poolKey) == parse)
            holders++;
        bool uploaded = false, waiting = false;
        std::vector<ExternalBuffer*> showing;
        for (Scene* scene : scenes){
            auto models = scene->getComponentArray<ModelComponent>();
            for (int i = 0; i < (int)models->size(); i++){
                if (models->getComponentFromIndex(i).gltfModel == parse)
                    holders++;
            }
            auto meshes = scene->getComponentArray<MeshComponent>();
            for (int i = 0; i < (int)meshes->size(); i++){
                MeshComponent& mesh = meshes->getComponentFromIndex(i);
                for (unsigned int b = 0; b < mesh.numExternalBuffers; b++){
                    ExternalBuffer& buffer = mesh.eBuffers[b];
                    if (buffer.getModelBuffer() < 0 || buffer.getShareOwner().lock() != std::static_pointer_cast<const void>(parse))
                        continue;
                    if (!mesh.loaded || mesh.needReload || !buffer.getRender()->isCreated())
                        waiting = true;
                    else
                        uploaded = true;
                    showing.push_back(&buffer);
                }
            }
        }
        // anything else holding the parse may hold pointers into it: its turn comes later
        if (waiting || !uploaded || parse.use_count() != holders){
            heldBack = entry->filename + ": " + (waiting ? "a mesh has not uploaded" : !uploaded ? "no mesh has uploaded" : "held by " +
                       std::to_string(parse.use_count()) + " references, " + std::to_string(holders) + " known");
            continue;
        }

        uint64_t freed = 0;
        for (ExternalBuffer* buffer : showing){
            if (isFileBuffer(parse->buffers[(size_t)buffer->getModelBuffer()]))
                buffer->releaseData();
        }
        for (tinygltf::Buffer& buffer : parse->buffers){
            if (!isFileBuffer(buffer))
                continue;
            freed += buffer.data.size();
            std::vector<unsigned char>().swap(buffer.data);
        }
        entry->released = true;
        entry->bytes = freed;
        pendingCount--;
        releasedCount++;
        releasedBytes += freed;
        CookedModelEntry cooked;
        const bool found = CookedModels::find(entry->filename, cooked);
        Log::print("Released the CPU copy of %s, loaded from %s: %llu bytes", entry->filename.c_str(), found ? cooked.file.c_str() : "?", (unsigned long long)freed);
    }
}

bool ModelCpuData::ensure(const std::shared_ptr<tinygltf::Model>& parse){
    if (releasedCount.load() == 0 || !parse)
        return true;
    std::lock_guard<std::recursive_mutex> bytesLock(bytesMutex());
    std::string filename;
    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex());
        auto it = entries().find(parse.get());
        if (it == entries().end() || !it->second.released)
            return true;
        filename = it->second.filename;
        bytes = it->second.bytes;
    }
    if (!restore(*parse, filename))
        return false;
    std::lock_guard<std::mutex> lock(stateMutex());
    Entry& entry = entries()[parse.get()];
    entry.released = false;     // bytes != 0 from here on: never released again
    releasedCount--;
    releasedBytes -= bytes;
    Log::verbose("Read %s again: something at runtime reads its mesh data", filename.c_str());
    return true;
}

bool ModelCpuData::ensure(const MeshComponent& mesh){
    bool any = false;
    for (unsigned int b = 0; b < mesh.numExternalBuffers; b++)
        any = any || mesh.eBuffers[b].isDataReleased();
    if (!any)
        return true;
    std::lock_guard<std::recursive_mutex> bytesLock(bytesMutex());
    // (const for the readers: a reader sees the same mesh, with its bytes back)
    MeshComponent& writable = const_cast<MeshComponent&>(mesh);
    for (unsigned int b = 0; b < mesh.numExternalBuffers; b++){
        ExternalBuffer& buffer = writable.eBuffers[b];
        if (!buffer.isDataReleased())
            continue;
        std::shared_ptr<tinygltf::Model> parse = std::static_pointer_cast<tinygltf::Model>(std::const_pointer_cast<void>(buffer.getShareOwner().lock()));
        if (!parse || !ensure(parse)){
            Log::error("Mesh buffer '%s' lost its model's data", buffer.getName().c_str());
            return false;
        }
        std::vector<unsigned char>& data = parse->buffers[(size_t)buffer.getModelBuffer()].data;
        if (buffer.getModelOffset() + buffer.getSize() > data.size()){
            Log::error("Mesh buffer '%s' does not fit the file read again", buffer.getName().c_str());
            return false;
        }
        buffer.restoreData(data.data() + buffer.getModelOffset());
    }
    return true;
}

std::string ModelCpuData::describeHeld(){
    return heldBack;
}

uint64_t ModelCpuData::getReleasedBytes(){
    return releasedBytes.load();
}

size_t ModelCpuData::getPendingCount(){
    return pendingCount.load();
}

void ModelCpuData::clear(){
    std::lock_guard<std::mutex> lock(stateMutex());
    entries().clear();
    pendingCount = 0;
    releasedCount = 0;
    releasedBytes = 0;
}
