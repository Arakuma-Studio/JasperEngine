// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PickGeometry.h"
#include "model/ModelCpuData.h"

#include "component/MeshComponent.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

using namespace jasperengine;

namespace {

    size_t indexSize(AttributeDataType type){
        return (type == AttributeDataType::UNSIGNED_SHORT || type == AttributeDataType::SHORT) ? 2u :
               (type == AttributeDataType::UNSIGNED_BYTE || type == AttributeDataType::BYTE) ? 1u : 4u;
    }

    inline uint32_t readIndex(const unsigned char* data, size_t size, uint32_t i){
        if (size == 2){
            uint16_t value;
            std::memcpy(&value, data + (size_t)i * 2u, 2);
            return value;
        }
        if (size == 1){
            return data[i];
        }
        uint32_t value;
        std::memcpy(&value, data + (size_t)i * 4u, 4);
        return value;
    }

    // the elements of an index range that fit in its buffer
    uint32_t fittingIndices(const PickSubmeshSource& source){
        if (!source.indexBuffer || !source.indexBuffer->getData()){
            return 0;
        }
        const size_t size = indexSize(source.indexType);
        const size_t bufferSize = source.indexBuffer->getSize();
        if (source.indexOffset >= bufferSize){
            return 0;
        }
        return (uint32_t)std::min<size_t>(source.drawCount, (bufferSize - source.indexOffset) / size);
    }

    // corners of triangle `t` of a submesh in its source's vertices: indices or vertex
    // numbers, a strip's odd triangles flipped (GL strip order)
    inline void sourceCorners(const PickSubmeshSource& source, const unsigned char* indexData, size_t indexElementSize,
                              uint32_t t, uint32_t corners[3]){
        uint32_t a, b, c;
        if (source.primitive == PrimitiveType::TRIANGLE_STRIP){
            a = t; b = t + 1; c = t + 2;
            if (t & 1u){
                std::swap(a, b);
            }
        }else{
            a = t * 3u; b = a + 1u; c = a + 2u;
        }
        if (indexData){
            corners[0] = readIndex(indexData, indexElementSize, a);
            corners[1] = readIndex(indexData, indexElementSize, b);
            corners[2] = readIndex(indexData, indexElementSize, c);
        }else{
            corners[0] = a;
            corners[1] = b;
            corners[2] = c;
        }
        // a corner outside the source: the triangle stays, degenerate
        if (corners[0] >= source.vertexCount || corners[1] >= source.vertexCount || corners[2] >= source.vertexCount){
            corners[0] = corners[1] = corners[2] = 0;
        }
    }

    // 64-bit word mixing (a SplitMix finalizer per word), two independent lanes
    inline uint64_t mixWord(uint64_t h, uint64_t word){
        h ^= word * 0x9e3779b97f4a7c15ull;
        h = (h ^ (h >> 29)) * 0xbf58476d1ce4e5b9ull;
        return h ^ (h >> 32);
    }

}


uint32_t PickGeometry::submeshOf(uint32_t triangle) const{
    if (submeshFirstTriangle.size() < 2){
        return 0;
    }
    // the last submesh whose first triangle is at or before it
    auto it = std::upper_bound(submeshFirstTriangle.begin(), submeshFirstTriangle.end() - 1, triangle);
    return it == submeshFirstTriangle.begin() ? 0u : (uint32_t)(it - submeshFirstTriangle.begin()) - 1u;
}

size_t PickGeometry::memoryBytes() const{
    return positions.capacity() * sizeof(Vector3) + indices.capacity() * sizeof(uint32_t) +
           submeshFirstTriangle.capacity() * sizeof(uint32_t);
}

void PickGeometry::clear(){
    positions.clear();
    indices.clear();
    submeshFirstTriangle.clear();
}

bool picking::resolveSources(const MeshComponent& mesh, PickGeometryLayout& layout){
    ModelCpuData::ensure(mesh, "picking");
    layout.submeshes.clear();
    layout.complete = false;

    // the buffers RenderSystem::loadMesh binds, by name
    std::map<std::string, const Buffer*> buffers;
    if (mesh.buffer.getSize() > 0){
        buffers["vertices"] = &mesh.buffer;
    }
    if (mesh.indices.getSize() > 0){
        buffers["indices"] = &mesh.indices;
    }
    for (unsigned int i = 0; i < mesh.numExternalBuffers; i++){
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
    }

    // what every submesh starts with: the render-attribute buffers (the last one with a
    // position, in name order, as the draw binds them)
    const Buffer* defaultPositionBuffer = nullptr;
    Attribute defaultPosition;
    const Buffer* defaultIndexBuffer = nullptr;
    Attribute defaultIndex;
    uint32_t defaultIndexCount = 0;
    for (const auto& entry : buffers){
        const Buffer* buffer = entry.second;
        if (!buffer->isRenderAttributes()){
            continue;
        }
        const std::map<AttributeType, Attribute> attributes = buffer->getAttributes();
        if (buffer->getType() == BufferType::INDEX_BUFFER){
            auto it = attributes.find(AttributeType::INDEX);
            if (it != attributes.end()){
                defaultIndexBuffer = buffer;
                defaultIndex = it->second;
                defaultIndexCount = buffer->getCount();
            }
        }else{
            auto it = attributes.find(AttributeType::POSITION);
            if (it != attributes.end()){
                defaultPositionBuffer = buffer;
                defaultPosition = it->second;
            }
        }
    }

    const uint32_t meshVertexCount = mesh.vertexCount > 0 ? mesh.vertexCount : mesh.buffer.getCount();

    bool complete = true;
    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
        const Submesh& submesh = mesh.submeshes[s];
        PickSubmeshSource source;
        source.primitive = submesh.primitiveType;

        const Buffer* positionBuffer = defaultPositionBuffer;
        Attribute position = defaultPosition;
        const Buffer* indexBuffer = defaultIndexBuffer;
        Attribute index = defaultIndex;
        uint32_t indexCount = defaultIndexCount;
        for (const auto& attribute : submesh.attributes){
            auto buffer = buffers.find(attribute.second.getBufferName());
            if (buffer == buffers.end()){
                continue;
            }
            if (attribute.first == AttributeType::POSITION){
                positionBuffer = buffer->second;
                position = attribute.second;
            }else if (attribute.first == AttributeType::INDEX){
                indexBuffer = buffer->second;
                index = attribute.second;
                indexCount = attribute.second.getCount();
            }
        }

        const bool triangles = source.primitive == PrimitiveType::TRIANGLES || source.primitive == PrimitiveType::TRIANGLE_STRIP;
        if (positionBuffer && position.getDataType() == AttributeDataType::FLOAT && position.getElements() >= 3 &&
                positionBuffer->getData() && positionBuffer->getStride() > 0){
            source.positionBuffer = positionBuffer;
            source.positionOffset = position.getOffset();
            source.positionStride = positionBuffer->getStride();
            uint32_t count = position.getCount() > 0 ? position.getCount() : positionBuffer->getCount();
            // only the vertices that fit in the buffer
            const size_t size = positionBuffer->getSize();
            if (source.positionOffset + 3 * sizeof(float) > size){
                count = 0;
            }else{
                const size_t fit = (size - source.positionOffset - 3 * sizeof(float)) / source.positionStride + 1;
                count = (uint32_t)std::min<size_t>(count, fit);
            }
            source.vertexCount = count;
            source.valid = true;
        }else if (triangles){
            complete = false;
        }

        if (indexBuffer){
            source.indexBuffer = indexBuffer;
            source.indexOffset = index.getOffset();
            source.indexType = index.getDataType();
            source.drawCount = indexCount;
            source.drawCount = fittingIndices(source);
        }else{
            source.drawCount = meshVertexCount;
        }

        layout.submeshes.push_back(source);
    }

    layout.complete = complete && !layout.submeshes.empty();
    return layout.complete;
}

uint32_t picking::countTriangles(const PickGeometryLayout& layout){
    uint64_t total = 0;
    for (const PickSubmeshSource& source : layout.submeshes){
        if (source.valid){
            total += primitiveTriangles(source.primitive, source.drawCount);
        }
    }
    return (uint32_t)std::min<uint64_t>(total, UINT32_MAX);
}

bool picking::gather(const MeshComponent& mesh, PickGeometry& geometry, PickGeometryLayout* layoutOut){
    geometry.clear();

    PickGeometryLayout localLayout;
    PickGeometryLayout& layout = layoutOut ? *layoutOut : localLayout;
    if (!resolveSources(mesh, layout)){
        return false;
    }

    // one block of positions per distinct source, shared by the submeshes that read it
    struct Block {
        const PickSubmeshSource* source;
        uint32_t base;
    };
    std::vector<Block> blocks;
    size_t vertexTotal = 0;
    for (PickSubmeshSource& source : layout.submeshes){
        if (!source.valid){
            continue;
        }
        auto it = std::find_if(blocks.begin(), blocks.end(), [&](const Block& block){
            return block.source->positionBuffer == source.positionBuffer && block.source->positionOffset == source.positionOffset &&
                   block.source->positionStride == source.positionStride;
        });
        if (it != blocks.end()){
            source.base = it->base;
            source.vertexCount = std::min(source.vertexCount, it->source->vertexCount);
            continue;
        }
        source.base = (uint32_t)vertexTotal;
        blocks.push_back({&source, source.base});
        vertexTotal += source.vertexCount;
    }

    geometry.positions.resize(vertexTotal);
    for (const Block& block : blocks){
        const PickSubmeshSource& source = *block.source;
        Vector3* out = geometry.positions.data() + block.base;
        const unsigned char* data = source.positionBuffer->getData() + source.positionOffset;
        if (source.positionStride == 3 * sizeof(float)){
            std::memcpy(static_cast<void*>(out), data, (size_t)source.vertexCount * sizeof(Vector3));
        }else{
            for (uint32_t v = 0; v < source.vertexCount; v++){
                float p[3];
                std::memcpy(p, data + (size_t)v * source.positionStride, sizeof(p));
                out[v] = Vector3(p[0], p[1], p[2]);
            }
        }
    }

    geometry.submeshFirstTriangle.reserve(layout.submeshes.size() + 1);
    geometry.indices.reserve((size_t)countTriangles(layout) * 3u);
    uint32_t triangleTotal = 0;
    for (const PickSubmeshSource& source : layout.submeshes){
        geometry.submeshFirstTriangle.push_back(triangleTotal);
        if (!source.valid){
            continue;
        }
        const uint32_t count = primitiveTriangles(source.primitive, source.drawCount);
        const unsigned char* indexData = source.indexBuffer ? source.indexBuffer->getData() + source.indexOffset : nullptr;
        const size_t elementSize = indexSize(source.indexType);

        if (indexData && source.primitive == PrimitiveType::TRIANGLES && elementSize == 4){
            // the common case: a 32-bit list, copied and checked in one pass
            const size_t first = geometry.indices.size();
            geometry.indices.resize(first + (size_t)count * 3u);
            uint32_t* out = geometry.indices.data() + first;
            std::memcpy(out, indexData, (size_t)count * 3u * sizeof(uint32_t));
            for (uint32_t t = 0; t < count; t++){
                uint32_t* corners = out + (size_t)t * 3u;
                if (corners[0] >= source.vertexCount || corners[1] >= source.vertexCount || corners[2] >= source.vertexCount){
                    corners[0] = corners[1] = corners[2] = 0;
                }
                corners[0] += source.base;
                corners[1] += source.base;
                corners[2] += source.base;
            }
        }else{
            for (uint32_t t = 0; t < count; t++){
                uint32_t corners[3];
                sourceCorners(source, indexData, elementSize, t, corners);
                geometry.indices.push_back(corners[0] + source.base);
                geometry.indices.push_back(corners[1] + source.base);
                geometry.indices.push_back(corners[2] + source.base);
            }
        }
        triangleTotal += count;
    }
    geometry.submeshFirstTriangle.push_back(triangleTotal);

    return true;
}

bool picking::triangleCorners(const PickGeometryLayout& layout, uint32_t triangle, uint32_t& submesh, uint32_t corners[3]){
    uint32_t first = 0;
    for (size_t s = 0; s < layout.submeshes.size(); s++){
        const PickSubmeshSource& source = layout.submeshes[s];
        const uint32_t count = source.valid ? primitiveTriangles(source.primitive, source.drawCount) : 0u;
        if (triangle < first + count){
            const unsigned char* indexData = source.indexBuffer ? source.indexBuffer->getData() + source.indexOffset : nullptr;
            sourceCorners(source, indexData, indexSize(source.indexType), triangle - first, corners);
            submesh = (uint32_t)s;
            return true;
        }
        first += count;
    }
    return false;
}

uint64_t picking::hashBytes(const void* data, size_t size, uint64_t seed){
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t lane0 = mixWord(seed, (uint64_t)size);
    uint64_t lane1 = mixWord(seed ^ 0x632be59bd9b4e019ull, ~(uint64_t)size);
    while (size >= 16){
        uint64_t a, b;
        std::memcpy(&a, p, 8);
        std::memcpy(&b, p + 8, 8);
        lane0 = mixWord(lane0, a);
        lane1 = mixWord(lane1, b);
        p += 16;
        size -= 16;
    }
    // the last 0..15 bytes, zero padded
    uint64_t tail[2] = {0, 0};
    std::memcpy(tail, p, size);
    lane0 = mixWord(lane0, tail[0]);
    lane1 = mixWord(lane1, tail[1]);
    return mixWord(lane0, lane1);
}

uint64_t picking::hashSources(const PickGeometryLayout& layout){
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (const PickSubmeshSource& source : layout.submeshes){
        const uint32_t header[4] = {(uint32_t)source.primitive, source.valid ? 1u : 0u, source.drawCount, source.vertexCount};
        h = hashBytes(header, sizeof(header), h);
        if (!source.valid){
            continue;
        }
        // the positions it reads: the whole strided range (other attributes interleaved in it
        // only cost a spurious rebuild when they change)
        if (source.vertexCount > 0){
            const size_t span = (size_t)(source.vertexCount - 1) * source.positionStride + 3 * sizeof(float);
            h = hashBytes(source.positionBuffer->getData() + source.positionOffset, span, h);
            const uint32_t stride = source.positionStride;
            h = hashBytes(&stride, sizeof(stride), h);
        }
        if (source.indexBuffer){
            const uint32_t type = (uint32_t)source.indexType;
            h = hashBytes(&type, sizeof(type), h);
            h = hashBytes(source.indexBuffer->getData() + source.indexOffset, (size_t)source.drawCount * indexSize(source.indexType), h);
        }
    }
    return h == 0 ? 1 : h;
}
