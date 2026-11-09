// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R34, AC10: the collector reaches every variant a player can ask for, by the tiers the export ships.

#include "Scene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/Terrain.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderVariantCollector.h"
#include "subsystem/MeshSystem.h"
#include "terrain/TerrainQuality.h"
#include "texture/MaterialPbr.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    struct TierKeys {
        bool high = false;
        bool medium = false;
        bool low = false;
        bool debug = false;
        bool holes = false;
    };

    TierKeys collect(Scene& scene, uint32_t tiers){
        ShaderVariantContent content;
        content.registry = &scene;
        content.entities = scene.getEntityList();
        std::set<ShaderKey> keys;
        ShaderVariantCollector::collectScene(&scene, content, {}, keys, std::filesystem::path(), false, false, {}, false, false, PbrTier::FULL, tiers);
        TierKeys out;
        for (ShaderKey key : keys){
            const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
            if (ShaderPool::getShaderTypeFromKey(key) != ShaderType::MESH || !(properties & shaderPropertyBit(17))){
                continue;
            }
            // each variant also ships the built-in program it draws with while it compiles (custom id 0),
            // which has no defines: not a tier of its own
            if (ShaderPool::getCustomIdFromKey(key) == 0){
                out.holes |= (properties & ShaderPool::getTerrainHolesProperty(ShaderType::MESH)) != 0;
                continue;
            }
            const std::string name = ShaderPool::getCustomShaderName(ShaderPool::getCustomIdFromKey(key));
            const bool medium = name.find("TERRAIN_QUALITY_MEDIUM") != std::string::npos;
            const bool low = name.find("TERRAIN_QUALITY_LOW") != std::string::npos;
            out.medium |= medium;
            out.low |= low;
            // High is the tier that keeps a limited terrain's TERRAIN_LAYER_LIMIT
            out.high |= name.find("TERRAIN_LAYER_LIMIT") != std::string::npos;
            out.debug |= name.find("TERRAIN_DEBUG_VIEW") != std::string::npos;
            out.holes |= (properties & ShaderPool::getTerrainHolesProperty(ShaderType::MESH)) != 0;
        }
        return out;
    }

}

TEST(ShaderVariantCollectorTerrain, TiersReachable){
    Scene scene;
    // the handle owns its entity: it lives as long as the collection
    Terrain terrain(&scene);
    TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain.getEntity());
    component.heightMap.setPath("height.png");
    // only High defines the layer limit, so its key has a name of its own; it needs more layers than the
    // cap keeps, and a terrain with no layer would draw its base material alone at every tier
    component.limitLayersPerPixel = true;
    component.surfaceLayers.resize(5);
    // a project at Low on Android: Low's keys, and no High's
    const TierKeys low = collect(scene, terraincore::terrainQualityBit(terraincore::TerrainQuality::LOW));
    EXPECT_TRUE(low.low);
    EXPECT_FALSE(low.high);
    EXPECT_FALSE(low.medium);
    // a script names them: all three
    const TierKeys all = collect(scene, terraincore::TERRAIN_ALL_TIERS);
    EXPECT_TRUE(all.low);
    EXPECT_TRUE(all.medium);
    EXPECT_TRUE(all.high);
    EXPECT_FALSE(all.debug) << "never collected for an export";
    EXPECT_FALSE(all.holes) << "no hole map, no Thl";
}

TEST(ShaderVariantCollectorTerrain, HoleMapShipsThl){
    Scene scene;
    Terrain terrain(&scene);
    TerrainComponent& component = scene.getComponent<TerrainComponent>(terrain.getEntity());
    component.heightMap.setPath("height.png");
    component.holeMap.setPath("holes.png");
    EXPECT_TRUE(collect(scene, terraincore::TERRAIN_ALL_TIERS).holes);
}
