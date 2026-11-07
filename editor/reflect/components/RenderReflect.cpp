// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The renderers: meshes, models, sprites, tilemaps, polygons, terrain, points, lines and
// instancing; and scripts and prefab roots (docs/specs/S08-reflection.md P3/P4). Their encodings
// stay their own (ComponentCodec: generated geometry left out, counted arrays, legacy keys); the
// tables give their properties, schemas, Inspector metadata and asset locators.

#include "reflect/components/MaterialReflect.h"

#include "Factory.h"
#include "Stream.h"
#include "component/InstancedMeshComponent.h"
#include "component/LineRendererComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/PointsComponent.h"
#include "component/PolygonComponent.h"
#include "component/PrefabComponent.h"
#include "component/ScriptComponent.h"
#include "component/SpriteComponent.h"
#include "component/TerrainComponent.h"
#include "component/TilemapComponent.h"

namespace jasperengine::editor::reflect {

// ---------------------------------------------------------------- shared element structs

namespace {

    constexpr FieldDesc kSpriteFrameFields[] = {
        JE_FIELD(SpriteFrameData, name),
        JE_FIELD(SpriteFrameData, rect),
    };

    constexpr uint64_t kTilemap = UpdateFlags_Tilemap;

    constexpr FieldDesc kTileRectFields[] = {
        JE_FIELD(TileRectData, name, .updateFlags = kTilemap),
        JE_FIELD(TileRectData, submeshId, .updateFlags = kTilemap),
        JE_FIELD(TileRectData, rect, .updateFlags = kTilemap),
    };

    constexpr FieldDesc kTileFields[] = {
        JE_FIELD(TileData, name, .updateFlags = kTilemap),
        JE_FIELD(TileData, rectId, .updateFlags = kTilemap),
        JE_FIELD(TileData, position, .updateFlags = kTilemap),
        JE_FIELD(TileData, width, .updateFlags = kTilemap),
        JE_FIELD(TileData, height, .updateFlags = kTilemap),
    };

    constexpr FieldDesc kPolygonPointMeshFields[] = {
        JE_FIELD(PolygonPoint, position, .updateFlags = UpdateFlags_Mesh_Reload),
        JE_FIELD(PolygonPoint, color, .flags = ColorLinear, .updateFlags = UpdateFlags_Mesh_Reload),
    };

    constexpr FieldDesc kPolygonPointUIFields[] = {
        JE_FIELD(PolygonPoint, position, .updateFlags = UpdateFlags_UI_Reload),
        JE_FIELD(PolygonPoint, color, .flags = ColorLinear, .updateFlags = UpdateFlags_UI_Reload),
    };

    constexpr uint64_t kPoints = UpdateFlags_Points;

    constexpr FieldDesc kPointFields[] = {
        JE_FIELD(PointData, position, .updateFlags = kPoints),
        JE_FIELD(PointData, color, .flags = ColorLinear, .updateFlags = kPoints),
        JE_FIELD(PointData, size, .updateFlags = kPoints),
        JE_FIELD(PointData, rotation, .updateFlags = kPoints),
        JE_FIELD(PointData, textureRect, .updateFlags = kPoints),
        JE_FIELD(PointData, visible, .updateFlags = kPoints),
    };

    constexpr uint64_t kLines = UpdateFlags_Lines;

    constexpr FieldDesc kLineFields[] = {
        JE_FIELD(LineData, pointA, .updateFlags = kLines),
        JE_FIELD(LineData, colorA, .flags = ColorLinear, .updateFlags = kLines),
        JE_FIELD(LineData, pointB, .updateFlags = kLines),
        JE_FIELD(LineData, colorB, .flags = ColorLinear, .updateFlags = kLines),
    };

    constexpr uint64_t kInstances = UpdateFlags_Instanced_Mesh;

    constexpr FieldDesc kInstanceFields[] = {
        JE_FIELD(InstanceData, position, .updateFlags = kInstances),
        JE_FIELD(InstanceData, rotation, .updateFlags = kInstances),
        JE_FIELD(InstanceData, scale, .updateFlags = kInstances),
        JE_FIELD(InstanceData, color, .flags = ColorLinear, .updateFlags = kInstances),
        JE_FIELD(InstanceData, textureRect, .flags = NotListed, .updateFlags = kInstances),
        JE_FIELD(InstanceData, visible, .updateFlags = kInstances),
    };

    constexpr uint64_t kTerrainTexture = UpdateFlags_Terrain_Texture;

    constexpr FieldDesc kTerrainSurfaceLayerFields[] = {
        JE_FIELD(TerrainSurfaceLayer, pbr, .updateFlags = kTerrainTexture, .label = "PBR"),
        JE_FIELD(TerrainSurfaceLayer, colorTexture, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, normalTexture, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, roughnessTexture, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, metallicTexture, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, occlusionTexture, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, heightTexture, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, colorFactor, .flags = ColorLinear, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, normalStrength, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, roughnessFactor, .updateFlags = kTerrainTexture, .min = 0.0f, .max = 1.0f),
        JE_FIELD(TerrainSurfaceLayer, metallicFactor, .updateFlags = kTerrainTexture, .min = 0.0f, .max = 1.0f),
        JE_FIELD(TerrainSurfaceLayer, occlusionStrength, .updateFlags = kTerrainTexture, .min = 0.0f, .max = 1.0f),
        JE_FIELD(TerrainSurfaceLayer, uvScale, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, uvOffset, .updateFlags = kTerrainTexture),
        // Stochastic Tiling: every map read on a lattice of random hexagonal cells
        JE_FIELD(TerrainSurfaceLayer, stochasticTiling, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, stochasticCellSize, .updateFlags = kTerrainTexture, .uiMin = 0.25f, .uiMax = 4.0f),
        JE_FIELD(TerrainSurfaceLayer, stochasticBlendSharpness, .updateFlags = kTerrainTexture, .uiMin = 0.0f, .uiMax = 1.0f),
        JE_FIELD(TerrainSurfaceLayer, stochasticContrast, .updateFlags = kTerrainTexture, .uiMin = 0.0f, .uiMax = 1.0f),
        JE_FIELD(TerrainSurfaceLayer, stochasticRotation, .updateFlags = kTerrainTexture, .uiMin = 0.0f, .uiMax = 1.0f),
        // hex tiling: a variant define and a uniform (RenderSystem follows them; the texture update
        // as the layer's other fields)
        JE_FIELD(TerrainSurfaceLayer, hexTiling, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainSurfaceLayer, hexRotation, .updateFlags = kTerrainTexture, .uiMin = 0.0f, .uiMax = 1.0f),
    };

    constexpr uint64_t kFoliage = UpdateFlags_Terrain_Foliage;

    constexpr FieldDesc kTerrainFoliageLayerFields[] = {
        JE_FIELD(TerrainFoliageLayer, meshPath, .updateFlags = kFoliage, .asset = AssetKind::Model, .label = "Mesh"),
        JE_FIELD(TerrainFoliageLayer, densityMap, .updateFlags = kFoliage),
        JE_FIELD(TerrainFoliageLayer, density, .updateFlags = kFoliage, .min = 0.0f),
        JE_FIELD(TerrainFoliageLayer, minScale, .updateFlags = kFoliage, .min = 0.0f),
        JE_FIELD(TerrainFoliageLayer, maxScale, .updateFlags = kFoliage, .min = 0.0f),
        JE_FIELD(TerrainFoliageLayer, rotationJitter, .updateFlags = kFoliage, .min = 0.0f, .max = 1.0f),
        JE_FIELD(TerrainFoliageLayer, alignToNormal, .updateFlags = kFoliage, .min = 0.0f, .max = 1.0f),
        JE_FIELD(TerrainFoliageLayer, minSlope, .updateFlags = kFoliage, .unit = Unit::Degrees),
        JE_FIELD(TerrainFoliageLayer, maxSlope, .updateFlags = kFoliage, .unit = Unit::Degrees),
        JE_FIELD(TerrainFoliageLayer, minHeight, .updateFlags = kFoliage),
        JE_FIELD(TerrainFoliageLayer, maxHeight, .updateFlags = kFoliage),
        JE_FIELD(TerrainFoliageLayer, drawDistance, .updateFlags = kFoliage, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(TerrainFoliageLayer, minDrawDistance, .flags = OmitIfDefault, .updateFlags = kFoliage, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(TerrainFoliageLayer, seed, .updateFlags = kFoliage),
        JE_FIELD(TerrainFoliageLayer, castShadows, .updateFlags = kFoliage),
    };

    constexpr uint64_t kSubmeshAll = UpdateFlags_Mesh_Texture | UpdateFlags_Mesh_Reload | UpdateFlags_Shader_Uniforms;

    constexpr FieldDesc kSubmeshFields[] = {
        JE_FIELD(Submesh, material, .ops = &ops::MaterialStruct<Material>, .flags = ListWhole, .updateFlags = kSubmeshAll),
        JE_FIELD(Submesh, primitiveType, .enumDesc = &enums::kPrimitiveType, .updateFlags = UpdateFlags_Mesh_Reload),
        JE_FIELD(Submesh, faceCulling, .updateFlags = UpdateFlags_Mesh_Reload),
        JE_FIELD(Submesh, textureShadow, .updateFlags = UpdateFlags_Mesh_Reload),
        JE_FIELD(Submesh, textureRect),
    };

}

JE_REFLECT_STRUCT_TRAITS(SpriteFrameData, kSpriteFrameStruct);
const StructDesc kSpriteFrameStruct = JE_STRUCT_DESC(SpriteFrameData, "SpriteFrameData", kSpriteFrameFields);
JE_REFLECT_STRUCT_TRAITS(TileRectData, kTileRectStruct);
const StructDesc kTileRectStruct = JE_STRUCT_DESC(TileRectData, "TileRectData", kTileRectFields);
JE_REFLECT_STRUCT_TRAITS(TileData, kTileStruct);
const StructDesc kTileStruct = JE_STRUCT_DESC(TileData, "TileData", kTileFields);
JE_REFLECT_STRUCT_TRAITS(PolygonPoint, kPolygonPointMeshStruct);
const StructDesc kPolygonPointMeshStruct = JE_STRUCT_DESC(PolygonPoint, "PolygonPoint", kPolygonPointMeshFields);
extern const StructDesc kPolygonPointUIStruct;
const StructDesc kPolygonPointUIStruct = JE_STRUCT_DESC(PolygonPoint, "PolygonPoint", kPolygonPointUIFields);
JE_REFLECT_STRUCT_TRAITS(PointData, kPointStruct);
const StructDesc kPointStruct = JE_STRUCT_DESC(PointData, "PointData", kPointFields);
JE_REFLECT_STRUCT_TRAITS(LineData, kLineStruct);
const StructDesc kLineStruct = JE_STRUCT_DESC(LineData, "LineData", kLineFields);
JE_REFLECT_STRUCT_TRAITS(InstanceData, kInstanceStruct);
const StructDesc kInstanceStruct = JE_STRUCT_DESC(InstanceData, "InstanceData", kInstanceFields);
JE_REFLECT_STRUCT_TRAITS(TerrainSurfaceLayer, kTerrainSurfaceLayerStruct);
const StructDesc kTerrainSurfaceLayerStruct = JE_STRUCT_DESC(TerrainSurfaceLayer, "TerrainSurfaceLayer", kTerrainSurfaceLayerFields);
JE_REFLECT_STRUCT_TRAITS(TerrainFoliageLayer, kTerrainFoliageLayerStruct);
const StructDesc kTerrainFoliageLayerStruct = JE_STRUCT_DESC(TerrainFoliageLayer, "TerrainFoliageLayer", kTerrainFoliageLayerFields);
JE_REFLECT_STRUCT_TRAITS(Submesh, kSubmeshStruct);
const StructDesc kSubmeshStruct = JE_STRUCT_DESC(Submesh, "Submesh", kSubmeshFields);

namespace {

    // ---------------------------------------------------------------- Mesh

    constexpr uint64_t kMeshReload = UpdateFlags_Mesh_Reload;

    constexpr FieldDesc kMeshFields[] = {
        JE_FIELD(MeshComponent, enabled),
        JE_FIELD(MeshComponent, receiveLights, .updateFlags = kMeshReload),
        JE_FIELD(MeshComponent, receiveIBL, .updateFlags = kMeshReload, .label = "Receive IBL"),
        JE_FIELD(MeshComponent, castShadows, .updateFlags = kMeshReload),
        JE_FIELD(MeshComponent, receiveShadows, .updateFlags = kMeshReload),
        JE_FIELD(MeshComponent, staticShadowCaster),
        JE_FIELD(MeshComponent, lodEnabled, .updateFlags = kMeshReload, .label = "LOD"),
        JE_FIELD(MeshComponent, lodBias, .min = 0.0f, .label = "LOD Bias"),
        JE_FIELD(MeshComponent, allowGpuInstancing, .updateFlags = kMeshReload, .label = "Allow GPU Instancing"),
        // a mesh that may fade loads its fade shaders with the mesh (spec S54 R10)
        JE_FIELD(MeshComponent, lodFadeMode, .enumDesc = &enums::kLodFadeMode, .updateFlags = kMeshReload, .label = "Fade Mode"),
        JE_FIELD(MeshComponent, lodAnimateCrossFading, .label = "Animate Cross-fading"),
        JE_FIELD(MeshComponent, lodFadeTransitionWidth, .min = 0.0f, .max = 1.0f, .label = "Fade Transition Width"),
        JE_FIELD(MeshComponent, lodCullBelow, .min = 0.0f, .max = 1.0f, .label = "Cull Below Screen Height"),
        JE_FIELD(MeshComponent, renderInReflectionProbes, .updateFlags = UpdateFlags_Reflection_Probe_Recapture_All),
        JE_FIELD(MeshComponent, transparent, .updateFlags = kMeshReload),
        JE_FIELD(MeshComponent, autoTransparency, .updateFlags = kMeshReload),
        JE_FIELD(MeshComponent, cullingMode, .enumDesc = &enums::kCullingMode, .updateFlags = kMeshReload),
        JE_FIELD(MeshComponent, customShader, .updateFlags = UpdateFlags_Shader_Reload),
        JE_FIELD(MeshComponent, customDepthShader, .updateFlags = UpdateFlags_Shader_Reload),
        JE_FIELD(MeshComponent, shaderUniforms, .ops = &ops::ShaderUniformValues, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(MeshComponent, numSubmeshes),
        JE_FIELD(MeshComponent, submeshes, .flags = ListTemplate, .updateFlags = kSubmeshAll, .assetCount = JE_COUNT(MeshComponent, numSubmeshes)),
        // one blend shape weight each ("morphWeights[2]"), so a prefab instance overrides weights
        // one by one (the Inspector's Blend Shapes rows, animation clips T3c); read every frame
        JE_FIELD(MeshComponent, morphWeights, .flags = ListElements | ListTemplate | NoCodegen),
    };

    // A mesh's geometry regenerated from a model file, sprite, tilemap, polygon or terrain is not
    // written (Stream::isModelBackedMesh / isGeneratedMesh)
    YAML::Node encodeMesh(const void* comp, const EncodeContext& ctx) {
        const MeshComponent& mesh = *static_cast<const MeshComponent*>(comp);
        if (!ctx.registry) return Stream::encodeMeshComponent(mesh);
        const bool isModel = Stream::isModelBackedMesh(ctx.entity, ctx.registry, ctx.signature);
        const bool generatesGeometry = Stream::isGeneratedMesh(ctx.registry, ctx.signature);
        const bool isGenerated = isModel || generatesGeometry;
        return Stream::encodeMeshComponent(mesh, !isGenerated, !isModel, !isGenerated, Stream::savedSubmeshCount(mesh, ctx.registry, ctx.signature),
                                           !generatesGeometry);
    }

    void decodeMesh(const YAML::Node& node, void* comp, const DecodeContext& ctx) {
        *static_cast<MeshComponent*>(comp) = Stream::decodeMeshComponent(node, static_cast<const MeshComponent*>(ctx.old));
    }

    const ComponentCodec kMeshCodec{&encodeMesh, &decodeMesh};

    // ---------------------------------------------------------------- Model

    constexpr uint64_t kModel = UpdateFlags_Model;

    constexpr FieldDesc kModelFields[] = {
        JE_FIELD(ModelComponent, enabled),
        JE_FIELD(ModelComponent, filename, .updateFlags = kModel, .asset = AssetKind::Model, .label = "Model"),
        JE_FIELD(ModelComponent, mergeStaticMeshes, .updateFlags = kModel),
        // read by the loader on the next load or reload; the scene step that turns it off ("Use
        // Animation Player", 9.R15) removes the animation entities itself
        JE_FIELD(ModelComponent, animationEntities),
        JE_FIELD(ModelComponent, skeleton, .ops = &ops::Entity, .updateFlags = kMeshReload),
        JE_FIELD(ModelComponent, animations, .flags = ListWhole | ListElements, .elementOps = &ops::Entity),
    };

    // the GameObjects the model made, by bone and node: entity references the clipboard and
    // duplication remap ("bonesIdMapping[3]", "bonesNameMapping[Hips]")
    template<typename Map>
    void listMapping(const char* name, Map& map, std::map<std::string, PropertyData>& out) {
        static Entity defEntity = NULL_ENTITY;
        for (auto& [key, entity] : map) {
            std::string index;
            if constexpr (std::is_same_v<typename Map::key_type, std::string>) index = key;
            else index = std::to_string(key);
            out[std::string(name) + "[" + index + "]"] = {PropertyType::Entity, UpdateFlags_None, &defEntity, &entity};
        }
    }

    void modelExtraProperties(void* comp, std::map<std::string, PropertyData>& out) {
        if (!comp) return;
        ModelComponent& model = *static_cast<ModelComponent*>(comp);
        listMapping("bonesIdMapping", model.bonesIdMapping, out);
        listMapping("bonesNameMapping", model.bonesNameMapping, out);
        listMapping("meshNodesMapping", model.meshNodesMapping, out);
        listMapping("nodesIdMapping", model.nodesIdMapping, out);
    }

    // ---------------------------------------------------------------- Points and lines

    constexpr uint64_t kShaderReload = UpdateFlags_Shader_Reload;

    constexpr FieldDesc kPointsFields[] = {
        JE_FIELD(PointsComponent, enabled),
        JE_FIELD(PointsComponent, maxPoints, .min = 1.0f),
        JE_FIELD(PointsComponent, transparent),
        JE_FIELD(PointsComponent, autoTransparency),
        JE_FIELD(PointsComponent, texture, .updateFlags = UpdateFlags_Points_Texture),
        JE_FIELD(PointsComponent, customShader, .updateFlags = kShaderReload),
        JE_FIELD(PointsComponent, shaderUniforms, .ops = &ops::ShaderUniformValues, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(PointsComponent, shaderTextures, .ops = &ops::ShaderTextures, .updateFlags = kShaderReload),
        JE_FIELD(PointsComponent, shaderFeatures, .ops = &ops::ShaderFeatures, .updateFlags = kShaderReload),
        JE_FIELD(PointsComponent, shaderGlobalBindings, .ops = &ops::ShaderGlobalBindings, .flags = Hidden, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(PointsComponent, blendMode, .enumDesc = &enums::kMaterialBlendMode, .updateFlags = kShaderReload),
        JE_FIELD(PointsComponent, depthWrite, .enumDesc = &enums::kMaterialDepthWrite, .updateFlags = kShaderReload),
        JE_FIELD(PointsComponent, stencil, .flags = ListWhole, .updateFlags = kShaderReload),
        JE_FIELD(PointsComponent, colorWrite, .ops = &ops::ColorWrite, .updateFlags = kShaderReload, .min = -1.0f, .max = 15.0f),
        JE_FIELD(PointsComponent, points, .flags = ListWhole, .updateFlags = kPoints),
        JE_FIELD(PointsComponent, numFramesRect, .flags = ComponentOnly),
        JE_FIELD(PointsComponent, framesRect, .count = JE_COUNT(PointsComponent, numFramesRect)),
    };

    constexpr FieldDesc kLineRendererFields[] = {
        JE_FIELD(LineRendererComponent, enabled),
        JE_FIELD(LineRendererComponent, maxLines, .updateFlags = UpdateFlags_Lines_Reload, .min = 1.0f),
        JE_FIELD(LineRendererComponent, customShader, .updateFlags = kShaderReload),
        JE_FIELD(LineRendererComponent, shaderUniforms, .ops = &ops::ShaderUniformValues, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(LineRendererComponent, shaderTextures, .ops = &ops::ShaderTextures, .updateFlags = kShaderReload),
        JE_FIELD(LineRendererComponent, shaderFeatures, .ops = &ops::ShaderFeatures, .updateFlags = kShaderReload),
        JE_FIELD(LineRendererComponent, shaderGlobalBindings, .ops = &ops::ShaderGlobalBindings, .flags = Hidden, .updateFlags = UpdateFlags_Shader_Uniforms),
        JE_FIELD(LineRendererComponent, blendMode, .enumDesc = &enums::kMaterialBlendMode, .updateFlags = kShaderReload),
        JE_FIELD(LineRendererComponent, depthWrite, .enumDesc = &enums::kMaterialDepthWrite, .updateFlags = kShaderReload),
        JE_FIELD(LineRendererComponent, stencil, .flags = ListWhole, .updateFlags = kShaderReload),
        JE_FIELD(LineRendererComponent, colorWrite, .ops = &ops::ColorWrite, .updateFlags = kShaderReload, .min = -1.0f, .max = 15.0f),
        JE_FIELD(LineRendererComponent, lines, .flags = ListWhole, .updateFlags = kLines),
        // upstream (lines and trails): the polyline and its ribbon (S27's Curve and Gradient)
        JE_FIELD(LineRendererComponent, mode, .enumDesc = &enums::kLineRendererMode, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, positions, .ops = &ops::PushBackList<Vector3>, .flags = NoDefault | ListWhole | ListElements, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, loop, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, useWorldSpace, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, widthMultiplier, .updateFlags = kLines, .min = 0.0f),
        JE_FIELD(LineRendererComponent, widthCurve, .updateFlags = kLines, .tooltip = "The width over the line's length (0 to 1), times Width Multiplier.",
                 .drawer = "curve"),
        JE_FIELD(LineRendererComponent, widthUnit, .enumDesc = &enums::kLineWidthUnit, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, colorGradient, .updateFlags = kLines, .tooltip = "The polyline's colour over its length (0 to 1).",
                 .drawer = "gradient"),
        JE_FIELD(LineRendererComponent, numCornerVertices, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, numCapVertices, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, alignment, .enumDesc = &enums::kLineAlignment),
        JE_FIELD(LineRendererComponent, textureMode, .enumDesc = &enums::kLineTextureMode, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, textureScale, .updateFlags = kLines),
        JE_FIELD(LineRendererComponent, sortPriority),
    };

    // ---------------------------------------------------------------- Mesh instancing

    constexpr FieldDesc kInstancedMeshFields[] = {
        JE_FIELD(InstancedMeshComponent, enabled),
        JE_FIELD(InstancedMeshComponent, maxInstances, .updateFlags = kMeshReload, .min = 1.0f),
        // picks the shader variant: a reload, which the plain fade uniforms need not
        JE_FIELD(InstancedMeshComponent, distanceFade, .updateFlags = kMeshReload),
        JE_FIELD(InstancedMeshComponent, cullInstances, .updateFlags = kInstances),
        JE_FIELD(InstancedMeshComponent, fadeStart, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(InstancedMeshComponent, cullDistance, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(InstancedMeshComponent, fadeEnd, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(InstancedMeshComponent, nearFadeStart, .flags = OmitIfDefault, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(InstancedMeshComponent, nearFadeEnd, .flags = OmitIfDefault, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(InstancedMeshComponent, instancedBillboard, .updateFlags = kMeshReload, .label = "Billboard"),
        JE_FIELD(InstancedMeshComponent, instancedCylindricalBillboard, .updateFlags = kMeshReload, .label = "Cylindrical Billboard"),
        JE_FIELD(InstancedMeshComponent, instances, .flags = ListWhole, .updateFlags = kInstances),
    };

    // ---------------------------------------------------------------- Terrain

    constexpr uint64_t kTerrain = UpdateFlags_Terrain;

    // the node tree and the physics heightfield are built from the map
    void terrainHeightMapMoved(void* owner) {
        static_cast<TerrainComponent*>(owner)->heightMapLoaded = false;
    }

    constexpr FieldDesc kTerrainFields[] = {
        JE_FIELD(TerrainComponent, enabled),
        JE_FIELD(TerrainComponent, heightMap, .updateFlags = kTerrain | kTerrainTexture | kFoliage, .assetMoved = &terrainHeightMapMoved),
        JE_FIELD(TerrainComponent, autoSetRanges, .updateFlags = kTerrain),
        JE_FIELD(TerrainComponent, offset),
        JE_FIELD(TerrainComponent, terrainSize, .updateFlags = kTerrain | kFoliage, .unit = Unit::Meters, .min = 0.0f),
        JE_FIELD(TerrainComponent, maxHeight, .updateFlags = kTerrain | kFoliage, .unit = Unit::Meters),
        JE_FIELD(TerrainComponent, resolution, .updateFlags = kTerrain | kMeshReload, .min = 1.0f),
        JE_FIELD(TerrainComponent, textureBaseTiles, .min = 0.0f),
        JE_FIELD(TerrainComponent, textureDetailTiles, .min = 0.0f),
        JE_FIELD(TerrainComponent, rootGridSize, .updateFlags = kTerrain | kMeshReload, .min = 1.0f),
        JE_FIELD(TerrainComponent, levels, .updateFlags = kTerrain | kMeshReload, .min = 1.0f),
        JE_FIELD(TerrainComponent, ranges, .flags = ListWhole | ListElements | ListTemplate, .updateFlags = kTerrain),
        JE_FIELD(TerrainComponent, blendMaps, .flags = ListWhole | ListElements | ListTemplate, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainComponent, surfaceLayers, .flags = ListWhole | ListTemplate, .updateFlags = kTerrainTexture),
        JE_FIELD(TerrainComponent, foliageLayers, .flags = ListWhole | ListTemplate, .updateFlags = kFoliage),
    };

    // ---------------------------------------------------------------- Sprite and tilemap

    constexpr uint64_t kSprite = UpdateFlags_Sprite;

    constexpr FieldDesc kSpriteFields[] = {
        JE_FIELD(SpriteComponent, enabled),
        JE_FIELD(SpriteComponent, width, .flags = NoDefault, .updateFlags = kSprite, .unit = Unit::Pixels),
        JE_FIELD(SpriteComponent, height, .flags = NoDefault, .updateFlags = kSprite, .unit = Unit::Pixels),
        JE_FIELD(SpriteComponent, automaticFlipY, .updateFlags = kSprite),
        JE_FIELD(SpriteComponent, flipY, .updateFlags = kSprite),
        JE_FIELD(SpriteComponent, textureScaleFactor, .updateFlags = kSprite, .min = 0.0f),
        JE_FIELD(SpriteComponent, pivotPreset, .enumDesc = &enums::kPivotPreset, .updateFlags = kSprite, .label = "Pivot"),
        // S41: texels per world unit (0: the project's) and the rotation policy under a Pixel Perfect Camera
        JE_FIELD(SpriteComponent, pixelsPerUnit, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kSprite, .min = 0.0f, .max = 4096.0f,
                 .label = "Pixels Per Unit", .tooltip = "Texels per world unit. 0: the project's Pixels Per Unit."),
        JE_FIELD(SpriteComponent, pixelRotation, .enumDesc = &enums::kPixelRotation, .flags = OmitIfDefault | CodegenIfChanged,
                 .label = "Pixel Rotation",
                 .tooltip = "Under a Pixel Perfect Camera: the camera's rotation policy, 90 Degree Steps or Rotate On Grid."),
        // frame definitions are animation metadata: they do not change the quad
        JE_FIELD(SpriteComponent, numFramesRect),
        JE_FIELD(SpriteComponent, framesRect, .count = JE_COUNT(SpriteComponent, numFramesRect)),
    };

    constexpr FieldDesc kTilemapFields[] = {
        JE_FIELD(TilemapComponent, enabled),
        JE_FIELD(TilemapComponent, width, .flags = NoDefault, .updateFlags = kTilemap, .unit = Unit::Pixels),
        JE_FIELD(TilemapComponent, height, .flags = NoDefault, .updateFlags = kTilemap, .unit = Unit::Pixels),
        JE_FIELD(TilemapComponent, automaticFlipY, .updateFlags = kTilemap),
        JE_FIELD(TilemapComponent, flipY, .updateFlags = kTilemap),
        JE_FIELD(TilemapComponent, textureScaleFactor, .updateFlags = kTilemap, .min = 0.0f),
        // S41: texels per world unit (0: the project's)
        JE_FIELD(TilemapComponent, pixelsPerUnit, .flags = OmitIfDefault | CodegenIfChanged, .updateFlags = kTilemap, .min = 0.0f,
                 .max = 4096.0f, .label = "Pixels Per Unit", .tooltip = "Texels per world unit. 0: the project's Pixels Per Unit."),
        JE_FIELD(TilemapComponent, reserveTiles, .updateFlags = kTilemap),
        JE_FIELD(TilemapComponent, numTilesRect, .updateFlags = kTilemap),
        JE_FIELD(TilemapComponent, tilesRect, .count = JE_COUNT(TilemapComponent, numTilesRect)),
        JE_FIELD(TilemapComponent, numTiles, .updateFlags = kTilemap),
        JE_FIELD(TilemapComponent, tiles, .count = JE_COUNT(TilemapComponent, numTiles)),
    };

    // ---------------------------------------------------------------- Polygons

    constexpr FieldDesc kMeshPolygonFields[] = {
        JE_FIELD(MeshPolygonComponent, width, .flags = NotListed | Hidden),
        JE_FIELD(MeshPolygonComponent, height, .flags = NotListed | Hidden),
        JE_FIELD(MeshPolygonComponent, automaticFlipY, .updateFlags = kMeshReload),
        JE_FIELD(MeshPolygonComponent, flipY, .updateFlags = kMeshReload),
        JE_FIELD(MeshPolygonComponent, points, .flags = ListWhole, .updateFlags = kMeshReload),
    };

    constexpr FieldDesc kPolygonFields[] = {
        JE_FIELD(PolygonComponent, points, .structDesc = &kPolygonPointUIStruct, .flags = ListWhole, .updateFlags = UpdateFlags_UI_Reload),
    };

    // ---------------------------------------------------------------- Script and prefab

    // the scripts' entries (their properties are listed one by one, scriptExtraProperties)
    constexpr TypeOps kScriptEntriesOps{"std::vector<ScriptEntry>", Kind::Custom, PropertyType::Custom, sizeof(std::vector<ScriptEntry>),
        &detail::construct<std::vector<ScriptEntry>>, &detail::destroy<std::vector<ScriptEntry>>, &detail::copy<std::vector<ScriptEntry>>,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, &detail::customSchema};

    constexpr FieldDesc kScriptFields[] = {
        // an enabled change runs onEnable / onDisable in Play (Catalog::updateEntity: UpdateFlags_Script_Enabled, as upstream's Catalog)
        JE_FIELD(ScriptComponent, scripts, .ops = &kScriptEntriesOps, .flags = NoDefault | Hidden, .updateFlags = UpdateFlags_Script_Enabled,
                 .assetForm = AssetForm::ScriptEntries),
    };

    // A script property's value and its default (the script's declared one)
    PropertyData scriptProperty(ScriptProperty& prop) {
        PropertyData data{Catalog::scriptPropertyTypeToPropertyType(prop.type), UpdateFlags_None, nullptr, nullptr};
        switch (prop.type) {
            case ScriptPropertyType::Bool: data.ref = &prop.valueRef<bool>(); data.def = &prop.defaultValueRef<bool>(); break;
            case ScriptPropertyType::Int: data.ref = &prop.valueRef<int>(); data.def = &prop.defaultValueRef<int>(); break;
            case ScriptPropertyType::Float: data.ref = &prop.valueRef<float>(); data.def = &prop.defaultValueRef<float>(); break;
            case ScriptPropertyType::String: data.ref = &prop.valueRef<std::string>(); data.def = &prop.defaultValueRef<std::string>(); break;
            case ScriptPropertyType::Vector2: data.ref = &prop.valueRef<Vector2>(); data.def = &prop.defaultValueRef<Vector2>(); break;
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Color3: data.ref = &prop.valueRef<Vector3>(); data.def = &prop.defaultValueRef<Vector3>(); break;
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color4: data.ref = &prop.valueRef<Vector4>(); data.def = &prop.defaultValueRef<Vector4>(); break;
            case ScriptPropertyType::EntityReference:
                data.ref = &prop.valueRef<EntityReference>().entity;
                data.def = &prop.defaultValueRef<EntityReference>().entity;
                break;
            case ScriptPropertyType::Prefab: data.ref = &prop.valueRef<PrefabAsset>(); data.def = &prop.defaultValueRef<PrefabAsset>(); break;
            case ScriptPropertyType::Asset: data.ref = &prop.valueRef<AssetRef>(); data.def = &prop.defaultValueRef<AssetRef>(); break;
            default: break;
        }
        return data;
    }

    PropertyData sceneIdProperty(ScriptProperty& prop) {
        static uint32_t defSceneId = 0;
        return {PropertyType::UInt, UpdateFlags_None, &defSceneId, &prop.valueRef<EntityReference>().sceneId};
    }

    // "scripts[N].enabled", "scripts[N].<property>" and "scripts[N].<property>.sceneId" of an
    // entity reference: a FieldSet provider over each script's declared properties (R29)
    PropertyData scriptExtraProperty(void* comp, const std::string& path) {
        ScriptComponent& script = *static_cast<ScriptComponent*>(comp);
        if (path.compare(0, 8, "scripts[") != 0) return PropertyData();
        size_t pos = 8;
        size_t index = 0;
        if (pos >= path.size() || path[pos] < '0' || path[pos] > '9') return PropertyData();
        while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') index = index * 10 + static_cast<size_t>(path[pos++] - '0');
        if (pos + 1 >= path.size() || path[pos] != ']' || path[pos + 1] != '.' || index >= script.scripts.size()) return PropertyData();
        ScriptEntry& entry = script.scripts[index];
        const std::string field = path.substr(pos + 2);
        if (field == "enabled") return {PropertyType::Bool, UpdateFlags_Script_Enabled, nullptr, &entry.enabled};
        for (ScriptProperty& prop : entry.properties) {
            if (prop.name == field) return scriptProperty(prop);
            if (prop.type == ScriptPropertyType::EntityReference && field == prop.name + ".sceneId") return sceneIdProperty(prop);
        }
        return PropertyData();
    }

    void scriptExtraProperties(void* comp, std::map<std::string, PropertyData>& out) {
        if (!comp) return;
        ScriptComponent& script = *static_cast<ScriptComponent*>(comp);
        for (size_t i = 0; i < script.scripts.size(); i++) {
            ScriptEntry& entry = script.scripts[i];
            const std::string prefix = "scripts[" + std::to_string(i) + "].";
            out[prefix + "enabled"] = {PropertyType::Bool, UpdateFlags_Script_Enabled, nullptr, &entry.enabled};
            for (ScriptProperty& prop : entry.properties) {
                out[prefix + prop.name] = scriptProperty(prop);
                if (prop.type == ScriptPropertyType::EntityReference) out[prefix + prop.name + ".sceneId"] = sceneIdProperty(prop);
            }
        }
    }

    constexpr FieldDesc kPrefabFields[] = {
        JE_FIELD(PrefabComponent, name, .drawer = "label"),
        JE_FIELD(PrefabComponent, path, .asset = AssetKind::Prefab, .drawer = "label", .assetForm = AssetForm::PrefabRoot),
        JE_FIELD(PrefabComponent, guid, .label = "GUID", .drawer = "label"),
    };

    // ---------------------------------------------------------------- codecs

    const ComponentCodec kModelCodec = JE_STREAM_CODEC(ModelComponent, encodeModelComponent, decodeModelComponent);
    const ComponentCodec kPointsCodec = JE_STREAM_CODEC(PointsComponent, encodePointsComponent, decodePointsComponent);
    const ComponentCodec kLineRendererCodec = JE_STREAM_CODEC(LineRendererComponent, encodeLineRendererComponent, decodeLineRendererComponent);
    const ComponentCodec kInstancedMeshCodec = JE_STREAM_CODEC(InstancedMeshComponent, encodeInstancedMeshComponent, decodeInstancedMeshComponent);
    const ComponentCodec kTerrainCodec = JE_STREAM_CODEC(TerrainComponent, encodeTerrainComponent, decodeTerrainComponent);
    const ComponentCodec kSpriteCodec = JE_STREAM_CODEC(SpriteComponent, encodeSpriteComponent, decodeSpriteComponent);
    const ComponentCodec kTilemapCodec = JE_STREAM_CODEC(TilemapComponent, encodeTilemapComponent, decodeTilemapComponent);
    const ComponentCodec kMeshPolygonCodec = JE_STREAM_CODEC(MeshPolygonComponent, encodeMeshPolygonComponent, decodeMeshPolygonComponent);
    const ComponentCodec kPolygonCodec = JE_STREAM_CODEC(PolygonComponent, encodePolygonComponent, decodePolygonComponent);
    const ComponentCodec kScriptCodec = JE_STREAM_CODEC(ScriptComponent, encodeScriptComponent, decodeScriptComponent);
    const ComponentCodec kPrefabCodec = JE_STREAM_CODEC(PrefabComponent, encodePrefabComponent, decodePrefabComponent);

    // the exported code holds no prefab component: the instances are built by their prefab's code
    std::string noCodegen(EntityRegistry*, Entity, const CodegenContext&) {
        return std::string();
    }

    constexpr const char* kPolygonExtraKeys[] = {"points"};

    // the keys its codec reads
    constexpr const char* kMeshKnownKeys[] = {"mirroredWinding", "aabb", "allowGpuInstancing", "autoTransparency", "bonesMatrix", "buffer", "castShadows", "cullingMode", "customDepthShader", "customShader", "eBuffers", "enabled", "indices", "lodAnimateCrossFading", "lodBias", "lodCullBelow", "lodEnabled", "lodFadeMode", "lodFadeTransitionWidth", "morphTargetCount", "morphWeights", "normAdjustJoint", "normAdjustWeight", "receiveIBL", "receiveLights", "receiveShadows", "renderInReflectionProbes", "shaderUniforms", "shadowsBillboard", "staticShadowCaster", "submeshes", "transparent", "verticesAABB", "windingOrder", "worldAABB"};

}

const ComponentDesc kMeshDesc{
    .type = ComponentType::MeshComponent, .cppName = "MeshComponent", .key = "mesh", .displayName = "Mesh Renderer", .icon = ICON_FA_SHAPES,
    .body = JE_STRUCT_DESC(MeshComponent, "MeshComponent", kMeshFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(MeshComponent),
    .codec = &kMeshCodec,
    .codegen = JE_FACTORY_CODEGEN_PATH(createMeshComponent),
    .extraKeys = kMeshKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kModelKnownKeys[] = {"animationEntities", "animations", "bonesIdMapping", "bonesNameMapping", "enabled", "filename", "mergeStaticMeshes", "meshNodesMapping", "nodesIdMapping", "skeleton"};

}

const ComponentDesc kModelDesc{
    .type = ComponentType::ModelComponent, .cppName = "ModelComponent", .key = "model", .displayName = "Model", .icon = ICON_FA_CUBES,
    .body = JE_STRUCT_DESC(ModelComponent, "ModelComponent", kModelFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(ModelComponent),
    .codec = &kModelCodec,
    .codegen = JE_FACTORY_CODEGEN(createModelComponent),
    .extraKeys = kModelKnownKeys,
    .extraProperties = &modelExtraProperties,
    .extraUpdateFlags = UpdateFlags_None,
};

namespace {

    // the keys its codec reads
    constexpr const char* kPointsKnownKeys[] = {"autoTransparency", "blendMode", "colorWrite", "customShader", "depthWrite", "enabled", "framesRect", "maxPoints", "points", "shaderFeatures", "shaderGlobalBindings", "shaderTextures", "shaderUniforms", "stencil", "texture", "transparent"};

}

const ComponentDesc kPointsDesc{
    .type = ComponentType::PointsComponent, .cppName = "PointsComponent", .key = "points", .displayName = "Point Renderer", .icon = ICON_FA_BRAILLE,
    .body = JE_STRUCT_DESC(PointsComponent, "PointsComponent", kPointsFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(PointsComponent),
    .codec = &kPointsCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN_PATH(createPointsComponent),
    .extraKeys = kPointsKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kLineRendererKnownKeys[] = {"alignment", "blendMode", "colorGradient", "colorWrite", "customShader", "depthWrite", "enabled", "lines", "loop",
        "maxLines", "mode", "numCapVertices", "numCornerVertices", "positions", "shaderFeatures", "shaderGlobalBindings", "shaderTextures", "shaderUniforms",
        "sortPriority", "stencil", "textureMode", "textureScale", "useWorldSpace", "widthCurve", "widthMultiplier", "widthUnit"};

}

namespace {

    // every point of a polyline as its own property ("positions[2]": Vector3), as the Inspector
    // and MCP name them (origin/main's catalog listed them so)
    void lineExtraProperties(void* comp, std::map<std::string, PropertyData>& out) {
        if (!comp) return;
        LineRendererComponent& lines = *static_cast<LineRendererComponent*>(comp);
        for (size_t i = 0; i < lines.positions.size(); i++) {
            out["positions[" + std::to_string(i) + "]"] = PropertyData{PropertyType::Vector3, UpdateFlags_Lines, nullptr, &lines.positions[i]};
        }
    }

}

const ComponentDesc kLineRendererDesc{
    .type = ComponentType::LineRendererComponent, .cppName = "LineRendererComponent", .key = "linerenderer", .displayName = "Line Renderer",
    .icon = ICON_FA_GRIP_LINES,
    .body = JE_STRUCT_DESC(LineRendererComponent, "LineRendererComponent", kLineRendererFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(LineRendererComponent),
    .codec = &kLineRendererCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createLineRendererComponent),
    .extraKeys = kLineRendererKnownKeys,
    .extraProperties = &lineExtraProperties,
};

namespace {

    // the keys its codec reads
    constexpr const char* kInstancedMeshKnownKeys[] = {"cullDistance", "cullInstances", "distanceFade", "enabled", "fadeEnd", "fadeStart", "instancedBillboard", "instancedCylindricalBillboard", "instances", "maxInstances", "nearFadeEnd", "nearFadeStart"};

}

const ComponentDesc kInstancedMeshDesc{
    .type = ComponentType::InstancedMeshComponent, .cppName = "InstancedMeshComponent", .key = "instancedmesh", .displayName = "Mesh Instancing",
    .icon = ICON_FA_CLONE,
    .body = JE_STRUCT_DESC(InstancedMeshComponent, "InstancedMeshComponent", kInstancedMeshFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(InstancedMeshComponent),
    .codec = &kInstancedMeshCodec,
    .codegen = JE_FACTORY_CODEGEN(createInstancedMeshComponent),
    .extraKeys = kInstancedMeshKnownKeys,
};

namespace {

    // the keys its codec reads
    // terrains saved before blend maps and surface layers
    constexpr const char* kTerrainLegacyTextureKeys[] = {"blendMap", "textureLayers[]", "textureDetailRed", "textureDetailGreen", "textureDetailBlue"};

    constexpr const char* kTerrainKnownKeys[] = {"autoSetRanges", "blendMap", "blendMaps", "enabled", "foliageLayers", "heightMap", "levels", "maxHeight", "offset", "ranges", "resolution", "rootGridSize", "surfaceLayers", "terrainSize", "textureBaseTiles", "textureDetailTiles", "textureLayers"};

}

const ComponentDesc kTerrainDesc{
    .type = ComponentType::TerrainComponent, .cppName = "TerrainComponent", .key = "terrain", .displayName = "Terrain", .icon = ICON_FA_MOUNTAIN,
    .body = JE_STRUCT_DESC(TerrainComponent, "TerrainComponent", kTerrainFields),
    .hasEnabled = true, .structural = true, JE_COMPONENT_ACCESS(TerrainComponent),
    .codec = &kTerrainCodec,
    .codegen = JE_FACTORY_CODEGEN_PATH(createTerrainComponent),
    .extraKeys = kTerrainKnownKeys,
    .legacyTextureKeys = kTerrainLegacyTextureKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kSpriteKnownKeys[] = {"automaticFlipY", "enabled", "flipY", "framesRect", "height", "pivotPreset", "pixelRotation", "pixelsPerUnit",
                                                "textureScaleFactor", "width"};

}

const ComponentDesc kSpriteDesc{
    .type = ComponentType::SpriteComponent, .cppName = "SpriteComponent", .key = "sprite", .displayName = "Sprite Renderer", .icon = ICON_FA_IMAGE,
    .body = JE_STRUCT_DESC(SpriteComponent, "SpriteComponent", kSpriteFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(SpriteComponent),
    .codec = &kSpriteCodec,
    .codegen = JE_FACTORY_CODEGEN(createSpriteComponent),
    .extraKeys = kSpriteKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kTilemapKnownKeys[] = {"automaticFlipY", "enabled", "flipY", "height", "pixelsPerUnit", "reserveTiles", "textureScaleFactor", "tiles",
                                                 "tilesRect", "width"};

}

const ComponentDesc kTilemapDesc{
    .type = ComponentType::TilemapComponent, .cppName = "TilemapComponent", .key = "tilemap", .displayName = "Tilemap", .icon = ICON_FA_BORDER_ALL,
    .body = JE_STRUCT_DESC(TilemapComponent, "TilemapComponent", kTilemapFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(TilemapComponent),
    .codec = &kTilemapCodec,
    .codegen = JE_FACTORY_CODEGEN(createTilemapComponent),
    .extraKeys = kTilemapKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kMeshPolygonKnownKeys[] = {"automaticFlipY", "flipY", "height", "points", "width"};

}

const ComponentDesc kMeshPolygonDesc{
    .type = ComponentType::MeshPolygonComponent, .cppName = "MeshPolygonComponent", .key = "meshpolygon", .displayName = "Polygon Mesh",
    .icon = ICON_FA_DRAW_POLYGON,
    .body = JE_STRUCT_DESC(MeshPolygonComponent, "MeshPolygonComponent", kMeshPolygonFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(MeshPolygonComponent),
    .codec = &kMeshPolygonCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createMeshPolygonComponent),
    .extraKeys = kMeshPolygonKnownKeys,
};

const ComponentDesc kPolygonDesc{
    .type = ComponentType::PolygonComponent, .cppName = "PolygonComponent", .key = "polygon", .displayName = "Polygon", .icon = ICON_FA_DRAW_POLYGON,
    .body = JE_STRUCT_DESC(PolygonComponent, "PolygonComponent", kPolygonFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(PolygonComponent),
    .codec = &kPolygonCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createPolygonComponent),
    .extraKeys = kPolygonExtraKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kScriptKnownKeys[] = {"scripts"};

}

const ComponentDesc kScriptDesc{
    .type = ComponentType::ScriptComponent, .cppName = "ScriptComponent", .key = "script", .displayName = "Script", .icon = ICON_FA_CODE,
    .body = JE_STRUCT_DESC(ScriptComponent, "ScriptComponent", kScriptFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ScriptComponent),
    .codec = &kScriptCodec,
    .codegen = JE_FACTORY_CODEGEN(createScriptComponent),
    .extraKeys = kScriptKnownKeys,
    .extraProperty = &scriptExtraProperty,
    .extraProperties = &scriptExtraProperties,
};

namespace {

    // the keys its codec reads
    constexpr const char* kPrefabKnownKeys[] = {"guid", "name", "path"};

}

const ComponentDesc kPrefabDesc{
    .type = ComponentType::PrefabComponent, .cppName = "PrefabComponent", .key = "prefab", .displayName = "Prefab", .icon = ICON_FA_CUBE,
    .body = JE_STRUCT_DESC(PrefabComponent, "PrefabComponent", kPrefabFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(PrefabComponent),
    .codec = &kPrefabCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = &noCodegen,
    .extraKeys = kPrefabKnownKeys,
};

}
