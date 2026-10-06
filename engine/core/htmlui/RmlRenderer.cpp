// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlRenderer.h"

#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/TextureRender.h"

using namespace jasperengine;

RmlRenderer::RmlRenderer(){
    // HTMLUI_SKELETON: p0-render
}

RmlRenderer::~RmlRenderer(){
    // HTMLUI_SKELETON: p0-render
}

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices){
    // HTMLUI_SKELETON: p0-render
    (void)vertices;
    (void)indices;
    return 0;
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture){
    // HTMLUI_SKELETON: p0-render
    (void)geometry;
    (void)translation;
    (void)texture;
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry){
    // HTMLUI_SKELETON: p0-render
    (void)geometry;
}

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source){
    // HTMLUI_SKELETON: p0-render
    (void)texture_dimensions;
    (void)source;
    return 0;
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions){
    // HTMLUI_SKELETON: p0-render
    (void)source;
    (void)source_dimensions;
    return 0;
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle texture){
    // HTMLUI_SKELETON: p0-render
    (void)texture;
}

void RmlRenderer::EnableScissorRegion(bool enable){
    // HTMLUI_SKELETON: p0-render
    (void)enable;
}

void RmlRenderer::SetScissorRegion(Rml::Rectanglei region){
    // HTMLUI_SKELETON: p0-render
    (void)region;
}

void RmlRenderer::SetTransform(const Rml::Matrix4f* transform){
    // HTMLUI_SKELETON: p0-render
    (void)transform;
}

bool RmlRenderer::loadPipelines(){
    // HTMLUI_SKELETON: p0-render
    return false;
}

bool RmlRenderer::isReady() const{
    // HTMLUI_SKELETON: p0-render
    return false;
}

void RmlRenderer::beginFrame(Rml::Vector2i contextSize, Rml::Vector2i targetSize){
    // HTMLUI_SKELETON: p0-render
    (void)contextSize;
    (void)targetSize;
}

bool RmlRenderer::endFrame(FramebufferRender& target, CameraRender& pass){
    // HTMLUI_SKELETON: p0-render
    (void)target;
    (void)pass;
    return false;
}

bool RmlRenderer::drawComposite(TextureRender& source, PipelineType compositePipeline, bool flipY){
    // HTMLUI_SKELETON: p0-render
    (void)source;
    (void)compositePipeline;
    (void)flipY;
    return false;
}

void RmlRenderer::releaseGpuResources(){
    // HTMLUI_SKELETON: p0-render
}

Rect RmlRenderer::toTargetScissor(Rml::Rectanglei region, Rml::Vector2i contextSize, Rml::Vector2i targetSize, bool openGLFlipped){
    // HTMLUI_SKELETON: p0-render
    (void)region;
    (void)contextSize;
    (void)targetSize;
    (void)openGLFlipped;
    return Rect(0, 0, 0, 0);
}

size_t RmlRenderer::getLiveGeometryCount() const{
    // HTMLUI_SKELETON: p0-render
    return 0;
}

size_t RmlRenderer::getLiveTextureCount() const{
    // HTMLUI_SKELETON: p0-render
    return 0;
}

size_t RmlRenderer::getLastFrameDrawCount() const{
    // HTMLUI_SKELETON: p0-render
    return 0;
}

size_t RmlRenderer::getVertexCapacity() const{
    // HTMLUI_SKELETON: p0-render
    return 0;
}
