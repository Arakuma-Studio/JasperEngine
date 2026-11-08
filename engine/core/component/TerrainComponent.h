// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_COMPONENT_H
#define TERRAIN_COMPONENT_H

#define MAX_TERRAINGRID 16
// CDLOD node selection is per-view: index 0 is the main camera (also reused by the
// shadow depth pass), 1..N-1 are render-to-texture cameras (mirror reflections,
// scene captures), each selecting + morphing its own node cut. Extra RTT cameras
// beyond this cap fall back to the main camera's selection (view 0).
#define MAX_TERRAIN_VIEWS 4

// The quadtree materializes rootGridSize^2 * (4^levels - 1)/3 nodes, so "levels"
// grows the node count exponentially. These bounds keep the node vector from
// requesting an impossible allocation (a large "levels" would abort with bad_alloc).
// MAX_TERRAIN_LEVELS also keeps getTerrainGridArraySize's 4^i math within size_t.
#define MAX_TERRAIN_LEVELS 20
#define MAX_TERRAIN_NODES 2000000u

// A blend map weights three layers in its RGB, and on a terrain of more than nine layers one more by
// its alpha, inverted (T3 R15): saved maps are opaque, which reads as weight 0. A shader's block keeps
// 9 entries up to 9 layers (TERRAIN_EXTENDED_LAYERS: 12).
#define MAX_TERRAIN_BLENDMAPS 3
#define MAX_TERRAIN_LAYERS 12
#define TERRAIN_SHADER_BASE_LAYERS 9

// One array texture holds every layer map, so a bigger source is downscaled into it
// instead of resizing every other slice up with it.
#define MAX_TERRAIN_DETAIL_SIZE 2048

#include "asset/AssetRef.h"
#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "component/TerrainSurfaceLayer.h"
#include "texture/Material.h"
#include "ecs/Entity.h"
#include "Engine.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct TerrainEditState;    // object/TerrainEdit.h

    struct TerrainNode{
        //-----u_vs_terrainNodeParams
        Vector2 position = Vector2(0, 0);
        float size = 0;
        float currentRange = 0;
        float resolution = 0; //int
        uint8_t _pad_20[12];
        //-----

        size_t childs[4];
        bool hasChilds = false;

        float maxHeight = 0;
        float minHeight = 0;
        
        float visible = false;
    };

    // Per-view CDLOD state. Index 0 is the main camera (also reused by the shadow
    // depth pass); 1..N-1 are render-to-texture cameras (mirror reflections, scene
    // captures), each selecting and morphing its own node cut. Grouping the per-view
    // buffers in a struct (instead of a 2-D array of InterleavedBuffer) keeps MSVC's
    // code generation from crashing when the ECS pool instantiates TerrainComponent.
    // T3 R49. All stored as ints: append only.
    enum class TerrainQualitySetting : int { AUTO, HIGH, MEDIUM, LOW };
    enum class TerrainOffscreenShadows : int { AUTO, ON, OFF };
    enum class TerrainMacroMode : int { OFF, NOISE, BASE_MAP };

    struct TerrainView{
        // 0 = fullRes, 1 = halfRes; selected and uploaded independently once per frame
        InterleavedBuffer nodesbuffer[2];
        // the TerrainComponent::nodes each record of nodesbuffer is, in its order (a shadow slot
        // culls them one by one, spec S55 R5)
        std::vector<uint32_t> nodeIndices[2];
        // per-view morph origin, paired with this view's node selection
        Vector3 nodesEyePos;
        bool needUpdateNodesBuffer = false;
    };

    // A scattered mesh layer painted over the terrain. The editor authors its density map;
    // instances are resolved from that map instead of being stored.
    struct TerrainFoliageLayer{
        AssetRef meshPath;
        Texture densityMap;

        float density = 1; //instances per square world unit where the map is fully painted
        float minScale = 0.8f;
        float maxScale = 1.2f;
        float rotationJitter = 1; //share of a full turn of random yaw
        float alignToNormal = 0; //0 stands instances upright, 1 lays them along the surface
        float minSlope = 0; //degrees
        float maxSlope = 35;
        float minHeight = 0; //normalized against the terrain maxHeight
        float maxHeight = 1;
        float drawDistance = 50;
        // nearer instances fade out (grow in over [0.75, 1] of it): with a nearer copy of the layer
        // whose drawDistance equals it, the two cross-fade (a far detail level with a cheaper mesh)
        float minDrawDistance = 0;
        unsigned int seed = 0;
        bool castShadows = true; // off spares grass-scale scatter the shadow cascades

        // T5: each default changes nothing, so a layer made before them places and looks the same
        float minHeightRatio = 1; //Min/Max Height, over the width scale
        float maxHeightRatio = 1;
        Vector4 healthyColor = Vector4(1, 1, 1, 1); //linear tint
        Vector4 dryColor = Vector4(1, 1, 1, 1);
        float noiseSpread = 0; //noise cells per world unit between dry and healthy; 0 picks per instance
        float windBend = 0; //0 draws the variant without wind
        float densityFalloffStart = 1; //share of the draw distance where density starts to fall
        float farDensity = 1; //what density falls to at the draw distance
        float densityScale = 1; //the layer's own multiplier of Detail Density
        unsigned int algorithmVersion = 1; //what its seed means: a changed scatter gets a new one
    };

    enum class TerrainTreeWindMask : uint8_t{
        Height,
        VertexColor
    };

    enum class TerrainTreeCollision : uint8_t{
        None,
        Capsule
    };

    // One kind of tree of a Terrain (T5 R13, R16): the instances live in treeData, not in the scene
    struct TerrainTreePrototype{
        unsigned int id = 0; //stable: the store's records name it
        AssetRef meshPath;
        std::vector<AssetRef> lodMeshes; //explicit LOD 1..3; empty: the generated levels of meshPath
        std::vector<float> lodDistances; //where LOD 1, 2, 3 start, world units
        float bendFactor = 0.5f;
        TerrainTreeWindMask windMask = TerrainTreeWindMask::Height;
        unsigned int shadowLodMask = 0x7F; //bit n: mesh LOD n casts; bit 7: the impostor casts
        TerrainTreeCollision collision = TerrainTreeCollision::None;
        float collisionRadius = 0.5f;
        float collisionHeight = 4.0f;
        bool navObstacle = false;
        // the baked impostor: atlases, and what they were baked from
        Texture impostorAlbedo;
        Texture impostorNormal;
        int impostorViews = 0; //0: none baked
        Vector3 impostorBoundsCenter;
        Vector2 impostorSize;
        std::string impostorSourceHash;
    };

    struct JASPERENGINE_API TerrainComponent{
        // component checkbox: its system skips a disabled component
        bool enabled = true;

        // per-view CDLOD node selection (see TerrainView). Extra RTT cameras beyond
        // MAX_TERRAIN_VIEWS fall back to the main camera's selection (view 0).
        TerrainView views[MAX_TERRAIN_VIEWS];

        Texture heightMap;
        // blendMaps[m] channel c weights surfaceLayers[m * 3 + c]
        std::vector<Texture> blendMaps;
        std::vector<TerrainSurfaceLayer> surfaceLayers;

        std::vector<TerrainFoliageLayer> foliageLayers;
        std::vector<TerrainTreePrototype> treePrototypes;
        AssetRef treeData; //the .jetrees file of its trees

        // Terrain Settings (T5 R20). An old scene loads these; a new Terrain sets Detail Distance
        // to 80 (applyNewTerrainDefaults).
        float detailDistance = 0; //0: each layer's Draw Distance only
        float detailDensity = 1;
        float treeDistance = 1000;
        float billboardStart = 80;
        float fadeLength = 5;
        int maxMeshTrees = 200;
        Vector3 windDirection = Vector3(0, 0, 1);
        float windSpeed = 1;
        float windSize = 8;
        float windBending = 0.5f;

        bool autoSetRanges = true;
        bool heightMapLoaded = false;

        Vector2 offset;
        std::vector<float> ranges;

        //using std::vector to avoid chkstk.asm stack overflow error in Windows
        std::vector<TerrainNode> nodes;
        unsigned int numNodes = 0;

        size_t grid[MAX_TERRAINGRID]; //root nodes

        //-----u_vs_terrainParams
        // eyePos is the morph origin for the view currently being drawn; it is set
        // from views[view].nodesEyePos right before the uniform upload (keep this
        // block's memory layout intact — it is uploaded as a contiguous struct).
        Vector3 eyePos;
        float terrainSize = 200;
        float maxHeight = 5;
        float resolution = 32; //int
        float textureBaseTiles = 1; //int
        float textureDetailTiles = 20; //int
        //-----

        int rootGridSize = 2;
        int levels = 6;

        // R8, 0 = hole, 255 = solid (T3 R18); absent means no holes, and no cost anywhere
        Texture holeMap;

        // T3 R49: the terrain's material features. Each costs nothing at its default.
        TerrainQualitySetting quality = TerrainQualitySetting::AUTO;
        bool limitLayersPerPixel = false;
        TerrainOffscreenShadows offscreenShadows = TerrainOffscreenShadows::AUTO;
        TerrainMacroMode macroMode = TerrainMacroMode::OFF;
        float macroScale = 64; //metres
        float macroStrength = 0.35f;
        Vector3 macroTintA = Vector3(0.80f, 0.84f, 0.76f);
        Vector3 macroTintB = Vector3(1.04f, 1.0f, 0.92f);
        float macroSlopeFade = 0.33f;
        bool resampleEnabled = false;
        float resampleNear = 30; //metres
        float resampleFar = 120; //metres
        float resampleReduction = 0.25f;
        float heightBlendSharpness = 0;
        bool macroNormal = false;

        // T3 R29, T6 R40: while above 0 (a terrain with an attached Automatic Voxel Volume, in Play) a
        // last fill keeps the map, all solid, and reloads nothing, so a first in-game dig costs no
        // variant reload. Not saved, not reflected, and reset on clone.
        int holeMapHolds = 0;
        // a hole map that fails to load, or is not R8, warns once and draws no holes (T3 R29)
        bool holeMapWarned = false;
        // bumped by each change of the map's texels, for who placed something by them (T3 R26). Not saved
        uint32_t holeMapRevision = 0;

        // Whether a running game may write this terrain's maps (T1 R77, R81)
        bool allowRuntimeEdits = false;

        // Goes up when the surface moved without a rebuild (T1 R23): what follows the surface
        // compares it, as needUpdateTerrain says the terrain was built again
        uint32_t surfaceRevision = 0;
        // Null until the terrain is first edited (T1 R53, DC10). Neither is saved, and a clone
        // starts without them.
        std::shared_ptr<TerrainEditState> editState;

        bool needUpdateTerrain = true;
        bool needUpdateTexture = false;
        bool needUpdateFoliage = true;
    };

    // A layer with its own surface, its own tiling or Stochastic Tiling needs the wider
    // terrain shader (its per-layer block)
    inline bool hasTerrainSurfaceLayers(const TerrainComponent& terrain){
        for (const TerrainSurfaceLayer& layer : terrain.surfaceLayers){
            if (layer.pbr || layer.uvScale != Vector2(1, 1) || layer.uvOffset != Vector2(0, 0) || layer.stochasticTiling ||
                    layer.hexTiling || layer.cliffProjection != TerrainCliffProjection::AUTO){
                return true;
            }
        }
        // a terrain with no layer draws its base material alone, on the narrow path (T3, spec Deviations)
        if (terrain.surfaceLayers.empty()){
            return false;
        }
        // T3 R9-R14: a material feature works on the wide path
        return terrain.macroMode != TerrainMacroMode::OFF || terrain.resampleEnabled ||
               terrain.heightBlendSharpness > 0.0f || terrain.macroNormal;
    }

    inline bool hasTerrainCliffLayers(const TerrainComponent& terrain){
        for (const TerrainSurfaceLayer& layer : terrain.surfaceLayers){
            if (layer.cliffProjection != TerrainCliffProjection::AUTO){
                return true;
            }
        }
        return false;
    }

    // a layer hex tiles (TERRAIN_HEX_TILING): the terrain draws its surface layers' path
    inline bool hasTerrainHexLayers(const TerrainComponent& terrain){
        for (const TerrainSurfaceLayer& layer : terrain.surfaceLayers){
            if (layer.hexTiling){
                return true;
            }
        }
        return false;
    }

    // a layer takes Stochastic Tiling: a hex tiled terrain's variant then carries that path too
    // (TERRAIN_HEX_STOCHASTIC)
    inline bool hasTerrainStochasticLayers(const TerrainComponent& terrain){
        for (const TerrainSurfaceLayer& layer : terrain.surfaceLayers){
            if (layer.stochasticTiling){
                return true;
            }
        }
        return false;
    }

}

#endif //TERRAIN_COMPONENT_H
