// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderWarmup.h"

#include "Engine.h"
#include "Log.h"
#include "io/AtomicFile.h"
#include "io/File.h"
#include "io/FileData.h"
#include "pool/ShaderPool.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"

#include "json.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

using namespace jasperengine;
using json = nlohmann::json;

namespace {

    constexpr int WARMUP_VERSION = 1;
    // variants a replay requests per frame (each may start a load or a compile)
    constexpr size_t REQUESTS_PER_FRAME = 8;

    struct WarmShader {
        std::string key;        // the storage key's text (no session id)
        std::string custom;     // a fork's or library shader's name, "" for built-in
    };

    struct WarmPipeline {
        size_t shader = 0;
        std::string family;     // "gl" or "modern": the backend family it was recorded on
        std::string desc;       // SokolPipelineRecord's JSON, as dumped (compared as text)
    };

    enum class ReplayState : uint8_t { Waiting, Requested, Ready, Failed };

    struct State {
        std::vector<WarmShader> shaders;
        std::vector<WarmPipeline> pipelines;
        std::map<std::pair<std::string, std::string>, size_t> shaderIndex;
        std::set<std::pair<size_t, std::string>> pipelineSet;
        size_t skipped = 0;

        // recording: what the recorder queued (any thread)
        bool recording = false;
        std::mutex queueMutex;
        std::vector<std::pair<uint32_t, std::string>> queued;

        // replay
        bool active = false;
        std::vector<ShaderKey> keys;
        std::vector<bool> keyValid;
        std::vector<ReplayState> shaderStates;
        std::vector<uint8_t> pipelineDone;          // 0 waiting, 1 held, 2 settled
        std::vector<std::vector<uint32_t>> held;
        size_t ready = 0;
        bool logged = false;
    };

    State& state(){
        static State s;
        return s;
    }

    const char* currentFamily(){
        return Engine::isOpenGL() ? "gl" : "modern";
    }

    size_t addShader(State& s, const std::string& key, const std::string& custom){
        const auto id = std::make_pair(key, custom);
        auto it = s.shaderIndex.find(id);
        if (it != s.shaderIndex.end())
            return it->second;
        s.shaders.push_back(WarmShader{key, custom});
        s.shaderIndex[id] = s.shaders.size() - 1;
        return s.shaders.size() - 1;
    }

    bool addPipeline(State& s, size_t shader, const std::string& family, const std::string& desc){
        if (!s.pipelineSet.insert(std::make_pair(shader, family + "|" + desc)).second)
            return false;
        s.pipelines.push_back(WarmPipeline{shader, family, desc});
        return true;
    }

    // the key of an entry in this session: its fork registered again
    bool sessionKey(const WarmShader& entry, ShaderKey& key){
        if (!ShaderPool::parseShaderKeyText(entry.key, key))
            return false;
        key.customId = entry.custom.empty() ? 0 : ShaderPool::registerCustomShader(entry.custom);
        return true;
    }

    // a list's entries added to s (true), or why the text is not a list
    bool readInto(State& s, const std::string& text, std::string* error){
        const json root = json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()){
            if (error) *error = "not a JSON object";
            return false;
        }
        const json version = root.value("version", json());
        if (!version.is_number_integer() || version.get<int>() != WARMUP_VERSION){
            if (error) *error = "unsupported version (this engine reads version " + std::to_string(WARMUP_VERSION) + ")";
            return false;
        }
        const json shaders = root.value("shaders", json::array());
        const json pipelines = root.value("pipelines", json::array());
        if (!shaders.is_array() || !pipelines.is_array()){
            if (error) *error = "shaders and pipelines must be arrays";
            return false;
        }
        // the indices of this text's shaders in s
        std::vector<long long> remap;
        for (const json& entry : shaders){
            ShaderKey key;
            if (!entry.is_object() || !entry.contains("key") || !entry["key"].is_string() ||
                    !ShaderPool::parseShaderKeyText(entry["key"].get<std::string>(), key) ||
                    (entry.contains("custom") && !entry["custom"].is_string())){
                s.skipped++;
                remap.push_back(-1);
                continue;
            }
            remap.push_back((long long)addShader(s, entry["key"].get<std::string>(), entry.value("custom", std::string())));
        }
        for (const json& entry : pipelines){
            if (!entry.is_object() || !entry.contains("shader") || !entry["shader"].is_number_unsigned() ||
                    !entry.contains("desc") || !entry["desc"].is_object()){
                s.skipped++;
                continue;
            }
            const size_t index = entry["shader"].get<size_t>();
            if (index >= remap.size() || remap[index] < 0){
                s.skipped++;
                continue;
            }
            const std::string family = entry.value("family", std::string(currentFamily()));
            addPipeline(s, (size_t)remap[index], family == "gl" ? "gl" : "modern", entry["desc"].dump());
        }
        return true;
    }

    std::string writeList(const State& s, const std::vector<bool>* keepShader){
        json root;
        root["version"] = WARMUP_VERSION;
        root["engine"] = "jasperengine";
        json shaders = json::array();
        std::vector<long long> remap(s.shaders.size(), -1);
        for (size_t i = 0; i < s.shaders.size(); i++){
            if (keepShader && !(*keepShader)[i])
                continue;
            remap[i] = (long long)shaders.size();
            shaders.push_back(json{{"key", s.shaders[i].key}, {"custom", s.shaders[i].custom}});
        }
        json pipelines = json::array();
        for (const WarmPipeline& p : s.pipelines){
            if (remap[p.shader] < 0)
                continue;
            pipelines.push_back(json{{"shader", remap[p.shader]}, {"family", p.family}, {"desc", json::parse(p.desc)}});
        }
        root["shaders"] = shaders;
        root["pipelines"] = pipelines;
        return root.dump(1);
    }

    void releaseHeld(State& s){
        for (std::vector<uint32_t>& pipelines : s.held){
            for (uint32_t pipeline : pipelines)
                SystemRender::releaseWarmPipeline(pipeline);
            pipelines.clear();
        }
    }

    void resolveRecorded(State& s){
        std::vector<std::pair<uint32_t, std::string>> queued;
        {
            std::lock_guard<std::mutex> lock(s.queueMutex);
            queued.swap(s.queued);
        }
        for (const auto& [shaderId, record] : queued){
            ShaderKey key;
            // a pipeline of a shader that is not the pool's (the engine's own passes made apart)
            if (!ShaderPool::findKeyOfHandle(shaderId, key))
                continue;
            const std::string custom = ShaderPool::getCustomShaderName(key.customId);
            const size_t index = addShader(s, ShaderPool::getShaderKeyText(ShaderPool::getStorageKey(key)), custom);
            addPipeline(s, index, currentFamily(), json::parse(record).dump());
        }
    }

    void replayStep(State& s){
        // what the list gained since the replay began (a recording merged) joins it
        while (s.keys.size() < s.shaders.size()){
            ShaderKey key;
            const bool valid = sessionKey(s.shaders[s.keys.size()], key);
            s.keys.push_back(key);
            s.keyValid.push_back(valid);
            s.shaderStates.push_back(valid ? ReplayState::Waiting : ReplayState::Failed);
        }
        while (s.pipelineDone.size() < s.pipelines.size()){
            s.pipelineDone.push_back(0);
            s.held.emplace_back();
        }

        size_t requests = 0;
        for (size_t i = 0; i < s.keys.size(); i++){
            ReplayState& st = s.shaderStates[i];
            if (st == ReplayState::Ready || st == ReplayState::Failed)
                continue;
            if (st == ReplayState::Waiting){
                if (requests >= REQUESTS_PER_FRAME)
                    continue;
                requests++;
                st = ReplayState::Requested;
            }
            const ShaderKey key = s.keys[i];
            if (ShaderPool::warmUp(key)){
                st = ReplayState::Ready;
            }else if (ShaderPool::getShaderState(key) == ResourceLoadState::Failed){
                st = ReplayState::Failed;
            }
        }

        for (size_t p = 0; p < s.pipelines.size(); p++){
            if (s.pipelineDone[p] == 2)
                continue;
            const WarmPipeline& entry = s.pipelines[p];
            const ReplayState shader = s.shaderStates[entry.shader];
            if (shader == ReplayState::Failed){
                s.pipelineDone[p] = 2;
                continue;
            }
            if (shader != ReplayState::Ready)
                continue;
            if (s.pipelineDone[p] == 0){
                const ShaderKey key = s.keys[entry.shader];
                std::shared_ptr<ShaderRender> render = ShaderPool::get(ShaderPool::getShaderTypeFromKey(key),
                    ShaderPool::getPropertiesFromKey(key), ShaderPool::getCustomIdFromKey(key));
                std::string error;
                if (!SystemRender::warmPipeline(render.get(), entry.desc, entry.family != currentFamily(), s.held[p], &error)){
                    Log::warn("Shader warm-up: a pipeline of %s is skipped: %s", s.shaders[entry.shader].key.c_str(), error.c_str());
                    s.skipped++;
                    s.pipelineDone[p] = 2;
                    continue;
                }
                s.pipelineDone[p] = 1;
            }
            bool settled = true;
            for (uint32_t pipeline : s.held[p])
                settled = settled && SystemRender::isPipelineSettled(pipeline);
            if (settled)
                s.pipelineDone[p] = 2;
        }

        size_t ready = 0;
        for (ReplayState st : s.shaderStates)
            ready += (st == ReplayState::Ready || st == ReplayState::Failed) ? 1 : 0;
        for (uint8_t done : s.pipelineDone)
            ready += done == 2 ? 1 : 0;
        s.ready = ready;
        if (!s.logged && ready == s.shaderStates.size() + s.pipelineDone.size()){
            s.logged = true;
            Log::print("[gpu] warm-up done %zu/%zu", ready, ready);
        }
    }

}

bool ShaderWarmup::loadFromString(const std::string& text, std::string* error){
    State& s = state();
    end();
    State fresh;
    if (!readInto(fresh, text, error))
        return false;
    s.shaders = std::move(fresh.shaders);
    s.pipelines = std::move(fresh.pipelines);
    s.shaderIndex = std::move(fresh.shaderIndex);
    s.pipelineSet = std::move(fresh.pipelineSet);
    s.skipped = fresh.skipped;
    return true;
}

bool ShaderWarmup::load(const std::string& path, std::string* error){
    // through File: shader:// is inside the package on Android and the web
    File file;
    if (file.open(path.c_str()) != FileErrors::FILEDATA_OK){
        if (error) *error = "cannot read " + path;
        return false;
    }
    const std::string text = file.readString();
    if (!loadFromString(text, error)){
        Log::warn("Shader warm-up list %s ignored: %s", path.c_str(), error ? error->c_str() : "");
        return false;
    }
    return true;
}

std::string ShaderWarmup::saveToString(){
    return writeList(state(), nullptr);
}

bool ShaderWarmup::save(const std::string& path, std::string* error){
    const io::WriteResult result = io::writeFileAtomic(FileData::getSystemPath(path), saveToString() + "\n");
    if (!result.ok()){
        if (error) *error = result.message;
        return false;
    }
    return true;
}

bool ShaderWarmup::mergeFromString(const std::string& text, std::string* error){
    return readInto(state(), text, error);
}

void ShaderWarmup::clear(){
    State& s = state();
    end();
    s.shaders.clear();
    s.pipelines.clear();
    s.shaderIndex.clear();
    s.pipelineSet.clear();
    s.skipped = 0;
}

size_t ShaderWarmup::getShaderCount(){
    return state().shaders.size();
}

size_t ShaderWarmup::getPipelineCount(){
    return state().pipelines.size();
}

size_t ShaderWarmup::getSkippedCount(){
    return state().skipped;
}

std::vector<ShaderKey> ShaderWarmup::getShaderKeys(){
    std::vector<ShaderKey> keys;
    for (const WarmShader& entry : state().shaders){
        ShaderKey key;
        if (sessionKey(entry, key))
            keys.push_back(key);
    }
    return keys;
}

bool ShaderWarmup::readShaderKeys(const std::string& text, std::vector<ShaderKey>& keys, std::string* error){
    State list;
    if (!readInto(list, text, error))
        return false;
    for (const WarmShader& entry : list.shaders){
        ShaderKey key;
        if (sessionKey(entry, key))
            keys.push_back(key);
    }
    return true;
}

std::string ShaderWarmup::filteredString(const std::vector<ShaderKey>& keep){
    const State& s = state();
    std::set<ShaderKey> kept(keep.begin(), keep.end());
    std::vector<bool> keepShader(s.shaders.size(), false);
    for (size_t i = 0; i < s.shaders.size(); i++){
        ShaderKey key;
        keepShader[i] = sessionKey(s.shaders[i], key) && kept.count(ShaderPool::normalizeKey(key)) > 0;
    }
    return writeList(s, &keepShader);
}

void ShaderWarmup::startRecording(){
    State& s = state();
    if (s.recording)
        return;
    s.recording = true;
    SystemRender::setPipelineRecorder([](uint32_t shaderId, const std::string& record){
        State& st = state();
        std::lock_guard<std::mutex> lock(st.queueMutex);
        st.queued.emplace_back(shaderId, record);
    });
}

void ShaderWarmup::stopRecording(){
    State& s = state();
    if (!s.recording)
        return;
    SystemRender::setPipelineRecorder(nullptr);
    s.recording = false;
    resolveRecorded(s);
}

bool ShaderWarmup::isRecording(){
    return state().recording;
}

bool ShaderWarmup::beginShipped(){
    std::string error;
    const std::string embedded = ShaderPool::getEmbeddedData("warmup");
    bool loaded = false;
    if (!embedded.empty()){
        loaded = loadFromString(embedded, &error);
        if (!loaded)
            Log::warn("Shader warm-up list (embedded) ignored: %s", error.c_str());
    }else{
        // absent from builds without a recorded list: not an error
        File probe;
        if (probe.open("shader://warmup.json") == FileErrors::FILEDATA_OK){
            probe.close();
            loaded = load("shader://warmup.json", &error);
        }
    }
    if (loaded)
        begin();
    return loaded;
}

void ShaderWarmup::begin(){
    State& s = state();
    end();
    s.active = true;
    s.logged = false;
    s.ready = 0;
    Log::print("[gpu] warm-up of %zu shaders and %zu pipelines", s.shaders.size(), s.pipelines.size());
}

bool ShaderWarmup::isActive(){
    return state().active;
}

bool ShaderWarmup::isDone(){
    const State& s = state();
    return !s.active || s.ready >= s.shaders.size() + s.pipelines.size();
}

float ShaderWarmup::getProgress(){
    const State& s = state();
    const size_t total = s.shaders.size() + s.pipelines.size();
    if (!s.active || total == 0)
        return 1.0f;
    return std::min(1.0f, (float)s.ready / (float)total);
}

size_t ShaderWarmup::getReadyCount(){
    return state().ready;
}

size_t ShaderWarmup::getEntryCount(){
    const State& s = state();
    return s.shaders.size() + s.pipelines.size();
}

void ShaderWarmup::end(){
    State& s = state();
    releaseHeld(s);
    s.active = false;
    s.keys.clear();
    s.keyValid.clear();
    s.shaderStates.clear();
    s.pipelineDone.clear();
    s.held.clear();
    s.ready = 0;
}

void ShaderWarmup::update(){
    State& s = state();
    if (s.recording)
        resolveRecorded(s);
    if (s.active && Engine::isViewLoaded())
        replayStep(s);
}
