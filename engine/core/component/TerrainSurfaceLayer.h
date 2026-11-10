// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_SURFACE_LAYER_H
#define TERRAIN_SURFACE_LAYER_H

// A header of its own so a component that holds layers (T6's voxel material slots) does not bring
// in TerrainComponent.h and everything that includes it (T1: component fan-out)

#include "math/Vector2.h"
#include "math/Vector4.h"
#include "texture/Material.h"
#include "texture/Texture.h"

namespace jasperengine{

    // T3 R11: a layer's projection on steep ground. Stored as an int: append only.
    enum class TerrainCliffProjection : int { AUTO, FLAT_ONLY };

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

}

#endif // TERRAIN_SURFACE_LAYER_H
