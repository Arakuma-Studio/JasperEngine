// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ObjectRender.h"

#include "Engine.h"

using namespace jasperengine;

ObjectRender::ObjectRender(){ }

ObjectRender::ObjectRender(const ObjectRender& rhs) = default;

ObjectRender& ObjectRender::operator=(const ObjectRender& rhs) = default;

ObjectRender::~ObjectRender(){
    //Cannot destroy because its a handle
}

void ObjectRender::beginLoad(PrimitiveType primitiveType){
    this->primitiveType = primitiveType;
    backend.beginLoad(primitiveType);
}

void ObjectRender::setShader(ShaderRender* shader){
    backend.setShader(shader);
}

void ObjectRender::setIndex(BufferRender* buffer, AttributeDataType dataType, size_t offset){
    backend.setIndex(buffer, dataType, offset);
}

void ObjectRender::addAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance){
    backend.addAttribute(slot, buffer, elements, dataType, stride, offset, normalized, perInstance);
}

void ObjectRender::addInterleavedAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance){
    backend.addInterleavedAttribute(slot, buffer, elements, dataType, stride, offset, normalized, perInstance);
}

void ObjectRender::addStorageBuffer(int slot, ShaderStageType stage, BufferRender* buffer){
    backend.addStorageBuffer(slot, stage, buffer);
}

void ObjectRender::replaceVertexBuffer(BufferRender* original, BufferRender* replacement, size_t byteOffset){
    backend.replaceVertexBuffer(original->backend.get().id, replacement->backend.get(), byteOffset);
}

void ObjectRender::setIndexBuffer(BufferRender* buffer, size_t byteOffset){
    backend.setIndexBuffer(buffer->backend.get(), byteOffset);
}

void ObjectRender::resetIndexBuffer(){
    backend.resetIndexBuffer();
}

void ObjectRender::retargetVertexBuffer(uint32_t originalId, BufferRender* replacement){
    backend.retargetVertexBuffer(originalId, replacement->backend.get());
}

void ObjectRender::retargetIndexBuffer(uint32_t originalId, BufferRender* replacement){
    backend.retargetIndexBuffer(originalId, replacement->backend.get());
}

size_t ObjectRender::getVertexBindSlotCount() const{
    return backend.getVertexBindSlotCount();
}

void ObjectRender::getTextureBinding(std::pair<int, int> slot, uint32_t& view, uint32_t& sampler) const{
    backend.getTextureBinding(slot, view, sampler);
}

void ObjectRender::addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture){
    backend.addTexture(slot, stage, texture);
}

void ObjectRender::addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture, TextureRender* samplerOf){
    backend.addTexture(slot, stage, texture, samplerOf);
}

bool ObjectRender::isReady() const{
    return backend.isReady();
}

bool ObjectRender::isDrawWaiting() const{
    return backend.isDrawWaiting();
}

uint32_t ObjectRender::getWaitingDrawCount(){
    return SokolObject::getWaitingDrawCount();
}

bool ObjectRender::prewarmWith(ShaderRender* shader){
    return backend.prewarmWith(shader);
}

uint32_t ObjectRender::getPipelineId(PipelineType pipType) const{
    return backend.getPipelineId(pipType);
}

void ObjectRender::setImmediatePipelines(bool immediate){
    backend.setImmediatePipelines(immediate);
}

void ObjectRender::setBlendState(const BlendState& state){
    backend.setBlendState(state);
}

bool ObjectRender::endLoad(uint16_t pipelines, const RenderState& state, int msaaSamples){
    return backend.endLoad(pipelines, state, msaaSamples);
}

bool ObjectRender::endLoad(uint16_t pipelines, bool enableFaceCulling, bool enableDepthWrite, CullingMode cullingMode, WindingOrder windingOrder, bool prepassFaceCulling){
    return backend.endLoad(pipelines, RenderState(enableFaceCulling, enableDepthWrite, cullingMode, windingOrder, prepassFaceCulling));
}

bool ObjectRender::endLoadDynamic(PrimitiveType primitiveType, IndexType indexType){
    this->primitiveType = primitiveType;
    return backend.endLoadDynamic(primitiveType, indexType);
}

bool ObjectRender::beginDraw(const PipelineState& state){
    return backend.beginDraw(state);
}

bool ObjectRender::loadAlike(const ObjectRender& source, ShaderRender* shader){
    primitiveType = source.primitiveType;
    return backend.loadAlike(source.backend, shader);
}

void ObjectRender::shareBindings(const ObjectRender& source){
    backend.shareBindings(source.backend);
}

bool ObjectRender::beginAccumulationDraw(PipelineType colorType, bool mirrored){
    return backend.beginAccumulationDraw(colorType, mirrored);
}

bool ObjectRender::beginDraw(PipelineType pipType, bool disableFaceCulling, bool mirrored, ShaderRender* lodFadeShader){
    return backend.beginDraw(pipType, disableFaceCulling, mirrored, lodFadeShader);
}

bool ObjectRender::isLodFadeReady(PipelineType pipType, bool disableFaceCulling, bool mirrored, ShaderRender* lodFadeShader){
    return backend.isLodFadeReady(pipType, disableFaceCulling, mirrored, lodFadeShader);
}

uint32_t ObjectRender::getAppliedPipelineId() const{
    return backend.getAppliedPipelineId();
}

void ObjectRender::applyUniformBlock(int slot, unsigned int count, void* data){
    backend.applyUniformBlock(slot, count, data);
}

void ObjectRender::draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount){
    // a draw skipped for want of uniform or descriptor buffer space is not counted
    if (!backend.draw(baseElement, vertexCount, instanceCount))
        return;

    uint64_t triangles = 0;
    if (primitiveType == PrimitiveType::TRIANGLES)
        triangles = vertexCount / 3;
    else if (primitiveType == PrimitiveType::TRIANGLE_STRIP && vertexCount > 2)
        triangles = vertexCount - 2;
    const uint64_t instances = instanceCount > 0 ? instanceCount : 1;
    Engine::addDrawStats(instanceCount, triangles * instances, (uint64_t)vertexCount * instances);
}

void ObjectRender::addBatchInstanceAttributes(const int* locations, int count, BatchLayout layout){
    backend.addBatchInstanceAttributes(locations, count, layout);
}

bool ObjectRender::linkBatchVariant(const ObjectRender& source, const ShaderData& sourceShader, const ShaderData& variantShader,
                                    const std::string& objectBlock){
    return backend.linkBatchVariant(source.backend, sourceShader, variantShader, objectBlock);
}

bool ObjectRender::isBatchVariant() const{
    return backend.isBatchVariant();
}

const char* ObjectRender::getBatchLinkFailure() const{
    return backend.getBatchLinkFailure();
}

void ObjectRender::setNextDrawBatch(ObjectRender* variant, int objectSlot, float lodFade){
    backend.setNextDrawBatch(variant ? &variant->backend : nullptr, objectSlot, lodFade);
}

bool ObjectRender::bindStreamVertexBuffer(BufferRender* original, const void* data, size_t bytes){
    return backend.bindStreamVertexBuffer(original->backend.get().id, data, bytes);
}

uint32_t ObjectRender::getVertexBufferId() const{
    return backend.getVertexBufferId();
}

uint32_t ObjectRender::getIndexBufferId() const{
    return backend.getIndexBufferId();
}

void ObjectRender::destroy(){
    backend.destroy();

}

size_t ObjectRender::getSharedVariantCount(){
    return SokolObject::getSharedVariantCount();
}

int ObjectRender::getLivePipelineCount(){
    return SokolObject::getLivePipelineCount();
}

int ObjectRender::getUnfedVertexInputLoads(){
    return SokolObject::getUnfedVertexInputLoads();
}

int ObjectRender::getLoadingDescCount(){
    return SokolObject::getLoadingDescCount();
}
