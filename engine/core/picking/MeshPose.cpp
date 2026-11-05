// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshPose.h"
#include "model/ModelCpuData.h"

#include "component/MeshComponent.h"
#include "jobs/JobSystem.h"
#include "texture/TexturePrepare.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

using namespace jasperengine;
using namespace jasperengine::picking;

namespace {

    size_t typeSize(AttributeDataType type){
        switch (type){
            case AttributeDataType::BYTE:
            case AttributeDataType::UNSIGNED_BYTE: return 1;
            case AttributeDataType::SHORT:
            case AttributeDataType::UNSIGNED_SHORT:
            case AttributeDataType::HALF: return 2;
            default: return 4;
        }
    }

    // an attribute's layout, read once: the per-vertex reads of a pose are plain arithmetic
    struct AttributeSource {
        const Buffer* buffer = nullptr;
        Attribute attribute;
        bool valid = false;
        const unsigned char* data = nullptr;
        size_t bufferSize = 0;
        size_t stride = 0;
        size_t offset = 0;
        size_t elementSize = 4;
        uint32_t elements = 0;
        AttributeDataType type = AttributeDataType::FLOAT;
        bool normalized = false;

        void cache(){
            if (!valid){
                return;
            }
            data = buffer->getData();
            bufferSize = buffer->getSize();
            type = attribute.getDataType();
            elementSize = typeSize(type);
            elements = attribute.getElements();
            stride = buffer->getStride() > 0 ? buffer->getStride() : elementSize * elements;
            offset = attribute.getOffset();
            // as the renderer binds it: every unsigned short attribute normalized (Sokol has no
            // USHORTn format, SokolObject::getVertexFormat), which glTF joints scale back with
            // normAdjustJoint 65535 (MeshSystem::loadGLTF)
            normalized = attribute.getNormalized() || type == AttributeDataType::UNSIGNED_SHORT;
        }
    };

    // the buffer and attribute a submesh draws `type` from: its own attribute, else the
    // render-attribute buffers' (the last in name order), as RenderSystem binds them
    AttributeSource findAttribute(const MeshComponent& mesh, const Submesh& submesh, AttributeType type){
        ModelCpuData::ensure(mesh, "picking");
        AttributeSource found;
        auto byName = [&](const std::string& name) -> const Buffer* {
            if (name == "vertices") return mesh.buffer.getSize() > 0 ? &mesh.buffer : nullptr;
            if (name == "indices") return mesh.indices.getSize() > 0 ? &mesh.indices : nullptr;
            for (unsigned int i = 0; i < mesh.numExternalBuffers; i++){
                if (mesh.eBuffers[i].getName() == name) return &mesh.eBuffers[i];
            }
            return nullptr;
        };

        auto own = submesh.attributes.find(type);
        if (own != submesh.attributes.end()){
            if (const Buffer* buffer = byName(own->second.getBufferName())){
                found.buffer = buffer;
                found.attribute = own->second;
                found.valid = buffer->getData() != nullptr;
            }
            found.cache();
            return found;
        }

        std::map<std::string, const Buffer*> buffers;
        if (mesh.buffer.getSize() > 0) buffers["vertices"] = &mesh.buffer;
        for (unsigned int i = 0; i < mesh.numExternalBuffers; i++){
            buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
        }
        for (const auto& entry : buffers){
            if (!entry.second->isRenderAttributes() || entry.second->getType() == BufferType::INDEX_BUFFER){
                continue;
            }
            const std::map<AttributeType, Attribute> attributes = entry.second->getAttributes();
            auto it = attributes.find(type);
            if (it != attributes.end()){
                found.buffer = entry.second;
                found.attribute = it->second;
                found.valid = entry.second->getData() != nullptr;
            }
        }
        found.cache();
        return found;
    }

    // element `e` of vertex `v` as the vertex shader reads it (normalized integers scaled to
    // [0, 1] or [-1, 1]); 0 past the buffer
    inline float readElement(const AttributeSource& source, uint32_t v, uint32_t e){
        const size_t size = source.elementSize;
        const size_t position = (size_t)v * source.stride + source.offset + (size_t)e * size;
        if (e >= source.elements || position + size > source.bufferSize){
            return 0.0f;
        }
        const unsigned char* data = source.data + position;
        const bool normalized = source.normalized;
        switch (source.type){
            case AttributeDataType::BYTE:{
                int8_t value; std::memcpy(&value, data, 1);
                return normalized ? std::max((float)value / 127.0f, -1.0f) : (float)value;
            }
            case AttributeDataType::UNSIGNED_BYTE:{
                uint8_t value = *data;
                return normalized ? (float)value / 255.0f : (float)value;
            }
            case AttributeDataType::SHORT:{
                int16_t value; std::memcpy(&value, data, 2);
                return normalized ? std::max((float)value / 32767.0f, -1.0f) : (float)value;
            }
            case AttributeDataType::UNSIGNED_SHORT:{
                uint16_t value; std::memcpy(&value, data, 2);
                return normalized ? (float)value / 65535.0f : (float)value;
            }
            case AttributeDataType::INT:{
                int32_t value; std::memcpy(&value, data, 4);
                return (float)value;
            }
            case AttributeDataType::UNSIGNED_INT:{
                uint32_t value; std::memcpy(&value, data, 4);
                return (float)value;
            }
            case AttributeDataType::FLOAT:{
                float value; std::memcpy(&value, data, 4);
                return value;
            }
            case AttributeDataType::HALF:{
                uint16_t value; std::memcpy(&value, data, 2);
                return halfToFloat(value);
            }
        }
        return 0.0f;
    }

    // elements 0..count-1 of vertex `v`, as readElement reads each: the type switch and the
    // bounds test once for the vertex when all of them are in the buffer (the posing loop, R30)
    template<uint32_t COUNT>
    inline void readElements(const AttributeSource& source, uint32_t v, float* out){
        const size_t size = source.elementSize;
        const size_t position = (size_t)v * source.stride + source.offset;
        if (source.elements < COUNT || position + COUNT * size > source.bufferSize){
            for (uint32_t e = 0; e < COUNT; e++){
                out[e] = readElement(source, v, e);
            }
            return;
        }
        const unsigned char* data = source.data + position;
        switch (source.type){
            case AttributeDataType::FLOAT:
                std::memcpy(out, data, COUNT * sizeof(float));
                return;
            case AttributeDataType::UNSIGNED_BYTE:
                for (uint32_t e = 0; e < COUNT; e++){
                    out[e] = source.normalized ? (float)data[e] / 255.0f : (float)data[e];
                }
                return;
            case AttributeDataType::UNSIGNED_SHORT:
                for (uint32_t e = 0; e < COUNT; e++){
                    uint16_t value; std::memcpy(&value, data + e * 2, 2);
                    out[e] = source.normalized ? (float)value / 65535.0f : (float)value;
                }
                return;
            default:
                for (uint32_t e = 0; e < COUNT; e++){
                    out[e] = readElement(source, v, e);
                }
                return;
        }
    }

    Vector3 readVector3(const AttributeSource& source, uint32_t v){
        float value[3];
        readElements<3>(source, v, value);
        return Vector3(value[0], value[1], value[2]);
    }

    // the morph targets a submesh's shader variant declares (morphtarget.glsl): 8 position
    // targets, 4 with morph normals or tangents, 2 with both
    uint32_t activeMorphTargets(const Submesh& submesh){
        if (!submesh.hasMorphTarget){
            return 0;
        }
        uint32_t count = 8;
        if (submesh.hasMorphNormal && submesh.hasMorphTangent){
            count = 2;
        }else if (submesh.hasMorphNormal || submesh.hasMorphTangent){
            count = 4;
        }
        return std::min<uint32_t>(count, MAX_MORPHTARGETS);
    }

    // the skin and morph sources of a submesh, resolved once
    struct SubmeshPoseSources {
        AttributeSource boneIds;
        AttributeSource boneWeights;
        AttributeSource morph[8];
        uint32_t morphCount = 0;
        bool skinned = false;
    };

    SubmeshPoseSources resolvePoseSources(const MeshComponent& mesh, uint32_t submeshIndex){
        SubmeshPoseSources sources;
        const Submesh& submesh = mesh.submeshes[submeshIndex];
        if (submesh.hasSkinning){
            sources.boneIds = findAttribute(mesh, submesh, AttributeType::BONEIDS);
            sources.boneWeights = findAttribute(mesh, submesh, AttributeType::BONEWEIGHTS);
            sources.skinned = sources.boneIds.valid && sources.boneWeights.valid;
        }
        const uint32_t morphs = activeMorphTargets(submesh);
        for (uint32_t k = 0; k < morphs; k++){
            sources.morph[k] = findAttribute(mesh, submesh, (AttributeType)((int)AttributeType::MORPHTARGET0 + (int)k));
            if (sources.morph[k].valid){
                sources.morphCount = k + 1;
            }
        }
        return sources;
    }

    void fillInput(const SubmeshPoseSources& sources, uint32_t v, VertexPoseInput& input){
        input.skinned = sources.skinned;
        if (sources.skinned){
            readElements<4>(sources.boneIds, v, input.boneIds);
            readElements<4>(sources.boneWeights, v, input.boneWeights);
        }
        input.morphCount = sources.morphCount;
        for (uint32_t k = 0; k < sources.morphCount; k++){
            input.morphTargets[k] = sources.morph[k].valid ? readVector3(sources.morph[k], v) : Vector3::ZERO;
        }
    }

}

Vector3 picking::poseVertex(const VertexPoseInput& input, const PoseParameters& parameters, Matrix4* boneTransform){
    // getMorphPosition: pos += morphWeights[k] * a_morphTargetK
    Vector3 position = input.position;
    if (parameters.morphWeights){
        for (uint32_t k = 0; k < input.morphCount; k++){
            const float w = parameters.morphWeights[k];
            position.x += w * input.morphTargets[k].x;
            position.y += w * input.morphTargets[k].y;
            position.z += w * input.morphTargets[k].z;
        }
    }
    if (!input.skinned){
        if (boneTransform){
            *boneTransform = Matrix4();
        }
        return position;
    }

    // getBoneTransform: sum of bonesMatrix[int(id * normAdjust.x)] * (weight * normAdjust.y)
    float m[4][4] = {};
    for (int i = 0; i < 4; i++){
        const float weight = input.boneWeights[i] * parameters.normAdjustWeight;
        if (weight == 0.0f){
            continue; // adds exactly zero
        }
        const int bone = (int)(input.boneIds[i] * parameters.normAdjustJoint);
        if (bone < 0 || (size_t)bone >= parameters.bones.size()){
            continue;
        }
        // column-major, as Matrix4::get(column, row) reads it
        const float* b = static_cast<const float*>(parameters.bones[(size_t)bone]);
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                m[c][r] += b[c * 4 + r] * weight;
            }
        }
    }

    // getSkinPosition: (boneTransform * vec4(pos, 1)).xyz / w
    float out[4];
    for (int r = 0; r < 4; r++){
        out[r] = m[0][r] * position.x + m[1][r] * position.y + m[2][r] * position.z + m[3][r];
    }
    if (boneTransform){
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                boneTransform->set(c, r, m[c][r]);
            }
        }
    }
    return Vector3(out[0] / out[3], out[1] / out[3], out[2] / out[3]);
}

bool picking::isPosed(const MeshComponent& mesh){
    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
        const Submesh& submesh = mesh.submeshes[s];
        if (submesh.hasSkinning){
            return true;
        }
        if (submesh.hasMorphTarget){
            const uint32_t morphs = activeMorphTargets(submesh);
            for (uint32_t k = 0; k < morphs; k++){
                if (mesh.morphWeights[k] != 0.0f){
                    return true;
                }
            }
        }
    }
    return false;
}

uint64_t picking::poseHash(const MeshComponent& mesh){
    uint64_t h = hashBytes(mesh.morphWeights, sizeof(mesh.morphWeights), 0x5ca1ab1eull);
    if (mesh.bonesMatrix.size() > 0){
        h = hashBytes(mesh.bonesMatrix.data(), mesh.bonesMatrix.size() * sizeof(Matrix4), h);
    }
    const float adjust[2] = {mesh.normAdjustJoint, mesh.normAdjustWeight};
    return hashBytes(adjust, sizeof(adjust), h);
}

PoseParameters picking::poseParameters(const MeshComponent& mesh, uint32_t submesh){
    PoseParameters parameters;
    parameters.bones = std::span<const Matrix4>(mesh.bonesMatrix.data(), mesh.bonesMatrix.size());
    parameters.morphWeights = mesh.morphWeights;
    const Submesh& s = mesh.submeshes[submesh];
    // applySkinningUniform (RenderSystem.cpp)
    parameters.normAdjustJoint = s.hasSkinningNormalization ? s.normAdjustJoint : mesh.normAdjustJoint;
    parameters.normAdjustWeight = s.hasSkinningNormalization ? s.normAdjustWeight : mesh.normAdjustWeight;
    return parameters;
}

bool picking::readPoseInput(const MeshComponent& mesh, const PickGeometryLayout& layout, uint32_t submesh, uint32_t vertex,
                            VertexPoseInput& input){
    if (submesh >= layout.submeshes.size() || submesh >= mesh.numSubmeshes){
        return false;
    }
    const PickSubmeshSource& source = layout.submeshes[submesh];
    if (!source.valid || vertex >= source.vertexCount){
        return false;
    }
    float p[3];
    std::memcpy(p, source.positionBuffer->getData() + source.positionOffset + (size_t)vertex * source.positionStride, sizeof(p));
    input = VertexPoseInput();
    input.position = Vector3(p[0], p[1], p[2]);
    fillInput(resolvePoseSources(mesh, submesh), vertex, input);
    return true;
}

bool picking::readVertexNormal(const MeshComponent& mesh, uint32_t submesh, uint32_t vertex, Vector3& normal){
    if (submesh >= mesh.numSubmeshes){
        return false;
    }
    const AttributeSource source = findAttribute(mesh, mesh.submeshes[submesh], AttributeType::NORMAL);
    if (!source.valid || source.attribute.getElements() < 3){
        return false;
    }
    normal = readVector3(source, vertex);
    return std::isfinite(normal.x) && std::isfinite(normal.y) && std::isfinite(normal.z) && normal.squaredLength() > 0.0f;
}

bool picking::poseGeometry(const MeshComponent& mesh, const PickGeometryLayout& layout,
                           std::span<const Vector3> positions, std::vector<Vector3>& posed){
    posed.assign(positions.begin(), positions.end());
    bool any = false;
    // each block of positions once, with the first submesh that reads it
    std::vector<uint32_t> donePositions;
    for (uint32_t s = 0; s < (uint32_t)layout.submeshes.size() && s < mesh.numSubmeshes; s++){
        const PickSubmeshSource& source = layout.submeshes[s];
        if (!source.valid || std::find(donePositions.begin(), donePositions.end(), source.base) != donePositions.end()){
            continue;
        }
        donePositions.push_back(source.base);
        const SubmeshPoseSources sources = resolvePoseSources(mesh, s);
        if (!sources.skinned && sources.morphCount == 0){
            continue;
        }
        const PoseParameters parameters = poseParameters(mesh, s);
        const uint32_t count = (uint32_t)std::min<size_t>(source.vertexCount, positions.size() > source.base ? positions.size() - source.base : 0);
        auto poseRange = [&](uint32_t begin, uint32_t end){
            VertexPoseInput input;
            for (uint32_t v = begin; v < end; v++){
                input.position = positions[(size_t)source.base + v];
                fillInput(sources, v, input);
                posed[(size_t)source.base + v] = poseVertex(input, parameters);
            }
        };
        // each vertex on its own: chunks of a large mesh run on the workers (R30)
        constexpr uint32_t CHUNK = 4096;
        if (count > CHUNK * 2){
            const size_t chunks = (count + CHUNK - 1) / CHUNK;
            jobs::parallelFor(chunks, [&](size_t chunk){
                const uint32_t begin = (uint32_t)chunk * CHUNK;
                poseRange(begin, std::min(count, begin + CHUNK));
            });
        }else{
            poseRange(0, count);
        }
        any = true;
    }
    return any;
}
