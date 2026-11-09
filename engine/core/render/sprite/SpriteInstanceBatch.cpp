// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SpriteInstanceBatch.h"
#include "component/SpriteBatchComponent.h"

#include "profiler/Profile.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

SpriteInstanceBatch::SpriteInstanceBatch(){
    instanceBuffer.clear();
    instanceBuffer.addAttribute(AttributeType::INSTANCEMATRIXCOL1, 4, 0, true);
    instanceBuffer.addAttribute(AttributeType::INSTANCEMATRIXCOL2, 4, 4 * sizeof(float), true);
    instanceBuffer.addAttribute(AttributeType::INSTANCEMATRIXCOL3, 4, 8 * sizeof(float), true);
    instanceBuffer.addAttribute(AttributeType::INSTANCEMATRIXCOL4, 4, 12 * sizeof(float), true);
    instanceBuffer.addAttribute(AttributeType::INSTANCECOLOR, 4, 16 * sizeof(float), true);
    instanceBuffer.addAttribute(AttributeType::INSTANCETEXTURERECT, 4, 20 * sizeof(float), true);
    instanceBuffer.setStride(sizeof(InstanceRenderData));
    instanceBuffer.setRenderAttributes(true);
    instanceBuffer.setInstanceBuffer(true);
    instanceBuffer.setUsage(BufferUsage::STREAM);

    fxBuffer.clear();
    fxBuffer.addAttribute(AttributeType::INSTANCESPRITEFX, 4, 0, true);
    fxBuffer.setStride(sizeof(Vector4));
    fxBuffer.setRenderAttributes(true);
    fxBuffer.setInstanceBuffer(true);
    fxBuffer.setUsage(BufferUsage::STREAM);

    pivotBuffer.clear();
    pivotBuffer.addAttribute(AttributeType::INSTANCESNAPPIVOT, 2, 0, true);
    pivotBuffer.setStride(sizeof(Vector2));
    pivotBuffer.setRenderAttributes(true);
    pivotBuffer.setInstanceBuffer(true);
    pivotBuffer.setUsage(BufferUsage::STREAM);
}

SpriteInstanceBatch::~SpriteInstanceBatch(){
    destroyGpu();
}

void SpriteInstanceBatch::reserve(uint32_t value){
    if (value <= capacity)
        return;
    capacity = value;
    cpuInstances.resize(capacity);
    if (fxEnabled)
        cpuFx.resize(capacity, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    if (pivotsEnabled)
        cpuPivots.resize(capacity, Vector2(0.0f, 0.0f));
}

void SpriteInstanceBatch::setCount(uint32_t value){
    if (value > capacity){
        // amortised: a batch filled one instance at a time does not reallocate each time
        reserve(std::max(value, capacity + capacity / 2));
    }
    count = value;
}

void SpriteInstanceBatch::enableFx(){
    if (fxEnabled)
        return;
    fxEnabled = true;
    cpuFx.assign(capacity, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
}

void SpriteInstanceBatch::setPivotsEnabled(bool enabled){
    if (pivotsEnabled == enabled)
        return;
    pivotsEnabled = enabled;
    if (enabled)
        cpuPivots.assign(capacity, Vector2(0.0f, 0.0f));
    else
        cpuPivots.clear();
}

void SpriteInstanceBatch::markChanged(float x0, float y0, float x1, float y1){
    revision++;
    bounds.setNull();
    // the corners of the quad through each instance matrix (2D: z is the translation's)
    const float xs[2] = {x0, x1};
    const float ys[2] = {y0, y1};
    for (uint32_t i = 0; i < count; i++){
        const Matrix4& m = cpuInstances[i].instanceMatrix;
        for (float x : xs){
            for (float y : ys){
                bounds.merge(Vector3(m[0][0] * x + m[1][0] * y + m[3][0], m[0][1] * x + m[1][1] * y + m[3][1],
                                     m[0][2] * x + m[1][2] * y + m[3][2]));
            }
        }
    }
}

void SpriteInstanceBatch::setPageRuns(std::span<const SpritePageRun> runs){
    pageRuns.assign(runs.begin(), runs.end());
}

void SpriteInstanceBatch::setPages(const std::vector<std::string>& paths, const Texture& sampler){
    bool same = paths.size() == pageTextures.size();
    for (size_t i = 0; same && i < paths.size(); i++)
        same = pageTextures[i].getPath() == paths[i];
    if (same)
        return;
    pageTextures.clear();
    pageTextures.reserve(paths.size());
    for (const std::string& path : paths){
        pageTextures.push_back(sampler);
        pageTextures.back().setPath(path);
    }
}

bool SpriteInstanceBatch::createGpu(){
    destroyGpu();
    const uint32_t room = std::max<uint32_t>(capacity, 1);
    JE_GPU_ALLOC_SCOPE(Meshes, "Sprite Batch");
    if (!instanceBuffer.getRender()->createBuffer(room * (unsigned int)sizeof(InstanceRenderData), nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM))
        return false;
    gpuFx = fxEnabled;
    if (gpuFx && !fxBuffer.getRender()->createBuffer(room * (unsigned int)sizeof(Vector4), nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM)){
        instanceBuffer.getRender()->destroyBuffer();
        gpuFx = false;
        return false;
    }
    gpuPivots = pivotsEnabled;
    if (gpuPivots && !pivotBuffer.getRender()->createBuffer(room * (unsigned int)sizeof(Vector2), nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM)){
        destroyGpu();
        return false;
    }
    gpuCapacity = room;
    uploadedRevision = 0;
    uploadFrame = 0;
    return true;
}

void SpriteInstanceBatch::destroyGpu(){
    if (instanceBuffer.getRender()->isCreated())
        instanceBuffer.getRender()->destroyBuffer();
    if (fxBuffer.getRender()->isCreated())
        fxBuffer.getRender()->destroyBuffer();
    if (pivotBuffer.getRender()->isCreated())
        pivotBuffer.getRender()->destroyBuffer();
    gpuCapacity = 0;
    gpuFx = false;
    gpuPivots = false;
    uploadedRevision = 0;
    uploadFrame = 0;
}

bool SpriteInstanceBatch::isGpuCreated() const{
    return gpuCapacity > 0;
}

bool SpriteInstanceBatch::needsUpload(uint64_t frame) const{
    if (count == 0 || uploadFrame == frame)
        return false;
    return storage == Storage::PerFrame || uploadedRevision != revision;
}

size_t SpriteInstanceBatch::upload(uint64_t frame){
    // (sokol takes one update of a buffer a frame: a second request this frame keeps the first)
    if (count == 0 || count > gpuCapacity || uploadFrame == frame)
        return 0;
    const unsigned int bytes = count * (unsigned int)sizeof(InstanceRenderData);
    if (!instanceBuffer.getRender()->updateBuffer(bytes, cpuInstances.data()))
        return 0;
    size_t written = bytes;
    if (gpuFx && fxEnabled){
        const unsigned int fxBytes = count * (unsigned int)sizeof(Vector4);
        if (fxBuffer.getRender()->updateBuffer(fxBytes, cpuFx.data()))
            written += fxBytes;
    }
    if (gpuPivots && pivotsEnabled){
        const unsigned int pivotBytes = count * (unsigned int)sizeof(Vector2);
        if (pivotBuffer.getRender()->updateBuffer(pivotBytes, cpuPivots.data()))
            written += pivotBytes;
    }
    uploadedRevision = revision;
    uploadFrame = frame;
    return written;
}

//-----SpriteBatchState (component/SpriteBatchComponent.h)-----

SpriteBatchState::SpriteBatchState() = default;

SpriteBatchState::~SpriteBatchState() = default;

void SpriteBatchState::resize(uint32_t newCount){
    positions.resize(newCount, Vector3(0.0f, 0.0f, 0.0f));
    rotations.resize(newCount, 0.0f);
    scales.resize(newCount, Vector2(1.0f, 1.0f));
    frames.resize(newCount, 0);
    colors.resize(newCount, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
    if (hasFx)
        fx.resize(newCount, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    count = newCount;
}

void SpriteBatchState::clear(){
    // the streams keep their storage: a Clear Each Frame batch refilled every frame does not allocate
    positions.clear();
    rotations.clear();
    scales.clear();
    frames.clear();
    colors.clear();
    fx.clear();
    count = 0;
    revision++;
}
