// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R11-R16, R80-R83, R95: the Vertex Colors components on their meshes.
//
// A target without corner overrides binds one colour buffer over the mesh's own (R11). Its bytes
// are IMMUTABLE and pooled by content (MeshBufferPool), so identically painted copies of a mesh
// draw from one buffer and still instance (R13). Once its values change after it was bound (an
// edit, gameplay paint) it is remade DYNAMIC and written in place at most once a frame (R14); a
// stream that stops changing goes back to an IMMUTABLE pooled one. A target with overrides binds
// split vertices (R12), built on a job and swapped in when ready.

#include "subsystem/VertexColorRenderer.h"
#include "subsystem/VertexColorSceneDetail.h"

#include "Engine.h"
#include "Log.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/VertexColorsComponent.h"
#include "jobs/JobSystem.h"
#include "model/ModelCpuData.h"
#include "thread/MainThreadQueue.h"
#include "vertexcolor/VertexColorFile.h"
#include "vertexcolor/VertexColorOps.h"
#include "vertexcolor/VertexColorRuntimeLayer.h"
#include "vertexcolor/VertexColorSplit.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

using namespace jasperengine;
using namespace jasperengine::vertexcolor;

// ---------------------------------------------------------------- SubmeshVertexColors

SubmeshVertexColors::~SubmeshVertexColors(){
    if (MainThreadQueue::isMainThread()){
        releaseGpu();
        return;
    }
    std::vector<std::shared_ptr<BufferRender>> renders;
    for (ExternalBuffer& buffer : buffers)
        renders.push_back(std::make_shared<BufferRender>(*buffer.getRender()));
    MainThreadQueue::post([renders](){
        for (const std::shared_ptr<BufferRender>& render : renders)
            render->destroyBuffer();
    });
}

ExternalBuffer& SubmeshVertexColors::addBuffer(const std::string& name, BufferType type, unsigned int stride, unsigned int count,
                                               std::vector<uint8_t>&& data, BufferUsage usage){
    bytes.push_back(std::move(data));
    std::vector<uint8_t>& owned = bytes.back();
    buffers.emplace_back();
    ExternalBuffer& buffer = buffers.back();
    buffer.setName(name);
    buffer.setType(type);
    buffer.setUsage(usage);
    buffer.setRenderAttributes(false);
    buffer.setStride(stride);
    buffer.setData(owned.data(), owned.size());
    buffer.setCount(count);
    return buffer;
}

void SubmeshVertexColors::releaseGpu(){
    for (ExternalBuffer& buffer : buffers)
        buffer.getRender()->destroyBuffer();
}

bool SubmeshVertexColors::uploadColor(){
    if (!color || color->getUsage() == BufferUsage::IMMUTABLE)
        return false;
    needUpload = true;
    return true;
}

// ---------------------------------------------------------------- state

namespace {

    // a DYNAMIC colour stream unchanged this many frames goes back to an IMMUTABLE pooled one (R13, R14)
    constexpr uint64_t VC_SETTLE_FRAMES = 90;

    uint64_t vcMix(uint64_t h, uint64_t v){
        return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
    }

    // what the bound streams depend on that a frame can change without an edit: the meshes'
    // layout, buffers and detail levels (a model reload, a primitive rebuilt, LODs arriving)
    uint64_t vcMeshLayoutStamp(uint64_t h, Entity entity, const MeshComponent& mesh){
        h = vcMix(h, entity);
        h = vcMix(h, mesh.numSubmeshes);
        h = vcMix(h, mesh.numExternalBuffers);
        h = vcMix(h, (uint64_t)(uintptr_t)mesh.buffer.getData());
        h = vcMix(h, mesh.buffer.getSize());
        h = vcMix(h, mesh.indices.getSize());
        for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
            const Submesh& submesh = mesh.submeshes[s];
            h = vcMix(h, (uint64_t)(uintptr_t)submesh.lodData.get());
            h = vcMix(h, submesh.lodData ? submesh.lodData->numLevels : 0);
            h = vcMix(h, submesh.attributes.size());
        }
        return h;
    }

    // the LOD1+ triangle lists of a submesh over its own vertices (S54)
    std::vector<std::vector<uint32_t>> vcLodLists(const Submesh& submesh){
        std::vector<std::vector<uint32_t>> lists;
        const MeshLodData* lod = submesh.lodData.get();
        if (!lod || lod->numLevels <= 1)
            return lists;
        const size_t size = lod->indexType == AttributeDataType::UNSIGNED_INT ? 4 : 2;
        for (unsigned int l = 1; l < lod->numLevels; l++){
            std::vector<uint32_t> list(lod->levels[l].indexCount);
            for (size_t i = 0; i < list.size(); i++){
                const size_t at = ((size_t)lod->levels[l].indexBase + i) * size;
                if (at + size > lod->indices.size()){
                    list.resize(i);
                    break;
                }
                if (size == 4){
                    std::memcpy(&list[i], &lod->indices[at], 4);
                }else{
                    uint16_t v;
                    std::memcpy(&v, &lod->indices[at], 2);
                    list[i] = v;
                }
            }
            lists.push_back(std::move(list));
        }
        return lists;
    }

    size_t vcTypeSize(AttributeDataType type){
        switch (type){
            case AttributeDataType::BYTE:
            case AttributeDataType::UNSIGNED_BYTE: return 1;
            case AttributeDataType::SHORT:
            case AttributeDataType::UNSIGNED_SHORT:
            case AttributeDataType::HALF: return 2;
            default: return 4;
        }
    }

    Attribute vcColorAttribute(const std::string& buffer, Format format, unsigned int count){
        if (format == Format::RGBA16F)
            return Attribute(AttributeDataType::HALF, buffer, 4, 0, count, false, false);
        return Attribute(AttributeDataType::UNSIGNED_BYTE, buffer, 4, 0, count, true, false);
    }

    std::string vcBufferName(unsigned int submesh, const char* what){
        return "vertexcolors:" + std::to_string(submesh) + ":" + what;
    }

}

struct VertexColorRenderer::TargetState {
    TargetKey key;
    Entity meshEntity = NULL_ENTITY;
    unsigned int submesh = 0;
    uint64_t geometry = 0;              // geometryStamp the binding was made for
    uint64_t authoredHash = 0;          // the authored values it shows (hashTarget, format)
    bool hasAuthored = false;
    std::shared_ptr<SubmeshVertexColors> binding;
    // per bound vertex: its authored value, and with a split the GPU vertex it copies
    std::vector<Vector4> authored;
    std::vector<uint32_t> sourceVertex;
    std::vector<uint32_t> splitIndices;     // LOD0 then every level, as bound
    bool dynamic = false;
    uint64_t lastChange = 0;                // frame of the last in-place write
    bool mismatchLogged = false;
    // a split being built (R12)
    jobs::Task<std::shared_ptr<SplitResult>> job;
    uint64_t jobHash = 0;
    uint64_t jobGeometry = 0;
    std::shared_ptr<const MeshView> jobView;
};

struct VertexColorRenderer::OwnerState {
    uint64_t revision = ~0ull;
    uint64_t layout = 0;
    bool enabled = false;
    Format format = Format::RGBA8;
    InitialValue initial = InitialValue::Asset;
    Vector4 customInitial;
    std::vector<TargetState> targets;
};

VertexColorRenderer::VertexColorRenderer(Scene* scene): scene(scene){
}

VertexColorRenderer::~VertexColorRenderer(){
    clear();
}

void VertexColorRenderer::clear(){
    for (auto& entry : owners){
        for (TargetState& target : entry.second->targets){
            if (target.job.isValid())
                target.job.cancel();
        }
    }
    meshBindings.clear();
    owners.clear();
    views.clear();
}

// ---------------------------------------------------------------- bindings

void VertexColorRenderer::setBinding(Entity meshEntity, unsigned int submesh, const std::shared_ptr<SubmeshVertexColors>& binding){
    MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity);
    auto it = meshBindings.find(meshEntity);
    if (!binding){
        if (it == meshBindings.end() || submesh >= it->second.size() || !it->second[submesh])
            return;
        it->second[submesh].reset();
        if (std::none_of(it->second.begin(), it->second.end(), [](const auto& b){ return b != nullptr; }))
            meshBindings.erase(it);
    }else{
        std::vector<std::shared_ptr<SubmeshVertexColors>>& list = meshBindings[meshEntity];
        if (list.size() <= submesh)
            list.resize(submesh + 1);
        list[submesh] = binding;
        stats.bindings++;
    }
    // the mesh binds again (a load in flight finishes first, then loads once more)
    if (mesh && (mesh->loaded || mesh->loadCalled))
        mesh->needReload = true;
}

std::shared_ptr<SubmeshVertexColors> VertexColorRenderer::getBinding(Entity meshEntity, unsigned int submesh) const{
    auto it = meshBindings.find(meshEntity);
    if (it == meshBindings.end() || submesh >= it->second.size())
        return nullptr;
    return it->second[submesh];
}

void VertexColorRenderer::unbindTarget(TargetState& target){
    if (target.job.isValid())
        target.job.cancel();
    target.job = {};
    target.jobView.reset();
    if (target.binding)
        setBinding(target.meshEntity, target.submesh, nullptr);
    target.binding.reset();
    target.authored.clear();
    target.sourceVertex.clear();
    target.splitIndices.clear();
    target.hasAuthored = false;
    target.dynamic = false;
}

void VertexColorRenderer::unbindOwner(OwnerState& owner){
    for (TargetState& target : owner.targets)
        unbindTarget(target);
    owner.targets.clear();
}

std::shared_ptr<SubmeshVertexColors> VertexColorRenderer::makeColorBinding(unsigned int submesh, Format format, std::vector<uint8_t>&& bytes,
                                                                          unsigned int count, bool dynamic){
    auto binding = std::make_shared<SubmeshVertexColors>();
    const std::string name = vcBufferName(submesh, "color");
    const unsigned int stride = format == Format::RGBA16F ? 8 : 4;
    binding->color = &binding->addBuffer(name, BufferType::VERTEX_BUFFER, stride, count, std::move(bytes),
                                         dynamic ? BufferUsage::DYNAMIC : BufferUsage::IMMUTABLE);
    binding->attributes[AttributeType::COLOR] = vcColorAttribute(name, format, count);
    return binding;
}

std::shared_ptr<SubmeshVertexColors> VertexColorRenderer::makeSplitBinding(MeshComponent& mesh, unsigned int submeshIndex, Format format,
                                                                          const SplitResult& split, std::vector<uint8_t>&& colorBytes, bool dynamic){
    const Submesh& submesh = mesh.submeshes[submeshIndex];
    ModelCpuData::ensure(mesh, "vertex colors");
    auto binding = makeColorBinding(submeshIndex, format, std::move(colorBytes), (unsigned int)split.values.size(), dynamic);
    binding->split = true;
    binding->splitVertices = split.added;

    // every vertex input, copied per split vertex from the GPU vertex it duplicates (skin and morph
    // targets with it): one tight buffer each
    std::set<AttributeType> inputs;
    for (const auto& entry : submesh.attributes){
        if (entry.first != AttributeType::INDEX)
            inputs.insert(entry.first);
    }
    if (mesh.buffer.getSize() > 0 && mesh.buffer.isRenderAttributes()){
        for (const auto& entry : mesh.buffer.getAttributes())
            inputs.insert(entry.first);
    }
    inputs.erase(AttributeType::COLOR);
    inputs.erase(AttributeType::INDEX);
    const size_t outCount = split.sourceVertex.size();
    for (AttributeType type : inputs){
        const vertexcolor::detail::AttributeSource source = vertexcolor::detail::findAttribute(mesh, submesh, type);
        if (!source || !source.buffer->getData())
            continue;
        const Attribute& attribute = source.attribute;
        const size_t element = attribute.getElements() * vcTypeSize(attribute.getDataType());
        const size_t stride = source.buffer->getStride() ? source.buffer->getStride() : element;
        const unsigned char* data = source.buffer->getData();
        std::vector<uint8_t> bytes(outCount * element);
        for (size_t o = 0; o < outCount; o++){
            const size_t at = attribute.getOffset() + (size_t)split.sourceVertex[o] * stride;
            if (at + element > source.buffer->getSize())
                return nullptr;
            std::memcpy(bytes.data() + o * element, data + at, element);
        }
        const std::string name = vcBufferName(submeshIndex, ("input" + std::to_string((int)type)).c_str());
        binding->addBuffer(name, BufferType::VERTEX_BUFFER, (unsigned int)element, (unsigned int)outCount, std::move(bytes), BufferUsage::IMMUTABLE);
        binding->attributes[type] = Attribute(attribute.getDataType(), name, attribute.getElements(), 0, (unsigned int)outCount,
                                              attribute.getNormalized(), false);
    }

    // the triangles over the split vertices: LOD0, then each level (R12's LOD rule)
    std::vector<uint32_t> indices = split.indices;
    binding->levels.push_back({0u, (uint32_t)split.indices.size()});
    for (const std::vector<uint32_t>& level : split.lodIndices){
        binding->levels.push_back({(uint32_t)indices.size(), (uint32_t)level.size()});
        indices.insert(indices.end(), level.begin(), level.end());
    }
    std::vector<uint8_t> indexBytes(indices.size() * 4);
    if (!indices.empty())
        std::memcpy(indexBytes.data(), indices.data(), indexBytes.size());
    const std::string indexName = vcBufferName(submeshIndex, "index");
    binding->index = &binding->addBuffer(indexName, BufferType::INDEX_BUFFER, 4, (unsigned int)indices.size(), std::move(indexBytes),
                                         BufferUsage::IMMUTABLE);
    binding->attributes[AttributeType::INDEX] = Attribute(AttributeDataType::UNSIGNED_INT, indexName, 1, 0, (unsigned int)split.indices.size(), false, false);
    return binding;
}

// ---------------------------------------------------------------- per frame

bool VertexColorRenderer::loadData(Entity entity, VertexColorsComponent& colors){
    bool read = false;
    const bool loaded = vertexcolor::detail::loadComponentData(scene, entity, colors, &read);
    if (read)
        stats.fileReads++;
    return loaded;
}

uint64_t VertexColorRenderer::ownerLayout(Entity entity){
    uint64_t h = 0x7663u;
    if (const ModelComponent* model = scene->findComponent<ModelComponent>(entity)){
        h = vcMix(h, (uint64_t)(uintptr_t)model->gltfModel.get());
        h = vcMix(h, model->loadedMergeStaticMeshes);
        for (const auto& [node, child] : model->meshNodesMapping){
            h = vcMix(h, (uint64_t)node);
            if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(child))
                h = vcMeshLayoutStamp(h, child, *mesh);
        }
    }
    if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity))
        h = vcMeshLayoutStamp(h, entity, *mesh);
    return h;
}

void VertexColorRenderer::prepare(){
    if (owners.empty() && scene->getComponentArray<VertexColorsComponent>()->size() == 0)
        return;
    frame++;

    // R15: a removed component leaves nothing behind
    for (auto it = owners.begin(); it != owners.end();){
        VertexColorsComponent* colors = scene->findComponent<VertexColorsComponent>(it->first);
        if (!colors){
            unbindOwner(*it->second);
            it = owners.erase(it);
        }else{
            ++it;
        }
    }

    auto colorsArray = scene->getComponentArray<VertexColorsComponent>();
    const double now = Engine::getGameTime();
    for (size_t i = 0; i < colorsArray->size(); i++){
        const Entity entity = colorsArray->getEntity(i);
        VertexColorsComponent& colors = colorsArray->getComponentFromIndex(i);
        std::unique_ptr<OwnerState>& owner = owners[entity];
        if (!owner)
            owner = std::make_unique<OwnerState>();
        syncOwner(entity, colors, *owner, now);
    }
}

void VertexColorRenderer::syncOwner(Entity entity, VertexColorsComponent& colors, OwnerState& owner, double now){
    // R15: off unbinds; the mesh draws as without it
    if (!colors.enabled){
        if (owner.enabled)
            unbindOwner(owner);
        owner.enabled = false;
        colors.needUpdate = false;
        return;
    }

    bool dirty = !owner.enabled || colors.needUpdate || colors.revision != owner.revision ||
                 colors.format != owner.format || colors.initialValue != owner.initial || colors.customInitial != owner.customInitial;
    if (colors.needUpdate || !colors.loaded)
        loadData(entity, colors);
    colors.needUpdate = false;

    // R82: runtime paint fades at most 10 times a second; nothing to do without live paint
    bool layers = false;
    for (auto& [key, layer] : colors.runtimeLayers){
        if (!layer)
            continue;
        if (layer->live())
            layer->update(now);
        layers = layers || layer->dirty();
    }

    const uint64_t layout = ownerLayout(entity);
    bool pending = false;
    for (const TargetState& target : owner.targets)
        pending = pending || target.job.isValid();
    bool settling = false;
    for (const TargetState& target : owner.targets)
        settling = settling || (target.dynamic && frame - target.lastChange > VC_SETTLE_FRAMES);
    if (!dirty && !layers && !pending && !settling && layout == owner.layout)
        return;

    owner.enabled = true;
    owner.revision = colors.revision;
    owner.layout = layout;
    owner.format = colors.format;
    owner.initial = colors.initialValue;
    owner.customInitial = colors.customInitial;

    const std::vector<SceneTarget> targets = vertexcolor::detail::resolveTargets(scene, entity);
    // targets that are gone (a reload that changed the model) let go of their meshes
    for (TargetState& old : owner.targets){
        const bool kept = std::any_of(targets.begin(), targets.end(), [&](const SceneTarget& t){
            return t.key == old.key && t.meshEntity == old.meshEntity && t.submesh == old.submesh; });
        if (!kept)
            unbindTarget(old);
    }
    std::vector<TargetState> next;
    next.reserve(targets.size());
    for (const SceneTarget& t : targets){
        TargetState state;
        for (TargetState& old : owner.targets){
            if (old.key == t.key && old.meshEntity == t.meshEntity && old.submesh == t.submesh){
                state = std::move(old);
                break;
            }
        }
        state.key = t.key;
        state.meshEntity = t.meshEntity;
        state.submesh = t.submesh;
        syncTarget(entity, colors, state);
        next.push_back(std::move(state));
    }
    owner.targets = std::move(next);
}

void VertexColorRenderer::syncTarget(Entity entity, VertexColorsComponent& colors, TargetState& target){
    MeshComponent* mesh = scene->findComponent<MeshComponent>(target.meshEntity);
    const TargetData* data = colors.loaded ? colors.loaded->find(target.key) : nullptr;
    auto layerIt = colors.runtimeLayers.find(target.key);
    RuntimeLayer* layer = layerIt != colors.runtimeLayers.end() ? layerIt->second.get() : nullptr;
    // R2: a target the file does not hold keeps the mesh's own colors (unless gameplay paints it)
    if (!mesh || (!data && !layer)){
        unbindTarget(target);
        return;
    }
    std::shared_ptr<const MeshView> view = getMeshView(entity, target.meshEntity, target.submesh, *mesh);
    if (!view){
        unbindTarget(target);
        return;
    }
    const Fingerprint fingerprint = view->fingerprint();
    // R10, R92: a runtime never remaps: a target painted on another topology is skipped
    if (data && (!data->fingerprint.sameTopology(fingerprint) || data->base.size() != view->vertexCount())){
        if (!target.mismatchLogged){
            target.mismatchLogged = true;
            Log::warn("Vertex Colors on '%s': %s was painted on another version of the mesh (%u vertices, %u indices; the mesh has %u, %u): "
                      "it draws its own colors. Open the scene in the editor and save it to remap them",
                      scene->getEntityName(entity).c_str(), target.key.describe().c_str(), data->fingerprint.vertexCount,
                      data->fingerprint.indexCount, fingerprint.vertexCount, fingerprint.indexCount);
        }
        unbindTarget(target);
        return;
    }
    target.mismatchLogged = false;

    uint64_t authoredHash = vcMix(0x61u, (uint64_t)colors.format);
    if (data){
        authoredHash = vcMix(authoredHash, hashTarget(*data));
    }else{
        // a target only gameplay paints starts from the initial value (R6)
        uint32_t custom[4];
        std::memcpy(custom, &colors.customInitial, sizeof(custom));
        authoredHash = vcMix(authoredHash, (uint64_t)colors.initialValue + 1);
        for (uint32_t c : custom)
            authoredHash = vcMix(authoredHash, c);
    }
    const bool split = data && data->hasOverrides();
    // a split remaps the detail levels too: new levels make it again (a plain stream does not care)
    const uint64_t geometry = vertexcolor::detail::geometryStamp(*mesh, target.submesh) ^
                              (split ? (uint64_t)(uintptr_t)mesh->submeshes[target.submesh].lodData.get() : 0u);

    // the authored values per bound vertex, when what they come from changed
    if (!target.hasAuthored || target.geometry != geometry || target.authoredHash != authoredHash){
        if (split){
            // R12: on a job; the bound stream draws until it is ready
            if (!target.job.isValid() || target.jobHash != authoredHash || target.jobGeometry != geometry){
                if (target.job.isValid())
                    target.job.cancel();
                auto copy = std::make_shared<TargetData>(*data);
                auto lods = std::make_shared<std::vector<std::vector<uint32_t>>>(vcLodLists(mesh->submeshes[target.submesh]));
                std::shared_ptr<const MeshView> jobView = view;
                target.job = jobs::submit([copy, lods, jobView](){
                    return std::make_shared<SplitResult>(buildSplit(*copy, *jobView, *lods));
                }, {jobs::Lane::Normal, jobs::GateClass::Default, "vertex-color-split"});
                target.jobHash = authoredHash;
                target.jobGeometry = geometry;
                target.jobView = view;
                stats.splitJobs++;
            }
            if (!target.job.isReady())
                return;
            std::shared_ptr<SplitResult> result;
            if (target.job.status() == jobs::TaskStatus::Completed)
                result = target.job.get();
            target.job = {};
            target.jobView.reset();
            if (!result){
                unbindTarget(target);
                return;
            }
            if (!result->split){
                target.authored = std::move(result->values);
                target.sourceVertex.clear();
                target.splitIndices.clear();
            }else{
                std::vector<uint32_t> indices = result->indices;
                for (const std::vector<uint32_t>& level : result->lodIndices)
                    indices.insert(indices.end(), level.begin(), level.end());
                const bool sameStructure = target.binding && target.binding->split && target.geometry == geometry &&
                                           target.sourceVertex == result->sourceVertex && target.splitIndices == indices;
                target.authored = result->values;
                if (!sameStructure){
                    target.sourceVertex = result->sourceVertex;
                    target.splitIndices = std::move(indices);
                    std::vector<Vector4> values;
                    if (layer)
                        layer->combine(target.authored, target.sourceVertex, values);
                    target.geometry = geometry;
                    target.authoredHash = authoredHash;
                    target.hasAuthored = true;
                    target.binding = makeSplitBinding(*mesh, target.submesh, colors.format, *result,
                                                      packValues(layer ? values : target.authored, colors.format), false);
                    target.dynamic = false;
                    setBinding(target.meshEntity, target.submesh, target.binding);
                    return;
                }
            }
        }else{
            if (target.job.isValid())
                target.job.cancel();
            target.job = {};
            if (data){
                target.authored = data->base;
            }else{
                TargetData initial;
                initializeTarget(initial, *view, colors.initialValue, colors.customInitial);
                target.authored = std::move(initial.base);
            }
            target.sourceVertex.clear();
            target.splitIndices.clear();
        }
        target.authoredHash = authoredHash;
        target.hasAuthored = true;
    }

    // the stream as drawn: authored values with the runtime layer over them (R80)
    std::vector<Vector4> values;
    if (layer && target.sourceVertex.empty())
        layer->combine(target.authored, values);
    else if (layer)
        layer->combine(target.authored, target.sourceVertex, values);
    std::vector<uint8_t> bytes = packValues(layer ? values : target.authored, colors.format);
    const bool splitBound = !target.sourceVertex.empty();

    const bool sameLayout = target.binding && target.geometry == geometry && target.binding->split == splitBound &&
                            target.binding->color && target.binding->color->getSize() == bytes.size() &&
                            target.binding->color->getStride() == (colors.format == Format::RGBA16F ? 8u : 4u);
    if (sameLayout){
        std::vector<uint8_t>& shown = target.binding->bytes.front();
        if (shown == bytes){
            // R13, R14: a stream that stopped changing goes back to a pooled immutable buffer
            if (target.dynamic && frame - target.lastChange > VC_SETTLE_FRAMES && !(layer && layer->live()))
                rebind(*mesh, target, colors.format, std::move(bytes), false);
            return;
        }
        if (target.dynamic){
            std::memcpy(shown.data(), bytes.data(), bytes.size());
            if (target.binding->uploadColor())
                stats.colorUploads++;
            target.lastChange = frame;
            return;
        }
        // the first change after it was bound: a stream of its own, written in place from now on (R14)
        rebind(*mesh, target, colors.format, std::move(bytes), true);
        target.lastChange = frame;
        return;
    }
    target.geometry = geometry;
    rebind(*mesh, target, colors.format, std::move(bytes), false);
}

void VertexColorRenderer::rebind(MeshComponent& mesh, TargetState& target, Format format, std::vector<uint8_t>&& bytes, bool dynamic){
    if (!target.sourceVertex.empty()){
        // the same split vertices, their colour stream DYNAMIC or not
        SplitResult split;
        split.split = true;
        split.sourceVertex = target.sourceVertex;
        split.values = target.authored;
        split.added = target.binding ? target.binding->splitVertices : 0u;
        const std::vector<std::pair<uint32_t, uint32_t>> levels = target.binding ? target.binding->levels : std::vector<std::pair<uint32_t, uint32_t>>();
        if (levels.empty())
            return;
        split.indices.assign(target.splitIndices.begin(), target.splitIndices.begin() + levels[0].second);
        for (size_t l = 1; l < levels.size(); l++)
            split.lodIndices.emplace_back(target.splitIndices.begin() + levels[l].first, target.splitIndices.begin() + levels[l].first + levels[l].second);
        target.binding = makeSplitBinding(mesh, target.submesh, format, split, std::move(bytes), dynamic);
    }else{
        const unsigned int count = (unsigned int)target.authored.size();
        target.binding = makeColorBinding(target.submesh, format, std::move(bytes), count, dynamic);
    }
    target.dynamic = dynamic;
    setBinding(target.meshEntity, target.submesh, target.binding);
}

// ---------------------------------------------------------------- queries

std::shared_ptr<const MeshView> VertexColorRenderer::getMeshView(Entity owner, Entity meshEntity, unsigned int submesh, MeshComponent& mesh){
    const uint64_t stamp = vertexcolor::detail::geometryStamp(mesh, submesh);
    auto found = views.find({meshEntity, submesh});
    if (found != views.end() && found->second.view && found->second.stamp == stamp)
        return found->second.view;

    // R83: a game that freed the model's bytes reads them back from its file, logged once
    bool released = false;
    for (unsigned int i = 0; i < mesh.numExternalBuffers; i++)
        released = released || mesh.eBuffers[i].isDataReleased();
    const auto start = std::chrono::steady_clock::now();
    if (released && !ModelCpuData::ensure(mesh, "vertex colors")){
        views.erase({meshEntity, submesh});
        return nullptr;
    }
    if (released){
        VertexColorsComponent* colors = scene->findComponent<VertexColorsComponent>(owner);
        if (colors && !colors->cpuGeometryLogged){
            colors->cpuGeometryLogged = true;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            Log::warn("Vertex Colors on '%s' read the model's geometry back from its file (%.1f ms): enable Read/Write in the "
                      "Model Importer to keep it in memory", scene->getEntityName(owner).c_str(), ms);
        }
    }

    auto view = std::make_shared<MeshView>();
    std::string error;
    if (!vertexcolor::detail::buildMeshView(mesh, submesh, *view, error)){
        views.erase({meshEntity, submesh});
        return nullptr;
    }
    stats.viewBuilds++;
    CachedView& cached = views[{meshEntity, submesh}];
    cached.stamp = vertexcolor::detail::geometryStamp(mesh, submesh);
    cached.view = view;
    return view;
}

uint32_t VertexColorRenderer::getBoundSplitVertices(Entity owner, const TargetKey& key) const{
    auto it = owners.find(owner);
    if (it == owners.end())
        return 0;
    for (const TargetState& target : it->second->targets){
        if (target.key == key)
            return target.binding ? target.binding->splitVertices : 0u;
    }
    return 0;
}

size_t VertexColorRenderer::getPendingSplits() const{
    size_t pending = 0;
    for (const auto& entry : owners){
        for (const TargetState& target : entry.second->targets)
            pending += target.job.isValid() ? 1 : 0;
    }
    return pending;
}

bool VertexColorRenderer::isBound(Entity owner, const TargetKey& key) const{
    auto it = owners.find(owner);
    if (it == owners.end())
        return false;
    for (const TargetState& target : it->second->targets){
        if (target.key == key)
            return target.binding != nullptr;
    }
    return false;
}
