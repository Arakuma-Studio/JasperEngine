// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "RmlRenderer.h"

#include "Engine.h"
#include "Log.h"
#include "pool/ShaderPool.h"
#include "render/CameraRender.h"
#include "render/FramebufferRender.h"
#include "render/ShaderRender.h"
#include "render/SystemRender.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jasperengine {

    // RmlUi guarantees the spans stay valid and unchanged until ReleaseGeometry
    // (RenderInterface.h), so a compiled geometry is only a view of them
    struct RmlGeometry {
        Rml::Span<const Rml::Vertex> vertices;
        Rml::Span<const int> indices;
    };

    struct RmlTexture {
        TextureRender render;
        int width = 0;
        int height = 0;
        bool dead = false;    // its GPU texture went with the view (releaseGpuResources)
    };

}

using namespace jasperengine;

namespace {

    static_assert(sizeof(Rml::Vertex) == 20, "HTMLUI vertex layout expects 20-byte Rml::Vertex");
    static_assert(sizeof(Rml::Vector2f) == 8 && sizeof(Rml::ColourbPremultiplied) == 4, "HTMLUI vertex layout");

    const size_t initialVertexCapacity = 16384;
    const size_t initialIndexCapacity = 49152;

    // u_vs_htmluiParams
    struct HtmlUIParams {
        float transform[16];
        float translation[4];
    };
    static_assert(sizeof(HtmlUIParams) == 80, "u_vs_htmluiParams is 80 bytes");

    HtmlUIParams makeParams(const Rml::Matrix4f& transform, Rml::Vector2f translation){
        HtmlUIParams params;
        // column-major, like GLSL (RMLUI_MATRIX_ROW_MAJOR is not defined)
        std::memcpy(params.transform, transform.data(), sizeof(params.transform));
        params.translation[0] = translation.x;
        params.translation[1] = translation.y;
        params.translation[2] = 0.0f;
        params.translation[3] = 0.0f;
        return params;
    }

    bool sameRect(const Rect& a, const Rect& b){
        return a.getX() == b.getX() && a.getY() == b.getY() && a.getWidth() == b.getWidth() && a.getHeight() == b.getHeight();
    }

}

RmlRenderer::RmlRenderer(){
    projection = Rml::Matrix4f::Identity();
}

RmlRenderer::~RmlRenderer(){
    releaseGpuResources();

    // RmlUi gave every handle back before (HtmlUIContext::destroy); whatever is left would leak
    for (RmlGeometry* geometry : liveGeometry){
        delete geometry;
    }
    liveGeometry.clear();
    for (RmlTexture* texture : liveTextures){
        delete texture;
    }
    liveTextures.clear();
}

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices){
    RmlGeometry* geometry = new RmlGeometry{vertices, indices};
    liveGeometry.insert(geometry);
    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle textureHandle){
    if (!inFrame){
        if (!loggedOutsideFrame){
            Log::error("[HTML UI] geometry rendered outside a frame is ignored");
            loggedOutsideFrame = true;
        }
        return;
    }

    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
    if (!geometry || geometry->vertices.empty() || geometry->indices.empty())
        return;

    RmlTexture* texture = reinterpret_cast<RmlTexture*>(textureHandle);

    // one vertex and index stream per frame; indices rebased instead of a base vertex,
    // which GLES 3.0 and WebGL 2 lack
    const uint32_t baseVertex = (uint32_t)frameVertices.size();
    const uint32_t firstIndex = (uint32_t)frameIndices.size();
    frameVertices.insert(frameVertices.end(), geometry->vertices.begin(), geometry->vertices.end());
    frameIndices.reserve(frameIndices.size() + geometry->indices.size());
    for (int index : geometry->indices){
        frameIndices.push_back(baseVertex + (uint32_t)index);
    }

    DrawCommand command;
    command.firstIndex = firstIndex;
    command.indexCount = (uint32_t)geometry->indices.size();
    command.texture = texture;
    command.translation = translation;
    command.transformIndex = currentTransform;
    command.scissorEnabled = scissorEnabled;
    command.scissor = scissorEnabled ? toTargetScissor(scissorRegion, contextSize, targetSize, Engine::isOpenGL())
                                     : Rect(0, 0, (float)targetSize.x, (float)targetSize.y);
    commands.push_back(command);
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle){
    RmlGeometry* geometry = reinterpret_cast<RmlGeometry*>(handle);
    // what a frame recorded was copied into the frame's streams
    if (liveGeometry.erase(geometry) > 0){
        delete geometry;
    }
}

RmlTexture* RmlRenderer::createTexture(const uint8_t* rgba, int width, int height){
    RmlTexture* texture = new RmlTexture();
    texture->width = width;
    texture->height = height;

    void* data[1] = {const_cast<uint8_t*>(rgba)};
    size_t size[1] = {(size_t)width * (size_t)height * 4};
    // LINEAR min filter: no mipmaps
    if (!texture->render.createTexture("htmlui-image", width, height, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, data, size,
                                       TextureFilter::LINEAR, TextureFilter::LINEAR, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)){
        texture->render.destroyTexture();
        delete texture;
        return nullptr;
    }

    liveTextures.insert(texture);
    return texture;
}

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source){
    TextureData data;
    if (source.empty() || !data.loadTextureFromFile(source.c_str())){
        Log::error("[HTML UI] could not load image '%s'", source.c_str());
        return 0;
    }

    const int width = data.getWidth();
    const int height = data.getHeight();
    const int channels = data.getChannels();
    const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
    if (data.getColorFormat() == ColorFormat::RED16 || !pixels || width <= 0 || height <= 0 || channels < 1 || channels > 4){
        Log::error("[HTML UI] unsupported image format '%s'", source.c_str());
        return 0;
    }

    // RGBA8 with premultiplied alpha, row 0 at the top (RmlUi's texture coordinates).
    // The engine's TextureAlphaBorder fix is never applied: premultiplied colors need none
    const size_t count = (size_t)width * (size_t)height;
    std::vector<uint8_t> rgba(count * 4);
    for (size_t i = 0; i < count; i++){
        const unsigned char* p = pixels + i * channels;
        uint8_t r, g, b, a;
        if (channels == 1){
            r = g = b = p[0]; a = 255;
        }else if (channels == 2){
            r = g = b = p[0]; a = p[1];
        }else if (channels == 3){
            r = p[0]; g = p[1]; b = p[2]; a = 255;
        }else{
            r = p[0]; g = p[1]; b = p[2]; a = p[3];
        }
        uint8_t* out = &rgba[i * 4];
        out[0] = (uint8_t)((r * a + 127) / 255);
        out[1] = (uint8_t)((g * a + 127) / 255);
        out[2] = (uint8_t)((b * a + 127) / 255);
        out[3] = a;
    }

    RmlTexture* texture = createTexture(rgba.data(), width, height);
    if (!texture){
        Log::error("[HTML UI] could not create the texture of image '%s'", source.c_str());
        return 0;
    }

    texture_dimensions = Rml::Vector2i(width, height);
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions){
    // already premultiplied RGBA8 (font pages, RmlUi callback textures)
    if (source_dimensions.x <= 0 || source_dimensions.y <= 0 ||
        source.size() != (size_t)source_dimensions.x * (size_t)source_dimensions.y * 4){
        Log::error("[HTML UI] GenerateTexture: %d x %d does not match %u bytes of RGBA8", source_dimensions.x, source_dimensions.y, (unsigned int)source.size());
        return 0;
    }

    RmlTexture* texture = createTexture(source.data(), source_dimensions.x, source_dimensions.y);
    if (!texture){
        Log::error("[HTML UI] could not create a %d x %d texture", source_dimensions.x, source_dimensions.y);
        return 0;
    }
    return reinterpret_cast<Rml::TextureHandle>(texture);
}

void RmlRenderer::destroyTexture(RmlTexture* texture){
    if (!texture->dead){
        texture->render.destroyTexture();
    }
    liveTextures.erase(texture);
    delete texture;
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle handle){
    RmlTexture* texture = reinterpret_cast<RmlTexture*>(handle);
    if (!texture || liveTextures.count(texture) == 0)
        return;

    // draws recorded earlier in this frame may still use it (a font page replaced while
    // the context renders): it goes after the replay
    if (inFrame){
        if (std::find(pendingReleases.begin(), pendingReleases.end(), texture) == pendingReleases.end()){
            pendingReleases.push_back(texture);
        }
        return;
    }

    destroyTexture(texture);
}

void RmlRenderer::EnableScissorRegion(bool enable){
    scissorEnabled = enable;
}

void RmlRenderer::SetScissorRegion(Rml::Rectanglei region){
    scissorRegion = region;
}

void RmlRenderer::SetTransform(const Rml::Matrix4f* transform){
    if (!transform){
        currentTransform = 0;
        return;
    }
    transforms.push_back(projection * (*transform));
    currentTransform = (uint32_t)(transforms.size() - 1);
}

bool RmlRenderer::loadObject(ObjectRender& object, ShaderRender* shader, BufferRender* vertices, BufferRender* indices, bool textured, uint16_t pipelines){
    ShaderData& shaderData = shader->shaderData;
    const unsigned int stride = sizeof(Rml::Vertex);

    object.beginLoad(PrimitiveType::TRIANGLES);
    object.setShader(shader);
    // Rml::Vertex: position (2 floats), colour (4 bytes), tex_coord (2 floats). An
    // attribute the variant does not use has slot -1 and is skipped
    object.addAttribute(shaderData.getAttrIndex(AttributeType::POSITION), vertices, 2, AttributeDataType::FLOAT, stride, 0, false, false);
    object.addAttribute(shaderData.getAttrIndex(AttributeType::COLOR), vertices, 4, AttributeDataType::UNSIGNED_BYTE, stride, 8, true, false);
    object.addAttribute(shaderData.getAttrIndex(AttributeType::TEXCOORD1), vertices, 2, AttributeDataType::FLOAT, stride, 12, false, false);
    object.setIndex(indices, AttributeDataType::UNSIGNED_INT, 0);
    if (textured){
        object.addTexture(shaderData.getTextureIndexByName("u_htmluiTexture"), ShaderStageType::FRAGMENT, &placeholderWhite);
    }
    object.setBlendState(BlendState::premultipliedAlpha());
    // no culling: RmlUi geometry and transforms can wind either way
    return object.endLoad(pipelines, false, false, CullingMode::BACK, WindingOrder::CCW);
}

bool RmlRenderer::createStreamBuffers(size_t vertices, size_t indices){
    if (!vertexBuffer.createBuffer((unsigned int)(vertices * sizeof(Rml::Vertex)), nullptr, BufferType::VERTEX_BUFFER, BufferUsage::STREAM) ||
        !indexBuffer.createBuffer((unsigned int)(indices * sizeof(uint32_t)), nullptr, BufferType::INDEX_BUFFER, BufferUsage::STREAM)){
        Log::error("[HTML UI] could not create the geometry buffers (%u vertices, %u indices)", (unsigned int)vertices, (unsigned int)indices);
        return false;
    }
    vertexCapacity = vertices;
    indexCapacity = indices;

    // the objects bind the buffers, so they are built again with them
    if (!loadObject(colorObject, colorShader.get(), &vertexBuffer, &indexBuffer, false, PIP_RTT_NODEPTH) ||
        !loadObject(texturedObject, texturedShader.get(), &vertexBuffer, &indexBuffer, true, PIP_RTT_NODEPTH)){
        Log::error("[HTML UI] could not create the HTML UI pipelines");
        return false;
    }
    return true;
}

void RmlRenderer::destroyStreamObjects(){
    colorObject.destroy();
    texturedObject.destroy();
    vertexBuffer.destroyBuffer();
    indexBuffer.destroyBuffer();
    vertexCapacity = 0;
    indexCapacity = 0;
}

bool RmlRenderer::loadPipelines(){
    if (pipelinesLoaded)
        return true;
    if (!Engine::isViewLoaded())
        return false;

    colorShader = ShaderPool::get(ShaderType::HTMLUI, ShaderPool::getHtmlUIProperties(false));
    texturedShader = ShaderPool::get(ShaderType::HTMLUI, ShaderPool::getHtmlUIProperties(true));
    // still compiling (or failed, which ShaderPool logged): asked again next frame
    if (!colorShader || !colorShader->isCreated() || !texturedShader || !texturedShader->isCreated())
        return false;

    colorParamsSlot = colorShader->shaderData.getUniformBlockIndexByName("u_vs_htmluiParams");
    texturedParamsSlot = texturedShader->shaderData.getUniformBlockIndexByName("u_vs_htmluiParams");
    texturedTextureSlot = texturedShader->shaderData.getTextureIndexByName("u_htmluiTexture");

    const uint8_t white[4] = {255, 255, 255, 255};
    void* whiteData[1] = {const_cast<uint8_t*>(white)};
    size_t whiteSize[1] = {sizeof(white)};
    if (!placeholderWhite.createTexture("htmlui-white", 1, 1, ColorFormat::RGBA, TextureType::TEXTURE_2D, 1, whiteData, whiteSize,
                                        TextureFilter::NEAREST, TextureFilter::NEAREST, TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE)){
        Log::error("[HTML UI] could not create the placeholder texture");
        destroyPipelineObjects();
        return false;
    }

    // the composite: the document target as a quad in normalized device coordinates
    const Rml::ColourbPremultiplied white4(255, 255, 255, 255);
    const Rml::Vertex quad[4] = {
        {Rml::Vector2f(-1.0f,  1.0f), white4, Rml::Vector2f(0.0f, 0.0f)},
        {Rml::Vector2f( 1.0f,  1.0f), white4, Rml::Vector2f(1.0f, 0.0f)},
        {Rml::Vector2f( 1.0f, -1.0f), white4, Rml::Vector2f(1.0f, 1.0f)},
        {Rml::Vector2f(-1.0f, -1.0f), white4, Rml::Vector2f(0.0f, 1.0f)},
    };
    const uint32_t quadIndices[6] = {0, 1, 2, 0, 2, 3};
    if (!compositeVertexBuffer.createBuffer(sizeof(quad), (void*)quad, BufferType::VERTEX_BUFFER, BufferUsage::IMMUTABLE) ||
        !compositeIndexBuffer.createBuffer(sizeof(quadIndices), (void*)quadIndices, BufferType::INDEX_BUFFER, BufferUsage::IMMUTABLE) ||
        !loadObject(compositeObject, texturedShader.get(), &compositeVertexBuffer, &compositeIndexBuffer, true, PIP_DEFAULT_NODEPTH | PIP_RTT_NODEPTH)){
        Log::error("[HTML UI] could not create the HTML UI composite");
        destroyPipelineObjects();
        return false;
    }

    if (!createStreamBuffers(initialVertexCapacity, initialIndexCapacity)){
        destroyPipelineObjects();
        return false;
    }

    pipelinesLoaded = true;
    return true;
}

bool RmlRenderer::isReady() const{
    return pipelinesLoaded;
}

void RmlRenderer::clearFrame(){
    frameVertices.clear();
    frameIndices.clear();
    commands.clear();
    transforms.clear();
    currentTransform = 0;
    inFrame = false;
}

void RmlRenderer::beginFrame(Rml::Vector2i contextSize, Rml::Vector2i targetSize){
    this->contextSize = Rml::Vector2i(std::max(contextSize.x, 1), std::max(contextSize.y, 1));
    this->targetSize = Rml::Vector2i(std::max(targetSize.x, 1), std::max(targetSize.y, 1));

    // document pixels to clip space; the viewport maps them onto the target's pixels, which
    // is the resolution scaling. GL renders offscreen Y-flipped, so the target's row 0 is
    // the top of the document on every backend (RenderSystem::isRenderingFlipped)
    projection = Rml::Matrix4f::ProjectOrtho(0.0f, (float)this->contextSize.x, (float)this->contextSize.y, 0.0f, -10000.0f, 10000.0f);
    if (Engine::isOpenGL()){
        projection = Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) * projection;
    }

    frameVertices.clear();
    frameIndices.clear();
    commands.clear();
    transforms.clear();
    transforms.push_back(projection);
    currentTransform = 0;
    scissorEnabled = false;
    inFrame = true;
    framed = true;
    frameNumber = SystemRender::getFrameCount();
}

bool RmlRenderer::hasFrame(uint64_t frame) const{
    return framed && frameNumber == frame;
}

void RmlRenderer::flushPendingReleases(){
    std::vector<RmlTexture*> releases;
    releases.swap(pendingReleases);
    for (RmlTexture* texture : releases){
        if (liveTextures.count(texture)){
            destroyTexture(texture);
        }
    }
}

bool RmlRenderer::endFrame(FramebufferRender& target, CameraRender& pass){
    if (!inFrame){
        Log::error("[HTML UI] endFrame without beginFrame");
        return false;
    }

    if (!isReady() && !loadPipelines()){
        // shaders still compiling: the frame is dropped
        clearFrame();
        flushPendingReleases();
        return false;
    }

    // grow the streams (doubling) until the frame fits
    if (frameVertices.size() > vertexCapacity || frameIndices.size() > indexCapacity){
        size_t newVertices = vertexCapacity;
        size_t newIndices = indexCapacity;
        while (newVertices < frameVertices.size()) newVertices *= 2;
        while (newIndices < frameIndices.size()) newIndices *= 2;
        destroyStreamObjects();
        if (!createStreamBuffers(newVertices, newIndices)){
            destroyPipelineObjects();
            clearFrame();
            flushPendingReleases();
            return false;
        }
    }

    // one update of each stream buffer per frame (sokol's rule)
    if (!frameVertices.empty()){
        vertexBuffer.updateBuffer((unsigned int)(frameVertices.size() * sizeof(Rml::Vertex)), frameVertices.data());
        indexBuffer.updateBuffer((unsigned int)(frameIndices.size() * sizeof(uint32_t)), frameIndices.data());
    }

    const Rect fullTarget(0, 0, (float)targetSize.x, (float)targetSize.y);

    pass.setClearColor(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    pass.startRenderPass(&target);
    pass.applyViewport(fullTarget);
    pass.applyScissor(fullTarget);

    Rect appliedScissor = fullTarget;
    ObjectRender* appliedObject = nullptr;
    size_t drawCount = 0;

    for (const DrawCommand& command : commands){
        const Rect& scissor = command.scissor;
        // clipped away: sokol widens an empty scissor to 1x1 on Metal/Vulkan/WebGPU
        if (scissor.getWidth() <= 0.0f || scissor.getHeight() <= 0.0f)
            continue;

        if (!sameRect(scissor, appliedScissor)){
            pass.applyScissor(scissor);
            appliedScissor = scissor;
        }

        const bool textured = command.texture != nullptr;
        // a texture released (or lost with the view) since the draw was recorded is skipped
        if (textured && (liveTextures.count(command.texture) == 0 || command.texture->dead))
            continue;

        ObjectRender* object = textured ? &texturedObject : &colorObject;
        if (object != appliedObject){
            if (!object->beginDraw(PIP_RTT_NODEPTH)){
                appliedObject = nullptr;
                continue;
            }
            appliedObject = object;
        }

        if (textured){
            object->addTexture(texturedTextureSlot, ShaderStageType::FRAGMENT, &command.texture->render);
        }

        HtmlUIParams params = makeParams(transforms[command.transformIndex], command.translation);
        object->applyUniformBlock(textured ? texturedParamsSlot : colorParamsSlot, sizeof(HtmlUIParams), &params);
        object->draw(command.firstIndex, command.indexCount, 1);
        drawCount++;
    }

    pass.endRenderPass();

    lastFrameDrawCount = drawCount;
    clearFrame();
    flushPendingReleases();

    return true;
}

bool RmlRenderer::drawComposite(TextureRender& source, PipelineType compositePipeline, bool flipY){
    if (!isReady() || !source.isCreated())
        return false;

    if (!compositeObject.beginDraw(compositePipeline))
        return false;

    compositeObject.addTexture(texturedTextureSlot, ShaderStageType::FRAGMENT, &source);
    const Rml::Matrix4f transform = flipY ? Rml::Matrix4f::Scale(1.0f, -1.0f, 1.0f) : Rml::Matrix4f::Identity();
    HtmlUIParams params = makeParams(transform, Rml::Vector2f(0.0f, 0.0f));
    compositeObject.applyUniformBlock(texturedParamsSlot, sizeof(HtmlUIParams), &params);
    compositeObject.draw(0, 6, 1);

    return true;
}

void RmlRenderer::releaseGpuResources(){
    // a frame in progress is dropped with it
    if (inFrame){
        clearFrame();
    }
    flushPendingReleases();
    framed = false;

    destroyPipelineObjects();

    // the handles stay valid for RmlUi, which releases them (Rml::ReleaseTextures)
    for (RmlTexture* texture : liveTextures){
        if (!texture->dead){
            texture->render.destroyTexture();
            texture->dead = true;
        }
    }
}

void RmlRenderer::destroyPipelineObjects(){
    destroyStreamObjects();
    compositeObject.destroy();
    compositeVertexBuffer.destroyBuffer();
    compositeIndexBuffer.destroyBuffer();
    placeholderWhite.destroyTexture();

    if (colorShader){
        colorShader.reset();
        ShaderPool::remove(ShaderType::HTMLUI, ShaderPool::getHtmlUIProperties(false));
    }
    if (texturedShader){
        texturedShader.reset();
        ShaderPool::remove(ShaderType::HTMLUI, ShaderPool::getHtmlUIProperties(true));
    }
    colorParamsSlot = -1;
    texturedParamsSlot = -1;
    texturedTextureSlot = {-1, -1};
    pipelinesLoaded = false;
}

Rect RmlRenderer::toTargetScissor(Rml::Rectanglei region, Rml::Vector2i contextSize, Rml::Vector2i targetSize, bool openGLFlipped){
    const float sx = (contextSize.x > 0) ? (float)targetSize.x / (float)contextSize.x : 1.0f;
    const float sy = (contextSize.y > 0) ? (float)targetSize.y / (float)contextSize.y : 1.0f;

    auto clampTo = [](float value, int limit){
        return std::min(std::max(value, 0.0f), (float)std::max(limit, 0));
    };

    const float x0 = clampTo(std::floor(region.Left() * sx), targetSize.x);
    const float x1 = std::max(x0, clampTo(std::ceil(region.Right() * sx), targetSize.x));
    const float y0 = clampTo(std::floor(region.Top() * sy), targetSize.y);
    const float y1 = std::max(y0, clampTo(std::ceil(region.Bottom() * sy), targetSize.y));

    // CameraRender::applyScissor counts y from the bottom; GL's flipped offscreen rendering
    // puts the document top at y = 0 there
    const float y = openGLFlipped ? y0 : (float)targetSize.y - y1;
    return Rect(x0, y, x1 - x0, y1 - y0);
}

size_t RmlRenderer::getLiveGeometryCount() const{
    return liveGeometry.size();
}

size_t RmlRenderer::getLiveTextureCount() const{
    return liveTextures.size();
}

size_t RmlRenderer::getLastFrameDrawCount() const{
    return lastFrameDrawCount;
}

size_t RmlRenderer::getVertexCapacity() const{
    return vertexCapacity;
}

TextureRender* RmlRenderer::getTextureRender(Rml::TextureHandle handle) const{
    RmlTexture* texture = reinterpret_cast<RmlTexture*>(handle);
    if (!texture || liveTextures.count(texture) == 0 || texture->dead)
        return nullptr;
    return &texture->render;
}

void RmlRenderer::getGpuHandles(std::vector<uint32_t>& buffers, std::vector<uint32_t>& images) const{
    buffers.clear();
    images.clear();

    for (const BufferRender* buffer : {&vertexBuffer, &indexBuffer, &compositeVertexBuffer, &compositeIndexBuffer}){
        uint32_t id = const_cast<BufferRender*>(buffer)->backend.get().id;
        if (id != 0) buffers.push_back(id);
    }

    uint32_t placeholder = const_cast<TextureRender&>(placeholderWhite).backend.get().id;
    if (placeholder != 0) images.push_back(placeholder);
    for (RmlTexture* texture : liveTextures){
        uint32_t id = texture->render.backend.get().id;
        if (id != 0) images.push_back(id);
    }
}
