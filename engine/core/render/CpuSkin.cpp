// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "render/CpuSkin.h"
#include "model/ModelCpuData.h"

#include "Scene.h"
#include "buffer/Buffer.h"
#include "component/BoneComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "lattice/LatticeSolver.h"
#include "lattice/LatticeTypes.h"
#include "subsystem/RenderSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

using namespace jasperengine;
using namespace jasperengine::cpuskin;

namespace{

    const char* attributeName(AttributeType type){
        switch (type){
            case AttributeType::INDEX: return "INDEX";
            case AttributeType::POSITION: return "POSITION";
            case AttributeType::TEXCOORD1: return "TEXCOORD1";
            case AttributeType::TEXCOORD2: return "TEXCOORD2";
            case AttributeType::NORMAL: return "NORMAL";
            case AttributeType::TANGENT: return "TANGENT";
            case AttributeType::COLOR: return "COLOR";
            case AttributeType::BONEWEIGHTS: return "BONEWEIGHTS";
            case AttributeType::BONEIDS: return "BONEIDS";
            case AttributeType::MORPHTARGET0: return "MORPHTARGET0";
            case AttributeType::MORPHTARGET1: return "MORPHTARGET1";
            case AttributeType::MORPHTARGET2: return "MORPHTARGET2";
            case AttributeType::MORPHTARGET3: return "MORPHTARGET3";
            case AttributeType::MORPHTARGET4: return "MORPHTARGET4";
            case AttributeType::MORPHTARGET5: return "MORPHTARGET5";
            case AttributeType::MORPHTARGET6: return "MORPHTARGET6";
            case AttributeType::MORPHTARGET7: return "MORPHTARGET7";
            case AttributeType::MORPHNORMAL0: return "MORPHNORMAL0";
            case AttributeType::MORPHNORMAL1: return "MORPHNORMAL1";
            case AttributeType::MORPHNORMAL2: return "MORPHNORMAL2";
            case AttributeType::MORPHNORMAL3: return "MORPHNORMAL3";
            case AttributeType::MORPHTANGENT0: return "MORPHTANGENT0";
            case AttributeType::MORPHTANGENT1: return "MORPHTANGENT1";
            default: return "attribute";
        }
    }

    const char* dataTypeName(AttributeDataType type){
        switch (type){
            case AttributeDataType::BYTE: return "BYTE";
            case AttributeDataType::UNSIGNED_BYTE: return "UNSIGNED_BYTE";
            case AttributeDataType::SHORT: return "SHORT";
            case AttributeDataType::UNSIGNED_SHORT: return "UNSIGNED_SHORT";
            case AttributeDataType::INT: return "INT";
            case AttributeDataType::UNSIGNED_INT: return "UNSIGNED_INT";
            default: return "FLOAT";
        }
    }

    size_t componentSize(AttributeDataType type){
        switch (type){
            case AttributeDataType::BYTE:
            case AttributeDataType::UNSIGNED_BYTE: return 1;
            case AttributeDataType::SHORT:
            case AttributeDataType::UNSIGNED_SHORT: return 2;
            default: return 4;
        }
    }

    // the formats SokolObject::getVertexFormat binds (anything else is SG_VERTEXFORMAT_INVALID)
    bool bindableFormat(unsigned int elements, AttributeDataType type){
        switch (type){
            case AttributeDataType::BYTE:
            case AttributeDataType::UNSIGNED_BYTE: return elements == 4;
            case AttributeDataType::SHORT:
            case AttributeDataType::UNSIGNED_SHORT: return elements == 2 || elements == 4;
            case AttributeDataType::FLOAT: return elements >= 1 && elements <= 4;
            default: return false;
        }
    }

    const std::vector<AttributeType>& readTypes(){
        static const std::vector<AttributeType> types = {
            AttributeType::POSITION, AttributeType::NORMAL, AttributeType::TANGENT,
            AttributeType::TEXCOORD1, AttributeType::TEXCOORD2, AttributeType::COLOR,
            AttributeType::BONEIDS, AttributeType::BONEWEIGHTS,
            AttributeType::MORPHTARGET0, AttributeType::MORPHTARGET1, AttributeType::MORPHTARGET2, AttributeType::MORPHTARGET3,
            AttributeType::MORPHTARGET4, AttributeType::MORPHTARGET5, AttributeType::MORPHTARGET6, AttributeType::MORPHTARGET7,
            AttributeType::MORPHNORMAL0, AttributeType::MORPHNORMAL1, AttributeType::MORPHNORMAL2, AttributeType::MORPHNORMAL3,
            AttributeType::MORPHTANGENT0, AttributeType::MORPHTANGENT1
        };
        return types;
    }

    // one vertex attribute as the draw binds it
    struct Stream{
        const Buffer* buffer = nullptr;
        std::string bufferName;
        size_t offset = 0;
        unsigned int stride = 0;
        unsigned int elements = 0;
        AttributeDataType type = AttributeDataType::FLOAT;
        bool normalized = false;

        bool operator<(const Stream& o) const{
            if (buffer != o.buffer) return buffer < o.buffer;
            if (offset != o.offset) return offset < o.offset;
            if (stride != o.stride) return stride < o.stride;
            if (elements != o.elements) return elements < o.elements;
            if (type != o.type) return type < o.type;
            return normalized < o.normalized;
        }
        bool operator==(const Stream& o) const{
            return !(*this < o) && !(o < *this);
        }
    };

    // the vertex fetch of element `index` (GL ES 3 / GL 4.2 conversion rules); missing
    // components keep (0, 0, 0, 1)
    void fetch(const Stream& s, uint32_t index, float out[4]){
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        const unsigned char* p = s.buffer->getData() + s.offset + (size_t)index * s.stride;
        const size_t size = componentSize(s.type);
        for (unsigned int c = 0; c < s.elements && c < 4; c++){
            const unsigned char* v = p + c * size;
            float value;
            switch (s.type){
                case AttributeDataType::BYTE: {
                    int8_t x;
                    std::memcpy(&x, v, 1);
                    value = s.normalized ? std::max((float)x / 127.0f, -1.0f) : (float)x;
                    break;
                }
                case AttributeDataType::UNSIGNED_BYTE: {
                    const uint8_t x = *v;
                    value = s.normalized ? (float)x / 255.0f : (float)x;
                    break;
                }
                case AttributeDataType::SHORT: {
                    int16_t x;
                    std::memcpy(&x, v, 2);
                    value = s.normalized ? std::max((float)x / 32767.0f, -1.0f) : (float)x;
                    break;
                }
                case AttributeDataType::UNSIGNED_SHORT: {
                    // USHORT2N / USHORT4N whatever the flag says (getVertexFormat)
                    uint16_t x;
                    std::memcpy(&x, v, 2);
                    value = (float)x / 65535.0f;
                    break;
                }
                default: {
                    std::memcpy(&value, v, 4);
                    break;
                }
            }
            out[c] = value;
        }
    }

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    std::string submeshLabel(unsigned int submesh){
        return "submesh " + std::to_string(submesh);
    }

    // The submesh's bindings as RenderSystem::loadMesh makes them
    struct Bindings{
        std::map<AttributeType, Stream> streams;
        bool indexed = false;
        Stream index;
        uint32_t indexCount = 0;
    };

    bool resolveBindings(const MeshComponent& mesh, unsigned int s, const std::map<std::string, const Buffer*>& buffers,
                         Bindings& out, std::string* error){
        const Submesh& submesh = mesh.submeshes[s];
        auto stream = [](const Buffer* buffer, const std::string& name, const Attribute& attribute){
            Stream st;
            st.buffer = buffer;
            st.bufferName = name;
            st.offset = attribute.getOffset();
            st.stride = buffer->getStride();
            st.elements = attribute.getElements();
            st.type = attribute.getDataType();
            st.normalized = attribute.getNormalized();
            return st;
        };

        // the buffers' own attributes (buffer name order), then the submesh's
        for (const auto& entry : buffers){
            const Buffer* buffer = entry.second;
            if (!buffer->isRenderAttributes())
                continue;
            if (buffer->getType() == BufferType::INDEX_BUFFER){
                const std::map<AttributeType, Attribute> attributes = buffer->getAttributes();
                auto found = attributes.find(AttributeType::INDEX);
                // loadMesh reads the INDEX attribute (a default one when it has none)
                const Attribute attribute = (found != attributes.end()) ? found->second : Attribute();
                out.indexed = true;
                out.index = stream(buffer, entry.first, attribute);
                out.indexCount = buffer->getCount();
                continue;
            }
            for (const auto& attribute : buffer->getAttributes()){
                if (attribute.second.getPerInstance())
                    continue;
                out.streams[attribute.first] = stream(buffer, entry.first, attribute.second);
            }
        }
        for (const auto& attribute : submesh.attributes){
            auto buffer = buffers.find(attribute.second.getBufferName());
            if (buffer == buffers.end()){
                return fail(error, submeshLabel(s) + ": " + attributeName(attribute.first) + " reads the buffer '" +
                            attribute.second.getBufferName() + "', which the mesh does not have");
            }
            if (attribute.first == AttributeType::INDEX){
                out.indexed = true;
                out.index = stream(buffer->second, buffer->first, attribute.second);
                out.indexCount = attribute.second.getCount();
            }else if (!attribute.second.getPerInstance()){
                out.streams[attribute.first] = stream(buffer->second, buffer->first, attribute.second);
            }
        }

        if (!out.streams.count(AttributeType::POSITION))
            return fail(error, submeshLabel(s) + ": no POSITION attribute");

        for (AttributeType type : readTypes()){
            auto found = out.streams.find(type);
            if (found == out.streams.end())
                continue;
            const Stream& st = found->second;
            if (!bindableFormat(st.elements, st.type)){
                return fail(error, submeshLabel(s) + ": " + attributeName(type) + " is " + std::to_string(st.elements) + " x " +
                            dataTypeName(st.type) + (st.normalized ? " (normalized)" : "") +
                            ", a vertex format the renderer cannot bind (it binds 4 bytes, 2 or 4 shorts, 2 or 4 unsigned shorts, 1 to 4 floats)");
            }
            if (st.stride == 0){
                return fail(error, submeshLabel(s) + ": " + attributeName(type) + " reads the buffer '" + st.bufferName +
                            "', which has no stride");
            }
            if (!st.buffer->getData() && st.buffer->getSize() > 0){
                return fail(error, submeshLabel(s) + ": the buffer '" + st.bufferName + "' has no data");
            }
        }
        if (out.indexed){
            if (out.index.type != AttributeDataType::UNSIGNED_SHORT && out.index.type != AttributeDataType::UNSIGNED_INT){
                return fail(error, submeshLabel(s) + ": index type " + dataTypeName(out.index.type) +
                            " (the renderer draws 16 and 32 bit indices)");
            }
        }
        return true;
    }

    // whether element `index` of the stream lies inside its buffer
    bool readable(const Stream& s, uint32_t index){
        const size_t end = s.offset + (size_t)index * s.stride + componentSize(s.type) * s.elements;
        return s.buffer->getData() && end <= s.buffer->getSize();
    }

    // the layout identity of a submesh: equal for submeshes whose vertex i decodes and deforms
    // the same (same bindings, normAdjust and morph targets read)
    struct LayoutKey{
        std::vector<std::pair<AttributeType, Stream>> streams;
        float normAdjust[2] = {1.0f, 1.0f};
        int morph[3] = {0, 0, 0};
        int colorComponents = 0;
        bool skinning = false;

        bool operator<(const LayoutKey& o) const{
            if (streams.size() != o.streams.size()) return streams.size() < o.streams.size();
            for (size_t i = 0; i < streams.size(); i++){
                if (streams[i].first != o.streams[i].first) return streams[i].first < o.streams[i].first;
                if (!(streams[i].second == o.streams[i].second)) return streams[i].second < o.streams[i].second;
            }
            if (normAdjust[0] != o.normAdjust[0]) return normAdjust[0] < o.normAdjust[0];
            if (normAdjust[1] != o.normAdjust[1]) return normAdjust[1] < o.normAdjust[1];
            for (int i = 0; i < 3; i++){
                if (morph[i] != o.morph[i]) return morph[i] < o.morph[i];
            }
            if (colorComponents != o.colorComponents) return colorComponents < o.colorComponents;
            return skinning < o.skinning;
        }
    };

    inline Vector3 normalized(const Vector3& v){
        const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return Vector3(v.x / length, v.y / length, v.z / length);
    }

    // column-major 4x4 as the shader holds it: m[col][row]
    struct Mat{
        float m[4][4];
    };

    // getBoneTransform: the weighted sum, one bone after the other
    void boneTransform(const std::vector<Matrix4>& bones, const std::array<int, 4>& ids, const Vector4& weights, Mat& out){
        std::memset(out.m, 0, sizeof(out.m));
        const float w[4] = {weights.x, weights.y, weights.z, weights.w};
        for (int k = 0; k < 4; k++){
            const int id = ids[k];
            // past the end: what robust access returns (zeros)
            if (id < 0 || static_cast<size_t>(id) >= bones.size())
                continue;
            const Matrix4& bone = bones[static_cast<size_t>(id)];
            for (int c = 0; c < 4; c++){
                for (int r = 0; r < 4; r++){
                    out.m[c][r] += bone.get(c, r) * w[k];
                }
            }
        }
    }

    // getSkinPosition: (B * vec4(p, 1)).xyz / w
    Vector3 skinPosition(const Mat& b, const Vector3& p){
        float v[4];
        for (int r = 0; r < 4; r++){
            v[r] = b.m[0][r] * p.x + b.m[1][r] * p.y + b.m[2][r] * p.z + b.m[3][r];
        }
        return Vector3(v[0] / v[3], v[1] / v[3], v[2] / v[3]);
    }

    // getSkinNormal / getSkinTangent: mat3(B) * d
    Vector3 skinDirection(const Mat& b, const Vector3& d){
        return Vector3(b.m[0][0] * d.x + b.m[1][0] * d.y + b.m[2][0] * d.z,
                       b.m[0][1] * d.x + b.m[1][1] * d.y + b.m[2][1] * d.z,
                       b.m[0][2] * d.x + b.m[1][2] * d.y + b.m[2][2] * d.z);
    }

    LatticeMaskChannel maskOf(int channel){
        if (channel < 0 || channel > 7)
            return LatticeMaskChannel::NONE;
        return static_cast<LatticeMaskChannel>(channel + 1);
    }

    int nearestInt(float value){
        return static_cast<int>(std::floor(value + 0.5f));
    }

}

const CpuSkinSubmesh& CpuSkinSource::submeshOf(uint32_t vertex) const{
    for (const CpuSkinSubmesh& submesh : submeshes){
        if (vertex >= submesh.firstVertex && vertex < submesh.firstVertex + submesh.vertexCount)
            return submesh;
    }
    static const CpuSkinSubmesh none;
    return none;
}

bool cpuskin::buildSource(const MeshComponent& mesh, CpuSkinSource& out, std::string* error){
    ModelCpuData::ensure(mesh);
    out = CpuSkinSource();
    if (mesh.numSubmeshes == 0)
        return fail(error, "the mesh has no submeshes");

    // the buffers RenderSystem::loadMesh binds, by name
    std::map<std::string, const Buffer*> buffers;
    if (mesh.buffer.getSize() > 0)
        buffers["vertices"] = &mesh.buffer;
    if (mesh.indices.getSize() > 0)
        buffers["indices"] = &mesh.indices;
    for (unsigned int i = 0; i < mesh.numExternalBuffers; i++)
        buffers[mesh.eBuffers[i].getName()] = &mesh.eBuffers[i];
    // a submesh without indices draws the mesh's vertex count
    const unsigned int meshVertexCount = (mesh.vertexCount != 0) ? mesh.vertexCount : mesh.buffer.getCount();

    std::map<LayoutKey, int> signatures;
    std::map<std::pair<int, uint32_t>, uint32_t> firstOf;

    for (unsigned int s = 0; s < mesh.numSubmeshes; s++){
        const Submesh& submesh = mesh.submeshes[s];
        Bindings bindings;
        if (!resolveBindings(mesh, s, buffers, bindings, error))
            return false;

        CpuSkinSubmesh part;
        part.submesh = s;
        part.primitive = submesh.primitiveType;
        auto has = [&bindings](AttributeType type){ return bindings.streams.count(type) > 0; };
        part.hasNormal = has(AttributeType::NORMAL);
        part.hasTangent = has(AttributeType::TANGENT);
        part.hasUv0 = has(AttributeType::TEXCOORD1);
        part.hasUv1 = has(AttributeType::TEXCOORD2);
        if (has(AttributeType::COLOR))
            part.colorComponents = (bindings.streams[AttributeType::COLOR].elements == 3) ? 3 : 4;
        part.hasSkinning = has(AttributeType::BONEIDS) || has(AttributeType::BONEWEIGHTS);
        if (part.hasSkinning){
            part.normAdjust = Vector2(submesh.hasSkinningNormalization ? submesh.normAdjustJoint : mesh.normAdjustJoint,
                                      submesh.hasSkinningNormalization ? submesh.normAdjustWeight : mesh.normAdjustWeight);
        }
        // morphtarget.glsl: HAS_MORPHTARGET by target 0's position, the attribute counts by
        // whether normals and tangents are morphed
        if (has(AttributeType::MORPHTARGET0)){
            const bool normals = has(AttributeType::MORPHNORMAL0);
            const bool tangents = has(AttributeType::MORPHTANGENT0);
            part.morphTargets = (normals && tangents) ? 2 : ((normals || tangents) ? 4 : MAX_MORPHTARGETS);
            part.morphNormalTargets = normals ? (tangents ? 2 : 4) : 0;
            part.morphTangentTargets = tangents ? 2 : 0;
            for (int t = 0; t < part.morphTargets; t++){
                if (has(static_cast<AttributeType>(static_cast<int>(AttributeType::MORPHTARGET0) + t)))
                    part.morphPositionMask |= (1u << t);
            }
            for (int t = 0; t < part.morphNormalTargets; t++){
                if (has(static_cast<AttributeType>(static_cast<int>(AttributeType::MORPHNORMAL0) + t)))
                    part.morphNormalMask |= (1u << t);
            }
            for (int t = 0; t < part.morphTangentTargets; t++){
                if (has(static_cast<AttributeType>(static_cast<int>(AttributeType::MORPHTANGENT0) + t)))
                    part.morphTangentMask |= (1u << t);
            }
        }

        // the draw's indices
        std::vector<uint32_t> drawn;
        if (bindings.indexed){
            const Stream& index = bindings.index;
            const size_t size = componentSize(index.type);
            const size_t end = index.offset + (size_t)bindings.indexCount * size;
            if (bindings.indexCount > 0 && (!index.buffer->getData() || end > index.buffer->getSize())){
                return fail(error, submeshLabel(s) + ": " + std::to_string(bindings.indexCount) + " indices from byte " +
                            std::to_string(index.offset) + " run past the index buffer '" + index.bufferName + "' (" +
                            std::to_string(index.buffer->getSize()) + " bytes)");
            }
            drawn.resize(bindings.indexCount);
            const unsigned char* data = index.buffer->getData() + index.offset;
            for (uint32_t k = 0; k < bindings.indexCount; k++){
                if (size == 2){
                    uint16_t v;
                    std::memcpy(&v, data + k * 2, 2);
                    drawn[k] = v;
                }else{
                    uint32_t v;
                    std::memcpy(&v, data + k * 4, 4);
                    drawn[k] = v;
                }
            }
        }else{
            drawn.resize(meshVertexCount);
            for (uint32_t k = 0; k < meshVertexCount; k++)
                drawn[k] = k;
        }

        // the vertices it reads, ascending
        std::vector<uint32_t> used = drawn;
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        for (const auto& entry : bindings.streams){
            const Stream& st = entry.second;
            if (std::find(readTypes().begin(), readTypes().end(), entry.first) == readTypes().end())
                continue;
            if (!used.empty() && !readable(st, used.back())){
                return fail(error, submeshLabel(s) + ": vertex " + std::to_string(used.back()) + " of " +
                            attributeName(entry.first) + " lies past the buffer '" + st.bufferName + "' (" +
                            std::to_string(st.buffer->getSize()) + " bytes, offset " + std::to_string(st.offset) +
                            ", stride " + std::to_string(st.stride) + ")");
            }
        }

        LayoutKey signature;
        for (AttributeType type : readTypes()){
            auto found = bindings.streams.find(type);
            if (found != bindings.streams.end())
                signature.streams.emplace_back(type, found->second);
        }
        signature.normAdjust[0] = part.normAdjust.x;
        signature.normAdjust[1] = part.normAdjust.y;
        signature.morph[0] = part.morphTargets;
        signature.morph[1] = part.morphNormalTargets;
        signature.morph[2] = part.morphTangentTargets;
        signature.colorComponents = part.colorComponents;
        signature.skinning = part.hasSkinning;
        const int signatureId = signatures.emplace(signature, (int)signatures.size()).first->second;

        part.firstVertex = static_cast<uint32_t>(out.positions.size());
        part.vertexCount = static_cast<uint32_t>(used.size());
        const size_t total = out.positions.size() + used.size();
        out.positions.resize(total, Vector3(0.0f, 0.0f, 0.0f));
        out.normals.resize(total, Vector3(0.0f, 0.0f, 0.0f));
        out.tangents.resize(total, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        out.uv0.resize(total, Vector2(0.0f, 0.0f));
        out.uv1.resize(total, Vector2(0.0f, 0.0f));
        out.colors.resize(total, Vector4(1.0f, 1.0f, 1.0f, 1.0f));
        out.boneIds.resize(total, std::array<int, 4>{{0, 0, 0, 0}});
        out.boneWeights.resize(total, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
        out.vertexSubmesh.resize(total, s);
        out.vertexIndex.resize(total, 0);
        out.canonical.resize(total, 0);
        auto grow = [total](std::vector<Vector3>& v, bool needed){
            if (needed || !v.empty())
                v.resize(total, Vector3(0.0f, 0.0f, 0.0f));
        };
        for (int t = 0; t < MAX_MORPHTARGETS; t++)
            grow(out.morphPositions[t], (part.morphPositionMask >> t) & 1u);
        for (int t = 0; t < 4; t++)
            grow(out.morphNormals[t], (part.morphNormalMask >> t) & 1u);
        for (int t = 0; t < 2; t++)
            grow(out.morphTangents[t], (part.morphTangentMask >> t) & 1u);

        // the bound streams of the attributes read (null: not bound)
        auto streamOf = [&bindings](AttributeType type) -> const Stream*{
            auto found = bindings.streams.find(type);
            return (found != bindings.streams.end()) ? &found->second : nullptr;
        };
        auto offsetType = [](AttributeType first, int t){
            return static_cast<AttributeType>(static_cast<int>(first) + t);
        };
        const Stream* positionStream = streamOf(AttributeType::POSITION);
        const Stream* normalStream = streamOf(AttributeType::NORMAL);
        const Stream* tangentStream = streamOf(AttributeType::TANGENT);
        const Stream* uv0Stream = streamOf(AttributeType::TEXCOORD1);
        const Stream* uv1Stream = streamOf(AttributeType::TEXCOORD2);
        const Stream* colorStream = streamOf(AttributeType::COLOR);
        const Stream* idStream = part.hasSkinning ? streamOf(AttributeType::BONEIDS) : nullptr;
        const Stream* weightStream = part.hasSkinning ? streamOf(AttributeType::BONEWEIGHTS) : nullptr;
        const Stream* morphPositionStreams[MAX_MORPHTARGETS] = {};
        const Stream* morphNormalStreams[4] = {};
        const Stream* morphTangentStreams[2] = {};
        for (int t = 0; t < MAX_MORPHTARGETS; t++){
            if ((part.morphPositionMask >> t) & 1u)
                morphPositionStreams[t] = streamOf(offsetType(AttributeType::MORPHTARGET0, t));
        }
        for (int t = 0; t < 4; t++){
            if ((part.morphNormalMask >> t) & 1u)
                morphNormalStreams[t] = streamOf(offsetType(AttributeType::MORPHNORMAL0, t));
        }
        for (int t = 0; t < 2; t++){
            if ((part.morphTangentMask >> t) & 1u)
                morphTangentStreams[t] = streamOf(offsetType(AttributeType::MORPHTANGENT0, t));
        }

        // where each buffer index went
        std::map<uint32_t, uint32_t> vertexOf;
        float value[4];
        for (size_t k = 0; k < used.size(); k++){
            const uint32_t v = part.firstVertex + static_cast<uint32_t>(k);
            const uint32_t index = used[k];
            vertexOf[index] = v;
            out.vertexIndex[v] = index;
            out.canonical[v] = firstOf.emplace(std::make_pair(signatureId, index), v).first->second;

            fetch(*positionStream, index, value);
            out.positions[v] = Vector3(value[0], value[1], value[2]);
            if (normalStream){
                fetch(*normalStream, index, value);
                out.normals[v] = Vector3(value[0], value[1], value[2]);
            }
            if (tangentStream){
                fetch(*tangentStream, index, value);
                out.tangents[v] = Vector4(value[0], value[1], value[2], value[3]);
            }
            if (uv0Stream){
                fetch(*uv0Stream, index, value);
                out.uv0[v] = Vector2(value[0], value[1]);
            }
            if (uv1Stream){
                fetch(*uv1Stream, index, value);
                out.uv1[v] = Vector2(value[0], value[1]);
            }
            if (colorStream){
                fetch(*colorStream, index, value);
                out.colors[v] = Vector4(value[0], value[1], value[2], part.colorComponents == 3 ? 1.0f : value[3]);
            }
            if (part.hasSkinning){
                // an unbound one of the two reads the default (0, 0, 0, 1)
                float ids[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                float weights[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                if (idStream)
                    fetch(*idStream, index, ids);
                if (weightStream)
                    fetch(*weightStream, index, weights);
                // int(a_boneIds[k] * normAdjust.x), a_boneWeights[k] * normAdjust.y
                for (int c = 0; c < 4; c++)
                    out.boneIds[v][c] = static_cast<int>(ids[c] * part.normAdjust.x);
                out.boneWeights[v] = Vector4(weights[0] * part.normAdjust.y, weights[1] * part.normAdjust.y,
                                             weights[2] * part.normAdjust.y, weights[3] * part.normAdjust.y);
            }
            for (int t = 0; t < MAX_MORPHTARGETS; t++){
                if (morphPositionStreams[t]){
                    fetch(*morphPositionStreams[t], index, value);
                    out.morphPositions[t][v] = Vector3(value[0], value[1], value[2]);
                }
            }
            for (int t = 0; t < 4; t++){
                if (morphNormalStreams[t]){
                    fetch(*morphNormalStreams[t], index, value);
                    out.morphNormals[t][v] = Vector3(value[0], value[1], value[2]);
                }
            }
            for (int t = 0; t < 2; t++){
                if (morphTangentStreams[t]){
                    fetch(*morphTangentStreams[t], index, value);
                    out.morphTangents[t][v] = Vector3(value[0], value[1], value[2]);
                }
            }
        }

        part.indices.resize(drawn.size());
        for (size_t k = 0; k < drawn.size(); k++)
            part.indices[k] = vertexOf[drawn[k]];

        out.submeshes.push_back(std::move(part));
    }
    return true;
}

bool cpuskin::buildSource(Scene* scene, Entity meshEntity, CpuSkinSource& out, std::string* error){
    out = CpuSkinSource();
    if (!scene || !scene->isEntityCreated(meshEntity))
        return fail(error, "no such entity");
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity);
    if (!mesh)
        return fail(error, "'" + scene->getEntityName(meshEntity) + "' has no mesh");
    if (mesh->numSubmeshes == 0){
        const ModelComponent* model = scene->findComponent<ModelComponent>(meshEntity);
        if (model && !model->meshNodesMapping.empty()){
            return fail(error, "'" + scene->getEntityName(meshEntity) + "' is a model whose geometry is on its mesh node entities "
                        "(ModelComponent::meshNodesMapping): deform those");
        }
    }
    return buildSource(*mesh, out, error);
}

void cpuskin::updateTransforms(Scene* scene){
    if (!scene)
        return;
    std::shared_ptr<RenderSystem> render = scene->getSystem<RenderSystem>();
    auto transforms = scene->getComponentArray<Transform>();
    if (!render || !transforms)
        return;
    // the Transform array keeps parents before their children
    for (size_t i = 0; i < transforms->size(); i++)
        render->updateTransform(transforms->getComponentFromIndex(i), transforms->getEntity(i));
}

bool cpuskin::computeSkinMatrices(Scene* scene, Entity meshEntity, std::vector<Matrix4>& out, std::string* error){
    out.clear();
    if (!scene || !scene->isEntityCreated(meshEntity))
        return fail(error, "no such entity");
    const MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity);
    const Transform* meshTransform = scene->findComponent<Transform>(meshEntity);
    if (!mesh || !meshTransform)
        return fail(error, "'" + scene->getEntityName(meshEntity) + "' has no mesh and transform");

    out.resize(mesh->bonesMatrix.size());
    for (size_t b = 0; b < out.size(); b++)
        out[b] = mesh->bonesMatrix[b];

    // RenderSystem::update: the skin bindings of every model, in array order
    bool bound = false;
    auto models = scene->getComponentArray<ModelComponent>();
    for (size_t i = 0; i < models->size(); i++){
        const ModelComponent& model = models->getComponentFromIndex(i);
        for (const ModelSkinBinding& binding : model.skinBindings){
            if (binding.mesh != meshEntity)
                continue;
            const Matrix4 inverseMesh = meshTransform->modelMatrix.inverse();
            if (!inverseMesh.isValid())
                return fail(error, "the model matrix of '" + scene->getEntityName(meshEntity) + "' is not invertible (a scale of 0)");
            bound = true;
            const size_t jointCount = std::min(binding.joints.size(), binding.inverseBindMatrices.size());
            for (size_t j = 0; j < jointCount; j++){
                const Transform* joint = scene->findComponent<Transform>(binding.joints[j]);
                if (!joint)
                    continue;
                if (j >= out.size())
                    out.resize(j + 1, Matrix4());
                out[j] = inverseMesh * joint->modelMatrix * binding.inverseBindMatrices[j];
            }
        }
    }
    if (bound)
        return true;

    // the legacy path (a model without skin bindings): each bone drives the model entity's
    // mesh and its mesh nodes' at its index
    auto bones = scene->getComponentArray<BoneComponent>();
    for (size_t i = 0; i < bones->size(); i++){
        const BoneComponent& bone = bones->getComponentFromIndex(i);
        if (bone.model == NULL_ENTITY || bone.index < 0)
            continue;
        const ModelComponent* model = scene->findComponent<ModelComponent>(bone.model);
        if (!model || !model->skinBindings.empty())
            continue;
        bool drives = (bone.model == meshEntity);
        for (const auto& node : model->meshNodesMapping)
            drives = drives || node.second == meshEntity;
        if (!drives)
            continue;
        const Transform* modelTransform = scene->findComponent<Transform>(bone.model);
        const Transform* boneTransform = scene->findComponent<Transform>(bones->getEntity(i));
        if (!modelTransform || !boneTransform)
            continue;
        const Matrix4 inverseModel = modelTransform->modelMatrix.inverse();
        if (!inverseModel.isValid())
            return fail(error, "the model matrix of '" + scene->getEntityName(bone.model) + "' is not invertible (a scale of 0)");
        const size_t index = static_cast<size_t>(bone.index);
        if (index >= out.size())
            out.resize(index + 1, Matrix4());
        out[index] = inverseModel * boneTransform->modelMatrix * bone.offsetMatrix;
    }
    return true;
}

bool cpuskin::latticeInputs(const MeshComponent& mesh, CpuSkinLattice& out, std::string* error){
    out.bindings.clear();
    const LatticeMeshState& state = mesh.lattice;
    const int count = std::min(state.count, LATTICE_MAX_PER_MESH);
    const std::vector<Vector4>& texels = state.texels;
    for (int b = 0; b < count; b++){
        const float startValue = state.recordStart[b];
        if (!(startValue >= 0.0f))
            return fail(error, "lattice binding " + std::to_string(b) + ": no record");
        const size_t start = static_cast<size_t>(nearestInt(startValue));
        if (start + LATTICE_RECORD_HEADER > texels.size())
            return fail(error, "lattice binding " + std::to_string(b) + ": its record runs past the packed texels");

        CpuSkinLatticeBinding binding;
        Matrix4 toNormalized;
        Matrix4 toObject;
        for (int c = 0; c < 4; c++){
            toNormalized.setColumn(c, texels[start + c]);
            toObject.setColumn(c, texels[start + 4 + c]);
        }
        binding.objectToNormalized = toNormalized;
        binding.normalizedToObject = toObject;
        const Vector4& grid = texels[start + 8];
        binding.lattice.enabled = true;
        binding.lattice.resolutionX = nearestInt(grid.x);
        binding.lattice.resolutionY = nearestInt(grid.y);
        binding.lattice.resolutionZ = nearestInt(grid.z);
        const int interpolation = nearestInt(grid.w);
        binding.lattice.interpolation = (interpolation == 2) ? LatticeInterpolation::CUBIC
            : ((interpolation == 1) ? LatticeInterpolation::SMOOTH : LatticeInterpolation::SHARP);
        const Vector4& settings = texels[start + 9];
        binding.stage = nearestInt(settings.x);
        binding.weight = settings.y;
        binding.lattice.outside = (nearestInt(settings.z) == 1) ? LatticeOutside::FADE : LatticeOutside::EXTEND;
        binding.lattice.fadeDistance = settings.w;
        binding.mask = maskOf(nearestInt(texels[start + 10].x));
        binding.submesh = state.submesh[b];
        binding.moving = (state.movingBindings >> b) & 1u;

        const int rx = binding.lattice.resolutionX;
        const int ry = binding.lattice.resolutionY;
        const int rz = binding.lattice.resolutionZ;
        if (rx < LATTICE_MIN_RESOLUTION || rx > LATTICE_MAX_RESOLUTION || ry < LATTICE_MIN_RESOLUTION ||
                ry > LATTICE_MAX_RESOLUTION || rz < LATTICE_MIN_RESOLUTION || rz > LATTICE_MAX_RESOLUTION)
            return fail(error, "lattice binding " + std::to_string(b) + ": resolution out of range");
        const size_t padded = static_cast<size_t>(rx + 2) * static_cast<size_t>(ry + 2) * static_cast<size_t>(rz + 2);
        if (start + LATTICE_RECORD_HEADER + padded > texels.size())
            return fail(error, "lattice binding " + std::to_string(b) + ": its grid runs past the packed texels");
        binding.padded.resize(padded);
        for (size_t p = 0; p < padded; p++){
            const Vector4& offset = texels[start + LATTICE_RECORD_HEADER + p];
            binding.padded[p] = Vector3(offset.x, offset.y, offset.z);
        }
        // only the padded grid deforms (LatticeComponent::offsets is not read past this point)
        binding.lattice.offsets.clear();
        out.bindings.push_back(std::move(binding));
    }
    return true;
}

bool cpuskin::cpuDeform(const CpuSkinSource& source, const std::vector<Matrix4>& skinMatrices, const float* morphWeights,
                        const CpuSkinLattice* lattice, CpuSkinResult& out, std::string* error){
    const size_t count = source.vertexCount();
    out.positions.assign(count, Vector3(0.0f, 0.0f, 0.0f));
    out.normals.assign(count, Vector3(0.0f, 0.0f, 0.0f));
    out.tangents.assign(count, Vector4(0.0f, 0.0f, 0.0f, 0.0f));

    for (const CpuSkinSubmesh& submesh : source.submeshes){
        if (submesh.hasSkinning && skinMatrices.empty())
            return fail(error, submeshLabel(submesh.submesh) + " is skinned and no skin matrices were given");
    }

    float weights[MAX_MORPHTARGETS] = {};
    if (morphWeights)
        std::memcpy(weights, morphWeights, sizeof(weights));

    const bool latticeVariant = lattice && !lattice->bindings.empty();

    // the bindings of one submesh at one stage, in order (lattice.glsl's loop)
    std::vector<lattice::ObjectSpaceLattice> stage0;
    std::vector<lattice::ObjectSpaceLattice> stage1;

    for (const CpuSkinSubmesh& submesh : source.submeshes){
        stage0.clear();
        stage1.clear();
        if (latticeVariant){
            for (const CpuSkinLatticeBinding& binding : lattice->bindings){
                // LatticeRender::getUniformBlock's bitmask: moving and for this submesh
                if (!binding.moving || (binding.submesh >= 0 && binding.submesh != (int)submesh.submesh))
                    continue;
                lattice::ObjectSpaceLattice os;
                os.lattice = &binding.lattice;
                os.padded = &binding.padded;
                os.objectToNormalized = binding.objectToNormalized;
                os.normalizedToObject = binding.normalizedToObject;
                os.weight = binding.weight;
                os.mask = binding.mask;
                // without skinning every binding deforms after the instance matrix
                if (submesh.hasSkinning && binding.stage == 0)
                    stage0.push_back(os);
                else if (!submesh.hasSkinning || binding.stage == 1)
                    stage1.push_back(os);
            }
        }

        for (uint32_t v = submesh.firstVertex; v < submesh.firstVertex + submesh.vertexCount; v++){
            // morph targets (getMorphPosition / Normal / Tangent)
            Vector3 position = source.positions[v];
            for (int t = 0; t < submesh.morphTargets; t++){
                if ((submesh.morphPositionMask >> t) & 1u)
                    position = position + source.morphPositions[t][v] * weights[t];
            }
            Vector3 normal = source.normals[v];
            for (int t = 0; t < submesh.morphNormalTargets; t++){
                if ((submesh.morphNormalMask >> t) & 1u)
                    normal = normal + source.morphNormals[t][v] * weights[t];
            }
            const Vector4& tangent4 = source.tangents[v];
            Vector3 tangent(tangent4.x, tangent4.y, tangent4.z);
            for (int t = 0; t < submesh.morphTangentTargets; t++){
                if ((submesh.morphTangentMask >> t) & 1u)
                    tangent = tangent + source.morphTangents[t][v] * weights[t];
            }

            Mat bone;
            if (submesh.hasSkinning)
                boneTransform(skinMatrices, source.boneIds[v], source.boneWeights[v], bone);

            if (latticeVariant){
                // mesh.vert's HAS_LATTICE path: defaults for what the variant does not have
                if (!submesh.hasNormal)
                    normal = Vector3(0.0f, 1.0f, 0.0f);
                if (!submesh.hasTangent)
                    tangent = Vector3(1.0f, 0.0f, 0.0f);
                const Vector4* color = (submesh.colorComponents > 0) ? &source.colors[v] : nullptr;
                for (const lattice::ObjectSpaceLattice& binding : stage0)
                    lattice::deformVertex(binding, position, &normal, &tangent, nullptr, color);
                if (submesh.hasSkinning){
                    position = skinPosition(bone, position);
                    normal = skinDirection(bone, normal);
                    tangent = skinDirection(bone, tangent);
                }
                normal = normalized(normal);
                tangent = normalized(tangent);
                for (const lattice::ObjectSpaceLattice& binding : stage1)
                    lattice::deformVertex(binding, position, &normal, &tangent, nullptr, color);
            }else{
                if (submesh.hasSkinning){
                    position = skinPosition(bone, position);
                    normal = skinDirection(bone, normal);
                    tangent = skinDirection(bone, tangent);
                }
                if (submesh.hasNormal)
                    normal = normalized(normal);
                if (submesh.hasTangent)
                    tangent = normalized(tangent);
            }

            out.positions[v] = position;
            if (submesh.hasNormal)
                out.normals[v] = normal;
            if (submesh.hasTangent)
                out.tangents[v] = Vector4(tangent.x, tangent.y, tangent.z, tangent4.w);
        }
    }
    return true;
}

bool cpuskin::deformEntity(Scene* scene, Entity meshEntity, CpuSkinResult& out, std::string* error, CpuSkinSource* source){
    CpuSkinSource own;
    CpuSkinSource& built = source ? *source : own;
    if (!buildSource(scene, meshEntity, built, error))
        return false;
    std::vector<Matrix4> skin;
    if (!computeSkinMatrices(scene, meshEntity, skin, error))
        return false;
    const MeshComponent& mesh = scene->getComponent<MeshComponent>(meshEntity);
    CpuSkinLattice lattice;
    if (!latticeInputs(mesh, lattice, error))
        return false;
    return cpuDeform(built, skin, mesh.morphWeights, &lattice, out, error);
}
