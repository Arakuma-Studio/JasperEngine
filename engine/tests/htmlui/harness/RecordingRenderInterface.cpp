// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RecordingRenderInterface.h"

#include "io/Data.h"
#include "stb_image.h"

using namespace jasperengine;
using namespace jasperengine::test;

RecordingRenderInterface::~RecordingRenderInterface(){
}

Rml::CompiledGeometryHandle RecordingRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices){
    const uintptr_t handle = nextHandle++;
    Geometry& geometry = geometries[handle];
    geometry.vertices.assign(vertices.begin(), vertices.end());
    geometry.indices.assign(indices.begin(), indices.end());
    compiledTotal++;
    return static_cast<Rml::CompiledGeometryHandle>(handle);
}

void RecordingRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture){
    Draw draw;
    draw.geometry = geometry;
    auto it = geometries.find(static_cast<uintptr_t>(geometry));
    if (it != geometries.end()){
        draw.vertexCount = it->second.vertices.size();
        draw.indexCount = it->second.indices.size();
    }
    draw.translation = translation;
    draw.texture = texture;
    draw.scissorEnabled = scissorEnabled;
    draw.scissor = scissor;
    draw.hasTransform = hasTransform;
    draw.transform = transform;
    draws.push_back(draw);
}

void RecordingRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry){
    geometries.erase(static_cast<uintptr_t>(geometry));
}

Rml::TextureHandle RecordingRenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source){
    loadRequests.push_back(source);

    Data data;
    if (data.open(source.c_str()) != FileErrors::FILEDATA_OK || data.length() == 0)
        return 0;

    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data.getMemPtr(), int(data.length()), &width, &height, &channels))
        return 0;

    const uintptr_t handle = nextHandle++;
    Texture& texture = textures[handle];
    texture.source = source;
    texture.dimensions = Rml::Vector2i(width, height);
    texture_dimensions = texture.dimensions;
    return static_cast<Rml::TextureHandle>(handle);
}

Rml::TextureHandle RecordingRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions){
    const uintptr_t handle = nextHandle++;
    Texture& texture = textures[handle];
    texture.dimensions = source_dimensions;
    texture.pixels.assign(source.begin(), source.end());
    return static_cast<Rml::TextureHandle>(handle);
}

void RecordingRenderInterface::ReleaseTexture(Rml::TextureHandle texture){
    textures.erase(static_cast<uintptr_t>(texture));
}

void RecordingRenderInterface::EnableScissorRegion(bool enable){
    scissorEnabled = enable;
}

void RecordingRenderInterface::SetScissorRegion(Rml::Rectanglei region){
    scissor = region;
}

void RecordingRenderInterface::SetTransform(const Rml::Matrix4f* newTransform){
    hasTransform = newTransform != nullptr;
    transform = newTransform ? *newTransform : Rml::Matrix4f::Identity();
}

void RecordingRenderInterface::beginFrame(){
    draws.clear();
}

const std::vector<RecordingRenderInterface::Draw>& RecordingRenderInterface::getDraws() const{
    return draws;
}

size_t RecordingRenderInterface::getLiveGeometryCount() const{
    return geometries.size();
}

size_t RecordingRenderInterface::getLiveTextureCount() const{
    return textures.size();
}

size_t RecordingRenderInterface::getCompiledGeometryTotal() const{
    return compiledTotal;
}

const RecordingRenderInterface::Geometry* RecordingRenderInterface::getGeometry(Rml::CompiledGeometryHandle handle) const{
    auto it = geometries.find(static_cast<uintptr_t>(handle));
    return it != geometries.end() ? &it->second : nullptr;
}

const RecordingRenderInterface::Texture* RecordingRenderInterface::getTexture(Rml::TextureHandle handle) const{
    auto it = textures.find(static_cast<uintptr_t>(handle));
    return it != textures.end() ? &it->second : nullptr;
}

const std::vector<std::string>& RecordingRenderInterface::getLoadRequests() const{
    return loadRequests;
}
