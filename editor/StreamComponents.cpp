// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The component codecs of their own (docs/specs/S08-reflection.md R14): the components whose
// scene YAML the generic reflection codec does not write (geometry buffers, embedded data, layouts
// read from the entity's other components) keep a hand-written encode and decode, called through
// their ComponentDesc's codec (editor/reflect/components). The rest of the scene file is Stream.cpp.

#include "Stream.h"
#include "reflect/Reflect.h"
#include "log/Logger.h"
#include "StreamSchema.h"
#include "Catalog.h"
#include "util/ProjectUtils.h"
#include "util/YamlNumbers.h"
#include "lattice/LatticeTypes.h"
#include "Out.h"
#include "particles/ParticleEffectCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;

YAML::Node editor::Stream::encodeMeshComponent(const MeshComponent& mesh, bool encodeBuffers, bool embedTextureData, bool encodeBones) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(mesh.enabled);

    if (encodeBuffers){
        node["buffer"] = encodeInterleavedBuffer(mesh.buffer);
        node["indices"] = encodeIndexBuffer(mesh.indices);

        // Encode external buffers
        YAML::Node eBuffersNode;
        for (unsigned int i = 0; i < mesh.numExternalBuffers; i++) {
            eBuffersNode.push_back(encodeExternalBuffer(mesh.eBuffers[i]));
        }
        node["eBuffers"] = eBuffersNode;
    }

    // Encode submeshes
    YAML::Node submeshesNode;
    for(unsigned int i = 0; i < mesh.numSubmeshes; i++) {
        submeshesNode.push_back(encodeSubmesh(mesh.submeshes[i], embedTextureData));
    }
    node["submeshes"] = submeshesNode;

    // Model-backed meshes regenerate these matrices from their skeleton on load.
    if (encodeBones) {
        // Trailing identity matrices are what a decoded mesh starts with, so they are left
        // out: every mesh made in the editor (a box, a sphere) holds MAX_BONES of them,
        // ~8 KB of YAML each. An array grown past MAX_BONES keeps its size (export reads it).
        size_t count = mesh.bonesMatrix.size();
        if (count <= MAX_BONES) {
            const Matrix4 identity;
            while (count > 0 && mesh.bonesMatrix[count - 1] == identity) {
                count--;
            }
        }
        YAML::Node bonesNode(YAML::NodeType::Sequence);
        for (size_t i = 0; i < count; i++)
            bonesNode.push_back(encodeMatrix4(mesh.bonesMatrix[i]));
        node["bonesMatrix"] = bonesNode;
    }

    node["normAdjustJoint"] = YamlNumbers::text(mesh.normAdjustJoint);
    node["normAdjustWeight"] = YamlNumbers::text(mesh.normAdjustWeight);

    // Encode morph weights array
    YAML::Node morphWeightsNode;
    morphWeightsNode.SetStyle(YAML::EmitterStyle::Flow);
    for(int i = 0; i < MAX_MORPHTARGETS; i++) {
        morphWeightsNode[i] = YamlNumbers::text(mesh.morphWeights[i]);
    }
    node["morphWeights"] = morphWeightsNode;

    // Encode AABBs
    node["aabb"] = encodeAABB(mesh.aabb);
    node["verticesAABB"] = encodeAABB(mesh.verticesAABB);
    node["worldAABB"] = encodeAABB(mesh.worldAABB);

    node["receiveLights"] = YamlNumbers::text(mesh.receiveLights);
    node["receiveIBL"] = YamlNumbers::text(mesh.receiveIBL);
    node["castShadows"] = YamlNumbers::text(mesh.castShadows);
    node["receiveShadows"] = YamlNumbers::text(mesh.receiveShadows);
    node["lodEnabled"] = YamlNumbers::text(mesh.lodEnabled);
    node["lodBias"] = YamlNumbers::text(mesh.lodBias);
    node["renderInReflectionProbes"] = YamlNumbers::text(mesh.renderInReflectionProbes);
    node["shadowsBillboard"] = YamlNumbers::text(mesh.shadowsBillboard);
    if (mesh.staticShadowCaster)
        node["staticShadowCaster"] = YamlNumbers::text(mesh.staticShadowCaster);
    node["transparent"] = YamlNumbers::text(mesh.transparent);
    node["autoTransparency"] = YamlNumbers::text(mesh.autoTransparency);

    node["cullingMode"] = cullingModeToString(mesh.cullingMode);
    node["windingOrder"] = windingOrderToString(mesh.windingOrder);

    if (!mesh.customShader.empty())
        encodeShaderRef(node, "customShader", mesh.customShader);
    if (!mesh.customDepthShader.empty())
        encodeShaderRef(node, "customDepthShader", mesh.customDepthShader);
    if (!mesh.shaderUniforms.empty())
        node["shaderUniforms"] = encodeShaderUniforms(mesh.shaderUniforms);

    return node;
}

MeshComponent editor::Stream::decodeMeshComponent(const YAML::Node& node, const MeshComponent* oldMesh) {
    MeshComponent mesh;

    // Use old values as defaults if provided
    if (oldMesh) {
        mesh = *oldMesh;
    }

    // Decode buffers using generic methods
    if (node["buffer"]) {
        decodeInterleavedBuffer(mesh.buffer, node["buffer"]);
    }

    if (node["indices"]) {
        decodeIndexBuffer(mesh.indices, node["indices"]);
    }

    // Decode external buffers
    if (node["eBuffers"]) {
        auto eBuffersNode = node["eBuffers"];
        for (unsigned int i = 0; i < eBuffersNode.size() && i < mesh.eBuffers.size(); i++) {
            decodeExternalBuffer(mesh.eBuffers[i], eBuffersNode[i]);
        }
        mesh.numExternalBuffers = eBuffersNode.size();
    }

    // Decode submeshes
    if (node["submeshes"]) {
        auto submeshesNode = node["submeshes"];
        for(unsigned int i = 0; i < submeshesNode.size() && i < mesh.submeshes.size(); i++) {
            mesh.submeshes[i] = decodeSubmesh(submeshesNode[i], &mesh.submeshes[i]);
        }
        mesh.numSubmeshes = submeshesNode.size();
    }

    // Decode bones matrix
    if (node["bonesMatrix"]) {
        auto bonesNode = node["bonesMatrix"];
        mesh.bonesMatrix = HybridArray<Matrix4, MAX_BONES>();
        for (size_t i = 0; i < bonesNode.size() &&
                mesh.bonesMatrix.validIndex(static_cast<int>(i)); i++)
            mesh.bonesMatrix[i] = decodeMatrix4(bonesNode[i]);
        // an empty list is a trimmed one (all identity), which still counts as written
        mesh.needUpdateBones = true;
    }

    if (node["normAdjustJoint"]) mesh.normAdjustJoint = YamlNumbers::as<float>(node["normAdjustJoint"]);
    if (node["normAdjustWeight"]) mesh.normAdjustWeight = YamlNumbers::as<float>(node["normAdjustWeight"]);

    // Decode morph weights
    if (node["morphWeights"]) {
        auto morphWeightsNode = node["morphWeights"];
        for(int i = 0; i < MAX_MORPHTARGETS; i++) {
            mesh.morphWeights[i] = YamlNumbers::as<float>(morphWeightsNode[i]);
        }
    }

    if (node["aabb"]) mesh.aabb = decodeAABB(node["aabb"]);
    if (node["verticesAABB"]) mesh.verticesAABB = decodeAABB(node["verticesAABB"]);
    if (node["worldAABB"]) mesh.worldAABB = decodeAABB(node["worldAABB"]);

    if (node["receiveLights"]) mesh.receiveLights = YamlNumbers::as<bool>(node["receiveLights"]);
    if (node["receiveIBL"]) mesh.receiveIBL = YamlNumbers::as<bool>(node["receiveIBL"]);
    if (node["castShadows"]) mesh.castShadows = YamlNumbers::as<bool>(node["castShadows"]);
    if (node["receiveShadows"]) mesh.receiveShadows = YamlNumbers::as<bool>(node["receiveShadows"]);
    if (node["lodEnabled"]) mesh.lodEnabled = YamlNumbers::as<bool>(node["lodEnabled"]);
    mesh.lodBias = decodeFinite(node["lodBias"], mesh.lodBias);
    if (node["renderInReflectionProbes"]) mesh.renderInReflectionProbes = YamlNumbers::as<bool>(node["renderInReflectionProbes"]);
    if (node["shadowsBillboard"]) mesh.shadowsBillboard = YamlNumbers::as<bool>(node["shadowsBillboard"]);
    // omitted when false
    mesh.staticShadowCaster = node["staticShadowCaster"] ? YamlNumbers::as<bool>(node["staticShadowCaster"]) : false;
    if (node["transparent"]) mesh.transparent = YamlNumbers::as<bool>(node["transparent"]);
    if (node["autoTransparency"]) mesh.autoTransparency = YamlNumbers::as<bool>(node["autoTransparency"]);

    if (node["cullingMode"]) mesh.cullingMode = decodeEnum(node, "cullingMode", StreamSchema::cullingMode());
    if (node["windingOrder"]) mesh.windingOrder = decodeEnum(node, "windingOrder", StreamSchema::windingOrder());

    if (node["customShader"]) mesh.customShader = decodeShaderRef(node, "customShader");
    if (node["customDepthShader"]) mesh.customDepthShader = decodeShaderRef(node, "customDepthShader");
    mesh.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    mesh.needUpdateShaderUniforms = true;

    if (node["enabled"]) mesh.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return mesh;
}

YAML::Node editor::Stream::encodeUIComponent(const UIComponent& ui, bool encodeBuffers, bool embedTextureData) {
    YAML::Node node;
    node["enabled"] = ui.enabled;
    if (encodeBuffers){
        node["buffer"] = encodeBuffer(ui.buffer);
        node["indices"] = encodeBuffer(ui.indices);
        node["minBufferCount"] = ui.minBufferCount;
        node["minIndicesCount"] = ui.minIndicesCount;
    }

    node["primitiveType"] = primitiveTypeToString(ui.primitiveType);
    node["vertexCount"] = ui.vertexCount;

    node["aabb"] = encodeAABB(ui.aabb);
    node["worldAABB"] = encodeAABB(ui.worldAABB);

    node["texture"] = encodeTexture(ui.texture, embedTextureData);
    node["color"] = encodeVector4(ui.color);
    // FunctionSubscribe fields are not serializable

    node["automaticFlipY"] = ui.automaticFlipY;
    node["flipY"] = ui.flipY;

    node["pointerMoved"] = ui.pointerMoved;
    node["focused"] = ui.focused;

    if (!ui.customShader.empty())
        encodeShaderRef(node, "customShader", ui.customShader);
    if (!ui.shaderUniforms.empty())
        node["shaderUniforms"] = encodeShaderUniforms(ui.shaderUniforms);
    encodeComponentShaderLibrary(node, ui.shaderTextures, ui.shaderFeatures, &ui.blendMode, &ui.depthWrite, embedTextureData);
    encodeStencilAndColorWrite(node, ui.stencil, ui.colorWrite);

    return node;
}

UIComponent editor::Stream::decodeUIComponent(const YAML::Node& node, const UIComponent* oldUI) {
    UIComponent ui;

    // Use old values as defaults if provided
    if (oldUI) {
        ui = *oldUI;
    }

    if (node["buffer"]) decodeBuffer(ui.buffer, node["buffer"]);
    if (node["indices"]) decodeBuffer(ui.indices, node["indices"]);
    if (node["minBufferCount"]) ui.minBufferCount = YamlNumbers::as<unsigned int>(node["minBufferCount"]);
    if (node["minIndicesCount"]) ui.minIndicesCount = YamlNumbers::as<unsigned int>(node["minIndicesCount"]);

    if (node["primitiveType"]) ui.primitiveType = decodeEnum(node, "primitiveType", StreamSchema::primitiveType());
    if (node["vertexCount"]) ui.vertexCount = YamlNumbers::as<unsigned int>(node["vertexCount"]);

    if (node["aabb"]) ui.aabb = decodeAABB(node["aabb"]);
    if (node["worldAABB"]) ui.worldAABB = decodeAABB(node["worldAABB"]);

    if (node["texture"]) ui.texture = decodeTexture(node["texture"]);
    if (node["color"]) ui.color = decodeVector4(node["color"]);

    if (node["automaticFlipY"]) ui.automaticFlipY = YamlNumbers::as<bool>(node["automaticFlipY"]);
    if (node["flipY"]) ui.flipY = YamlNumbers::as<bool>(node["flipY"]);

    if (node["pointerMoved"]) ui.pointerMoved = YamlNumbers::as<bool>(node["pointerMoved"]);
    if (node["focused"]) ui.focused = YamlNumbers::as<bool>(node["focused"]);

    if (node["customShader"]) ui.customShader = decodeShaderRef(node, "customShader");
    ui.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    ui.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, ui.shaderTextures, ui.shaderFeatures, &ui.blendMode, &ui.depthWrite);
    decodeStencilAndColorWrite(node, ui.stencil, ui.colorWrite);

    if (node["enabled"]) ui.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return ui;
}

YAML::Node editor::Stream::encodeSpriteComponent(const SpriteComponent& sprite) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(sprite.enabled);
    node["width"] = YamlNumbers::integerText(sprite.width);
    node["height"] = YamlNumbers::integerText(sprite.height);
    node["automaticFlipY"] = YamlNumbers::text(sprite.automaticFlipY);
    node["flipY"] = YamlNumbers::text(sprite.flipY);
    node["textureScaleFactor"] = YamlNumbers::text(sprite.textureScaleFactor);
    node["pivotPreset"] = pivotPresetToString(sprite.pivotPreset);

    YAML::Node framesNode;
    for (unsigned int i = 0; i < sprite.numFramesRect; i++) {
        framesNode.push_back(encodeSpriteFrameData(sprite.framesRect[i]));
    }
    if (framesNode.size() > 0) {
        node["framesRect"] = framesNode;
    }

    return node;
}

SpriteComponent editor::Stream::decodeSpriteComponent(const YAML::Node& node, const SpriteComponent* oldSprite) {
    SpriteComponent sprite;

    // Use old values as defaults if provided
    if (oldSprite) {
        sprite = *oldSprite;
    }

    // Pending frames are runtime requests, not part of the serialized sprite.
    // In particular, Stop must not apply a play-time frame over the restored UVs.
    sprite.pendingFrameRect = Rect();
    sprite.needUpdateFrameRect = false;

    if (node["width"]) sprite.width = YamlNumbers::as<unsigned int>(node["width"]);
    if (node["height"]) sprite.height = YamlNumbers::as<unsigned int>(node["height"]);
    if (node["automaticFlipY"]) sprite.automaticFlipY = YamlNumbers::as<bool>(node["automaticFlipY"]);
    if (node["flipY"]) sprite.flipY = YamlNumbers::as<bool>(node["flipY"]);
    if (node["textureScaleFactor"]) sprite.textureScaleFactor = YamlNumbers::as<float>(node["textureScaleFactor"]);
    if (node["pivotPreset"]) sprite.pivotPreset = decodeEnum(node, "pivotPreset", StreamSchema::pivotPreset());

    if (node["framesRect"]) {
        const YAML::Node& framesNode = node["framesRect"];

        if (framesNode.IsSequence()) {
            sprite.numFramesRect = 0;
            for (std::size_t i = 0; i < framesNode.size() && i < sprite.framesRect.size(); i++) {
                if (!framesNode[i] || framesNode[i].IsNull()) {
                    continue;
                }

                sprite.framesRect[sprite.numFramesRect] = decodeSpriteFrameData(framesNode[i]);
                sprite.numFramesRect++;
            }
        }
    }

    if (node["enabled"]) sprite.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return sprite;
}

YAML::Node editor::Stream::encodeTilemapComponent(const TilemapComponent& tilemap) {
    YAML::Node node;
    node["enabled"] = tilemap.enabled;
    node["width"] = tilemap.width;
    node["height"] = tilemap.height;
    node["automaticFlipY"] = tilemap.automaticFlipY;
    node["flipY"] = tilemap.flipY;
    node["textureScaleFactor"] = tilemap.textureScaleFactor;
    node["reserveTiles"] = tilemap.reserveTiles;

    YAML::Node tilesRectNode;
    for (unsigned int i = 0; i < tilemap.numTilesRect; i++) {
        tilesRectNode.push_back(encodeTileRectData(tilemap.tilesRect[i]));
    }
    if (tilesRectNode.size() > 0) {
        node["tilesRect"] = tilesRectNode;
    }

    YAML::Node tilesNode;
    for (unsigned int i = 0; i < tilemap.numTiles; i++) {
        tilesNode.push_back(encodeTileData(tilemap.tiles[i]));
    }
    if (tilesNode.size() > 0) {
        node["tiles"] = tilesNode;
    }

    return node;
}

TilemapComponent editor::Stream::decodeTilemapComponent(const YAML::Node& node, const TilemapComponent* oldTilemap) {
    TilemapComponent tilemap;

    if (oldTilemap) {
        tilemap = *oldTilemap;
    }

    if (node["width"]) tilemap.width = YamlNumbers::as<unsigned int>(node["width"]);
    if (node["height"]) tilemap.height = YamlNumbers::as<unsigned int>(node["height"]);
    if (node["automaticFlipY"]) tilemap.automaticFlipY = YamlNumbers::as<bool>(node["automaticFlipY"]);
    if (node["flipY"]) tilemap.flipY = YamlNumbers::as<bool>(node["flipY"]);
    if (node["textureScaleFactor"]) tilemap.textureScaleFactor = YamlNumbers::as<float>(node["textureScaleFactor"]);
    if (node["reserveTiles"]) tilemap.reserveTiles = YamlNumbers::as<unsigned int>(node["reserveTiles"]);

    if (node["tilesRect"]) {
        const YAML::Node& tilesRectNode = node["tilesRect"];
        if (tilesRectNode.IsSequence()) {
            tilemap.numTilesRect = 0;
            for (std::size_t i = 0; i < tilesRectNode.size() && i < tilemap.tilesRect.size(); i++) {
                if (!tilesRectNode[i] || tilesRectNode[i].IsNull()) {
                    continue;
                }
                tilemap.tilesRect[tilemap.numTilesRect] = decodeTileRectData(tilesRectNode[i]);
                tilemap.numTilesRect++;
            }
        }
    }

    if (node["tiles"]) {
        const YAML::Node& tilesNode = node["tiles"];
        if (tilesNode.IsSequence()) {
            tilemap.numTiles = 0;
            for (std::size_t i = 0; i < tilesNode.size() && i < tilemap.tiles.size(); i++) {
                if (!tilesNode[i] || tilesNode[i].IsNull()) {
                    continue;
                }
                tilemap.tiles[tilemap.numTiles] = decodeTileData(tilesNode[i]);
                tilemap.numTiles++;
            }
        }
    }

    if (node["enabled"]) tilemap.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return tilemap;
}

YAML::Node editor::Stream::encodeTerrainSurfaceLayer(const TerrainSurfaceLayer& layer) {
    YAML::Node node;
    node["pbr"] = layer.pbr;
    // An unassigned map writes nothing, so a plain color layer stays a two-line entry
    auto texture = [&](const char* name, const Texture& value) {
        if (!value.empty()) node[name] = encodeTexture(value);
    };
    texture("colorTexture", layer.colorTexture);
    texture("normalTexture", layer.normalTexture);
    texture("roughnessTexture", layer.roughnessTexture);
    texture("metallicTexture", layer.metallicTexture);
    texture("occlusionTexture", layer.occlusionTexture);
    texture("heightTexture", layer.heightTexture);
    node["colorFactor"] = encodeVector4(layer.colorFactor);
    node["normalStrength"] = layer.normalStrength;
    node["roughnessFactor"] = layer.roughnessFactor;
    node["metallicFactor"] = layer.metallicFactor;
    node["occlusionStrength"] = layer.occlusionStrength;
    node["uvScale"] = encodeVector2(layer.uvScale);
    node["uvOffset"] = encodeVector2(layer.uvOffset);
    // Stochastic Tiling writes only what differs from the defaults, so a layer without it
    // saves as it always did
    static const TerrainSurfaceLayer defaults;
    if (layer.stochasticTiling != defaults.stochasticTiling) node["stochasticTiling"] = layer.stochasticTiling;
    if (layer.stochasticCellSize != defaults.stochasticCellSize) node["stochasticCellSize"] = layer.stochasticCellSize;
    if (layer.stochasticBlendSharpness != defaults.stochasticBlendSharpness) node["stochasticBlendSharpness"] = layer.stochasticBlendSharpness;
    if (layer.stochasticContrast != defaults.stochasticContrast) node["stochasticContrast"] = layer.stochasticContrast;
    if (layer.stochasticRotation != defaults.stochasticRotation) node["stochasticRotation"] = layer.stochasticRotation;
    // hex tiling: written only when on, or with a rotation of its own
    if (layer.hexTiling) node["hexTiling"] = true;
    if (layer.hexRotation != TerrainSurfaceLayer().hexRotation) node["hexRotation"] = YamlNumbers::text(finiteOr(layer.hexRotation, 1.0f));
    return node;
}

TerrainSurfaceLayer editor::Stream::decodeTerrainSurfaceLayer(const YAML::Node& node) {
    TerrainSurfaceLayer layer;
    if (node["pbr"]) layer.pbr = YamlNumbers::as<bool>(node["pbr"]);
    if (node["colorTexture"]) layer.colorTexture = decodeTexture(node["colorTexture"]);
    if (node["normalTexture"]) layer.normalTexture = decodeTexture(node["normalTexture"]);
    if (node["roughnessTexture"]) layer.roughnessTexture = decodeTexture(node["roughnessTexture"]);
    if (node["metallicTexture"]) layer.metallicTexture = decodeTexture(node["metallicTexture"]);
    if (node["occlusionTexture"]) layer.occlusionTexture = decodeTexture(node["occlusionTexture"]);
    if (node["heightTexture"]) layer.heightTexture = decodeTexture(node["heightTexture"]);
    if (node["colorFactor"]) layer.colorFactor = decodeVector4(node["colorFactor"]);
    if (node["uvScale"]) layer.uvScale = decodeVector2(node["uvScale"]);
    if (node["uvOffset"]) layer.uvOffset = decodeVector2(node["uvOffset"]);
    layer.normalStrength = decodeFinite(node["normalStrength"], layer.normalStrength);
    layer.roughnessFactor = decodeFinite(node["roughnessFactor"], layer.roughnessFactor);
    layer.metallicFactor = decodeFinite(node["metallicFactor"], layer.metallicFactor);
    layer.occlusionStrength = decodeFinite(node["occlusionStrength"], layer.occlusionStrength);
    if (node["stochasticTiling"]) layer.stochasticTiling = YamlNumbers::as<bool>(node["stochasticTiling"]);
    layer.stochasticCellSize = decodeFinite(node["stochasticCellSize"], layer.stochasticCellSize);
    layer.stochasticBlendSharpness = decodeFinite(node["stochasticBlendSharpness"], layer.stochasticBlendSharpness);
    layer.stochasticContrast = decodeFinite(node["stochasticContrast"], layer.stochasticContrast);
    layer.stochasticRotation = decodeFinite(node["stochasticRotation"], layer.stochasticRotation);
    if (node["hexTiling"]) layer.hexTiling = YamlNumbers::as<bool>(node["hexTiling"]);
    layer.hexRotation = std::clamp(decodeFinite(node["hexRotation"], layer.hexRotation), 0.0f, 1.0f);
    return layer;
}

YAML::Node editor::Stream::encodeTerrainFoliageLayer(const TerrainFoliageLayer& layer) {
    YAML::Node node;
    encodeAssetRef(node, "meshPath", layer.meshPath);
    node["densityMap"] = encodeTexture(layer.densityMap);
    node["density"] = layer.density;
    node["minScale"] = layer.minScale;
    node["maxScale"] = layer.maxScale;
    node["rotationJitter"] = layer.rotationJitter;
    node["alignToNormal"] = layer.alignToNormal;
    node["minSlope"] = layer.minSlope;
    node["maxSlope"] = layer.maxSlope;
    node["minHeight"] = layer.minHeight;
    node["maxHeight"] = layer.maxHeight;
    node["drawDistance"] = layer.drawDistance;
    node["seed"] = layer.seed;
    node["castShadows"] = layer.castShadows;
    return node;
}

TerrainFoliageLayer editor::Stream::decodeTerrainFoliageLayer(const YAML::Node& node) {
    TerrainFoliageLayer layer;
    if (node["meshPath"]) layer.meshPath = decodeAssetRef(node, "meshPath");
    if (node["densityMap"]) layer.densityMap = decodeTexture(node["densityMap"]);
    if (node["seed"]) layer.seed = YamlNumbers::as<unsigned int>(node["seed"]);
    if (node["castShadows"]) layer.castShadows = YamlNumbers::as<bool>(node["castShadows"]);
    layer.density = decodeFinite(node["density"], layer.density);
    layer.minScale = decodeFinite(node["minScale"], layer.minScale);
    layer.maxScale = decodeFinite(node["maxScale"], layer.maxScale);
    layer.rotationJitter = decodeFinite(node["rotationJitter"], layer.rotationJitter);
    layer.alignToNormal = decodeFinite(node["alignToNormal"], layer.alignToNormal);
    layer.minSlope = decodeFinite(node["minSlope"], layer.minSlope);
    layer.maxSlope = decodeFinite(node["maxSlope"], layer.maxSlope);
    layer.minHeight = decodeFinite(node["minHeight"], layer.minHeight);
    layer.maxHeight = decodeFinite(node["maxHeight"], layer.maxHeight);
    layer.drawDistance = decodeFinite(node["drawDistance"], layer.drawDistance);
    return layer;
}

YAML::Node editor::Stream::encodeTerrainComponent(const TerrainComponent& terrain) {
    YAML::Node node;
    node["enabled"] = terrain.enabled;
    node["heightMap"] = encodeTexture(terrain.heightMap);
    YAML::Node blendMapsNode;
    for (const Texture& blendMap : terrain.blendMaps) {
        blendMapsNode.push_back(encodeTexture(blendMap));
    }
    node["blendMaps"] = blendMapsNode;

    YAML::Node layersNode;
    for (const TerrainSurfaceLayer& layer : terrain.surfaceLayers) {
        layersNode.push_back(encodeTerrainSurfaceLayer(layer));
    }
    node["surfaceLayers"] = layersNode;
    node["autoSetRanges"] = terrain.autoSetRanges;
    node["offset"] = encodeVector2(terrain.offset);
    node["terrainSize"] = terrain.terrainSize;
    node["maxHeight"] = terrain.maxHeight;
    node["resolution"] = terrain.resolution;
    node["textureBaseTiles"] = terrain.textureBaseTiles;
    node["textureDetailTiles"] = terrain.textureDetailTiles;
    node["rootGridSize"] = terrain.rootGridSize;
    node["levels"] = terrain.levels;

    if (!terrain.ranges.empty()) {
        YAML::Node rangesNode;
        for (float range : terrain.ranges) {
            rangesNode.push_back(range);
        }
        node["ranges"] = rangesNode;
    }

    if (!terrain.foliageLayers.empty()) {
        YAML::Node layersNode;
        for (const TerrainFoliageLayer& layer : terrain.foliageLayers) {
            layersNode.push_back(encodeTerrainFoliageLayer(layer));
        }
        node["foliageLayers"] = layersNode;
    }

    return node;
}

TerrainComponent editor::Stream::decodeTerrainComponent(const YAML::Node& node, const TerrainComponent* oldTerrain) {
    TerrainComponent terrain;

    if (oldTerrain) {
        terrain = *oldTerrain;
    }

    if (node["heightMap"]) terrain.heightMap = decodeTexture(node["heightMap"]);
    // Cleared like the other lists below: decoding over a loaded terrain (a stop, a prefab
    // reload) must replace what it has, not append to it.
    terrain.blendMaps.clear();
    if (node["blendMaps"] && node["blendMaps"].IsSequence()) {
        for (std::size_t i = 0; i < node["blendMaps"].size(); i++) {
            terrain.blendMaps.push_back(decodeTexture(node["blendMaps"][i]));
        }
    }

    terrain.surfaceLayers.clear();
    if (node["surfaceLayers"] && node["surfaceLayers"].IsSequence()) {
        for (std::size_t i = 0; i < node["surfaceLayers"].size(); i++) {
            terrain.surfaceLayers.push_back(decodeTerrainSurfaceLayer(node["surfaceLayers"][i]));
        }
    }

    // Backward compatibility: scenes saved before the terrain took lists named the single
    // blend map and the first three layers, and before a layer was more than its color.
    // Remove this block with support for them.
    if (node["blendMap"] && terrain.blendMaps.empty()) {
        terrain.blendMaps.push_back(decodeTexture(node["blendMap"]));
    }
    if (terrain.surfaceLayers.empty() && node["textureLayers"] && node["textureLayers"].IsSequence()) {
        for (std::size_t i = 0; i < node["textureLayers"].size(); i++) {
            TerrainSurfaceLayer layer;
            layer.colorTexture = decodeTexture(node["textureLayers"][i]);
            terrain.surfaceLayers.push_back(layer);
        }
    }
    if (terrain.surfaceLayers.empty()) {
        const char* legacyKeys[] = {"textureDetailRed", "textureDetailGreen", "textureDetailBlue"};
        int lastKey = -1;
        for (int i = 0; i < 3; i++) {
            if (node[legacyKeys[i]]) lastKey = i;
        }
        // A missing key still holds its slot, or the layers after it would shift down
        for (int i = 0; i <= lastKey; i++) {
            TerrainSurfaceLayer layer;
            if (node[legacyKeys[i]]) layer.colorTexture = decodeTexture(node[legacyKeys[i]]);
            terrain.surfaceLayers.push_back(layer);
        }
    }

    if (terrain.blendMaps.size() > MAX_TERRAIN_BLENDMAPS) {
        terrain.blendMaps.resize(MAX_TERRAIN_BLENDMAPS);
    }
    if (terrain.surfaceLayers.size() > MAX_TERRAIN_LAYERS) {
        terrain.surfaceLayers.resize(MAX_TERRAIN_LAYERS);
    }
    if (node["autoSetRanges"]) terrain.autoSetRanges = YamlNumbers::as<bool>(node["autoSetRanges"]);
    if (node["offset"]) terrain.offset = decodeVector2(node["offset"]);
    if (node["terrainSize"]) terrain.terrainSize = YamlNumbers::as<float>(node["terrainSize"]);
    if (node["maxHeight"]) terrain.maxHeight = YamlNumbers::as<float>(node["maxHeight"]);
    if (node["resolution"]) terrain.resolution = YamlNumbers::as<float>(node["resolution"]);
    if (node["textureBaseTiles"]) terrain.textureBaseTiles = YamlNumbers::as<float>(node["textureBaseTiles"]);
    if (node["textureDetailTiles"]) terrain.textureDetailTiles = YamlNumbers::as<float>(node["textureDetailTiles"]);
    if (node["rootGridSize"]) terrain.rootGridSize = YamlNumbers::as<int>(node["rootGridSize"]);
    if (node["levels"]) terrain.levels = YamlNumbers::as<int>(node["levels"]);

    // The encoder omits empty ranges, so applying a full component must clear old values.
    terrain.ranges.clear();
    if (node["ranges"] && node["ranges"].IsSequence()) {
        for (std::size_t i = 0; i < node["ranges"].size(); i++) {
            if (!node["ranges"][i] || node["ranges"][i].IsNull()) {
                continue;
            }
            terrain.ranges.push_back(YamlNumbers::as<float>(node["ranges"][i]));
        }
    }

    // Cleared unconditionally: an absent key means the terrain has no layers, and reloading a
    // scene over a loaded terrain must not resurrect layers that were removed.
    terrain.foliageLayers.clear();
    if (node["foliageLayers"] && node["foliageLayers"].IsSequence()) {
        for (std::size_t i = 0; i < node["foliageLayers"].size(); i++) {
            if (!node["foliageLayers"][i] || node["foliageLayers"][i].IsNull()) {
                continue;
            }
            terrain.foliageLayers.push_back(decodeTerrainFoliageLayer(node["foliageLayers"][i]));
        }
    }

    terrain.needUpdateTerrain = true;
    terrain.needUpdateFoliage = true;
    terrain.needUpdateTexture = true;
    for (int v = 0; v < MAX_TERRAIN_VIEWS; v++){
        terrain.views[v].needUpdateNodesBuffer = false;
    }
    terrain.heightMapLoaded = false;
    terrain.nodes.clear();
    terrain.numNodes = 0;

    if (node["enabled"]) terrain.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return terrain;
}

YAML::Node editor::Stream::encodeModelComponent(const ModelComponent& model) {
    YAML::Node node;
    node["enabled"] = model.enabled;

    // Every field is written, the empty ones too: decoding onto an existing Model (a Prefab
    // override, a pasted value) keeps the fields a node leaves out, so an emptied Model would
    // come back with the file and nodes it had.
    encodeAssetRef(node, "filename", model.filename);
    node["mergeStaticMeshes"] = model.mergeStaticMeshes;

    node["skeleton"] = static_cast<uint32_t>(model.skeleton);

    YAML::Node animsNode(YAML::NodeType::Sequence);
    for (const auto& anim : model.animations) {
        animsNode.push_back(static_cast<uint32_t>(anim));
    }
    node["animations"] = animsNode;

    YAML::Node bonesIdNode(YAML::NodeType::Sequence);
    for (const auto& bone : model.bonesIdMapping) {
        YAML::Node boneNode;
        boneNode["id"] = bone.first;
        boneNode["entity"] = static_cast<uint32_t>(bone.second);
        bonesIdNode.push_back(boneNode);
    }
    node["bonesIdMapping"] = bonesIdNode;

    YAML::Node bonesNameNode(YAML::NodeType::Sequence);
    for (const auto& bone : model.bonesNameMapping) {
        YAML::Node boneNode;
        boneNode["name"] = bone.first;
        boneNode["entity"] = static_cast<uint32_t>(bone.second);
        bonesNameNode.push_back(boneNode);
    }
    node["bonesNameMapping"] = bonesNameNode;

    YAML::Node meshNodesNode(YAML::NodeType::Sequence);
    for (const auto& meshNode : model.meshNodesMapping) {
        YAML::Node n;
        n["node"] = meshNode.first;
        n["entity"] = static_cast<uint32_t>(meshNode.second);
        meshNodesNode.push_back(n);
    }
    node["meshNodesMapping"] = meshNodesNode;

    YAML::Node nodesNode(YAML::NodeType::Sequence);
    for (const auto& mappedNode : model.nodesIdMapping) {
        YAML::Node n;
        n["node"] = mappedNode.first;
        n["entity"] = static_cast<uint32_t>(mappedNode.second);
        nodesNode.push_back(n);
    }
    node["nodesIdMapping"] = nodesNode;

    return node;
}

ModelComponent editor::Stream::decodeModelComponent(const YAML::Node& node, const ModelComponent* oldModel) {
    ModelComponent model;

    if (oldModel) {
        model = *oldModel;
    }

    // Also check for the old modelPath for exact backward compatibility with old save files
    if (node["filename"]) model.filename = decodeAssetRef(node, "filename");
    if (node["mergeStaticMeshes"]) model.mergeStaticMeshes = YamlNumbers::as<bool>(node["mergeStaticMeshes"]);
    if (node["skeleton"]) model.skeleton = static_cast<Entity>(YamlNumbers::as<uint32_t>(node["skeleton"]));

    if (node["animations"]) {
        model.animations.clear();
        for (const auto& animNode : node["animations"]) {
            model.animations.push_back(static_cast<Entity>(YamlNumbers::as<uint32_t>(animNode)));
        }
    }

    if (node["bonesIdMapping"]) {
        model.bonesIdMapping.clear();
        for (const auto& boneNode : node["bonesIdMapping"]) {
            int id = YamlNumbers::as<int>(boneNode["id"]);
            Entity boneEntity = static_cast<Entity>(YamlNumbers::as<uint32_t>(boneNode["entity"]));
            model.bonesIdMapping[id] = boneEntity;
        }
    }

    if (node["bonesNameMapping"]) {
        model.bonesNameMapping.clear();
        for (const auto& boneNode : node["bonesNameMapping"]) {
            std::string name = boneNode["name"].as<std::string>();
            Entity boneEntity = static_cast<Entity>(YamlNumbers::as<uint32_t>(boneNode["entity"]));
            model.bonesNameMapping[name] = boneEntity;
        }
    }

    if (node["meshNodesMapping"]) {
        model.meshNodesMapping.clear();
        for (const auto& meshNode : node["meshNodesMapping"]) {
            int nodeIdx = YamlNumbers::as<int>(meshNode["node"]);
            Entity nodeEntity = static_cast<Entity>(YamlNumbers::as<uint32_t>(meshNode["entity"]));
            model.meshNodesMapping[nodeIdx] = nodeEntity;
        }
    }

    model.nodesIdMapping.clear();
    if (node["nodesIdMapping"]) {
        for (const auto& mappedNode : node["nodesIdMapping"]) {
            int nodeIdx = YamlNumbers::as<int>(mappedNode["node"]);
            Entity nodeEntity = static_cast<Entity>(YamlNumbers::as<uint32_t>(mappedNode["entity"]));
            model.nodesIdMapping[nodeIdx] = nodeEntity;
        }
    }

    if (!oldModel || model.filename != oldModel->filename ||
            model.bonesIdMapping != oldModel->bonesIdMapping ||
            model.meshNodesMapping != oldModel->meshNodesMapping ||
            model.nodesIdMapping != oldModel->nodesIdMapping) {
        model.skinBindings.clear();
        model.loadedFilename.clear();
        model.needUpdateModel = true;
    }

    if (node["enabled"]) model.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return model;
}

YAML::Node editor::Stream::encodeScriptComponent(const ScriptComponent& script) {
    YAML::Node node;

    if (!script.scripts.empty()) {
        YAML::Node scriptsNode;
        for (const auto& scriptEntry : script.scripts) {
            YAML::Node scriptNode;

            scriptNode["type"] = scriptTypeToString(scriptEntry.type);

            if (!scriptEntry.path.empty())
                encodeAssetRef(scriptNode, "path", scriptEntry.path);

            if (!scriptEntry.headerPath.empty())
                encodeAssetRef(scriptNode, "headerPath", scriptEntry.headerPath);

            if (!scriptEntry.className.empty())
                scriptNode["className"] = scriptEntry.className;

            scriptNode["enabled"] = scriptEntry.enabled;

            if (!scriptEntry.properties.empty()) {
                YAML::Node propsNode;
                for (const auto& prop : scriptEntry.properties) {
                    propsNode.push_back(encodeScriptProperty(prop));
                }
                scriptNode["properties"] = propsNode;
            }

            scriptsNode.push_back(scriptNode);
        }
        node["scripts"] = scriptsNode;
    }

    return node;
}

ScriptComponent editor::Stream::decodeScriptComponent(const YAML::Node& node, const ScriptComponent* oldScript) {
    ScriptComponent script;

    // Use old values as defaults if provided
    if (oldScript) {
        script = *oldScript;
    }

    script.scripts.clear();

    if (!node["scripts"] || !node["scripts"].IsSequence())
        return script;

    for (const auto& scriptNode : node["scripts"]) {
        ScriptEntry entry;

        if (scriptNode["type"]) {
            entry.type = decodeEnum(scriptNode, "type", StreamSchema::scriptType());
        }

        if (scriptNode["path"])
            entry.path = decodeAssetRef(scriptNode, "path");

        if (scriptNode["headerPath"])
            entry.headerPath = decodeAssetRef(scriptNode, "headerPath");

        if (scriptNode["className"])
            entry.className = scriptNode["className"].as<std::string>();

        if (scriptNode["enabled"])
            entry.enabled = YamlNumbers::as<bool>(scriptNode["enabled"]);

        if (scriptNode["properties"] && scriptNode["properties"].IsSequence()) {
            for (const auto& propNode : scriptNode["properties"]) {
                entry.properties.push_back(decodeScriptProperty(propNode));
            }
        }

        script.scripts.push_back(std::move(entry));
    }

    return script;
}

YAML::Node editor::Stream::encodeSkyboxComponent(const SkyboxComponent& sky) {
    YAML::Node node;

    const bool useDefaultTexture = (sky.texture.getId() == "editor:resources:default_sky");
    node["useDefaultTexture"] = useDefaultTexture;
    node["texture"] = encodeTexture(sky.texture);
    node["color"] = encodeVector4(sky.color);
    node["rotation"] = sky.rotation;
    node["visible"] = sky.visible;

    if (!sky.customShader.empty())
        encodeShaderRef(node, "customShader", sky.customShader);
    if (!sky.shaderUniforms.empty())
        node["shaderUniforms"] = encodeShaderUniforms(sky.shaderUniforms);
    encodeComponentShaderLibrary(node, sky.shaderTextures, sky.shaderFeatures, nullptr, nullptr, true);

    return node;
}

SkyboxComponent editor::Stream::decodeSkyboxComponent(const YAML::Node& node, const SkyboxComponent* oldSky) {
    SkyboxComponent sky;

    if (oldSky) {
        sky = *oldSky;
    }

    const bool useDefaultTexture = node["useDefaultTexture"] ? YamlNumbers::as<bool>(node["useDefaultTexture"]) : false;
    if (useDefaultTexture) {
        ProjectUtils::setDefaultSkyTexture(sky.texture);
    }

    if (node["texture"]) {
        if (useDefaultTexture) {
            const YAML::Node& texNode = node["texture"];
            if (texNode["minFilter"]) sky.texture.setMinFilter(decodeEnum(texNode, "minFilter", StreamSchema::textureFilter()));
            if (texNode["magFilter"]) sky.texture.setMagFilter(decodeEnum(texNode, "magFilter", StreamSchema::textureFilter()));
            if (texNode["wrapU"]) sky.texture.setWrapU(decodeEnum(texNode, "wrapU", StreamSchema::textureWrap()));
            if (texNode["wrapV"]) sky.texture.setWrapV(decodeEnum(texNode, "wrapV", StreamSchema::textureWrap()));
            if (texNode["releaseDataAfterLoad"]) sky.texture.setReleaseDataAfterLoad(YamlNumbers::as<bool>(texNode["releaseDataAfterLoad"]));
        } else {
            sky.texture = decodeTexture(node["texture"]);
        }
    }
    if (node["color"]) sky.color = decodeVector4(node["color"]);
    if (node["rotation"]) sky.rotation = YamlNumbers::as<float>(node["rotation"]);
    if (node["visible"]) sky.visible = YamlNumbers::as<bool>(node["visible"]);

    if (node["customShader"]) sky.customShader = decodeShaderRef(node, "customShader");
    sky.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    sky.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, sky.shaderTextures, sky.shaderFeatures, nullptr, nullptr);

    return sky;
}

// ==============================
// Body/Joint type string converters
// ==============================

std::string editor::Stream::bodyTypeToString(BodyType type) {
    return StreamSchema::bodyType().toString(type);
}

BodyType editor::Stream::stringToBodyType(const std::string& str) {
    return StreamSchema::bodyType().fromString(str);
}

std::string editor::Stream::rigidbodyMotionQualityToString(RigidbodyMotionQuality motionQuality) {
    return StreamSchema::rigidbodyMotionQuality().toString(motionQuality);
}

RigidbodyMotionQuality editor::Stream::stringToRigidbodyMotionQuality(const std::string& str) {
    return StreamSchema::rigidbodyMotionQuality().fromString(str);
}

std::string editor::Stream::collider2DTypeToString(Collider2DType type) {
    return StreamSchema::collider2DType().toString(type);
}

Collider2DType editor::Stream::stringToCollider2DType(const std::string& str) {
    return StreamSchema::collider2DType().fromString(str);
}

std::string editor::Stream::collider3DTypeToString(Collider3DType type) {
    return StreamSchema::collider3DType().toString(type);
}

Collider3DType editor::Stream::stringToCollider3DType(const std::string& str) {
    return StreamSchema::collider3DType().fromString(str);
}

std::string editor::Stream::joint2DTypeToString(Joint2DType type) {
    return StreamSchema::joint2DType().toString(type);
}

Joint2DType editor::Stream::stringToJoint2DType(const std::string& str) {
    return StreamSchema::joint2DType().fromString(str);
}

std::string editor::Stream::joint3DTypeToString(Joint3DType type) {
    return StreamSchema::joint3DType().toString(type);
}

Joint3DType editor::Stream::stringToJoint3DType(const std::string& str) {
    return StreamSchema::joint3DType().fromString(str);
}

// ==============================
// Rigidbody2DComponent encode/decode
// ==============================

YAML::Node editor::Stream::encodeRigidbody2DComponent(const Rigidbody2DComponent& body) {
    YAML::Node node;
    node["enabled"] = body.enabled;

    node["type"] = bodyTypeToString(body.type);
    node["numColliders"] = static_cast<unsigned int>(body.numColliders);

    YAML::Node shapesNode;
    for (size_t i = 0; i < body.numColliders; i++) {
        YAML::Node shapeNode;
        shapeNode["type"] = collider2DTypeToString(body.colliders[i].type);
        shapeNode["pointA"] = encodeVector2(body.colliders[i].pointA);
        shapeNode["pointB"] = encodeVector2(body.colliders[i].pointB);
        shapeNode["radius"] = body.colliders[i].radius;
        shapeNode["loop"] = body.colliders[i].loop;

        shapeNode["density"] = body.colliders[i].density;
        shapeNode["friction"] = body.colliders[i].friction;
        shapeNode["restitution"] = body.colliders[i].restitution;
        shapeNode["sensor"] = body.colliders[i].sensor;
        shapeNode["enableHitEvents"] = body.colliders[i].enableHitEvents;
        shapeNode["contactEvents"] = body.colliders[i].contactEvents;
        shapeNode["preSolveEvents"] = body.colliders[i].preSolveEvents;
        shapeNode["sensorEvents"] = body.colliders[i].sensorEvents;
        shapeNode["categoryBits"] = body.colliders[i].categoryBits;
        shapeNode["maskBits"] = body.colliders[i].maskBits;
        shapeNode["groupIndex"] = body.colliders[i].groupIndex;

        YAML::Node verticesNode;
        for (size_t j = 0; j < body.colliders[i].numVertices; j++) {
            verticesNode.push_back(encodeVector2(body.colliders[i].vertices[j]));
        }
        shapeNode["vertices"] = verticesNode;

        shapesNode.push_back(shapeNode);
    }
    node["colliders"] = shapesNode;

    return node;
}

Rigidbody2DComponent editor::Stream::decodeRigidbody2DComponent(const YAML::Node& node, const Rigidbody2DComponent* oldBody) {
    Rigidbody2DComponent body;

    if (oldBody) {
        body = *oldBody;
    }

    if (node["type"]) body.type = decodeEnum(node, "type", StreamSchema::bodyType());
    if (node["numColliders"]) body.numColliders = YamlNumbers::as<unsigned int>(node["numColliders"]);

    if (node["colliders"]) {
        size_t count = node["colliders"].size();
        for (size_t i = 0; i < count; i++) {
            if (node["colliders"][i]["type"]) {
                body.colliders[i].type = decodeEnum(node["colliders"][i], "type", StreamSchema::collider2DType());
            }
            if (node["colliders"][i]["pointA"]) {
                body.colliders[i].pointA = decodeVector2(node["colliders"][i]["pointA"]);
            }
            if (node["colliders"][i]["pointB"]) {
                body.colliders[i].pointB = decodeVector2(node["colliders"][i]["pointB"]);
            }
            if (node["colliders"][i]["radius"]) body.colliders[i].radius = YamlNumbers::as<float>(node["colliders"][i]["radius"]);
            if (node["colliders"][i]["loop"]) body.colliders[i].loop = YamlNumbers::as<bool>(node["colliders"][i]["loop"]);

            if (node["colliders"][i]["density"]) body.colliders[i].density = YamlNumbers::as<float>(node["colliders"][i]["density"]);
            if (node["colliders"][i]["friction"]) body.colliders[i].friction = YamlNumbers::as<float>(node["colliders"][i]["friction"]);
            if (node["colliders"][i]["restitution"]) body.colliders[i].restitution = YamlNumbers::as<float>(node["colliders"][i]["restitution"]);
            if (node["colliders"][i]["sensor"]) body.colliders[i].sensor = YamlNumbers::as<bool>(node["colliders"][i]["sensor"]);
            if (node["colliders"][i]["enableHitEvents"]) body.colliders[i].enableHitEvents = YamlNumbers::as<bool>(node["colliders"][i]["enableHitEvents"]);
            if (node["colliders"][i]["contactEvents"]) body.colliders[i].contactEvents = YamlNumbers::as<bool>(node["colliders"][i]["contactEvents"]);
            if (node["colliders"][i]["preSolveEvents"]) body.colliders[i].preSolveEvents = YamlNumbers::as<bool>(node["colliders"][i]["preSolveEvents"]);
            if (node["colliders"][i]["sensorEvents"]) body.colliders[i].sensorEvents = YamlNumbers::as<bool>(node["colliders"][i]["sensorEvents"]);
            if (node["colliders"][i]["categoryBits"]) body.colliders[i].categoryBits = YamlNumbers::as<uint16_t>(node["colliders"][i]["categoryBits"]);
            if (node["colliders"][i]["maskBits"]) body.colliders[i].maskBits = YamlNumbers::as<uint16_t>(node["colliders"][i]["maskBits"]);
            if (node["colliders"][i]["groupIndex"]) body.colliders[i].groupIndex = YamlNumbers::as<int16_t>(node["colliders"][i]["groupIndex"]);

            if (node["colliders"][i]["vertices"]) {
                size_t vcount = node["colliders"][i]["vertices"].size();
                body.colliders[i].numVertices = (uint8_t)vcount;
                for (size_t j = 0; j < vcount; j++) {
                    body.colliders[i].vertices[j] = decodeVector2(node["colliders"][i]["vertices"][j]);
                }
            }

            body.colliders[i].shape = b2_nullShapeId;
            body.colliders[i].chain = b2_nullChainId;
        }
    }

    if (oldBody && b2Body_IsValid(oldBody->body)) {
        body.needReloadBody = true;
        body.needUpdateColliders = true;
    }

    if (node["enabled"]) body.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return body;
}

// ==============================
// RigidbodyComponent encode/decode
// ==============================

YAML::Node editor::Stream::encodeRigidbodyComponent(const RigidbodyComponent& body) {
    YAML::Node node;
    node["enabled"] = body.enabled;

    node["type"] = bodyTypeToString(body.type);
    node["motionQuality"] = rigidbodyMotionQualityToString(body.motionQuality);
    node["sensor"] = body.sensor;
    node["gravityFactor"] = body.gravityFactor;
    node["friction"] = body.friction;
    node["restitution"] = body.restitution;
    node["buoyancy"] = body.buoyancy;
    node["waterDrag"] = body.waterDrag;
    node["waterAngularDrag"] = body.waterAngularDrag;

    // Six booleans instead of the bit mask, so the file stays readable.
    YAML::Node dofsNode;
    auto dof = [&](const char* name, uint8_t bit) {
        dofsNode[name] = (body.allowedDOFs & bit) != 0;
    };
    dof("translationX", RigidbodyAllowedDOF_TranslationX);
    dof("translationY", RigidbodyAllowedDOF_TranslationY);
    dof("translationZ", RigidbodyAllowedDOF_TranslationZ);
    dof("rotationX", RigidbodyAllowedDOF_RotationX);
    dof("rotationY", RigidbodyAllowedDOF_RotationY);
    dof("rotationZ", RigidbodyAllowedDOF_RotationZ);
    node["allowedDOFs"] = dofsNode;

    node["numColliders"] = static_cast<unsigned int>(body.numColliders);

    YAML::Node shapesNode;
    for (size_t i = 0; i < body.numColliders; i++) {
        YAML::Node shapeNode;
        shapeNode["type"] = collider3DTypeToString(body.colliders[i].type);
        shapeNode["position"] = encodeVector3(body.colliders[i].position);
        shapeNode["rotation"] = encodeQuaternion(body.colliders[i].rotation);
        shapeNode["width"] = body.colliders[i].width;
        shapeNode["height"] = body.colliders[i].height;
        shapeNode["depth"] = body.colliders[i].depth;
        shapeNode["radius"] = body.colliders[i].radius;
        shapeNode["halfHeight"] = body.colliders[i].halfHeight;
        shapeNode["topRadius"] = body.colliders[i].topRadius;
        shapeNode["bottomRadius"] = body.colliders[i].bottomRadius;
        shapeNode["density"] = body.colliders[i].density;

        switch (body.colliders[i].source) {
            case Collider3DSource::RAW_VERTICES: shapeNode["source"] = "raw_vertices"; break;
            case Collider3DSource::RAW_MESH: shapeNode["source"] = "raw_mesh"; break;
            case Collider3DSource::ENTITY_MESH: shapeNode["source"] = "entity_mesh"; break;
            case Collider3DSource::ENTITY_HEIGHTFIELD: shapeNode["source"] = "entity_heightfield"; break;
            default: shapeNode["source"] = "none"; break;
        }

        shapeNode["sourceEntity"] = body.colliders[i].sourceEntity;
        shapeNode["samplesSize"] = body.colliders[i].samplesSize;

        YAML::Node verticesNode;
        for (size_t j = 0; j < body.colliders[i].numVertices; j++) {
            verticesNode.push_back(encodeVector3(body.colliders[i].vertices[j]));
        }
        shapeNode["vertices"] = verticesNode;

        YAML::Node indicesNode;
        for (size_t j = 0; j < body.colliders[i].numIndices; j++) {
            indicesNode.push_back(body.colliders[i].indices[j]);
        }
        shapeNode["indices"] = indicesNode;

        shapesNode.push_back(shapeNode);
    }
    node["colliders"] = shapesNode;

    return node;
}

RigidbodyComponent editor::Stream::decodeRigidbodyComponent(const YAML::Node& node, const RigidbodyComponent* oldBody, const Vector3& bodyScale) {
    RigidbodyComponent body;

    if (oldBody) {
        body = *oldBody;
    }

    if (node["type"]) body.type = decodeEnum(node, "type", StreamSchema::bodyType());
    if (node["motionQuality"]) body.motionQuality = decodeEnum(node, "motionQuality", StreamSchema::rigidbodyMotionQuality());
    if (node["sensor"]) body.sensor = YamlNumbers::as<bool>(node["sensor"]);
    if (node["gravityFactor"]) body.gravityFactor = YamlNumbers::as<float>(node["gravityFactor"]);
    if (node["friction"]) body.friction = YamlNumbers::as<float>(node["friction"]);
    if (node["restitution"]) body.restitution = YamlNumbers::as<float>(node["restitution"]);
    if (node["buoyancy"]) body.buoyancy = YamlNumbers::as<float>(node["buoyancy"]);
    if (node["waterDrag"]) body.waterDrag = YamlNumbers::as<float>(node["waterDrag"]);
    if (node["waterAngularDrag"]) body.waterAngularDrag = YamlNumbers::as<float>(node["waterAngularDrag"]);

    if (node["allowedDOFs"]) {
        const YAML::Node& dofsNode = node["allowedDOFs"];
        uint8_t dofs = 0;
        // A present map is authoritative, so a missing key means that axis is locked.
        auto dof = [&](const char* name, uint8_t bit) {
            if (dofsNode[name] && YamlNumbers::as<bool>(dofsNode[name])) dofs |= bit;
        };
        dof("translationX", RigidbodyAllowedDOF_TranslationX);
        dof("translationY", RigidbodyAllowedDOF_TranslationY);
        dof("translationZ", RigidbodyAllowedDOF_TranslationZ);
        dof("rotationX", RigidbodyAllowedDOF_RotationX);
        dof("rotationY", RigidbodyAllowedDOF_RotationY);
        dof("rotationZ", RigidbodyAllowedDOF_RotationZ);
        // None crashes Jolt on body creation, and a static body is how you freeze
        // everything, so an all-false map falls back to All.
        body.allowedDOFs = dofs == 0 ? static_cast<uint8_t>(RigidbodyAllowedDOF_All) : dofs;
    }

    if (node["numColliders"]) body.numColliders = YamlNumbers::as<unsigned int>(node["numColliders"]);

    if (node["colliders"]) {
        size_t count = node["colliders"].size();
        for (size_t i = 0; i < count; i++) {
            if (node["colliders"][i]["type"]) {
                body.colliders[i].type = decodeEnum(node["colliders"][i], "type", StreamSchema::collider3DType());
            }
            if (node["colliders"][i]["position"]) {
                body.colliders[i].position = decodeVector3(node["colliders"][i]["position"]);

                const float physicsOffset = (body.colliders[i].position * bodyScale).length();
                if (body.numColliders == 1 && physicsOffset > MAX_SINGLE_SHAPE_PHYSICS_OFFSET) {
                    Log::warn("Rigidbody shape position after transform scale is too large (%.2f). Physics may be unstable.", physicsOffset);
                }
            }
            if (node["colliders"][i]["rotation"]) {
                body.colliders[i].rotation = decodeQuaternion(node["colliders"][i]["rotation"]);
            }
            if (node["colliders"][i]["width"]) body.colliders[i].width = YamlNumbers::as<float>(node["colliders"][i]["width"]);
            if (node["colliders"][i]["height"]) body.colliders[i].height = YamlNumbers::as<float>(node["colliders"][i]["height"]);
            if (node["colliders"][i]["depth"]) body.colliders[i].depth = YamlNumbers::as<float>(node["colliders"][i]["depth"]);
            if (node["colliders"][i]["radius"]) body.colliders[i].radius = YamlNumbers::as<float>(node["colliders"][i]["radius"]);
            if (node["colliders"][i]["halfHeight"]) body.colliders[i].halfHeight = YamlNumbers::as<float>(node["colliders"][i]["halfHeight"]);
            if (node["colliders"][i]["topRadius"]) body.colliders[i].topRadius = YamlNumbers::as<float>(node["colliders"][i]["topRadius"]);
            if (node["colliders"][i]["bottomRadius"]) body.colliders[i].bottomRadius = YamlNumbers::as<float>(node["colliders"][i]["bottomRadius"]);
            if (node["colliders"][i]["density"]) body.colliders[i].density = YamlNumbers::as<float>(node["colliders"][i]["density"]);

            if (node["colliders"][i]["source"]) {
                std::string source = node["colliders"][i]["source"].as<std::string>();
                if (source == "raw_vertices") body.colliders[i].source = Collider3DSource::RAW_VERTICES;
                else if (source == "raw_mesh") body.colliders[i].source = Collider3DSource::RAW_MESH;
                else if (source == "entity_mesh") body.colliders[i].source = Collider3DSource::ENTITY_MESH;
                else if (source == "entity_heightfield") body.colliders[i].source = Collider3DSource::ENTITY_HEIGHTFIELD;
                else body.colliders[i].source = Collider3DSource::NONE;
            }

            if (body.colliders[i].source == Collider3DSource::NONE) {
                if (body.colliders[i].type == Collider3DType::CONVEX_HULL) {
                    body.colliders[i].source = Collider3DSource::ENTITY_MESH;
                } else if (body.colliders[i].type == Collider3DType::MESH) {
                    body.colliders[i].source = Collider3DSource::ENTITY_MESH;
                } else if (body.colliders[i].type == Collider3DType::HEIGHTFIELD) {
                    body.colliders[i].source = Collider3DSource::ENTITY_HEIGHTFIELD;
                }
            }

            if (node["colliders"][i]["sourceEntity"]) body.colliders[i].sourceEntity = YamlNumbers::as<Entity>(node["colliders"][i]["sourceEntity"]);

            if (node["colliders"][i]["samplesSize"]) body.colliders[i].samplesSize = YamlNumbers::as<unsigned int>(node["colliders"][i]["samplesSize"]);

            if (node["colliders"][i]["vertices"]) {
                size_t vcount = node["colliders"][i]["vertices"].size();
                body.colliders[i].numVertices = (uint16_t)vcount;
                for (size_t j = 0; j < vcount; j++) {
                    body.colliders[i].vertices[j] = decodeVector3(node["colliders"][i]["vertices"][j]);
                }
            }

            if (node["colliders"][i]["indices"]) {
                size_t icount = node["colliders"][i]["indices"].size();
                body.colliders[i].numIndices = (uint16_t)icount;
                for (size_t j = 0; j < icount; j++) {
                    body.colliders[i].indices[j] = YamlNumbers::as<uint16_t>(node["colliders"][i]["indices"][j]);
                }
            }

            body.colliders[i].shape = NULL;
        }
    }

    if (oldBody && !oldBody->body.IsInvalid()) {
        body.needReloadBody = true;
        body.needUpdateColliders = true;
    }

    if (node["enabled"]) body.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return body;
}

// ==============================
// Joint2DComponent encode/decode
// ==============================

// ==============================
// Joint3DComponent encode/decode
// ==============================

// ── ActionComponent ──

std::string editor::Stream::actionStateToString(ActionState state) {
    return StreamSchema::actionState().toString(state);
}

ActionState editor::Stream::stringToActionState(const std::string& str) {
    return StreamSchema::actionState().fromString(str);
}

// ── TweenComponent ──

// ── PositionTweenComponent ──

// ── RotationTweenComponent ──

// ── ScaleTweenComponent ──

// ── ColorTweenComponent ──

// ── AlphaTweenComponent ──

// ── SpriteAnimationComponent ──

YAML::Node editor::Stream::encodeSpriteAnimationComponent(const SpriteAnimationComponent& spriteanim) {
    YAML::Node node;

    node["loop"] = spriteanim.loop;

    YAML::Node framesNode;
    for (unsigned int i = 0; i < spriteanim.framesSize; i++) {
        framesNode.push_back(spriteanim.frames[i]);
    }
    node["frames"] = framesNode;

    YAML::Node framesTimeNode;
    for (unsigned int i = 0; i < spriteanim.framesTimeSize; i++) {
        framesTimeNode.push_back(spriteanim.framesTime[i]);
    }
    node["framesTime"] = framesTimeNode;

    return node;
}

SpriteAnimationComponent editor::Stream::decodeSpriteAnimationComponent(const YAML::Node& node, const SpriteAnimationComponent* oldSpriteanim) {
    SpriteAnimationComponent spriteanim;

    if (oldSpriteanim) {
        spriteanim = *oldSpriteanim;
    }

    spriteanim.frameIndex = 0;
    spriteanim.frameTimeIndex = 0;
    spriteanim.spriteFrameCount = 0;

    if (node["loop"]) spriteanim.loop = YamlNumbers::as<bool>(node["loop"]);

    if (node["frames"]) {
        spriteanim.framesSize = 0;
        for (const YAML::Node& frameNode : node["frames"]) {
            if (!spriteanim.frames.validIndex(spriteanim.framesSize)) break;
            spriteanim.frames[spriteanim.framesSize] = YamlNumbers::as<int>(frameNode);
            spriteanim.framesSize++;
        }
    }

    if (node["framesTime"]) {
        spriteanim.framesTimeSize = 0;
        for (const YAML::Node& timeNode : node["framesTime"]) {
            if (!spriteanim.framesTime.validIndex(spriteanim.framesTimeSize)) break;
            spriteanim.framesTime[spriteanim.framesTimeSize] = YamlNumbers::as<int>(timeNode);
            spriteanim.framesTimeSize++;
        }
    }

    return spriteanim;
}

// Value/tangent list plumbing shared by the keyframe track components below
template<typename T, typename EncodeFn>
static YAML::Node encodeList(const std::vector<T>& values, EncodeFn encodeValue) {
    YAML::Node node;
    for (const auto& v : values) {
        node.push_back(encodeValue(v));
    }
    return node;
}

template<typename T, typename DecodeFn>
static void decodeListInto(const YAML::Node& node, std::vector<T>& values, DecodeFn decodeValue) {
    values.clear();
    for (const YAML::Node& v : node) {
        values.push_back(decodeValue(v));
    }
}

// Encodes values plus the optional Hermite tangents (GLTF CUBICSPLINE clips);
// decode clears tangents that are absent from the file
template<typename Tracks, typename EncodeFn>
static YAML::Node encodeTrackLists(const Tracks& tracks, EncodeFn encodeValue) {
    YAML::Node node;
    node["values"] = encodeList(tracks.values, encodeValue);
    if (!tracks.inTangents.empty()) {
        node["inTangents"] = encodeList(tracks.inTangents, encodeValue);
    }
    if (!tracks.outTangents.empty()) {
        node["outTangents"] = encodeList(tracks.outTangents, encodeValue);
    }
    return node;
}

template<typename Tracks, typename DecodeFn>
static void decodeTrackLists(const YAML::Node& node, Tracks& tracks, DecodeFn decodeValue) {
    if (node["values"]) {
        decodeListInto(node["values"], tracks.values, decodeValue);
    }
    tracks.inTangents.clear();
    if (node["inTangents"]) {
        decodeListInto(node["inTangents"], tracks.inTangents, decodeValue);
    }
    tracks.outTangents.clear();
    if (node["outTangents"]) {
        decodeListInto(node["outTangents"], tracks.outTangents, decodeValue);
    }
}

static YAML::Node encodeFloatList(const std::vector<float>& values) {
    YAML::Node node;
    for (float f : values) {
        node.push_back(f);
    }
    return node;
}

static std::vector<float> decodeFloatList(const YAML::Node& node) {
    std::vector<float> values;
    for (const YAML::Node& f : node) {
        values.push_back(editor::YamlNumbers::as<float>(f));
    }
    return values;
}

YAML::Node editor::Stream::encodeMorphTracksComponent(const MorphTracksComponent& tracks) {
    return encodeTrackLists(tracks, encodeFloatList);
}

MorphTracksComponent editor::Stream::decodeMorphTracksComponent(const YAML::Node& node, const MorphTracksComponent* oldTracks) {
    MorphTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    decodeTrackLists(node, tracks, decodeFloatList);

    return tracks;
}

YAML::Node editor::Stream::encodeParticleSystemComponent(const ParticleSystemComponent& particles) {
    YAML::Node node;
    node["enabled"] = particles.enabled;
    if (!particles.effect.empty() || !particles.effect.guid.isNull()){
        encodeAssetRef(node, "effect", particles.effect);
    }else{
        // the inline effect, as the canonical .particles JSON
        node["inlineEffect"] = particles::ParticleEffectCodec::write(particles.inlineEffect);
    }
    node["playOnAwake"] = particles.playOnAwake;
    node["seed"] = particles.seed;
    node["playbackSpeed"] = particles.playbackSpeed;
    if (!particles.exposedValues.empty()){
        YAML::Node values;
        for (const ParticleExposedValue& v : particles.exposedValues){
            YAML::Node value;
            value["name"] = v.name;
            value["value"] = encodeVector4(v.value);
            values.push_back(value);
        }
        node["exposedValues"] = values;
    }
    if (!particles.collisionPlanes.empty()){
        YAML::Node planes;
        for (Entity plane : particles.collisionPlanes) planes.push_back(plane);
        node["collisionPlanes"] = planes;
    }
    return node;
}

ParticleSystemComponent editor::Stream::decodeParticleSystemComponent(const YAML::Node& node, const ParticleSystemComponent* oldParticles) {
    ParticleSystemComponent particles;
    // over an existing component its runtime stays: the decoded effect rebinds it (keeping the
    // live particles when only parameters changed)
    if (oldParticles) particles = *oldParticles;
    if (node["enabled"]) particles.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["effect"]){
        particles.effect = decodeAssetRef(node, "effect");
    }else if (node["inlineEffect"]){
        particles.effect.clear();
        particles::ParticleEffectDocument document;
        std::vector<particles::Diagnostic> diagnostics;
        if (particles::ParticleEffectCodec::read(node["inlineEffect"].as<std::string>(), document, diagnostics)){
            particles.inlineEffect = document;
        }else{
            Out::error("The inline particle effect does not read: %s", particles::ParticleEffectCodec::format(diagnostics).c_str());
        }
    }
    if (node["playOnAwake"]) particles.playOnAwake = YamlNumbers::as<bool>(node["playOnAwake"]);
    if (node["seed"]) particles.seed = YamlNumbers::as<uint32_t>(node["seed"]);
    if (node["playbackSpeed"]) particles.playbackSpeed = YamlNumbers::as<float>(node["playbackSpeed"]);
    // written only when not empty: absent is none (over an old component too)
    particles.exposedValues.clear();
    if (node["exposedValues"] && node["exposedValues"].IsSequence()){
        for (const YAML::Node& value : node["exposedValues"]){
            ParticleExposedValue v;
            if (value["name"]) v.name = value["name"].as<std::string>();
            if (value["value"]) v.value = decodeVector4(value["value"]);
            particles.exposedValues.push_back(v);
        }
    }
    particles.collisionPlanes.clear();
    if (node["collisionPlanes"] && node["collisionPlanes"].IsSequence()){
        for (const YAML::Node& plane : node["collisionPlanes"]) particles.collisionPlanes.push_back(YamlNumbers::as<Entity>(plane));
    }
    particles.needReload = true;
    return particles;
}

// The particle system of a scene written before S20 (migration only: StreamMigrations converts
// it into an effect)
LegacyParticleSystem editor::Stream::decodeLegacyParticleSystem(const YAML::Node& node) {
    LegacyParticleSystem particles;

    if (node["maxParticles"]) particles.maxParticles = YamlNumbers::as<unsigned int>(node["maxParticles"]);
    if (node["loop"]) particles.loop = YamlNumbers::as<bool>(node["loop"]);
    if (node["localSpace"]) particles.localSpace = YamlNumbers::as<bool>(node["localSpace"]);
    if (node["rate"]) particles.rate = YamlNumbers::as<int>(node["rate"]);
    if (node["maxPerUpdate"]) particles.maxPerUpdate = YamlNumbers::as<int>(node["maxPerUpdate"]);

    if (node["bursts"] && node["bursts"].IsSequence()) {
        particles.bursts.clear();
        for (const auto& bn : node["bursts"]) {
            ParticleBurst b;
            if (bn["time"]) b.time = YamlNumbers::as<float>(bn["time"]);
            if (bn["minCount"]) b.minCount = YamlNumbers::as<int>(bn["minCount"]);
            if (bn["maxCount"]) b.maxCount = YamlNumbers::as<int>(bn["maxCount"]);
            particles.bursts.push_back(b);
        }
    }

    if (node["lifeInitializer"]) {
        const YAML::Node& n = node["lifeInitializer"];
        if (n["minLife"]) particles.lifeInitializer.minLife = YamlNumbers::as<float>(n["minLife"]);
        if (n["maxLife"]) particles.lifeInitializer.maxLife = YamlNumbers::as<float>(n["maxLife"]);
    }
    if (node["positionInitializer"]) {
        const YAML::Node& n = node["positionInitializer"];
        if (n["shape"]) particles.positionInitializer.shape = decodeEnum(n, "shape", StreamSchema::particleEmitterShape());
        if (n["minPosition"]) particles.positionInitializer.minPosition = decodeVector3(n["minPosition"]);
        if (n["maxPosition"]) particles.positionInitializer.maxPosition = decodeVector3(n["maxPosition"]);
        if (n["radius"]) particles.positionInitializer.radius = YamlNumbers::as<float>(n["radius"]);
        if (n["innerRadius"]) particles.positionInitializer.innerRadius = YamlNumbers::as<float>(n["innerRadius"]);
        if (n["coneAngle"]) particles.positionInitializer.coneAngle = YamlNumbers::as<float>(n["coneAngle"]);
        if (n["coneHeight"]) particles.positionInitializer.coneHeight = YamlNumbers::as<float>(n["coneHeight"]);
    }
    if (node["positionModifier"]) {
        const YAML::Node& n = node["positionModifier"];
        if (n["fromTime"]) particles.positionModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.positionModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["fromPosition"]) particles.positionModifier.fromPosition = decodeVector3(n["fromPosition"]);
        if (n["toPosition"]) particles.positionModifier.toPosition = decodeVector3(n["toPosition"]);
        if (n["function"]) particles.positionModifier.function = decodeEase(n["function"]);
    }
    if (node["velocityInitializer"]) {
        const YAML::Node& n = node["velocityInitializer"];
        if (n["minVelocity"]) particles.velocityInitializer.minVelocity = decodeVector3(n["minVelocity"]);
        if (n["maxVelocity"]) particles.velocityInitializer.maxVelocity = decodeVector3(n["maxVelocity"]);
    }
    if (node["velocityModifier"]) {
        const YAML::Node& n = node["velocityModifier"];
        if (n["fromTime"]) particles.velocityModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.velocityModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["fromVelocity"]) particles.velocityModifier.fromVelocity = decodeVector3(n["fromVelocity"]);
        if (n["toVelocity"]) particles.velocityModifier.toVelocity = decodeVector3(n["toVelocity"]);
        if (n["function"]) particles.velocityModifier.function = decodeEase(n["function"]);
    }
    if (node["accelerationInitializer"]) {
        const YAML::Node& n = node["accelerationInitializer"];
        if (n["minAcceleration"]) particles.accelerationInitializer.minAcceleration = decodeVector3(n["minAcceleration"]);
        if (n["maxAcceleration"]) particles.accelerationInitializer.maxAcceleration = decodeVector3(n["maxAcceleration"]);
    }
    if (node["accelerationModifier"]) {
        const YAML::Node& n = node["accelerationModifier"];
        if (n["fromTime"]) particles.accelerationModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.accelerationModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["fromAcceleration"]) particles.accelerationModifier.fromAcceleration = decodeVector3(n["fromAcceleration"]);
        if (n["toAcceleration"]) particles.accelerationModifier.toAcceleration = decodeVector3(n["toAcceleration"]);
        if (n["function"]) particles.accelerationModifier.function = decodeEase(n["function"]);
    }
    if (node["colorInitializer"]) {
        const YAML::Node& n = node["colorInitializer"];
        if (n["minColor"]) particles.colorInitializer.minColor = decodeVector3(n["minColor"]);
        if (n["maxColor"]) particles.colorInitializer.maxColor = decodeVector3(n["maxColor"]);
        if (n["useSRGB"]) particles.colorInitializer.useSRGB = YamlNumbers::as<bool>(n["useSRGB"]);
    }
    if (node["colorModifier"]) {
        const YAML::Node& n = node["colorModifier"];
        if (n["fromTime"]) particles.colorModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.colorModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["fromColor"]) particles.colorModifier.fromColor = decodeVector3(n["fromColor"]);
        if (n["toColor"]) particles.colorModifier.toColor = decodeVector3(n["toColor"]);
        if (n["function"]) particles.colorModifier.function = decodeEase(n["function"]);
        if (n["useSRGB"]) particles.colorModifier.useSRGB = YamlNumbers::as<bool>(n["useSRGB"]);
    }
    if (node["colorGradient"]) {
        const YAML::Node& n = node["colorGradient"];
        if (n["useSRGB"]) particles.colorGradient.useSRGB = YamlNumbers::as<bool>(n["useSRGB"]);
        if (n["stops"] && n["stops"].IsSequence()) {
            particles.colorGradient.stops.clear();
            for (const auto& sn : n["stops"]) {
                ParticleColorGradientStop s;
                if (sn["time"]) s.time = YamlNumbers::as<float>(sn["time"]);
                if (sn["color"]) s.color = decodeVector3(sn["color"]);
                particles.colorGradient.stops.push_back(s);
            }
            particles.colorGradient.normalize();
        }
    }
    if (node["alphaInitializer"]) {
        const YAML::Node& n = node["alphaInitializer"];
        if (n["minAlpha"]) particles.alphaInitializer.minAlpha = YamlNumbers::as<float>(n["minAlpha"]);
        if (n["maxAlpha"]) particles.alphaInitializer.maxAlpha = YamlNumbers::as<float>(n["maxAlpha"]);
    }
    if (node["alphaModifier"]) {
        const YAML::Node& n = node["alphaModifier"];
        if (n["fromTime"]) particles.alphaModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.alphaModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["fromAlpha"]) particles.alphaModifier.fromAlpha = YamlNumbers::as<float>(n["fromAlpha"]);
        if (n["toAlpha"]) particles.alphaModifier.toAlpha = YamlNumbers::as<float>(n["toAlpha"]);
        if (n["function"]) particles.alphaModifier.function = decodeEase(n["function"]);
    }
    if (node["sizeInitializer"]) {
        const YAML::Node& n = node["sizeInitializer"];
        if (n["minSize"]) particles.sizeInitializer.minSize = YamlNumbers::as<float>(n["minSize"]);
        if (n["maxSize"]) particles.sizeInitializer.maxSize = YamlNumbers::as<float>(n["maxSize"]);
    }
    if (node["sizeModifier"]) {
        const YAML::Node& n = node["sizeModifier"];
        if (n["fromTime"]) particles.sizeModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.sizeModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["fromSize"]) particles.sizeModifier.fromSize = YamlNumbers::as<float>(n["fromSize"]);
        if (n["toSize"]) particles.sizeModifier.toSize = YamlNumbers::as<float>(n["toSize"]);
        if (n["function"]) particles.sizeModifier.function = decodeEase(n["function"]);
    }
    if (node["spriteInitializer"]) {
        const YAML::Node& n = node["spriteInitializer"];
        if (n["frames"]) {
            particles.spriteInitializer.frames.clear();
            for (const YAML::Node& f : n["frames"]) particles.spriteInitializer.frames.push_back(YamlNumbers::as<int>(f));
        }
    }
    if (node["spriteModifier"]) {
        const YAML::Node& n = node["spriteModifier"];
        if (n["fromTime"]) particles.spriteModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.spriteModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["function"]) particles.spriteModifier.function = decodeEase(n["function"]);
        if (n["frames"]) {
            particles.spriteModifier.frames.clear();
            for (const YAML::Node& f : n["frames"]) particles.spriteModifier.frames.push_back(YamlNumbers::as<int>(f));
        }
    }
    if (node["rotationInitializer"]) {
        const YAML::Node& n = node["rotationInitializer"];
        if (n["minRotation"]) particles.rotationInitializer.minRotation = decodeQuaternion(n["minRotation"]);
        if (n["maxRotation"]) particles.rotationInitializer.maxRotation = decodeQuaternion(n["maxRotation"]);
        if (n["shortestPath"]) particles.rotationInitializer.shortestPath = YamlNumbers::as<bool>(n["shortestPath"]);
    }
    if (node["rotationModifier"]) {
        const YAML::Node& n = node["rotationModifier"];
        if (n["fromTime"]) particles.rotationModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.rotationModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["fromRotation"]) particles.rotationModifier.fromRotation = decodeQuaternion(n["fromRotation"]);
        if (n["toRotation"]) particles.rotationModifier.toRotation = decodeQuaternion(n["toRotation"]);
        if (n["function"]) particles.rotationModifier.function = decodeEase(n["function"]);
        if (n["shortestPath"]) particles.rotationModifier.shortestPath = YamlNumbers::as<bool>(n["shortestPath"]);
    }
    if (node["scaleInitializer"]) {
        const YAML::Node& n = node["scaleInitializer"];
        if (n["minScale"]) particles.scaleInitializer.minScale = decodeVector3(n["minScale"]);
        if (n["maxScale"]) particles.scaleInitializer.maxScale = decodeVector3(n["maxScale"]);
        if (n["linearSort"]) particles.scaleInitializer.linearSort = YamlNumbers::as<bool>(n["linearSort"]);
    }
    if (node["scaleModifier"]) {
        const YAML::Node& n = node["scaleModifier"];
        if (n["fromTime"]) particles.scaleModifier.fromTime = YamlNumbers::as<float>(n["fromTime"]);
        if (n["toTime"]) particles.scaleModifier.toTime = YamlNumbers::as<float>(n["toTime"]);
        if (n["fromScale"]) particles.scaleModifier.fromScale = decodeVector3(n["fromScale"]);
        if (n["toScale"]) particles.scaleModifier.toScale = decodeVector3(n["toScale"]);
        if (n["function"]) particles.scaleModifier.function = decodeEase(n["function"]);
    }

    return particles;
}

YAML::Node editor::Stream::encodeForceFieldComponent(const ForceFieldComponent& field) {
    YAML::Node node;
    node["enabled"] = field.enabled;
    node["shape"] = StreamSchema::forceFieldShape().toString(field.shape);
    node["size"] = encodeVector3(field.size);
    node["kind"] = StreamSchema::forceFieldKind().toString(field.kind);
    node["strength"] = field.strength;
    node["direction"] = encodeVector3(field.direction);
    node["turbulence"] = field.turbulence;
    node["pulseFrequency"] = field.pulseFrequency;
    node["frequency"] = field.frequency;
    // [time, value, inTangent, outTangent] per key (a step tangent is .inf)
    YAML::Node keys(YAML::NodeType::Sequence);
    for (const CurveKey& key : field.falloff.keys){
        YAML::Node k(YAML::NodeType::Sequence);
        k.SetStyle(YAML::EmitterStyle::Flow);
        k.push_back(key.time);
        k.push_back(key.value);
        k.push_back(key.inTangent);
        k.push_back(key.outTangent);
        keys.push_back(k);
    }
    node["falloff"] = keys;
    node["layers"] = field.layers;
    return node;
}

ForceFieldComponent editor::Stream::decodeForceFieldComponent(const YAML::Node& node, const ForceFieldComponent* oldField) {
    ForceFieldComponent field;
    if (oldField) field = *oldField;
    if (node["enabled"]) field.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["shape"]) field.shape = decodeEnum(node, "shape", StreamSchema::forceFieldShape());
    if (node["size"]) field.size = decodeVector3(node["size"]);
    if (node["kind"]) field.kind = decodeEnum(node, "kind", StreamSchema::forceFieldKind());
    if (node["strength"]) field.strength = YamlNumbers::as<float>(node["strength"]);
    if (node["direction"]) field.direction = decodeVector3(node["direction"]);
    if (node["turbulence"]) field.turbulence = YamlNumbers::as<float>(node["turbulence"]);
    if (node["pulseFrequency"]) field.pulseFrequency = YamlNumbers::as<float>(node["pulseFrequency"]);
    if (node["frequency"]) field.frequency = YamlNumbers::as<float>(node["frequency"]);
    if (node["falloff"] && node["falloff"].IsSequence()){
        field.falloff.keys.clear();
        for (const YAML::Node& k : node["falloff"]){
            if (!k.IsSequence() || k.size() < 4) continue;
            CurveKey key;
            key.time = YamlNumbers::as<float>(k[0]);
            key.value = YamlNumbers::as<float>(k[1]);
            key.inTangent = YamlNumbers::as<float>(k[2]);
            key.outTangent = YamlNumbers::as<float>(k[3]);
            field.falloff.keys.push_back(key);
        }
    }
    if (node["layers"]) field.layers = YamlNumbers::as<uint32_t>(node["layers"]);
    return field;
}

YAML::Node editor::Stream::encodeInstancedMeshComponent(const InstancedMeshComponent& instmesh) {
    YAML::Node node;
    node["enabled"] = instmesh.enabled;

    node["maxInstances"] = instmesh.maxInstances;
    node["cullInstances"] = instmesh.cullInstances;
    node["distanceFade"] = instmesh.distanceFade;
    node["cullDistance"] = instmesh.cullDistance;
    node["fadeStart"] = instmesh.fadeStart;
    node["fadeEnd"] = instmesh.fadeEnd;
    node["instancedBillboard"] = instmesh.instancedBillboard;
    node["instancedCylindricalBillboard"] = instmesh.instancedCylindricalBillboard;

    YAML::Node instancesNode;
    for (const InstanceData& inst : instmesh.instances) {
        YAML::Node instNode;
        instNode["position"] = encodeVector3(inst.position);
        instNode["rotation"] = encodeQuaternion(inst.rotation);
        instNode["scale"] = encodeVector3(inst.scale);
        instNode["color"] = encodeVector4(inst.color);
        instNode["textureRect"] = encodeRect(inst.textureRect);
        instNode["visible"] = inst.visible;
        instancesNode.push_back(instNode);
    }
    node["instances"] = instancesNode;

    return node;
}

InstancedMeshComponent editor::Stream::decodeInstancedMeshComponent(const YAML::Node& node, const InstancedMeshComponent* oldInstmesh) {
    InstancedMeshComponent instmesh;
    if (oldInstmesh) { instmesh = *oldInstmesh; }

    if (node["maxInstances"]) instmesh.maxInstances = YamlNumbers::as<unsigned int>(node["maxInstances"]);
    if (node["cullInstances"]) instmesh.cullInstances = YamlNumbers::as<bool>(node["cullInstances"]);
    if (node["distanceFade"]) instmesh.distanceFade = YamlNumbers::as<bool>(node["distanceFade"]);
    instmesh.cullDistance = std::max(0.0f, decodeFinite(node["cullDistance"], instmesh.cullDistance));
    instmesh.fadeStart = decodeFinite(node["fadeStart"], instmesh.fadeStart);
    instmesh.fadeEnd = decodeFinite(node["fadeEnd"], instmesh.fadeEnd);
    if (node["instancedBillboard"]) instmesh.instancedBillboard = YamlNumbers::as<bool>(node["instancedBillboard"]);
    if (node["instancedCylindricalBillboard"]) instmesh.instancedCylindricalBillboard = YamlNumbers::as<bool>(node["instancedCylindricalBillboard"]);

    if (node["instances"]) {
        instmesh.instances.clear();
        for (const YAML::Node& instNode : node["instances"]) {
            InstanceData inst;
            if (instNode["position"]) inst.position = decodeVector3(instNode["position"]);
            if (instNode["rotation"]) inst.rotation = decodeQuaternion(instNode["rotation"]);
            if (instNode["scale"]) inst.scale = decodeVector3(instNode["scale"]);
            if (instNode["color"]) inst.color = decodeVector4(instNode["color"]);
            if (instNode["textureRect"]) inst.textureRect = decodeRect(instNode["textureRect"]);
            if (instNode["visible"]) inst.visible = YamlNumbers::as<bool>(instNode["visible"]);
            instmesh.instances.push_back(inst);
        }
    }

    // Reset runtime fields
    instmesh.renderInstances.clear();
    instmesh.numVisible = 0;
    instmesh.needUpdateBuffer = false;
    instmesh.needUpdateInstances = true;

    if (node["enabled"]) instmesh.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return instmesh;
}

YAML::Node editor::Stream::encodePointsComponent(const PointsComponent& points) {
    YAML::Node node;
    node["enabled"] = points.enabled;

    node["maxPoints"] = points.maxPoints;
    node["transparent"] = points.transparent;
    node["autoTransparency"] = points.autoTransparency;
    node["texture"] = encodeTexture(points.texture);

    YAML::Node framesNode;
    for (unsigned int i = 0; i < points.numFramesRect; i++) {
        framesNode.push_back(encodeSpriteFrameData(points.framesRect[i]));
    }
    if (framesNode.size() > 0) {
        node["framesRect"] = framesNode;
    }

    YAML::Node pointsNode;
    for (const PointData& pt : points.points) {
        YAML::Node ptNode;
        ptNode["position"] = encodeVector3(pt.position);
        ptNode["color"] = encodeVector4(pt.color);
        ptNode["size"] = pt.size;
        ptNode["rotation"] = pt.rotation;
        ptNode["textureRect"] = encodeRect(pt.textureRect);
        ptNode["visible"] = pt.visible;
        pointsNode.push_back(ptNode);
    }
    node["points"] = pointsNode;

    if (!points.customShader.empty())
        encodeShaderRef(node, "customShader", points.customShader);
    if (!points.shaderUniforms.empty())
        node["shaderUniforms"] = encodeShaderUniforms(points.shaderUniforms);
    encodeComponentShaderLibrary(node, points.shaderTextures, points.shaderFeatures, &points.blendMode, &points.depthWrite, true);
    encodeStencilAndColorWrite(node, points.stencil, points.colorWrite);

    return node;
}

PointsComponent editor::Stream::decodePointsComponent(const YAML::Node& node, const PointsComponent* oldPoints) {
    PointsComponent points;
    if (oldPoints) { points = *oldPoints; }

    if (node["maxPoints"]) points.maxPoints = YamlNumbers::as<unsigned int>(node["maxPoints"]);
    if (node["transparent"]) points.transparent = YamlNumbers::as<bool>(node["transparent"]);
    if (node["autoTransparency"]) points.autoTransparency = YamlNumbers::as<bool>(node["autoTransparency"]);
    if (node["texture"]) points.texture = decodeTexture(node["texture"]);

    if (node["framesRect"]) {
        points.numFramesRect = 0;
        for (const YAML::Node& frameNode : node["framesRect"]) {
            if (points.numFramesRect < (unsigned int)points.framesRect.size()) {
                points.framesRect[points.numFramesRect] = decodeSpriteFrameData(frameNode);
                points.numFramesRect++;
            }
        }
    }

    if (node["points"]) {
        points.points.clear();
        for (const YAML::Node& ptNode : node["points"]) {
            PointData pt;
            if (ptNode["position"]) pt.position = decodeVector3(ptNode["position"]);
            if (ptNode["color"]) pt.color = decodeVector4(ptNode["color"]);
            if (ptNode["size"]) pt.size = YamlNumbers::as<float>(ptNode["size"]);
            if (ptNode["rotation"]) pt.rotation = YamlNumbers::as<float>(ptNode["rotation"]);
            if (ptNode["textureRect"]) pt.textureRect = decodeRect(ptNode["textureRect"]);
            if (ptNode["visible"]) pt.visible = YamlNumbers::as<bool>(ptNode["visible"]);
            points.points.push_back(pt);
        }
    }

    if (node["customShader"]) points.customShader = decodeShaderRef(node, "customShader");
    points.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    points.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, points.shaderTextures, points.shaderFeatures, &points.blendMode, &points.depthWrite);
    decodeStencilAndColorWrite(node, points.stencil, points.colorWrite);

    // Reset runtime fields
    points.renderPoints.clear();
    points.numVisible = 0;
    points.loaded = false;
    points.loadCalled = false;
    points.needUpdate = true;
    points.needUpdateBuffer = false;
    points.needUpdateTexture = true;
    points.needReload = false;

    if (node["enabled"]) points.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return points;
}

YAML::Node editor::Stream::encodeLineRendererComponent(const LineRendererComponent& lines) {
    YAML::Node node;
    node["enabled"] = lines.enabled;

    node["maxLines"] = lines.maxLines;

    YAML::Node linesNode;
    for (const LineData& line : lines.lines) {
        YAML::Node lineNode;
        lineNode["pointA"] = encodeVector3(line.pointA);
        lineNode["colorA"] = encodeVector4(line.colorA);
        lineNode["pointB"] = encodeVector3(line.pointB);
        lineNode["colorB"] = encodeVector4(line.colorB);
        linesNode.push_back(lineNode);
    }
    node["lines"] = linesNode;

    if (!lines.customShader.empty())
        encodeShaderRef(node, "customShader", lines.customShader);
    if (!lines.shaderUniforms.empty())
        node["shaderUniforms"] = encodeShaderUniforms(lines.shaderUniforms);
    encodeComponentShaderLibrary(node, lines.shaderTextures, lines.shaderFeatures, &lines.blendMode, &lines.depthWrite, true);
    encodeStencilAndColorWrite(node, lines.stencil, lines.colorWrite);

    return node;
}

LineRendererComponent editor::Stream::decodeLineRendererComponent(const YAML::Node& node, const LineRendererComponent* oldLines) {
    LineRendererComponent lines;
    if (oldLines) { lines = *oldLines; }

    if (node["maxLines"]) lines.maxLines = YamlNumbers::as<unsigned int>(node["maxLines"]);

    if (node["lines"]) {
        lines.lines.clear();
        for (const YAML::Node& lineNode : node["lines"]) {
            LineData line;
            if (lineNode["pointA"]) line.pointA = decodeVector3(lineNode["pointA"]);
            if (lineNode["colorA"]) line.colorA = decodeVector4(lineNode["colorA"]);
            if (lineNode["pointB"]) line.pointB = decodeVector3(lineNode["pointB"]);
            if (lineNode["colorB"]) line.colorB = decodeVector4(lineNode["colorB"]);
            lines.lines.push_back(line);
        }
    }

    if (lines.lines.size() > lines.maxLines) {
        lines.maxLines = static_cast<unsigned int>(lines.lines.size());
    }

    if (node["customShader"]) lines.customShader = decodeShaderRef(node, "customShader");
    lines.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    lines.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, lines.shaderTextures, lines.shaderFeatures, &lines.blendMode, &lines.depthWrite);
    decodeStencilAndColorWrite(node, lines.stencil, lines.colorWrite);

    lines.loaded = false;
    lines.loadCalled = false;
    lines.needUpdateBuffer = false;
    lines.needReload = false;

    if (node["enabled"]) lines.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return lines;
}

YAML::Node editor::Stream::encodePolygonComponent(const PolygonComponent& polygon) {
    YAML::Node node;

    YAML::Node pointsNode;
    for (const PolygonPoint& point : polygon.points) {
        YAML::Node pointNode;
        pointNode["position"] = encodeVector3(point.position);
        pointNode["color"] = encodeVector4(point.color);
        pointsNode.push_back(pointNode);
    }
    node["points"] = pointsNode;

    return node;
}

PolygonComponent editor::Stream::decodePolygonComponent(const YAML::Node& node, const PolygonComponent* oldPolygon) {
    PolygonComponent polygon;
    if (oldPolygon) { polygon = *oldPolygon; }

    if (node["points"]) {
        polygon.points.clear();
        for (const YAML::Node& pointNode : node["points"]) {
            PolygonPoint point;
            if (pointNode["position"]) point.position = decodeVector3(pointNode["position"]);
            if (pointNode["color"]) point.color = decodeVector4(pointNode["color"]);
            polygon.points.push_back(point);
        }
    }

    polygon.needUpdatePolygon = true;

    return polygon;
}

YAML::Node editor::Stream::encodeMeshPolygonComponent(const MeshPolygonComponent& polygon) {
    YAML::Node node;

    node["width"] = polygon.width;
    node["height"] = polygon.height;
    node["automaticFlipY"] = polygon.automaticFlipY;
    node["flipY"] = polygon.flipY;

    YAML::Node pointsNode;
    for (const PolygonPoint& point : polygon.points) {
        YAML::Node pointNode;
        pointNode["position"] = encodeVector3(point.position);
        pointNode["color"] = encodeVector4(point.color);
        pointsNode.push_back(pointNode);
    }
    node["points"] = pointsNode;

    return node;
}

MeshPolygonComponent editor::Stream::decodeMeshPolygonComponent(const YAML::Node& node, const MeshPolygonComponent* oldPolygon) {
    MeshPolygonComponent polygon;
    if (oldPolygon) { polygon = *oldPolygon; }

    if (node["width"]) polygon.width = YamlNumbers::as<unsigned int>(node["width"]);
    if (node["height"]) polygon.height = YamlNumbers::as<unsigned int>(node["height"]);
    if (node["automaticFlipY"]) polygon.automaticFlipY = YamlNumbers::as<bool>(node["automaticFlipY"]);
    if (node["flipY"]) polygon.flipY = YamlNumbers::as<bool>(node["flipY"]);

    if (node["points"]) {
        polygon.points.clear();
        for (const YAML::Node& pointNode : node["points"]) {
            PolygonPoint point;
            if (pointNode["position"]) point.position = decodeVector3(pointNode["position"]);
            if (pointNode["color"]) point.color = decodeVector4(pointNode["color"]);
            polygon.points.push_back(point);
        }
    }

    polygon.needUpdatePolygon = true;

    return polygon;
}

YAML::Node editor::Stream::encodeLatticeOffsets(const std::vector<Vector3>& offsets) {
    YAML::Node node(YAML::NodeType::Sequence);
    for (const Vector3& offset : offsets) {
        node.push_back(encodeVector3(offset));
    }
    if (offsets.empty()) node.SetStyle(YAML::EmitterStyle::Flow);
    return node;
}

std::vector<Vector3> editor::Stream::decodeLatticeOffsets(const YAML::Node& node) {
    std::vector<Vector3> offsets;
    if (node && node.IsSequence()) {
        offsets.reserve(node.size());
        for (const YAML::Node& offset : node) {
            offsets.push_back(decodeVector3(offset));
        }
    }
    return offsets;
}

YAML::Node editor::Stream::encodeLatticeComponent(const LatticeComponent& lattice) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(lattice.enabled);

    node["resolutionX"] = YamlNumbers::integerText(lattice.resolutionX);
    node["resolutionY"] = YamlNumbers::integerText(lattice.resolutionY);
    node["resolutionZ"] = YamlNumbers::integerText(lattice.resolutionZ);
    node["offsets"] = encodeLatticeOffsets(lattice.offsets);
    node["interpolation"] = latticeInterpolationToString(lattice.interpolation);
    node["outside"] = latticeOutsideToString(lattice.outside);
    encodeFinite(node, "fadeDistance", lattice.fadeDistance);

    YAML::Node posesNode(YAML::NodeType::Sequence);
    for (const LatticePose& pose : lattice.poses) {
        YAML::Node poseNode;
        poseNode["name"] = pose.name;
        poseNode["offsets"] = encodeLatticeOffsets(pose.offsets);
        posesNode.push_back(poseNode);
    }
    if (lattice.poses.empty()) posesNode.SetStyle(YAML::EmitterStyle::Flow);
    node["poses"] = posesNode;

    node["jiggle"] = YamlNumbers::text(lattice.jiggle);
    encodeFinite(node, "jiggleStiffness", lattice.jiggleStiffness);
    encodeFinite(node, "jiggleDamping", lattice.jiggleDamping);
    encodeFinite(node, "jiggleInertia", lattice.jiggleInertia);
    node["jiggleAnchor"] = latticeJiggleAnchorToString(lattice.jiggleAnchor);
    encodeFinite(node, "jiggleMaxOffset", lattice.jiggleMaxOffset);

    return node;
}

LatticeComponent editor::Stream::decodeLatticeComponent(const YAML::Node& node, const LatticeComponent* oldLattice) {
    LatticeComponent lattice;

    if (oldLattice) {
        lattice = *oldLattice;
    }

    if (node["enabled"]) lattice.enabled = YamlNumbers::as<bool>(node["enabled"]);

    if (node["resolutionX"]) lattice.resolutionX = YamlNumbers::as<int>(node["resolutionX"]);
    if (node["resolutionY"]) lattice.resolutionY = YamlNumbers::as<int>(node["resolutionY"]);
    if (node["resolutionZ"]) lattice.resolutionZ = YamlNumbers::as<int>(node["resolutionZ"]);
    lattice.resolutionX = std::clamp(lattice.resolutionX, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
    lattice.resolutionY = std::clamp(lattice.resolutionY, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
    lattice.resolutionZ = std::clamp(lattice.resolutionZ, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
    const size_t pointCount = static_cast<size_t>(lattice.resolutionX * lattice.resolutionY * lattice.resolutionZ);

    if (node["offsets"]) lattice.offsets = decodeLatticeOffsets(node["offsets"]);
    lattice.offsets.resize(pointCount, Vector3::ZERO);

    if (node["interpolation"]) lattice.interpolation = decodeEnum(node, "interpolation", StreamSchema::latticeInterpolation());
    if (node["outside"]) lattice.outside = decodeEnum(node, "outside", StreamSchema::latticeOutside());
    if (node["fadeDistance"]) lattice.fadeDistance = decodeFinite(node["fadeDistance"], lattice.fadeDistance);

    if (node["poses"]) {
        lattice.poses.clear();
        if (node["poses"].IsSequence()) {
            for (const YAML::Node& poseNode : node["poses"]) {
                LatticePose pose;
                if (poseNode["name"]) pose.name = poseNode["name"].as<std::string>();
                // a pose of another resolution is resampled where it is applied (spec §6.1)
                pose.offsets = decodeLatticeOffsets(poseNode["offsets"]);
                lattice.poses.push_back(std::move(pose));
            }
        }
    }

    if (node["jiggle"]) lattice.jiggle = YamlNumbers::as<bool>(node["jiggle"]);
    if (node["jiggleStiffness"]) lattice.jiggleStiffness = decodeFinite(node["jiggleStiffness"], lattice.jiggleStiffness);
    if (node["jiggleDamping"]) lattice.jiggleDamping = decodeFinite(node["jiggleDamping"], lattice.jiggleDamping);
    if (node["jiggleInertia"]) lattice.jiggleInertia = decodeFinite(node["jiggleInertia"], lattice.jiggleInertia);
    if (node["jiggleAnchor"]) lattice.jiggleAnchor = decodeEnum(node, "jiggleAnchor", StreamSchema::latticeJiggleAnchor());
    if (node["jiggleMaxOffset"]) lattice.jiggleMaxOffset = decodeFinite(node["jiggleMaxOffset"], lattice.jiggleMaxOffset);

    // the old one's springs and tween are laid out for its own points
    if (oldLattice && (oldLattice->resolutionX != lattice.resolutionX || oldLattice->resolutionY != lattice.resolutionY ||
                       oldLattice->resolutionZ != lattice.resolutionZ)) {
        lattice.dynamicOffsets.clear();
        lattice.jiggleVelocities.clear();
        lattice.jiggleSleeping = true;
        lattice.tweenFrom.clear();
        lattice.tweenTo.clear();
        lattice.tweening = false;
    }

    return lattice;
}

YAML::Node editor::Stream::encodeLatticeModifierComponent(const LatticeModifierComponent& modifier) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(modifier.enabled);

    YAML::Node bindingsNode(YAML::NodeType::Sequence);
    for (const LatticeBinding& binding : modifier.bindings) {
        YAML::Node bindingNode;
        bindingNode["lattice"] = binding.lattice;
        encodeFinite(bindingNode, "weight", binding.weight);
        bindingNode["stage"] = latticeStageToString(binding.stage);
        bindingNode["submesh"] = YamlNumbers::integerText(binding.submesh);
        bindingNode["mask"] = latticeMaskChannelToString(binding.mask);
        bindingsNode.push_back(bindingNode);
    }
    if (modifier.bindings.empty()) bindingsNode.SetStyle(YAML::EmitterStyle::Flow);
    node["bindings"] = bindingsNode;
    node["includeChildren"] = YamlNumbers::text(modifier.includeChildren);
    node["subdivisions"] = YamlNumbers::integerText(modifier.subdivisions);

    return node;
}

LatticeModifierComponent editor::Stream::decodeLatticeModifierComponent(const YAML::Node& node, const LatticeModifierComponent* oldModifier) {
    LatticeModifierComponent modifier;

    if (oldModifier) {
        modifier = *oldModifier;
    }

    if (node["enabled"]) modifier.enabled = YamlNumbers::as<bool>(node["enabled"]);

    if (node["bindings"]) {
        modifier.bindings.clear();
        if (node["bindings"].IsSequence()) {
            for (const YAML::Node& bindingNode : node["bindings"]) {
                LatticeBinding binding;
                if (bindingNode["lattice"]) binding.lattice = YamlNumbers::as<Entity>(bindingNode["lattice"]);
                if (bindingNode["weight"]) binding.weight = std::clamp(decodeFinite(bindingNode["weight"], binding.weight), 0.0f, 2.0f);
                if (bindingNode["stage"]) binding.stage = decodeEnum(bindingNode, "stage", StreamSchema::latticeStage());
                if (bindingNode["submesh"]) binding.submesh = YamlNumbers::as<int>(bindingNode["submesh"]);
                if (bindingNode["mask"]) binding.mask = decodeEnum(bindingNode, "mask", StreamSchema::latticeMaskChannel());
                modifier.bindings.push_back(binding);
            }
        }
        modifier.warnedOverflow = false;
    }
    if (node["includeChildren"]) modifier.includeChildren = YamlNumbers::as<bool>(node["includeChildren"]);
    if (node["subdivisions"]) modifier.subdivisions = std::clamp(YamlNumbers::as<int>(node["subdivisions"]), 1, 64);

    return modifier;
}

YAML::Node editor::Stream::encodeLatticeTrackKeys(const std::vector<std::vector<Vector3>>& keys) {
    YAML::Node node(YAML::NodeType::Sequence);
    for (const std::vector<Vector3>& key : keys) {
        node.push_back(encodeLatticeOffsets(key));
    }
    if (keys.empty()) node.SetStyle(YAML::EmitterStyle::Flow);
    return node;
}

std::vector<std::vector<Vector3>> editor::Stream::decodeLatticeTrackKeys(const YAML::Node& node) {
    std::vector<std::vector<Vector3>> keys;
    if (node && node.IsSequence()) {
        for (const YAML::Node& key : node) {
            keys.push_back(decodeLatticeOffsets(key));
        }
    }
    return keys;
}

YAML::Node editor::Stream::encodeLatticeTracksComponent(const LatticeTracksComponent& tracks) {
    YAML::Node node;

    YAML::Node pointsNode(YAML::NodeType::Sequence);
    pointsNode.SetStyle(YAML::EmitterStyle::Flow);
    for (int point : tracks.points) {
        pointsNode.push_back(YamlNumbers::integerText(point));
    }
    node["points"] = pointsNode;
    node["values"] = encodeLatticeTrackKeys(tracks.values);
    // Hermite tangents only on cubic tracks
    if (!tracks.inTangents.empty()) {
        node["inTangents"] = encodeLatticeTrackKeys(tracks.inTangents);
    }
    if (!tracks.outTangents.empty()) {
        node["outTangents"] = encodeLatticeTrackKeys(tracks.outTangents);
    }

    return node;
}

LatticeTracksComponent editor::Stream::decodeLatticeTracksComponent(const YAML::Node& node, const LatticeTracksComponent* oldTracks) {
    LatticeTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    if (node["points"]) {
        tracks.points.clear();
        if (node["points"].IsSequence()) {
            for (const YAML::Node& point : node["points"]) {
                tracks.points.push_back(YamlNumbers::as<int>(point));
            }
        }
    }
    if (node["values"]) tracks.values = decodeLatticeTrackKeys(node["values"]);
    // left out when empty, so an absent key is none
    tracks.inTangents = decodeLatticeTrackKeys(node["inTangents"]);
    tracks.outTangents = decodeLatticeTrackKeys(node["outTangents"]);

    return tracks;
}
