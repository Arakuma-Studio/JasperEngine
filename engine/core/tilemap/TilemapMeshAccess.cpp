// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilemapMeshAccess.h"

#include "Scene.h"
#include "component/MeshComponent.h"

using namespace jasperengine;

bool tilemapmesh::hasMesh(Scene* scene, Entity entity){
    return scene && scene->findComponent<MeshComponent>(entity) != nullptr;
}

int32_t tilemapmesh::sortingOrder(Scene* scene, Entity entity){
    const MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    return mesh ? mesh->sortingOrder : 0;
}

int32_t tilemapmesh::sortingLayer(Scene* scene, Entity entity){
    const MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    return mesh ? mesh->sortingLayer : 0;
}

std::vector<std::string> tilemapmesh::submeshTextures(Scene* scene, Entity entity){
    std::vector<std::string> out;
    const MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (!mesh)
        return out;
    for (unsigned int i = 0; i < mesh->numSubmeshes; i++)
        out.push_back(mesh->submeshes[i].material.baseColorTexture.getPath());
    return out;
}

Vector4 tilemapmesh::baseColor(Scene* scene, Entity entity){
    const MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    return mesh && mesh->numSubmeshes > 0 ? mesh->submeshes[0].material.baseColorFactor : Vector4(1.0f, 1.0f, 1.0f, 1.0f);
}

Texture* tilemapmesh::layerTexture(Scene* scene, Entity entity){
    MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (!mesh || mesh->numSubmeshes == 0)
        return nullptr;
    return &mesh->submeshes[0].material.baseColorTexture;
}

bool tilemapmesh::clearForTileLayer(Scene* scene, Entity entity, MeshState& before){
    MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (!mesh)
        return false;
    before.numSubmeshes = mesh->numSubmeshes;
    before.tileLayer = mesh->tileLayer;
    before.texture = mesh->numSubmeshes > 0 ? mesh->submeshes[0].material.baseColorTexture.getPath() : std::string();
    mesh->buffer.clear();
    mesh->indices.clear();
    mesh->numSubmeshes = 1;
    if (mesh->loaded)
        mesh->needReload = true;
    return true;
}

void tilemapmesh::restore(Scene* scene, Entity entity, const MeshState& before){
    MeshComponent* mesh = scene ? scene->findComponent<MeshComponent>(entity) : nullptr;
    if (!mesh)
        return;
    mesh->numSubmeshes = before.numSubmeshes;
    mesh->tileLayer = before.tileLayer;
    if (before.numSubmeshes > 0 && mesh->submeshes[0].material.baseColorTexture.getPath() != before.texture){
        mesh->submeshes[0].material.baseColorTexture.setPath(before.texture);
        mesh->submeshes[0].needUpdateTexture = true;
    }
    if (mesh->loaded)
        mesh->needReload = true;
}
