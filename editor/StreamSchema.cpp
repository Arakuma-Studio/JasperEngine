// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StreamSchema.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // Easing names are read case-insensitively, with '-' for '_' (the AI editor actions and
    // hand-written files use both)
    std::string normalizeEaseName(const std::string& text) {
        std::string normalized;
        normalized.reserve(text.size());
        for (char ch : text) {
            if (ch == '-') {
                normalized.push_back('_');
            } else {
                normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
            }
        }
        return normalized;
    }

    const std::vector<std::string> noKeys;

    template<typename K>
    const std::vector<std::string>& lookup(const std::map<K, std::vector<std::string>>& tables, K key) {
        auto it = tables.find(key);
        return it == tables.end() ? noKeys : it->second;
    }

    bool contains(const std::vector<std::string>& keys, const std::string& key) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    }

    // An asset reference keeps its GUID next to its path hint: "<key>Guid" is known wherever
    // "<key>" is (docs/specs/S04-asset-identity.md, Stream::encodeAssetRef)
    bool containsOrGuidOf(const std::vector<std::string>& keys, const std::string& key) {
        if (contains(keys, key)) return true;
        static const std::string suffix = "Guid";
        return key.size() > suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0 &&
               contains(keys, key.substr(0, key.size() - suffix.size()));
    }

}

// Top-level keys each component's decoder reads (tools/check_stream_keys.py --print lists them)
const std::vector<std::string>& StreamSchema::knownFieldKeys(ComponentType type) {
    static const std::map<ComponentType, std::vector<std::string>> tables = {
        {ComponentType::ActionComponent, {"ownedTarget", "speed", "state", "target"}},
        {ComponentType::AlphaTweenComponent, {"endAlpha", "startAlpha"}},
        {ComponentType::AnimationComponent, {"actions", "defaultFadeTime", "duration", "loop", "ownedActions"}},
        {ComponentType::AudioSourceComponent, {"attenuationModel", "attenuationRolloffFactor", "dopplerFactor", "enableClocked", "enabled", "filename", "inaudibleBehaviorKill", "inaudibleBehaviorMustTick", "looping", "loopingPoint", "maxDistance", "minDistance", "pan", "protectVoice", "speed", "state", "volume"}},
        {ComponentType::BoneComponent, {"bindPosition", "bindRotation", "bindScale", "index", "model", "offsetMatrix"}},
        {ComponentType::ButtonComponent, {"colorDisabled", "colorHovered", "colorNormal", "colorPressed", "disabled", "label", "textureDisabled", "textureHovered", "textureNormal", "texturePressed"}},
        {ComponentType::CameraComponent, {"aspect", "autoResize", "bottomClip", "cullingMask", "depthTest", "enabled", "farClip", "framebufferFilter", "framebufferHeight", "framebufferWidth", "leftClip", "nearClip", "renderToTexture", "rightClip", "target", "topClip", "transparentSort", "type", "up", "useTarget", "yfov"}},
        {ComponentType::ColorTweenComponent, {"endColor", "startColor", "useSRGB"}},
        {ComponentType::FogComponent, {"color", "density", "linearEnd", "linearStart", "type"}},
        {ComponentType::ImageComponent, {"patchMarginBottom", "patchMarginLeft", "patchMarginRight", "patchMarginTop", "textureScaleFactor"}},
        {ComponentType::InputFieldComponent, {"cursor", "cursorBlink", "cursorColor", "cursorIndex", "cursorWidth", "disabled", "password", "passwordChar", "placeholder", "placeholderColor", "selection", "selectionAnchor", "selectionColor", "text"}},
        {ComponentType::InstancedMeshComponent, {"cullDistance", "cullInstances", "distanceFade", "enabled", "fadeEnd", "fadeStart", "instancedBillboard", "instancedCylindricalBillboard", "instances", "maxInstances"}},
        {ComponentType::Joint2DComponent, {"anchorA", "anchorB", "autoAnchors", "axis", "bodyA", "bodyB", "enabled", "rope", "target", "type"}},
        {ComponentType::Joint3DComponent, {"anchor", "anchorA", "anchorB", "autoAnchors", "axis", "axisX", "axisY", "bodyA", "bodyB", "enabled", "fixedPointA", "fixedPointB", "hinge", "hingeA", "hingeB", "isLooping", "limitsMax", "limitsMin", "normal", "normalHalfConeAngle", "numTeethGear", "numTeethGearA", "numTeethGearB", "numTeethRack", "pathPoints", "pathPosition", "planeAxis", "planeHalfConeAngle", "rackLength", "slider", "twistAxis", "twistMaxAngle", "twistMinAngle", "type"}},
        {ComponentType::KeyframeTracksComponent, {"easings", "index", "interpolation", "times"}},
        {ComponentType::LayoutGroupComponent, {"fillAvailableSpace", "type", "useAllWrapSpace", "wrapCellHeight", "wrapCellWidth"}},
        {ComponentType::Light2DComponent, {"color", "enabled", "falloff", "height", "intensity", "mapResolution", "range", "shadowBias", "shadowSoftness", "shadows"}},
        {ComponentType::LightComponent, {"automaticShadowCamera", "cascadeSplitMode", "cascadeSplits", "color", "cullingMask", "direction", "enabled", "innerConeCos", "intensity", "mapResolution", "numShadowCascades", "outerConeCos", "range", "shading", "shadowAngularDiameter", "shadowBias", "shadowCameraNearFar", "shadowDistance", "shadowNormalBias", "shadowSourceRadius", "shadowStrength", "shadowUpdateMode", "shadows", "softShadows", "spotMask", "type", "worldDirection"}},
        {ComponentType::LineRendererComponent, {"blendMode", "colorWrite", "customShader", "depthWrite", "enabled", "lines", "maxLines", "shaderFeatures", "shaderTextures", "shaderUniforms", "stencil"}},
        {ComponentType::MeshComponent, {"aabb", "autoTransparency", "bonesMatrix", "buffer", "castShadows", "cullingMode", "customDepthShader", "customShader", "eBuffers", "enabled", "indices", "lodBias", "lodEnabled", "morphWeights", "normAdjustJoint", "normAdjustWeight", "receiveIBL", "receiveLights", "receiveShadows", "renderInReflectionProbes", "shaderUniforms", "shadowsBillboard", "staticShadowCaster", "submeshes", "transparent", "verticesAABB", "windingOrder", "worldAABB"}},
        {ComponentType::MeshPolygonComponent, {"automaticFlipY", "flipY", "height", "points", "width"}},
        {ComponentType::MirrorComponent, {"normal"}},
        {ComponentType::ModelComponent, {"animations", "bonesIdMapping", "bonesNameMapping", "enabled", "filename", "mergeStaticMeshes", "meshNodesMapping", "nodesIdMapping", "skeleton"}},
        {ComponentType::MorphTracksComponent, {"inTangents", "outTangents", "values"}},
        {ComponentType::PanelComponent, {"canBringToFront", "canMove", "canResize", "defaultHeaderMargin", "headerMarginBottom", "headerMarginLeft", "headerMarginRight", "headerMarginTop", "headercontainer", "headerimage", "headertext", "minHeight", "minWidth", "resizeMargin", "titleAnchorPreset"}},
        {ComponentType::ParticleSystemComponent, {"accelerationInitializer", "accelerationModifier", "alphaInitializer", "alphaModifier", "bursts", "colorGradient", "colorInitializer", "colorModifier", "emitter", "enabled", "lifeInitializer", "localSpace", "loop", "maxParticles", "maxPerUpdate", "positionInitializer", "positionModifier", "rate", "rotationInitializer", "rotationModifier", "scaleInitializer", "scaleModifier", "sizeInitializer", "sizeModifier", "spriteInitializer", "spriteModifier", "velocityInitializer", "velocityModifier"}},
        {ComponentType::PointsComponent, {"autoTransparency", "blendMode", "colorWrite", "customShader", "depthWrite", "enabled", "framesRect", "maxPoints", "points", "shaderFeatures", "shaderTextures", "shaderUniforms", "stencil", "texture", "transparent"}},
        {ComponentType::PolygonComponent, {"points"}},
        {ComponentType::PositionTweenComponent, {"endPosition", "startPosition"}},
        {ComponentType::PrefabComponent, {"guid", "name", "path"}},
        {ComponentType::ProgressbarComponent, {"fill", "fillMarginBottom", "fillMarginLeft", "fillMarginRight", "fillMarginTop", "type", "value"}},
        {ComponentType::RectTransformComponent, {"anchorOffsetBottom", "anchorOffsetLeft", "anchorOffsetRight", "anchorOffsetTop", "anchorPointBottom", "anchorPointLeft", "anchorPointRight", "anchorPointTop", "anchorPreset", "containerBoxIndex", "height", "ignoreEvents", "ignoreScissor", "panel", "pivot", "positionOffset", "scissor", "usingAnchors", "width"}},
        {ComponentType::ReflectionProbeComponent, {"blendDistance", "boxOffset", "boxSize", "cullingMask", "farClip", "includeSky", "intensity", "mode", "nearClip", "priority", "resolution", "texture", "updateInterval", "updateMode"}},
        {ComponentType::Rigidbody2DComponent, {"colliders", "enabled", "numColliders", "type"}},
        {ComponentType::RigidbodyComponent, {"allowedDOFs", "colliders", "enabled", "friction", "gravityFactor", "motionQuality", "numColliders", "restitution", "sensor", "type"}},
        {ComponentType::RotateTracksComponent, {"inTangents", "outTangents", "values"}},
        {ComponentType::RotationTweenComponent, {"endRotation", "shortestPath", "startRotation"}},
        {ComponentType::ScaleTracksComponent, {"inTangents", "outTangents", "values"}},
        {ComponentType::ScaleTweenComponent, {"endScale", "startScale"}},
        {ComponentType::ScriptComponent, {"scripts"}},
        {ComponentType::ScrollbarComponent, {"bar", "barMarginBottom", "barMarginLeft", "barMarginRight", "barMarginTop", "barSize", "step", "type"}},
        {ComponentType::ShadowCaster2DComponent, {"closed", "enabled", "points", "shape"}},
        {ComponentType::SkyboxComponent, {"blendMode", "color", "customShader", "depthWrite", "rotation", "shaderFeatures", "shaderTextures", "shaderUniforms", "texture", "useDefaultTexture", "visible"}},
        {ComponentType::SpriteAnimationComponent, {"frames", "framesTime", "loop"}},
        {ComponentType::SpriteComponent, {"automaticFlipY", "enabled", "flipY", "framesRect", "height", "pivotPreset", "textureScaleFactor", "width"}},
        {ComponentType::TerrainComponent, {"autoSetRanges", "blendMap", "blendMaps", "enabled", "foliageLayers", "heightMap", "levels", "maxHeight", "offset", "ranges", "resolution", "rootGridSize", "surfaceLayers", "terrainSize", "textureBaseTiles", "textureDetailTiles", "textureLayers"}},
        {ComponentType::TextComponent, {"fixedHeight", "fixedWidth", "font", "fontGuid", "fontSize", "maxTextSize", "multiline", "pivotBaseline", "pivotCentered", "text"}},
        {ComponentType::TilemapComponent, {"automaticFlipY", "enabled", "flipY", "height", "reserveTiles", "textureScaleFactor", "tiles", "tilesRect", "width"}},
        {ComponentType::Transform, {"billboard", "billboardRotation", "cylindricalBillboard", "fakeBillboard", "localMatrix", "modelMatrix", "normalMatrix", "position", "rotation", "scale", "visible", "worldPosition", "worldRotation", "worldScale"}},
        {ComponentType::TranslateTracksComponent, {"inTangents", "outTangents", "values"}},
        {ComponentType::TweenComponent, {"duration", "function", "loop"}},
        {ComponentType::UIComponent, {"aabb", "automaticFlipY", "blendMode", "buffer", "color", "colorWrite", "customShader", "depthWrite", "enabled", "flipY", "focused", "indices", "minBufferCount", "minIndicesCount", "pointerMoved", "primitiveType", "shaderFeatures", "shaderTextures", "shaderUniforms", "stencil", "texture", "vertexCount", "worldAABB"}},
        {ComponentType::UIDocumentComponent, {"enabled", "filename", "inputSurface", "interactive", "pixelRatio", "renderMode", "sortOrder", "textureHeight", "textureWidth"}},
    };
    return lookup(tables, type);
}

const std::vector<std::string>& StreamSchema::knownNestedKeys(NestedKind kind) {
    static const std::map<NestedKind, std::vector<std::string>> tables = {
        {NestedKind::Material, {"alphaCutoff", "alphaMode", "baseColorFactor", "baseColorTexCoord", "baseColorTexture", "blendMode", "colorWrite", "cullMode", "depthOffsetFactor", "depthOffsetUnits", "depthTest", "depthWrite", "emissiveFactor", "emissiveTexCoord", "emissiveTexture", "extraPass", "foliageTransmission", "lighting", "metallicFactor", "metallicRoughnessTexCoord", "metallicRoughnessTexture", "name", "normalTexCoord", "normalTexture", "occlusionTexCoord", "occlusionTexture", "roughnessFactor", "shader", "shaderFeatures", "shaderParams", "shaderTextures", "sortPriority", "stencil"}},
        {NestedKind::MaterialPass, {"blendMode", "colorWrite", "cullMode", "depthOffsetFactor", "depthOffsetUnits", "depthTest", "depthWrite", "order", "shader", "shaderFeatures", "shaderParams", "shaderTextures", "stencil"}},
        {NestedKind::Stencil, {"compare", "depthFail", "fail", "pass", "readMask", "ref", "writeMask"}},
        {NestedKind::Submesh, {"faceCulling", "hasMorphNormal", "hasMorphTangent", "hasMorphTarget", "hasNormalMap", "hasSkinning", "hasTangent", "hasTexCoord1", "hasTexCoord2", "hasTextureRect", "hasVertexColor3", "hasVertexColor4", "material", "normAdjustJoint", "normAdjustWeight", "overrideFields", "primitiveType", "textureRect", "textureShadow", "vertexCount"}},
        {NestedKind::Texture, {"alphaBorder", "camera", "channels", "colorFormat", "cubeMode", "data", "guid", "guids", "height", "id", "magFilter", "minFilter", "path", "paths", "releaseDataAfterLoad", "size", "source", "svgScale", "textureType", "width", "wrapU", "wrapV"}},
    };
    return lookup(tables, kind);
}

const std::vector<std::string>& StreamSchema::knownRootKeys(RootKind kind) {
    static const std::map<RootKind, std::vector<std::string>> tables = {
        {RootKind::Scene, {"formatVersion", "guid", "formerGuids", "id", "name", "scene", "sceneType", "mainCamera", "maxValues", "shaderKeys",
            "childScenes", "cppScripts", "cppScriptPaths", "prefabs", "entities"}},
        {RootKind::SceneSettings, {"ambientLight2DColor", "ambientLight2DIntensity", "backgroundColor", "defaultLinesShader", "defaultMeshShader", "defaultPointsShader", "defaultSkyShader", "defaultUIShader", "depthPrepassEnabled", "enableUIEvents", "fixedResolutionEnabled", "fixedResolutionFilter", "fixedResolutionHeight", "fixedResolutionWidth", "globalIlluminationColor", "globalIlluminationIntensity", "gravity2D", "gravity3D", "lightState", "maxLightsPerObject", "meshLodEnabled", "meshLodThreshold", "postProcess", "postProcessing", "sceneColorScale", "shadow2DQuality", "shadowCascadeBlend", "shadowDistance", "shadowFadeFraction", "shadowQuality", "ssaoBias", "ssaoEnabled", "ssaoIntensity", "ssaoRadius", "ssrBlur", "ssrEnabled", "ssrIntensity", "ssrMaxDistance", "ssrMaxSteps", "ssrThickness"}},
        // prefabInstance and the keys written before it (prefabOverrides...), and the keys of
        // the GameObjects a prefab adds to an instance (addedTo, childIndex...)
        {RootKind::Entity, {"type", "entity", "name", "active", "tag", "layer", "components", "children",
            "prefabInstance", "prefabOverrides", "prefabRemovedGameObjects", "prefabLocalEntities",
            "addedTo", "childIndex", "parentRegistryEntity", "prefabRootRegistryEntity", "prefabPath"}},
        // the editor state an older project.yaml held (Workspace::adoptLegacyProjectState) and
        // its build settings (AppSettings, build.yaml) are read too
        {RootKind::Project, {"formatVersion", "android", "application", "assetsDir", "canvasHeight", "canvasWidth", "cxxStandard", "export", "graphics", "htmlUiMsaaSamples", "ignoredLayerCollisions", "ios", "layers", "linux", "loading", "luaDir", "macos", "name", "packNativeResources", "physics2D", "physics3D", "runtimeShaders", "scalingMode", "scenes", "scriptDirs", "standalonePrefabs", "startSceneId", "tags", "textureStrategy", "versionControlMetadata", "vsync", "web", "windowHeight", "windowIcon", "windowMode", "windowResizable", "windowTitle", "windowWidth", "windows",
            "cmakeBuildJobs", "cmakeCCompiler", "cmakeCxxCompiler", "cmakeGenerator", "selectedScene", "tabs", "terrainEditor"}},
        {RootKind::Material, {}},
        {RootKind::PrefabHeader, {"type", "version", "guid", "formerGuids", "nextEntity", "root", "members", "stripped"}},
    };
    if (kind == RootKind::Material) {
        // a .material file is one encoded Material
        static const std::vector<std::string> material = [] {
            // the identity of a .material is in the file (S04)
            std::vector<std::string> keys = {"formatVersion", "guid", "formerGuids"};
            const std::vector<std::string>& fields = knownNestedKeys(NestedKind::Material);
            keys.insert(keys.end(), fields.begin(), fields.end());
            return keys;
        }();
        return material;
    }
    return lookup(tables, kind);
}

bool StreamSchema::isKnownFieldKey(ComponentType type, const std::string& key) {
    return containsOrGuidOf(knownFieldKeys(type), key);
}

bool StreamSchema::isKnownRootKey(RootKind kind, const std::string& key) {
    return containsOrGuidOf(knownRootKeys(kind), key);
}

bool StreamSchema::isKnownNestedKey(NestedKind kind, const std::string& key) {
    return containsOrGuidOf(knownNestedKeys(kind), key);
}

const char* StreamSchema::nestedKindName(NestedKind kind) {
    switch (kind) {
        case NestedKind::Submesh: return "Submesh";
        case NestedKind::Material: return "Material";
        case NestedKind::MaterialPass: return "MaterialPass";
        case NestedKind::Stencil: return "Stencil";
        case NestedKind::Texture: return "Texture";
    }
    return "";
}

namespace {

    const std::vector<std::pair<std::string, ComponentType>>& componentTypesByName() {
        static const std::vector<std::pair<std::string, ComponentType>> names = [] {
            std::vector<std::pair<std::string, ComponentType>> list;
            for (int type = (int)ComponentType::Transform; type <= (int)ComponentType::UIDocumentComponent; type++) {
                list.emplace_back(Catalog::getComponentName((ComponentType)type, true), (ComponentType)type);
            }
            return list;
        }();
        return names;
    }

}

const std::vector<std::string>& StreamSchema::componentNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> list;
        for (const auto& [name, type] : componentTypesByName())
            list.push_back(name);
        return list;
    }();
    return names;
}

bool StreamSchema::findComponentType(const std::string& name, ComponentType& type) {
    for (const auto& [candidate, candidateType] : componentTypesByName()) {
        if (candidate == name) {
            type = candidateType;
            return true;
        }
    }
    return false;
}

// ---- enum tables (the names written first, older spellings as aliases) ----

const EnumTable<ActionState>& StreamSchema::actionState() {
    static const EnumTable<ActionState> table("ActionState", {
        {ActionState::Running, "Running"},
        {ActionState::Paused, "Paused"},
        {ActionState::Stopped, "Stopped"},
    }, ActionState::Stopped);
    return table;
}

const EnumTable<AnchorPreset>& StreamSchema::anchorPreset() {
    static const EnumTable<AnchorPreset> table("AnchorPreset", {
        {AnchorPreset::NONE, "NONE"},
        {AnchorPreset::TOP_LEFT, "TOP_LEFT"},
        {AnchorPreset::TOP_RIGHT, "TOP_RIGHT"},
        {AnchorPreset::BOTTOM_LEFT, "BOTTOM_LEFT"},
        {AnchorPreset::BOTTOM_RIGHT, "BOTTOM_RIGHT"},
        {AnchorPreset::CENTER_LEFT, "CENTER_LEFT"},
        {AnchorPreset::CENTER_TOP, "CENTER_TOP"},
        {AnchorPreset::CENTER_RIGHT, "CENTER_RIGHT"},
        {AnchorPreset::CENTER_BOTTOM, "CENTER_BOTTOM"},
        {AnchorPreset::CENTER, "CENTER"},
        {AnchorPreset::LEFT_WIDE, "LEFT_WIDE"},
        {AnchorPreset::TOP_WIDE, "TOP_WIDE"},
        {AnchorPreset::RIGHT_WIDE, "RIGHT_WIDE"},
        {AnchorPreset::BOTTOM_WIDE, "BOTTOM_WIDE"},
        {AnchorPreset::VERTICAL_CENTER_WIDE, "VERTICAL_CENTER_WIDE"},
        {AnchorPreset::HORIZONTAL_CENTER_WIDE, "HORIZONTAL_CENTER_WIDE"},
        {AnchorPreset::FULL_LAYOUT, "FULL_LAYOUT"},
    }, AnchorPreset::NONE);
    return table;
}

const EnumTable<AndroidOrientation>& StreamSchema::androidOrientation() {
    static const EnumTable<AndroidOrientation> table("AndroidOrientation", {
        {AndroidOrientation::Portrait, "portrait"},
        {AndroidOrientation::Landscape, "landscape"},
        {AndroidOrientation::SensorPortrait, "sensorPortrait"},
        {AndroidOrientation::SensorLandscape, "sensorLandscape"},
        {AndroidOrientation::FullSensor, "fullSensor"},
        {AndroidOrientation::Unspecified, "unspecified"},
    }, AndroidOrientation::Unspecified);
    return table;
}

const EnumTable<AttributeDataType>& StreamSchema::attributeDataType() {
    static const EnumTable<AttributeDataType> table("AttributeDataType", {
        {AttributeDataType::BYTE, "byte"},
        {AttributeDataType::UNSIGNED_BYTE, "unsigned_byte"},
        {AttributeDataType::SHORT, "short"},
        {AttributeDataType::UNSIGNED_SHORT, "unsigned_short"},
        {AttributeDataType::INT, "int"},
        {AttributeDataType::UNSIGNED_INT, "unsigned_int"},
        {AttributeDataType::FLOAT, "float"},
    }, AttributeDataType::FLOAT);
    return table;
}

const EnumTable<AttributeType>& StreamSchema::attributeType() {
    static const EnumTable<AttributeType> table("AttributeType", {
        {AttributeType::INDEX, "index"},
        {AttributeType::POSITION, "position"},
        {AttributeType::TEXCOORD1, "texcoord1"},
        {AttributeType::TEXCOORD2, "texcoord2"},
        {AttributeType::NORMAL, "normal"},
        {AttributeType::TANGENT, "tangent"},
        {AttributeType::COLOR, "color"},
        {AttributeType::POINTSIZE, "pointsize"},
        {AttributeType::POINTROTATION, "pointrotation"},
        {AttributeType::TEXTURERECT, "texturerect"},
        {AttributeType::BONEWEIGHTS, "boneweights"},
        {AttributeType::BONEIDS, "boneids"},
        {AttributeType::MORPHTARGET0, "morphtarget0"},
        {AttributeType::MORPHTARGET1, "morphtarget1"},
        {AttributeType::MORPHTARGET2, "morphtarget2"},
        {AttributeType::MORPHTARGET3, "morphtarget3"},
        {AttributeType::MORPHTARGET4, "morphtarget4"},
        {AttributeType::MORPHTARGET5, "morphtarget5"},
        {AttributeType::MORPHTARGET6, "morphtarget6"},
        {AttributeType::MORPHTARGET7, "morphtarget7"},
        {AttributeType::MORPHNORMAL0, "morphnormal0"},
        {AttributeType::MORPHNORMAL1, "morphnormal1"},
        {AttributeType::MORPHNORMAL2, "morphnormal2"},
        {AttributeType::MORPHNORMAL3, "morphnormal3"},
        {AttributeType::MORPHTANGENT0, "morphtangent0"},
        {AttributeType::MORPHTANGENT1, "morphtangent1"},
        {AttributeType::INSTANCEMATRIXCOL1, "instancematrixcol1"},
        {AttributeType::INSTANCEMATRIXCOL2, "instancematrixcol2"},
        {AttributeType::INSTANCEMATRIXCOL3, "instancematrixcol3"},
        {AttributeType::INSTANCEMATRIXCOL4, "instancematrixcol4"},
        {AttributeType::INSTANCECOLOR, "instancecolor"},
        {AttributeType::INSTANCETEXTURERECT, "instancetexturerect"},
        {AttributeType::TERRAINNODEPOSITION, "terrainnodeposition"},
        {AttributeType::TERRAINNODESIZE, "terrainnodesize"},
        {AttributeType::TERRAINNODERANGE, "terrainnoderange"},
        {AttributeType::TERRAINNODERESOLUTION, "terrainnoderesolution"},
        {AttributeType::SMOOTHNORMAL, "smoothnormal"},
    }, AttributeType::POSITION);
    return table;
}

const EnumTable<AudioSourceAttenuation>& StreamSchema::audioSourceAttenuation() {
    static const EnumTable<AudioSourceAttenuation> table("AudioSourceAttenuation", {
        {AudioSourceAttenuation::NO_ATTENUATION, "no_attenuation"},
        {AudioSourceAttenuation::INVERSE_DISTANCE, "inverse_distance"},
        {AudioSourceAttenuation::LINEAR_DISTANCE, "linear_distance"},
        {AudioSourceAttenuation::EXPONENTIAL_DISTANCE, "exponential_distance"},
    }, AudioSourceAttenuation::NO_ATTENUATION);
    return table;
}

const EnumTable<AudioSourceState>& StreamSchema::audioSourceState() {
    static const EnumTable<AudioSourceState> table("AudioSourceState", {
        {AudioSourceState::Playing, "playing"},
        {AudioSourceState::Paused, "paused"},
        {AudioSourceState::Stopped, "stopped"},
    }, AudioSourceState::Stopped);
    return table;
}

const EnumTable<BodyType>& StreamSchema::bodyType() {
    static const EnumTable<BodyType> table("BodyType", {
        {BodyType::STATIC, "static"},
        {BodyType::KINEMATIC, "kinematic"},
        {BodyType::DYNAMIC, "dynamic"},
    }, BodyType::STATIC);
    return table;
}

const EnumTable<BufferType>& StreamSchema::bufferType() {
    static const EnumTable<BufferType> table("BufferType", {
        {BufferType::VERTEX_BUFFER, "vertex_buffer"},
        {BufferType::INDEX_BUFFER, "index_buffer"},
        {BufferType::STORAGE_BUFFER, "storage_buffer"},
    }, BufferType::VERTEX_BUFFER);
    return table;
}

const EnumTable<BufferUsage>& StreamSchema::bufferUsage() {
    static const EnumTable<BufferUsage> table("BufferUsage", {
        {BufferUsage::IMMUTABLE, "immutable"},
        {BufferUsage::DYNAMIC, "dynamic"},
        {BufferUsage::STREAM, "stream"},
    }, BufferUsage::IMMUTABLE);
    return table;
}

const EnumTable<CameraType>& StreamSchema::cameraType() {
    static const EnumTable<CameraType> table("CameraType", {
        {CameraType::CAMERA_UI, "camera_ui"},
        {CameraType::CAMERA_ORTHO, "camera_ortho"},
        {CameraType::CAMERA_PERSPECTIVE, "camera_perspective"},
    }, CameraType::CAMERA_PERSPECTIVE);
    return table;
}

const EnumTable<Collider2DType>& StreamSchema::collider2DType() {
    static const EnumTable<Collider2DType> table("Collider2DType", {
        {Collider2DType::POLYGON, "polygon"},
        {Collider2DType::CIRCLE, "circle"},
        {Collider2DType::CAPSULE, "capsule"},
        {Collider2DType::SEGMENT, "segment"},
        {Collider2DType::CHAIN, "chain"},
    }, Collider2DType::POLYGON);
    return table;
}

const EnumTable<Collider3DType>& StreamSchema::collider3DType() {
    static const EnumTable<Collider3DType> table("Collider3DType", {
        {Collider3DType::SPHERE, "sphere"},
        {Collider3DType::BOX, "box"},
        {Collider3DType::CAPSULE, "capsule"},
        {Collider3DType::TAPERED_CAPSULE, "tapered_capsule"},
        {Collider3DType::CYLINDER, "cylinder"},
        {Collider3DType::CONVEX_HULL, "convex_hull"},
        {Collider3DType::MESH, "mesh"},
        {Collider3DType::HEIGHTFIELD, "heightfield"},
    }, Collider3DType::SPHERE);
    return table;
}

const EnumTable<CullingMode>& StreamSchema::cullingMode() {
    static const EnumTable<CullingMode> table("CullingMode", {
        {CullingMode::BACK, "back"},
        {CullingMode::FRONT, "front"},
    }, CullingMode::BACK);
    return table;
}

const EnumTable<EaseType>& StreamSchema::easeType() {
    static const EnumTable<EaseType> table("EaseType", {
        {EaseType::LINEAR, "LINEAR"},
        {EaseType::QUAD_IN, "QUAD_IN"},
        {EaseType::QUAD_OUT, "QUAD_OUT"},
        {EaseType::QUAD_IN_OUT, "QUAD_IN_OUT"},
        {EaseType::CUBIC_IN, "CUBIC_IN"},
        {EaseType::CUBIC_OUT, "CUBIC_OUT"},
        {EaseType::CUBIC_IN_OUT, "CUBIC_IN_OUT"},
        {EaseType::QUART_IN, "QUART_IN"},
        {EaseType::QUART_OUT, "QUART_OUT"},
        {EaseType::QUART_IN_OUT, "QUART_IN_OUT"},
        {EaseType::QUINT_IN, "QUINT_IN"},
        {EaseType::QUINT_OUT, "QUINT_OUT"},
        {EaseType::QUINT_IN_OUT, "QUINT_IN_OUT"},
        {EaseType::SINE_IN, "SINE_IN"},
        {EaseType::SINE_OUT, "SINE_OUT"},
        {EaseType::SINE_IN_OUT, "SINE_IN_OUT"},
        {EaseType::EXPO_IN, "EXPO_IN"},
        {EaseType::EXPO_OUT, "EXPO_OUT"},
        {EaseType::EXPO_IN_OUT, "EXPO_IN_OUT"},
        {EaseType::CIRC_IN, "CIRC_IN"},
        {EaseType::CIRC_OUT, "CIRC_OUT"},
        {EaseType::CIRC_IN_OUT, "CIRC_IN_OUT"},
        {EaseType::ELASTIC_IN, "ELASTIC_IN"},
        {EaseType::ELASTIC_OUT, "ELASTIC_OUT"},
        {EaseType::ELASTIC_IN_OUT, "ELASTIC_IN_OUT"},
        {EaseType::BACK_IN, "BACK_IN"},
        {EaseType::BACK_OUT, "BACK_OUT"},
        {EaseType::BACK_IN_OUT, "BACK_IN_OUT"},
        {EaseType::BOUNCE_IN, "BOUNCE_IN"},
        {EaseType::BOUNCE_OUT, "BOUNCE_OUT"},
        {EaseType::BOUNCE_IN_OUT, "BOUNCE_IN_OUT"},
        {EaseType::STEP, "STEP"},
        {EaseType::CUSTOM, "CUSTOM"},
    }, EaseType::LINEAR, {}, normalizeEaseName);
    return table;
}

const EnumTable<FogType>& StreamSchema::fogType() {
    static const EnumTable<FogType> table("FogType", {
        {FogType::LINEAR, "linear"},
        {FogType::EXPONENTIAL, "exponential"},
        {FogType::EXPONENTIALSQUARED, "exponential_squared"},
    }, FogType::LINEAR, {
        {FogType::EXPONENTIALSQUARED, "exponentialsquared"},
        {FogType::EXPONENTIALSQUARED, "exponentialSquared"},
    });
    return table;
}

const EnumTable<Joint2DType>& StreamSchema::joint2DType() {
    static const EnumTable<Joint2DType> table("Joint2DType", {
        {Joint2DType::DISTANCE, "distance"},
        {Joint2DType::REVOLUTE, "revolute"},
        {Joint2DType::PRISMATIC, "prismatic"},
        {Joint2DType::MOUSE, "mouse"},
        {Joint2DType::WHEEL, "wheel"},
        {Joint2DType::WELD, "weld"},
        {Joint2DType::MOTOR, "motor"},
    }, Joint2DType::DISTANCE);
    return table;
}

const EnumTable<Joint3DType>& StreamSchema::joint3DType() {
    static const EnumTable<Joint3DType> table("Joint3DType", {
        {Joint3DType::FIXED, "fixed"},
        {Joint3DType::DISTANCE, "distance"},
        {Joint3DType::POINT, "point"},
        {Joint3DType::HINGE, "hinge"},
        {Joint3DType::CONE, "cone"},
        {Joint3DType::PRISMATIC, "prismatic"},
        {Joint3DType::SWINGTWIST, "swingtwist"},
        {Joint3DType::SIXDOF, "sixdof"},
        {Joint3DType::PATH, "path"},
        {Joint3DType::GEAR, "gear"},
        {Joint3DType::RACKANDPINON, "rackandpinion"},
        {Joint3DType::PULLEY, "pulley"},
    }, Joint3DType::FIXED);
    return table;
}

const EnumTable<LayoutGroupType>& StreamSchema::layoutGroupType() {
    static const EnumTable<LayoutGroupType> table("LayoutGroupType", {
        {LayoutGroupType::VERTICAL, "vertical"},
        {LayoutGroupType::HORIZONTAL, "horizontal"},
        {LayoutGroupType::VERTICAL_WRAP, "vertical_wrap"},
        {LayoutGroupType::HORIZONTAL_WRAP, "horizontal_wrap"},
    }, LayoutGroupType::VERTICAL);
    return table;
}

const EnumTable<LightShading>& StreamSchema::lightShading() {
    static const EnumTable<LightShading> table("LightShading", {
        {LightShading::BOTH, "BOTH"},
        {LightShading::PIXEL, "PIXEL"},
        {LightShading::VERTEX, "VERTEX"},
    }, LightShading::BOTH);
    return table;
}

const EnumTable<LightState>& StreamSchema::lightState() {
    static const EnumTable<LightState> table("LightState", {
        {LightState::OFF, "off"},
        {LightState::ON, "on"},
        {LightState::AUTO, "auto"},
    }, LightState::AUTO);
    return table;
}

const EnumTable<LightType>& StreamSchema::lightType() {
    static const EnumTable<LightType> table("LightType", {
        {LightType::DIRECTIONAL, "directional"},
        {LightType::POINT, "point"},
        {LightType::SPOT, "spot"},
    }, LightType::DIRECTIONAL);
    return table;
}

const EnumTable<MaterialAlphaMode>& StreamSchema::materialAlphaMode() {
    static const EnumTable<MaterialAlphaMode> table("MaterialAlphaMode", {
        {MaterialAlphaMode::AUTO, "AUTO"},
        {MaterialAlphaMode::ALPHA_OPAQUE, "ALPHA_OPAQUE"},
        {MaterialAlphaMode::MASK, "MASK"},
        {MaterialAlphaMode::BLEND, "BLEND"},
    }, MaterialAlphaMode::AUTO);
    return table;
}

const EnumTable<MaterialBlendMode>& StreamSchema::materialBlendMode() {
    static const EnumTable<MaterialBlendMode> table("MaterialBlendMode", {
        {MaterialBlendMode::AUTO, "AUTO"},
        {MaterialBlendMode::NONE, "NONE"},
        {MaterialBlendMode::ALPHA, "ALPHA"},
        {MaterialBlendMode::PREMULTIPLIED, "PREMULTIPLIED"},
        {MaterialBlendMode::ADDITIVE, "ADDITIVE"},
        {MaterialBlendMode::MULTIPLY, "MULTIPLY"},
    }, MaterialBlendMode::AUTO, {
        {MaterialBlendMode::NONE, "OPAQUE"},
    });
    return table;
}

const EnumTable<MaterialCullMode>& StreamSchema::materialCullMode() {
    static const EnumTable<MaterialCullMode> table("MaterialCullMode", {
        {MaterialCullMode::AUTO, "AUTO"},
        {MaterialCullMode::BACK, "BACK"},
        {MaterialCullMode::FRONT, "FRONT"},
        {MaterialCullMode::OFF, "OFF"},
    }, MaterialCullMode::AUTO);
    return table;
}

const EnumTable<MaterialDepthTest>& StreamSchema::materialDepthTest() {
    static const EnumTable<MaterialDepthTest> table("MaterialDepthTest", {
        {MaterialDepthTest::AUTO, "AUTO"},
        {MaterialDepthTest::LESS, "LESS"},
        {MaterialDepthTest::LESS_EQUAL, "LESS_EQUAL"},
        {MaterialDepthTest::EQUAL, "EQUAL"},
        {MaterialDepthTest::GREATER_EQUAL, "GREATER_EQUAL"},
        {MaterialDepthTest::GREATER, "GREATER"},
        {MaterialDepthTest::NOT_EQUAL, "NOT_EQUAL"},
        {MaterialDepthTest::ALWAYS, "ALWAYS"},
    }, MaterialDepthTest::AUTO);
    return table;
}

const EnumTable<MaterialDepthWrite>& StreamSchema::materialDepthWrite() {
    static const EnumTable<MaterialDepthWrite> table("MaterialDepthWrite", {
        {MaterialDepthWrite::AUTO, "AUTO"},
        {MaterialDepthWrite::ENABLED, "ENABLED"},
        {MaterialDepthWrite::DISABLED, "DISABLED"},
    }, MaterialDepthWrite::AUTO);
    return table;
}

const EnumTable<MaterialLighting>& StreamSchema::materialLighting() {
    static const EnumTable<MaterialLighting> table("MaterialLighting", {
        {MaterialLighting::AUTO, "AUTO"},
        {MaterialLighting::PIXEL, "PIXEL"},
        {MaterialLighting::VERTEX, "VERTEX"},
        {MaterialLighting::MIXED, "MIXED"},
        {MaterialLighting::NONE, "NONE"},
    }, MaterialLighting::AUTO);
    return table;
}

const EnumTable<MaterialPassOrder>& StreamSchema::materialPassOrder() {
    static const EnumTable<MaterialPassOrder> table("MaterialPassOrder", {
        {MaterialPassOrder::BEFORE, "BEFORE"},
        {MaterialPassOrder::AFTER, "AFTER"},
    }, MaterialPassOrder::AFTER);
    return table;
}

const EnumTable<MaterialStencilCompare>& StreamSchema::materialStencilCompare() {
    static const EnumTable<MaterialStencilCompare> table("MaterialStencilCompare", {
        {MaterialStencilCompare::AUTO, "AUTO"},
        {MaterialStencilCompare::DISABLED, "DISABLED"},
        {MaterialStencilCompare::NEVER, "NEVER"},
        {MaterialStencilCompare::LESS, "LESS"},
        {MaterialStencilCompare::EQUAL, "EQUAL"},
        {MaterialStencilCompare::LESS_EQUAL, "LESS_EQUAL"},
        {MaterialStencilCompare::GREATER, "GREATER"},
        {MaterialStencilCompare::NOT_EQUAL, "NOT_EQUAL"},
        {MaterialStencilCompare::GREATER_EQUAL, "GREATER_EQUAL"},
        {MaterialStencilCompare::ALWAYS, "ALWAYS"},
    }, MaterialStencilCompare::AUTO);
    return table;
}

const EnumTable<MaterialStencilOp>& StreamSchema::materialStencilOp() {
    static const EnumTable<MaterialStencilOp> table("MaterialStencilOp", {
        {MaterialStencilOp::KEEP, "KEEP"},
        {MaterialStencilOp::ZERO, "ZERO"},
        {MaterialStencilOp::REPLACE, "REPLACE"},
        {MaterialStencilOp::INCR_CLAMP, "INCR_CLAMP"},
        {MaterialStencilOp::DECR_CLAMP, "DECR_CLAMP"},
        {MaterialStencilOp::INVERT, "INVERT"},
        {MaterialStencilOp::INCR_WRAP, "INCR_WRAP"},
        {MaterialStencilOp::DECR_WRAP, "DECR_WRAP"},
    }, MaterialStencilOp::KEEP);
    return table;
}

const EnumTable<ParticleEmitterShape>& StreamSchema::particleEmitterShape() {
    static const EnumTable<ParticleEmitterShape> table("ParticleEmitterShape", {
        {ParticleEmitterShape::Box, "Box"},
        {ParticleEmitterShape::Sphere, "Sphere"},
        {ParticleEmitterShape::Hemisphere, "Hemisphere"},
        {ParticleEmitterShape::Circle, "Circle"},
        {ParticleEmitterShape::Cone, "Cone"},
    }, ParticleEmitterShape::Box);
    return table;
}

const EnumTable<PivotPreset>& StreamSchema::pivotPreset() {
    static const EnumTable<PivotPreset> table("PivotPreset", {
        {PivotPreset::CENTER, "center"},
        {PivotPreset::TOP_CENTER, "top_center"},
        {PivotPreset::BOTTOM_CENTER, "bottom_center"},
        {PivotPreset::LEFT_CENTER, "left_center"},
        {PivotPreset::RIGHT_CENTER, "right_center"},
        {PivotPreset::TOP_LEFT, "top_left"},
        {PivotPreset::BOTTOM_LEFT, "bottom_left"},
        {PivotPreset::TOP_RIGHT, "top_right"},
        {PivotPreset::BOTTOM_RIGHT, "bottom_right"},
    }, PivotPreset::BOTTOM_LEFT);
    return table;
}

const EnumTable<PrimitiveType>& StreamSchema::primitiveType() {
    static const EnumTable<PrimitiveType> table("PrimitiveType", {
        {PrimitiveType::TRIANGLES, "triangles"},
        {PrimitiveType::TRIANGLE_STRIP, "triangle_strip"},
        {PrimitiveType::POINTS, "points"},
        {PrimitiveType::LINES, "lines"},
    }, PrimitiveType::TRIANGLES);
    return table;
}

const EnumTable<ProgressbarType>& StreamSchema::progressbarType() {
    static const EnumTable<ProgressbarType> table("ProgressbarType", {
        {ProgressbarType::VERTICAL, "vertical"},
        {ProgressbarType::HORIZONTAL, "horizontal"},
    }, ProgressbarType::HORIZONTAL);
    return table;
}

const EnumTable<RigidbodyMotionQuality>& StreamSchema::rigidbodyMotionQuality() {
    static const EnumTable<RigidbodyMotionQuality> table("RigidbodyMotionQuality", {
        {RigidbodyMotionQuality::LINEAR_CAST, "linear_cast"},
        {RigidbodyMotionQuality::DISCRETE, "discrete"},
    }, RigidbodyMotionQuality::DISCRETE);
    return table;
}

const EnumTable<Scaling>& StreamSchema::scalingMode() {
    static const EnumTable<Scaling> table("Scaling", {
        {Scaling::FITWIDTH, "fitwidth"},
        {Scaling::FITHEIGHT, "fitheight"},
        {Scaling::LETTERBOX, "letterbox"},
        {Scaling::CROP, "crop"},
        {Scaling::STRETCH, "stretch"},
        {Scaling::NATIVE, "native"},
    }, Scaling::FITWIDTH);
    return table;
}

const EnumTable<SceneColorScale>& StreamSchema::sceneColorScale() {
    static const EnumTable<SceneColorScale> table("SceneColorScale", {
        {SceneColorScale::FULL, "full"},
        {SceneColorScale::HALF, "half"},
    }, SceneColorScale::HALF);
    return table;
}

const EnumTable<SceneType>& StreamSchema::sceneType() {
    static const EnumTable<SceneType> table("SceneType", {
        {SceneType::SCENE_3D, "scene_3d"},
        {SceneType::SCENE_2D, "scene_2d"},
        {SceneType::SCENE_UI, "scene_ui"},
    }, SceneType::SCENE_3D);
    return table;
}

const EnumTable<ScriptPropertyType>& StreamSchema::scriptPropertyType() {
    static const EnumTable<ScriptPropertyType> table("ScriptPropertyType", {
        {ScriptPropertyType::Bool, "Bool"},
        {ScriptPropertyType::Int, "Int"},
        {ScriptPropertyType::Float, "Float"},
        {ScriptPropertyType::String, "String"},
        {ScriptPropertyType::Vector2, "Vector2"},
        {ScriptPropertyType::Vector3, "Vector3"},
        {ScriptPropertyType::Vector4, "Vector4"},
        {ScriptPropertyType::Color3, "Color3"},
        {ScriptPropertyType::Color4, "Color4"},
        {ScriptPropertyType::EntityReference, "EntityReference"},
        {ScriptPropertyType::Prefab, "Prefab"},
        {ScriptPropertyType::Asset, "Asset"},
    }, ScriptPropertyType::Bool);
    return table;
}

const EnumTable<ScriptType>& StreamSchema::scriptType() {
    static const EnumTable<ScriptType> table("ScriptType", {
        {ScriptType::CPP, "cpp"},
        {ScriptType::LUA, "lua"},
    }, ScriptType::CPP, {
        {ScriptType::LUA, "script_lua"},
        {ScriptType::CPP, "subclass"},
        {ScriptType::CPP, "script_class"},
    });
    return table;
}

const EnumTable<ScrollbarType>& StreamSchema::scrollbarType() {
    static const EnumTable<ScrollbarType> table("ScrollbarType", {
        {ScrollbarType::VERTICAL, "vertical"},
        {ScrollbarType::HORIZONTAL, "horizontal"},
    }, ScrollbarType::VERTICAL);
    return table;
}

const EnumTable<ShadowCaster2DShape>& StreamSchema::shadowCaster2DShape() {
    static const EnumTable<ShadowCaster2DShape> table("ShadowCaster2DShape", {
        {ShadowCaster2DShape::AUTO_QUAD, "autoquad"},
        {ShadowCaster2DShape::POLYGON, "polygon"},
    }, ShadowCaster2DShape::AUTO_QUAD);
    return table;
}

const EnumTable<ShadowQuality>& StreamSchema::shadowQuality() {
    static const EnumTable<ShadowQuality> table("ShadowQuality", {
        {ShadowQuality::NONE, "none"},
        {ShadowQuality::LOW, "low"},
        {ShadowQuality::MEDIUM, "medium"},
        {ShadowQuality::HIGH, "high"},
        {ShadowQuality::AUTO, "auto"},
        {ShadowQuality::ULTRA, "ultra"},
    }, ShadowQuality::AUTO);
    return table;
}

const EnumTable<ShadowUpdateMode>& StreamSchema::shadowUpdateMode() {
    static const EnumTable<ShadowUpdateMode> table("ShadowUpdateMode", {
        {ShadowUpdateMode::AUTO, "auto"},
        {ShadowUpdateMode::EVERY_FRAME, "everyFrame"},
        {ShadowUpdateMode::ON_DEMAND, "onDemand"},
    }, ShadowUpdateMode::AUTO);
    return table;
}

const EnumTable<CascadeSplitMode>& StreamSchema::cascadeSplitMode() {
    static const EnumTable<CascadeSplitMode> table("CascadeSplitMode", {
        {CascadeSplitMode::AUTO, "auto"},
        {CascadeSplitMode::MANUAL, "manual"},
    }, CascadeSplitMode::AUTO);
    return table;
}

const EnumTable<ShadowTier>& StreamSchema::shadowTier() {
    static const EnumTable<ShadowTier> table("ShadowTier", {
        {ShadowTier::LOW, "low"},
        {ShadowTier::MEDIUM, "medium"},
        {ShadowTier::HIGH, "high"},
        {ShadowTier::ULTRA, "ultra"},
    }, ShadowTier::HIGH);
    return table;
}

const EnumTable<TerrainBrushFalloff>& StreamSchema::terrainBrushFalloff() {
    static const EnumTable<TerrainBrushFalloff> table("TerrainBrushFalloff", {
        {TerrainBrushFalloff::Smooth, "Smooth"},
        {TerrainBrushFalloff::Linear, "Linear"},
        {TerrainBrushFalloff::Constant, "Constant"},
    }, TerrainBrushFalloff::Smooth);
    return table;
}

const EnumTable<TerrainBrushMode>& StreamSchema::terrainBrushMode() {
    static const EnumTable<TerrainBrushMode> table("TerrainBrushMode", {
        {TerrainBrushMode::Raise, "Raise"},
        {TerrainBrushMode::Lower, "Lower"},
        {TerrainBrushMode::Smooth, "Smooth"},
        {TerrainBrushMode::Flatten, "Flatten"},
        {TerrainBrushMode::Sharpen, "Sharpen"},
        {TerrainBrushMode::Noise, "Noise"},
        {TerrainBrushMode::Terrace, "Terrace"},
        {TerrainBrushMode::Stamp, "Stamp"},
        {TerrainBrushMode::Erode, "Erode"},
        {TerrainBrushMode::Ramp, "Ramp"},
        {TerrainBrushMode::PaintBase, "PaintBase"},
        {TerrainBrushMode::PaintLayer, "PaintLayer"},
        {TerrainBrushMode::PaintDensity, "PaintDensity"},
        {TerrainBrushMode::EraseDensity, "EraseDensity"},
        {TerrainBrushMode::PlaceObject, "PlaceObject"},
        {TerrainBrushMode::EraseObject, "EraseObject"},
    }, TerrainBrushMode::Raise, {
        {TerrainBrushMode::PaintLayer, "PaintRed"},
        {TerrainBrushMode::PaintLayer, "PaintGreen"},
        {TerrainBrushMode::PaintLayer, "PaintBlue"},
    });
    return table;
}

const EnumTable<TerrainBrushShape>& StreamSchema::terrainBrushShape() {
    static const EnumTable<TerrainBrushShape> table("TerrainBrushShape", {
        {TerrainBrushShape::Circle, "Circle"},
        {TerrainBrushShape::Square, "Square"},
    }, TerrainBrushShape::Circle);
    return table;
}

const EnumTable<TextureAlphaBorder>& StreamSchema::textureAlphaBorder() {
    static const EnumTable<TextureAlphaBorder> table("TextureAlphaBorder", {
        {TextureAlphaBorder::FIX, "fix"},
        {TextureAlphaBorder::KEEP, "keep"},
        {TextureAlphaBorder::AUTO, "auto"},
    }, TextureAlphaBorder::AUTO);
    return table;
}

const EnumTable<TextureFilter>& StreamSchema::textureFilter() {
    static const EnumTable<TextureFilter> table("TextureFilter", {
        {TextureFilter::NEAREST, "nearest"},
        {TextureFilter::LINEAR, "linear"},
        {TextureFilter::NEAREST_MIPMAP_NEAREST, "nearest_mipmap_nearest"},
        {TextureFilter::NEAREST_MIPMAP_LINEAR, "nearest_mipmap_linear"},
        {TextureFilter::LINEAR_MIPMAP_NEAREST, "linear_mipmap_nearest"},
        {TextureFilter::LINEAR_MIPMAP_LINEAR, "linear_mipmap_linear"},
    }, TextureFilter::LINEAR);
    return table;
}

const EnumTable<TextureStrategy>& StreamSchema::textureStrategy() {
    static const EnumTable<TextureStrategy> table("TextureStrategy", {
        {TextureStrategy::FIT, "fit"},
        {TextureStrategy::RESIZE, "resize"},
        {TextureStrategy::NONE, "none"},
    }, TextureStrategy::NONE);
    return table;
}

const EnumTable<TextureWrap>& StreamSchema::textureWrap() {
    static const EnumTable<TextureWrap> table("TextureWrap", {
        {TextureWrap::REPEAT, "repeat"},
        {TextureWrap::MIRRORED_REPEAT, "mirrored_repeat"},
        {TextureWrap::CLAMP_TO_EDGE, "clamp_to_edge"},
        {TextureWrap::CLAMP_TO_BORDER, "clamp_to_border"},
    }, TextureWrap::REPEAT);
    return table;
}

const EnumTable<UIDocumentRenderMode>& StreamSchema::uiDocumentRenderMode() {
    static const EnumTable<UIDocumentRenderMode> table("UIDocumentRenderMode", {
        {UIDocumentRenderMode::TEXTURE, "texture"},
        {UIDocumentRenderMode::SCREEN, "screen"},
    }, UIDocumentRenderMode::SCREEN);
    return table;
}

const EnumTable<UIEventState>& StreamSchema::uiEventState() {
    static const EnumTable<UIEventState> table("UIEventState", {
        {UIEventState::NOT_SET, "not_set"},
        {UIEventState::ENABLED, "true"},
        {UIEventState::DISABLED, "false"},
    }, UIEventState::NOT_SET);
    return table;
}

const EnumTable<WindingOrder>& StreamSchema::windingOrder() {
    static const EnumTable<WindingOrder> table("WindingOrder", {
        {WindingOrder::CCW, "ccw"},
        {WindingOrder::CW, "cw"},
    }, WindingOrder::CCW);
    return table;
}

const EnumTable<WindowMode>& StreamSchema::windowMode() {
    static const EnumTable<WindowMode> table("WindowMode", {
        {WindowMode::WINDOWED, "windowed"},
        {WindowMode::MAXIMIZED, "maximized"},
        {WindowMode::FULLSCREEN, "fullscreen"},
    }, WindowMode::WINDOWED);
    return table;
}
