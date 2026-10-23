// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlRenderer's geometry storage, shared by RmlRenderer.cpp and the batched replay
// (RmlRendererBatch.cpp). Not part of the renderer's interface.

#ifndef RMLRENDERERINTERNAL_H
#define RMLRENDERERINTERNAL_H

#include "render/BufferRender.h"

#include <RmlUi/Core/Matrix4.h>
#include <RmlUi/Core/Span.h>
#include <RmlUi/Core/Vertex.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace jasperengine {

    struct RmlGeometryPage;

    // u_vs_htmluiParams
    struct HtmlUIParams {
        float transform[16];
        float translation[4];
    };
    static_assert(sizeof(HtmlUIParams) == 80, "u_vs_htmluiParams is 80 bytes");

    inline HtmlUIParams makeParams(const Rml::Matrix4f& transform, Rml::Vector2f translation){
        HtmlUIParams params;
        // column-major, like GLSL (RMLUI_MATRIX_ROW_MAJOR is not defined)
        std::memcpy(params.transform, transform.data(), sizeof(params.transform));
        params.translation[0] = translation.x;
        params.translation[1] = translation.y;
        params.translation[2] = 0.0f;
        params.translation[3] = 0.0f;
        return params;
    }

    // A compiled RmlUi geometry. RmlUi keeps the spans valid and unchanged until
    // ReleaseGeometry (RenderInterface.h), so its data is read from them: into the frame's
    // transient buffers on the first draw, into persistent storage at the end of that frame,
    // and again after the GPU objects went with the view. Geometry the batched replay gathers
    // into its runs is read from the spans only.
    struct RmlGeometry {
        bool alive = false;         // compiled and not released (released ones wait for reuse)
        uint64_t generation = 0;    // unique per CompileGeometry: a reused struct is another geometry to the run cache
        Rml::Span<const Rml::Vertex> vertices;
        Rml::Span<const int> indices;
        // the vertices' bounding box (document px, before the translation), computed on first use
        bool boundsValid = false;
        float bounds[4] = {};       // min x, min y, max x, max y
        // persistent storage
        RmlGeometryPage* vertexPage = nullptr;
        RmlGeometryPage* indexPage = nullptr;
        size_t vertexOffset = 0;
        size_t indexOffset = 0;
        size_t vertexBytes = 0;     // allocated (aligned) sizes
        size_t indexBytes = 0;
        // this frame's transient copy (before persistence)
        uint64_t transientFrame = UINT64_MAX;
        BufferRender* transientVertices = nullptr;
        BufferRender* transientIndices = nullptr;
        size_t transientVertexOffset = 0;
        size_t transientIndexOffset = 0;
    };

    // Persistent geometry storage: a STREAM buffer (sokol's dynamic buffers wait for the GPU
    // to idle on every Vulkan update, stream buffers go through the per-frame staging buffer;
    // both keep their content until updated again). A page holds many small geometries,
    // bump-allocated in a CPU copy and uploaded whole, at most once a frame, at the end of a
    // frame; it starts over once all its geometries are released. A dedicated buffer holds
    // one large geometry.
    struct RmlGeometryPage {
        BufferRender buffer;
        size_t capacity = 0;
        bool index = false;
        bool dedicated = false;
        std::vector<uint8_t> shadow;            // pages: the CPU copy of [0, top)
        size_t top = 0;
        size_t uploadedTop = 0;
        size_t live = 0;                        // geometries in it
        uint64_t updateFrame = UINT64_MAX;      // frame of the last sg_update_buffer
        uint64_t releaseFrame = UINT64_MAX;     // frame its last geometry went: reused from the next
    };

    // geometry first drawn in a frame, appended (sokol starts every frame at offset 0)
    struct RmlTransientBuffer {
        BufferRender buffer;
        size_t capacity = 0;
        size_t used = 0;
        bool index = false;
        uint64_t frame = UINT64_MAX;            // the frame "used" counts
    };

}

#endif //RMLRENDERERINTERNAL_H
