// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ModelSubClips.h"

#include "math/Matrix4.h"

#include "animation/AnimationClipIO.h"
#include "animation/ClipBinding.h"
#include "io/File.h"
#include "io/FileData.h"
#include "pool/AnimationClipPoolTesting.h"
#include "pool/ModelPool.h"
#include "subsystem/MeshSystem.h"

#include "tiny_gltf.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>

using namespace jasperengine;

namespace{

    std::string lower(std::string text){
        for (char& c : text) c = (char)std::tolower((unsigned char)c);
        return text;
    }

    const char* const modelExtensions[] = {".gltf", ".glb", ".fbx"};

    // loadGLTF's entity name of a node
    std::string nodeName(const tinygltf::Model& model, size_t index){
        const std::string& name = model.nodes[index].name;
        return name.empty() ? "Node " + std::to_string(index) : name;
    }

    std::string pathSegment(const std::string& name, int index){
        return index > 0 ? name + "#" + std::to_string(index) : name;
    }

    struct NodePaths{
        std::vector<std::string> paths;
        // false when a name on the way can't be addressed
        std::vector<bool> addressable;
    };

    // buildGLTFNodeHierarchy's layout: the parent of a node is the last node listing it as a child
    // (loadGLTF's nodesParent), nodes without one sit under the model root, siblings keep node index
    // order. A node listing itself, or a parent loop (invalid files), goes under the model root.
    NodePaths computeNodePaths(const tinygltf::Model& model){
        const size_t count = model.nodes.size();
        std::vector<int> parents(count, -1);
        for (size_t i = 0; i < count; i++){
            for (int child : model.nodes[i].children){
                if (child >= 0 && (size_t)child < count && (size_t)child != i) parents[child] = (int)i;
            }
        }
        // a node whose ancestor chain comes back to it loses its parent
        for (size_t i = 0; i < count; i++){
            int current = parents[i];
            for (size_t steps = 0; current >= 0 && steps <= count; steps++){
                if ((size_t)current == i){
                    parents[i] = -1;
                    break;
                }
                current = parents[current];
            }
        }

        std::vector<std::string> segments(count);
        std::vector<bool> ownAddressable(count);
        std::map<std::pair<int, std::string>, int> siblings;
        for (size_t i = 0; i < count; i++){
            std::string name = nodeName(model, i);
            int index = siblings[{parents[i], name}]++;
            ownAddressable[i] = anim::isAddressableName(name);   // ClipBinding's rule (spec 9.R39)
            segments[i] = pathSegment(name, index);
        }

        NodePaths out;
        out.paths.assign(count, std::string());
        out.addressable.assign(count, true);
        std::vector<bool> done(count, false);
        std::vector<int> chain;
        for (size_t i = 0; i < count; i++){
            // the ancestors not done yet first (the chains are loop free now)
            chain.clear();
            for (int current = (int)i; current >= 0 && !done[current]; current = parents[current]){
                chain.push_back(current);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it){
                int node = *it;
                int parent = parents[node];
                if (parent >= 0){
                    out.paths[node] = out.paths[parent] + "/" + segments[node];
                    out.addressable[node] = out.addressable[parent] && ownAddressable[node];
                }else{
                    out.paths[node] = segments[node];
                    out.addressable[node] = ownAddressable[node];
                }
                done[node] = true;
            }
        }
        return out;
    }

    std::vector<std::string> animationNames(const tinygltf::Model& model){
        std::vector<std::string> names;
        names.reserve(model.animations.size());
        for (const tinygltf::Animation& animation : model.animations) names.push_back(animation.name);
        return anim::subClipNames(names);
    }

    // glTF's normalized integer to float conversion (floats as they are)
    float readComponent(const unsigned char* data, int componentType){
        switch (componentType){
            case TINYGLTF_COMPONENT_TYPE_BYTE:{
                int8_t value; std::memcpy(&value, data, 1);
                return std::max(value / 127.0f, -1.0f);
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:{
                uint8_t value; std::memcpy(&value, data, 1);
                return value / 255.0f;
            }
            case TINYGLTF_COMPONENT_TYPE_SHORT:{
                int16_t value; std::memcpy(&value, data, 2);
                return std::max(value / 32767.0f, -1.0f);
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:{
                uint16_t value; std::memcpy(&value, data, 2);
                return value / 65535.0f;
            }
            default:{
                float value; std::memcpy(&value, data, 4);
                return value;
            }
        }
    }

    const char* typeName(int type){
        switch (type){
            case TINYGLTF_TYPE_SCALAR: return "SCALAR";
            case TINYGLTF_TYPE_VEC2: return "VEC2";
            case TINYGLTF_TYPE_VEC3: return "VEC3";
            case TINYGLTF_TYPE_VEC4: return "VEC4";
            default: return "a matrix";
        }
    }

    // Reads an accessor as floats (`type` elements: SCALAR, VEC3 or VEC4), honouring the view's
    // byteStride and every offset, bounds-checked against the view and the buffer. Float, or a
    // normalized (signed/unsigned) byte or short when `normalizedAllowed`. Sparse accessors are not
    // read. False with `reason`.
    bool readAccessor(const tinygltf::Model& model, int accessorIndex, int type, bool normalizedAllowed,
                      std::vector<float>& out, std::string& reason){
        out.clear();
        if (accessorIndex < 0 || (size_t)accessorIndex >= model.accessors.size()){
            reason = "accessor " + std::to_string(accessorIndex) + " does not exist";
            return false;
        }
        const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
        const std::string name = "accessor " + std::to_string(accessorIndex);
        if (accessor.sparse.isSparse){
            reason = name + " is sparse (not supported)";
            return false;
        }
        if (accessor.type != type){
            reason = name + " is " + typeName(accessor.type) + ", expected " + typeName(type);
            return false;
        }
        size_t componentSize = 0;
        switch (accessor.componentType){
            case TINYGLTF_COMPONENT_TYPE_FLOAT: componentSize = 4; break;
            case TINYGLTF_COMPONENT_TYPE_BYTE:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: componentSize = 1; break;
            case TINYGLTF_COMPONENT_TYPE_SHORT:
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: componentSize = 2; break;
            default: break;
        }
        if (componentSize == 0 || (componentSize != 4 && (!normalizedAllowed || !accessor.normalized))){
            reason = name + " has component type " + std::to_string(accessor.componentType) +
                     (normalizedAllowed ? " (float or a normalized byte/short expected)" : " (float expected)");
            return false;
        }
        if (accessor.count == 0){
            reason = name + " is empty";
            return false;
        }
        if (accessor.bufferView < 0 || (size_t)accessor.bufferView >= model.bufferViews.size()){
            reason = name + " has no buffer view";
            return false;
        }
        const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
        if (view.buffer < 0 || (size_t)view.buffer >= model.buffers.size()){
            reason = name + ": buffer view " + std::to_string(accessor.bufferView) + " has no buffer";
            return false;
        }
        const std::vector<unsigned char>& data = model.buffers[view.buffer].data;
        const size_t components = (size_t)tinygltf::GetNumComponentsInType((uint32_t)type);
        const size_t elementSize = components * componentSize;
        const size_t stride = view.byteStride ? view.byteStride : elementSize;
        // the view inside the buffer, every element inside the view (no overflow on the way)
        if (stride < elementSize || view.byteOffset > data.size() || view.byteLength > data.size() - view.byteOffset ||
                accessor.byteOffset > view.byteLength || elementSize > view.byteLength - accessor.byteOffset ||
                (accessor.count - 1) > (view.byteLength - accessor.byteOffset - elementSize) / stride){
            reason = name + " reads past the end of its buffer";
            return false;
        }
        out.resize(accessor.count * components);
        const unsigned char* base = data.data() + view.byteOffset + accessor.byteOffset;
        for (size_t e = 0; e < accessor.count; e++){
            const unsigned char* element = base + e * stride;
            for (size_t c = 0; c < components; c++){
                out[e * components + c] = readComponent(element + c * componentSize, accessor.componentType);
            }
        }
        return true;
    }

    // The last model file withModelFile parsed itself (not in the ModelPool), so the sub-clips of
    // one library file (a player's N clips, the alias table, previews) share one parse instead of
    // a full parse each. Only what sub-clips read is kept (trimToAnimations: no meshes, skins or
    // images). Read-only once cached (any thread may read it: shared ownership); dropped by
    // dropModelFileParse (AnimationClipPool::invalidateModel, clear and clearUnused), and not used
    // once the time or size of a file it was read from changed: the file parsed (an FBX's imported
    // .glb) and a .gltf's external buffers. A buffer URI with escapes (%20) is not stat'ed: the
    // editor's model reload invalidates it.
    struct FileStamp{
        int64_t time = 0;
        uintmax_t size = 0;
        bool operator==(const FileStamp& other) const { return time == other.time && size == other.size; }
    };

    // zero when the file can't be stat'ed (a resource pack): then only the drops count
    FileStamp fileStamp(const std::string& key){
        FileStamp stamp;
        std::error_code ec;
        const std::filesystem::path path(key);
        const auto time = std::filesystem::last_write_time(path, ec);
        if (ec) return stamp;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec) return stamp;
        stamp.time = (int64_t)time.time_since_epoch().count();
        stamp.size = size;
        return stamp;
    }

    // the files a parse read, with their stamps when it was made
    using FileStamps = std::vector<std::pair<std::string, FileStamp>>;

    bool stampsCurrent(const FileStamps& files){
        for (const auto& file : files){
            if (!(fileStamp(file.first) == file.second)) return false;
        }
        return true;
    }

    // the file parseGLTFFile reads for `modelPath` (system path): a cooked .glb, an FBX's
    // imported .glb, or the file
    std::string parsedFile(const std::string& modelPath){
        const std::string read = MeshSystem::getImportedModelPath(modelPath);
        if (read != modelPath){
            return FileData::getSystemPath(read);
        }
        return MeshSystem::getModelFilenameKey(modelPath);
    }

    // Only what the sub-clip functions read: nodes, scenes and animations, the accessors (their
    // metadata) and the buffer views of the animation samplers, whose bytes are copied into one
    // buffer. Meshes, skins, materials, images and every other byte (a GLB's whole binary chunk,
    // embedded textures) go. Views no sampler uses lose their buffer (reads of them fail cleanly).
    void trimToAnimations(tinygltf::Model& model){
        std::set<int> views;
        for (const tinygltf::Animation& animation : model.animations){
            for (const tinygltf::AnimationSampler& sampler : animation.samplers){
                for (int accessor : {sampler.input, sampler.output}){
                    if (accessor < 0 || (size_t)accessor >= model.accessors.size()) continue;
                    const int view = model.accessors[accessor].bufferView;
                    if (view >= 0 && (size_t)view < model.bufferViews.size()) views.insert(view);
                }
            }
        }
        tinygltf::Buffer kept;
        for (size_t i = 0; i < model.bufferViews.size(); i++){
            tinygltf::BufferView& view = model.bufferViews[i];
            const bool used = views.count((int)i) && view.buffer >= 0 && (size_t)view.buffer < model.buffers.size() &&
                              view.byteOffset <= model.buffers[view.buffer].data.size() &&
                              view.byteLength <= model.buffers[view.buffer].data.size() - view.byteOffset;
            if (!used){
                view.buffer = -1;
                continue;
            }
            const std::vector<unsigned char>& data = model.buffers[view.buffer].data;
            while (kept.data.size() % 16) kept.data.push_back(0);    // the views' element alignment
            const size_t offset = kept.data.size();
            kept.data.insert(kept.data.end(), data.begin() + view.byteOffset, data.begin() + view.byteOffset + view.byteLength);
            view.buffer = 0;
            view.byteOffset = offset;
        }
        model.buffers.clear();
        model.buffers.push_back(std::move(kept));
        model.meshes.clear();
        model.skins.clear();
        model.materials.clear();
        model.textures.clear();
        model.images.clear();
        model.samplers.clear();
        model.cameras.clear();
        model.lights.clear();
    }

    struct ParseCache{
        std::mutex mutex;
        std::string key;
        FileStamps files;
        std::shared_ptr<const tinygltf::Model> model;
        uint64_t epoch = 0;     // bumped by every drop: a parse that started before it is not cached
        size_t parses = 0;      // parseGLTFFile calls (test builds read it)
    };

    // never destroyed: loads may run during static destruction
    ParseCache& parseCache(){
        static ParseCache* cache = new ParseCache();
        return *cache;
    }

    // Runs `read` on the model of `modelPath`: the pooled parse under the pooled-glTF mutex (loads
    // append buffers to it), else the cached parse of the file, else a parse of its own (cached)
    template<typename Read>
    bool withModelFile(const std::string& modelPath, std::string* error, Read read){
        if (!anim::isModelFilePath(modelPath)){
            if (error) *error = modelPath + " is not a glTF, GLB or FBX model";
            return false;
        }
        const std::string key = MeshSystem::getModelFilenameKey(modelPath);
        std::shared_ptr<tinygltf::Model> pooled = ModelPool::getGLTF(key);
        if (pooled){
            std::lock_guard<std::recursive_mutex> lock(MeshSystem::getPooledGLTFMutex());
            return read(*pooled);
        }
        ParseCache& cache = parseCache();
        std::shared_ptr<const tinygltf::Model> cached;
        FileStamps cachedFiles;
        uint64_t epoch = 0;
        {
            std::lock_guard<std::mutex> lock(cache.mutex);
            if (cache.model && cache.key == key){
                cached = cache.model;
                cachedFiles = cache.files;
            }
            epoch = cache.epoch;
        }
        // stat'ed without the lock
        if (cached && stampsCurrent(cachedFiles)) return read(*cached);

        // parsed without the lock (other files' reads go on meanwhile). The main file is stamped
        // before parsing, so a write during the parse is seen next time; the external buffers are
        // known only from the parse: one written since the parse started (its time not before the
        // start, also within a coarse timestamp's tick) keeps this parse out of the cache.
        const std::string mainFile = parsedFile(modelPath);
        FileStamps files = {{mainFile, fileStamp(mainFile)}};
        const std::filesystem::file_time_type parseStart = std::filesystem::file_time_type::clock::now();
        std::shared_ptr<tinygltf::Model> parsed = std::make_shared<tinygltf::Model>();
        std::string parseError;
        const bool ok = MeshSystem::parseGLTFFile(modelPath, *parsed, &parseError);
        bool cacheable = ok;
        if (ok){
            const std::filesystem::path folder = std::filesystem::path(mainFile).parent_path();
            for (const tinygltf::Buffer& buffer : parsed->buffers){
                if (buffer.uri.empty() || buffer.uri.rfind("data:", 0) == 0 || buffer.uri.find('%') != std::string::npos) continue;
                const std::string file = (folder / std::filesystem::path(std::u8string(buffer.uri.begin(), buffer.uri.end()))).lexically_normal().generic_string();
                std::error_code ec;
                const std::filesystem::file_time_type written = std::filesystem::last_write_time(file, ec);
                if (!ec && written >= parseStart) cacheable = false;
                files.push_back({file, fileStamp(file)});
            }
            trimToAnimations(*parsed);
        }
        {
            std::lock_guard<std::mutex> lock(cache.mutex);
            cache.parses++;
            if (cacheable && cache.epoch == epoch){
                cache.key = key;
                cache.files = std::move(files);
                cache.model = parsed;
            }
        }
        if (!ok){
            if (error){
                File imported;
                if (lower(FileData::getFilePathExtension(modelPath)) == "fbx" &&
                        imported.open(MeshSystem::getFbxImportPath(modelPath).c_str()) != FileErrors::FILEDATA_OK){
                    *error = "cannot load " + modelPath + ": the FBX is not imported yet (the editor makes its .glb "
                             "when it imports the file)";
                }else{
                    *error = "cannot load " + modelPath + ": " + parseError;
                }
            }
            return false;
        }
        return read(*parsed);
    }

}

bool anim::isModelFilePath(const std::string& path){
    std::string text = lower(path);
    for (const char* extension : modelExtensions){
        size_t length = std::strlen(extension);
        if (text.size() >= length && text.compare(text.size() - length, length, extension) == 0) return true;
    }
    return false;
}

bool anim::isSubClipReference(const std::string& reference){
    SubClipReference parsed;
    return parseSubClipReference(reference, parsed);
}

bool anim::parseSubClipReference(const std::string& reference, SubClipReference& out){
    out = SubClipReference();
    std::string text = lower(reference);
    size_t split = std::string::npos;
    for (const char* extension : modelExtensions){
        std::string marker = std::string(extension) + "#";
        size_t at = text.find(marker);
        if (at != std::string::npos) split = std::min(split, at + marker.size() - 1);
    }
    if (split == std::string::npos || split + 1 >= reference.size()) return false;
    out.model = reference.substr(0, split);
    out.clip = reference.substr(split + 1);
    return true;
}

std::string anim::formatSubClipReference(const std::string& model, const std::string& clip){
    return model + "#" + clip;
}

std::vector<std::string> anim::subClipNames(const std::vector<std::string>& animationNames){
    std::vector<std::string> bases(animationNames.size());
    for (size_t i = 0; i < animationNames.size(); i++){
        bases[i] = animationNames[i].empty() ? "Animation " + std::to_string(i) : animationNames[i];
    }
    std::set<std::string> taken(bases.begin(), bases.end());
    std::set<std::string> seen;
    std::map<std::string, int> nextNumber;
    std::vector<std::string> names(bases.size());
    for (size_t i = 0; i < bases.size(); i++){
        if (seen.insert(bases[i]).second){
            names[i] = bases[i];
            continue;
        }
        int& k = nextNumber.emplace(bases[i], 2).first->second;
        std::string candidate = bases[i] + " " + std::to_string(k++);
        while (taken.count(candidate)) candidate = bases[i] + " " + std::to_string(k++);
        taken.insert(candidate);
        names[i] = candidate;
    }
    return names;
}

std::vector<anim::ModelClipInfo> anim::listModelClips(const tinygltf::Model& model){
    std::vector<std::string> names = animationNames(model);
    std::vector<ModelClipInfo> out;
    out.reserve(model.animations.size());
    for (size_t i = 0; i < model.animations.size(); i++){
        const tinygltf::Animation& animation = model.animations[i];
        ModelClipInfo info;
        info.index = (int)i;
        info.name = names[i];
        info.channels = animation.channels.size();
        // the last input time of its samplers (a time accessor's max when it has one)
        for (const tinygltf::AnimationSampler& sampler : animation.samplers){
            if (sampler.input >= 0 && (size_t)sampler.input < model.accessors.size() &&
                    model.accessors[sampler.input].maxValues.size() == 1 &&
                    std::isfinite(model.accessors[sampler.input].maxValues[0])){
                info.duration = std::max(info.duration, (float)model.accessors[sampler.input].maxValues[0]);
                continue;
            }
            std::vector<float> times;
            std::string reason;
            if (readAccessor(model, sampler.input, TINYGLTF_TYPE_SCALAR, false, times, reason)){
                for (float time : times){
                    if (std::isfinite(time)) info.duration = std::max(info.duration, time);
                }
            }
        }
        out.push_back(info);
    }
    return out;
}

int anim::findModelClip(const tinygltf::Model& model, const std::string& clip){
    if (clip.empty()) return -1;
    std::vector<std::string> names = animationNames(model);
    for (size_t i = 0; i < names.size(); i++){
        if (names[i] == clip) return (int)i;
    }
    // a number is an index only when no animation has that name; one form per index (the pool's
    // alias table knows "2", not "02")
    if (clip.size() > 9 || (clip.size() > 1 && clip[0] == '0')) return -1;
    for (char c : clip){
        if (!std::isdigit((unsigned char)c)) return -1;
    }
    int index = std::stoi(clip);
    return index < (int)model.animations.size() ? index : -1;
}

std::vector<std::string> anim::modelNodePaths(const tinygltf::Model& model){
    return computeNodePaths(model).paths;
}

bool anim::buildSubClip(const tinygltf::Model& model, int animationIndex, ClipData& out,
                        std::vector<ClipWarning>& warnings, std::string* error){
    out = ClipData();
    if (animationIndex < 0 || (size_t)animationIndex >= model.animations.size()){
        if (error) *error = "the model has no animation " + std::to_string(animationIndex);
        return false;
    }
    const tinygltf::Animation& animation = model.animations[animationIndex];
    const std::string clipName = animationNames(model)[animationIndex];
    const std::string animationPath = "animations[" + std::to_string(animationIndex) + "]";
    if (animation.channels.empty()){
        if (error) *error = "animation \"" + clipName + "\" has no channels";
        return false;
    }

    const NodePaths nodes = computeNodePaths(model);
    for (size_t j = 0; j < animation.channels.size(); j++){
        const tinygltf::AnimationChannel& channel = animation.channels[j];
        const std::string where = animationPath + ".channels[" + std::to_string(j) + "]";
        auto drop = [&](const std::string& message){
            warnings.push_back({where, message + ": channel dropped"});
        };

        ClipChannel kind = ClipChannel::Position;
        int valueType = TINYGLTF_TYPE_VEC3;
        if (channel.target_path == "translation"){
            kind = ClipChannel::Position;
        }else if (channel.target_path == "rotation"){
            kind = ClipChannel::Rotation;
            valueType = TINYGLTF_TYPE_VEC4;
        }else if (channel.target_path == "scale"){
            kind = ClipChannel::Scale;
        }else if (channel.target_path == "weights"){
            kind = ClipChannel::Morph;
            valueType = TINYGLTF_TYPE_SCALAR;
        }else{
            drop("target path \"" + channel.target_path + "\" is not animatable");
            continue;
        }
        if (channel.target_node < 0 || (size_t)channel.target_node >= model.nodes.size()){
            drop("it targets no node");
            continue;
        }
        if (!nodes.addressable[channel.target_node]){
            drop("node \"" + nodes.paths[channel.target_node] +
                 "\" has a name with '/' or ending in '#' or \"#<number>\" on its path, which a clip can't address");
            continue;
        }
        if (channel.sampler < 0 || (size_t)channel.sampler >= animation.samplers.size()){
            drop("sampler " + std::to_string(channel.sampler) + " does not exist");
            continue;
        }
        const tinygltf::AnimationSampler& sampler = animation.samplers[channel.sampler];
        const bool step = sampler.interpolation == "STEP";
        const bool cubic = sampler.interpolation == "CUBICSPLINE";
        if (!step && !cubic && !sampler.interpolation.empty() && sampler.interpolation != "LINEAR"){
            warnings.push_back({where, "unknown interpolation \"" + sampler.interpolation + "\", using LINEAR"});
        }

        std::vector<float> times;
        std::vector<float> values;
        std::string reason;
        if (!readAccessor(model, sampler.input, TINYGLTF_TYPE_SCALAR, false, times, reason)){
            drop("input " + reason);
            continue;
        }
        // rotations and weights may be normalized integers (glTF 2.0 animation samplers)
        const bool normalizedAllowed = kind == ClipChannel::Rotation || kind == ClipChannel::Morph;
        if (!readAccessor(model, sampler.output, valueType, normalizedAllowed, values, reason)){
            drop("output " + reason);
            continue;
        }
        const size_t keys = times.size();
        const size_t perKey = cubic ? 3 : 1;   // CUBICSPLINE: in-tangent, value, out-tangent
        if (cubic && keys < 2){
            drop("CUBICSPLINE needs at least 2 keyframes");
            continue;
        }
        size_t width = kind == ClipChannel::Rotation ? 4 : 3;
        if (kind == ClipChannel::Morph){
            // one scalar per morph target in each element (CUBICSPLINE: a1..an, v1..vn, b1..bn)
            if (values.size() % (keys * perKey) != 0){
                drop(std::to_string(values.size()) + " output elements are not a multiple of " + std::to_string(keys * perKey));
                continue;
            }
            width = values.size() / (keys * perKey);
        }else if (values.size() != keys * perKey * width){
            drop(std::to_string(values.size() / width) + " output elements, expected " + std::to_string(keys * perKey));
            continue;
        }

        ClipTrackData track;
        track.path = nodes.paths[channel.target_node];
        track.channel = kind;
        track.keys.reserve(keys);
        size_t skipped = 0;
        for (size_t k = 0; k < keys; k++){
            if (!std::isfinite(times[k])){
                skipped++;
                continue;
            }
            ClipKey key;
            key.time = times[k];
            const float* element = values.data() + k * perKey * width;
            const float* value = element + (cubic ? width : 0);
            key.value.assign(value, value + width);
            if (cubic){
                key.inTangent.assign(element, element + width);
                key.outTangent.assign(element + 2 * width, element + 3 * width);
            }else if (step && k + 1 < keys){
                // a STEP sampler holds each key's value for its whole segment
                key.ease = EaseType::STEP;
            }
            if (kind == ClipChannel::Rotation){
                // quantized rotations are not unit length: renormalized without a warning
                float length = std::sqrt(key.value[0] * key.value[0] + key.value[1] * key.value[1] +
                                         key.value[2] * key.value[2] + key.value[3] * key.value[3]);
                if (length > 0.0f && std::isfinite(length) && std::fabs(length - 1.0f) > 1e-5f){
                    // the tangents scale with the key (as normalize does)
                    for (float& v : key.value) v /= length;
                    for (float& v : key.inTangent) v /= length;
                    for (float& v : key.outTangent) v /= length;
                }
            }
            track.keys.push_back(std::move(key));
        }
        if (skipped > 0){
            warnings.push_back({where, std::to_string(skipped) + " key(s) with a non-finite time dropped"});
        }
        if (track.keys.empty()){
            drop("no key has a finite time");
            continue;
        }
        out.tracks.push_back(std::move(track));
    }

    if (out.tracks.empty()){
        if (error) *error = "no channel of animation \"" + clipName + "\" could be read";
        return false;
    }

    // spec 2.4: frame rate 30, the importer's wrap (once), the length of the last key
    out.frameRate = 30.0f;
    out.wrap = ClipWrapMode::Once;
    out.length = -1.0f;
    out.source.kind = "model";
    out.source.name = clipName;
    normalize(out, &warnings);
    return true;
}

bool anim::readModelFile(const std::string& modelPath, const std::function<bool(const tinygltf::Model&)>& read, std::string* error){
    if (!isModelFilePath(modelPath)){
        if (error) *error = modelPath + " is not a glTF, GLB or FBX model";
        return false;
    }
    const std::string key = MeshSystem::getModelFilenameKey(modelPath);
    if (std::shared_ptr<tinygltf::Model> pooled = ModelPool::getGLTF(key)){
        std::lock_guard<std::recursive_mutex> lock(MeshSystem::getPooledGLTFMutex());
        return read(*pooled);
    }
    tinygltf::Model parsed;
    std::string parseError;
    if (!MeshSystem::parseGLTFFile(modelPath, parsed, &parseError)){
        if (error){
            File imported;
            if (lower(FileData::getFilePathExtension(modelPath)) == "fbx" &&
                    imported.open(MeshSystem::getFbxImportPath(modelPath).c_str()) != FileErrors::FILEDATA_OK){
                *error = "cannot load " + modelPath + ": the FBX is not imported yet (the editor makes its .glb when it imports the file)";
            }else{
                *error = "cannot load " + modelPath + ": " + parseError;
            }
        }
        return false;
    }
    return read(parsed);
}

void anim::dropModelFileParse(const std::string& modelPath){
    const std::string key = modelPath.empty() ? std::string() : MeshSystem::getModelFilenameKey(modelPath);
    ParseCache& cache = parseCache();
    std::shared_ptr<const tinygltf::Model> dropped;    // freed after the unlock
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.epoch++;
    if (cache.model && (key.empty() || cache.key == key)){
        dropped.swap(cache.model);
        cache.key.clear();
    }
}

#ifdef JASPERENGINE_ANIM_TEST_HOOKS
size_t jasperengine::getModelFileParseCount(){
    ParseCache& cache = parseCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    return cache.parses;
}
#endif

bool anim::readModelFileNodes(const std::string& modelPath, std::vector<ModelNodeInfo>& out, std::string* error){
    out.clear();
    return withModelFile(modelPath, error, [&](const tinygltf::Model& model){
        const NodePaths paths = computeNodePaths(model);
        const size_t count = model.nodes.size();
        out.resize(count);
        for (size_t i = 0; i < count; i++){
            out[i].name = nodeName(model, i);
            out[i].path = paths.paths[i];
            out[i].addressable = paths.addressable[i];
        }
        // computeNodePaths' parent rule: the last node listing it, loops broken
        for (size_t i = 0; i < count; i++){
            for (int child : model.nodes[i].children){
                if (child >= 0 && (size_t)child < count && (size_t)child != i) out[(size_t)child].parent = (int)i;
            }
        }
        for (size_t i = 0; i < count; i++){
            int current = out[i].parent;
            for (size_t steps = 0; current >= 0 && steps <= count; steps++){
                if ((size_t)current == i){
                    out[i].parent = -1;
                    break;
                }
                current = out[(size_t)current].parent;
            }
        }
        for (size_t i = 0; i < count; i++){
            const tinygltf::Node& node = model.nodes[i];
            ModelNodeInfo& info = out[i];
            if (node.matrix.size() == 16){
                Matrix4 matrix;
                for (int c = 0; c < 4; c++){
                    for (int r = 0; r < 4; r++) matrix[c][r] = (float)node.matrix[(size_t)(c * 4 + r)];
                }
                matrix.decompose(info.position, info.scale, info.rotation);
            }else{
                if (node.translation.size() == 3){
                    info.position = Vector3((float)node.translation[0], (float)node.translation[1], (float)node.translation[2]);
                }
                if (node.rotation.size() == 4){
                    // glTF x y z w
                    info.rotation = Quaternion((float)node.rotation[3], (float)node.rotation[0], (float)node.rotation[1], (float)node.rotation[2]);
                }
                if (node.scale.size() == 3){
                    info.scale = Vector3((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]);
                }
            }
        }
        return true;
    });
}

bool anim::listModelFileClips(const std::string& modelPath, std::vector<ModelClipInfo>& out, std::string* error){
    out.clear();
    return withModelFile(modelPath, error, [&](const tinygltf::Model& model){
        out = listModelClips(model);
        return true;
    });
}

bool anim::listModelFileNodes(const std::string& modelPath, std::vector<std::string>& out, std::string* error){
    out.clear();
    return withModelFile(modelPath, error, [&](const tinygltf::Model& model){
        out = modelNodePaths(model);
        return true;
    });
}

bool anim::buildModelSubClip(const std::string& reference, ClipData& out,
                             std::vector<ClipWarning>& warnings, std::string* error){
    out = ClipData();
    SubClipReference parsed;
    if (!parseSubClipReference(reference, parsed)){
        if (error) *error = "\"" + reference + "\" is not a model sub-clip (<model>#<animation>)";
        return false;
    }
    return withModelFile(parsed.model, error, [&](const tinygltf::Model& model){
        int index = findModelClip(model, parsed.clip);
        if (index < 0){
            if (error) *error = "no animation \"" + parsed.clip + "\" in " + parsed.model;
            return false;
        }
        if (!buildSubClip(model, index, out, warnings, error)) return false;
        out.source.path = parsed.model;
        return true;
    });
}
