// SPDX-License-Identifier: MIT
//
// Moved out of JoltShapeBuilder (physics-independent; docs/specs/S19-navigation.md R3).

#include "MeshTriangles.h"
#include "model/ModelCpuData.h"

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"

#include <map>
#include <string>
#include <utility>

using namespace jasperengine;

MeshTriangles::VertexRange MeshTriangles::vertexRange(const MeshSourceView& view){
    return VertexRange(view.vertexBuffer->getData(), view.vertexAttr.getOffset(), view.vertexAttr.getCount());
}

std::vector<MeshTriangles::MeshSourceView> MeshTriangles::collectMeshSourceViews(MeshComponent* mesh){
    std::vector<MeshSourceView> views;
    if (!mesh) return views;
    ModelCpuData::ensure(*mesh);

    std::map<std::string, Buffer*> buffers;
    if (mesh->buffer.getSize() > 0) buffers["vertices"] = &mesh->buffer;
    if (mesh->indices.getSize() > 0) buffers["indices"] = &mesh->indices;
    for (unsigned int i = 0; i < mesh->numExternalBuffers; i++) buffers[mesh->eBuffers[i].getName()] = &mesh->eBuffers[i];

    MeshSourceView view;
    for (auto const& buf : buffers){
        if (buf.second->getAttribute(AttributeType::POSITION)){
            view.vertexBuffer = buf.second;
            view.vertexAttr = *buf.second->getAttribute(AttributeType::POSITION);
        }
        if (buf.second->getAttribute(AttributeType::INDEX)){
            view.indexBuffer = buf.second;
            view.indexAttr = *buf.second->getAttribute(AttributeType::INDEX);
        }
    }

    // An empty name keeps the buffer already in view; an unknown one drops the
    // attribute, whose offset only means anything against its own buffer.
    auto resolveNamedBuffer = [&buffers](const std::string& bufferName, Buffer* current) -> Buffer* {
        if (bufferName.empty()) return current;
        auto named = buffers.find(bufferName);
        return named != buffers.end() ? named->second : NULL;
    };

    for (size_t i = 0; i < mesh->numSubmeshes; i++){
        for (auto const& attr : mesh->submeshes[i].attributes){
            if (attr.first == AttributeType::POSITION){
                if (Buffer* resolved = resolveNamedBuffer(attr.second.getBufferName(), view.vertexBuffer)){
                    view.vertexBuffer = resolved;
                    view.vertexAttr = attr.second;
                }
            }
            if (attr.first == AttributeType::INDEX){
                if (Buffer* resolved = resolveNamedBuffer(attr.second.getBufferName(), view.indexBuffer)){
                    view.indexBuffer = resolved;
                    view.indexAttr = attr.second;
                }
            }
        }

        if (view.vertexBuffer) views.push_back(view);
    }

    return views;
}

std::vector<Entity> MeshTriangles::meshSourceEntities(Scene* scene, Entity meshEntity){
    std::vector<Entity> sources;

    MeshComponent* mesh = scene->findComponent<MeshComponent>(meshEntity);
    if (mesh && mesh->numSubmeshes > 0){
        sources.push_back(meshEntity);
        return sources;
    }

    if (ModelComponent* model = scene->findComponent<ModelComponent>(meshEntity)){
        for (auto const& node : model->meshNodesMapping){
            MeshComponent* nodeMesh = scene->findComponent<MeshComponent>(node.second);
            if (nodeMesh && nodeMesh->numSubmeshes > 0){
                sources.push_back(node.second);
            }
        }
    }

    return sources;
}

float MeshTriangles::linearDeterminant(const Matrix4& m){
    return m.get(0, 0) * (m.get(1, 1) * m.get(2, 2) - m.get(2, 1) * m.get(1, 2)) -
           m.get(1, 0) * (m.get(0, 1) * m.get(2, 2) - m.get(2, 1) * m.get(0, 2)) +
           m.get(2, 0) * (m.get(0, 1) * m.get(1, 2) - m.get(1, 1) * m.get(0, 2));
}

void MeshTriangles::appendMeshTriangles(MeshComponent* mesh, const Matrix4& toTarget,
                                           std::vector<Vector3>& vertices, std::vector<uint32_t>& indices){
    std::map<VertexRange, uint32_t> vertexBases;
    // a mirroring map turns the triangles inside out: wound back so they face out again
    const bool mirrored = linearDeterminant(toTarget) < 0.0f;
    const size_t firstIndex = indices.size();

    for (MeshSourceView& view : collectMeshSourceViews(mesh)){
        const VertexRange range = vertexRange(view);
        const int verticesize = int(view.vertexAttr.getCount());

        auto base = vertexBases.find(range);
        if (base == vertexBases.end()){
            base = vertexBases.emplace(range, (uint32_t)vertices.size()).first;
            for (int i = 0; i < verticesize; i++){
                vertices.push_back(toTarget * view.vertexBuffer->getVector3(&view.vertexAttr, i));
            }
        }

        // Indices are local to their submesh, rebase onto its vertices
        const uint32_t vertexBase = base->second;

        if (!view.indexBuffer){
            // No index buffer means a plain triangle list
            for (int i = 0; i + 2 < verticesize; i += 3){
                indices.push_back(vertexBase + i);
                indices.push_back(vertexBase + i + 1);
                indices.push_back(vertexBase + i + 2);
            }
            continue;
        }

        int numIdxTriangles = int(view.indexAttr.getCount() / 3);
        for (int i = 0; i < numIdxTriangles; i++){
            for (int j = 0; j < 3; j++){
                uint32_t indice = 0;
                if (view.indexAttr.getDataType() == AttributeDataType::UNSIGNED_INT){
                    indice = view.indexBuffer->getUInt32(&view.indexAttr, (3 * i) + j);
                }else if (view.indexAttr.getDataType() == AttributeDataType::UNSIGNED_SHORT){
                    indice = view.indexBuffer->getUInt16(&view.indexAttr, (3 * i) + j);
                }
                indices.push_back(vertexBase + indice);
            }
        }
    }

    if (mirrored){
        for (size_t t = firstIndex; t + 2 < indices.size(); t += 3) std::swap(indices[t + 1], indices[t + 2]);
    }
}

bool MeshTriangles::isGeometryLoading(Scene* scene, Entity entity){
    Entity current = entity;

    while (current != NULL_ENTITY){
        ModelComponent* model = scene->findComponent<ModelComponent>(current);
        if (model && !model->filename.empty() && model->needUpdateModel){
            return true;
        }

        TerrainComponent* terrain = scene->findComponent<TerrainComponent>(current);
        if (terrain && !terrain->heightMap.empty() && terrain->needUpdateTerrain){
            return true;
        }

        Transform* transform = scene->findComponent<Transform>(current);
        current = transform ? transform->parent : NULL_ENTITY;
    }

    return false;
}
