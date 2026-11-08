// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StreamSchema.h"
#include "render/LodFade.h"

#include "reflect/Enums.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <unordered_map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

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

// Top-level keys each component's decoder reads: its table's (editor/reflect, knownKeys)
const std::vector<std::string>& StreamSchema::knownFieldKeys(ComponentType type) {
    const reflect::ComponentDesc* desc = reflect::find(type);
    return desc ? reflect::knownKeys(*desc) : noKeys;
}

const std::vector<std::string>& StreamSchema::knownNestedKeys(NestedKind kind) {
    static const std::map<NestedKind, std::vector<std::string>> tables = {
        {NestedKind::Material, {"alphaCutoff", "alphaMode", "baseColorFactor", "baseColorTexCoord", "baseColorTexture", "blendMode", "colorWrite", "cullMode", "depthOffsetFactor", "depthOffsetUnits", "depthTest", "depthWrite", "emissiveFactor", "emissiveStrength", "emissiveTexCoord", "emissiveTexture", "extraPass", "foliageTransmission", "lighting", "metallicFactor", "metallicRoughnessTexCoord", "metallicRoughnessTexture", "name", "normalTexCoord", "normalTexture", "occlusionTexCoord", "occlusionTexture", "pbr", "roughnessFactor", "shader", "shaderFeatures", "shaderGlobalBindings", "shaderPalettes", "shaderParams", "shaderTextures", "sortPriority", "stencil", "stochasticBlendSharpness", "stochasticCellSize", "stochasticContrast", "stochasticRotation", "stochasticTiling"}},
        {NestedKind::MaterialPass, {"blendMode", "colorWrite", "cullMode", "depthOffsetFactor", "depthOffsetUnits", "depthTest", "depthWrite", "order", "shader", "shaderFeatures", "shaderParams", "shaderTextures", "stencil"}},
        {NestedKind::Stencil, {"compare", "depthFail", "fail", "pass", "readMask", "ref", "writeMask"}},
        {NestedKind::Submesh, {"faceCulling", "hasMorphNormal", "hasMorphTangent", "hasMorphTarget", "hasNormalMap", "hasSkinning", "hasTangent", "hasTexCoord1", "hasTexCoord2", "hasTextureRect", "hasVertexColor3", "hasVertexColor4", "material", "normAdjustJoint", "normAdjustWeight", "overrideFields", "primitiveType", "textureRect", "textureShadow", "vertexCount"}},
        {NestedKind::Texture, {"video", "alphaBorder", "camera", "channels", "colorFormat", "cubeMode", "data", "fluid", "guid", "guids", "height", "id", "magFilter", "minFilter", "path", "paths", "releaseDataAfterLoad", "size", "source", "svgScale", "textureType", "width", "wrapU", "wrapV"}},
    };
    return lookup(tables, kind);
}

const std::vector<std::string>& StreamSchema::knownRootKeys(RootKind kind) {
    static const std::map<RootKind, std::vector<std::string>> tables = {
        {RootKind::Scene, {"formatVersion", "guid", "formerGuids", "labels", "id", "name", "scene", "sceneType", "mainCamera", "maxValues", "shaderKeys",
            "childScenes", "cppScripts", "cppScriptPaths", "prefabs", "imports", "entities"}},
        {RootKind::SceneSettings, {"ambientLight2DColor", "ambientLight2DIntensity", "backgroundColor", "capsuleShadowIntensity", "capsuleShadowSoftness", "capsuleShadowsEnabled", "defaultLinesShader", "defaultLinesShaderUniforms", "defaultMeshShader", "defaultMeshShaderUniforms", "defaultPointsShader", "defaultPointsShaderUniforms", "defaultSkyShader", "defaultSkyShaderUniforms", "defaultUIShader", "defaultUIShaderUniforms", "depthPrepassEnabled", "enableUIEvents", "fixedResolutionEnabled", "fixedResolutionFilter", "fixedResolutionHeight", "fixedResolutionWidth", "globalIlluminationColor", "globalIlluminationIntensity", "gravity2D", "gravity3D", "lightState", "litQuality", "maxLightsPerObject", "maxPlanarReflections", "meshLodCrossFade", "meshLodCrossFadeDuration", "meshLodEnabled", "meshLodThreshold", "physicalLightEV100", "physicsInterpolation", "planarReflectionClipOffset", "planarReflectionFarClip", "planarReflectionLayers", "planarReflectionResolution", "planarReflectionShadows", "postProcess", "postProcessing", "sceneColorScale", "shadow2DQuality", "shadowCascadeBlend", "shadowDistance", "shadowFadeFraction", "shadowQuality", "ssaoBias", "ssaoEnabled", "ssaoIntensity", "ssaoRadius", "ssrBlur", "ssrEnabled", "ssrIntensity", "ssrMaxDistance", "ssrMaxSteps", "ssrThickness", "waterWakeArea", "waterWakeFade", "waterWakeResolution",
            // upstream's scene settings (SYNC#4): written away from their defaults
            "canvasScaler", "physics2DSubSteps"}},
        // prefabInstance and the keys written before it (prefabOverrides...), and the keys of
        // the GameObjects a prefab adds to an instance (addedTo, childIndex...)
        {RootKind::Entity, {"type", "entity", "name", "active", "tag", "layer", "components", "children",
            "prefabInstance", "prefabOverrides", "prefabRemovedGameObjects", "prefabLocalEntities",
            "addedTo", "childIndex", "parentRegistryEntity", "prefabRootRegistryEntity", "prefabPath"}},
        // the editor state an older project.yaml held (Workspace::adoptLegacyProjectState) and
        // its build settings (AppSettings, build.yaml) are read too
        {RootKind::Project, {"formatVersion", "android", "apple", "application", "assetLabels", "assetsDir", "atmosphereQuality", "canvasHeight", "canvasWidth", "cxxStandard", "debug", "device", "dynamicBatching", "export", "foliageDistanceScale", "graphics", "htmlUiGlyphAtlasMiB", "htmlUiMsaaSamples", "htmlUiSdfPrimitives", "htmlUiTransformAwareText", "ignoredLayerCollisions", "input", "ios", "layers", "linux", "loading", "luaDir", "macos", "mesh", "mobilePbrTier", "name", "navigation", "packNativeResources", "palettes", "physics2D", "physics3D", "playerLogging", "runtimeShaders", "savedSearches", "scalingMode", "scenes", "scriptDirs", "shaderCreation", "standalonePrefabs", "startSceneId", "tags", "texture", "textureStrategy", "threading", "versionControl", "versionControlMetadata", "vsync", "web", "windowHeight", "windowIcon", "windowMode", "windowResizable", "windowTitle", "windowWidth", "windows",
            "cmakeBuildJobs", "cmakeCCompiler", "cmakeCxxCompiler", "cmakeGenerator", "selectedScene", "tabs", "terrainEditor",
            // upstream's project.yaml blocks (SYNC#4): the editor writes and reads them
            "audio", "physics3DWorld", "playerCrash", "presentation", "shaderGlobals", "uiScale"}},
        {RootKind::Material, {}},
        {RootKind::PrefabHeader, {"type", "version", "guid", "formerGuids", "labels", "nextEntity", "root", "members", "stripped"}},
    };
    if (kind == RootKind::Material) {
        // a .material file is one encoded Material
        static const std::vector<std::string> material = [] {
            // the identity of a .material is in the file (S04)
            // and its labels (S25)
            std::vector<std::string> keys = {"formatVersion", "guid", "formerGuids", "labels"};
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
            for (int type = (int)ComponentType::Transform; type <= (int)LAST_COMPONENT_TYPE; type++) {
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
    static const EnumTable<ActionState> table(reflect::enums::kActionState);
    return table;
}

const EnumTable<AnchorPreset>& StreamSchema::anchorPreset() {
    static const EnumTable<AnchorPreset> table(reflect::enums::kAnchorPreset);
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
        {AttributeDataType::HALF, "half"},
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
        {AttributeType::VAT_INDEX, "vatindex"},
        {AttributeType::INSTANCEVAT0, "instancevat0"},
        {AttributeType::INSTANCEVAT1, "instancevat1"},
        {AttributeType::PARTICLECORNER, "particlecorner"},
        {AttributeType::PARTICLEPOSITION, "particleposition"},
        {AttributeType::PARTICLECOLOR, "particlecolor"},
        {AttributeType::PARTICLESIZE, "particlesize"},
        {AttributeType::PARTICLEROTATIONFRAME, "particlerotationframe"},
        {AttributeType::PARTICLEVELOCITY, "particlevelocity"},
        {AttributeType::PARTICLEROTATION, "particlerotation"},
        {AttributeType::PARTICLECUSTOM, "particlecustom"},
        {AttributeType::LINETANGENTIN, "linetangentin"},
        {AttributeType::LINETANGENTOUT, "linetangentout"},
        {AttributeType::LINEPARAMS, "lineparams"},
    }, AttributeType::POSITION);
    return table;
}

const EnumTable<AudioSourceAttenuation>& StreamSchema::audioSourceAttenuation() {
    static const EnumTable<AudioSourceAttenuation> table(reflect::enums::kAudioSourceAttenuation);
    return table;
}

const EnumTable<AudioSourceState>& StreamSchema::audioSourceState() {
    static const EnumTable<AudioSourceState> table(reflect::enums::kAudioSourceState);
    return table;
}

const EnumTable<BodyType>& StreamSchema::bodyType() {
    static const EnumTable<BodyType> table(reflect::enums::kBodyType);
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
    static const EnumTable<CameraType> table(reflect::enums::kCameraType);
    return table;
}

const EnumTable<Collider2DType>& StreamSchema::collider2DType() {
    static const EnumTable<Collider2DType> table(reflect::enums::kCollider2DType);
    return table;
}

const EnumTable<Collider3DType>& StreamSchema::collider3DType() {
    static const EnumTable<Collider3DType> table(reflect::enums::kCollider3DType);
    return table;
}

const EnumTable<CullingMode>& StreamSchema::cullingMode() {
    static const EnumTable<CullingMode> table(reflect::enums::kCullingMode);
    return table;
}

const EnumTable<LodFadeMode>& StreamSchema::lodFadeMode() {
    static const EnumTable<LodFadeMode> table(reflect::enums::kLodFadeMode);
    return table;
}

const EnumTable<EaseType>& StreamSchema::easeType() {
    static const EnumTable<EaseType> table(reflect::enums::kEaseType);
    return table;
}

const EnumTable<FluidOutput>& StreamSchema::fluidOutput() {
    static const EnumTable<FluidOutput> table(reflect::enums::kFluidOutput);
    return table;
}

const EnumTable<FogType>& StreamSchema::fogType() {
    static const EnumTable<FogType> table(reflect::enums::kFogType);
    return table;
}

const EnumTable<FogColorMode>& StreamSchema::fogColorMode() {
    static const EnumTable<FogColorMode> table(reflect::enums::kFogColorMode);
    return table;
}

// ---- navigation (S19): from their reflection tables (editor/reflect/components/NavigationReflect.cpp)
const EnumTable<NavCollectObjects>& StreamSchema::navCollectObjects() {
    static const EnumTable<NavCollectObjects> table(reflect::enums::kNavCollectObjects);
    return table;
}

const EnumTable<NavUseGeometry>& StreamSchema::navUseGeometry() {
    static const EnumTable<NavUseGeometry> table(reflect::enums::kNavUseGeometry);
    return table;
}

const EnumTable<nav::NavPlane>& StreamSchema::navPlane() {
    static const EnumTable<nav::NavPlane> table(reflect::enums::kNavPlane);
    return table;
}

const EnumTable<nav::AvoidanceQuality>& StreamSchema::navAvoidanceQuality() {
    static const EnumTable<nav::AvoidanceQuality> table(reflect::enums::kNavAvoidanceQuality);
    return table;
}

const EnumTable<NavAgentDriver>& StreamSchema::navAgentDriver() {
    static const EnumTable<NavAgentDriver> table(reflect::enums::kNavAgentDriver);
    return table;
}

const EnumTable<NavObstacleShape>& StreamSchema::navObstacleShape() {
    static const EnumTable<NavObstacleShape> table(reflect::enums::kNavObstacleShape);
    return table;
}

const EnumTable<NavModifierInclusion>& StreamSchema::navModifierInclusion() {
    static const EnumTable<NavModifierInclusion> table(reflect::enums::kNavModifierInclusion);
    return table;
}

const EnumTable<nav::GridLayout>& StreamSchema::navGridLayout() {
    static const EnumTable<nav::GridLayout> table(reflect::enums::kNavGridLayout);
    return table;
}

const EnumTable<nav::GridMovement>& StreamSchema::navGridMovement() {
    static const EnumTable<nav::GridMovement> table(reflect::enums::kNavGridMovement);
    return table;
}

const EnumTable<NavGridSource>& StreamSchema::navGridSource() {
    static const EnumTable<NavGridSource> table(reflect::enums::kNavGridSource);
    return table;
}

const EnumTable<WaterVolumeWaves>& StreamSchema::waterVolumeWaves() {
    static const EnumTable<WaterVolumeWaves> table(reflect::enums::kWaterVolumeWaves);
    return table;
}

const EnumTable<BuoyancyColumns>& StreamSchema::buoyancyColumns() {
    static const EnumTable<BuoyancyColumns> table(reflect::enums::kBuoyancyColumns);
    return table;
}

const EnumTable<ForceFieldShapeType>& StreamSchema::forceFieldShape() {
    static const EnumTable<ForceFieldShapeType> table(reflect::enums::kForceFieldShape);
    return table;
}

const EnumTable<ForceFieldKind>& StreamSchema::forceFieldKind() {
    static const EnumTable<ForceFieldKind> table(reflect::enums::kForceFieldKind);
    return table;
}

const EnumTable<FogVolumeShape>& StreamSchema::fogVolumeShape() {
    static const EnumTable<FogVolumeShape> table(reflect::enums::kFogVolumeShape);
    return table;
}

const EnumTable<FogVolumeFalloff>& StreamSchema::fogVolumeFalloff() {
    static const EnumTable<FogVolumeFalloff> table(reflect::enums::kFogVolumeFalloff);
    return table;
}

const EnumTable<CloudsPreset>& StreamSchema::cloudsPreset() {
    static const EnumTable<CloudsPreset> table(reflect::enums::kCloudsPreset);
    return table;
}

const EnumTable<CloudsRendering>& StreamSchema::cloudsRendering() {
    static const EnumTable<CloudsRendering> table(reflect::enums::kCloudsRendering);
    return table;
}

const EnumTable<AtmosphereQuality>& StreamSchema::atmosphereQuality() {
    static const EnumTable<AtmosphereQuality> table("AtmosphereQuality", {
        {AtmosphereQuality::OFF, "off"},
        {AtmosphereQuality::LOW, "low"},
        {AtmosphereQuality::MEDIUM, "medium"},
        {AtmosphereQuality::HIGH, "high"},
    }, AtmosphereQuality::HIGH);
    return table;
}

const EnumTable<AtmosphereQualityOverride>& StreamSchema::atmosphereQualityOverride() {
    static const EnumTable<AtmosphereQualityOverride> table(reflect::enums::kAtmosphereQualityOverride);
    return table;
}

const EnumTable<Joint2DType>& StreamSchema::joint2DType() {
    static const EnumTable<Joint2DType> table(reflect::enums::kJoint2DType);
    return table;
}

const EnumTable<Joint3DType>& StreamSchema::joint3DType() {
    static const EnumTable<Joint3DType> table(reflect::enums::kJoint3DType);
    return table;
}

const EnumTable<LatticeInterpolation>& StreamSchema::latticeInterpolation() {
    static const EnumTable<LatticeInterpolation> table(reflect::enums::kLatticeInterpolation);
    return table;
}

const EnumTable<LatticeJiggleAnchor>& StreamSchema::latticeJiggleAnchor() {
    static const EnumTable<LatticeJiggleAnchor> table(reflect::enums::kLatticeJiggleAnchor);
    return table;
}

const EnumTable<LatticeMaskChannel>& StreamSchema::latticeMaskChannel() {
    static const EnumTable<LatticeMaskChannel> table(reflect::enums::kLatticeMaskChannel);
    return table;
}

const EnumTable<LatticeOutside>& StreamSchema::latticeOutside() {
    static const EnumTable<LatticeOutside> table(reflect::enums::kLatticeOutside);
    return table;
}

const EnumTable<LatticeStage>& StreamSchema::latticeStage() {
    static const EnumTable<LatticeStage> table(reflect::enums::kLatticeStage);
    return table;
}

const EnumTable<LayoutGroupType>& StreamSchema::layoutGroupType() {
    static const EnumTable<LayoutGroupType> table(reflect::enums::kLayoutGroupType);
    return table;
}

const EnumTable<LightShading>& StreamSchema::lightShading() {
    static const EnumTable<LightShading> table(reflect::enums::kLightShading);
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

const EnumTable<LightUnit>& StreamSchema::lightUnit() {
    static const EnumTable<LightUnit> table(reflect::enums::kLightUnit);
    return table;
}

const EnumTable<LitQuality>& StreamSchema::litQuality() {
    static const EnumTable<LitQuality> table("LitQuality", {
        {LitQuality::STANDARD, "standard"},
        {LitQuality::HIGH, "high"},
    }, LitQuality::STANDARD);
    return table;
}

const EnumTable<PbrTier>& StreamSchema::pbrTier() {
    static const EnumTable<PbrTier> table("PbrTier", {
        {PbrTier::FULL, "full"},
        {PbrTier::REDUCED, "reduced"},
    }, PbrTier::REDUCED);
    return table;
}

const EnumTable<LightType>& StreamSchema::lightType() {
    static const EnumTable<LightType> table(reflect::enums::kLightType);
    return table;
}

const EnumTable<MaterialAlphaMode>& StreamSchema::materialAlphaMode() {
    static const EnumTable<MaterialAlphaMode> table(reflect::enums::kMaterialAlphaMode);
    return table;
}

const EnumTable<MaterialBlendMode>& StreamSchema::materialBlendMode() {
    static const EnumTable<MaterialBlendMode> table(reflect::enums::kMaterialBlendMode);
    return table;
}

const EnumTable<MaterialCullMode>& StreamSchema::materialCullMode() {
    static const EnumTable<MaterialCullMode> table(reflect::enums::kMaterialCullMode);
    return table;
}

const EnumTable<MaterialDepthTest>& StreamSchema::materialDepthTest() {
    static const EnumTable<MaterialDepthTest> table(reflect::enums::kMaterialDepthTest);
    return table;
}

const EnumTable<MaterialDepthWrite>& StreamSchema::materialDepthWrite() {
    static const EnumTable<MaterialDepthWrite> table(reflect::enums::kMaterialDepthWrite);
    return table;
}

const EnumTable<MaterialLighting>& StreamSchema::materialLighting() {
    static const EnumTable<MaterialLighting> table(reflect::enums::kMaterialLighting);
    return table;
}

const EnumTable<MaterialPassOrder>& StreamSchema::materialPassOrder() {
    static const EnumTable<MaterialPassOrder> table(reflect::enums::kMaterialPassOrder);
    return table;
}

const EnumTable<MaterialStencilCompare>& StreamSchema::materialStencilCompare() {
    static const EnumTable<MaterialStencilCompare> table(reflect::enums::kMaterialStencilCompare);
    return table;
}

const EnumTable<MaterialStencilOp>& StreamSchema::materialStencilOp() {
    static const EnumTable<MaterialStencilOp> table(reflect::enums::kMaterialStencilOp);
    return table;
}

const EnumTable<ParticleEmitterShape>& StreamSchema::particleEmitterShape() {
    static const EnumTable<ParticleEmitterShape> table(reflect::enums::kParticleEmitterShape);
    return table;
}

const EnumTable<SpriteDrawMode>& StreamSchema::spriteDrawMode() {
    static const EnumTable<SpriteDrawMode> table(reflect::enums::kSpriteDrawMode);
    return table;
}

const EnumTable<PivotPreset>& StreamSchema::pivotPreset() {
    static const EnumTable<PivotPreset> table(reflect::enums::kPivotPreset);
    return table;
}

const EnumTable<PrimitiveType>& StreamSchema::primitiveType() {
    static const EnumTable<PrimitiveType> table(reflect::enums::kPrimitiveType);
    return table;
}

const EnumTable<ProgressbarType>& StreamSchema::progressbarType() {
    static const EnumTable<ProgressbarType> table(reflect::enums::kProgressbarType);
    return table;
}

const EnumTable<RigidbodyMotionQuality>& StreamSchema::rigidbodyMotionQuality() {
    static const EnumTable<RigidbodyMotionQuality> table(reflect::enums::kRigidbodyMotionQuality);
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
        {Scaling::INTEGER, "integer"},
        {Scaling::INTEGER_EXPAND, "integer_expand"},
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

const EnumTable<WaterWakeResolution>& StreamSchema::waterWakeResolution() {
    static const EnumTable<WaterWakeResolution> table("WaterWakeResolution", {
        {WaterWakeResolution::AUTO, "auto"},
        {WaterWakeResolution::LOW, "low"},
        {WaterWakeResolution::MEDIUM, "medium"},
        {WaterWakeResolution::HIGH, "high"},
    }, WaterWakeResolution::AUTO);
    return table;
}

const EnumTable<PlanarReflectionResolution>& StreamSchema::planarReflectionResolution() {
    static const EnumTable<PlanarReflectionResolution> table("PlanarReflectionResolution", {
        {PlanarReflectionResolution::OFF, "off"},
        {PlanarReflectionResolution::QUARTER, "quarter"},
        {PlanarReflectionResolution::HALF, "half"},
        {PlanarReflectionResolution::FULL, "full"},
    }, PlanarReflectionResolution::HALF);
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
    static const EnumTable<ScriptType> table(reflect::enums::kScriptType);
    return table;
}

const EnumTable<ScrollbarType>& StreamSchema::scrollbarType() {
    static const EnumTable<ScrollbarType> table(reflect::enums::kScrollbarType);
    return table;
}

const EnumTable<ShadowCaster2DShape>& StreamSchema::shadowCaster2DShape() {
    static const EnumTable<ShadowCaster2DShape> table(reflect::enums::kShadowCaster2DShape);
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
    static const EnumTable<ShadowUpdateMode> table(reflect::enums::kShadowUpdateMode);
    return table;
}

const EnumTable<CascadeSplitMode>& StreamSchema::cascadeSplitMode() {
    static const EnumTable<CascadeSplitMode> table(reflect::enums::kCascadeSplitMode);
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
    static const EnumTable<TextureAlphaBorder> table(reflect::enums::kTextureAlphaBorder);
    return table;
}

const EnumTable<TextureFilter>& StreamSchema::textureFilter() {
    static const EnumTable<TextureFilter> table(reflect::enums::kTextureFilter);
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
    static const EnumTable<TextureWrap> table(reflect::enums::kTextureWrap);
    return table;
}

const EnumTable<UIDocumentRenderMode>& StreamSchema::uiDocumentRenderMode() {
    static const EnumTable<UIDocumentRenderMode> table(reflect::enums::kUIDocumentRenderMode);
    return table;
}

const EnumTable<UIEventState>& StreamSchema::uiEventState() {
    static const EnumTable<UIEventState> table(reflect::enums::kUIEventState);
    return table;
}

const EnumTable<WindingOrder>& StreamSchema::windingOrder() {
    static const EnumTable<WindingOrder> table(reflect::enums::kWindingOrder);
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
