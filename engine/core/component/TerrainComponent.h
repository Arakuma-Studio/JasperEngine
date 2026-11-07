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

#include "buffer/InterleavedBuffer.h"
#include "buffer/IndexBuffer.h"
#include "texture/Material.h"
#include "ecs/Entity.h"
#include "Engine.h"

#include <string>
#include <vector>

namespace jasperengine{

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
    enum class TerrainCliffProjection : int { AUTO, FLAT_ONLY };

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

    // One painted surface. The maps are separate inputs so an existing material can fill
    // them without repacking images; the renderer packs them into its own array slices.
    struct TerrainSurfaceLayer{
        // Off is the historical layer: it blends its color and leaves every other property
        // to the terrain material, with the texture alpha carrying the blending height.
        bool pbr = false;

        Texture colorTexture;
        Texture normalTexture;
        Texture roughnessTexture; //green channel, or the only channel of a grayscale map
        Texture metallicTexture; //blue channel, same fallback
        Texture occlusionTexture; //red channel
        Texture heightTexture; //blending height only, never geometry

        Vector4 colorFactor = Vector4(1, 1, 1, 1); //linear tint; PBR layers ignore alpha
        float normalStrength = 1;
        float roughnessFactor = 1;
        float metallicFactor = 0;
        float occlusionStrength = 1;

        Vector2 uvScale = Vector2(1, 1); //multiplies the shared detail tiling
        Vector2 uvOffset;

        // Stochastic Tiling (shaderlib/includes/stochastic.glsl, Mikkelsen's hex-tiling):
        // every map of the layer is read on a lattice of hexagonal cells, each with its own
        // random offset, blended at their borders, so the tiling does not repeat. Three
        // fetches per map instead of one where cells meet.
        bool stochasticTiling = false;
        float stochasticCellSize = 1.0f;        //cell width in texture repeats (0.25-4)
        float stochasticBlendSharpness = 0.4f;  //0-1: how narrow the blend between cells is
        float stochasticContrast = 0.45f;       //0-1: keeps the texture's contrast where cells blend
        float stochasticRotation = 0.0f;        //0-1: random turn per cell, share of +-180 degrees
        // hex tiling (includes/hex_tiling.glsl): the flat projection sampled on three randomly
        // rotated copies instead of its near and far tiling, which hides the repetition; the
        // side planes stay planar. hexRotation 0..1 is the share of a turn a tile may rotate.
        bool hexTiling = false;
        float hexRotation = 1;
        // T3 R11: Auto is the tier's (triplanar at High, one plane below); Flat Only never reads a
        // side plane, for grass or sand that never sits on a cliff
        TerrainCliffProjection cliffProjection = TerrainCliffProjection::AUTO;

        bool operator == (const TerrainSurfaceLayer& other) const{
            return pbr == other.pbr &&
                   colorTexture == other.colorTexture &&
                   normalTexture == other.normalTexture &&
                   roughnessTexture == other.roughnessTexture &&
                   metallicTexture == other.metallicTexture &&
                   occlusionTexture == other.occlusionTexture &&
                   heightTexture == other.heightTexture &&
                   colorFactor == other.colorFactor &&
                   normalStrength == other.normalStrength &&
                   roughnessFactor == other.roughnessFactor &&
                   metallicFactor == other.metallicFactor &&
                   occlusionStrength == other.occlusionStrength &&
                   uvScale == other.uvScale &&
                   uvOffset == other.uvOffset &&
                   stochasticTiling == other.stochasticTiling &&
                   stochasticCellSize == other.stochasticCellSize &&
                   stochasticBlendSharpness == other.stochasticBlendSharpness &&
                   stochasticContrast == other.stochasticContrast &&
                   stochasticRotation == other.stochasticRotation &&
                   hexTiling == other.hexTiling &&
                   hexRotation == other.hexRotation &&
                   cliffProjection == other.cliffProjection;
        }

        bool operator != (const TerrainSurfaceLayer& other) const{
            return !(*this == other);
        }
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

        bool needUpdateTerrain = true;
        bool needUpdateTexture = false;
        bool needUpdateFoliage = true;
    };

    // The one place a material becomes a layer, shared by the engine setter and the editor
    inline TerrainSurfaceLayer terrainLayerFromMaterial(const Material& material, const TerrainSurfaceLayer& previous){
        TerrainSurfaceLayer layer;
        layer.pbr = true;
        layer.colorTexture = material.baseColorTexture;
        layer.colorFactor = material.baseColorFactor;
        layer.normalTexture = material.normalTexture;
        layer.roughnessTexture = material.metallicRoughnessTexture;
        layer.metallicTexture = material.metallicRoughnessTexture;
        layer.occlusionTexture = material.occlusionTexture;
        layer.roughnessFactor = material.roughnessFactor;
        layer.metallicFactor = material.metallicFactor;
        // Tiling is the layer's own, and no material carries it; so is how it is sampled
        layer.uvScale = previous.uvScale;
        layer.uvOffset = previous.uvOffset;
        layer.stochasticTiling = previous.stochasticTiling;
        layer.stochasticCellSize = previous.stochasticCellSize;
        layer.stochasticBlendSharpness = previous.stochasticBlendSharpness;
        layer.stochasticContrast = previous.stochasticContrast;
        layer.stochasticRotation = previous.stochasticRotation;
        layer.hexTiling = previous.hexTiling;
        layer.hexRotation = previous.hexRotation;
        layer.cliffProjection = previous.cliffProjection;
        return layer;
    }

    // Every map a layer can hold, so callers walking terrain assets cannot miss one
    template<typename F>
    inline void forEachTerrainLayerTexture(TerrainSurfaceLayer& layer, F&& fn){
        fn(layer.colorTexture);
        fn(layer.normalTexture);
        fn(layer.roughnessTexture);
        fn(layer.metallicTexture);
        fn(layer.occlusionTexture);
        fn(layer.heightTexture);
    }

    // A layer with its own surface, its own tiling or Stochastic Tiling needs the wider
    // terrain shader (its per-layer block)
    inline bool hasTerrainSurfaceLayers(const TerrainComponent& terrain){
        for (const TerrainSurfaceLayer& layer : terrain.surfaceLayers){
            if (layer.pbr || layer.uvScale != Vector2(1, 1) || layer.uvOffset != Vector2(0, 0) || layer.stochasticTiling ||
                    layer.hexTiling || layer.cliffProjection != TerrainCliffProjection::AUTO){
                return true;
            }
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
