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
        // A pass of the frame (post-processing, SSAO, composites, blits): its pipelines are
        // created at once and drawn without waiting for a GL warm draw (spec S32 §6.4). A skipped
        // pass would leave its target undefined for the pass reading it. Content (meshes,
        // particles, UI) leaves it off and is drawn once its pipelines are ready. Before endLoad.
        void setImmediatePipelines(bool immediate);
        // builds every pipeline type in pipelines with the render state; the PIP_MSAA* types
        // take msaaSamples (the post-processed camera's multisampled target, 2 or 4)
        bool endLoad(uint16_t pipelines, const RenderState& state, int msaaSamples = 1);
        bool endLoad(uint16_t pipelines, bool enableFaceCulling, bool enableDepthWrite, CullingMode cullingMode, WindingOrder windingOrder, bool prepassFaceCulling = true);
        // The dynamic path: records the shader, vertex layout and index type and creates no
        // pipeline; every draw then picks its state with beginDraw(const PipelineState&).
        // Bindings may be swapped (replaceVertexBuffer, setIndexBuffer, addTexture) between
        // beginDraw and draw. Not mixed with endLoad on the same object
        bool endLoadDynamic(PrimitiveType primitiveType, IndexType indexType);
        // Another variant of source's shader (the same vertex inputs, textures and blocks) drawn
        // on source's vertex layout, bindings and render state: the OIT accumulation render of a
        // submesh (RenderSystem). Creates no pipeline; beginAccumulationDraw makes its own
        bool loadAlike(const ObjectRender& source, ShaderRender* shader);
        // source's bindings as they are now (its material textures after a rebind)
        void shareBindings(const ObjectRender& source);
        // the weighted blended OIT accumulation draw: colorType's (PIP_RTT, PIP_MSAA) pipeline into
        // RGBA16F and R16F targets, one blend state for both, no depth write (mirrored: the face
        // winding reversed, as beginDraw's)
        bool beginAccumulationDraw(PipelineType colorType, bool mirrored = false);

        // disableFaceCulling draws with a variant of the pipeline that culls no face, built on
        // first use and released by destroy() (the editor's Scene view debug toggle; ignored
        // outside the editor). mirrored draws with a variant whose face winding is reversed,
        // built on first use too: a draw whose transform mirrors the mesh (its triangles turn on
        // screen) keeps culling its back faces and gl_FrontFacing true on its front
        // (RenderSystem::isMirroredDraw)
        // lodFadeShader draws with the pipeline's LOD cross-fade variant on that shader (the
        // submesh's MESH 'Lfd' variant, whose bindings are its shader's; spec S54 R6), made on
        // first use and shared like the mirrored one; false while it is not ready
        bool beginDraw(PipelineType pipType, bool disableFaceCulling = false, bool mirrored = false, ShaderRender* lodFadeShader = nullptr);
        // that fade variant made (asked for on the first call) and warm: a fade may start
        bool isLodFadeReady(PipelineType pipType, bool disableFaceCulling, bool mirrored, ShaderRender* lodFadeShader);
        // the backend id of the pipeline the last beginDraw applied (0: none; diagnostics, tests)
        uint32_t getAppliedPipelineId() const;
        // the pipeline of that state from the pipeline cache (created on first use), applied
        // unless it is the one applied last in this pass; false while the shader is not ready
        bool beginDraw(const PipelineState& state);
        void applyUniformBlock(int slot, unsigned int count, void* data);

        // Automatic GPU instancing (spec S55, SystemRender::beginDrawBatch). An object loaded on the
        // HAS_AUTO_INSTANCING variant of another's shader, with the other's attributes, then the
        // per-instance vectors of layout at these locations (after every other attribute):
        void addBatchInstanceAttributes(const int* locations, int count, BatchLayout layout);
        // after both loaded: true when this object can draw source's batches (objectBlock, the
        // block it reads per instance instead; the shaders' other blocks, textures and vertex
        // buffers the same)
        bool linkBatchVariant(const ObjectRender& source, const ShaderData& sourceShader, const ShaderData& variantShader,
                              const std::string& objectBlock);
        bool isBatchVariant() const;
        // what the last linkBatchVariant refused (null: linked or never tried; diagnostics)
        const char* getBatchLinkFailure() const;
        // the next draw() may join a batch of variant's (linked to this object), whose instances
        // read the block in objectSlot, with this LOD fade (spec S54 C3: -1..1, 0 none)
        void setNextDrawBatch(ObjectRender* variant, int objectSlot, float lodFade = 0.0f);
        // the per-instance data of this draw from the frame's stream buffer instead of original
        // (the same layout; spec S55: a shadow slot's terrain nodes): false when it does not fit
        bool bindStreamVertexBuffer(BufferRender* original, const void* data, size_t bytes);
        // the first vertex buffer and the index buffer bound now (sort keys)
        uint32_t getVertexBufferId() const;
        uint32_t getIndexBufferId() const;
        // baseElement skips that many elements of the bound index (or vertex) range,
        // used to draw a single sub-range of a submesh, like one tilemap chunk
        void draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount);

        void destroy();

        // every pipeline endLoad made is created (spec S32: none pending or failed)
        bool isReady() const;
        // the last beginDraw was false because a pipeline (or its shader) is still being created
        // or warmed (spec S32), not because it failed or is gone: draw nothing and wait, no reload
        bool isDrawWaiting() const;
        // the draws that waited so far (render thread): a pass whose result is kept for later
        // frames (a cached shadow slot, a probe capture) is drawn again when it grew meanwhile
        static uint32_t getWaitingDrawCount();
        // spec S32: makes the pipelines it will have on another variant of its shader (its
        // description, created where the backend creates them) and their GL warm draws with its
        // bindings; true once they are ready (or none is needed). An object drawing a stand-in or
        // an older variant loads again on the new one after that, its pipelines ready at once
        bool prewarmWith(ShaderRender* shader);
        // the backend handle id of a pipeline type (0: none), for diagnostics and tests
        uint32_t getPipelineId(PipelineType pipType) const;

        // pipelines made by every object and not destroyed yet (diagnostics)
        static int getLivePipelineCount();
        // the pipeline variants (mirrored draws, the editor's no-cull view) alive, shared by
        // every object drawing with the same description (diagnostics)
        static size_t getSharedVariantCount();
        // loads that left a vertex input of their shader without a buffer (diagnostics: zero
        // when every load binds what its shader reads)
        static int getUnfedVertexInputLoads();
        // objects holding a load's working description, only kept from beginLoad to endLoad
        // (diagnostics: zero once every object has loaded)
        static int getLoadingDescCount();
    };
}


#endif /* ObjectRender_h */
