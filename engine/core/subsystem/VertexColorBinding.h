// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// docs/specs/V1-vertex-colors.md R11, R12: what one painted submesh binds over its own geometry,
// made by VertexColorSystem and bound by RenderSystem::loadMesh (Submesh::vertexColors). It owns
// its buffers and their bytes; the shared geometry is never written.

#include "Export.h"
#include "buffer/Attribute.h"
#include "buffer/ExternalBuffer.h"
#include "render/Render.h"

#include <cstdint>
#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace jasperengine {

    struct JASPERENGINE_API SubmeshVertexColors {
        // R11: COLOR over the submesh's attributes; R12 (split): every vertex input and INDEX too
        std::map<AttributeType, Attribute> attributes;
        // the buffers those attributes name and the bytes they show (a deque: no buffer moves)
        std::deque<ExternalBuffer> buffers;
        std::deque<std::vector<uint8_t>> bytes;
        ExternalBuffer* color = nullptr;
        ExternalBuffer* index = nullptr;    // split only
        bool split = false;
        uint32_t splitVertices = 0;         // copies added (R12, R124)
        // split: element base and count of each detail level in `index`, level 0 first
        std::vector<std::pair<uint32_t, uint32_t>> levels;
        // set by RenderSystem::loadMesh: the submesh's own attributes, which the passes that read
        // no vertex colour bind (depth, shadows, SSAO, the G-buffer keep the shared buffers)
        std::map<AttributeType, Attribute> own;

        SubmeshVertexColors() = default;
        SubmeshVertexColors(const SubmeshVertexColors&) = delete;
        SubmeshVertexColors& operator=(const SubmeshVertexColors&) = delete;
        // releases the GPU buffers (main thread; off it the release is posted there)
        ~SubmeshVertexColors();

        // a buffer of these bytes (moved in), named for the submesh's map of buffers
        ExternalBuffer& addBuffer(const std::string& name, BufferType type, unsigned int stride, unsigned int count,
                                  std::vector<uint8_t>&& data, BufferUsage usage);
        // the GPU buffers go (RenderSystem::destroyMesh); the next load makes them again
        void releaseGpu();
        // a DYNAMIC colour stream's bytes to write before the frame's first pass
        // (RenderSystem::updateMeshBuffers): a dynamic buffer is made empty, and an edit writes in place
        bool needUpload = false;
        // the colour stream's bytes changed in place: written before the next pass that draws them
        // (false: it is not dynamic)
        bool uploadColor();
    };

}
