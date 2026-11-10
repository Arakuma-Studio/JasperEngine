// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilemapPixelArt.h"

#include "Project.h"
#include "component/TilemapLayerComponent.h"
#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelArtInput.h"
#include "texture/Texture.h"
#include "tilemap/TilemapMeshAccess.h"
#include "tilemap/TilesetPool.h"

using namespace jasperengine;

void editor::tilemap::registerTilemapPixelArtDrawables() {
    // once: registering again would forget the input cache, so closed scenes decoded every check
    static bool registered = false;
    if (registered) return;
    registered = true;
    pixelart::registerDrawableCollector("tilemap_layers", [](const SceneProject& scene, const pixelart::DrawableBase& base,
                                                             std::vector<pixelart::PixelArtDrawable>& out) {
        auto layers = scene.scene->getComponentArray<TilemapLayerComponent>();
        for (size_t i = 0; i < layers->size(); i++) {
            const TilemapLayerComponent& layer = layers->getComponentFromIndex(i);
            const Entity entity = layers->getEntity(i);
            const Texture* sampler = tilemapmesh::layerTexture(scene.scene, entity);
            if (!sampler || !layer.enabled || layer.tileset.empty())
                continue;
            const std::shared_ptr<const TilesetRuntime> tileset = TilesetPool::get(layer.tileset.path);
            if (!tileset)
                continue;
            for (const std::string& page : tileset->pages) {
                pixelart::PixelArtDrawable drawable = base((uint32_t)entity, pixelart::DrawableKind::Tilemap);
                // every page is sampled with the Mesh's texture settings (S52's page runs)
                drawable.textureField = "mesh.submeshes[0].material.baseColorTexture";
                drawable.texture = page;
                drawable.minFilter = sampler->getMinFilter();
                drawable.magFilter = sampler->getMagFilter();
                out.push_back(std::move(drawable));
            }
        }
    });
}
