// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipPool.h"
#include "AnimationClipPoolTesting.h"

#include "animation/AnimationClip.h"
#include "animation/AnimationClipIO.h"
#include "animation/HumanoidClip.h"
#include "animation/ModelSubClips.h"
#include "pose/HumanoidMapIO.h"
#include "pose/ModelRig.h"
#include "Engine.h"
#include "io/Data.h"
#include "io/File.h"
#include "io/FileData.h"
#include "io/ResourcePack.h"
#include "subsystem/MeshSystem.h"
#include "jobs/JobSystem.h"
#include "memory/MemoryStats.h"

#include <map>
#include <mutex>

using namespace jasperengine;

namespace{

    // the sub-clips of one model: clip part as written ("Walk", "0") -> animation index, and
    // each index's name (what the builder is given)
    struct ModelAliases{
        std::map<std::string, int> indices;
        std::vector<std::string> names;
    };

    struct PoolState{
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<const AnimationClip>> entries;
        // a key's generation, drawn from one pool-wide counter that only grows: two versions of
        // any keys never share a number, so a sub-clip alias that moves to another index entry
        // after a reimport still reads as changed. Survive invalidate and clear.
        std::map<std::string, uint64_t> generations;
        uint64_t generationCounter = 0;
        // bumped by every invalidate of the key ("<model key>#" for a model's alias table): a load
        // that started before it is not cached
        std::map<std::string, uint64_t> epochs;
        uint64_t clearEpoch = 0;
        std::map<std::string, ModelAliases> models;   // by model key
        uint64_t revision = 0;
        // the entries' memoryBytes(): every insert and erase of `entries` keeps it current. A
        // clip dropped while a player holds it leaves here at once and the heap at the player's
        // next rebind
        memory::TrackedBytes loadedBytes{"Animation.Clips", memory::Residency::Cpu};
#ifdef JASPERENGINE_ANIM_TEST_HOOKS
        AnimationClipPoolTestHooks hooks;
#endif
    };

    // never destroyed: players may release clips during static destruction
    PoolState& state(){
        static PoolState* pool = new PoolState();
        return *pool;
    }

    // a .anim path's key: MeshSystem's model key rule (FileData::getSystemPath, lexically normal,
    // generic separators), so the sub-clip keys and MeshSystem::invalidateModelFile's agree
    std::string pathKey(const std::string& path){
        if (path.empty()) return std::string();
        return MeshSystem::getModelFilenameKey(path);
    }

    // the entry key of a sub-clip index: "<model key>#<index>"
    std::string subClipKey(const std::string& modelKey, int index){
        return modelKey + "#" + std::to_string(index);
    }

    std::string aliasEpochKey(const std::string& modelKey){
        return modelKey + "#";
    }

    // A reference normalized without the lock (the path work allocates and asks System for the
    // asset root): a .anim key, or a model key + the clip part to resolve under the lock
    struct Reference{
        bool subClip = false;
        std::string key;        // .anim: the entry key; sub-clip: the model key
        std::string clip;       // sub-clip: the part after '#'
    };

    Reference reference(const std::string& input){
        Reference out;
        const std::string& path = input;
        anim::SubClipReference parsed;
        if (anim::parseSubClipReference(path, parsed)){
            out.subClip = true;
            out.key = pathKey(parsed.model);
            out.clip = parsed.clip;
        }else{
            out.key = pathKey(path);
        }
        return out;
    }

    // with the mutex held: the entry key, empty when a sub-clip alias is not known yet
    std::string entryKeyLocked(PoolState& pool, const Reference& ref){
        if (!ref.subClip) return ref.key;
        auto model = pool.models.find(ref.key);
        if (model == pool.models.end()) return std::string();
        auto alias = model->second.indices.find(ref.clip);
        if (alias == model->second.indices.end()) return std::string();
        return subClipKey(ref.key, alias->second);
    }

    uint64_t epochLocked(PoolState& pool, const std::string& key){
        auto it = pool.epochs.find(key);
        return it == pool.epochs.end() ? 0 : it->second;
    }

    // An invalidate of `key`: in-flight loads of it are not cached, a generation it had moves on
    void bumpLocked(PoolState& pool, const std::string& key){
        pool.epochs[key]++;
        auto generation = pool.generations.find(key);
        if (generation != pool.generations.end()) generation->second = ++pool.generationCounter;
    }

    // What a load saw when it started; it may cache its result only if none of it changed
    struct LoadStamp{
        uint64_t epoch = 0;
        uint64_t clearEpoch = 0;
        std::string aliasKey;       // sub-clips: the model's alias table epoch too
        uint64_t aliasEpoch = 0;
    };

    LoadStamp stampLocked(PoolState& pool, const std::string& key, const std::string& aliasKey = std::string()){
        LoadStamp stamp;
        stamp.epoch = epochLocked(pool, key);
        stamp.clearEpoch = pool.clearEpoch;
        stamp.aliasKey = aliasKey;
        if (!aliasKey.empty()) stamp.aliasEpoch = epochLocked(pool, aliasKey);
        return stamp;
    }

    bool stampValidLocked(PoolState& pool, const std::string& key, const LoadStamp& stamp){
        return epochLocked(pool, key) == stamp.epoch && pool.clearEpoch == stamp.clearEpoch &&
               (stamp.aliasKey.empty() || epochLocked(pool, stamp.aliasKey) == stamp.aliasEpoch);
    }

    std::map<std::string, std::shared_ptr<const AnimationClip>>::iterator
    eraseEntryLocked(PoolState& pool, std::map<std::string, std::shared_ptr<const AnimationClip>>::iterator entry){
        pool.loadedBytes.subtract(entry->second->memoryBytes());
        return pool.entries.erase(entry);
    }

    // Caches `clip` under `key` unless something it was loaded from was invalidated (or the pool
    // cleared) since the load started; a clip cached meanwhile by another load wins.
    std::shared_ptr<const AnimationClip> insert(PoolState& pool, const std::string& key, const LoadStamp& stamp,
                                                std::shared_ptr<const AnimationClip> clip){
#ifdef JASPERENGINE_ANIM_TEST_HOOKS
        std::function<void(const std::string&)> beforeInsert;
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            beforeInsert = pool.hooks.beforeInsert;
        }
        if (beforeInsert) beforeInsert(key);
#endif

        std::lock_guard<std::mutex> lock(pool.mutex);
        auto existing = pool.entries.find(key);
        if (existing != pool.entries.end()) return existing->second;
        if (!stampValidLocked(pool, key, stamp)) return clip;
        pool.entries[key] = clip;
        pool.loadedBytes.add(clip->memoryBytes());
        pool.generations[key] = ++pool.generationCounter;
        pool.revision++;
        return clip;
    }

    void dropModelLocked(PoolState& pool, const std::string& modelKey){
        std::string prefix = aliasEpochKey(modelKey);
        for (auto it = pool.entries.lower_bound(prefix); it != pool.entries.end() && it->first.compare(0, prefix.size(), prefix) == 0;){
            bumpLocked(pool, it->first);
            it = eraseEntryLocked(pool, it);
        }
        // in-flight sub-clip loads of this model that have no entry yet
        for (auto it = pool.epochs.lower_bound(prefix); it != pool.epochs.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it){
            it->second++;
        }
        // in-flight alias table fills and every sub-clip load that resolved through the table
        pool.epochs[prefix]++;
        pool.models.erase(modelKey);
        pool.revision++;
    }

    std::shared_ptr<const AnimationClip> loadFile(const std::string& path, std::string* error){
        // the parser reports problems as results; this is the last guard of a loader thread
        try{
            Data data;
            if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
                if (error) *error = "cannot read " + path;
                return nullptr;
            }
            std::string text(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
            anim::ClipParseResult parsed = anim::parseClip(text);
            if (!parsed.ok()){
                if (error) *error = parsed.error.toString();
                return nullptr;
            }
            return AnimationClip::build(parsed);
        }catch (const std::exception& e){
            if (error) *error = std::string("cannot load ") + path + ": " + e.what();
        }catch (...){
            if (error) *error = "cannot load " + path;
        }
        return nullptr;
    }

    // pollOrLoad's builds in flight, by getKey; their own mutex, never held with the pool's
    struct AsyncLoad{
        std::shared_ptr<const AnimationClip> clip;
        std::string error;
    };
    struct AsyncLoads{
        std::mutex mutex;
        std::map<std::string, jobs::Task<AsyncLoad>> pending;
    };

    AsyncLoads& asyncLoads(){
        static AsyncLoads* loads = new AsyncLoads();
        return *loads;
    }

    // forgets the build of one key: one not asked for again must not keep its clip alive
    void forgetAsyncLoad(const std::string& key){
        AsyncLoads& loads = asyncLoads();
        std::lock_guard<std::mutex> lock(loads.mutex);
        loads.pending.erase(key);
    }

    // forgets the builds in flight of a model ("<model key>#" prefix), every one when empty
    void forgetAsyncLoads(const std::string& modelKey){
        AsyncLoads& loads = asyncLoads();
        std::lock_guard<std::mutex> lock(loads.mutex);
        if (modelKey.empty()){
            loads.pending.clear();
            return;
        }
        const std::string prefix = aliasEpochKey(modelKey);
        for (auto it = loads.pending.lower_bound(prefix); it != loads.pending.end() && it->first.compare(0, prefix.size(), prefix) == 0;){
            it = loads.pending.erase(it);
        }
    }

    std::shared_ptr<const AnimationClip> cachedLocked(PoolState& pool, const std::string& key, std::vector<ClipWarning>* warnings){
        auto it = pool.entries.find(key);
        if (it == pool.entries.end()) return nullptr;
        if (warnings) *warnings = it->second->getWarnings();
        return it->second;
    }

}

#ifdef JASPERENGINE_ANIM_TEST_HOOKS
void jasperengine::setAnimationClipPoolTestHooks(const AnimationClipPoolTestHooks& hooks){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    pool.hooks = hooks;
}
#endif

std::string AnimationClipPool::getKey(const std::string& path){
    Reference ref = reference(path);
    return ref.subClip ? anim::formatSubClipReference(ref.key, ref.clip) : ref.key;
}

std::shared_ptr<const AnimationClip> AnimationClipPool::get(const std::string& path){
    Reference ref = reference(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    std::string key = entryKeyLocked(pool, ref);
    return key.empty() ? nullptr : cachedLocked(pool, key, nullptr);
}

namespace{

    // the sidecar of a model says Animation Type: Humanoid
    bool modelIsHumanoid(const std::string& modelPath){
        std::string stamp;
        if (!pose::humanoidSidecarStamp(modelPath, stamp, nullptr) || stamp.empty()) return false;
        pose::HumanoidOverrides overrides;
        std::vector<std::string> warnings;
        return pose::parseHumanoidOverrides(stamp, overrides, warnings, nullptr) && overrides.isHumanoid();
    }

    // `data` converted from `modelPath`'s rig (false + error when the model is no humanoid)
    bool convertFromModel(const ClipData& data, const std::string& modelPath, ClipData& out, std::vector<ClipWarning>& warnings,
                          std::string* error){
        std::shared_ptr<const anim::HumanoidSource> source;
        std::string sourceError;
        if (!anim::HumanoidSourceCache::get(modelPath, source, &sourceError)){
            if (error) *error = "retarget from " + modelPath + ": " + sourceError;
            return false;
        }
        return anim::convertToHumanoid(data, *source, out, warnings, error);
    }

}

std::shared_ptr<const AnimationClip> AnimationClipPool::getOrLoad(const std::string& path, std::string* error,
                                                                  std::vector<ClipWarning>* warnings){
    PoolState& pool = state();
    if (warnings) warnings->clear();
    if (path.empty()){
        if (error) *error = "no clip path";
        return nullptr;
    }
    Reference ref = reference(path);

    if (!ref.subClip){
        LoadStamp stamp;
        {
            std::lock_guard<std::mutex> lock(pool.mutex);
            if (std::shared_ptr<const AnimationClip> cached = cachedLocked(pool, ref.key, warnings)) return cached;
            stamp = stampLocked(pool, ref.key);
        }
        // read and build without the lock (lock order: never held while loading)
        std::shared_ptr<const AnimationClip> clip = loadFile(path, error);
        if (!clip) return nullptr;
        std::shared_ptr<const AnimationClip> cached = insert(pool, ref.key, stamp, clip);
        if (warnings) *warnings = cached->getWarnings();
        return cached;
    }

    // A model sub-clip: the model's alias table first (filled once per model), then the entry
    // of the animation index (ModelSubClips' builders: one parse of a model that is not in the
    // ModelPool serves the table and every index). Everything resolved
    // through a table is stamped with the table's epoch, so an invalidateModel (a reimport that
    // may reorder animations) during the load keeps the result out of the cache.
    anim::SubClipReference parsed;
    anim::parseSubClipReference(path, parsed);
    const std::string& modelKey = ref.key;
    std::string aliasKey = aliasEpochKey(modelKey);
    ModelAliases aliases;
    bool known = false;
    LoadStamp tableStamp;
#ifdef JASPERENGINE_ANIM_TEST_HOOKS
    AnimationClipPoolTestHooks hooks;
#endif
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        auto model = pool.models.find(modelKey);
        if (model != pool.models.end()){
            aliases = model->second;
            known = true;
        }
        tableStamp = stampLocked(pool, aliasKey);
#ifdef JASPERENGINE_ANIM_TEST_HOOKS
        hooks = pool.hooks;
#endif
    }
    if (!known){
        std::vector<anim::ModelClipInfo> infos;
#ifdef JASPERENGINE_ANIM_TEST_HOOKS
        bool listed = hooks.listModelClips ? hooks.listModelClips(parsed.model, infos, error)
                                           : anim::listModelFileClips(parsed.model, infos, error);
#else
        bool listed = anim::listModelFileClips(parsed.model, infos, error);
#endif
        if (!listed) return nullptr;
        for (const anim::ModelClipInfo& info : infos){
            if (info.index < 0) continue;
            if ((size_t)info.index >= aliases.names.size()) aliases.names.resize(info.index + 1);
            aliases.names[info.index] = info.name;
            aliases.indices[info.name] = info.index;
        }
        // a number is an index only when no animation has that name
        for (const anim::ModelClipInfo& info : infos){
            if (info.index >= 0) aliases.indices.emplace(std::to_string(info.index), info.index);
        }
        std::lock_guard<std::mutex> lock(pool.mutex);
        if (stampValidLocked(pool, aliasKey, tableStamp)){
            pool.models.emplace(modelKey, aliases);
        }
    }
    auto alias = aliases.indices.find(ref.clip);
    if (alias == aliases.indices.end()){
        if (error) *error = "no animation \"" + ref.clip + "\" in " + parsed.model;
        return nullptr;
    }
    int index = alias->second;
    std::string key = subClipKey(modelKey, index);
    LoadStamp stamp;
    {
        std::lock_guard<std::mutex> lock(pool.mutex);
        // only an entry made from the same table version counts as a hit
        if (stampValidLocked(pool, aliasKey, tableStamp)){
            if (std::shared_ptr<const AnimationClip> cached = cachedLocked(pool, key, warnings)) return cached;
        }
        stamp = stampLocked(pool, key, aliasKey);
        // the table this load resolved through, not a newer one
        stamp.aliasEpoch = tableStamp.epoch;
        if (pool.clearEpoch != tableStamp.clearEpoch) stamp.clearEpoch = tableStamp.clearEpoch;
    }
    // the builder resolves the reference by name: the index's own name, unique within a model
    ClipData data;
    std::vector<ClipWarning> buildWarnings;
    std::string name = (size_t)index < aliases.names.size() ? aliases.names[index] : std::to_string(index);
    std::string build = anim::formatSubClipReference(parsed.model, name);
#ifdef JASPERENGINE_ANIM_TEST_HOOKS
    bool built = hooks.buildSubClip ? hooks.buildSubClip(build, data, buildWarnings, error)
                                    : anim::buildModelSubClip(build, data, buildWarnings, error);
#else
    bool built = anim::buildModelSubClip(build, data, buildWarnings, error);
#endif
    if (!built) return nullptr;
    // a model whose Animation Type is Humanoid serves its clips as humanoid clips (spec 7.4)
    if (modelIsHumanoid(parsed.model)){
        ClipData converted;
        std::string convertError;
        if (convertFromModel(data, parsed.model, converted, buildWarnings, &convertError)){
            data = std::move(converted);
        }else{
            buildWarnings.push_back({"", "the model's Animation Type is Humanoid, but " + convertError + ": played as a generic clip"});
        }
    }
    std::shared_ptr<const AnimationClip> clip = AnimationClip::build(data, buildWarnings, nullptr);
    std::shared_ptr<const AnimationClip> cached = insert(pool, key, stamp, clip);
    if (warnings) *warnings = cached->getWarnings();
    return cached;
}

namespace{

    // Under this a .anim parses in the call: at about 50 ms per MB it takes a few milliseconds at
    // most, and a job would start it at least a frame late, moving the clip against the rest of
    // the game's timeline for nothing (tools/test-projects/upgrade-from-main plays frame for frame)
    constexpr uint64_t kAnimInCallBytes = 64 * 1024;

    // a file that cannot be opened counts as small: its error comes from the call
    bool smallAnim(const std::string& path){
        uint64_t packed = 0;
        if (ResourcePack::size(path, packed)) return packed < kAnimInCallBytes;
        File file;
        return file.open(path.c_str()) != 0 || file.length() < kAnimInCallBytes;
    }

    bool hasAsyncLoad(const std::string& key){
        AsyncLoads& loads = asyncLoads();
        std::lock_guard<std::mutex> lock(loads.mutex);
        return loads.pending.count(key) != 0;
    }

    bool noThreads(){
#ifdef JASPERENGINE_ANIM_TEST_HOOKS
        std::function<bool()> singleThreaded;
        {
            PoolState& pool = state();
            std::lock_guard<std::mutex> lock(pool.mutex);
            singleThreaded = pool.hooks.singleThreaded;
        }
        if (singleThreaded) return singleThreaded();
#endif
        return jobs::system().isSingleThreaded();
    }

}

std::shared_ptr<const AnimationClip> AnimationClipPool::pollOrLoad(const std::string& path, bool& loading, std::string* error, bool animInCall){
    loading = false;
    if (path.empty() || !Engine::isAsyncLoading()) return getOrLoad(path, error);
    // a .anim takes no lock another thread holds: without threads it needs no frames to wait for
    // the pump (a sub-clip's builder may wait for a loading model, so it keeps its job)
    if (!anim::isSubClipReference(path) && (animInCall || noThreads())){
        std::shared_ptr<const AnimationClip> clip = getOrLoad(path, error);
        // a job of an earlier poll is not asked for again
        if (clip) forgetAsyncLoad(getKey(path));
        return clip;
    }

    const std::string key = getKey(path);
    if (std::shared_ptr<const AnimationClip> cached = get(path)){
        // its job's handle holds the clip too: clearUnused would never free it
        forgetAsyncLoad(key);
        return cached;
    }
    if (!anim::isSubClipReference(path) && !hasAsyncLoad(key) && smallAnim(path)) return getOrLoad(path, error);

    AsyncLoads& loads = asyncLoads();
    jobs::Task<AsyncLoad> build;
    {
        std::lock_guard<std::mutex> lock(loads.mutex);
        auto it = loads.pending.find(key);
        if (it != loads.pending.end()){
            build = it->second;
        }else{
            // a Background job of the S06 system (a stopping system cancels it: built here then)
            jobs::SubmitOptions options;
            options.lane = jobs::Lane::Background;
            options.name = "anim-clip-load";
            options.tags = jobs::Tag::Asset;
            build = jobs::submit([path](){
                AsyncLoad result;
                result.clip = getOrLoad(path, &result.error);
                return result;
            }, options);
            loads.pending[key] = build;
        }
        if (build.status() != jobs::TaskStatus::Cancelled && !build.isReady()){
            loading = true;
            return nullptr;
        }
        loads.pending.erase(key);
    }
    // cancelled (a stopping job system) or failed: built on this thread
    if (build.status() != jobs::TaskStatus::Completed) return getOrLoad(path, error);
    const AsyncLoad& result = build.get();
    if (!result.clip && error) *error = result.error;
    return result.clip;
}

uint64_t AnimationClipPool::getGeneration(const std::string& path){
    Reference ref = reference(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    std::string key = entryKeyLocked(pool, ref);
    if (key.empty()) return 0;
    auto it = pool.generations.find(key);
    return it == pool.generations.end() ? 0 : it->second;
}

uint64_t AnimationClipPool::revision(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.revision;
}

void AnimationClipPool::invalidate(const std::string& path){
    // a model file: every sub-clip of it (spec 9.R13)
    if (anim::isModelFilePath(path)){
        invalidateModel(path);
        return;
    }
    Reference ref = reference(path);
    if (ref.subClip) forgetAsyncLoads(ref.key);
    else forgetAsyncLoad(ref.key);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    std::string key = entryKeyLocked(pool, ref);
    if (key.empty()) return;
    bumpLocked(pool, key);
    auto entry = pool.entries.find(key);
    if (entry != pool.entries.end()) eraseEntryLocked(pool, entry);
    pool.revision++;
}

void AnimationClipPool::invalidateModel(const std::string& modelPath){
    if (modelPath.empty()) return;
    // the builders' own parse of the file first: a load after this reads the file again
    anim::dropModelFileParse(modelPath);
    // its humanoid rig (the model or its .humanoid sidecar changed)
    anim::HumanoidSourceCache::drop(modelPath);
    std::string modelKey = pathKey(modelPath);
    forgetAsyncLoads(modelKey);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    dropModelLocked(pool, modelKey);
}

void AnimationClipPool::clear(){
    anim::dropModelFileParse(std::string());
    anim::HumanoidSourceCache::drop(std::string());
    forgetAsyncLoads(std::string());
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    for (const auto& entry : pool.entries){
        auto generation = pool.generations.find(entry.first);
        if (generation != pool.generations.end()) generation->second = ++pool.generationCounter;
    }
    for (auto it = pool.entries.begin(); it != pool.entries.end();) it = eraseEntryLocked(pool, it);
    pool.models.clear();
    pool.epochs.clear();
    pool.clearEpoch++;
    pool.revision++;
}

void AnimationClipPool::clearUnused(){
    anim::dropModelFileParse(std::string());
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    bool changed = false;
    for (auto it = pool.entries.begin(); it != pool.entries.end();){
        if (it->second.use_count() <= 1){
            it = eraseEntryLocked(pool, it);
            changed = true;
        }else{
            ++it;
        }
    }
    // alias tables of models with no clip left (the model may change before its next use)
    for (auto it = pool.models.begin(); it != pool.models.end();){
        std::string prefix = aliasEpochKey(it->first);
        auto entry = pool.entries.lower_bound(prefix);
        bool used = entry != pool.entries.end() && entry->first.compare(0, prefix.size(), prefix) == 0;
        if (!used){
            it = pool.models.erase(it);
            changed = true;
        }else{
            ++it;
        }
    }
    if (changed) pool.revision++;
}

void AnimationClipPool::releaseUnused(const std::string& path){
    Reference ref = reference(path);
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    std::string key = entryKeyLocked(pool, ref);
    auto it = key.empty() ? pool.entries.end() : pool.entries.find(key);
    if (it != pool.entries.end() && it->second.use_count() <= 1){
        eraseEntryLocked(pool, it);
        pool.revision++;
    }
}

size_t AnimationClipPool::size(){
    PoolState& pool = state();
    std::lock_guard<std::mutex> lock(pool.mutex);
    return pool.entries.size();
}

uint64_t AnimationClipPool::loadedBytes(){
    return state().loadedBytes.current();
}
