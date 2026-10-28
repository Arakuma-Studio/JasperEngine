// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ObjectRender_h
#define ObjectRender_h

#include "texture/Texture.h"
#include "Render.h"
#include "PipelineState.h"

#include "sokol/SokolObject.h"

namespace jasperengine {

    class JASPERENGINE_API ObjectRender{

    public:

        //***Backend***
        SokolObject backend;
        //***

        PrimitiveType primitiveType = PrimitiveType::TRIANGLES;

        ObjectRender();
        ObjectRender(const ObjectRender& rhs);
        ObjectRender& operator=(const ObjectRender& rhs);
        virtual ~ObjectRender();

        void beginLoad(PrimitiveType primitiveType);
        void setShader(ShaderRender* shader);
        void setIndex(BufferRender* buffer, AttributeDataType dataType, size_t offset);
        void addAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance);
        // one bind slot for every attribute of the buffer on every backend (offset inside the
        // stride, below 2048): layouts with more attributes than vertex buffer bind slots
        void addInterleavedAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance);
        void addStorageBuffer(int slot, ShaderStageType stage, BufferRender* buffer);
        // swap the vertex buffer bound at load (original) for one of identical layout
        // (replacement), just before draw — used to bind a per-view instance buffer
        void replaceVertexBuffer(BufferRender* original, BufferRender* replacement, size_t byteOffset = 0);
        // draw from another index buffer of the same element type (a LOD range), from byteOffset;
        // resetIndexBuffer returns to the one bound at load
        void setIndexBuffer(BufferRender* buffer, size_t byteOffset = 0);
        void resetIndexBuffer();
        void addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture);
        // the texture through the sampler (filter, wrap) of another one: a pass that needs
        // bilinear taps of a target the user made NEAREST
        void addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture, TextureRender* samplerOf);
        // color blending of the color pipelines (PIP_DEFAULT*, PIP_RTT*, PIP_MSAA*, PIP_EFFECT)
        // instead of the render state's blend; between beginLoad and endLoad
        void setBlendState(const BlendState& state);
        // builds every pipeline type in pipelines with the render state; the PIP_MSAA* types
        // take msaaSamples (the post-processed camera's multisampled target, 2 or 4)
        bool endLoad(uint16_t pipelines, const RenderState& state, int msaaSamples = 1);
        bool endLoad(uint16_t pipelines, bool enableFaceCulling, bool enableDepthWrite, CullingMode cullingMode, WindingOrder windingOrder, bool prepassFaceCulling = true);
        // The dynamic path: records the shader, vertex layout and index type and creates no
        // pipeline; every draw then picks its state with beginDraw(const PipelineState&).
        // Bindings may be swapped (replaceVertexBuffer, setIndexBuffer, addTexture) between
        // beginDraw and draw. Not mixed with endLoad on the same object
        bool endLoadDynamic(PrimitiveType primitiveType, IndexType indexType);

        // disableFaceCulling draws with a variant of the pipeline that culls no face, built on
        // first use and released by destroy() (the editor's Scene view debug toggle; ignored
        // outside the editor)
        bool beginDraw(PipelineType pipType, bool disableFaceCulling = false);
        // the pipeline of that state from the pipeline cache (created on first use), applied
        // unless it is the one applied last in this pass; false while the shader is not ready
        bool beginDraw(const PipelineState& state);
        void applyUniformBlock(int slot, unsigned int count, void* data);
        // baseElement skips that many elements of the bound index (or vertex) range,
        // used to draw a single sub-range of a submesh, like one tilemap chunk
        void draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount);

        void destroy();

        // pipelines made by every object and not destroyed yet (diagnostics)
        static int getLivePipelineCount();
    };
}


#endif /* ObjectRender_h */
