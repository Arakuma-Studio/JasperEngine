// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef TILEMAP_MESH_ACCESS_H
#define TILEMAP_MESH_ACCESS_H

// The fields of a Tilemap Layer's (or a Free Tilemap's) Mesh that the editor's tilemap code reads
// and sets: its sort fields, its textures, and the reset a conversion makes. They live here, in
// one engine unit, so the editor's tilemap sources do not include the Mesh component (each one
// that does rebuilds with it: tools/check_fanout.py).

#include "Export.h"
#include "ecs/Entity.h"
#include "math/Vector4.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    class Scene;
    class Texture;

    namespace tilemapmesh{

        JASPERENGINE_API bool hasMesh(Scene* scene, Entity entity);
        // 0 without a Mesh
        JASPERENGINE_API int32_t sortingOrder(Scene* scene, Entity entity);
        JASPERENGINE_API int32_t sortingLayer(Scene* scene, Entity entity);
        // the base colour texture path of each submesh in use
        JASPERENGINE_API std::vector<std::string> submeshTextures(Scene* scene, Entity entity);
        // submesh 0's material colour; white without a Mesh
        JASPERENGINE_API Vector4 baseColor(Scene* scene, Entity entity);
        // submesh 0's base colour texture (its sampler is every page's); null without a Mesh
        JASPERENGINE_API Texture* layerTexture(Scene* scene, Entity entity);

        // what a conversion to a Tilemap Layer clears, to put back on undo
        struct MeshState{
            unsigned int numSubmeshes = 0;
            bool tileLayer = false;
            std::string texture;
        };
        // empties the legacy geometry and keeps one submesh (the layer's system builds its quad)
        JASPERENGINE_API bool clearForTileLayer(Scene* scene, Entity entity, MeshState& before);
        JASPERENGINE_API void restore(Scene* scene, Entity entity, const MeshState& before);
    }

}

#endif // TILEMAP_MESH_ACCESS_H
