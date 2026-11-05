// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The component codecs of their own (docs/specs/S08-reflection.md R14): the components whose
// scene YAML the generic reflection codec does not write (geometry buffers, embedded data, layouts
// read from the entity's other components) keep a hand-written encode and decode, called through
// their ComponentDesc's codec (editor/reflect/components). The rest of the scene file is Stream.cpp.

#include "Stream.h"
#include "component/BuoyancyComponent.h"
#include "component/CharacterController2DComponent.h"
#include "component/CharacterControllerComponent.h"
#include "component/ForceFieldComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/LatticeTracksComponent.h"
#include "component/MeshComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PointsComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/Rigidbody2DComponent.h"
#include "component/RigidbodyComponent.h"
#include "component/SkyboxComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "component/UIComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/WaterVolumeComponent.h"
#include "component/ScriptComponent.h"
#include "component/LineRendererComponent.h"
#include "subsystem/MeshSystem.h"
#include "reflect/Reflect.h"
#include "log/Logger.h"
#include "StreamSchema.h"
#include "Catalog.h"
#include "util/ProjectUtils.h"
#include "util/YamlNumbers.h"
#include "lattice/LatticeTypes.h"
#include "Out.h"
#include "particles/ParticleEffectCodec.h"
#include "stream/CurveCodec.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ModelSubClips.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>

using namespace jasperengine;

YAML::Node editor::Stream::encodeMeshComponent(const MeshComponent& mesh, bool encodeBuffers, bool embedTextureData, bool encodeBones,
                                               unsigned int numSubmeshes, bool encodeGeneratedValues) {
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
    // past the count, what the generator will start from (the first slot exists before it ran)
    static const Submesh unbuilt{};
    for(unsigned int i = 0; i < std::max(mesh.numSubmeshes, numSubmeshes); i++) {
        submeshesNode.push_back(encodeSubmesh(i == 0 || i < mesh.numSubmeshes ? mesh.submeshes[i] : unbuilt, embedTextureData, encodeGeneratedValues));
    }
    node["submeshes"] = submeshesNode;

    // Model-backed meshes regenerate these matrices from their skeleton on load.
    if (encodeBones) {
        // the whole palette (a mesh without a skin has none): its length is how many joints the
        // skinned vertices may name. Files of older editors list it without its trailing
        // identities, as they all had 128 of them
        const size_t count = mesh.bonesMatrix.stored();
        YAML::Node bonesNode(YAML::NodeType::Sequence);
        for (size_t i = 0; i < count; i++)
            bonesNode.push_back(encodeMatrix4(mesh.bonesMatrix[i]));
        node["bonesMatrix"] = bonesNode;
    }

    node["normAdjustJoint"] = YamlNumbers::text(mesh.normAdjustJoint);
    node["normAdjustWeight"] = YamlNumbers::text(mesh.normAdjustWeight);

    // Encode morph weights: only the mesh's own (MeshComponent::morphWeightsSet, or a weight set
    // before any load): a mesh saved before its model ever loaded gets the file's defaults then,
    // and the target count the weights cover lets targets a re-export adds get theirs
    bool anyWeight = false;
    for (int i = 0; i < MAX_MORPHTARGETS; i++) anyWeight = anyWeight || mesh.morphWeights[i] != 0.0f;
    if (mesh.morphWeightsSet || anyWeight) {
        YAML::Node morphWeightsNode;
        morphWeightsNode.SetStyle(YAML::EmitterStyle::Flow);
        for(int i = 0; i < MAX_MORPHTARGETS; i++) {
            morphWeightsNode[i] = YamlNumbers::text(mesh.morphWeights[i]);
        }
        node["morphWeights"] = morphWeightsNode;
        // the count only when it says more than the weights: without it, decode takes the count up
        // to the last non-zero weight (as older versions saved them), so a scene saved before the
        // count existed re-saves byte for byte
        const int count = mesh.morphWeightsSet ? mesh.morphTargetCount : morphWeightsPresentCount(mesh.morphWeights);
        if (count != morphWeightsPresentCount(mesh.morphWeights)) node["morphTargetCount"] = count;
    }

    // Encode AABBs
    if (encodeGeneratedValues) {
        node["aabb"] = encodeAABB(mesh.aabb);
        node["verticesAABB"] = encodeAABB(mesh.verticesAABB);
    }
    // computed from the other two and the transform when the scene runs
    if (reflect::encodesComputedValues()) {
        node["worldAABB"] = encodeAABB(mesh.worldAABB);
    }

    node["receiveLights"] = YamlNumbers::text(mesh.receiveLights);
    node["receiveIBL"] = YamlNumbers::text(mesh.receiveIBL);
    node["castShadows"] = YamlNumbers::text(mesh.castShadows);
    node["receiveShadows"] = YamlNumbers::text(mesh.receiveShadows);
    node["lodEnabled"] = YamlNumbers::text(mesh.lodEnabled);
    node["lodBias"] = YamlNumbers::text(mesh.lodBias);
    // the fade settings (spec S54 R9) only away from their defaults (MeshComponent's), so a
    // scene saved before them re-saves byte for byte
    if (mesh.lodFadeMode != LodFadeMode::CROSS_FADE)
        node["lodFadeMode"] = StreamSchema::lodFadeMode().toString(mesh.lodFadeMode);
    if (!mesh.lodAnimateCrossFading)
        node["lodAnimateCrossFading"] = YamlNumbers::text(mesh.lodAnimateCrossFading);
    if (mesh.lodFadeTransitionWidth != 0.1f)
        node["lodFadeTransitionWidth"] = YamlNumbers::text(mesh.lodFadeTransitionWidth);
    if (mesh.lodCullBelow != 0.0f)
        node["lodCullBelow"] = YamlNumbers::text(mesh.lodCullBelow);
    node["renderInReflectionProbes"] = YamlNumbers::text(mesh.renderInReflectionProbes);
    node["shadowsBillboard"] = YamlNumbers::text(mesh.shadowsBillboard);
    if (mesh.staticShadowCaster)
        node["staticShadowCaster"] = YamlNumbers::text(mesh.staticShadowCaster);
    // (S55) written when off
    if (!mesh.allowGpuInstancing)
        node["allowGpuInstancing"] = YamlNumbers::text(mesh.allowGpuInstancing);
    node["transparent"] = YamlNumbers::text(mesh.transparent);
    node["autoTransparency"] = YamlNumbers::text(mesh.autoTransparency);

    node["cullingMode"] = cullingModeToString(mesh.cullingMode);
    node["windingOrder"] = windingOrderToString(mesh.windingOrder);
    if (mesh.mirroredWinding)
        node["mirroredWinding"] = true;

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
        mesh.numExternalBuffers = 0;
        for (unsigned int i = 0; i < eBuffersNode.size(); i++) {
            if (!mesh.eBuffers.validIndex(static_cast<int>(i))) {
                Out::error("A mesh's buffers past %zu are left out", ARRAY_INDEX_LIMIT);
                break;
            }
            decodeExternalBuffer(mesh.eBuffers[i], eBuffersNode[i]);
            mesh.numExternalBuffers = i + 1;
        }
    }

    // Decode submeshes
    if (node["submeshes"]) {
        auto submeshesNode = node["submeshes"];
        mesh.numSubmeshes = 0;
        for (unsigned int i = 0; i < submeshesNode.size(); i++) {
            if (!mesh.submeshes.validIndex(static_cast<int>(i))) {
                Out::error("A mesh's submeshes past %zu are left out", ARRAY_INDEX_LIMIT);
                break;
            }
            Submesh& submesh = mesh.submeshes[i];
            submesh = decodeSubmesh(submeshesNode[i], &submesh);
            mesh.numSubmeshes = i + 1;
        }
    }

    // Decode bones matrix
    if (node["bonesMatrix"]) {
        auto bonesNode = node["bonesMatrix"];
        mesh.bonesMatrix.clear();
        for (size_t i = 0; i < bonesNode.size() &&
                mesh.bonesMatrix.validIndex(static_cast<int>(i)); i++)
            mesh.bonesMatrix[i] = decodeMatrix4(bonesNode[i]);
        // an empty list is a trimmed one (all identity), which still counts as written
        mesh.needUpdateBones = true;
    }

    if (node["normAdjustJoint"]) mesh.normAdjustJoint = YamlNumbers::as<float>(node["normAdjustJoint"]);
    if (node["normAdjustWeight"]) mesh.normAdjustWeight = YamlNumbers::as<float>(node["normAdjustWeight"]);

    // Decode morph weights: saved weights are the mesh's own, a model (re)load keeps them
    // (MeshComponent::morphWeightsSet)
    if (node["morphWeights"]) {
        auto morphWeightsNode = node["morphWeights"];
        for(int i = 0; i < MAX_MORPHTARGETS; i++) {
            mesh.morphWeights[i] = YamlNumbers::as<float>(morphWeightsNode[i]);
        }
        mesh.morphWeightsSet = true;
        // (weights saved without their count, by an older version, cover up to their last non-zero
        // weight: MeshComponent::morphTargetCount)
        mesh.morphTargetCount = node["morphTargetCount"] ? std::clamp(node["morphTargetCount"].as<int>(), 0, MAX_MORPHTARGETS)
                                                         : morphWeightsPresentCount(mesh.morphWeights);
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
    // (omitted when at their defaults)
    mesh.lodFadeMode = node["lodFadeMode"] ? decodeEnum(node, "lodFadeMode", StreamSchema::lodFadeMode()) : LodFadeMode::CROSS_FADE;
    mesh.lodAnimateCrossFading = node["lodAnimateCrossFading"] ? YamlNumbers::as<bool>(node["lodAnimateCrossFading"]) : true;
    mesh.lodFadeTransitionWidth = std::clamp(decodeFinite(node["lodFadeTransitionWidth"], 0.1f), 0.0f, 1.0f);
    mesh.lodCullBelow = std::clamp(decodeFinite(node["lodCullBelow"], 0.0f), 0.0f, 1.0f);
    if (node["renderInReflectionProbes"]) mesh.renderInReflectionProbes = YamlNumbers::as<bool>(node["renderInReflectionProbes"]);
    if (node["shadowsBillboard"]) mesh.shadowsBillboard = YamlNumbers::as<bool>(node["shadowsBillboard"]);
    // omitted when false
    mesh.staticShadowCaster = node["staticShadowCaster"] ? YamlNumbers::as<bool>(node["staticShadowCaster"]) : false;
    // omitted when true (scenes from before S55 allow it)
    mesh.allowGpuInstancing = node["allowGpuInstancing"] ? YamlNumbers::as<bool>(node["allowGpuInstancing"]) : true;
    if (node["transparent"]) mesh.transparent = YamlNumbers::as<bool>(node["transparent"]);
    if (node["autoTransparency"]) mesh.autoTransparency = YamlNumbers::as<bool>(node["autoTransparency"]);

    if (node["cullingMode"]) mesh.cullingMode = decodeEnum(node, "cullingMode", StreamSchema::cullingMode());
    if (node["windingOrder"]) mesh.windingOrder = decodeEnum(node, "windingOrder", StreamSchema::windingOrder());
    if (node["windingOrder"]) mesh.mirroredWinding = node["mirroredWinding"] && YamlNumbers::as<bool>(node["mirroredWinding"]);

    if (node["customShader"]) mesh.customShader = decodeShaderRef(node, "customShader");
    if (node["customDepthShader"]) mesh.customDepthShader = decodeShaderRef(node, "customDepthShader");
    mesh.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    mesh.needUpdateShaderUniforms = true;

    if (node["enabled"]) mesh.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return mesh;
}

YAML::Node editor::Stream::encodeAtlasSampler(const Texture& texture) {
    YAML::Node node;
    const Texture defaults;
    const bool samplerSet = texture.getMinFilter() != defaults.getMinFilter() || texture.getMagFilter() != defaults.getMagFilter() ||
        texture.getWrapU() != defaults.getWrapU() || texture.getWrapV() != defaults.getWrapV() ||
        texture.getAlphaBorder() != defaults.getAlphaBorder();
    if (!samplerSet)
        return node;
    // the keys and order of encodeTexture's sampler part
    node["minFilter"] = textureFilterToString(texture.getMinFilter());
    node["magFilter"] = textureFilterToString(texture.getMagFilter());
    node["wrapU"] = textureWrapToString(texture.getWrapU());
    node["wrapV"] = textureWrapToString(texture.getWrapV());
    if (texture.getAlphaBorder() != TextureAlphaBorder::AUTO) node["alphaBorder"] = textureAlphaBorderToString(texture.getAlphaBorder());
    return node;
}

void editor::Stream::decodeAtlasSampler(const YAML::Node& node, Texture& texture) {
    // a key left out (or a null node) is a new texture's value
    const Texture defaults;
    const bool map = node.IsMap();
    texture.setMinFilter(map && node["minFilter"] ? stringToTextureFilter(node["minFilter"].as<std::string>()) : defaults.getMinFilter());
    texture.setMagFilter(map && node["magFilter"] ? stringToTextureFilter(node["magFilter"].as<std::string>()) : defaults.getMagFilter());
    texture.setWrapU(map && node["wrapU"] ? stringToTextureWrap(node["wrapU"].as<std::string>()) : defaults.getWrapU());
    texture.setWrapV(map && node["wrapV"] ? stringToTextureWrap(node["wrapV"].as<std::string>()) : defaults.getWrapV());
    texture.setAlphaBorder(map && node["alphaBorder"] ? stringToTextureAlphaBorder(node["alphaBorder"].as<std::string>()) : defaults.getAlphaBorder());
}

YAML::Node editor::Stream::encodeUIComponent(const UIComponent& ui, bool encodeBuffers, bool embedTextureData, bool textureIsAtlas) {
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
    // computed from the aabb and the transform when the scene runs
    if (reflect::encodesComputedValues()) {
        node["worldAABB"] = encodeAABB(ui.worldAABB);
    }

    // A Text's texture is its font atlas, published by the UISystem once the glyphs are
    // rasterized, under a new id at each atlas change: the file does not depend on whether
    // the Text was drawn
    node["texture"] = textureIsAtlas ? encodeAtlasSampler(ui.texture) : encodeTexture(ui.texture, embedTextureData);
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
    if (!ui.shaderGlobalBindings.empty())
        node["shaderGlobalBindings"] = encodeShaderGlobalBindings(ui.shaderGlobalBindings);
    encodeComponentShaderLibrary(node, ui.shaderTextures, ui.shaderFeatures, &ui.blendMode, &ui.depthWrite, embedTextureData);
    encodeStencilAndColorWrite(node, ui.stencil, ui.colorWrite);

    return node;
}

UIComponent editor::Stream::decodeUIComponent(const YAML::Node& node, const UIComponent* oldUI, bool textureIsAtlas) {
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

    // A Text keeps the atlas it shows (the UISystem publishes it once built). An atlas id an older
    // editor saved after drawing ("font|20#7") named an atlas of that run and is not read, only
    // the sampler settings beside it
    if (node["texture"]) {
        if (textureIsAtlas) decodeAtlasSampler(node["texture"], ui.texture);
        else ui.texture = decodeTexture(node["texture"]);
    }
    if (node["color"]) ui.color = decodeVector4(node["color"]);

    if (node["automaticFlipY"]) ui.automaticFlipY = YamlNumbers::as<bool>(node["automaticFlipY"]);
    if (node["flipY"]) ui.flipY = YamlNumbers::as<bool>(node["flipY"]);

    if (node["pointerMoved"]) ui.pointerMoved = YamlNumbers::as<bool>(node["pointerMoved"]);
    if (node["focused"]) ui.focused = YamlNumbers::as<bool>(node["focused"]);

    if (node["customShader"]) ui.customShader = decodeShaderRef(node, "customShader");
    ui.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    ui.shaderGlobalBindings = decodeShaderGlobalBindings(node["shaderGlobalBindings"]);
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
            for (std::size_t i = 0; i < framesNode.size(); i++) {
                if (!framesNode[i] || framesNode[i].IsNull()) {
                    continue;
                }
                if (!sprite.framesRect.validIndex(static_cast<int>(sprite.numFramesRect))) {
                    Out::error("A Sprite's frames past %zu are left out", ARRAY_INDEX_LIMIT);
                    break;
                }
                sprite.framesRect[sprite.numFramesRect] = decodeSpriteFrameData(framesNode[i]);
                sprite.numFramesRect++;
            }
            sprite.framesRect.resize(sprite.numFramesRect);
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
            for (std::size_t i = 0; i < tilesRectNode.size(); i++) {
                if (!tilesRectNode[i] || tilesRectNode[i].IsNull()) {
                    continue;
                }
                if (!tilemap.tilesRect.validIndex(static_cast<int>(tilemap.numTilesRect))) {
                    Out::error("A Tilemap's tile rects past %zu are left out", ARRAY_INDEX_LIMIT);
                    break;
                }
                tilemap.tilesRect[tilemap.numTilesRect] = decodeTileRectData(tilesRectNode[i]);
                tilemap.numTilesRect++;
            }
            tilemap.tilesRect.resize(tilemap.numTilesRect);
        }
    }

    if (node["tiles"]) {
        const YAML::Node& tilesNode = node["tiles"];
        if (tilesNode.IsSequence()) {
            tilemap.numTiles = 0;
            for (std::size_t i = 0; i < tilesNode.size(); i++) {
                if (!tilesNode[i] || tilesNode[i].IsNull()) {
                    continue;
                }
                if (!tilemap.tiles.validIndex(static_cast<int>(tilemap.numTiles))) {
                    Out::error("A Tilemap's tiles past %zu are left out", ARRAY_INDEX_LIMIT);
                    break;
                }
                tilemap.tiles[tilemap.numTiles] = decodeTileData(tilesNode[i]);
                tilemap.numTiles++;
            }
            tilemap.tiles.resize(tilemap.numTiles);
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
        if (!value.empty() || hasTexturePath(value)) node[name] = encodeTexture(value);
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

    // Auto Ranges: set from the camera's far clip when the terrain runs (MeshSystem::updateTerrainAutoRanges)
    if (!terrain.ranges.empty() && (!terrain.autoSetRanges || reflect::encodesComputedValues())) {
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

    MeshSystem::applyTerrainMapSettings(terrain);

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
    // written only when off (an Animation Player plays the model's clips, 9.R15), so files with
    // models save as before; decoding reads a missing key as on, never as the old value
    if (!model.animationEntities) node["animationEntities"] = false;

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
    // absent (or emptied by a paste over a Model that had it) is the default, on
    model.animationEntities = true;
    if (const YAML::Node value = node["animationEntities"]; value && value.IsScalar() && !value.Scalar().empty()) {
        bool flag = true;
        if (YamlNumbers::readPlain(value.Scalar(), flag)) {
            model.animationEntities = flag;
        } else {
            try {
                model.animationEntities = value.as<bool>();
            } catch (const YAML::Exception&) {
                model.animationEntities = true;
            }
        }
    }
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
    if (!sky.shaderGlobalBindings.empty())
        node["shaderGlobalBindings"] = encodeShaderGlobalBindings(sky.shaderGlobalBindings);
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
    sky.shaderGlobalBindings = decodeShaderGlobalBindings(node["shaderGlobalBindings"]);
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

namespace {

    std::string collisionDetection2DToString(CollisionDetectionMode2D mode){
        return mode == CollisionDetectionMode2D::CONTINUOUS ? "continuous" : "discrete";
    }

    CollisionDetectionMode2D stringToCollisionDetection2D(const std::string& text){
        return text == "continuous" ? CollisionDetectionMode2D::CONTINUOUS : CollisionDetectionMode2D::DISCRETE;
    }

    std::string sleepMode2DToString(RigidbodySleepMode2D mode){
        switch (mode){
            case RigidbodySleepMode2D::NEVER_SLEEP: return "never_sleep";
            case RigidbodySleepMode2D::START_ASLEEP: return "start_asleep";
            default: return "start_awake";
        }
    }

    RigidbodySleepMode2D stringToSleepMode2D(const std::string& text){
        if (text == "never_sleep") return RigidbodySleepMode2D::NEVER_SLEEP;
        if (text == "start_asleep") return RigidbodySleepMode2D::START_ASLEEP;
        return RigidbodySleepMode2D::START_AWAKE;
    }

}

YAML::Node editor::Stream::encodeRigidbody2DComponent(const Rigidbody2DComponent& body) {
    YAML::Node node;
    node["enabled"] = body.enabled;

    node["type"] = bodyTypeToString(body.type);
    // still written for editors from before colliders had no count limit
    node["numColliders"] = static_cast<unsigned int>(body.colliders.size());

    node["gravityScale"] = body.gravityScale;
    node["linearDamping"] = body.linearDamping;
    node["angularDamping"] = body.angularDamping;
    node["fixedRotation"] = body.fixedRotation;
    node["collisionDetection"] = collisionDetection2DToString(body.collisionDetection);
    node["sleepingMode"] = sleepMode2DToString(body.sleepingMode);
    node["useAutoMass"] = body.useAutoMass;
    node["mass"] = body.mass;
    node["autoCenterOfMass"] = body.autoCenterOfMass;
    node["centerOfMass"] = encodeVector2(body.centerOfMass);
    node["autoInertia"] = body.autoInertia;
    node["inertia"] = body.inertia;
    node["material"] = body.material;

    YAML::Node shapesNode;
    for (const Collider2D& collider : body.colliders) {
        YAML::Node shapeNode;
        shapeNode["type"] = collider2DTypeToString(collider.type);
        shapeNode["pointA"] = encodeVector2(collider.pointA);
        shapeNode["pointB"] = encodeVector2(collider.pointB);
        shapeNode["radius"] = collider.radius;
        shapeNode["loop"] = collider.loop;

        shapeNode["density"] = collider.density;
        shapeNode["friction"] = collider.friction;
        shapeNode["restitution"] = collider.restitution;
        if (!collider.material.empty()) {
            shapeNode["material"] = collider.material;
        }
        shapeNode["sensor"] = collider.sensor;
        shapeNode["enableHitEvents"] = collider.enableHitEvents;
        shapeNode["contactEvents"] = collider.contactEvents;
        shapeNode["preSolveEvents"] = collider.preSolveEvents;
        shapeNode["sensorEvents"] = collider.sensorEvents;
        shapeNode["categoryBits"] = collider.categoryBits;
        shapeNode["maskBits"] = collider.maskBits;
        shapeNode["groupIndex"] = collider.groupIndex;
        if (collider.receiver != NULL_ENTITY) {
            shapeNode["receiver"] = collider.receiver;
        }

        YAML::Node pointsNode;
        for (const Vector2& point : collider.points) {
            pointsNode.push_back(encodeVector2(point));
        }
        shapeNode["points"] = pointsNode;

        shapesNode.push_back(shapeNode);
    }
    node["colliders"] = shapesNode;

    return node;
}

Rigidbody2DComponent editor::Stream::decodeRigidbody2DComponent(const YAML::Node& node, const Rigidbody2DComponent* oldBody) {
    Rigidbody2DComponent body;

    if (oldBody) {
        body = *oldBody;
        // a script's material objects (runtime only) do not outlive what is decoded over them
        body.materialInstance.reset();
        body.appliedMaterialRevision = 0;
    }

    if (node["type"]) body.type = decodeEnum(node, "type", StreamSchema::bodyType());

    if (node["gravityScale"]) body.gravityScale = YamlNumbers::as<float>(node["gravityScale"]);
    if (node["linearDamping"]) body.linearDamping = YamlNumbers::as<float>(node["linearDamping"]);
    if (node["angularDamping"]) body.angularDamping = YamlNumbers::as<float>(node["angularDamping"]);
    if (node["fixedRotation"]) body.fixedRotation = YamlNumbers::as<bool>(node["fixedRotation"]);
    if (node["collisionDetection"]) body.collisionDetection = stringToCollisionDetection2D(node["collisionDetection"].as<std::string>());
    if (node["sleepingMode"]) body.sleepingMode = stringToSleepMode2D(node["sleepingMode"].as<std::string>());
    if (node["useAutoMass"]) body.useAutoMass = YamlNumbers::as<bool>(node["useAutoMass"]);
    if (node["mass"]) body.mass = YamlNumbers::as<float>(node["mass"]);
    if (node["autoCenterOfMass"]) body.autoCenterOfMass = YamlNumbers::as<bool>(node["autoCenterOfMass"]);
    if (node["centerOfMass"]) body.centerOfMass = decodeVector2(node["centerOfMass"]);
    if (node["autoInertia"]) body.autoInertia = YamlNumbers::as<bool>(node["autoInertia"]);
    if (node["inertia"]) body.inertia = YamlNumbers::as<float>(node["inertia"]);
    // keys written only when set: absent is the default, also over an existing component
    body.material = node["material"] ? node["material"].as<std::string>() : std::string();

    if (node["colliders"]) {
        const YAML::Node collidersNode = node["colliders"];
        size_t count = collidersNode.size();
        // files of older editors also say how many colliders count
        if (node["numColliders"]) {
            count = std::min(count, (size_t)YamlNumbers::as<unsigned int>(node["numColliders"]));
        }

        const size_t previousCount = body.colliders.size();
        body.colliders.resize(count);
        for (size_t i = 0; i < count; i++) {
            const YAML::Node shapeNode = collidersNode[i];
            Collider2D& collider = body.colliders[i];
            // a collider new to the component: keys missing from files of before default as
            // they did then (contact events off)
            if (i >= previousCount) {
                collider.contactEvents = false;
            }

            if (shapeNode["type"]) {
                collider.type = decodeEnum(shapeNode, "type", StreamSchema::collider2DType());
            }
            if (shapeNode["pointA"]) {
                collider.pointA = decodeVector2(shapeNode["pointA"]);
            }
            if (shapeNode["pointB"]) {
                collider.pointB = decodeVector2(shapeNode["pointB"]);
            }
            if (shapeNode["radius"]) collider.radius = YamlNumbers::as<float>(shapeNode["radius"]);
            if (shapeNode["loop"]) collider.loop = YamlNumbers::as<bool>(shapeNode["loop"]);

            if (shapeNode["density"]) collider.density = YamlNumbers::as<float>(shapeNode["density"]);
            if (shapeNode["friction"]) collider.friction = YamlNumbers::as<float>(shapeNode["friction"]);
            if (shapeNode["restitution"]) collider.restitution = YamlNumbers::as<float>(shapeNode["restitution"]);
            collider.material = shapeNode["material"] ? shapeNode["material"].as<std::string>() : std::string();
            collider.materialInstance.reset();
            collider.ownsMaterialInstance = false;
            if (shapeNode["sensor"]) collider.sensor = YamlNumbers::as<bool>(shapeNode["sensor"]);
            if (shapeNode["enableHitEvents"]) collider.enableHitEvents = YamlNumbers::as<bool>(shapeNode["enableHitEvents"]);
            if (shapeNode["contactEvents"]) collider.contactEvents = YamlNumbers::as<bool>(shapeNode["contactEvents"]);
            if (shapeNode["preSolveEvents"]) collider.preSolveEvents = YamlNumbers::as<bool>(shapeNode["preSolveEvents"]);
            if (shapeNode["sensorEvents"]) collider.sensorEvents = YamlNumbers::as<bool>(shapeNode["sensorEvents"]);
            if (shapeNode["categoryBits"]) collider.categoryBits = YamlNumbers::as<uint16_t>(shapeNode["categoryBits"]);
            if (shapeNode["maskBits"]) collider.maskBits = YamlNumbers::as<uint16_t>(shapeNode["maskBits"]);
            if (shapeNode["groupIndex"]) collider.groupIndex = YamlNumbers::as<int16_t>(shapeNode["groupIndex"]);
            collider.receiver = shapeNode["receiver"] ? YamlNumbers::as<Entity>(shapeNode["receiver"]) : NULL_ENTITY;

            // "vertices": files of before
            const YAML::Node pointsNode = shapeNode["points"] ? shapeNode["points"] : shapeNode["vertices"];
            if (pointsNode) {
                collider.points.resize(pointsNode.size());
                for (size_t j = 0; j < pointsNode.size(); j++) {
                    collider.points[j] = decodeVector2(pointsNode[j]);
                }
            }

            collider.shapes.clear();
            collider.chain = b2_nullChainId;
            collider.dirty = true;
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
    node["material"] = body.material;
    node["linearDamping"] = body.linearDamping;
    node["angularDamping"] = body.angularDamping;
    node["maxLinearVelocity"] = body.maxLinearVelocity;
    node["maxAngularVelocity"] = body.maxAngularVelocity;
    node["automaticMass"] = body.automaticMass;
    node["mass"] = body.mass;
    node["automaticCenterOfMass"] = body.automaticCenterOfMass;
    node["centerOfMass"] = encodeVector3(body.centerOfMass);
    node["automaticInertiaTensor"] = body.automaticInertiaTensor;
    node["inertiaTensor"] = encodeVector3(body.inertiaTensor);
    node["inertiaTensorRotation"] = encodeQuaternion(body.inertiaTensorRotation);
    node["allowSleeping"] = body.allowSleeping;
    node["collideKinematicVsNonDynamic"] = body.collideKinematicVsNonDynamic;
    node["interpolation"] = rigidbodyInterpolationToString(body.interpolation);
    if (body.overrideMassProperties) {
        node["overrideMassProperties"] = true;
        node["solidBoxSize"] = encodeVector3(body.solidBoxSize);
        node["solidBoxDensity"] = body.solidBoxDensity;
    }

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

    // read by editors from before the list had no cap
    node["numColliders"] = static_cast<unsigned int>(body.colliders.size());

    YAML::Node shapesNode = YAML::Node(YAML::NodeType::Sequence);
    for (const Collider3D& collider : body.colliders) {
        YAML::Node shapeNode;
        shapeNode["type"] = collider3DTypeToString(collider.type);
        shapeNode["position"] = encodeVector3(collider.position);
        shapeNode["rotation"] = encodeQuaternion(collider.rotation);
        shapeNode["width"] = collider.width;
        shapeNode["height"] = collider.height;
        shapeNode["depth"] = collider.depth;
        shapeNode["radius"] = collider.radius;
        shapeNode["halfHeight"] = collider.halfHeight;
        shapeNode["topRadius"] = collider.topRadius;
        shapeNode["bottomRadius"] = collider.bottomRadius;
        shapeNode["density"] = collider.density;

        switch (collider.source) {
            case Collider3DSource::RAW_VERTICES: shapeNode["source"] = "raw_vertices"; break;
            case Collider3DSource::RAW_MESH: shapeNode["source"] = "raw_mesh"; break;
            case Collider3DSource::ENTITY_MESH: shapeNode["source"] = "entity_mesh"; break;
            case Collider3DSource::ENTITY_HEIGHTFIELD: shapeNode["source"] = "entity_heightfield"; break;
            default: shapeNode["source"] = "none"; break;
        }

        shapeNode["sourceEntity"] = collider.sourceEntity;
        shapeNode["samplesSize"] = collider.samplesSize;
        shapeNode["isTrigger"] = collider.isTrigger;
        if (!collider.material.empty()) {
            shapeNode["material"] = collider.material;
        }
        if (collider.receiver != NULL_ENTITY) {
            shapeNode["receiver"] = collider.receiver;
        }

        YAML::Node verticesNode = YAML::Node(YAML::NodeType::Sequence);
        for (const Vector3& vertex : collider.vertices) {
            verticesNode.push_back(encodeVector3(vertex));
        }
        shapeNode["vertices"] = verticesNode;

        YAML::Node indicesNode = YAML::Node(YAML::NodeType::Sequence);
        for (uint32_t index : collider.indices) {
            indicesNode.push_back(index);
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
        // keys left out at their default decode to it over a live component too (Stop's restore)
        body.material.clear();
        body.materialInstance.reset();
        body.overrideMassProperties = false;
        body.hasPendingType = false;
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
    // the Rigidbody fields; a scene saved before them gets what Jolt used (the struct defaults)
    if (node["material"]) body.material = node["material"].as<std::string>();
    if (node["linearDamping"]) body.linearDamping = YamlNumbers::as<float>(node["linearDamping"]);
    if (node["angularDamping"]) body.angularDamping = YamlNumbers::as<float>(node["angularDamping"]);
    if (node["maxLinearVelocity"]) body.maxLinearVelocity = YamlNumbers::as<float>(node["maxLinearVelocity"]);
    if (node["maxAngularVelocity"]) body.maxAngularVelocity = YamlNumbers::as<float>(node["maxAngularVelocity"]);
    if (node["automaticMass"]) body.automaticMass = YamlNumbers::as<bool>(node["automaticMass"]);
    if (node["mass"]) body.mass = YamlNumbers::as<float>(node["mass"]);
    if (node["automaticCenterOfMass"]) body.automaticCenterOfMass = YamlNumbers::as<bool>(node["automaticCenterOfMass"]);
    if (node["centerOfMass"]) body.centerOfMass = decodeVector3(node["centerOfMass"]);
    if (node["automaticInertiaTensor"]) body.automaticInertiaTensor = YamlNumbers::as<bool>(node["automaticInertiaTensor"]);
    if (node["inertiaTensor"]) body.inertiaTensor = decodeVector3(node["inertiaTensor"]);
    if (node["inertiaTensorRotation"]) body.inertiaTensorRotation = decodeQuaternion(node["inertiaTensorRotation"]);
    if (node["allowSleeping"]) body.allowSleeping = YamlNumbers::as<bool>(node["allowSleeping"]);
    if (node["collideKinematicVsNonDynamic"]) body.collideKinematicVsNonDynamic = YamlNumbers::as<bool>(node["collideKinematicVsNonDynamic"]);
    if (node["interpolation"]) body.interpolation = stringToRigidbodyInterpolation(node["interpolation"].as<std::string>());
    if (node["overrideMassProperties"]) body.overrideMassProperties = YamlNumbers::as<bool>(node["overrideMassProperties"]);
    if (node["solidBoxSize"]) body.solidBoxSize = decodeVector3(node["solidBoxSize"]);
    if (node["solidBoxDensity"]) body.solidBoxDensity = YamlNumbers::as<float>(node["solidBoxDensity"]);

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

    if (node["colliders"]) {
        const YAML::Node& collidersNode = node["colliders"];
        size_t count = collidersNode.size();
        // an older editor's count of the list (it kept unused slots)
        if (node["numColliders"]) {
            count = std::min(count, (size_t)YamlNumbers::as<unsigned int>(node["numColliders"]));
        }
        body.colliders.clear();
        body.colliders.resize(count);
        for (size_t i = 0; i < count; i++) {
            const YAML::Node& shapeNode = collidersNode[i];
            Collider3D& collider = body.colliders[i];
            if (shapeNode["type"]) collider.type = decodeEnum(shapeNode, "type", StreamSchema::collider3DType());
            if (shapeNode["position"]) {
                collider.position = decodeVector3(shapeNode["position"]);

                const float physicsOffset = (collider.position * bodyScale).length();
                if (count == 1 && physicsOffset > MAX_SINGLE_SHAPE_PHYSICS_OFFSET) {
                    Log::warn("Rigidbody shape position after transform scale is too large (%.2f). Physics may be unstable.", physicsOffset);
                }
            }
            if (shapeNode["rotation"]) collider.rotation = decodeQuaternion(shapeNode["rotation"]);
            if (shapeNode["width"]) collider.width = YamlNumbers::as<float>(shapeNode["width"]);
            if (shapeNode["height"]) collider.height = YamlNumbers::as<float>(shapeNode["height"]);
            if (shapeNode["depth"]) collider.depth = YamlNumbers::as<float>(shapeNode["depth"]);
            if (shapeNode["radius"]) collider.radius = YamlNumbers::as<float>(shapeNode["radius"]);
            if (shapeNode["halfHeight"]) collider.halfHeight = YamlNumbers::as<float>(shapeNode["halfHeight"]);
            if (shapeNode["topRadius"]) collider.topRadius = YamlNumbers::as<float>(shapeNode["topRadius"]);
            if (shapeNode["bottomRadius"]) collider.bottomRadius = YamlNumbers::as<float>(shapeNode["bottomRadius"]);
            if (shapeNode["density"]) collider.density = YamlNumbers::as<float>(shapeNode["density"]);

            if (shapeNode["source"]) {
                std::string source = shapeNode["source"].as<std::string>();
                if (source == "raw_vertices") collider.source = Collider3DSource::RAW_VERTICES;
                else if (source == "raw_mesh") collider.source = Collider3DSource::RAW_MESH;
                else if (source == "entity_mesh") collider.source = Collider3DSource::ENTITY_MESH;
                else if (source == "entity_heightfield") collider.source = Collider3DSource::ENTITY_HEIGHTFIELD;
                else collider.source = Collider3DSource::NONE;
            }

            if (collider.source == Collider3DSource::NONE) {
                if (collider.type == Collider3DType::CONVEX_HULL) {
                    collider.source = Collider3DSource::ENTITY_MESH;
                } else if (collider.type == Collider3DType::MESH) {
                    collider.source = Collider3DSource::ENTITY_MESH;
                } else if (collider.type == Collider3DType::HEIGHTFIELD) {
                    collider.source = Collider3DSource::ENTITY_HEIGHTFIELD;
                }
            }

            if (shapeNode["sourceEntity"]) collider.sourceEntity = YamlNumbers::as<Entity>(shapeNode["sourceEntity"]);
            if (shapeNode["samplesSize"]) collider.samplesSize = YamlNumbers::as<unsigned int>(shapeNode["samplesSize"]);
            if (shapeNode["isTrigger"]) collider.isTrigger = YamlNumbers::as<bool>(shapeNode["isTrigger"]);
            if (shapeNode["material"]) collider.material = shapeNode["material"].as<std::string>();
            if (shapeNode["receiver"]) collider.receiver = YamlNumbers::as<Entity>(shapeNode["receiver"]);

            if (shapeNode["vertices"]) {
                for (const YAML::Node& vertex : shapeNode["vertices"]) {
                    collider.vertices.push_back(decodeVector3(vertex));
                }
            }
            if (shapeNode["indices"]) {
                for (const YAML::Node& index : shapeNode["indices"]) {
                    collider.indices.push_back(YamlNumbers::as<uint32_t>(index));
                }
            }
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
            if (!spriteanim.frames.validIndex(spriteanim.framesSize)) {
                Out::error("A Sprite Animation's frames past %zu are left out", ARRAY_INDEX_LIMIT);
                break;
            }
            spriteanim.frames[spriteanim.framesSize] = YamlNumbers::as<int>(frameNode);
            spriteanim.framesSize++;
        }
    }

    if (node["framesTime"]) {
        spriteanim.framesTimeSize = 0;
        for (const YAML::Node& timeNode : node["framesTime"]) {
            if (!spriteanim.framesTime.validIndex(spriteanim.framesTimeSize)) {
                Out::error("A Sprite Animation's frame times past %zu are left out", ARRAY_INDEX_LIMIT);
                break;
            }
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
    // written only when on, so a file from before it stays as it was
    if (particles.useUnscaledTime) node["useUnscaledTime"] = true;
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
    // absent: off, over an old component too (it is written only when on)
    particles.useUnscaledTime = node["useUnscaledTime"] ? YamlNumbers::as<bool>(node["useUnscaledTime"]) : false;
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
    // [time, value, inTangent, outTangent(, mode)] per key, a step tangent ~ (the one curve
    // encoding, stream/CurveCodec.h; older files wrote it .inf, read the same)
    node["falloff"] = curvecodec::curveToYaml(field.falloff);
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
    if (node["falloff"]){
        Curve falloff;
        std::string reason;
        if (curvecodec::curveFromYaml(node["falloff"], falloff, reason)) field.falloff = std::move(falloff);
        else Out::warning("Force Field falloff not read (%s): the curve is kept", reason.c_str());
    }
    if (node["layers"]) field.layers = YamlNumbers::as<uint32_t>(node["layers"]);
    return field;
}

YAML::Node editor::Stream::encodeCharacterController2DComponent(const CharacterController2DComponent& controller) {
    YAML::Node node;
    node["enabled"] = controller.enabled;
    node["center"] = encodeVector2(controller.center);
    node["height"] = controller.height;
    node["radius"] = controller.radius;
    node["slopeLimit"] = controller.slopeLimit;
    node["stepOffset"] = controller.stepOffset;
    node["skinWidth"] = controller.skinWidth;
    node["minMoveDistance"] = controller.minMoveDistance;
    node["pushStrength"] = controller.pushStrength;
    node["pushBodies"] = controller.pushBodies;
    node["movingPlatforms"] = controller.movingPlatforms;
    node["categoryBits"] = controller.categoryBits;
    node["maskBits"] = controller.maskBits;
    return node;
}

CharacterController2DComponent editor::Stream::decodeCharacterController2DComponent(const YAML::Node& node, const CharacterController2DComponent* oldController) {
    // the runtime state (the last move and step) stays the live component's
    CharacterController2DComponent controller;
    if (oldController) {
        controller = *oldController;
    }
    if (node["enabled"]) controller.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["center"]) controller.center = decodeVector2(node["center"]);
    if (node["height"]) controller.height = YamlNumbers::as<float>(node["height"]);
    if (node["radius"]) controller.radius = YamlNumbers::as<float>(node["radius"]);
    if (node["slopeLimit"]) controller.slopeLimit = YamlNumbers::as<float>(node["slopeLimit"]);
    if (node["stepOffset"]) controller.stepOffset = YamlNumbers::as<float>(node["stepOffset"]);
    if (node["skinWidth"]) controller.skinWidth = YamlNumbers::as<float>(node["skinWidth"]);
    if (node["minMoveDistance"]) controller.minMoveDistance = YamlNumbers::as<float>(node["minMoveDistance"]);
    if (node["pushStrength"]) controller.pushStrength = YamlNumbers::as<float>(node["pushStrength"]);
    if (node["pushBodies"]) controller.pushBodies = YamlNumbers::as<bool>(node["pushBodies"]);
    if (node["movingPlatforms"]) controller.movingPlatforms = YamlNumbers::as<bool>(node["movingPlatforms"]);
    if (node["categoryBits"]) controller.categoryBits = YamlNumbers::as<uint16_t>(node["categoryBits"]);
    if (node["maskBits"]) controller.maskBits = YamlNumbers::as<uint16_t>(node["maskBits"]);
    return controller;
}

// =====================================================
// WaterVolumeComponent and BuoyancyComponent encode/decode
// =====================================================

YAML::Node editor::Stream::encodeWaterVolumeComponent(const WaterVolumeComponent& volume) {
    YAML::Node node;
    node["enabled"] = volume.enabled;
    node["size"] = encodeVector3(volume.size);
    node["surfaceOffset"] = volume.surfaceOffset;
    node["infinite"] = volume.infinite;
    node["density"] = volume.density;
    node["flow"] = encodeVector3(volume.flow);
    node["linearDrag"] = volume.linearDrag;
    node["angularDrag"] = volume.angularDrag;
    node["waves"] = StreamSchema::waterVolumeWaves().toString(volume.waves);
    return node;
}

WaterVolumeComponent editor::Stream::decodeWaterVolumeComponent(const YAML::Node& node, const WaterVolumeComponent* oldVolume) {
    WaterVolumeComponent volume;
    if (oldVolume) {
        volume = *oldVolume;
    }
    if (node["enabled"]) volume.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["size"]) volume.size = decodeVector3(node["size"]);
    if (node["surfaceOffset"]) volume.surfaceOffset = YamlNumbers::as<float>(node["surfaceOffset"]);
    if (node["infinite"]) volume.infinite = YamlNumbers::as<bool>(node["infinite"]);
    if (node["density"]) volume.density = YamlNumbers::as<float>(node["density"]);
    if (node["flow"]) volume.flow = decodeVector3(node["flow"]);
    if (node["linearDrag"]) volume.linearDrag = YamlNumbers::as<float>(node["linearDrag"]);
    if (node["angularDrag"]) volume.angularDrag = YamlNumbers::as<float>(node["angularDrag"]);
    if (node["waves"]) volume.waves = decodeEnum(node, "waves", StreamSchema::waterVolumeWaves());
    return volume;
}

YAML::Node editor::Stream::encodeBuoyancyComponent(const BuoyancyComponent& buoyancy) {
    YAML::Node node;
    node["enabled"] = buoyancy.enabled;
    node["columns"] = StreamSchema::buoyancyColumns().toString(buoyancy.columns);
    YAML::Node points(YAML::NodeType::Sequence);
    for (const Vector3& point : buoyancy.customPoints) {
        points.push_back(encodeVector3(point));
    }
    node["customPoints"] = points;
    node["volumeScale"] = buoyancy.volumeScale;
    node["dragScale"] = buoyancy.dragScale;
    return node;
}

BuoyancyComponent editor::Stream::decodeBuoyancyComponent(const YAML::Node& node, const BuoyancyComponent* oldBuoyancy) {
    BuoyancyComponent buoyancy;
    if (oldBuoyancy) {
        buoyancy = *oldBuoyancy;
    }
    if (node["enabled"]) buoyancy.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["columns"]) buoyancy.columns = decodeEnum(node, "columns", StreamSchema::buoyancyColumns());
    if (node["customPoints"] && node["customPoints"].IsSequence()) {
        buoyancy.customPoints.clear();
        for (const YAML::Node& point : node["customPoints"]) {
            buoyancy.customPoints.push_back(decodeVector3(point));
        }
    }
    if (node["volumeScale"]) buoyancy.volumeScale = YamlNumbers::as<float>(node["volumeScale"]);
    if (node["dragScale"]) buoyancy.dragScale = YamlNumbers::as<float>(node["dragScale"]);
    return buoyancy;
}

// =========================================
// CharacterControllerComponent encode/decode
// =========================================

YAML::Node editor::Stream::encodeCharacterControllerComponent(const CharacterControllerComponent& controller) {
    YAML::Node node;
    node["enabled"] = controller.enabled;
    node["center"] = encodeVector3(controller.center);
    node["height"] = controller.height;
    node["radius"] = controller.radius;
    node["slopeLimit"] = controller.slopeLimit;
    node["stepOffset"] = controller.stepOffset;
    node["skinWidth"] = controller.skinWidth;
    node["minMoveDistance"] = controller.minMoveDistance;
    node["mass"] = controller.mass;
    node["pushStrength"] = controller.pushStrength;
    node["pushBodies"] = controller.pushBodies;
    node["movingPlatforms"] = controller.movingPlatforms;
    node["inheritPlatformYaw"] = controller.inheritPlatformYaw;
    node["detectCollisions"] = controller.detectCollisions;
    node["characterCollisions"] = controller.characterCollisions;
    node["enhancedEdgeRemoval"] = controller.enhancedEdgeRemoval;
    node["categoryBits"] = controller.categoryBits;
    node["maskBits"] = controller.maskBits;
    return node;
}

CharacterControllerComponent editor::Stream::decodeCharacterControllerComponent(const YAML::Node& node, const CharacterControllerComponent* oldController) {
    // the runtime state (the last move and step, the character's handle) stays the live component's
    CharacterControllerComponent controller;
    if (oldController) {
        controller = *oldController;
    }
    if (node["enabled"]) controller.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["center"]) controller.center = decodeVector3(node["center"]);
    if (node["height"]) controller.height = YamlNumbers::as<float>(node["height"]);
    if (node["radius"]) controller.radius = YamlNumbers::as<float>(node["radius"]);
    if (node["slopeLimit"]) controller.slopeLimit = YamlNumbers::as<float>(node["slopeLimit"]);
    if (node["stepOffset"]) controller.stepOffset = YamlNumbers::as<float>(node["stepOffset"]);
    if (node["skinWidth"]) controller.skinWidth = YamlNumbers::as<float>(node["skinWidth"]);
    if (node["minMoveDistance"]) controller.minMoveDistance = YamlNumbers::as<float>(node["minMoveDistance"]);
    if (node["mass"]) controller.mass = YamlNumbers::as<float>(node["mass"]);
    if (node["pushStrength"]) controller.pushStrength = YamlNumbers::as<float>(node["pushStrength"]);
    if (node["pushBodies"]) controller.pushBodies = YamlNumbers::as<bool>(node["pushBodies"]);
    if (node["movingPlatforms"]) controller.movingPlatforms = YamlNumbers::as<bool>(node["movingPlatforms"]);
    if (node["inheritPlatformYaw"]) controller.inheritPlatformYaw = YamlNumbers::as<bool>(node["inheritPlatformYaw"]);
    if (node["detectCollisions"]) controller.detectCollisions = YamlNumbers::as<bool>(node["detectCollisions"]);
    if (node["characterCollisions"]) controller.characterCollisions = YamlNumbers::as<bool>(node["characterCollisions"]);
    if (node["enhancedEdgeRemoval"]) controller.enhancedEdgeRemoval = YamlNumbers::as<bool>(node["enhancedEdgeRemoval"]);
    if (node["categoryBits"]) controller.categoryBits = YamlNumbers::as<uint16_t>(node["categoryBits"]);
    if (node["maskBits"]) controller.maskBits = YamlNumbers::as<uint16_t>(node["maskBits"]);
    return controller;
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
    if (!points.shaderGlobalBindings.empty())
        node["shaderGlobalBindings"] = encodeShaderGlobalBindings(points.shaderGlobalBindings);
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
            if (!points.framesRect.validIndex(static_cast<int>(points.numFramesRect))) {
                Out::error("A Points' frames past %zu are left out", ARRAY_INDEX_LIMIT);
                break;
            }
            points.framesRect[points.numFramesRect] = decodeSpriteFrameData(frameNode);
            points.numFramesRect++;
        }
        points.framesRect.resize(points.numFramesRect);
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
    points.shaderGlobalBindings = decodeShaderGlobalBindings(node["shaderGlobalBindings"]);
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

    // the polyline, width and ribbon settings (lines and trails spec 6.7): written only away
    // from their defaults, so a scene without them is written as it always was
    encodeLineRibbonSettings(node, lines);

    if (!lines.customShader.empty())
        encodeShaderRef(node, "customShader", lines.customShader);
    if (!lines.shaderUniforms.empty())
        node["shaderUniforms"] = encodeShaderUniforms(lines.shaderUniforms);
    if (!lines.shaderGlobalBindings.empty())
        node["shaderGlobalBindings"] = encodeShaderGlobalBindings(lines.shaderGlobalBindings);
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

    // not merged: an absent key is the default (encode leaves defaults out)
    decodeLineRibbonSettings(node, lines);

    if (node["customShader"]) lines.customShader = decodeShaderRef(node, "customShader");
    lines.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    lines.shaderGlobalBindings = decodeShaderGlobalBindings(node["shaderGlobalBindings"]);
    lines.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, lines.shaderTextures, lines.shaderFeatures, &lines.blendMode, &lines.depthWrite);
    decodeStencilAndColorWrite(node, lines.stencil, lines.colorWrite);

    lines.needUpdateBuffer = false;
    lines.needUpdateRibbon = true;
    lines.polylineSegments.clear();
    if (oldLines && oldLines->loaded) {
        // decoded over a loaded line: the render system destroys its GPU objects and loads it
        // again (dropping them here would leak its buffers and pipelines)
        lines.needReload = true;
    } else {
        lines.loaded = false;
        lines.loadCalled = false;
        lines.needReload = false;
        lines.loadedRibbon = false;
        lines.loadedRibbonKey = 0;
        lines.ribbon = LineRibbonData();
    }

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

// PoseAnimatorComponent, AnimationPlayerComponent and VertexAnimatorComponent encode/decode

YAML::Node editor::Stream::encodePoseAnimatorComponent(const PoseAnimatorComponent& animator) {
    YAML::Node node;
    // written when empty too: decoding over a component with an animation must clear it
    encodeAssetRef(node, "animation", animator.animation);
    node["autoplay"] = animator.autoplay;
    encodeFinite(node, "speed", animator.speed);
    encodeFinite(node, "weight", animator.weight);
    return node;
}

PoseAnimatorComponent editor::Stream::decodePoseAnimatorComponent(const YAML::Node& node, const PoseAnimatorComponent* oldAnimator) {
    // the old one's runtime state (what it loaded, where it plays) stays while the
    // animation is the same: paste and play-stop restore decode over an existing component
    PoseAnimatorComponent animator;
    if (oldAnimator) {
        animator = *oldAnimator;
    }
    // callbacks are the running game's (scripts register them again): a snapshot or a copy
    // restored over the component must not bring back a released Lua function
    animator.onFinish.clear();

    // a missing key is the default, not the old value
    const PoseAnimatorComponent defaults;
    auto given = [&node](const char* key) {
        const YAML::Node value = node[key];
        return value && !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    };
    animator.animation = given("animation") ? decodeAssetRef(node, "animation") : AssetRef();
    animator.autoplay = given("autoplay") ? YamlNumbers::as<bool>(node["autoplay"]) : defaults.autoplay;
    animator.speed = given("speed") ? decodeFinite(node["speed"], defaults.speed) : defaults.speed;
    animator.weight = given("weight") ? std::clamp(decodeFinite(node["weight"], defaults.weight), 0.0f, 1.0f) : defaults.weight;

    if (oldAnimator && oldAnimator->animation != animator.animation) {
        animator.needsLoad = true;
    }

    return animator;
}

YAML::Node editor::Stream::encodeAnimationPlayerComponent(const AnimationPlayerComponent& player) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(player.enabled);
    // written when empty too: decoding over a player with clips must clear them
    YAML::Node clipsNode(YAML::NodeType::Sequence);
    for (const AnimationPlayerClip& entry : player.clips) {
        YAML::Node entryNode;
        entryNode["clip"] = entry.clip;
        if (!entry.guid.empty()) entryNode["guid"] = entry.guid;
        if (!entry.name.empty()) entryNode["name"] = entry.name;
        if (entry.wrap != ClipWrapMode::Default) entryNode["wrap"] = anim::wrapName(entry.wrap);
        if (entry.layer != 0) entryNode["layer"] = entry.layer;
        // humanoid playback (anim-retarget 7.1, on S21's one retarget path): written only when set
        if (entry.retarget) entryNode["retarget"] = YamlNumbers::text(true);
        if (entry.mirror) entryNode["mirror"] = YamlNumbers::text(true);
        if (entry.footIK) entryNode["footIK"] = YamlNumbers::text(true);
        clipsNode.push_back(entryNode);
    }
    node["clips"] = clipsNode;
    // layers (upstream anim layers spec 6.3): only when they differ from the default single Base
    // Layer (a missing key decodes to that default)
    const bool defaultLayers = player.layers.empty() ||
        (player.layers.size() == 1 && player.layers[0].name == anim::BASE_LAYER_NAME && player.layers[0].mask.empty() &&
         player.layers[0].maskGuid.empty() && player.layers[0].weight == 1.0f && player.layers[0].blend == AnimationLayerBlend::Override);
    if (!defaultLayers) {
        YAML::Node layersNode(YAML::NodeType::Sequence);
        for (const AnimationPlayerLayer& layer : player.layers) {
            YAML::Node layerNode;
            layerNode["name"] = layer.name;
            if (layer.weight != 1.0f) encodeFinite(layerNode, "weight", layer.weight);
            if (layer.blend != AnimationLayerBlend::Override) layerNode["blend"] = anim::layerBlendName(layer.blend);
            if (!layer.mask.empty()) layerNode["mask"] = layer.mask;
            if (!layer.maskGuid.empty()) layerNode["maskGuid"] = layer.maskGuid;
            layersNode.push_back(layerNode);
        }
        node["layers"] = layersNode;
    }
    node["defaultClip"] = player.defaultClip;
    node["playAutomatically"] = YamlNumbers::text(player.playAutomatically);
    encodeFinite(node, "speed", player.speed);
    encodeFinite(node, "defaultFadeTime", player.defaultFadeTime);
    return node;
}

AnimationPlayerComponent editor::Stream::decodeAnimationPlayerComponent(const YAML::Node& node, const AnimationPlayerComponent* oldPlayer) {
    // the old one's runtime (states, rest values, bindings) stays: paste and play-stop restore
    // decode over an existing component; a changed clip list reloads it (needsLoad)
    AnimationPlayerComponent player;
    if (oldPlayer) {
        player = *oldPlayer;
    }
    // callbacks are the running game's (scripts subscribe again): a snapshot or a copy restored
    // over the component must not bring back a released Lua function
    player.onEvent.clear();
    player.onFinish.clear();
    player.onStateEnter.clear();
    player.onStateExit.clear();

    // a missing key is the default, not the old value
    const AnimationPlayerComponent defaults;
    auto given = [&node](const char* key) {
        const YAML::Node value = node[key];
        return value && !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    };
    // a value of the wrong shape is the default (one bad key never fails the scene's load)
    auto boolean = [&node, &given](const char* key, bool fallback) {
        if (!given(key) || !node[key].IsScalar()) return fallback;
        bool value = fallback;
        if (YamlNumbers::readPlain(node[key].Scalar(), value)) return value;
        try {
            return node[key].as<bool>();
        } catch (const YAML::Exception&) {
            return fallback;
        }
    };
    player.enabled = boolean("enabled", defaults.enabled);

    std::vector<AnimationPlayerClip> clips;
    const YAML::Node clipsNode = node["clips"];
    if (clipsNode && clipsNode.IsSequence()) {
        for (const YAML::Node& entryNode : clipsNode) {
            AnimationPlayerClip entry;
            if (entryNode.IsScalar()) {
                // a plain path: {clip}
                entry.clip = entryNode.Scalar();
            } else if (entryNode.IsMap() && (!entryNode["clip"] || entryNode["clip"].IsScalar() || entryNode["clip"].IsNull())) {
                auto text = [&entryNode](const char* key) {
                    const YAML::Node value = entryNode[key];
                    return value && value.IsScalar() ? value.Scalar() : std::string();
                };
                entry.clip = text("clip");
                entry.guid = text("guid");
                entry.name = text("name");
                const std::string wrap = text("wrap");
                if (!wrap.empty() && !anim::wrapFromName(wrap, entry.wrap)) {
                    // once per (clip, wrap): paste, undo, prefab diffs and play startup decode it again
                    static std::mutex warnedMutex;
                    static std::set<std::string> warned;
                    std::lock_guard<std::mutex> lock(warnedMutex);
                    if (warned.insert(entry.clip + "\n" + wrap).second) {
                        Out::warning("Animation Player clip \"%s\": unknown wrap \"%s\" (default, once, loop, ping_pong, clamp_forever), using default",
                                     entry.clip.c_str(), wrap.c_str());
                    }
                    entry.wrap = ClipWrapMode::Default;
                }
                const YAML::Node layerNode = entryNode["layer"];
                if (layerNode && layerNode.IsScalar()) {
                    int layer = 0;
                    if (YamlNumbers::readPlain(layerNode.Scalar(), layer) && layer >= 0) entry.layer = layer;
                }
                auto flag = [&entryNode](const char* key) {
                    const YAML::Node value = entryNode[key];
                    if (!value || !value.IsScalar() || value.Scalar().empty()) return false;
                    bool result = false;
                    if (YamlNumbers::readPlain(value.Scalar(), result)) return result;
                    try {
                        return value.as<bool>();
                    } catch (const YAML::Exception&) {
                        return false;
                    }
                };
                entry.retarget = flag("retarget");
                entry.mirror = flag("mirror");
                entry.footIK = flag("footIK");
            } else {
                // a list, or a map whose clip is not a path
                continue;
            }
            clips.push_back(std::move(entry));
        }
    }
    player.clips = std::move(clips);

    // layers: a missing key is the default single Base Layer (empty)
    std::vector<AnimationPlayerLayer> layers;
    const YAML::Node layersNode = node["layers"];
    if (layersNode && layersNode.IsSequence()) {
        for (const YAML::Node& layerNode : layersNode) {
            AnimationPlayerLayer layer;
            if (layerNode.IsScalar()) {
                layer.name = layerNode.Scalar();
            } else if (layerNode.IsMap()) {
                auto text = [&layerNode](const char* key) {
                    const YAML::Node value = layerNode[key];
                    return value && value.IsScalar() ? value.Scalar() : std::string();
                };
                layer.name = text("name");
                if (layerNode["weight"] && layerNode["weight"].IsScalar()) {
                    layer.weight = std::clamp(decodeFinite(layerNode["weight"], 1.0f), 0.0f, 1.0f);
                }
                const std::string blend = text("blend");
                if (!blend.empty() && !anim::layerBlendFromName(blend, layer.blend)) {
                    Out::warning("Animation Player layer \"%s\": unknown blend \"%s\" (override, additive), using override", layer.name.c_str(), blend.c_str());
                    layer.blend = AnimationLayerBlend::Override;
                }
                layer.mask = text("mask");
                layer.maskGuid = text("maskGuid");
            } else {
                continue;
            }
            layers.push_back(std::move(layer));
        }
    }
    player.layers = std::move(layers);
    // upstream's controller of the player is an Animator's since the scene migration
    // (StreamMigrations: scene 5 -> 6, prefab 6 -> 7); one left in a newer file plays nothing
    if (given("controller")) {
        static std::mutex warnedMutex;
        static std::set<std::string> warned;
        const std::string controller = node["controller"].IsScalar() ? node["controller"].Scalar() : std::string();
        std::lock_guard<std::mutex> lock(warnedMutex);
        if (warned.insert(controller).second) {
            Out::warning("Animation Player: \"controller: %s\" is not read (an Animator plays controllers: add one with it)", controller.c_str());
        }
    }

    player.defaultClip = given("defaultClip") && node["defaultClip"].IsScalar() ? node["defaultClip"].Scalar() : std::string();
    player.playAutomatically = boolean("playAutomatically", defaults.playAutomatically);
    player.speed = given("speed") ? decodeFinite(node["speed"], defaults.speed) : defaults.speed;
    player.defaultFadeTime = given("defaultFadeTime") ? std::max(0.0f, decodeFinite(node["defaultFadeTime"], defaults.defaultFadeTime)) : defaults.defaultFadeTime;

    if (oldPlayer && (oldPlayer->clips != player.clips || oldPlayer->layers != player.layers)) {
        player.needsLoad = true;
    }

    return player;
}

YAML::Node editor::Stream::encodeVertexAnimatorComponent(const VertexAnimatorComponent& animator) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(animator.enabled);
    // written when empty too: decoding over an animator with an animation must clear it
    node["animation"] = animator.animation;
    if (!animator.guid.empty()) node["guid"] = animator.guid;
    node["defaultClip"] = animator.defaultClip;
    node["playAutomatically"] = YamlNumbers::text(animator.playAutomatically);
    encodeFinite(node, "speed", animator.speed);
    node["randomStart"] = YamlNumbers::text(animator.randomStart);
    encodeFinite(node, "defaultFadeTime", animator.defaultFadeTime);
    node["instanceEvents"] = YamlNumbers::text(animator.instanceEvents);
    YAML::Node attachmentsNode(YAML::NodeType::Sequence);
    for (const VertexAnimatorAttachment& attachment : animator.attachments) {
        YAML::Node attachmentNode;
        attachmentNode["socket"] = attachment.socket;
        attachmentNode["entity"] = attachment.entity;
        attachmentsNode.push_back(attachmentNode);
    }
    if (animator.attachments.empty()) attachmentsNode.SetStyle(YAML::EmitterStyle::Flow);
    node["attachments"] = attachmentsNode;
    // per-instance starts (Create Crowd): only when set, so other animators' YAML is unchanged
    if (!animator.instanceClips.empty()) {
        YAML::Node clipsNode(YAML::NodeType::Sequence);
        for (const std::string& clip : animator.instanceClips) clipsNode.push_back(clip);
        clipsNode.SetStyle(YAML::EmitterStyle::Flow);
        node["instanceClips"] = clipsNode;
    }
    if (!animator.instanceSpeeds.empty()) {
        YAML::Node speedsNode(YAML::NodeType::Sequence);
        for (float speed : animator.instanceSpeeds) speedsNode.push_back(YamlNumbers::text(finiteOr(speed, 1.0f)));
        speedsNode.SetStyle(YAML::EmitterStyle::Flow);
        node["instanceSpeeds"] = speedsNode;
    }
    return node;
}

VertexAnimatorComponent editor::Stream::decodeVertexAnimatorComponent(const YAML::Node& node, const VertexAnimatorComponent* oldAnimator) {
    // the authored fields only: what an old one played starts over (its runtime is not copied),
    // the Inspector's Preview toggle stays
    VertexAnimatorComponent animator;
    if (oldAnimator) {
        animator.preview = oldAnimator->preview;
    }

    // a missing key is the default, not the old value
    const VertexAnimatorComponent defaults;
    auto given = [&node](const char* key) {
        const YAML::Node value = node[key];
        return value && !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    };
    // a value of the wrong shape is the default (one bad key never fails the scene's load)
    auto boolean = [&node, &given](const char* key, bool fallback) {
        if (!given(key) || !node[key].IsScalar()) return fallback;
        bool value = fallback;
        if (YamlNumbers::readPlain(node[key].Scalar(), value)) return value;
        try {
            return node[key].as<bool>();
        } catch (const YAML::Exception&) {
            return fallback;
        }
    };
    auto text = [&node, &given](const char* key) {
        return given(key) && node[key].IsScalar() ? node[key].Scalar() : std::string();
    };
    auto number = [&node, &given](const char* key, float fallback) {
        if (!given(key) || !node[key].IsScalar()) return fallback;
        try {
            return decodeFinite(node[key], fallback);
        } catch (const YAML::Exception&) {
            return fallback;
        }
    };

    animator.enabled = boolean("enabled", defaults.enabled);
    animator.animation = text("animation");
    animator.guid = text("guid");
    animator.defaultClip = text("defaultClip");
    animator.playAutomatically = boolean("playAutomatically", defaults.playAutomatically);
    animator.speed = number("speed", defaults.speed);
    animator.randomStart = boolean("randomStart", defaults.randomStart);
    animator.defaultFadeTime = std::max(0.0f, number("defaultFadeTime", defaults.defaultFadeTime));
    animator.instanceEvents = boolean("instanceEvents", defaults.instanceEvents);

    const YAML::Node attachmentsNode = node["attachments"];
    if (attachmentsNode && attachmentsNode.IsSequence()) {
        for (const YAML::Node& attachmentNode : attachmentsNode) {
            if (!attachmentNode.IsMap()) continue;
            VertexAnimatorAttachment attachment;
            const YAML::Node socket = attachmentNode["socket"];
            attachment.socket = socket && socket.IsScalar() ? socket.Scalar() : std::string();
            const YAML::Node entity = attachmentNode["entity"];
            if (entity && entity.IsScalar() && !entity.Scalar().empty()) {
                try {
                    attachment.entity = YamlNumbers::as<Entity>(entity);
                } catch (const YAML::Exception&) {
                    attachment.entity = NULL_ENTITY;
                }
            }
            animator.attachments.push_back(std::move(attachment));
        }
    }

    const YAML::Node clipsNode = node["instanceClips"];
    if (clipsNode && clipsNode.IsSequence()) {
        for (const YAML::Node& clip : clipsNode) {
            animator.instanceClips.push_back(clip.IsScalar() ? clip.Scalar() : std::string());
        }
    }
    const YAML::Node speedsNode = node["instanceSpeeds"];
    if (speedsNode && speedsNode.IsSequence()) {
        for (const YAML::Node& speed : speedsNode) {
            animator.instanceSpeeds.push_back(decodeFinite(speed, 1.0f));
        }
    }

    return animator;
}
