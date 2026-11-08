// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Stream.h"

#include <unordered_map>
#include <unordered_set>
#include "component/AlphaTweenComponent.h"
#include "component/CanvasGroupComponent.h"
#include "component/ImageComponent.h"
#include "component/SafeAreaComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/TextComponent.h"
#include "component/PrefabComponent.h"
#include "component/TextEffectComponent.h"
#include "component/VideoPlayerComponent.h"
#include "component/CameraComponent.h"
#include "component/MeshPolygonComponent.h"
#include "component/ModelComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "util/WorldTransforms.h"
#include "reflect/Reflect.h"
#include "log/Logger.h"
#include "StreamSchema.h"
#include "StreamPreserved.h"
#include "StreamMigrations.h"
#include "stream/CurveCodec.h"
#include "util/ShaderGlobalsJson.h"
#include "AppSettings.h"
#include "mcp/McpValues.h"
#include "EditorHost.h"
#include "Out.h"
#include "ShippingTypes.h"
#include "Workspace.h"

#include "util/Base64.h"
#include "Catalog.h"
#include "util/YamlNumbers.h"
#include "lattice/LatticeTypes.h"
#include "object/CameraFollow.h"
#include "object/CameraShaker.h"
#include "Factory.h"
#include "pool/SoundPool.h"
#include "pool/ShaderPool.h"
#include "subsystem/MeshSystem.h"
#include "System.h"
#include "util/CameraTextureLink.h"
#include "util/Color.h"
#include "util/FluidTextureLink.h"
#include "util/VideoTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/StringUtils.h"
#include "util/FileUtils.h"
#include "util/LightPhotometry.h"
#include "texture/TexturePack.h"
#include "ecs/EntityManager.h"
#include "render/SceneRender2D.h"
#include "render/SceneRender3D.h"
#include "render/SceneDrawModes.h"
#include "window/TerrainEditWindow.h"
#include "physics/PlacementSettingsLimits.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "input/InputActions.h"
#include "DebugSettings.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <iomanip>
#include <locale>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>

using namespace jasperengine;

// The meaning of a key's property bits changed once since keys were written to files: MESH bit
// 5 was 'Pcf' (USE_SHADOWS_PCF) until July 2026, then unnamed (stripped as keys were read), and
// is Stochastic Tiling ('Sto') since October 2026. An editor of the time between could still
// leave it set in what it wrote, so a key without a version (1) that has it holds a stale 'Pcf':
// it is cleared as the key is read, or it would turn into a Stochastic Tiling variant. A key
// written with the bit on carries `keyVersion: 2`; every other key is written as before, so
// files without such variants keep their bytes.
namespace {
    constexpr int SHADER_KEY_VERSION_STOCHASTIC = 2;

    bool hasVersionedBits(ShaderKey key) {
        return ShaderPool::getShaderTypeFromKey(key) == ShaderType::MESH && (key.properties & shaderPropertyBit(5));
    }

    ShaderKey stripStaleBits(ShaderKey key, int version) {
        if (ShaderPool::getShaderTypeFromKey(key) == ShaderType::MESH && version < SHADER_KEY_VERSION_STOCHASTIC)
            key.properties &= ~shaderPropertyBit(5);
        return key;
    }
}

// The customShaderId in a key is a session-local registry index, so a fork is
// named by its project-relative path instead, the way scenes name theirs.
YAML::Node editor::Stream::encodeShaderKey(ShaderKey key) {
    const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
    std::string customShader;
    if (customId != 0) {
        // Without a name there is nothing stable to write, and dropping only
        // the id would turn the override into one for the built-in shader
        customShader = ShaderPool::getCustomShaderName(customId);
        if (customShader.empty()) return YAML::Node();
    }

    // a plain integer: the 32-bit values files hold read and write as before
    const uint64_t properties = ShaderPool::getPropertiesFromKey(key);
    YAML::Node entry(YAML::NodeType::Map);
    entry["type"] = ShaderPool::getShaderTypeName(ShaderPool::getShaderTypeFromKey(key), true);
    if (properties) entry["properties"] = properties;
    if (!customShader.empty()) entry["customShader"] = customShader;
    if (hasVersionedBits(key)) entry["keyVersion"] = SHADER_KEY_VERSION_STOCHASTIC;
    return entry;
}

bool editor::Stream::decodeShaderKey(const YAML::Node& keyNode, ShaderKey& key) {
    if (!keyNode) {
        return false;
    }
    if (!keyNode.IsMap()) {
        // Backward compatibility: scenes saved before forks were named by path
        // stored the packed key (32 property bits), so only its stable half survives and
        // the fork is re-paired on the next save. Remove this branch with support for them.
        if (!keyNode.IsScalar()) {
            return false;
        }
        key = stripStaleBits(ShaderPool::getStorageKey(ShaderPool::unpackLegacyKey(editor::YamlNumbers::as<uint64_t>(keyNode))), 1);
        return true;
    }
    ShaderType shaderType;
    if (!keyNode["type"] || !ShaderPool::parseShaderTypeToken(keyNode["type"].as<std::string>(), shaderType)) {
        return false;
    }

    // Keyed by name, so this returns the id the scenes resolve for it too
    uint16_t customId = 0;
    if (keyNode["customShader"]) {
        customId = ShaderPool::registerCustomShader(keyNode["customShader"].as<std::string>());
    }
    // 64 bits; a file written before they were widened holds a 32-bit value
    const uint64_t properties = keyNode["properties"] ? editor::YamlNumbers::as<uint64_t>(keyNode["properties"]) : 0;
    const int version = keyNode["keyVersion"] ? editor::YamlNumbers::as<int>(keyNode["keyVersion"]) : 1;
    key = stripStaleBits(ShaderPool::getShaderKey(shaderType, properties, customId), version);
    return true;
}

// Both directions normalize, keeping a since-retired property bit out of the file.
// Built-in variants keep the set's order (type, then properties); variants of custom shaders
// follow, ordered by the shader's name, not by its session-local customId (the order shaders
// were registered in this session), so a second save writes the same list.
YAML::Node editor::Stream::encodeShaderKeys(const std::set<ShaderKey>& shaderKeys) {
    YAML::Node node(YAML::NodeType::Sequence);
    // YAML::Node assignment writes through to the node it refers to: the entries stay put and
    // only their order (names and indices) is sorted
    std::vector<YAML::Node> customEntries;
    std::vector<std::pair<std::string, size_t>> customOrder;
    for (ShaderKey rawKey : shaderKeys) {
        const ShaderKey key = ShaderPool::normalizeKey(rawKey);
        YAML::Node entry = encodeShaderKey(key);
        if (!entry.IsMap()) continue;
        if (key.customId == 0) {
            node.push_back(entry);
            continue;
        }
        const std::string name = entry["customShader"] && entry["customShader"].IsScalar() ? entry["customShader"].Scalar() : std::string();
        customOrder.emplace_back(name, customEntries.size());
        customEntries.push_back(entry);
    }
    // stable: one shader's variants keep their (type, properties) order
    std::stable_sort(customOrder.begin(), customOrder.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& item : customOrder) node.push_back(customEntries[item.second]);
    return node;
}

std::vector<ShaderKey> editor::Stream::decodeShaderKeys(const YAML::Node& node) {
    std::vector<ShaderKey> shaderKeys;
    if (!node || !node.IsSequence()) {
        return shaderKeys;
    }

    for (const auto& keyNode : node) {
        ShaderKey key;
        if (decodeShaderKey(keyNode, key)) {
            shaderKeys.push_back(ShaderPool::normalizeKey(key));
        }
    }
    return shaderKeys;
}

namespace {
    YAML::Node encodeExportBackendSet(const std::set<ShaderBackend>& backends) {
        YAML::Node node(YAML::NodeType::Sequence);
        for (ShaderBackend backend : backends) {
            node.push_back(ShaderPool::getShaderBackendCliToken(backend));
        }
        return node;
    }

    std::set<ShaderBackend> decodeExportBackendSet(const YAML::Node& node) {
        std::set<ShaderBackend> backends;
        if (!node || !node.IsSequence()) {
            return backends;
        }

        for (const auto& backendNode : node) {
            ShaderBackend backend;
            if (ShaderPool::parseShaderBackend(backendNode.as<std::string>(), backend)) {
                backends.insert(backend);
            }
        }
        return backends;
    }

    // presentation.allowedOrientations: the names of the set, Portrait first
    YAML::Node orientationMaskToNode(uint8_t mask) {
        YAML::Node node(YAML::NodeType::Sequence);
        for (ScreenOrientation orientation : {ScreenOrientation::Portrait, ScreenOrientation::PortraitUpsideDown,
                                              ScreenOrientation::LandscapeLeft, ScreenOrientation::LandscapeRight}) {
            if (mask & PlatformRules::orientationBit(orientation)) node.push_back(PlatformRules::orientationName(orientation));
        }
        return node;
    }

    uint8_t nodeToOrientationMask(const YAML::Node& node, uint8_t fallback) {
        if (!node || !node.IsSequence()) return fallback;
        uint8_t mask = 0;
        for (const auto& item : node) {
            ScreenOrientation orientation;
            if (item.IsScalar() && PlatformRules::parseOrientationName(item.as<std::string>(), orientation) &&
                PlatformRules::isFixedOrientation(orientation)) {
                mask |= PlatformRules::orientationBit(orientation);
            }
        }
        return mask ? mask : fallback;
    }
}

// Projects written before Player > Resolution and Presentation kept an Android-only
// orientation (android.orientation, the manifest's screenOrientation): its value as the shared
// default and allowed orientations. False for "unspecified" or an unknown value (the defaults stay).
bool editor::Stream::migrateAndroidOrientation(const std::string& value, PresentationSettings& presentation) {
    const uint8_t P = PlatformRules::orientationBit(ScreenOrientation::Portrait);
    const uint8_t PU = PlatformRules::orientationBit(ScreenOrientation::PortraitUpsideDown);
    const uint8_t LL = PlatformRules::orientationBit(ScreenOrientation::LandscapeLeft);
    const uint8_t LR = PlatformRules::orientationBit(ScreenOrientation::LandscapeRight);
    if (value == "portrait") {
        presentation.defaultOrientation = ScreenOrientation::Portrait;
        presentation.allowedOrientations = P;
    } else if (value == "landscape") {
        presentation.defaultOrientation = ScreenOrientation::LandscapeLeft;
        presentation.allowedOrientations = LL;
    } else if (value == "sensorPortrait") {
        presentation.defaultOrientation = ScreenOrientation::AutoRotation;
        presentation.allowedOrientations = P | PU;
    } else if (value == "sensorLandscape") {
        presentation.defaultOrientation = ScreenOrientation::AutoRotation;
        presentation.allowedOrientations = LL | LR;
    } else if (value == "fullSensor") {
        presentation.defaultOrientation = ScreenOrientation::AutoRotation;
        presentation.allowedOrientations = P | PU | LL | LR;
    } else {
        return false;
    }
    return true;
}

std::string editor::Stream::makeEmbeddedTextureId(){
    static uint64_t embeddedTextureCounter = 1;
    return "__stream_embedded_texture_" + std::to_string(embeddedTextureCounter++);
}

const char* editor::Stream::colorFormatToString(ColorFormat format){
    return format == ColorFormat::RED ? "red" : "rgba";
}

ColorFormat editor::Stream::stringToColorFormat(const std::string& value, int channels){
    if (value == "red"){
        return ColorFormat::RED;
    }
    if (value == "rgba"){
        return ColorFormat::RGBA;
    }
    return channels == 1 ? ColorFormat::RED : ColorFormat::RGBA;
}

bool editor::Stream::hasTexturePath(const Texture& texture) {
    for (size_t face = 0; face < 6; face++) {
        if (!texture.getPath(face).empty()) return true;
    }
    return false;
}

bool editor::Stream::getEmbeddedTextureData(const Texture& texture, TextureData*& data){
    if (texture.empty() || texture.isFramebuffer()){
        return false;
    }

    Texture& mutableTexture = const_cast<Texture&>(texture);
    TextureLoadResult result = mutableTexture.load();
    if (result.state != ResourceLoadState::Finished || !result.data){
        return false;
    }

    TextureData& loadedData = mutableTexture.getData();
    if (!loadedData.getData() || loadedData.getSize() == 0){
        return false;
    }

    data = &loadedData;
    return true;
}

bool editor::Stream::setEmbeddedTextureData(Texture& texture, const std::string& preferredId, int width, int height, ColorFormat format, int channels, const std::vector<unsigned char>& pixels){
    const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
    if (width <= 0 || height <= 0 || channels <= 0 || pixels.size() < size){
        return false;
    }

    unsigned char* raw = static_cast<unsigned char*>(std::malloc(size));
    if (!raw){
        return false;
    }

    std::memcpy(raw, pixels.data(), size);

    TextureData data(width, height, static_cast<unsigned int>(size), format, channels, raw);
    data.setDataOwned(false);

    std::string textureId = preferredId.empty() ? makeEmbeddedTextureId() : preferredId;
    texture.setData(textureId, data);
    texture.getData().setDataOwned(true);
    return true;
}

float editor::Stream::finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : (std::isfinite(fallback) ? fallback : 0.0f);
}

double editor::Stream::finiteOr(double value, double fallback) {
    return std::isfinite(value) ? value : (std::isfinite(fallback) ? fallback : 0.0);
}

float editor::Stream::positiveFiniteOr(float value, float fallback) {
    if (std::isfinite(value) && value > 0.0f) {
        return value;
    }
    return std::isfinite(fallback) && fallback > 0.0f ? fallback : 1.0f;
}

namespace {

    // A scalar that reads as a number (.nan and .inf included)
    bool isNumber(const YAML::Node& node) {
        if (!node || !node.IsScalar()) {
            return false;
        }
        double value;
        if (editor::YamlNumbers::readPlain(node.Scalar(), value)) {
            return true;
        }
        try {
            (void)node.as<double>();
            return true;
        } catch (const YAML::Exception&) {
            return false;
        }
    }

    // A sequence of at least `count` numbers. Anything else is reported (and kept by the load
    // report's preservation) before the decoder falls back to its default; null is no value.
    bool numberSequence(const YAML::Node& node, size_t count) {
        if (!node || node.IsNull()) {
            return false;
        }
        bool valid = node.IsSequence() && node.size() >= count;
        for (size_t i = 0; valid && i < count; i++) {
            valid = isNumber(node[i]);
        }
        if (!valid) {
            editor::StreamLoad::invalidValue(node, std::to_string(count) + " numbers");
        }
        return valid;
    }

}

float editor::Stream::decodeFinite(const YAML::Node& node, float fallback) {
    if (!node || !node.IsScalar()) {
        if (node && !node.IsNull()) {
            StreamLoad::invalidValue(node, "a number");
        }
        return finiteOr(fallback);
    }

    float value;
    if (YamlNumbers::readPlain(node.Scalar(), value)) {
        return finiteOr(value, fallback);
    }

    try {
        float value = node.as<float>();
        return finiteOr(value, fallback);
    } catch (const YAML::Exception&) {
        StreamLoad::invalidValue(node, "a number");
        return finiteOr(fallback);
    }
}

double editor::Stream::decodeFiniteDouble(const YAML::Node& node, double fallback) {
    if (!node || !node.IsScalar()) {
        if (node && !node.IsNull()) {
            StreamLoad::invalidValue(node, "a number");
        }
        return finiteOr(fallback);
    }

    double value;
    if (YamlNumbers::readPlain(node.Scalar(), value)) {
        return finiteOr(value, fallback);
    }

    try {
        double value = node.as<double>();
        return finiteOr(value, fallback);
    } catch (const YAML::Exception&) {
        StreamLoad::invalidValue(node, "a number");
        return finiteOr(fallback);
    }
}

float editor::Stream::decodePositiveFinite(const YAML::Node& node, float fallback) {
    if (!node || !node.IsScalar()) {
        return positiveFiniteOr(fallback, 1.0f);
    }

    float value;
    if (YamlNumbers::readPlain(node.Scalar(), value)) {
        return positiveFiniteOr(value, fallback);
    }

    try {
        float value = node.as<float>();
        return positiveFiniteOr(value, fallback);
    } catch (const YAML::Exception&) {
        return positiveFiniteOr(fallback, 1.0f);
    }
}

uint32_t editor::Stream::decodeLayerMask(const YAML::Node& node, uint32_t fallback) {
    if (!node || !node.IsScalar()) {
        return fallback;
    }

    // written unsigned; a hand-written -1 (Unity's Everything) also reads as all layers
    try {
        return static_cast<uint32_t>(YamlNumbers::as<long long>(node));
    } catch (const YAML::Exception&) {
        return fallback;
    }
}

void editor::Stream::encodeFinite(YAML::Node& node, const char* key, float value) {
    if (std::isfinite(value)) {
        node[key] = YamlNumbers::text(value);
    }
}

void editor::Stream::encodeFinite(YAML::Node& node, const char* key, double value) {
    if (std::isfinite(value)) {
        node[key] = YamlNumbers::text(value);
    }
}

void editor::Stream::encodePositiveFinite(YAML::Node& node, const char* key, float value) {
    if (std::isfinite(value) && value > 0.0f) {
        node[key] = YamlNumbers::text(value);
    }
}

void editor::Stream::encodeFiniteVector3(YAML::Node& node, const char* key, const Vector3& value) {
    if (value.isValid()) {
        node[key] = encodeVector3(value);
    }
}

Vector3 editor::Stream::decodeFiniteVector3(const YAML::Node& node, const Vector3& fallback) {
    Vector3 safeFallback = fallback.isValid() ? fallback : Vector3::ZERO;
    if (!node || !node.IsSequence() || node.size() < 3) {
        return safeFallback;
    }

    return Vector3(
        decodeFinite(node[0], safeFallback.x),
        decodeFinite(node[1], safeFallback.y),
        decodeFinite(node[2], safeFallback.z)
    );
}

std::string editor::Stream::sceneTypeToString(editor::SceneType type){
    return StreamSchema::sceneType().toString(type);
}

editor::SceneType editor::Stream::stringToSceneType(const std::string& str){
    return StreamSchema::sceneType().fromString(str);
}

std::string editor::Stream::primitiveTypeToString(PrimitiveType type) {
    return StreamSchema::primitiveType().toString(type);
}

PrimitiveType editor::Stream::stringToPrimitiveType(const std::string& str) {
    return StreamSchema::primitiveType().fromString(str);
}

std::string editor::Stream::easeTypeToString(EaseType type) {
    return StreamSchema::easeType().toString(type);
}

EaseType editor::Stream::stringToEaseType(const std::string& str) {
    return StreamSchema::easeType().fromString(str);
}

YAML::Node editor::Stream::encodeEase(const Ease& ease) {
    YAML::Node node;
    node = easeTypeToString(ease.getType());
    return node;
}

Ease editor::Stream::decodeEase(const YAML::Node& node, const Ease* oldEase) {
    Ease ease;
    if (oldEase) {
        ease = *oldEase;
    }

    EaseType type = decodeEnumValue(node, StreamSchema::easeType());

    if (type == EaseType::CUSTOM) {
        if (!oldEase || oldEase->getType() != EaseType::CUSTOM) {
            ease = Ease(EaseType::LINEAR);
        }
        ease.setType(EaseType::CUSTOM);
    } else {
        ease.setType(type);
    }

    return ease;
}

std::string editor::Stream::bufferTypeToString(BufferType type) {
    return StreamSchema::bufferType().toString(type);
}

BufferType editor::Stream::stringToBufferType(const std::string& str) {
    return StreamSchema::bufferType().fromString(str);
}

// BufferUsage enum conversion
std::string editor::Stream::bufferUsageToString(BufferUsage usage) {
    return StreamSchema::bufferUsage().toString(usage);
}

BufferUsage editor::Stream::stringToBufferUsage(const std::string& str) {
    return StreamSchema::bufferUsage().fromString(str);
}

// AttributeType enum conversion
std::string editor::Stream::attributeTypeToString(AttributeType type) {
    return StreamSchema::attributeType().toString(type);
}

AttributeType editor::Stream::stringToAttributeType(const std::string& str) {
    return StreamSchema::attributeType().fromString(str);
}

// AttributeDataType enum conversion
std::string editor::Stream::attributeDataTypeToString(AttributeDataType type) {
    return StreamSchema::attributeDataType().toString(type);
}

AttributeDataType editor::Stream::stringToAttributeDataType(const std::string& str) {
    return StreamSchema::attributeDataType().fromString(str);
}

// CullingMode enum conversion
std::string editor::Stream::cullingModeToString(CullingMode mode) {
    return StreamSchema::cullingMode().toString(mode);
}

CullingMode editor::Stream::stringToCullingMode(const std::string& str) {
    return StreamSchema::cullingMode().fromString(str);
}

// WindingOrder enum conversion
std::string editor::Stream::windingOrderToString(WindingOrder order) {
    return StreamSchema::windingOrder().toString(order);
}

WindingOrder editor::Stream::stringToWindingOrder(const std::string& str) {
    return StreamSchema::windingOrder().fromString(str);
}

// TextureFilter enum conversion
std::string editor::Stream::textureFilterToString(TextureFilter filter) {
    return StreamSchema::textureFilter().toString(filter);
}

TextureFilter editor::Stream::stringToTextureFilter(const std::string& str) {
    return StreamSchema::textureFilter().fromString(str);
}

// TextureWrap enum conversion
std::string editor::Stream::textureWrapToString(TextureWrap wrap) {
    return StreamSchema::textureWrap().toString(wrap);
}

TextureWrap editor::Stream::stringToTextureWrap(const std::string& str) {
    return StreamSchema::textureWrap().fromString(str);
}

std::string editor::Stream::textureAlphaBorderToString(TextureAlphaBorder alphaBorder) {
    return StreamSchema::textureAlphaBorder().toString(alphaBorder);
}

TextureAlphaBorder editor::Stream::stringToTextureAlphaBorder(const std::string& str) {
    return StreamSchema::textureAlphaBorder().fromString(str);
}

std::string editor::Stream::anchorPresetToString(AnchorPreset preset) {
    return StreamSchema::anchorPreset().toString(preset);
}

AnchorPreset editor::Stream::stringToAnchorPreset(const std::string& str) {
    return StreamSchema::anchorPreset().fromString(str);
}

std::string editor::Stream::materialAlphaModeToString(MaterialAlphaMode mode) {
    return StreamSchema::materialAlphaMode().toString(mode);
}

MaterialAlphaMode editor::Stream::stringToMaterialAlphaMode(const std::string& str) {
    return StreamSchema::materialAlphaMode().fromString(str);
}

std::string editor::Stream::materialBlendModeToString(MaterialBlendMode mode) {
    return StreamSchema::materialBlendMode().toString(mode);
}

MaterialBlendMode editor::Stream::stringToMaterialBlendMode(const std::string& str) {
    return StreamSchema::materialBlendMode().fromString(str);
}

std::string editor::Stream::materialCullModeToString(MaterialCullMode mode) {
    return StreamSchema::materialCullMode().toString(mode);
}

MaterialCullMode editor::Stream::stringToMaterialCullMode(const std::string& str) {
    return StreamSchema::materialCullMode().fromString(str);
}

std::string editor::Stream::materialDepthWriteToString(MaterialDepthWrite mode) {
    return StreamSchema::materialDepthWrite().toString(mode);
}

MaterialDepthWrite editor::Stream::stringToMaterialDepthWrite(const std::string& str) {
    return StreamSchema::materialDepthWrite().fromString(str);
}

std::string editor::Stream::materialDepthTestToString(MaterialDepthTest mode) {
    return StreamSchema::materialDepthTest().toString(mode);
}

MaterialDepthTest editor::Stream::stringToMaterialDepthTest(const std::string& str) {
    return StreamSchema::materialDepthTest().fromString(str);
}

std::string editor::Stream::materialLightingToString(MaterialLighting mode) {
    return StreamSchema::materialLighting().toString(mode);
}

MaterialLighting editor::Stream::stringToMaterialLighting(const std::string& str) {
    return StreamSchema::materialLighting().fromString(str);
}

std::string editor::Stream::lightShadingToString(LightShading shading) {
    return StreamSchema::lightShading().toString(shading);
}

LightShading editor::Stream::stringToLightShading(const std::string& str) {
    return StreamSchema::lightShading().fromString(str);
}

void editor::Stream::encodeComponentShaderLibrary(YAML::Node& node, const std::vector<std::pair<std::string, Texture>>& textures,
        const std::vector<std::string>& features, const MaterialBlendMode* blendMode, const MaterialDepthWrite* depthWrite, bool embedData) {
    // written only away from their defaults, like the material's
    if (!textures.empty()) {
        YAML::Node texturesNode = encodeShaderTextures(textures, embedData);
        if (texturesNode.size() > 0) node["shaderTextures"] = texturesNode;
    }
    if (!features.empty()) node["shaderFeatures"] = encodeShaderFeatures(features);
    if (blendMode && *blendMode != MaterialBlendMode::AUTO) node["blendMode"] = materialBlendModeToString(*blendMode);
    if (depthWrite && *depthWrite != MaterialDepthWrite::AUTO) node["depthWrite"] = materialDepthWriteToString(*depthWrite);
}

void editor::Stream::decodeComponentShaderLibrary(const YAML::Node& node, std::vector<std::pair<std::string, Texture>>& textures,
        std::vector<std::string>& features, MaterialBlendMode* blendMode, MaterialDepthWrite* depthWrite) {
    // not merged: an absent key is the default
    textures = decodeShaderTextures(node["shaderTextures"]);
    features = decodeShaderFeatures(node["shaderFeatures"]);
    if (blendMode) *blendMode = node["blendMode"] ? decodeEnum(node, "blendMode", StreamSchema::materialBlendMode()) : MaterialBlendMode::AUTO;
    if (depthWrite) *depthWrite = node["depthWrite"] ? decodeEnum(node, "depthWrite", StreamSchema::materialDepthWrite()) : MaterialDepthWrite::AUTO;
}

std::string editor::Stream::materialStencilCompareToString(MaterialStencilCompare compare) {
    return StreamSchema::materialStencilCompare().toString(compare);
}

MaterialStencilCompare editor::Stream::stringToMaterialStencilCompare(const std::string& str) {
    return StreamSchema::materialStencilCompare().fromString(str);
}

std::string editor::Stream::materialStencilOpToString(MaterialStencilOp op) {
    return StreamSchema::materialStencilOp().toString(op);
}

MaterialStencilOp editor::Stream::stringToMaterialStencilOp(const std::string& str) {
    return StreamSchema::materialStencilOp().fromString(str);
}

std::string editor::Stream::materialPassOrderToString(MaterialPassOrder order) {
    return StreamSchema::materialPassOrder().toString(order);
}

MaterialPassOrder editor::Stream::stringToMaterialPassOrder(const std::string& str) {
    return StreamSchema::materialPassOrder().fromString(str);
}

std::string editor::Stream::colorWriteToString(int colorWrite) {
    return ShaderManifest::getColorWriteName(colorWrite);
}

int editor::Stream::stringToColorWrite(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (lower == "auto") return -1;
    return ShaderManifest::parseColorWrite(lower); // -1 (AUTO) for anything unreadable
}

YAML::Node editor::Stream::encodeStencil(const MaterialStencil& stencil) {
    const MaterialStencil defaults;
    YAML::Node node(YAML::NodeType::Map);
    node["compare"] = materialStencilCompareToString(stencil.compare);
    if (stencil.ref != defaults.ref) node["ref"] = YamlNumbers::integerText(stencil.ref);
    if (stencil.readMask != defaults.readMask) node["readMask"] = YamlNumbers::integerText(stencil.readMask);
    if (stencil.writeMask != defaults.writeMask) node["writeMask"] = YamlNumbers::integerText(stencil.writeMask);
    if (stencil.pass != defaults.pass) node["pass"] = materialStencilOpToString(stencil.pass);
    if (stencil.fail != defaults.fail) node["fail"] = materialStencilOpToString(stencil.fail);
    if (stencil.depthFail != defaults.depthFail) node["depthFail"] = materialStencilOpToString(stencil.depthFail);
    return node;
}

MaterialStencil editor::Stream::decodeStencil(const YAML::Node& node) {
    StreamLoad::checkNestedKeys(node, NestedKind::Stencil);
    // not merged: encodeStencil omits the defaults
    MaterialStencil stencil;
    if (!node || !node.IsMap()) return stencil;
    if (node["compare"]) stencil.compare = decodeEnum(node, "compare", StreamSchema::materialStencilCompare());
    if (node["ref"]) stencil.ref = YamlNumbers::as<int>(node["ref"]);
    if (node["readMask"]) stencil.readMask = YamlNumbers::as<int>(node["readMask"]);
    if (node["writeMask"]) stencil.writeMask = YamlNumbers::as<int>(node["writeMask"]);
    if (node["pass"]) stencil.pass = decodeEnum(node, "pass", StreamSchema::materialStencilOp());
    if (node["fail"]) stencil.fail = decodeEnum(node, "fail", StreamSchema::materialStencilOp());
    if (node["depthFail"]) stencil.depthFail = decodeEnum(node, "depthFail", StreamSchema::materialStencilOp());
    return stencil;
}

void editor::Stream::encodeStencilAndColorWrite(YAML::Node& node, const MaterialStencil& stencil, int colorWrite) {
    if (stencil != MaterialStencil()) node["stencil"] = encodeStencil(stencil);
    if (colorWrite >= 0) node["colorWrite"] = colorWriteToString(colorWrite);
}

void editor::Stream::decodeStencilAndColorWrite(const YAML::Node& node, MaterialStencil& stencil, int& colorWrite) {
    // an absent key is the default (AUTO)
    stencil = decodeStencil(node["stencil"]);
    colorWrite = node["colorWrite"] ? stringToColorWrite(node["colorWrite"].as<std::string>()) : -1;
}

YAML::Node editor::Stream::encodeMaterialPass(const MaterialPass& pass, bool embedTextureData) {
    YAML::Node node(YAML::NodeType::Map);
    if (!pass.shader.empty()) encodeShaderRef(node, "shader", pass.shader);
    if (!pass.shaderParams.empty()) node["shaderParams"] = encodeShaderUniforms(pass.shaderParams);
    if (!pass.shaderTextures.empty()) {
        YAML::Node textures = encodeShaderTextures(pass.shaderTextures, embedTextureData);
        if (textures.size() > 0) node["shaderTextures"] = textures;
    }
    if (!pass.shaderFeatures.empty()) node["shaderFeatures"] = encodeShaderFeatures(pass.shaderFeatures);
    if (pass.blendMode != MaterialBlendMode::AUTO) node["blendMode"] = materialBlendModeToString(pass.blendMode);
    if (pass.cullMode != MaterialCullMode::AUTO) node["cullMode"] = materialCullModeToString(pass.cullMode);
    if (pass.depthWrite != MaterialDepthWrite::AUTO) node["depthWrite"] = materialDepthWriteToString(pass.depthWrite);
    if (pass.depthTest != MaterialDepthTest::AUTO) node["depthTest"] = materialDepthTestToString(pass.depthTest);
    if (pass.depthOffsetFactor != 0.0f) node["depthOffsetFactor"] = YamlNumbers::text(finiteOr(pass.depthOffsetFactor));
    if (pass.depthOffsetUnits != 0.0f) node["depthOffsetUnits"] = YamlNumbers::text(finiteOr(pass.depthOffsetUnits));
    encodeStencilAndColorWrite(node, pass.stencil, pass.colorWrite);
    if (pass.order != MaterialPassOrder::AFTER) node["order"] = materialPassOrderToString(pass.order);
    return node;
}

MaterialPass editor::Stream::decodeMaterialPass(const YAML::Node& node) {
    StreamLoad::checkNestedKeys(node, NestedKind::MaterialPass);
    // not merged: encodeMaterialPass omits the defaults
    MaterialPass pass;
    if (!node || !node.IsMap()) return pass;
    pass.shader = decodeShaderRef(node, "shader");
    pass.shaderParams = decodeShaderUniforms(node["shaderParams"]);
    pass.shaderTextures = decodeShaderTextures(node["shaderTextures"]);
    pass.shaderFeatures = decodeShaderFeatures(node["shaderFeatures"]);
    if (node["blendMode"]) pass.blendMode = decodeEnum(node, "blendMode", StreamSchema::materialBlendMode());
    if (node["cullMode"]) pass.cullMode = decodeEnum(node, "cullMode", StreamSchema::materialCullMode());
    if (node["depthWrite"]) pass.depthWrite = decodeEnum(node, "depthWrite", StreamSchema::materialDepthWrite());
    if (node["depthTest"]) pass.depthTest = decodeEnum(node, "depthTest", StreamSchema::materialDepthTest());
    pass.depthOffsetFactor = decodeFinite(node["depthOffsetFactor"], 0.0f);
    pass.depthOffsetUnits = decodeFinite(node["depthOffsetUnits"], 0.0f);
    decodeStencilAndColorWrite(node, pass.stencil, pass.colorWrite);
    if (node["order"]) pass.order = decodeEnum(node, "order", StreamSchema::materialPassOrder());
    return pass;
}

YAML::Node editor::Stream::encodeShaderTextures(const std::vector<std::pair<std::string, Texture>>& textures, bool embedData) {
    YAML::Node node(YAML::NodeType::Map);
    for (const auto& [uniform, texture] : textures) {
        // an empty one binds the manifest default, like no entry
        if (texture.empty() && !hasTexturePath(texture) && !CameraTextureLink::isLinkTexture(texture)) continue;
        node[uniform] = encodeTexture(texture, embedData);
    }
    return node;
}

std::vector<std::pair<std::string, Texture>> editor::Stream::decodeShaderTextures(const YAML::Node& node) {
    std::vector<std::pair<std::string, Texture>> textures;
    if (node && node.IsMap()) {
        for (const auto& entry : node) {
            textures.push_back({entry.first.as<std::string>(), decodeTexture(entry.second)});
        }
    }
    return textures;
}

YAML::Node editor::Stream::encodeShaderFeatures(const std::vector<std::string>& features) {
    YAML::Node node(YAML::NodeType::Sequence);
    node.SetStyle(YAML::EmitterStyle::Flow);
    for (const std::string& feature : features) {
        node.push_back(feature);
    }
    return node;
}

std::vector<std::string> editor::Stream::decodeShaderFeatures(const YAML::Node& node) {
    std::vector<std::string> features;
    if (node && node.IsSequence()) {
        for (const auto& entry : node) {
            if (entry.IsScalar()) features.push_back(entry.as<std::string>());
        }
    }
    return features;
}

std::string editor::Stream::scriptPropertyTypeToString(ScriptPropertyType type) {
    return StreamSchema::scriptPropertyType().toString(type);
}

ScriptPropertyType editor::Stream::stringToScriptPropertyType(const std::string& str) {
    return StreamSchema::scriptPropertyType().fromString(str);
}

std::string editor::Stream::particleEmitterShapeToString(ParticleEmitterShape shape) {
    return StreamSchema::particleEmitterShape().toString(shape);
}

ParticleEmitterShape editor::Stream::stringToParticleEmitterShape(const std::string& str) {
    return StreamSchema::particleEmitterShape().fromString(str);
}

namespace {

std::string terrainBrushModeToString(editor::TerrainBrushMode mode) {
    return editor::StreamSchema::terrainBrushMode().toString(mode);
}

std::string terrainBrushShapeToString(editor::TerrainBrushShape shape) {
    return editor::StreamSchema::terrainBrushShape().toString(shape);
}

std::string terrainBrushFalloffToString(editor::TerrainBrushFalloff falloff) {
    return editor::StreamSchema::terrainBrushFalloff().toString(falloff);
}

} // namespace

std::string editor::Stream::layoutGroupTypeToString(LayoutGroupType type) {
    return StreamSchema::layoutGroupType().toString(type);
}

std::string editor::Stream::canvasGroupRenderModeToString(CanvasGroupRenderMode mode) {
    return mode == CanvasGroupRenderMode::FLATTEN ? "FLATTEN" : "PER_ELEMENT";
}

CanvasGroupRenderMode editor::Stream::stringToCanvasGroupRenderMode(const std::string& str) {
    std::string upper = str;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c){ return (char)std::toupper(c); });
    return upper == "FLATTEN" ? CanvasGroupRenderMode::FLATTEN : CanvasGroupRenderMode::PER_ELEMENT;
}

LayoutGroupType editor::Stream::stringToLayoutGroupType(const std::string& str) {
    return StreamSchema::layoutGroupType().fromString(str);
}

std::string editor::Stream::scrollbarTypeToString(ScrollbarType type) {
    return StreamSchema::scrollbarType().toString(type);
}

ScrollbarType editor::Stream::stringToScrollbarType(const std::string& str) {
    return StreamSchema::scrollbarType().fromString(str);
}

std::string editor::Stream::progressbarTypeToString(ProgressbarType type) {
    return StreamSchema::progressbarType().toString(type);
}

ProgressbarType editor::Stream::stringToProgressbarType(const std::string& str) {
    return StreamSchema::progressbarType().fromString(str);
}

std::string editor::Stream::scalingModeToString(Scaling mode) {
    return StreamSchema::scalingMode().toString(mode);
}

Scaling editor::Stream::stringToScalingMode(const std::string& str) {
    return StreamSchema::scalingMode().fromString(str);
}

std::string editor::Stream::textureStrategyToString(TextureStrategy strategy) {
    return StreamSchema::textureStrategy().toString(strategy);
}

TextureStrategy editor::Stream::stringToTextureStrategy(const std::string& str) {
    return StreamSchema::textureStrategy().fromString(str);
}

std::string editor::Stream::windowModeToString(WindowMode mode) {
    return StreamSchema::windowMode().toString(mode);
}

editor::WindowMode editor::Stream::stringToWindowMode(const std::string& str) {
    return StreamSchema::windowMode().fromString(str);
}

std::string editor::Stream::lightTypeToString(LightType type) {
    return StreamSchema::lightType().toString(type);
}

LightType editor::Stream::stringToLightType(const std::string& str) {
    return StreamSchema::lightType().fromString(str);
}

std::string editor::Stream::fogTypeToString(FogType type) {
    return StreamSchema::fogType().toString(type);
}

FogType editor::Stream::stringToFogType(const std::string& str) {
    return StreamSchema::fogType().fromString(str);
}

static std::string shadowQualityToString(ShadowQuality quality) {
    return editor::StreamSchema::shadowQuality().toString(quality);
}

static std::string pbrTierToString(PbrTier tier) {
    return editor::StreamSchema::pbrTier().toString(tier);
}

static PbrTier stringToPbrTier(const std::string& name) {
    return editor::StreamSchema::pbrTier().fromString(name);
}

static std::string sceneColorScaleToString(SceneColorScale scale) {
    return editor::StreamSchema::sceneColorScale().toString(scale);
}

std::string editor::Stream::lightStateToString(LightState state) {
    return StreamSchema::lightState().toString(state);
}

LightState editor::Stream::stringToLightState(const std::string& str) {
    return StreamSchema::lightState().fromString(str);
}

std::string editor::Stream::audioSourceStateToString(AudioSourceState state) {
    return StreamSchema::audioSourceState().toString(state);
}

AudioSourceState editor::Stream::stringToAudioSourceState(const std::string& str) {
    return StreamSchema::audioSourceState().fromString(str);
}

std::string editor::Stream::uiDocumentRenderModeToString(UIDocumentRenderMode mode) {
    return StreamSchema::uiDocumentRenderMode().toString(mode);
}

UIDocumentRenderMode editor::Stream::stringToUIDocumentRenderMode(const std::string& str) {
    return StreamSchema::uiDocumentRenderMode().fromString(str);
}

FluidOutput editor::Stream::stringToFluidOutput(const std::string& str) {
    return StreamSchema::fluidOutput().fromString(str);
}

std::string editor::Stream::latticeInterpolationToString(LatticeInterpolation interpolation) {
    return StreamSchema::latticeInterpolation().toString(interpolation);
}

LatticeInterpolation editor::Stream::stringToLatticeInterpolation(const std::string& str) {
    return StreamSchema::latticeInterpolation().fromString(str);
}

std::string editor::Stream::latticeOutsideToString(LatticeOutside outside) {
    return StreamSchema::latticeOutside().toString(outside);
}

LatticeOutside editor::Stream::stringToLatticeOutside(const std::string& str) {
    return StreamSchema::latticeOutside().fromString(str);
}

std::string editor::Stream::latticeJiggleAnchorToString(LatticeJiggleAnchor anchor) {
    return StreamSchema::latticeJiggleAnchor().toString(anchor);
}

LatticeJiggleAnchor editor::Stream::stringToLatticeJiggleAnchor(const std::string& str) {
    return StreamSchema::latticeJiggleAnchor().fromString(str);
}

std::string editor::Stream::latticeStageToString(LatticeStage stage) {
    return StreamSchema::latticeStage().toString(stage);
}

LatticeStage editor::Stream::stringToLatticeStage(const std::string& str) {
    return StreamSchema::latticeStage().fromString(str);
}

std::string editor::Stream::latticeMaskChannelToString(LatticeMaskChannel mask) {
    return StreamSchema::latticeMaskChannel().toString(mask);
}

LatticeMaskChannel editor::Stream::stringToLatticeMaskChannel(const std::string& str) {
    return StreamSchema::latticeMaskChannel().fromString(str);
}

std::string editor::Stream::audioSourceAttenuationToString(AudioSourceAttenuation attenuation) {
    return StreamSchema::audioSourceAttenuation().toString(attenuation);
}

AudioSourceAttenuation editor::Stream::stringToAudioSourceAttenuation(const std::string& str) {
    return StreamSchema::audioSourceAttenuation().fromString(str);
}

std::string editor::Stream::uiEventStateToString(UIEventState state) {
    return StreamSchema::uiEventState().toString(state);
}

UIEventState editor::Stream::stringToUIEventState(const std::string& str) {
    return StreamSchema::uiEventState().fromString(str);
}

std::string editor::Stream::cameraTypeToString(CameraType type) {
    return StreamSchema::cameraType().toString(type);
}

CameraType editor::Stream::stringToCameraType(const std::string& str) {
    return StreamSchema::cameraType().fromString(str);
}

std::string editor::Stream::pivotPresetToString(PivotPreset preset) {
    return StreamSchema::pivotPreset().toString(preset);
}

PivotPreset editor::Stream::stringToPivotPreset(const std::string& str) {
    return StreamSchema::pivotPreset().fromString(str);
}

std::string editor::Stream::scriptTypeToString(ScriptType type) {
    return StreamSchema::scriptType().toString(type);
}

ScriptType editor::Stream::stringToScriptType(const std::string& str) {
    return StreamSchema::scriptType().fromString(str);
}

YAML::Node editor::Stream::encodeCanvasScaler(const CanvasScalerSettings& settings){
    const CanvasScalerSettings d;
    YAML::Node node(YAML::NodeType::Map);
    if (settings.mode != d.mode)
        node["mode"] = CanvasScaler::modeName(settings.mode);
    if (settings.scaleFactor != d.scaleFactor)
        node["scaleFactor"] = settings.scaleFactor;
    if (settings.referenceResolution != d.referenceResolution)
        node["referenceResolution"] = encodeVector2(settings.referenceResolution);
    if (settings.screenMatchMode != d.screenMatchMode)
        node["screenMatchMode"] = CanvasScaler::matchModeName(settings.screenMatchMode);
    if (settings.matchWidthOrHeight != d.matchWidthOrHeight)
        node["matchWidthOrHeight"] = settings.matchWidthOrHeight;
    if (settings.physicalUnit != d.physicalUnit)
        node["physicalUnit"] = CanvasScaler::unitName(settings.physicalUnit);
    if (settings.fallbackScreenDPI != d.fallbackScreenDPI)
        node["fallbackScreenDPI"] = settings.fallbackScreenDPI;
    return node;
}

CanvasScalerSettings editor::Stream::decodeCanvasScaler(const YAML::Node& node){
    CanvasScalerSettings settings;
    if (!node || !node.IsMap())
        return settings;
    if (node["mode"] && !CanvasScaler::parseMode(node["mode"].as<std::string>().c_str(), settings.mode))
        Log::warn("Unknown Canvas Scaler mode '%s', using follow_game_canvas", node["mode"].as<std::string>().c_str());
    if (node["scaleFactor"])
        settings.scaleFactor = decodeFinite(node["scaleFactor"], settings.scaleFactor);
    if (node["referenceResolution"])
        settings.referenceResolution = decodeVector2(node["referenceResolution"]);
    if (node["screenMatchMode"] && !CanvasScaler::parseMatchMode(node["screenMatchMode"].as<std::string>().c_str(), settings.screenMatchMode))
        Log::warn("Unknown Canvas Scaler screen match mode '%s'", node["screenMatchMode"].as<std::string>().c_str());
    if (node["matchWidthOrHeight"])
        settings.matchWidthOrHeight = decodeFinite(node["matchWidthOrHeight"], settings.matchWidthOrHeight);
    if (node["physicalUnit"] && !CanvasScaler::parseUnit(node["physicalUnit"].as<std::string>().c_str(), settings.physicalUnit))
        Log::warn("Unknown Canvas Scaler physical unit '%s'", node["physicalUnit"].as<std::string>().c_str());
    if (node["fallbackScreenDPI"])
        settings.fallbackScreenDPI = decodeFinite(node["fallbackScreenDPI"], settings.fallbackScreenDPI);
    return settings;
}

YAML::Node editor::Stream::encodeVector2(const Vector2& vec){
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node[0] = YamlNumbers::text(finiteOr(vec.x));
    node[1] = YamlNumbers::text(finiteOr(vec.y));
    return node;
}

Vector2 editor::Stream::decodeVector2(const YAML::Node& node) {
    if (!numberSequence(node, 2)) {
        return Vector2();
    }

    return Vector2(decodeFinite(node[0], 0.0f), decodeFinite(node[1], 0.0f));
}

YAML::Node editor::Stream::encodeVector3(const Vector3& vec) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node[0] = YamlNumbers::text(finiteOr(vec.x));
    node[1] = YamlNumbers::text(finiteOr(vec.y));
    node[2] = YamlNumbers::text(finiteOr(vec.z));
    return node;
}

Vector3 editor::Stream::decodeVector3(const YAML::Node& node) {
    if (!numberSequence(node, 3)) {
        return Vector3();
    }

    return Vector3(decodeFinite(node[0], 0.0f), decodeFinite(node[1], 0.0f), decodeFinite(node[2], 0.0f));
}

YAML::Node editor::Stream::encodeVector4(const Vector4& vec){
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node[0] = YamlNumbers::text(finiteOr(vec.x));
    node[1] = YamlNumbers::text(finiteOr(vec.y));
    node[2] = YamlNumbers::text(finiteOr(vec.z));
    node[3] = YamlNumbers::text(finiteOr(vec.w));
    return node;
}

Vector4 editor::Stream::decodeVector4(const YAML::Node& node) {
    if (!numberSequence(node, 4)) {
        return Vector4();
    }

    return Vector4(decodeFinite(node[0], 0.0f), decodeFinite(node[1], 0.0f), decodeFinite(node[2], 0.0f), decodeFinite(node[3], 0.0f));
}

YAML::Node editor::Stream::encodeCurve(const Curve& curve) {
    return curvecodec::curveToYaml(curve);
}

Curve editor::Stream::decodeCurve(const YAML::Node& node, const Curve& fallback) {
    if (!node || node.IsNull()) return fallback;
    Curve curve;
    std::string reason;
    if (!curvecodec::curveFromYaml(node, curve, reason)) {
        Out::warning("Curve: %s: the default curve is used", reason.c_str());
        return fallback;
    }
    return curve;
}

YAML::Node editor::Stream::encodeGradient(const Gradient& gradient) {
    return curvecodec::gradientToYaml(gradient);
}

Gradient editor::Stream::decodeGradient(const YAML::Node& node, const Gradient& fallback) {
    if (!node || node.IsNull()) return fallback;
    Gradient gradient;
    std::string reason;
    if (!curvecodec::gradientFromYaml(node, gradient, reason)) {
        Out::warning("Gradient: %s: the default gradient is used", reason.c_str());
        return fallback;
    }
    return gradient;
}

YAML::Node editor::Stream::encodeShaderUniforms(const ShaderUniformValues& uniforms) {
    YAML::Node node;
    for (const auto& uniform : uniforms) {
        node[uniform.first] = encodeVector4(uniform.second);
    }
    return node;
}

ShaderUniformValues editor::Stream::decodeShaderUniforms(const YAML::Node& node) {
    ShaderUniformValues uniforms;
    if (node && node.IsMap()) {
        for (const auto& uniform : node) {
            std::string name = uniform.first.as<std::string>();
            if (!ShaderUniforms::isReserved(name))
                uniforms.push_back({name, decodeVector4(uniform.second)});
        }
    }
    return uniforms;
}

// {member: global}, written only when not empty
YAML::Node editor::Stream::encodeShaderGlobalBindings(const ShaderGlobalBindings& bindings) {
    YAML::Node node(YAML::NodeType::Map);
    for (const auto& binding : bindings) node[binding.first] = binding.second;
    return node;
}

ShaderGlobalBindings editor::Stream::decodeShaderGlobalBindings(const YAML::Node& node) {
    ShaderGlobalBindings bindings;
    if (!node || !node.IsMap()) return bindings;
    for (const auto& binding : node) {
        if (!binding.second.IsScalar()) continue;
        const std::string member = binding.first.as<std::string>();
        const std::string global = binding.second.as<std::string>();
        if (!member.empty() && !global.empty()) bindings.push_back({member, global});
    }
    return bindings;
}

YAML::Node editor::Stream::encodeQuaternion(const Quaternion& quat) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node[0] = YamlNumbers::text(finiteOr(quat.w, 1.0f));
    node[1] = YamlNumbers::text(finiteOr(quat.x));
    node[2] = YamlNumbers::text(finiteOr(quat.y));
    node[3] = YamlNumbers::text(finiteOr(quat.z));
    return node;
}

Quaternion editor::Stream::decodeQuaternion(const YAML::Node& node) {
    if (!numberSequence(node, 4)) {
        return Quaternion::IDENTITY;
    }

    return Quaternion(decodeFinite(node[0], 1.0f), decodeFinite(node[1], 0.0f), decodeFinite(node[2], 0.0f), decodeFinite(node[3], 0.0f));
}

YAML::Node editor::Stream::encodeRect(const Rect& rect) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node[0] = YamlNumbers::text(finiteOr(rect.getX()));
    node[1] = YamlNumbers::text(finiteOr(rect.getY()));
    node[2] = YamlNumbers::text(finiteOr(rect.getWidth()));
    node[3] = YamlNumbers::text(finiteOr(rect.getHeight()));
    return node;
}

Rect editor::Stream::decodeRect(const YAML::Node& node) {
    if (!numberSequence(node, 4)) {
        return Rect();
    }

    return Rect(decodeFinite(node[0], 0.0f), decodeFinite(node[1], 0.0f), decodeFinite(node[2], 0.0f), decodeFinite(node[3], 0.0f));
}

YAML::Node editor::Stream::encodeMatrix4(const Matrix4& mat) {
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    Matrix4 safeMatrix = mat.isValid() ? mat : Matrix4();
    for (int i = 0; i < 4; i++) {
        YAML::Node row;
        row.SetStyle(YAML::EmitterStyle::Flow);
        for (int j = 0; j < 4; j++) {
            row[j] = YamlNumbers::text(safeMatrix[i][j]);
        }
        node.push_back(row);
    }
    return node;
}

Matrix4 editor::Stream::decodeMatrix4(const YAML::Node& node) {
    Matrix4 mat;
    if (!node || !node.IsSequence() || node.size() < 4) {
        return mat;
    }

    for (int i = 0; i < 4; i++) {
        if (!node[i] || !node[i].IsSequence() || node[i].size() < 4) {
            return Matrix4();
        }

        for (int j = 0; j < 4; j++) {
            mat[i][j] = decodeFinite(node[i][j], i == j ? 1.0f : 0.0f);
        }
    }
    return mat;
}

std::string editor::Stream::portablePath(std::string path) {
    return FileUtils::portablePath(std::move(path));
}

std::string editor::Stream::portablePath(const std::filesystem::path& path) {
    return portablePath(FileUtils::pathToGenericUtf8(path));
}

void editor::Stream::encodeAssetRef(YAML::Node& node, const std::string& key, const AssetRef& ref) {
    node[key] = portablePath(ref.path);
    if (!ref.guid.isNull()) {
        node[key + "Guid"] = ref.guid.str();
    }
}

AssetRef editor::Stream::decodeAssetRef(const YAML::Node& node, const std::string& key) {
    AssetRef ref;
    const YAML::Node value = node[key];
    if (value && !value.IsNull()) {
        ref.path = portablePath(value.as<std::string>());
    }
    const YAML::Node guid = node[key + "Guid"];
    if (guid && guid.IsScalar()) {
        ref.guid = Guid::parse(guid.as<std::string>());
    }
    return ref;
}

std::string editor::Stream::shaderRefGuids(const ShaderRef& ref) {
    if (ref.vertGuid.isNull() && ref.fragGuid.isNull()) {
        return std::string();
    }
    return ref.vertGuid.str() + "|" + ref.fragGuid.str();
}

void editor::Stream::parseShaderRefGuids(const std::string& text, ShaderRef& ref) {
    const size_t bar = text.find('|');
    ref.vertGuid = Guid::parse(bar == std::string::npos ? text : text.substr(0, bar));
    ref.fragGuid = bar == std::string::npos ? Guid() : Guid::parse(text.substr(bar + 1));
}

void editor::Stream::encodeShaderRef(YAML::Node& node, const std::string& key, const ShaderRef& ref) {
    node[key] = portablePath(ref.path);
    const std::string guids = shaderRefGuids(ref);
    if (!guids.empty()) {
        node[key + "Guid"] = guids;
    }
}

ShaderRef editor::Stream::decodeShaderRef(const YAML::Node& node, const std::string& key) {
    ShaderRef ref;
    const YAML::Node value = node[key];
    if (value && !value.IsNull()) {
        ref.path = portablePath(value.as<std::string>());
    }
    const YAML::Node guids = node[key + "Guid"];
    if (guids && guids.IsScalar()) {
        parseShaderRefGuids(guids.as<std::string>(), ref);
    }
    return ref;
}

YAML::Node editor::Stream::encodeTexture(const Texture& texture, bool embedData) {
    YAML::Node node;

    // camera render-to-texture link; checked before empty() because an
    // unresolved link has no framebuffer bound yet
    Entity linkedCamera = CameraTextureLink::parse(texture.getId());
    if (linkedCamera != NULL_ENTITY) {
        node["textureType"] = "2D";
        node["source"] = "camera";
        node["camera"] = linkedCamera;
        return node;
    }
    // a FluidSimulation's output (FluidTextureLink), the same way
    Entity linkedFluid = FluidTextureLink::parse(texture.getId());
    if (linkedFluid != NULL_ENTITY) {
        node["textureType"] = "2D";
        node["source"] = "fluid";
        node["fluid"] = linkedFluid;
        return node;
    }
    // a Video Player's render texture (VideoTextureLink), the same way
    Entity linkedVideo = VideoTextureLink::parse(texture.getId());
    if (linkedVideo != NULL_ENTITY) {
        node["textureType"] = "2D";
        node["source"] = "video";
        node["video"] = linkedVideo;
        return node;
    }

    if ((texture.empty() && !hasTexturePath(texture)) || texture.isFramebuffer())
        return node;

    const bool isCube = texture.isCubeMap() || (texture.getNumFaces() == 6);
    node["textureType"] = isCube ? "CUBE" : "2D";

    // Source selection: prefer filesystem paths; fall back to id only when there is no path.
    if (!isCube) {
        const std::string path = texture.getPath(0);
        if (!path.empty()) {
            node["source"] = "path";
            node["path"] = portablePath(path);
            if (!texture.getPathGuid(0).isNull()) node["guid"] = texture.getPathGuid(0).str();
        } else {
            TextureData* embeddedData = nullptr;
            if (embedData && getEmbeddedTextureData(texture, embeddedData)) {
                node["source"] = "data";
                node["id"] = texture.getId();
                node["width"] = embeddedData->getWidth();
                node["height"] = embeddedData->getHeight();
                node["channels"] = embeddedData->getChannels();
                node["size"] = embeddedData->getSize();
                node["colorFormat"] = colorFormatToString(embeddedData->getColorFormat());
                node["data"] = Base64::encode(static_cast<const unsigned char*>(embeddedData->getData()), embeddedData->getSize());
            } else {
                node["source"] = "id";
                node["id"] = texture.getId();
            }
        }
    } else {
        bool hasAnyNonZeroFace = false;
        bool hasAnyEmptyFace = false;
        for (int f = 1; f < 6; f++) {
            if (!texture.getPath((size_t)f).empty()) hasAnyNonZeroFace = true;
            if (texture.getPath((size_t)f).empty()) hasAnyEmptyFace = true;
        }

        const bool singleFileCube = !texture.getPath(0).empty() && !hasAnyNonZeroFace;
        if (singleFileCube) {
            node["cubeMode"] = "single";
            node["source"] = "path";
            node["path"] = portablePath(texture.getPath(0));
            if (!texture.getPathGuid(0).isNull()) node["guid"] = texture.getPathGuid(0).str();
        } else {
            node["cubeMode"] = "faces";
            node["source"] = "paths";
            YAML::Node pathsNode;
            pathsNode.SetStyle(YAML::EmitterStyle::Flow);
            for (int f = 0; f < 6; f++) {
                pathsNode.push_back(portablePath(texture.getPath((size_t)f)));
            }
            node["paths"] = pathsNode;
            bool hasGuids = false;
            for (int f = 0; f < 6; f++) hasGuids |= !texture.getPathGuid((size_t)f).isNull();
            if (hasGuids) {
                YAML::Node guidsNode;
                guidsNode.SetStyle(YAML::EmitterStyle::Flow);
                for (int f = 0; f < 6; f++) guidsNode.push_back(texture.getPathGuid((size_t)f).str());
                node["guids"] = guidsNode;
            }
            node["hasEmptyFaces"] = hasAnyEmptyFace;
        }
    }

    node["minFilter"] = textureFilterToString(texture.getMinFilter());
    node["magFilter"] = textureFilterToString(texture.getMagFilter());
    node["wrapU"] = textureWrapToString(texture.getWrapU());
    node["wrapV"] = textureWrapToString(texture.getWrapV());
    if (texture.getSvgScale() != 1.0f) node["svgScale"] = texture.getSvgScale();
    if (texture.getAlphaBorder() != TextureAlphaBorder::AUTO) node["alphaBorder"] = textureAlphaBorderToString(texture.getAlphaBorder());
    node["releaseDataAfterLoad"] = texture.isReleaseDataAfterLoad();
    return node;
}

namespace {
    // A model texture's id from a scene saved before the ids were relative (the model file's
    // absolute path on the machine that saved it): as the model keys it now
    std::string modelTextureId(const std::string& id) {
        return MeshSystem::migrateModelTextureId(id, System::instance().getAssetPath());
    }
}

Texture editor::Stream::decodeTexture(const YAML::Node& node) {
    StreamLoad::checkNestedKeys(node, NestedKind::Texture);
    Texture texture;
    if (node.IsMap()) { // Check if node has data
        const std::string textureType = node["textureType"] ? node["textureType"].as<std::string>() : std::string();
        const std::string source = node["source"] ? node["source"].as<std::string>() : std::string();

        if (textureType == "CUBE") {
            const std::string cubeMode = node["cubeMode"] ? node["cubeMode"].as<std::string>() : std::string();
            if (cubeMode == "single") {
                if (node["path"]) {
                    texture.setCubeMap(portablePath(node["path"].as<std::string>()));
                    if (node["guid"]) texture.setPathGuid(0, Guid::parse(node["guid"].as<std::string>()));
                }
            } else {
                if (node["paths"] && node["paths"].IsSequence() && node["paths"].size() == 6) {
                    std::array<std::string, 6> paths;
                    for (size_t f = 0; f < 6; f++) {
                        paths[f] = portablePath(node["paths"][f].as<std::string>());
                    }

                    // Set per-face paths individually (supports incomplete cubemaps).
                    for (size_t f = 0; f < 6; f++) {
                        texture.setCubePath(f, paths[f]);
                    }
                    const YAML::Node guids = node["guids"];
                    if (guids && guids.IsSequence() && guids.size() == 6) {
                        for (size_t f = 0; f < 6; f++) {
                            texture.setPathGuid(f, Guid::parse(guids[f].as<std::string>()));
                        }
                    }
                }
            }
        } else if (textureType == "2D") {
            if (source == "path") {
                if (node["path"]) {
                    texture.setPath(portablePath(node["path"].as<std::string>()));
                    if (node["guid"]) texture.setPathGuid(0, Guid::parse(node["guid"].as<std::string>()));
                }
            } else if (source == "data") {
                if (node["data"] && node["width"] && node["height"] && node["channels"]) {
                    std::vector<unsigned char> decodedData = Base64::decode(node["data"].as<std::string>());
                    const int width = YamlNumbers::as<int>(node["width"]);
                    const int height = YamlNumbers::as<int>(node["height"]);
                    const int channels = YamlNumbers::as<int>(node["channels"]);
                    const size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels);
                    const size_t dataSize = node["size"] ? YamlNumbers::as<size_t>(node["size"]) : expectedSize;
                    const std::string colorFormat = node["colorFormat"] ? node["colorFormat"].as<std::string>() : std::string();
                    const std::string textureId = node["id"] ? modelTextureId(node["id"].as<std::string>()) : std::string();

                    if (dataSize == expectedSize && decodedData.size() >= dataSize) {
                        setEmbeddedTextureData(texture, textureId, width, height, stringToColorFormat(colorFormat, channels), channels, decodedData);
                    }
                }
            } else if (source == "id") {
                if (node["id"]) {
                    texture.setId(modelTextureId(node["id"].as<std::string>()));
                }
            } else if (source == "camera") {
                // framebuffer is bound later by CameraTextureLink::resolve,
                // after every entity of the scene exists
                if (node["camera"]) {
                    texture.setId(CameraTextureLink::makeId(YamlNumbers::as<Entity>(node["camera"])));
                }
            } else if (source == "fluid") {
                // a FluidSimulation's output, bound by CameraTextureLink::resolve too
                if (node["fluid"]) {
                    texture.setId(FluidTextureLink::makeId(YamlNumbers::as<Entity>(node["fluid"])));
                }
            } else if (source == "video") {
                // a Video Player's render texture, bound by CameraTextureLink::resolve too
                if (node["video"]) {
                    texture.setId(VideoTextureLink::makeId(YamlNumbers::as<Entity>(node["video"])));
                }
            }
        }

        if (node["minFilter"]) texture.setMinFilter(decodeEnum(node, "minFilter", StreamSchema::textureFilter()));
        if (node["magFilter"]) texture.setMagFilter(decodeEnum(node, "magFilter", StreamSchema::textureFilter()));
        if (node["wrapU"]) texture.setWrapU(decodeEnum(node, "wrapU", StreamSchema::textureWrap()));
        if (node["wrapV"]) texture.setWrapV(decodeEnum(node, "wrapV", StreamSchema::textureWrap()));
        // Old scenes carry the scale inside the path ("?svgScale=N"), absorbed by setPath()
        if (node["svgScale"]) texture.setSvgScale(YamlNumbers::as<float>(node["svgScale"]));
        if (node["alphaBorder"]) texture.setAlphaBorder(decodeEnum(node, "alphaBorder", StreamSchema::textureAlphaBorder()));

        if (node["releaseDataAfterLoad"]) texture.setReleaseDataAfterLoad(YamlNumbers::as<bool>(node["releaseDataAfterLoad"]));
    }
    return texture;
}

YAML::Node editor::Stream::encodeBuffer(const Buffer& buffer) {
    YAML::Node node;

    // Encode buffer properties
    node["type"] = bufferTypeToString(buffer.getType());
    node["usage"] = bufferUsageToString(buffer.getUsage());
    node["stride"] = buffer.getStride();
    node["size"] = buffer.getSize();
    node["count"] = buffer.getCount();
    node["renderAttributes"] = buffer.isRenderAttributes();
    node["instanceBuffer"] = buffer.isInstanceBuffer();

    // Encode attributes
    YAML::Node attributesNode;
    for (const auto& [type, attr] : buffer.getAttributes()) {
        YAML::Node attrNode;
        attrNode["type"] = attributeTypeToString(type);
        attrNode["dataType"] = attributeDataTypeToString(attr.getDataType());
        attrNode["bufferName"] = attr.getBufferName();
        attrNode["elements"] = attr.getElements();
        attrNode["offset"] = attr.getOffset();
        attrNode["count"] = attr.getCount();
        attrNode["normalized"] = attr.getNormalized();
        attrNode["perInstance"] = attr.getPerInstance();
        attributesNode.push_back(attrNode);
    }
    node["attributes"] = attributesNode;

    // Encode buffer data if it exists
    if (buffer.getData() && buffer.getSize() > 0) {
        std::string base64Data = Base64::encode(buffer.getData(), buffer.getSize());
        node["data"] = base64Data;
    }

    return node;
}

void editor::Stream::decodeBuffer(Buffer& buffer, const YAML::Node& node) {
    if (!node.IsMap()) return;

    // Decode buffer properties
    buffer.setType(decodeEnum(node, "type", StreamSchema::bufferType()));
    buffer.setUsage(decodeEnum(node, "usage", StreamSchema::bufferUsage()));
    buffer.setStride(YamlNumbers::as<unsigned int>(node["stride"]));
    buffer.setSize(YamlNumbers::as<size_t>(node["size"]));
    buffer.setCount(YamlNumbers::as<unsigned int>(node["count"]));
    buffer.setRenderAttributes(YamlNumbers::as<bool>(node["renderAttributes"]));
    buffer.setInstanceBuffer(YamlNumbers::as<bool>(node["instanceBuffer"]));

    // Decode attributes
    if (node["attributes"]) {
        for (const auto& attrNode : node["attributes"]) {
            AttributeType type = decodeEnum(attrNode, "type", StreamSchema::attributeType());
            AttributeDataType dataType = decodeEnum(attrNode, "dataType", StreamSchema::attributeDataType());
            std::string bufferName = attrNode["bufferName"].as<std::string>();
            unsigned int elements = YamlNumbers::as<unsigned int>(attrNode["elements"]);
            size_t offset = YamlNumbers::as<size_t>(attrNode["offset"]);
            unsigned int count = YamlNumbers::as<unsigned int>(attrNode["count"]);
            bool normalized = YamlNumbers::as<bool>(attrNode["normalized"]);
            bool perInstance = YamlNumbers::as<bool>(attrNode["perInstance"]);

            Attribute attr(dataType, bufferName, elements, offset, count, normalized, perInstance);
            buffer.addAttribute(type, attr);
        }
    }

    // Decode buffer data if it exists
    if (node["data"]) {
        std::string base64Data = node["data"].as<std::string>();
        std::vector<unsigned char> decodedData = Base64::decode(base64Data);

        buffer.importData(decodedData.data(), decodedData.size());
    }
}

YAML::Node editor::Stream::encodeInterleavedBuffer(const InterleavedBuffer& buffer) {
    YAML::Node node = encodeBuffer(buffer);
    node["vertexSize"] = buffer.getVertexSize();
    return node;
}

void editor::Stream::decodeInterleavedBuffer(InterleavedBuffer& buffer, const YAML::Node& node) {
    decodeBuffer(buffer, node);

    if (node["vertexSize"]) {
        buffer.setVertexSize(YamlNumbers::as<unsigned int>(node["vertexSize"]));
    }
}

YAML::Node editor::Stream::encodeIndexBuffer(const IndexBuffer& buffer) {
    YAML::Node node = encodeBuffer(buffer);
    return node;
}

void editor::Stream::decodeIndexBuffer(IndexBuffer& buffer, const YAML::Node& node) {
    decodeBuffer(buffer, node);
}

YAML::Node editor::Stream::encodeExternalBuffer(const ExternalBuffer& buffer) {
    YAML::Node node = encodeBuffer(buffer); // Use base Buffer encoding
    node["name"] = buffer.getName(); // Add ExternalBuffer specific property
    return node;
}

void editor::Stream::decodeExternalBuffer(ExternalBuffer& buffer, const YAML::Node& node) {
    // Decoded over a copy of a model's mesh, the buffer still points at that model's bytes
    // and holds its attributes: the saved buffer replaces all of it, and its bytes are the
    // buffer's own (importData), not written through that pointer
    buffer.clearAll();
    decodeBuffer(buffer, node); // Use base Buffer decoding
    buffer.setName(node["name"].as<std::string>()); // Set ExternalBuffer specific property
}

YAML::Node editor::Stream::encodeSubmesh(const Submesh& submesh, bool embedTextureData, bool encodeVertexCount) {
    YAML::Node node;

    node["material"] = encodeMaterial(submesh.material, embedTextureData);
    node["textureRect"] = encodeRect(submesh.textureRect);
    node["primitiveType"] = primitiveTypeToString(submesh.primitiveType);
    if (encodeVertexCount) {
        node["vertexCount"] = YamlNumbers::integerText(submesh.vertexCount);
    }
    node["faceCulling"] = YamlNumbers::text(submesh.faceCulling);
    node["textureShadow"] = YamlNumbers::text(submesh.textureShadow);

    // The fields the next model load must keep instead of taking them from the file.
    if (submesh.overrideFields != 0) {
        node["overrideFields"] = submesh.overrideFields;
    }

    // Flags
    node["hasTexCoord1"] = YamlNumbers::text(submesh.hasTexCoord1);
    node["hasTexCoord2"] = YamlNumbers::text(submesh.hasTexCoord2);
    node["hasNormalMap"] = YamlNumbers::text(submesh.hasNormalMap);
    node["hasTangent"] = YamlNumbers::text(submesh.hasTangent);
    node["hasVertexColor3"] = YamlNumbers::text(submesh.hasVertexColor3);
    node["hasVertexColor4"] = YamlNumbers::text(submesh.hasVertexColor4);
    node["hasTextureRect"] = YamlNumbers::text(submesh.hasTextureRect);
    node["hasSkinning"] = YamlNumbers::text(submesh.hasSkinning);
    node["hasMorphTarget"] = YamlNumbers::text(submesh.hasMorphTarget);
    node["hasMorphNormal"] = YamlNumbers::text(submesh.hasMorphNormal);
    node["hasMorphTangent"] = YamlNumbers::text(submesh.hasMorphTangent);

    if (submesh.hasSkinningNormalization) {
        node["normAdjustJoint"] = YamlNumbers::text(submesh.normAdjustJoint);
        node["normAdjustWeight"] = YamlNumbers::text(submesh.normAdjustWeight);
    }

    return node;
}

Submesh editor::Stream::decodeSubmesh(const YAML::Node& node, const Submesh* oldSubmesh) {
    StreamLoad::checkNestedKeys(node, NestedKind::Submesh);
    Submesh submesh;
    if (oldSubmesh) {
        submesh = *oldSubmesh;
    }

    // All optional: an absent key keeps the value the submesh already has
    if (node["material"]) submesh.material = decodeMaterial(node["material"], &submesh.material);
    if (node["textureRect"]) submesh.textureRect = decodeRect(node["textureRect"]);
    if (node["primitiveType"]) submesh.primitiveType = decodeEnum(node, "primitiveType", StreamSchema::primitiveType());
    if (node["vertexCount"]) submesh.vertexCount = YamlNumbers::as<uint32_t>(node["vertexCount"]);
    if (node["faceCulling"]) submesh.faceCulling = YamlNumbers::as<bool>(node["faceCulling"]);
    if (node["textureShadow"]) submesh.textureShadow = YamlNumbers::as<bool>(node["textureShadow"]);

    // Omitted at its default, so an absent key is a value, not a reason to keep the old one.
    submesh.overrideFields = node["overrideFields"] ? YamlNumbers::as<uint64_t>(node["overrideFields"]) : 0;

    // Flags
    if (node["hasTexCoord1"]) submesh.hasTexCoord1 = YamlNumbers::as<bool>(node["hasTexCoord1"]);
    if (node["hasTexCoord2"]) submesh.hasTexCoord2 = YamlNumbers::as<bool>(node["hasTexCoord2"]);
    if (node["hasNormalMap"]) submesh.hasNormalMap = YamlNumbers::as<bool>(node["hasNormalMap"]);
    if (node["hasTangent"]) submesh.hasTangent = YamlNumbers::as<bool>(node["hasTangent"]);
    if (node["hasVertexColor3"]) submesh.hasVertexColor3 = YamlNumbers::as<bool>(node["hasVertexColor3"]);
    if (node["hasVertexColor4"]) submesh.hasVertexColor4 = YamlNumbers::as<bool>(node["hasVertexColor4"]);
    if (node["hasTextureRect"]) submesh.hasTextureRect = YamlNumbers::as<bool>(node["hasTextureRect"]);
    if (node["hasSkinning"]) submesh.hasSkinning = YamlNumbers::as<bool>(node["hasSkinning"]);
    if (node["hasMorphTarget"]) submesh.hasMorphTarget = YamlNumbers::as<bool>(node["hasMorphTarget"]);
    if (node["hasMorphNormal"]) submesh.hasMorphNormal = YamlNumbers::as<bool>(node["hasMorphNormal"]);
    if (node["hasMorphTangent"]) submesh.hasMorphTangent = YamlNumbers::as<bool>(node["hasMorphTangent"]);

    submesh.hasSkinningNormalization = false;
    if (node["normAdjustJoint"] || node["normAdjustWeight"]) {
        submesh.normAdjustJoint = node["normAdjustJoint"] ? YamlNumbers::as<float>(node["normAdjustJoint"]) : 1.0f;
        submesh.normAdjustWeight = node["normAdjustWeight"] ? YamlNumbers::as<float>(node["normAdjustWeight"]) : 1.0f;
        submesh.hasSkinningNormalization = true;
    }

    return submesh;
}

YAML::Node editor::Stream::encodeAABB(const AABB& aabb) {
    YAML::Node node;
    node["min"] = encodeVector3(aabb.getMinimum());
    node["max"] = encodeVector3(aabb.getMaximum());
    return node;
}

AABB editor::Stream::decodeAABB(const YAML::Node& node) {
    Vector3 min = decodeVector3(node["min"]);
    Vector3 max = decodeVector3(node["max"]);
    return AABB(min, max);
}

YAML::Node editor::Stream::encodeSpriteFrameData(const SpriteFrameData& frameData) {
    YAML::Node node;
    node["name"] = frameData.name;
    node["rect"] = encodeRect(frameData.rect);
    return node;
}

SpriteFrameData editor::Stream::decodeSpriteFrameData(const YAML::Node& node) {
    SpriteFrameData frameData;
    if (node["name"]) frameData.name = node["name"].as<std::string>();
    if (node["rect"]) frameData.rect = decodeRect(node["rect"]);
    return frameData;
}

YAML::Node editor::Stream::encodeTileRectData(const TileRectData& tileRect) {
    YAML::Node node;
    node["name"] = tileRect.name;
    node["submeshId"] = tileRect.submeshId;
    node["rect"] = encodeRect(tileRect.rect);
    return node;
}

TileRectData editor::Stream::decodeTileRectData(const YAML::Node& node) {
    TileRectData tileRect;
    if (node["name"]) tileRect.name = node["name"].as<std::string>();
    if (node["submeshId"]) tileRect.submeshId = YamlNumbers::as<int>(node["submeshId"]);
    if (node["rect"]) tileRect.rect = decodeRect(node["rect"]);
    return tileRect;
}

YAML::Node editor::Stream::encodeTileData(const TileData& tile) {
    YAML::Node node;
    node["name"] = tile.name;
    node["rectId"] = tile.rectId;
    node["position"] = encodeVector2(tile.position);
    node["width"] = tile.width;
    node["height"] = tile.height;
    return node;
}

TileData editor::Stream::decodeTileData(const YAML::Node& node) {
    TileData tile;
    if (node["name"]) tile.name = node["name"].as<std::string>();
    if (node["rectId"]) tile.rectId = YamlNumbers::as<int>(node["rectId"]);
    if (node["position"]) tile.position = decodeVector2(node["position"]);
    if (node["width"]) tile.width = YamlNumbers::as<float>(node["width"]);
    if (node["height"]) tile.height = YamlNumbers::as<float>(node["height"]);
    return tile;
}

YAML::Node editor::Stream::encodeTerrainEditorSettings(const TerrainEditorSettings& ts) {
    YAML::Node terrainNode;
    terrainNode["brushMode"] = terrainBrushModeToString(static_cast<TerrainBrushMode>(ts.brushMode));
    terrainNode["brushShape"] = terrainBrushShapeToString(static_cast<TerrainBrushShape>(ts.brushShape));
    terrainNode["brushFalloff"] = terrainBrushFalloffToString(static_cast<TerrainBrushFalloff>(ts.brushFalloff));
    encodePositiveFinite(terrainNode, "brushSize", ts.brushSize);
    encodeFinite(terrainNode, "brushStrength", ts.brushStrength);
    encodeFinite(terrainNode, "brushRotation", ts.brushRotation);
    terrainNode["brushMaskPath"] = ts.brushMaskPath;
    encodeFinite(terrainNode, "flattenHeight", ts.flattenHeight);
    terrainNode["terraceSteps"] = ts.terraceSteps;
    encodePositiveFinite(terrainNode, "noiseSize", ts.noiseSize);
    terrainNode["heightMapResolution"] = ts.heightMapResolution;
    terrainNode["blendMapResolution"] = ts.blendMapResolution;
    terrainNode["densityMapResolution"] = ts.densityMapResolution;
    terrainNode["normalizeBlendPaint"] = ts.normalizeBlendPaint;
    terrainNode["heightMapStartAtMiddle"] = ts.heightMapStartAtMiddle;
    terrainNode["flattenPickOnStroke"] = ts.flattenPickOnStroke;
    terrainNode["paintUseMask"] = ts.paintUseMask;
    encodeFinite(terrainNode, "paintMinSlope", ts.paintMinSlope);
    encodeFinite(terrainNode, "paintMaxSlope", ts.paintMaxSlope);
    encodeFinite(terrainNode, "paintMinHeight", ts.paintMinHeight);
    encodeFinite(terrainNode, "paintMaxHeight", ts.paintMaxHeight);
    terrainNode["placeAssetPath"] = ts.placeAssetPath;
    terrainNode["placeInstanced"] = ts.placeInstanced;
    encodePositiveFinite(terrainNode, "placeSpacing", ts.placeSpacing);
    encodePositiveFinite(terrainNode, "placeMinScale", ts.placeMinScale);
    encodePositiveFinite(terrainNode, "placeMaxScale", ts.placeMaxScale);
    encodeFinite(terrainNode, "placeRotationJitter", ts.placeRotationJitter);
    encodeFinite(terrainNode, "placeAlignToNormal", ts.placeAlignToNormal);
    return terrainNode;
}

editor::TerrainEditorSettings editor::Stream::decodeTerrainEditorSettings(const YAML::Node& tn) {
    TerrainEditorSettings ts;
    if (!tn.IsMap()) return ts;

    if (tn["brushMode"].IsDefined())          ts.brushMode          = static_cast<int>(decodeEnum(tn, "brushMode", StreamSchema::terrainBrushMode()));
    if (tn["brushShape"].IsDefined())         ts.brushShape         = static_cast<int>(decodeEnum(tn, "brushShape", StreamSchema::terrainBrushShape()));
    if (tn["brushFalloff"].IsDefined())       ts.brushFalloff       = static_cast<int>(decodeEnum(tn, "brushFalloff", StreamSchema::terrainBrushFalloff()));
    if (tn["brushSize"].IsDefined())          ts.brushSize          = decodePositiveFinite(tn["brushSize"], ts.brushSize);
    if (tn["brushStrength"].IsDefined())      ts.brushStrength      = decodeFinite(tn["brushStrength"], ts.brushStrength);
    if (tn["brushRotation"].IsDefined())      ts.brushRotation      = decodeFinite(tn["brushRotation"], ts.brushRotation);
    if (tn["brushMaskPath"].IsDefined())      ts.brushMaskPath      = tn["brushMaskPath"].as<std::string>();
    if (tn["flattenHeight"].IsDefined())      ts.flattenHeight      = decodeFinite(tn["flattenHeight"], ts.flattenHeight);
    if (tn["terraceSteps"].IsDefined())       ts.terraceSteps       = YamlNumbers::as<int>(tn["terraceSteps"]);
    if (tn["noiseSize"].IsDefined())          ts.noiseSize          = decodePositiveFinite(tn["noiseSize"], ts.noiseSize);
    if (tn["heightMapResolution"].IsDefined()) ts.heightMapResolution = YamlNumbers::as<int>(tn["heightMapResolution"]);
    if (tn["blendMapResolution"].IsDefined()) ts.blendMapResolution = YamlNumbers::as<int>(tn["blendMapResolution"]);
    if (tn["densityMapResolution"].IsDefined()) ts.densityMapResolution = YamlNumbers::as<int>(tn["densityMapResolution"]);
    if (tn["normalizeBlendPaint"].IsDefined()) ts.normalizeBlendPaint = YamlNumbers::as<bool>(tn["normalizeBlendPaint"]);
    if (tn["heightMapStartAtMiddle"].IsDefined()) ts.heightMapStartAtMiddle = YamlNumbers::as<bool>(tn["heightMapStartAtMiddle"]);
    if (tn["flattenPickOnStroke"].IsDefined()) ts.flattenPickOnStroke = YamlNumbers::as<bool>(tn["flattenPickOnStroke"]);
    else ts.brushStrength = TerrainEditorSettings{}.brushStrength; // file predates time-based flow; old per-event strengths are far too weak under the new semantics
    if (tn["paintUseMask"].IsDefined())       ts.paintUseMask       = YamlNumbers::as<bool>(tn["paintUseMask"]);
    if (tn["paintMinSlope"].IsDefined())      ts.paintMinSlope      = decodeFinite(tn["paintMinSlope"], ts.paintMinSlope);
    if (tn["paintMaxSlope"].IsDefined())      ts.paintMaxSlope      = decodeFinite(tn["paintMaxSlope"], ts.paintMaxSlope);
    if (tn["paintMinHeight"].IsDefined())     ts.paintMinHeight     = decodeFinite(tn["paintMinHeight"], ts.paintMinHeight);
    if (tn["paintMaxHeight"].IsDefined())     ts.paintMaxHeight     = decodeFinite(tn["paintMaxHeight"], ts.paintMaxHeight);
    if (tn["placeAssetPath"].IsDefined())     ts.placeAssetPath     = tn["placeAssetPath"].as<std::string>();
    if (tn["placeInstanced"].IsDefined())     ts.placeInstanced     = YamlNumbers::as<bool>(tn["placeInstanced"]);
    if (tn["placeSpacing"].IsDefined())       ts.placeSpacing       = decodePositiveFinite(tn["placeSpacing"], ts.placeSpacing);
    if (tn["placeMinScale"].IsDefined())      ts.placeMinScale      = decodePositiveFinite(tn["placeMinScale"], ts.placeMinScale);
    if (tn["placeMaxScale"].IsDefined())      ts.placeMaxScale      = decodePositiveFinite(tn["placeMaxScale"], ts.placeMaxScale);
    if (tn["placeRotationJitter"].IsDefined()) ts.placeRotationJitter = decodeFinite(tn["placeRotationJitter"], ts.placeRotationJitter);
    if (tn["placeAlignToNormal"].IsDefined()) ts.placeAlignToNormal = decodeFinite(tn["placeAlignToNormal"], ts.placeAlignToNormal);
    return ts;
}

namespace {

const char* decompositionQualityToString(DecompositionQuality quality) {
    switch (quality) {
        case DecompositionQuality::LOW: return "low";
        case DecompositionQuality::HIGH: return "high";
        default: return "medium";
    }
}

bool stringToDecompositionQuality(const std::string& name, DecompositionQuality& out) {
    if (name == "low") { out = DecompositionQuality::LOW; return true; }
    if (name == "medium") { out = DecompositionQuality::MEDIUM; return true; }
    if (name == "high") { out = DecompositionQuality::HIGH; return true; }
    return false;
}

const char* rotatePivotToString(editor::PlacementRotatePivot pivot) {
    return pivot == editor::PlacementRotatePivot::EachObject ? "each_object" : "group_center";
}

bool stringToRotatePivot(const std::string& name, editor::PlacementRotatePivot& out) {
    if (name == "group_center") { out = editor::PlacementRotatePivot::GroupCenter; return true; }
    if (name == "each_object") { out = editor::PlacementRotatePivot::EachObject; return true; }
    return false;
}

// One key of a settings map: a value that does not read keeps the default, and the rest of
// the map still loads
template <typename Read>
void decodeSettingKey(const YAML::Node& map, const char* key, Read read) {
    const YAML::Node node = map[key];
    if (!node.IsDefined() || node.IsNull()) return;
    try {
        read(node);
    } catch (const YAML::Exception&) {
    }
}

} // namespace

YAML::Node editor::Stream::encodePhysicsPlacementSettings(const PhysicsPlacementSettings& ps) {
    YAML::Node node;
    node["feel"] = placementFeelName(ps.feel);
    node["settleOnRelease"] = ps.settleOnRelease;
    node["keepFormation"] = ps.keepFormation;
    node["pushNeighbours"] = ps.pushNeighbours;
    encodeFiniteVector3(node, "gravity", ps.gravity);
    node["gravityTowardSurface"] = ps.gravityTowardSurface;
    node["colliders"] = placementColliderPolicyName(ps.colliders);
    node["decomposition"] = placementDecompositionName(ps.decomposition);
    node["quality"] = decompositionQualityToString(ps.quality);
    node["alignToSurface"] = ps.alignToSurface;
    encodeFinite(node, "snapOffset", ps.snapOffset);
    node["rotatePivot"] = rotatePivotToString(ps.rotatePivot);
    encodeFinite(node, "regionMargin", ps.regionMargin);
    node["ignoreLayerMask"] = ps.ignoreLayerMask;
    encodeFinite(node, "maxMoverSize", ps.maxMoverSize);
    encodeFinite(node, "maxSpeed", ps.maxSpeed);
    encodeFinite(node, "contactSpeed", ps.contactSpeed);
    encodeFinite(node, "maxAngularSpeed", ps.maxAngularSpeed);
    node["substeps"] = ps.substeps;
    node["velocityIterations"] = ps.velocityIterations;
    node["positionIterations"] = ps.positionIterations;
    encodeFinite(node, "teleportDistance", ps.teleportDistance);
    encodeFinite(node, "sleepLinear", ps.sleepLinear);
    encodeFinite(node, "sleepAngularDeg", ps.sleepAngularDeg);
    encodeFinite(node, "sleepSeconds", ps.sleepSeconds);
    encodeFinite(node, "maxSettleSeconds", ps.maxSettleSeconds);
    encodeFinite(node, "killYDepth", ps.killYDepth);
    node["showDistanceIndicators"] = ps.showDistanceIndicators;
    node["showProxies"] = ps.showProxies;
    return node;
}

editor::PhysicsPlacementSettings editor::Stream::decodePhysicsPlacementSettings(const YAML::Node& node) {
    PhysicsPlacementSettings ps;
    if (!node.IsMap()) return ps;

    auto readBool = [&node](const char* key, bool& value) {
        decodeSettingKey(node, key, [&value](const YAML::Node& n) { value = YamlNumbers::as<bool>(n); });
    };
    auto readFloat = [&node](const char* key, float& value) {
        decodeSettingKey(node, key, [&value](const YAML::Node& n) { value = decodeFinite(n, value); });
    };
    auto readInt = [&node](const char* key, int& value) {
        decodeSettingKey(node, key, [&value](const YAML::Node& n) { value = YamlNumbers::as<int>(n); });
    };
    auto readName = [&node](const char* key, auto parse) {
        decodeSettingKey(node, key, [&parse](const YAML::Node& n) { parse(n.as<std::string>()); });
    };

    readName("feel", [&ps](const std::string& name) { parsePlacementFeel(name.c_str(), ps.feel); });
    readBool("settleOnRelease", ps.settleOnRelease);
    readBool("keepFormation", ps.keepFormation);
    readBool("pushNeighbours", ps.pushNeighbours);
    decodeSettingKey(node, "gravity", [&ps](const YAML::Node& n) { ps.gravity = decodeFiniteVector3(n, ps.gravity); });
    readBool("gravityTowardSurface", ps.gravityTowardSurface);
    readName("colliders", [&ps](const std::string& name) { parsePlacementColliderPolicy(name.c_str(), ps.colliders); });
    readName("decomposition", [&ps](const std::string& name) { parsePlacementDecomposition(name.c_str(), ps.decomposition); });
    readName("quality", [&ps](const std::string& name) { stringToDecompositionQuality(name, ps.quality); });
    readBool("alignToSurface", ps.alignToSurface);
    readFloat("snapOffset", ps.snapOffset);
    readName("rotatePivot", [&ps](const std::string& name) { stringToRotatePivot(name, ps.rotatePivot); });
    readFloat("regionMargin", ps.regionMargin);
    decodeSettingKey(node, "ignoreLayerMask", [&ps](const YAML::Node& n) { ps.ignoreLayerMask = decodeLayerMask(n, ps.ignoreLayerMask); });
    readFloat("maxMoverSize", ps.maxMoverSize);
    readFloat("maxSpeed", ps.maxSpeed);
    readFloat("contactSpeed", ps.contactSpeed);
    readFloat("maxAngularSpeed", ps.maxAngularSpeed);
    readInt("substeps", ps.substeps);
    readInt("velocityIterations", ps.velocityIterations);
    readInt("positionIterations", ps.positionIterations);
    readFloat("teleportDistance", ps.teleportDistance);
    readFloat("sleepLinear", ps.sleepLinear);
    readFloat("sleepAngularDeg", ps.sleepAngularDeg);
    readFloat("sleepSeconds", ps.sleepSeconds);
    readFloat("maxSettleSeconds", ps.maxSettleSeconds);
    readFloat("killYDepth", ps.killYDepth);
    readBool("showDistanceIndicators", ps.showDistanceIndicators);
    readBool("showProxies", ps.showProxies);
    // a hand-edited value outside what the panel allows is brought into range
    clampPhysicsPlacementSettings(ps);
    return ps;
}

namespace {

// The window's settings are stored as the recipe writes them (settingsToJson), JSON <-> YAML
YAML::Node jsonToYaml(const nlohmann::json& j) {
    YAML::Node node;
    if (j.is_object()) {
        node = YAML::Node(YAML::NodeType::Map);
        for (auto it = j.begin(); it != j.end(); ++it) node[it.key()] = jsonToYaml(it.value());
    } else if (j.is_array()) {
        node = YAML::Node(YAML::NodeType::Sequence);
        for (const nlohmann::json& v : j) node.push_back(jsonToYaml(v));
    } else if (j.is_boolean()) {
        node = j.get<bool>();
    } else if (j.is_number_integer()) {
        node = j.get<int64_t>();
    } else if (j.is_number()) {
        node = j.get<double>();
    } else if (j.is_string()) {
        node = j.get<std::string>();
    }
    return node;
}

nlohmann::json yamlToJson(const YAML::Node& node) {
    if (node.IsMap()) {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& entry : node) j[entry.first.as<std::string>()] = yamlToJson(entry.second);
        return j;
    }
    if (node.IsSequence()) {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& entry : node) j.push_back(yamlToJson(entry));
        return j;
    }
    if (!node.IsScalar()) return nlohmann::json();
    const std::string text = node.Scalar();
    if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str") return text;   // quoted
    if (text == "true") return true;
    if (text == "false") return false;
    char* end = nullptr;
    const long long integer = std::strtoll(text.c_str(), &end, 10);
    if (!text.empty() && end && *end == '\0') return integer;
    const double number = std::strtod(text.c_str(), &end);
    if (!text.empty() && end && *end == '\0') return number;
    return text;
}

// JSON -> YAML with numbers written as their shortest float text ("0.1", not 0.10000000000000001)
YAML::Node shaderGlobalsJsonToYaml(const nlohmann::json& j) {
    if (j.is_number_float()) {
        char text[32];
        snprintf(text, sizeof(text), "%.17g", j.get<double>());
        // the shortest that reads back the same (the values are float-exact doubles)
        for (int digits = 6; digits <= 17; digits++) {
            snprintf(text, sizeof(text), "%.*g", digits, j.get<double>());
            if (std::strtod(text, nullptr) == j.get<double>()) break;
        }
        return YAML::Node(std::string(text));
    }
    if (j.is_object()) {
        YAML::Node node(YAML::NodeType::Map);
        for (auto it = j.begin(); it != j.end(); ++it) node[it.key()] = shaderGlobalsJsonToYaml(it.value());
        return node;
    }
    if (j.is_array()) {
        YAML::Node node(YAML::NodeType::Sequence);
        for (const nlohmann::json& v : j) node.push_back(shaderGlobalsJsonToYaml(v));
        return node;
    }
    return jsonToYaml(j);
}

const char* spriteNormalsViewName(int view) {
    switch (view) {
        case 0: return "color";
        case 1: return "normal";
        case 3: return "split";
        default: return "lit";
    }
}

} // namespace

YAML::Node editor::Stream::encodeSpriteNormalsSettings(const SpriteNormalsWindowSettings& s) {
    YAML::Node node;
    node["settings"] = jsonToYaml(spritenormals::settingsToJson(s.settings));
    node["view"] = spriteNormalsViewName(s.view);
    encodeFinite(node, "lightHeight", s.lightHeight);
    encodeFiniteVector3(node, "lightColour", s.lightColour);
    encodeFinite(node, "ambient", s.ambient);
    node["orbit"] = s.orbit;
    node["grid"] = s.grid;
    node["advancedOpen"] = s.advancedOpen;
    node["autoRegenerate"] = s.autoRegenerate;
    return node;
}

editor::SpriteNormalsWindowSettings editor::Stream::decodeSpriteNormalsSettings(const YAML::Node& node) {
    SpriteNormalsWindowSettings s;
    if (!node.IsMap()) return s;
    decodeSettingKey(node, "view", [&s](const YAML::Node& n) {
        const std::string view = n.as<std::string>();
        s.view = view == "color" ? 0 : view == "normal" ? 1 : view == "split" ? 3 : 2;
    });
    decodeSettingKey(node, "lightHeight", [&s](const YAML::Node& n) { s.lightHeight = std::clamp(decodeFinite(n, s.lightHeight), 0.0f, 4096.0f); });
    decodeSettingKey(node, "lightColour", [&s](const YAML::Node& n) { s.lightColour = decodeFiniteVector3(n, s.lightColour); });
    decodeSettingKey(node, "ambient", [&s](const YAML::Node& n) { s.ambient = std::clamp(decodeFinite(n, s.ambient), 0.0f, 1.0f); });
    decodeSettingKey(node, "orbit", [&s](const YAML::Node& n) { s.orbit = YamlNumbers::as<bool>(n); });
    decodeSettingKey(node, "grid", [&s](const YAML::Node& n) { s.grid = YamlNumbers::as<bool>(n); });
    decodeSettingKey(node, "advancedOpen", [&s](const YAML::Node& n) { s.advancedOpen = YamlNumbers::as<bool>(n); });
    decodeSettingKey(node, "autoRegenerate", [&s](const YAML::Node& n) { s.autoRegenerate = YamlNumbers::as<bool>(n); });
    if (node["settings"] && node["settings"].IsMap()) {
        // the preset first, then every value over it (the recipe's resolution order)
        const nlohmann::json j = yamlToJson(node["settings"]);
        spritenormals::Preset preset = spritenormals::Preset::Default;
        if (j.contains("preset") && j["preset"].is_string()) spritenormals::parsePreset(j["preset"].get<std::string>(), preset);
        spritenormals::Settings settings = spritenormals::presetSettings(preset);
        std::string error;
        if (!spritenormals::settingsFromJson(j, settings, error)) throw std::runtime_error(error);
        settings.preset = preset;
        const std::vector<spritenormals::Problem> problems = spritenormals::validateSettings(settings);
        if (!problems.empty()) throw std::runtime_error(problems.front().message);
        s.settings = settings;
    }
    return s;
}


namespace {

// The per-scene view keys an older project.yaml carried
const char* const sceneDisplayKeys[] = {
    "showAllJoints", "showAllBones", "showAllBodies", "hideCameraView",
    "hideLightIcons", "hideSoundIcons", "hideContainerGuides", "showOrigin",
    "showGrid3D", "hideSelectionOutline", "disableFaceCulling", "showGrid2D",
    "gridSpacing2D", "gridSpacing3D", "snapToGrid", "snapTile", "snapRotation",
    "rotationSnapDegrees", "animateWater",
};

} // namespace

bool editor::Stream::hasSceneDisplaySettings(const YAML::Node& node) {
    if (!node.IsMap()) return false;
    for (const char* key : sceneDisplayKeys) {
        if (node[key]) return true;
    }
    return false;
}

YAML::Node editor::Stream::encodeSceneDisplaySettings(const SceneDisplaySettings& ds) {
    YAML::Node sceneNode;
    sceneNode["showAllJoints"]        = ds.showAllJoints;
    sceneNode["showAllBones"]         = ds.showAllBones;
    sceneNode["showAllBodies"]        = ds.showAllBodies;
    sceneNode["hideCameraView"]       = ds.hideCameraView;
    sceneNode["hideLightIcons"]       = ds.hideLightIcons;
    sceneNode["hideSoundIcons"]       = ds.hideSoundIcons;
    sceneNode["hideContainerGuides"]  = ds.hideContainerGuides;
    sceneNode["showOrigin"]           = ds.showOrigin;
    sceneNode["showGrid3D"]           = ds.showGrid3D;
    sceneNode["hideSelectionOutline"] = ds.hideSelectionOutline;
    sceneNode["disableFaceCulling"]   = ds.disableFaceCulling;
    sceneNode["disableFog"]           = ds.disableFog;
    sceneNode["disableClouds"]        = ds.disableClouds;
    // written only when off (the default is on)
    if (!ds.animateWater)
        sceneNode["animateWater"] = ds.animateWater;
    if (ds.showPostProcessing)
        sceneNode["showPostProcessing"] = ds.showPostProcessing;
    if (ds.showAllLattices)
        sceneNode["showAllLattices"] = ds.showAllLattices;
    if (ds.drawMode != ViewDrawMode::SHADED)
        sceneNode["drawMode"] = drawModeKey(ds.drawMode);
    if (ds.showStats)
        sceneNode["showStats"] = ds.showStats;
    // S41: written only when turned off (on by default)
    if (!ds.pixelZoom) sceneNode["pixelZoom"] = false;
    if (!ds.pixelGrid) sceneNode["pixelGrid"] = false;
    if (!ds.pixelSnapPreview) sceneNode["pixelSnapPreview"] = false;
    if (!ds.pixelMoveSnap) sceneNode["pixelMoveSnap"] = false;
    sceneNode["showGrid2D"]           = ds.showGrid2D;
    encodePositiveFinite(sceneNode, "gridSpacing2D", ds.gridSpacing2D);
    encodePositiveFinite(sceneNode, "gridSpacing3D", ds.gridSpacing3D);
    sceneNode["snapToGrid"]           = ds.snapToGrid;
    sceneNode["snapTile"]             = ds.snapTile;
    sceneNode["snapRotation"]         = ds.snapRotation;
    encodePositiveFinite(sceneNode, "rotationSnapDegrees", ds.rotationSnapDegrees);
    const YAML::Node navigationNode = encodeNavOverlaySettings(ds.navigation);
    if (navigationNode.size() > 0)
        sceneNode["navigation"] = navigationNode;
    return sceneNode;
}

void editor::Stream::decodeSceneDisplaySettings(const YAML::Node& node, SceneDisplaySettings& ds) {
    if (!node.IsMap()) return;

    if (node["showAllJoints"])        ds.showAllJoints        = YamlNumbers::as<bool>(node["showAllJoints"]);
    if (node["showAllBones"])         ds.showAllBones         = YamlNumbers::as<bool>(node["showAllBones"]);
    if (node["showAllBodies"])        ds.showAllBodies        = YamlNumbers::as<bool>(node["showAllBodies"]);
    if (node["hideCameraView"])       ds.hideCameraView       = YamlNumbers::as<bool>(node["hideCameraView"]);
    if (node["hideLightIcons"])       ds.hideLightIcons       = YamlNumbers::as<bool>(node["hideLightIcons"]);
    if (node["hideSoundIcons"])       ds.hideSoundIcons       = YamlNumbers::as<bool>(node["hideSoundIcons"]);
    if (node["hideContainerGuides"])  ds.hideContainerGuides  = YamlNumbers::as<bool>(node["hideContainerGuides"]);
    if (node["showOrigin"])           ds.showOrigin           = YamlNumbers::as<bool>(node["showOrigin"]);
    if (node["showGrid3D"])           ds.showGrid3D           = YamlNumbers::as<bool>(node["showGrid3D"]);
    if (node["hideSelectionOutline"]) ds.hideSelectionOutline = YamlNumbers::as<bool>(node["hideSelectionOutline"]);
    if (node["disableFaceCulling"])   ds.disableFaceCulling   = YamlNumbers::as<bool>(node["disableFaceCulling"]);
    if (node["disableFog"])           ds.disableFog           = YamlNumbers::as<bool>(node["disableFog"]);
    if (node["disableClouds"])        ds.disableClouds        = YamlNumbers::as<bool>(node["disableClouds"]);
    if (node["animateWater"])         ds.animateWater         = YamlNumbers::as<bool>(node["animateWater"]);
    ds.showPostProcessing = node["showPostProcessing"] ? YamlNumbers::as<bool>(node["showPostProcessing"]) : false;
    ds.pixelZoom = node["pixelZoom"] ? YamlNumbers::as<bool>(node["pixelZoom"]) : true;
    ds.pixelGrid = node["pixelGrid"] ? YamlNumbers::as<bool>(node["pixelGrid"]) : true;
    ds.pixelSnapPreview = node["pixelSnapPreview"] ? YamlNumbers::as<bool>(node["pixelSnapPreview"]) : true;
    ds.pixelMoveSnap = node["pixelMoveSnap"] ? YamlNumbers::as<bool>(node["pixelMoveSnap"]) : true;
    ds.showAllLattices = node["showAllLattices"] ? YamlNumbers::as<bool>(node["showAllLattices"]) : false;
    ds.drawMode = ViewDrawMode::SHADED;
    if (node["drawMode"] && node["drawMode"].IsScalar())
        drawModeFromKey(node["drawMode"].as<std::string>(), ds.drawMode);
    ds.showStats = node["showStats"] ? YamlNumbers::as<bool>(node["showStats"]) : false;
    decodeNavOverlaySettings(node["navigation"], ds.navigation);
    if (node["showGrid2D"])           ds.showGrid2D           = YamlNumbers::as<bool>(node["showGrid2D"]);
    if (node["gridSpacing2D"])        ds.gridSpacing2D        = decodePositiveFinite(node["gridSpacing2D"], ds.gridSpacing2D);
    if (node["gridSpacing3D"])        ds.gridSpacing3D        = decodePositiveFinite(node["gridSpacing3D"], ds.gridSpacing3D);
    if (node["snapToGrid"])           ds.snapToGrid           = YamlNumbers::as<bool>(node["snapToGrid"]);
    if (node["snapTile"])             ds.snapTile             = YamlNumbers::as<bool>(node["snapTile"]);
    if (node["snapRotation"])         ds.snapRotation         = YamlNumbers::as<bool>(node["snapRotation"]);
    if (node["rotationSnapDegrees"])  ds.rotationSnapDegrees  = decodePositiveFinite(node["rotationSnapDegrees"], ds.rotationSnapDegrees);
}

namespace {

// ---- the native plugin settings of project.yaml (android/ios/macos/web nodes)

using jasperengine::editor::NativePlistValue;

void encodeStringList(YAML::Node& parent, const char* key, const std::vector<std::string>& values) {
    if (values.empty()) return;
    YAML::Node list(YAML::NodeType::Sequence);
    for (const std::string& value : values) list.push_back(value);
    parent[key] = list;
}

std::vector<std::string> decodeStringList(const YAML::Node& node) {
    std::vector<std::string> values;
    if (!node || !node.IsSequence()) return values;
    for (const YAML::Node& item : node) {
        if (item.IsScalar()) values.push_back(item.as<std::string>());
    }
    return values;
}

// apple.privacy: the project's additions to the Apple privacy manifest, short names; nothing
// when there are none
void encodeApplePrivacy(YAML::Node& root, const jasperengine::editor::ApplePrivacy::Manifest& privacy) {
    if (privacy.empty()) return;
    YAML::Node node;
    if (privacy.tracking) node["tracking"] = true;
    encodeStringList(node, "trackingDomains", privacy.trackingDomains);
    if (!privacy.collectedData.empty()) {
        YAML::Node list(YAML::NodeType::Sequence);
        for (const auto& data : privacy.collectedData) {
            YAML::Node item;
            item["type"] = data.type;
            item["linked"] = data.linked;
            item["tracking"] = data.tracking;
            encodeStringList(item, "purposes", data.purposes);
            list.push_back(item);
        }
        node["collectedData"] = list;
    }
    if (!privacy.accessedApis.empty()) {
        YAML::Node list(YAML::NodeType::Sequence);
        for (const auto& api : privacy.accessedApis) {
            YAML::Node item;
            item["type"] = api.type;
            encodeStringList(item, "reasons", api.reasons);
            list.push_back(item);
        }
        node["accessedApis"] = list;
    }
    root["apple"]["privacy"] = node;
}

// Long names (NSPrivacy...) are read too; what is no YAML of that shape is left out
jasperengine::editor::ApplePrivacy::Manifest decodeApplePrivacy(const YAML::Node& node) {
    namespace ApplePrivacy = jasperengine::editor::ApplePrivacy;
    ApplePrivacy::Manifest privacy;
    if (!node || !node.IsMap()) return privacy;
    auto flag = [](const YAML::Node& value) {
        return value && value.IsScalar() && (value.as<std::string>() == "true" || value.as<std::string>() == "yes");
    };
    privacy.tracking = flag(node["tracking"]);
    privacy.trackingDomains = decodeStringList(node["trackingDomains"]);
    if (node["collectedData"] && node["collectedData"].IsSequence()) {
        for (const YAML::Node& item : node["collectedData"]) {
            if (!item.IsMap() || !item["type"] || !item["type"].IsScalar()) continue;
            ApplePrivacy::CollectedData data;
            data.type = item["type"].as<std::string>();
            data.linked = flag(item["linked"]);
            data.tracking = flag(item["tracking"]);
            data.purposes = decodeStringList(item["purposes"]);
            privacy.collectedData.push_back(data);
        }
    }
    if (node["accessedApis"] && node["accessedApis"].IsSequence()) {
        for (const YAML::Node& item : node["accessedApis"]) {
            if (!item.IsMap() || !item["type"] || !item["type"].IsScalar()) continue;
            ApplePrivacy::AccessedApi api;
            api.type = item["type"].as<std::string>();
            api.reasons = decodeStringList(item["reasons"]);
            privacy.accessedApis.push_back(api);
        }
    }
    return ApplePrivacy::normalized(privacy);
}

// A plain scalar read back as a plist value: true/false, an integer, a real, else a string
NativePlistValue plainPlistValue(const std::string& text) {
    NativePlistValue value;
    if (text == "true" || text == "false") {
        value.kind = NativePlistValue::Kind::Bool;
        value.flag = text == "true";
        return value;
    }
    if (!text.empty()) {
        char* end = nullptr;
        errno = 0;
        const long long integer = std::strtoll(text.c_str(), &end, 10);
        if (errno == 0 && end && *end == '\0' && text.find_first_of(" \t") == std::string::npos) {
            value.kind = NativePlistValue::Kind::Integer;
            value.integer = integer;
            return value;
        }
        errno = 0;
        const double real = std::strtod(text.c_str(), &end);
        if (errno == 0 && end && *end == '\0' && std::isfinite(real) && text.find_first_of(" \tinfINFnNxX") == std::string::npos) {
            value.kind = NativePlistValue::Kind::Real;
            value.real = real;
            return value;
        }
    }
    value.kind = NativePlistValue::Kind::String;
    value.text = text;
    return value;
}

YAML::Node encodePlistValue(const NativePlistValue& value) {
    switch (value.kind) {
        case NativePlistValue::Kind::Bool: return YAML::Node(value.flag);
        case NativePlistValue::Kind::Integer: return YAML::Node(value.integer);
        case NativePlistValue::Kind::Real: {
            // a real keeps a decimal point, so it reads back as a real
            std::ostringstream out;
            out.imbue(std::locale::classic());
            out << std::setprecision(17) << value.real;
            std::string text = out.str();
            if (text.find_first_of(".eE") == std::string::npos) text += ".0";
            return YAML::Node(text);
        }
        case NativePlistValue::Kind::StringArray: {
            YAML::Node list(YAML::NodeType::Sequence);
            for (const std::string& item : value.items) list.push_back(item);
            return list;
        }
        case NativePlistValue::Kind::String:
            break;
    }
    // a string that would read back as something else ("true", "12", "") is written {string: ...}
    const NativePlistValue plain = plainPlistValue(value.text);
    if (plain.kind != NativePlistValue::Kind::String || value.text.empty() || value.text == "null" || value.text == "~") {
        YAML::Node wrapped;
        wrapped["string"] = value.text;
        return wrapped;
    }
    return YAML::Node(value.text);
}

bool decodePlistValue(const YAML::Node& node, NativePlistValue& out) {
    if (node.IsSequence()) {
        out = NativePlistValue();
        out.kind = NativePlistValue::Kind::StringArray;
        out.items = decodeStringList(node);
        return true;
    }
    if (node.IsMap() && node["string"] && node["string"].IsScalar()) {
        out = NativePlistValue();
        out.kind = NativePlistValue::Kind::String;
        out.text = node["string"].as<std::string>();
        return true;
    }
    if (!node.IsScalar()) return false;
    // a quoted scalar is a string whatever it says
    if (node.Tag() == "!") {
        out = NativePlistValue();
        out.text = node.as<std::string>();
        return true;
    }
    out = plainPlistValue(node.as<std::string>());
    return true;
}

void encodePlistEntries(YAML::Node& parent, const std::map<std::string, NativePlistValue>& entries) {
    if (entries.empty()) return;
    YAML::Node map(YAML::NodeType::Map);
    for (const auto& [key, value] : entries) map[key] = encodePlistValue(value);
    parent["plistEntries"] = map;
}

std::map<std::string, NativePlistValue> decodePlistEntries(const YAML::Node& node) {
    std::map<std::string, NativePlistValue> entries;
    if (!node || !node.IsMap()) return entries;
    for (const auto& item : node) {
        NativePlistValue value;
        if (item.first.IsScalar() && decodePlistValue(item.second, value)) entries[item.first.as<std::string>()] = value;
    }
    return entries;
}

} // namespace

namespace {

// ---- an older editor's Project Settings > Input in project.yaml: `input: {version: 1, actions:
// [...]}`, Unity's Input Manager as a flat list (the JSON InputManagerFormat reads, each binding a
// flow map). It becomes the "Input Manager" map of an .inputactions asset

// YAML -> the JSON InputManagerFormat reads: names, kinds, axes and control names stay text ("0"
// is the key 0, an action may be named "1"), the rest reads as numbers and booleans
nlohmann::json inputYamlToJson(const YAML::Node& node, const std::string& key = "") {
    static const std::set<std::string> textKeys = {"name", "description", "type", "kind", "axis", "positive", "negative"};
    if (node.IsMap()) {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& entry : node) {
            const std::string child = entry.first.as<std::string>();
            j[child] = inputYamlToJson(entry.second, child);
        }
        return j;
    }
    if (node.IsSequence()) {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& entry : node) j.push_back(inputYamlToJson(entry, key));
        return j;
    }
    if (!node.IsScalar()) return nlohmann::json();
    const std::string text = node.Scalar();
    if (textKeys.count(key) || node.Tag() == "!") return text;
    if (text == "true") return true;
    if (text == "false") return false;
    char* end = nullptr;
    const long long integer = std::strtoll(text.c_str(), &end, 10);
    if (!text.empty() && end && *end == '\0') return integer;
    const double number = std::strtod(text.c_str(), &end);
    if (!text.empty() && end && *end == '\0') return number;
    return text;
}

// the input node is an older editor's: a version, or an actions list (S22 writes a path)
bool isInputManagerNode(const YAML::Node& input) {
    if (input.IsSequence()) return true;
    if (!input.IsMap()) return false;
    return input["version"].IsDefined() || (input["actions"].IsDefined() && !input["actions"].IsScalar());
}

std::string inputSourceHash(const std::string& json) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : json) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char text[17];
    snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// the asset the actions become: InputManager.inputactions in the assets directory (another name
// when a file there is not this conversion), or the one an earlier load of the same node wrote
editor::InputActionsMigration convertInputManagerNode(editor::Project* project, const std::vector<InputManagerAction>& actions) {
    const std::string hash = inputSourceHash(InputManagerFormat::actionsToJson(actions));
    InputActionAssetDesc asset;
    asset.maps.push_back(InputManagerFormat::toMap(actions));
    nlohmann::ordered_json marker = nlohmann::ordered_json::object();
    marker["migratedFrom"] = {{"projectYaml", "input"}, {"hash", hash}};
    asset.passthrough = marker.dump();

    editor::InputActionsMigration migration;
    migration.actions = actions.size();
    migration.text = asset.save();
    migration.pending = true;
    const fs::path assets = project->getAssetsPath();
    for (int n = 1;; n++) {
        const std::string name = n == 1 ? "InputManager.inputactions" : "InputManager " + std::to_string(n) + ".inputactions";
        const fs::path file = assets / name;
        fs::path relative = file.lexically_relative(project->getProjectPath());
        if (relative.empty()) relative = file;
        migration.asset = relative;
        std::error_code ec;
        if (!fs::exists(file, ec))
            return migration;
        // written by an earlier load of the same node: kept as it is now (edits made since stay)
        std::ifstream in(file, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json existing = nlohmann::json::parse(text, nullptr, false);
        if (!existing.is_discarded() && existing.is_object() && existing.contains("migratedFrom") &&
                existing["migratedFrom"].is_object() && existing["migratedFrom"].value("hash", std::string()) == hash) {
            migration.pending = false;
            migration.reused = true;
            migration.text = text;
            return migration;
        }
    }
}

}

YAML::Node editor::Stream::encodeProject(Project* project) {
    YAML::Node root;

    root[StreamMigrations::versionKey(DocumentKind::Project)] = StreamMigrations::currentVersion(DocumentKind::Project);
    root["name"] = project->getName();

    root["canvasWidth"] = project->getCanvasWidth();
    root["canvasHeight"] = project->getCanvasHeight();

    root["scalingMode"] = scalingModeToString(project->getScalingMode());
    // absent = Follow Game Canvas (projects from before the Canvas Scaler stay unchanged)
    if (project->getUIScale() != CanvasScalerSettings())
        root["uiScale"] = encodeCanvasScaler(project->getUIScale());
    root["textureStrategy"] = textureStrategyToString(project->getTextureStrategy());
    root["vsync"] = project->isVSyncEnabled();
    // absent = on (projects from before Dynamic Batching)
    if (!project->isDynamicBatching())
        root["dynamicBatching"] = false;
    if (project->getAtmosphereQuality() != Project::defaultAtmosphereQuality) {
        YAML::Node quality;
        quality["desktop"] = StreamSchema::atmosphereQuality().toString(project->getAtmosphereQuality()[0]);
        quality["mobile"] = StreamSchema::atmosphereQuality().toString(project->getAtmosphereQuality()[1]);
        quality["web"] = StreamSchema::atmosphereQuality().toString(project->getAtmosphereQuality()[2]);
        root["atmosphereQuality"] = quality;
    }
    // Project Settings > Quality > Foliage Distance, omitted at its defaults
    if (project->getFoliageDistanceScale() != Project::defaultFoliageDistanceScale) {
        YAML::Node scale;
        scale["desktop"] = project->getFoliageDistanceScale()[0];
        scale["mobile"] = project->getFoliageDistanceScale()[1];
        scale["web"] = project->getFoliageDistanceScale()[2];
        root["foliageDistanceScale"] = scale;
    }
    // Project Settings > Input, omitted at its defaults
    if (!project->getInputActionsAsset().empty() || project->getInputDeadzone() != Project::defaultInputDeadzone) {
        YAML::Node input;
        if (!project->getInputActionsAsset().empty())
            input["actions"] = project->getInputActionsAsset().generic_string();
        if (project->getInputDeadzone() != Project::defaultInputDeadzone)
            input["deadzone"] = project->getInputDeadzone();
        root["input"] = input;
    } else if (!project->getUnreadableInputNode().empty()) {
        // an older editor's Input Manager node that could not be read: back as it was, until
        // an actions asset is set
        try {
            root["input"] = YAML::Load(project->getUnreadableInputNode());
        } catch (const YAML::Exception&) {
        }
    }
    if (!(project->getPlayerLogging() == PlayerLoggingSettings())) {
        const PlayerLoggingSettings& logging = project->getPlayerLogging();
        YAML::Node loggingNode;
        loggingNode["level"] = logging.level;
        loggingNode["fileLog"] = logging.fileLog;
        loggingNode["backtrace"] = logging.backtrace;
        root["playerLogging"] = loggingNode;
    }
    if (!(project->getPlayerCrash() == PlayerCrashSettings())) {
        const PlayerCrashSettings& crash = project->getPlayerCrash();
        YAML::Node crashNode;
        crashNode["enabled"] = crash.enabled;
        crashNode["logTail"] = crash.includeLogTail;
        crashNode["dumpType"] = crash.dumpWithHeap ? "with_heap" : "small";
        crashNode["hangWatchdog"] = crash.hangWatchdog;
        crashNode["hangSeconds"] = crash.hangSeconds;
        crashNode["webOverlay"] = crash.webCrashOverlay;
        root["playerCrash"] = crashNode;
    }
    root["htmlUiMsaaSamples"] = project->getHtmlUiMsaaSamples();
    // device: what differs from the defaults of the Device Tier table and the rest (S56)
    {
        const nlohmann::json device = nlohmann::json::parse(project->getDeviceSettings().toJson());
        if (!device.empty())
            root["device"] = mcp::McpValues::jsonToYaml(device);
    }
    {
        const HtmlUiRenderSettings& htmlUiRender = project->getHtmlUiRenderSettings();
        root["htmlUiGlyphAtlasMiB"] = htmlUiRender.glyphAtlasMiB;
        root["htmlUiSdfPrimitives"] = htmlUiRender.sdfPrimitives;
        root["htmlUiTransformAwareText"] = htmlUiRender.transformAwareText;
    }
    // graphics.shadowTier.<platform>, only the platforms away from their default tier,
    // graphics.lightMode when not Automatic, and GPU Instancing (S55) when off
    {
        YAML::Node graphics;
        YAML::Node tiers;
        for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++){
            const ShadowTier tier = project->getShadowTier(platform);
            if (tier != ShadowTiers::defaultTier(platform))
                tiers[Project::getShadowTierPlatformName(platform)] = StreamSchema::shadowTier().toString(tier);
        }
        if (tiers.size() > 0)
            graphics["shadowTier"] = tiers;
        const DrawSubmissionSettings& drawSubmission = project->getDrawSubmissionSettings();
        if (drawSubmission.gpuInstancing != DrawSubmissionSettings().gpuInstancing)
            graphics["gpuInstancing"] = drawSubmission.gpuInstancing;
        if (project->getLightMode() != Project::defaultLightMode)
            graphics["lightMode"] = LightingTiers::getModeName(project->getLightMode());
        // graphics.pixelsPerUnit (S41), only away from 1
        if (project->getPixelsPerUnit() != Project::defaultPixelsPerUnit)
            graphics["pixelsPerUnit"] = project->getPixelsPerUnit();
        if (graphics.size() > 0)
            root["graphics"] = graphics;
    }

    root["windowMode"] = windowModeToString(project->getWindowMode());
    root["windowWidth"] = project->getWindowWidth();
    root["windowHeight"] = project->getWindowHeight();
    root["windowResizable"] = project->isWindowResizable();
    if (!project->getWindowTitle().empty()) {
        root["windowTitle"] = project->getWindowTitle();
    }
    if (!project->getWindowIcon().empty()) {
        root["windowIcon"] = portablePath(project->getWindowIcon());
    }

    if (!project->getAssetsDir().empty() && project->getAssetsDir() != ".") {
        root["assetsDir"] = portablePath(project->getAssetsDir());
    }
    if (!project->getLuaDir().empty()) {
        root["luaDir"] = portablePath(project->getLuaDir());
    }
    if (!project->getScriptDirs().empty()) {
        YAML::Node scriptDirsNode;
        for (const fs::path& scriptDir : project->getScriptDirs()) {
            scriptDirsNode.push_back(portablePath(scriptDir));
        }
        root["scriptDirs"] = scriptDirsNode;
    }
    if (project->getCxxStandard() != Project::defaultCxxStandard) {
        root["cxxStandard"] = project->getCxxStandard();
    }
    if (project->isPhysics2DEnabled() != Project::defaultPhysics2DEnabled) {
        root["physics2D"] = project->isPhysics2DEnabled();
    }
    if (project->isPhysics3DEnabled() != Project::defaultPhysics3DEnabled) {
        root["physics3D"] = project->isPhysics3DEnabled();
    }
    if (project->isRuntimeShadersEnabled() != Project::defaultRuntimeShadersEnabled) {
        root["runtimeShaders"] = project->isRuntimeShadersEnabled();
    }
    if (project->getMobilePbrTier() != Project::defaultMobilePbrTier) {
        root["mobilePbrTier"] = pbrTierToString(project->getMobilePbrTier());
    }
    if (project->shouldPackNativeResources() != Project::defaultPackNativeResources) {
        root["packNativeResources"] = project->shouldPackNativeResources();
    }
    if (project->hasVersionControlMetadata() != Project::defaultVersionControlMetadata) {
        root["versionControlMetadata"] = project->hasVersionControlMetadata();
    }
    // S29: only what differs from a project saved before it (auto, no LFS, no lockable scenes)
    if (const VcsProjectSettings& vcsSettings = project->getVcsProjectSettings(); vcsSettings != VcsProjectSettings()) {
        YAML::Node vcsNode(YAML::NodeType::Map);
        if (vcsSettings.provider != vcs::ProviderChoice::Auto) vcsNode["provider"] = vcs::providerChoiceName(vcsSettings.provider);
        if (vcsSettings.lfs) vcsNode["lfs"] = true;
        if (vcsSettings.lockScenes) vcsNode["lockScenes"] = true;
        root["versionControl"] = vcsNode;
    }

    if (!project->getTags().empty()) {
        YAML::Node tagsNode;
        for (const std::string& tag : project->getTags()) {
            tagsNode.push_back(tag);
        }
        root["tags"] = tagsNode;
    }
    {
        YAML::Node layersNode;
        const std::vector<std::string>& layerNames = project->getLayerNames();
        for (int layer = 0; layer < (int)layerNames.size(); layer++) {
            if (!layerNames[layer].empty()) {
                // a string key keeps the node a map (an integer key can turn it into a sequence)
                layersNode[std::to_string(layer)] = layerNames[layer];
            }
        }
        if (layersNode.size() != 0) {
            root["layers"] = layersNode;
        }
    }
    {
        // Layer Collision Matrices: the layer pairs that do not collide, e.g. [8, 9]
        auto encodeIgnoredPairs = [](const LayerCollisionRows& rows) {
            YAML::Node pairs;
            for (int layer = 0; layer < MAX_LAYERS; layer++) {
                for (int other = layer; other < MAX_LAYERS; other++) {
                    if (((rows[layer] >> other) & 1u) == 0) {
                        YAML::Node pair;
                        pair.push_back(layer);
                        pair.push_back(other);
                        pair.SetStyle(YAML::EmitterStyle::Flow);
                        pairs.push_back(pair);
                    }
                }
            }
            return pairs;
        };
        YAML::Node ignoredNode;
        YAML::Node pairs3D = encodeIgnoredPairs(project->getLayerCollisionMatrix3D());
        YAML::Node pairs2D = encodeIgnoredPairs(project->getLayerCollisionMatrix2D());
        if (pairs3D.size() != 0) ignoredNode["physics"] = pairs3D;
        if (pairs2D.size() != 0) ignoredNode["physics2D"] = pairs2D;
        if (ignoredNode.size() != 0) {
            root["ignoredLayerCollisions"] = ignoredNode;
        }
    }

    // Project Settings > Graphics > Shader Globals: {name, type, arraySize (arrays), default}
    if (!project->getShaderGlobals().empty()) {
        YAML::Node globals = shaderGlobalsJsonToYaml(ShaderGlobalsJson::listToJson(project->getShaderGlobals(), "arraySize"));
        for (YAML::Node global : globals) {
            if (global["default"] && global["default"].IsSequence()) global["default"].SetStyle(YAML::EmitterStyle::Flow);
        }
        root["shaderGlobals"] = globals;
    }

    // Project Settings > Physics > 3D World Capacity: only when it differs from the defaults (its own key:
    // physics3D is the 3D physics on/off switch)
    if (project->getPhysics3DWorldSettings() != Physics3DWorldSettings()) {
        const Physics3DWorldSettings& world = project->getPhysics3DWorldSettings();
        YAML::Node physicsNode;
        physicsNode["maxBodies"] = world.maxBodies;
        physicsNode["maxBodyPairs"] = world.maxBodyPairs;
        physicsNode["maxContactConstraints"] = world.maxContactConstraints;
        root["physics3DWorld"] = physicsNode;
    }


    {
        YAML::Node exportNode;

        const ShaderOverrides& shaders = project->getShaderOverrides();
        if (!shaders.additions.empty()) exportNode["shaderAdditions"] = Stream::encodeShaderKeys(shaders.additions);
        if (!shaders.exclusions.empty()) exportNode["shaderExclusions"] = Stream::encodeShaderKeys(shaders.exclusions);

        const SourceCodeExportSettings& sourceCode = project->getSourceCodeExportSettings();
        if (sourceCode.graphicBackendsConfigured) {
            YAML::Node sourceCodeNode;
            sourceCodeNode["graphicBackends"] = encodeExportBackendSet(sourceCode.graphicBackends);
            exportNode["sourceCode"] = sourceCodeNode;
        }

        if (project->isDevelopmentBuild()) {
            exportNode["developmentBuild"] = true;
        }
        if (project->getBuildMethod() != "auto") exportNode["method"] = project->getBuildMethod();
        if (project->isPackContent()) exportNode["pack"] = true;
        if (!project->isStripEngineCode()) exportNode["stripEngineCode"] = false;

        const DesktopExportSettings& desktop = project->getDesktopExportSettings();
        if (desktop.graphicBackendConfigured) {
            YAML::Node desktopNode;
            desktopNode["graphicBackend"] = ShaderPool::getShaderBackendCliToken(desktop.graphicBackend);
            exportNode["desktop"] = desktopNode;
        }

        if (exportNode.size() != 0) {
            root["export"] = exportNode;
        }
    }

    {
        // Player > Resolution and Presentation (non-defaults only)
        const PresentationSettings& presentation = project->getPresentationSettings();
        const PresentationSettings defaults;
        YAML::Node presentationNode;
        if (presentation.defaultOrientation != defaults.defaultOrientation) {
            presentationNode["defaultOrientation"] = PlatformRules::orientationName(presentation.defaultOrientation);
        }
        if (presentation.allowedOrientations != defaults.allowedOrientations) {
            presentationNode["allowedOrientations"] = orientationMaskToNode(presentation.allowedOrientations);
        }
        if (presentation.keepScreenOn != defaults.keepScreenOn) presentationNode["keepScreenOn"] = presentation.keepScreenOn;
        if (presentation.targetFrameRate != defaults.targetFrameRate) presentationNode["targetFrameRate"] = presentation.targetFrameRate;
        if (presentationNode.size() != 0) root["presentation"] = presentationNode;
    }

    {
        const ApplicationSettings& application = project->getApplicationSettings();
        const ApplicationSettings defaultApplication;
        YAML::Node applicationNode;
        if (!application.name.empty()) applicationNode["name"] = application.name;
        if (application.identifier != defaultApplication.identifier) applicationNode["identifier"] = application.identifier;
        if (application.version != defaultApplication.version) applicationNode["version"] = application.version;
        if (application.build != defaultApplication.build) applicationNode["build"] = application.build;
        if (application.allowInsecureHttp != defaultApplication.allowInsecureHttp) applicationNode["allowInsecureHttp"] = application.allowInsecureHttp;
        if (applicationNode.size() != 0) root["application"] = applicationNode;
    }

    // The platform nodes below only carry overrides; an omitted key inherits
    // from the application block.
    {
        const WebProjectSettings& web = project->getWebProjectSettings();
        const WebProjectSettings defaultWeb;
        YAML::Node webNode;
        if (!web.applicationName.empty()) webNode["applicationName"] = web.applicationName;
        if (!web.favicon.empty()) webNode["favicon"] = web.favicon.generic_string();
        if (!web.customHtmlShell.empty()) webNode["customHtmlShell"] = web.customHtmlShell.generic_string();
        if (!web.headInclude.empty()) webNode["headInclude"] = web.headInclude;
        if (web.resizeCanvasToWindow != defaultWeb.resizeCanvasToWindow) webNode["resizeCanvasToWindow"] = web.resizeCanvasToWindow;
        if (web.hideEmscriptenUI != defaultWeb.hideEmscriptenUI) webNode["hideEmscriptenUI"] = web.hideEmscriptenUI;
        encodeStringList(webNode, "scriptUrls", web.scriptUrls);
        if (web.viewportFitCover != defaultWeb.viewportFitCover) webNode["viewportFitCover"] = web.viewportFitCover;
        if (webNode.size() != 0) root["web"] = webNode;
    }

    {
        const LinuxProjectSettings& linuxSettings = project->getLinuxProjectSettings();
        const LinuxProjectSettings defaultLinux;
        YAML::Node linuxNode;
        if (!linuxSettings.applicationName.empty()) linuxNode["applicationName"] = linuxSettings.applicationName;
        if (!linuxSettings.comment.empty()) linuxNode["comment"] = linuxSettings.comment;
        if (linuxSettings.categories != defaultLinux.categories) linuxNode["categories"] = linuxSettings.categories;
        if (linuxNode.size() != 0) root["linux"] = linuxNode;
    }

    {
        const WindowsProjectSettings& windows = project->getWindowsProjectSettings();
        YAML::Node windowsNode;
        if (!windows.productName.empty()) windowsNode["productName"] = windows.productName;
        if (!windows.companyName.empty()) windowsNode["companyName"] = windows.companyName;
        if (!windows.fileVersion.empty()) windowsNode["fileVersion"] = windows.fileVersion;
        if (!windows.productVersion.empty()) windowsNode["productVersion"] = windows.productVersion;
        if (windowsNode.size() != 0) root["windows"] = windowsNode;
    }

    {
        const MacOSProjectSettings& macOS = project->getMacOSProjectSettings();
        const MacOSProjectSettings defaultMacOS;
        YAML::Node macOSNode;
        if (!macOS.applicationName.empty()) macOSNode["applicationName"] = macOS.applicationName;
        if (!macOS.bundleIdentifier.empty()) macOSNode["bundleIdentifier"] = macOS.bundleIdentifier;
        if (!macOS.versionName.empty()) macOSNode["versionName"] = macOS.versionName;
        if (!macOS.buildNumber.empty()) macOSNode["buildNumber"] = macOS.buildNumber;
        if (!macOS.icon.empty()) macOSNode["icon"] = macOS.icon.generic_string();
        if (macOS.highDpi != defaultMacOS.highDpi) macOSNode["highDpi"] = macOS.highDpi;
        encodePlistEntries(macOSNode, macOS.plistEntries);
        encodeStringList(macOSNode, "frameworks", macOS.frameworks);
        if (macOSNode.size() != 0) root["macos"] = macOSNode;
    }

    {
        const IOSProjectSettings& ios = project->getIOSProjectSettings();
        const IOSProjectSettings defaultIOS;
        YAML::Node iosNode;
        if (!ios.applicationName.empty()) iosNode["applicationName"] = ios.applicationName;
        if (!ios.bundleIdentifier.empty()) iosNode["bundleIdentifier"] = ios.bundleIdentifier;
        if (!ios.versionName.empty()) iosNode["versionName"] = ios.versionName;
        if (!ios.buildNumber.empty()) iosNode["buildNumber"] = ios.buildNumber;
        if (!ios.icon.empty()) iosNode["icon"] = ios.icon.generic_string();
        if (ios.hideStatusBar != defaultIOS.hideStatusBar) iosNode["hideStatusBar"] = ios.hideStatusBar;
        if (ios.hideHomeIndicator != defaultIOS.hideHomeIndicator) iosNode["hideHomeIndicator"] = ios.hideHomeIndicator;
        if (ios.supportsHighRefreshRate != defaultIOS.supportsHighRefreshRate) iosNode["supportsHighRefreshRate"] = ios.supportsHighRefreshRate;
        if (!ios.teamId.empty()) iosNode["teamId"] = ios.teamId;
        if (ios.exportMethod != defaultIOS.exportMethod) iosNode["exportMethod"] = ios.exportMethod;
        encodePlistEntries(iosNode, ios.plistEntries);
        if (!ios.plistXml.empty()) iosNode["plistXml"] = ios.plistXml;
        encodeStringList(iosNode, "frameworks", ios.frameworks);
        if (!ios.cocoapods.empty()) {
            YAML::Node pods(YAML::NodeType::Sequence);
            for (const NativeCocoaPod& pod : ios.cocoapods) {
                YAML::Node podNode;
                podNode["name"] = pod.name;
                if (!pod.version.empty()) podNode["version"] = pod.version;
                if (!pod.source.empty()) podNode["source"] = pod.source;
                pods.push_back(podNode);
            }
            iosNode["cocoapods"] = pods;
        }
        if (!ios.swiftPackages.empty()) {
            YAML::Node packages(YAML::NodeType::Sequence);
            for (const NativeSwiftPackage& package : ios.swiftPackages) {
                YAML::Node packageNode;
                packageNode["url"] = package.url;
                packageNode["version"] = package.version;
                encodeStringList(packageNode, "products", package.products);
                packages.push_back(packageNode);
            }
            iosNode["swiftPackages"] = packages;
        }
        if (!ios.entitlementsXml.empty()) iosNode["entitlementsXml"] = ios.entitlementsXml;
        if (ios.deploymentTarget != defaultIOS.deploymentTarget) iosNode["deploymentTarget"] = ios.deploymentTarget;
        if (iosNode.size() != 0) root["ios"] = iosNode;
    }
    encodeApplePrivacy(root, project->getApplePrivacy());

    {
        const AndroidProjectSettings& android = project->getAndroidProjectSettings();
        YAML::Node androidNode;
        if (!android.applicationName.empty()) {
            androidNode["applicationName"] = android.applicationName;
        }
        const AndroidProjectSettings defaults;
        if (!android.packageName.empty()) androidNode["packageName"] = android.packageName;
        if (android.versionCode != defaults.versionCode) androidNode["versionCode"] = android.versionCode;
        if (!android.versionName.empty()) androidNode["versionName"] = android.versionName;
        if (!android.launcherIcon.empty()) {
            androidNode["launcherIcon"] = portablePath(android.launcherIcon);
        }
        if (!android.adaptiveIconForeground.empty()) {
            androidNode["adaptiveIconForeground"] = portablePath(android.adaptiveIconForeground);
        }
        if (!android.adaptiveIconBackground.empty()) {
            androidNode["adaptiveIconBackground"] = portablePath(android.adaptiveIconBackground);
        }
        if (android.minSdk != defaults.minSdk) androidNode["minSdk"] = android.minSdk;
        if (android.targetSdk != defaults.targetSdk) androidNode["targetSdk"] = android.targetSdk;

        YAML::Node abiNode;
        if (android.abiArmeabiV7a != defaults.abiArmeabiV7a) abiNode["armeabi-v7a"] = android.abiArmeabiV7a;
        if (android.abiArm64V8a != defaults.abiArm64V8a) abiNode["arm64-v8a"] = android.abiArm64V8a;
        if (android.abiX86 != defaults.abiX86) abiNode["x86"] = android.abiX86;
        if (android.abiX86_64 != defaults.abiX86_64) abiNode["x86_64"] = android.abiX86_64;
        if (abiNode.size()) androidNode["architectures"] = abiNode;

        YAML::Node permissionsNode(YAML::NodeType::Sequence);
        for (const std::string& permission : android.permissions) {
            permissionsNode.push_back(permission);
        }
        if (android.permissions != defaults.permissions) androidNode["permissions"] = permissionsNode;
        if (android.internetAccess != defaults.internetAccess) {
            androidNode["internetAccess"] = android.internetAccess == AndroidInternetAccess::Require ? "require" : "auto";
        }

        if (android.allowBackup != defaults.allowBackup) androidNode["allowBackup"] = android.allowBackup;
        if (android.fullscreen != defaults.fullscreen) androidNode["fullscreen"] = android.fullscreen;
        if (android.packageFormat != defaults.packageFormat) androidNode["packageFormat"] = android.packageFormat;
        if (!android.keystorePath.empty() || !android.keystoreAlias.empty()) {
            YAML::Node keystoreNode;
            keystoreNode["path"] = portablePath(android.keystorePath);
            keystoreNode["alias"] = android.keystoreAlias;
            androidNode["keystore"] = keystoreNode;
        }
        encodeStringList(androidNode, "gradleDependencies", android.gradleDependencies);
        encodeStringList(androidNode, "gradleRepositories", android.gradleRepositories);
        encodeStringList(androidNode, "gradleProperties", android.gradleProperties);
        encodeStringList(androidNode, "extraPermissions", android.extraPermissions);
        if (!android.manifestXml.empty()) androidNode["manifestXml"] = android.manifestXml;
        if (!android.manifestApplicationXml.empty()) androidNode["manifestApplicationXml"] = android.manifestApplicationXml;
        if (androidNode.size()) root["android"] = androidNode;
    }

    if (project->getStartSceneId() != NULL_PROJECT_SCENE) {
        root["startSceneId"] = project->getStartSceneId();
    }

    {
        const LoadingSettings& loading = project->getLoadingSettings();
        const LoadingSettings defaults;
        YAML::Node loadingNode;
        if (loading.sceneId != defaults.sceneId) loadingNode["sceneId"] = loading.sceneId;
        if (loading.delay != defaults.delay) loadingNode["delay"] = loading.delay;
        if (loading.timeout != defaults.timeout) loadingNode["timeout"] = loading.timeout;
        if (loading.asyncLoading != defaults.asyncLoading) loadingNode["asyncLoading"] = loading.asyncLoading;
        if (loadingNode.size()) root["loading"] = loadingNode;
    }

    {
        // only what differs from Auto: an old project.yaml stays as it was
        const ThreadingProjectSettings& threading = project->getThreadingSettings();
        const ThreadingProjectSettings defaults;
        YAML::Node threadingNode;
        if (threading.workers != defaults.workers) threadingNode["workers"] = threading.workers;
        if (threading.reservedFrameWorkers != defaults.reservedFrameWorkers) threadingNode["reservedFrameWorkers"] = threading.reservedFrameWorkers;
        if (threading.ioThreads != defaults.ioThreads) threadingNode["ioThreads"] = threading.ioThreads;
        if (threading.webPumpBudgetMs != defaults.webPumpBudgetMs) threadingNode["webPumpBudgetMs"] = threading.webPumpBudgetMs;
        if (threading.webMultithreaded != defaults.webMultithreaded) threadingNode["webMultithreaded"] = threading.webMultithreaded;
        if (threadingNode.size()) root["threading"] = threadingNode;
        // only when set: an old project.yaml stays as it was
        if (project->getParticleWorldBudget() != 0) root["particleWorldBudget"] = project->getParticleWorldBudget();
        {
            const ParticleQaSettings& qa = project->getParticleQaSettings();
            const ParticleQaSettings defaults;
            YAML::Node qaNode;
            for (const ParticleQaField& field : particleQaFields()) {
                if (field.real && qa.*field.real != defaults.*field.real) qaNode[field.yaml] = qa.*field.real;
                if (field.whole && qa.*field.whole != defaults.*field.whole) qaNode[field.yaml] = qa.*field.whole;
            }
            if (qaNode.size()) root["particleQa"] = qaNode;
        }
        {
            const procmat::ProcMatQaSettings& qa = project->getProcMatQaSettings();
            const procmat::ProcMatQaSettings defaults;
            YAML::Node qaNode;
            for (const procmat::ProcMatQaField& field : procmat::procMatQaFields()) {
                if (field.real && qa.*field.real != defaults.*field.real) qaNode[field.yaml] = qa.*field.real;
                if (field.whole && qa.*field.whole != defaults.*field.whole) qaNode[field.yaml] = qa.*field.whole;
            }
            if (qaNode.size()) root["proceduralMaterialQa"] = qaNode;
        }
        // Shader Creation (spec S32): the values away from their defaults, by its reflection table
        const YAML::Node shaderCreationNode = reflect::encode(reflect::shaderCreationStruct(), &project->getShaderCreationSettings(), reflect::EncodeContext{});
        if (shaderCreationNode.IsMap() && shaderCreationNode.size()) root["shaderCreation"] = shaderCreationNode;
    }

    {
        // Project Settings > Navigation (S19): only what differs from the defaults
        const YAML::Node navigationNode = encodeNavigationProjectSettings(project->getNavigationSettings());
        if (navigationNode.size() > 0) root["navigation"] = navigationNode;
    }

    {
        // Project Settings > Texture (S14 R24): only what differs from the defaults
        const YAML::Node textureNode = encodeTextureProjectSettings(project->getTextureSettings());
        if (textureNode.size() > 0) root["texture"] = textureNode;
    }

    {
        // Project Settings > Mesh and the render budgets (S53 R19, R25): only what differs
        const YAML::Node meshNode = encodeModelProjectSettings(project->getModelSettings());
        if (meshNode.size() > 0) root["mesh"] = meshNode;
    }

    // Project Settings > Audio: only what differs from the defaults (old projects have no node)
    {
        const AudioSettings& audio = project->getAudioSettings();
        const AudioSettings defaults;
        YAML::Node audioNode;
        if (audio.mixer != defaults.mixer) audioNode["mixer"] = audio.mixer;
        if (audio.maxRealVoices != defaults.maxRealVoices) audioNode["maxRealVoices"] = audio.maxRealVoices;
        if (audio.maxVoices != defaults.maxVoices) audioNode["maxVoices"] = audio.maxVoices;
        if (audio.seed != defaults.seed) audioNode["seed"] = audio.seed;
        if (audio.captureMaxSeconds != defaults.captureMaxSeconds) audioNode["captureMaxSeconds"] = audio.captureMaxSeconds;
        if (audio.captureKeep != defaults.captureKeep) audioNode["captureKeep"] = audio.captureKeep;
        if (audio.captureStems != defaults.captureStems) audioNode["captureStems"] = audio.captureStems;
        if (audio.qa != defaults.qa) {
            YAML::Node qaNode;
            for (const AudioQaField& field : audioQaFields()) {
                if (field.real && audio.qa.*field.real != defaults.qa.*field.real) qaNode[field.yaml] = audio.qa.*field.real;
                if (field.whole && audio.qa.*field.whole != defaults.qa.*field.whole) qaNode[field.yaml] = audio.qa.*field.whole;
            }
            if (audio.qa.musicGroup != defaults.qa.musicGroup) qaNode["musicGroup"] = audio.qa.musicGroup;
            audioNode["qa"] = qaNode;
        }
        if (audio.sampleRate != defaults.sampleRate) audioNode["sampleRate"] = audio.sampleRate;
        if (audio.defaultOutputGroup != defaults.defaultOutputGroup) audioNode["defaultOutputGroup"] = audio.defaultOutputGroup;
        if (audio.musicGroup != defaults.musicGroup) audioNode["musicGroup"] = audio.musicGroup;
        if (audio.creditsFile != defaults.creditsFile) audioNode["creditsFile"] = audio.creditsFile;
        if (audio.loudnessTargetLufs != defaults.loudnessTargetLufs) audioNode["loudnessTargetLufs"] = audio.loudnessTargetLufs;
        if (audio.loudnessToleranceLu != defaults.loudnessToleranceLu) audioNode["loudnessToleranceLu"] = audio.loudnessToleranceLu;
        if (audio.truePeakCeilingDbtp != defaults.truePeakCeilingDbtp) audioNode["truePeakCeilingDbtp"] = audio.truePeakCeilingDbtp;
        if (audio.outputStage != defaults.outputStage) audioNode["outputStage"] = audio.outputStage;
        if (audio.iosSessionCategory != defaults.iosSessionCategory) audioNode["iosSessionCategory"] = audio.iosSessionCategory;
        if (audioNode.size()) root["audio"] = audioNode;
    }

    // Project Settings > Debug: what differs from the defaults (its reflection table)
    {
        const YAML::Node debugNode = reflect::encode(reflect::debugSettingsStruct(), &project->getDebugSettings(), reflect::EncodeContext{});
        if (debugNode.IsMap() && debugNode.size()) root["debug"] = debugNode;
    }

    if (!project->getStandalonePrefabs().empty()) {
        YAML::Node prefabsNode;
        for (const fs::path& prefabPath : project->getStandalonePrefabs()) {
            YAML::Node prefabNode;
            prefabNode["path"] = prefabPath.generic_string();
            const std::string guid = project->getPrefabGuid(prefabPath);
            if (!guid.empty()) {
                prefabNode["guid"] = guid;
            }
            prefabsNode.push_back(prefabNode);
        }
        root["standalonePrefabs"] = prefabsNode;
    }


    // Only the list belongs to the project; which were open, where each camera sat
    // and which guides were toggled live in the workspace
    YAML::Node scenesNode;
    {
        const auto& unresolved = project->getUnresolvedScenes();
        size_t next = 0;
        size_t position = 0;

        auto appendScene = [&scenesNode, &position, project](const std::string& filepath) {
            YAML::Node sceneNode;
            sceneNode["filepath"] = filepath;
            // S04: the scene file by GUID too, so the list follows it when it moves
            const Guid guid = project->getAssetDatabase().getAssetGuid(filepath);
            if (!guid.isNull()) sceneNode["filepathGuid"] = guid.str();
            scenesNode.push_back(sceneNode);
            position++;
        };

        // A scene this checkout could not load goes back where it was, so the list
        // reads the same for whoever can. One since loaded is skipped, or the next
        // load would build two scenes from one file.
        auto isLoaded = [project](const fs::path& filepath) {
            const fs::path normalized = filepath.lexically_normal();
            for (const auto& sceneProject : project->getScenes()) {
                if (!sceneProject.filepath.empty() && sceneProject.filepath.lexically_normal() == normalized) {
                    return true;
                }
            }
            return false;
        };

        auto appendUnresolved = [&]() {
            while (next < unresolved.size() && unresolved[next].first <= position) {
                if (!isLoaded(unresolved[next].second)) {
                    appendScene(unresolved[next].second.generic_string());
                }
                next++;
            }
        };

        // one entry per file: two scenes on one file would each load and overwrite the other
        std::unordered_map<std::string, const SceneProject*> listedFiles;
        for (const auto& sceneProject : project->getScenes()) {
            if (sceneProject.filepath.empty()) {
                continue;
            }
            const auto [listed, added] = listedFiles.emplace(project->sceneFileKey(sceneProject.filepath), &sceneProject);
            if (!added) {
                Out::warning("Scenes \"%s\" and \"%s\" both name %s: project.yaml lists the file once",
                             listed->second->name.c_str(), sceneProject.name.c_str(), sceneProject.filepath.generic_string().c_str());
                continue;
            }
            appendUnresolved();
            appendScene(sceneProject.filepath.string());
        }
        appendUnresolved();
    }
    root["scenes"] = scenesNode;

    // S25: label colours and the saved searches shared with the project
    if (!project->getAssetLabels().entries().empty()) {
        YAML::Node labels(YAML::NodeType::Sequence);
        for (const LabelColor& entry : project->getAssetLabels().entries()) {
            YAML::Node label(YAML::NodeType::Map);
            label.SetStyle(YAML::EmitterStyle::Flow);
            label["name"] = entry.name;
            label["color"] = entry.color;
            labels.push_back(label);
        }
        root["assetLabels"] = labels;
    }
    if (!project->getOrganisation().projectSearches().empty()) {
        YAML::Node searches;
        Organisation::encodeSearches(project->getOrganisation().projectSearches(), searches);
        root["savedSearches"] = searches;
    }
    // S45 R18: Project Settings > 2D > Sprite Atlas > Mode, written only when not the default
    if (project->getSpriteAtlasMode() != "enabled") {
        root["spriteAtlas"]["mode"] = project->getSpriteAtlasMode();
    }

    return root;
}

void editor::Stream::decodeProject(Project* project, const YAML::Node& node, const Workspace& workspace) {
    if (!node.IsMap()) return;

    // unknown root keys survive saves through the file overlay (Project::saveProjectFile);
    // an enum value this editor does not name cannot, it is reported as dropped
    StreamLoad::reportRootKeys(node, RootKind::Project);
    StreamLoad::BlockScope block(nullptr, "", node, RootKind::Project);

    if (node["name"]) {
        project->setName(node["name"].as<std::string>());
    }

    // "nextSceneId" is gone: a shared counter hands the same id to two branches.
    // The selected scene is restored at the end, since every scene loadScene() opens
    // selects itself and would overwrite a value applied here.

    if (node["canvasWidth"] && node["canvasHeight"]) {
        project->setCanvasSize(
            YamlNumbers::as<unsigned int>(node["canvasWidth"]),
            YamlNumbers::as<unsigned int>(node["canvasHeight"])
        );
    }

    if (node["scalingMode"]) {
        project->setScalingMode(decodeEnum(node, "scalingMode", StreamSchema::scalingMode()));
    }

    project->setUIScale(decodeCanvasScaler(node["uiScale"]));

    if (node["textureStrategy"]) {
        project->setTextureStrategy(decodeEnum(node, "textureStrategy", StreamSchema::textureStrategy()));
    }

    if (node["vsync"].IsDefined()) {
        project->setVSyncEnabled(YamlNumbers::as<bool>(node["vsync"]));
    }

    project->setDynamicBatching(node["dynamicBatching"].IsDefined() ? YamlNumbers::as<bool>(node["dynamicBatching"]) : Project::defaultDynamicBatching);

    {
        // omitted at the defaults; each platform group defaults on its own
        std::array<AtmosphereQuality, 3> quality = Project::defaultAtmosphereQuality;
        const YAML::Node qualityNode = node["atmosphereQuality"];
        if (qualityNode.IsDefined() && qualityNode.IsMap()) {
            const char* keys[3] = {"desktop", "mobile", "web"};
            for (int i = 0; i < 3; i++) {
                if (qualityNode[keys[i]].IsDefined())
                    quality[i] = decodeEnum(qualityNode, keys[i], StreamSchema::atmosphereQuality());
            }
        }
        project->setAtmosphereQuality(quality);
    }

    {
        std::array<float, 3> scale = Project::defaultFoliageDistanceScale;
        const YAML::Node scaleNode = node["foliageDistanceScale"];
        if (scaleNode.IsDefined() && scaleNode.IsMap()) {
            const char* keys[3] = {"desktop", "mobile", "web"};
            for (int i = 0; i < 3; i++)
                scale[i] = decodeFinite(scaleNode[keys[i]], scale[i]);
        }
        project->setFoliageDistanceScale(scale);
    }

    {
        const YAML::Node input = node["input"];
        project->setUnreadableInputNode("");
        project->setInputActionsMigration(InputActionsMigration());
        if (input.IsDefined() && !input.IsNull() && isInputManagerNode(input)) {
            // an older editor's Input Manager actions: the Input Manager map of an actions asset
            std::vector<InputManagerAction> actions;
            std::string error;
            if (InputManagerFormat::parseActionsJson(inputYamlToJson(input).dump(), actions, &error)) {
                const InputActionsMigration migration = convertInputManagerNode(project, actions);
                project->setInputActionsMigration(migration);
                project->setInputActionsAsset(migration.asset);
            } else {
                Out::error("Project Settings > Input could not be read from project.yaml (%s): Unity's default "
                           "actions are used, and the input node is kept as it is until an actions asset is set",
                           error.c_str());
                project->setUnreadableInputNode(YAML::Dump(input));
                project->setInputActionsAsset(fs::path());
            }
            project->setInputDeadzone(Project::defaultInputDeadzone);
        } else {
            const bool map = input.IsDefined() && input.IsMap();
            project->setInputActionsAsset(map && input["actions"].IsDefined() ? fs::path(input["actions"].as<std::string>()) : fs::path());
            project->setInputDeadzone(map && input["deadzone"].IsDefined() ? YamlNumbers::as<float>(input["deadzone"]) : Project::defaultInputDeadzone);
        }
    }

    {
        PlayerLoggingSettings logging;
        if (node["playerLogging"].IsDefined() && node["playerLogging"].IsMap()) {
            const YAML::Node loggingNode = node["playerLogging"];
            LogLevel level;
            if (loggingNode["level"].IsDefined() && loggingNode["level"].IsScalar() && parseLogLevel(loggingNode["level"].as<std::string>().c_str(), level)) {
                logging.level = logLevelName(level);
            }
            if (loggingNode["fileLog"].IsDefined()) logging.fileLog = YamlNumbers::as<bool>(loggingNode["fileLog"]);
            if (loggingNode["backtrace"].IsDefined()) logging.backtrace = YamlNumbers::as<bool>(loggingNode["backtrace"]);
        }
        project->setPlayerLogging(logging);
    }

    {
        PlayerCrashSettings crash;
        if (node["playerCrash"].IsDefined() && node["playerCrash"].IsMap()) {
            const YAML::Node crashNode = node["playerCrash"];
            if (crashNode["enabled"].IsDefined()) crash.enabled = YamlNumbers::as<bool>(crashNode["enabled"]);
            if (crashNode["logTail"].IsDefined()) crash.includeLogTail = YamlNumbers::as<bool>(crashNode["logTail"]);
            if (crashNode["dumpType"].IsDefined() && crashNode["dumpType"].IsScalar()) {
                crash.dumpWithHeap = crashNode["dumpType"].as<std::string>() == "with_heap";
            }
            if (crashNode["hangWatchdog"].IsDefined()) crash.hangWatchdog = YamlNumbers::as<bool>(crashNode["hangWatchdog"]);
            if (crashNode["hangSeconds"].IsDefined()) crash.hangSeconds = YamlNumbers::as<int>(crashNode["hangSeconds"]);
            if (crashNode["webOverlay"].IsDefined()) crash.webCrashOverlay = YamlNumbers::as<bool>(crashNode["webOverlay"]);
        }
        project->setPlayerCrash(crash);
    }

    if (node["htmlUiMsaaSamples"].IsDefined()) {
        project->setHtmlUiMsaaSamples(YamlNumbers::as<int>(node["htmlUiMsaaSamples"]));
    }

    // S56: a project saved before it has no device block, and gets the defaults
    {
        DeviceSettings device;
        if (node["device"].IsDefined()) {
            std::string deviceError;
            if (!DeviceSettings::fromJson(mcp::McpValues::yamlToJson(node["device"]).dump(), device, deviceError)) {
                Out::warning("project.yaml device: %s; the device settings take their defaults", deviceError.c_str());
                device = DeviceSettings();
            }
        }
        project->setDeviceSettings(device);
    }

    // S16: projects saved before them take the defaults
    {
        HtmlUiRenderSettings htmlUiRender;
        if (node["htmlUiGlyphAtlasMiB"].IsDefined())
            htmlUiRender.glyphAtlasMiB = YamlNumbers::as<int>(node["htmlUiGlyphAtlasMiB"]);
        if (node["htmlUiSdfPrimitives"].IsDefined())
            htmlUiRender.sdfPrimitives = YamlNumbers::as<bool>(node["htmlUiSdfPrimitives"]);
        if (node["htmlUiTransformAwareText"].IsDefined())
            htmlUiRender.transformAwareText = YamlNumbers::as<bool>(node["htmlUiTransformAwareText"]);
        project->setHtmlUiRenderSettings(htmlUiRender);
    }

    // graphics.gpuInstancing (S55): projects saved before it take the default
    {
        DrawSubmissionSettings drawSubmission;
        const YAML::Node graphics = node["graphics"];
        if (graphics && graphics.IsMap()){
            if (graphics["gpuInstancing"].IsDefined())
                drawSubmission.gpuInstancing = YamlNumbers::as<bool>(graphics["gpuInstancing"]);
        }
        project->setDrawSubmissionSettings(drawSubmission);
    }

    // graphics.shadowTier.<platform>: an absent platform takes its default tier
    for (int platform = 0; platform < ShadowTiers::PLATFORM_COUNT; platform++){
        ShadowTier tier = ShadowTiers::defaultTier(platform);
        const YAML::Node graphics = node["graphics"];
        if (graphics && graphics.IsMap() && graphics["shadowTier"] && graphics["shadowTier"].IsMap()){
            const YAML::Node value = graphics["shadowTier"][Project::getShadowTierPlatformName(platform)];
            if (value && value.IsScalar())
                tier = StreamSchema::shadowTier().fromString(value.Scalar());
        }
        project->setShadowTier(platform, tier);
    }
    // graphics.pixelsPerUnit (S41): absent is 1
    {
        const YAML::Node graphics = node["graphics"];
        int pixelsPerUnit = Project::defaultPixelsPerUnit;
        if (graphics && graphics.IsMap() && graphics["pixelsPerUnit"] && graphics["pixelsPerUnit"].IsScalar())
            pixelsPerUnit = YamlNumbers::as<int>(graphics["pixelsPerUnit"]);
        project->setPixelsPerUnit(pixelsPerUnit);
    }
    // graphics.lightMode: absent or unknown is Automatic
    {
        LightMode lightMode = Project::defaultLightMode;
        const YAML::Node graphics = node["graphics"];
        if (graphics && graphics.IsMap() && graphics["lightMode"] && graphics["lightMode"].IsScalar()){
            const std::string name = graphics["lightMode"].Scalar();
            if (!LightingTiers::modeFromName(name.c_str(), lightMode)){
                StreamLoad::unknownEnumValue(graphics["lightMode"], "LightMode", {"automatic", "per_object", "tiled"});
                lightMode = Project::defaultLightMode;
            }
        }
        project->setLightMode(lightMode);
    }

    if (node["windowMode"]) {
        project->setWindowMode(decodeEnum(node, "windowMode", StreamSchema::windowMode()));
    }

    if (node["windowWidth"] && node["windowHeight"]) {
        project->setWindowSize(
            YamlNumbers::as<unsigned int>(node["windowWidth"]),
            YamlNumbers::as<unsigned int>(node["windowHeight"])
        );
    } else {
        // Projects saved before window settings existed follow the canvas size
        project->setWindowSize(project->getCanvasWidth(), project->getCanvasHeight());
    }

    if (node["windowResizable"].IsDefined()) {
        project->setWindowResizable(YamlNumbers::as<bool>(node["windowResizable"]));
    }

    if (node["windowTitle"]) {
        project->setWindowTitle(node["windowTitle"].as<std::string>());
    }

    if (node["windowIcon"]) {
        project->setWindowIcon(FileUtils::pathFromUtf8(portablePath(node["windowIcon"].as<std::string>())));
    }

    if (node["assetsDir"]) {
        project->setAssetsDir(FileUtils::pathFromUtf8(portablePath(node["assetsDir"].as<std::string>())));
    }

    if (node["luaDir"]) {
        project->setLuaDir(FileUtils::pathFromUtf8(portablePath(node["luaDir"].as<std::string>())));
    }

    if (node["scriptDirs"]) {
        std::vector<fs::path> scriptDirs;
        for (const auto& dirNode : node["scriptDirs"]) {
            scriptDirs.push_back(FileUtils::pathFromUtf8(portablePath(dirNode.as<std::string>())));
        }
        project->setScriptDirs(std::move(scriptDirs));
    }

    if (node["cxxStandard"]) {
        const int cxxStandard = YamlNumbers::as<int>(node["cxxStandard"]);
        // C++17 projects (and unknown values) move to the default: the engine API needs C++20
        if (!isSupportedCxxStandard(cxxStandard)) {
            Out::warning("Project C++ standard %d is no longer supported; the project now builds with C++%d (see docs: cpp-scripting, \"Moving from C++17\")",
                cxxStandard, Project::defaultCxxStandard);
        }
        project->setCxxStandard(cxxStandard);
    }

    if (node["physics2D"]) {
        project->setPhysics2DEnabled(YamlNumbers::as<bool>(node["physics2D"]));
    }
    if (node["physics3D"]) {
        project->setPhysics3DEnabled(YamlNumbers::as<bool>(node["physics3D"]));
    }
    if (node["runtimeShaders"]) {
        project->setRuntimeShadersEnabled(YamlNumbers::as<bool>(node["runtimeShaders"]));
    }
    if (node["mobilePbrTier"]) {
        project->setMobilePbrTier(stringToPbrTier(node["mobilePbrTier"].as<std::string>()));
    }

    // Backward compatibility: the compiler and job count used to live here before
    // moving to the editor settings, still per project. Migrated once, never over an
    // entry this machine already has. Remove this block with support for them.
    if (node["cmakeCCompiler"] || node["cmakeCxxCompiler"] || node["cmakeGenerator"] || node["cmakeBuildJobs"]) {
        const fs::path projectFile = project->getProjectPath() / "project.yaml";
        if (!AppSettings::getBuildSettings(projectFile).configured) {
            LocalBuildSettings build;
            if (node["cmakeCCompiler"]) build.cCompiler = node["cmakeCCompiler"].as<std::string>();
            if (node["cmakeCxxCompiler"]) build.cxxCompiler = node["cmakeCxxCompiler"].as<std::string>();
            if (node["cmakeGenerator"]) build.generator = node["cmakeGenerator"].as<std::string>();
            if (node["cmakeBuildJobs"]) {
                const long long jobs = YamlNumbers::as<long long>(node["cmakeBuildJobs"]);
                const long long maxJobs = static_cast<long long>(Generator::MAX_SUPPORTED_PARALLEL_BUILD_JOBS);
                build.buildJobs = static_cast<unsigned int>(std::clamp(jobs, 0LL, maxJobs));
            }
            AppSettings::setBuildSettings(projectFile, build);
        }
    }

    if (node["packNativeResources"].IsDefined()) {
        project->setPackNativeResources(YamlNumbers::as<bool>(node["packNativeResources"]));
    }

    if (node["versionControlMetadata"].IsDefined()) {
        project->setVersionControlMetadata(YamlNumbers::as<bool>(node["versionControlMetadata"]));
    }
    {
        VcsProjectSettings vcsSettings;
        if (const YAML::Node vcsNode = node["versionControl"]; vcsNode && vcsNode.IsMap()) {
            if (vcsNode["provider"] && !vcs::parseProviderChoice(vcsNode["provider"].as<std::string>(), vcsSettings.provider)) {
                Out::warning("project.yaml: versionControl.provider must be auto, git, svn or none; auto is used");
            }
            if (vcsNode["lfs"]) vcsSettings.lfs = YamlNumbers::as<bool>(vcsNode["lfs"]);
            if (vcsNode["lockScenes"]) vcsSettings.lockScenes = YamlNumbers::as<bool>(vcsNode["lockScenes"]);
        }
        project->setVcsProjectSettings(vcsSettings, false);
    }

    // S45 R18: the Sprite Atlas mode (enabled when absent)
    {
        const std::string mode = node["spriteAtlas"] && node["spriteAtlas"].IsMap() && node["spriteAtlas"]["mode"]
                                     ? node["spriteAtlas"]["mode"].as<std::string>("enabled") : std::string("enabled");
        project->setSpriteAtlasMode(mode == "enabled_for_builds" || mode == "disabled" ? mode : "enabled");
    }

    // S25: label colours, saved searches of scope project
    {
        std::vector<LabelColor> entries;
        if (node["assetLabels"] && node["assetLabels"].IsSequence()) {
            for (const auto& label : node["assetLabels"]) {
                if (!label.IsMap() || !label["name"]) continue;
                entries.push_back({label["name"].as<std::string>(""), label["color"] ? label["color"].as<std::string>("") : std::string()});
            }
        }
        project->getAssetLabels().setEntries(std::move(entries));
        project->getOrganisation().setProjectSearches(Organisation::decodeSearches(node["savedSearches"], true));
    }

    {
        // Tags and Layers: "tags" lists the user tags, "layers" maps a user layer index to its name
        std::vector<std::string> tags;
        if (node["tags"] && node["tags"].IsSequence()) {
            for (const auto& tagNode : node["tags"]) {
                if (tagNode.IsScalar()) tags.push_back(tagNode.as<std::string>());
            }
        }
        std::vector<std::string> layerNames(MAX_LAYERS);
        if (node["layers"] && node["layers"].IsMap()) {
            for (const auto& layerNode : node["layers"]) {
                int layer = -1;
                if (!YAML::convert<int>::decode(layerNode.first, layer) || layer < 0 || layer >= MAX_LAYERS || !layerNode.second.IsScalar()) {
                    Out::warning("Ignoring layer entry \"%s\" in project settings", layerNode.first.Scalar().c_str());
                    continue;
                }
                layerNames[layer] = layerNode.second.as<std::string>();
            }
        }
        project->setTagsAndLayers(tags, layerNames);
    }

    {
        // "ignoredLayerCollisions" lists, for Physics and Physics 2D, the layer pairs that
        // do not collide; every other pair does
        auto decodeIgnoredPairs = [](const YAML::Node& pairsNode, const char* key) {
            LayerCollisionRows rows = allLayersCollide();
            if (!pairsNode || pairsNode.IsNull()) return rows;
            if (!pairsNode.IsSequence()) {
                Out::warning("Ignoring ignoredLayerCollisions.%s in project settings: not a list", key);
                return rows;
            }
            for (const auto& pairNode : pairsNode) {
                int layer1 = -1;
                int layer2 = -1;
                if (!pairNode.IsSequence() || pairNode.size() != 2 ||
                    !YAML::convert<int>::decode(pairNode[0], layer1) || !YAML::convert<int>::decode(pairNode[1], layer2) ||
                    layer1 < 0 || layer1 >= MAX_LAYERS || layer2 < 0 || layer2 >= MAX_LAYERS) {
                    Out::warning("Ignoring an ignoredLayerCollisions.%s entry in project settings: expected [layer, layer] with layers 0 to %d", key, MAX_LAYERS - 1);
                    continue;
                }
                rows[layer1] &= ~(1u << layer2);
                rows[layer2] &= ~(1u << layer1);
            }
            return rows;
        };
        const YAML::Node ignoredNode = node["ignoredLayerCollisions"];
        const bool isMap = ignoredNode && ignoredNode.IsMap();
        if (ignoredNode && !isMap) {
            Out::warning("Ignoring ignoredLayerCollisions in project settings: not a map");
        }
        project->setLayerCollisionMatrices(
            decodeIgnoredPairs(isMap ? ignoredNode["physics"] : YAML::Node(), "physics"),
            decodeIgnoredPairs(isMap ? ignoredNode["physics2D"] : YAML::Node(), "physics2D"));
    }

    {
        // Project Settings > Graphics > Shader Globals; an entry that cannot be read is left out
        // (with an error), the others are kept
        std::vector<ShaderGlobalDecl> globals;
        const YAML::Node globalsNode = node["shaderGlobals"];
        if (globalsNode && globalsNode.IsSequence()) {
            for (const YAML::Node& entry : globalsNode) {
                ShaderGlobalDecl decl;
                std::string error;
                std::vector<ShaderGlobalDecl> candidate = globals;
                if (ShaderGlobalsJson::declFromJson(yamlToJson(entry), decl, error)) {
                    candidate.push_back(decl);
                    error = ShaderGlobals::validate(candidate);
                }
                if (!error.empty()) {
                    Out::error("Project Settings > Graphics > Shader Globals: %s (left out)", error.c_str());
                    continue;
                }
                globals = candidate;
            }
        } else if (globalsNode && !globalsNode.IsNull()) {
            Out::error("Project Settings > Graphics > Shader Globals: shaderGlobals in project.yaml is not a list (ignored)");
        }
        project->setShaderGlobals(globals);
    }

    {
        // Project Settings > Physics > 3D World Capacity; missing: the defaults
        Physics3DWorldSettings world;
        const YAML::Node physicsNode = node["physics3DWorld"];
        if (physicsNode && physicsNode.IsMap()) {
            if (physicsNode["maxBodies"]) world.maxBodies = YamlNumbers::as<unsigned int>(physicsNode["maxBodies"]);
            if (physicsNode["maxBodyPairs"]) world.maxBodyPairs = YamlNumbers::as<unsigned int>(physicsNode["maxBodyPairs"]);
            if (physicsNode["maxContactConstraints"]) world.maxContactConstraints = YamlNumbers::as<unsigned int>(physicsNode["maxContactConstraints"]);
        } else if (physicsNode) {
            Out::warning("Ignoring physics3DWorld in project settings: not a map");
        }
        project->setPhysics3DWorldSettings(world);
    }


    if (node["export"] && node["export"].IsMap()) {
        const YAML::Node& exportNode = node["export"];

        ShaderOverrides& shaders = project->getShaderOverrides();
        for (ShaderKey key : Stream::decodeShaderKeys(exportNode["shaderAdditions"])) shaders.additions.insert(key);
        for (ShaderKey key : Stream::decodeShaderKeys(exportNode["shaderExclusions"])) {
            shaders.exclusions.insert(key);
            shaders.additions.erase(key);
        }

        if (exportNode["sourceCode"] && exportNode["sourceCode"].IsMap()) {
            const YAML::Node& sourceCodeNode = exportNode["sourceCode"];
            SourceCodeExportSettings& sourceCode = project->getSourceCodeExportSettings();
            if (sourceCodeNode["graphicBackends"]) {
                sourceCode.graphicBackends = decodeExportBackendSet(sourceCodeNode["graphicBackends"]);
                sourceCode.graphicBackendsConfigured = true;
            }
        }

        project->setDevelopmentBuild(exportNode["developmentBuild"] && exportNode["developmentBuild"].as<bool>());
        if (exportNode["method"] && exportNode["method"].IsScalar()) {
            const std::string method = exportNode["method"].as<std::string>();
            project->setBuildMethod(method == "prebuilt" || method == "compile" ? method : "auto");
        }
        project->setPackContent(exportNode["pack"] && exportNode["pack"].as<bool>());
        project->setStripEngineCode(!exportNode["stripEngineCode"] || exportNode["stripEngineCode"].as<bool>());

        if (exportNode["desktop"] && exportNode["desktop"].IsMap()) {
            const YAML::Node& desktopNode = exportNode["desktop"];
            DesktopExportSettings& desktop = project->getDesktopExportSettings();
            if (desktopNode["graphicBackend"]) {
                ShaderBackend backend;
                if (ShaderPool::parseShaderBackend(desktopNode["graphicBackend"].as<std::string>(), backend)) {
                    desktop.graphicBackend = backend;
                    desktop.graphicBackendConfigured = true;
                }
            }
        }
    }

    // a project saved with Player > Resolution and Presentation no longer has android.orientation
    const bool hasPresentationOrientation = node["presentation"] && node["presentation"].IsMap() &&
        (node["presentation"]["defaultOrientation"] || node["presentation"]["allowedOrientations"]);
    if (node["presentation"] && node["presentation"].IsMap()) {
        const YAML::Node& presentationNode = node["presentation"];
        PresentationSettings& presentation = project->getPresentationSettings();
        if (presentationNode["defaultOrientation"]) {
            ScreenOrientation orientation;
            if (PlatformRules::parseOrientationName(presentationNode["defaultOrientation"].as<std::string>(), orientation)) {
                presentation.defaultOrientation = orientation;
            }
        }
        presentation.allowedOrientations = nodeToOrientationMask(presentationNode["allowedOrientations"], presentation.allowedOrientations);
        if (presentationNode["keepScreenOn"].IsDefined()) presentation.keepScreenOn = YamlNumbers::as<bool>(presentationNode["keepScreenOn"]);
        if (presentationNode["targetFrameRate"].IsDefined()) {
            const int fps = YamlNumbers::as<int>(presentationNode["targetFrameRate"]);
            presentation.targetFrameRate = fps <= 0 ? -1 : std::min(fps, 1000);
        }
    }

    if (node["application"] && node["application"].IsMap()) {
        const YAML::Node& applicationNode = node["application"];
        ApplicationSettings& application = project->getApplicationSettings();
        if (applicationNode["name"]) application.name = applicationNode["name"].as<std::string>();
        if (applicationNode["identifier"]) application.identifier = applicationNode["identifier"].as<std::string>();
        if (applicationNode["version"]) application.version = applicationNode["version"].as<std::string>();
        if (applicationNode["build"]) application.build = std::max(1u, YamlNumbers::as<unsigned int>(applicationNode["build"]));
        if (applicationNode["allowInsecureHttp"].IsDefined()) application.allowInsecureHttp = YamlNumbers::as<bool>(applicationNode["allowInsecureHttp"]);
    }

    if (node["web"] && node["web"].IsMap()) {
        const YAML::Node& webNode = node["web"];
        WebProjectSettings& web = project->getWebProjectSettings();
        if (webNode["applicationName"]) web.applicationName = webNode["applicationName"].as<std::string>();
        if (webNode["favicon"]) web.favicon = webNode["favicon"].as<std::string>();
        if (webNode["customHtmlShell"]) web.customHtmlShell = webNode["customHtmlShell"].as<std::string>();
        if (webNode["headInclude"]) web.headInclude = webNode["headInclude"].as<std::string>();
        if (webNode["resizeCanvasToWindow"].IsDefined()) web.resizeCanvasToWindow = YamlNumbers::as<bool>(webNode["resizeCanvasToWindow"]);
        if (webNode["hideEmscriptenUI"].IsDefined()) web.hideEmscriptenUI = YamlNumbers::as<bool>(webNode["hideEmscriptenUI"]);
        web.scriptUrls = decodeStringList(webNode["scriptUrls"]);
        if (webNode["viewportFitCover"].IsDefined()) web.viewportFitCover = YamlNumbers::as<bool>(webNode["viewportFitCover"]);
    }

    if (node["linux"] && node["linux"].IsMap()) {
        const YAML::Node& linuxNode = node["linux"];
        LinuxProjectSettings& linuxSettings = project->getLinuxProjectSettings();
        if (linuxNode["applicationName"]) linuxSettings.applicationName = linuxNode["applicationName"].as<std::string>();
        if (linuxNode["comment"]) linuxSettings.comment = linuxNode["comment"].as<std::string>();
        if (linuxNode["categories"]) linuxSettings.categories = linuxNode["categories"].as<std::string>();
    }

    if (node["windows"] && node["windows"].IsMap()) {
        const YAML::Node& windowsNode = node["windows"];
        WindowsProjectSettings& windows = project->getWindowsProjectSettings();
        if (windowsNode["productName"]) windows.productName = windowsNode["productName"].as<std::string>();
        if (windowsNode["companyName"]) windows.companyName = windowsNode["companyName"].as<std::string>();
        if (windowsNode["fileVersion"]) windows.fileVersion = windowsNode["fileVersion"].as<std::string>();
        if (windowsNode["productVersion"]) windows.productVersion = windowsNode["productVersion"].as<std::string>();
    }

    if (node["macos"] && node["macos"].IsMap()) {
        const YAML::Node& macOSNode = node["macos"];
        MacOSProjectSettings& macOS = project->getMacOSProjectSettings();
        if (macOSNode["applicationName"]) macOS.applicationName = macOSNode["applicationName"].as<std::string>();
        if (macOSNode["bundleIdentifier"]) macOS.bundleIdentifier = macOSNode["bundleIdentifier"].as<std::string>();
        if (macOSNode["versionName"]) macOS.versionName = macOSNode["versionName"].as<std::string>();
        if (macOSNode["buildNumber"]) macOS.buildNumber = macOSNode["buildNumber"].as<std::string>();
        if (macOSNode["icon"]) macOS.icon = macOSNode["icon"].as<std::string>();
        if (macOSNode["highDpi"].IsDefined()) macOS.highDpi = YamlNumbers::as<bool>(macOSNode["highDpi"]);
        macOS.plistEntries = decodePlistEntries(macOSNode["plistEntries"]);
        macOS.frameworks = decodeStringList(macOSNode["frameworks"]);
    }

    project->setApplePrivacy(node["apple"] && node["apple"].IsMap() ? decodeApplePrivacy(node["apple"]["privacy"])
                                                                    : ApplePrivacy::Manifest());

    if (node["ios"] && node["ios"].IsMap()) {
        const YAML::Node& iosNode = node["ios"];
        IOSProjectSettings& ios = project->getIOSProjectSettings();
        if (iosNode["applicationName"]) ios.applicationName = iosNode["applicationName"].as<std::string>();
        if (iosNode["bundleIdentifier"]) ios.bundleIdentifier = iosNode["bundleIdentifier"].as<std::string>();
        if (iosNode["versionName"]) ios.versionName = iosNode["versionName"].as<std::string>();
        if (iosNode["buildNumber"]) ios.buildNumber = iosNode["buildNumber"].as<std::string>();
        if (iosNode["icon"]) ios.icon = iosNode["icon"].as<std::string>();
        if (iosNode["hideStatusBar"].IsDefined()) ios.hideStatusBar = YamlNumbers::as<bool>(iosNode["hideStatusBar"]);
        if (iosNode["hideHomeIndicator"].IsDefined()) ios.hideHomeIndicator = YamlNumbers::as<bool>(iosNode["hideHomeIndicator"]);
        if (iosNode["supportsHighRefreshRate"].IsDefined()) ios.supportsHighRefreshRate = YamlNumbers::as<bool>(iosNode["supportsHighRefreshRate"]);
        if (iosNode["teamId"]) ios.teamId = iosNode["teamId"].as<std::string>();
        if (iosNode["exportMethod"]) {
            const std::string method = iosNode["exportMethod"].as<std::string>();
            if (method == "development" || method == "ad-hoc" || method == "app-store") ios.exportMethod = method;
        }
        ios.plistEntries = decodePlistEntries(iosNode["plistEntries"]);
        if (iosNode["plistXml"]) ios.plistXml = iosNode["plistXml"].as<std::string>();
        ios.frameworks = decodeStringList(iosNode["frameworks"]);
        ios.cocoapods.clear();
        if (iosNode["cocoapods"] && iosNode["cocoapods"].IsSequence()) {
            for (const YAML::Node& podNode : iosNode["cocoapods"]) {
                if (!podNode.IsMap() || !podNode["name"]) continue;
                NativeCocoaPod pod;
                pod.name = podNode["name"].as<std::string>();
                if (podNode["version"]) pod.version = podNode["version"].as<std::string>();
                if (podNode["source"]) pod.source = podNode["source"].as<std::string>();
                ios.cocoapods.push_back(pod);
            }
        }
        ios.swiftPackages.clear();
        if (iosNode["swiftPackages"] && iosNode["swiftPackages"].IsSequence()) {
            for (const YAML::Node& packageNode : iosNode["swiftPackages"]) {
                if (!packageNode.IsMap() || !packageNode["url"]) continue;
                NativeSwiftPackage package;
                package.url = packageNode["url"].as<std::string>();
                if (packageNode["version"]) package.version = packageNode["version"].as<std::string>();
                package.products = decodeStringList(packageNode["products"]);
                ios.swiftPackages.push_back(package);
            }
        }
        if (iosNode["entitlementsXml"]) ios.entitlementsXml = iosNode["entitlementsXml"].as<std::string>();
        if (iosNode["deploymentTarget"]) ios.deploymentTarget = iosNode["deploymentTarget"].as<std::string>();
    }

    if (node["android"] && node["android"].IsMap()) {
        const YAML::Node& androidNode = node["android"];
        AndroidProjectSettings& android = project->getAndroidProjectSettings();

        if (androidNode["applicationName"]) android.applicationName = androidNode["applicationName"].as<std::string>();
        if (androidNode["packageName"]) android.packageName = androidNode["packageName"].as<std::string>();
        if (androidNode["versionCode"]) android.versionCode = YamlNumbers::as<unsigned int>(androidNode["versionCode"]);
        if (androidNode["versionName"]) android.versionName = androidNode["versionName"].as<std::string>();
        if (androidNode["launcherIcon"]) android.launcherIcon = FileUtils::pathFromUtf8(portablePath(androidNode["launcherIcon"].as<std::string>()));
        if (androidNode["adaptiveIconForeground"]) android.adaptiveIconForeground = FileUtils::pathFromUtf8(portablePath(androidNode["adaptiveIconForeground"].as<std::string>()));
        if (androidNode["adaptiveIconBackground"]) android.adaptiveIconBackground = FileUtils::pathFromUtf8(portablePath(androidNode["adaptiveIconBackground"].as<std::string>()));
        if (androidNode["minSdk"]) android.minSdk = std::max(1u, YamlNumbers::as<unsigned int>(androidNode["minSdk"]));
        if (androidNode["targetSdk"]) android.targetSdk = std::max(android.minSdk, YamlNumbers::as<unsigned int>(androidNode["targetSdk"]));
        // the Android-only orientation of older projects becomes the shared one (Player >
        // Resolution and Presentation), unless the project already has it; written back as
        // presentation on the next save
        if (androidNode["orientation"] && !hasPresentationOrientation) {
            migrateAndroidOrientation(androidNode["orientation"].as<std::string>(), project->getPresentationSettings());
        }

        if (androidNode["architectures"] && androidNode["architectures"].IsMap()) {
            const YAML::Node& abiNode = androidNode["architectures"];
            if (abiNode["armeabi-v7a"].IsDefined()) android.abiArmeabiV7a = YamlNumbers::as<bool>(abiNode["armeabi-v7a"]);
            if (abiNode["arm64-v8a"].IsDefined()) android.abiArm64V8a = YamlNumbers::as<bool>(abiNode["arm64-v8a"]);
            if (abiNode["x86"].IsDefined()) android.abiX86 = YamlNumbers::as<bool>(abiNode["x86"]);
            if (abiNode["x86_64"].IsDefined()) android.abiX86_64 = YamlNumbers::as<bool>(abiNode["x86_64"]);
        }

        if (androidNode["permissions"] && androidNode["permissions"].IsSequence()) {
            android.permissions.clear();
            for (const auto& permissionNode : androidNode["permissions"]) {
                android.permissions.insert(permissionNode.as<std::string>());
            }
        }

        if (androidNode["internetAccess"]) {
            android.internetAccess = androidNode["internetAccess"].as<std::string>() == "require"
                ? AndroidInternetAccess::Require : AndroidInternetAccess::Auto;
        }
        if (androidNode["allowBackup"].IsDefined()) android.allowBackup = YamlNumbers::as<bool>(androidNode["allowBackup"]);
        if (androidNode["fullscreen"].IsDefined()) android.fullscreen = YamlNumbers::as<bool>(androidNode["fullscreen"]);
        // android.keepScreenOn is dropped: exported games always kept the screen on (the native
        // window flag), which presentation.keepScreenOn (default true) keeps doing
        if (androidNode["packageFormat"]) {
            // aab, apk-per-abi or apk (universal); an unknown word reads as the default
            AndroidFormat format = AndroidFormat::Aab;
            parseAndroidFormat(androidNode["packageFormat"].as<std::string>(), format);
            android.packageFormat = androidFormatName(format);
        }
        if (androidNode["keystore"] && androidNode["keystore"].IsMap()) {
            if (androidNode["keystore"]["path"]) android.keystorePath = FileUtils::pathFromUtf8(portablePath(androidNode["keystore"]["path"].as<std::string>()));
            if (androidNode["keystore"]["alias"]) android.keystoreAlias = androidNode["keystore"]["alias"].as<std::string>();
        }
        android.gradleDependencies = decodeStringList(androidNode["gradleDependencies"]);
        android.gradleRepositories = decodeStringList(androidNode["gradleRepositories"]);
        android.gradleProperties = decodeStringList(androidNode["gradleProperties"]);
        android.extraPermissions = decodeStringList(androidNode["extraPermissions"]);
        if (androidNode["manifestXml"]) android.manifestXml = androidNode["manifestXml"].as<std::string>();
        if (androidNode["manifestApplicationXml"]) android.manifestApplicationXml = androidNode["manifestApplicationXml"].as<std::string>();
    }

    if (node["startSceneId"]) {
        project->setStartSceneId(YamlNumbers::as<uint32_t>(node["startSceneId"]));
    }

    if (node["loading"]) {
        const YAML::Node loadingNode = node["loading"];
        LoadingSettings& loading = project->getLoadingSettings();
        if (loadingNode["sceneId"]) loading.sceneId = YamlNumbers::as<uint32_t>(loadingNode["sceneId"]);
        loading.delay = std::max(0.0f, decodeFinite(loadingNode["delay"], loading.delay));
        loading.timeout = std::max(0.0f, decodeFinite(loadingNode["timeout"], loading.timeout));
        if (loadingNode["asyncLoading"]) loading.asyncLoading = YamlNumbers::as<bool>(loadingNode["asyncLoading"]);
    }

    {
        // absent keys are Auto; values out of range clamp to what the settings offer
        ThreadingProjectSettings threading;
        if (node["threading"]) {
            const YAML::Node threadingNode = node["threading"];
            auto readCount = [&threadingNode](const char* key, int fallback, int maximum) {
                if (!threadingNode[key]) return fallback;
                const int value = YamlNumbers::as<int>(threadingNode[key]);
                return value < 0 ? -1 : std::min(value, maximum);
            };
            threading.workers = readCount("workers", threading.workers, 15);
            if (threading.workers == 0) threading.workers = 1;
            threading.reservedFrameWorkers = readCount("reservedFrameWorkers", threading.reservedFrameWorkers, 4);
            threading.ioThreads = readCount("ioThreads", threading.ioThreads, 4);
            threading.webPumpBudgetMs = std::max(0.5f, std::min(50.0f, decodeFinite(threadingNode["webPumpBudgetMs"], threading.webPumpBudgetMs)));
            if (threadingNode["webMultithreaded"]) threading.webMultithreaded = YamlNumbers::as<bool>(threadingNode["webMultithreaded"]);
        }
        project->setThreadingSettings(threading);
        // missing: 0, the platform's default
        project->setParticleWorldBudget(node["particleWorldBudget"] ? std::min(YamlNumbers::as<unsigned int>(node["particleWorldBudget"]), Project::maxParticleWorldBudget) : 0u);
        ParticleQaSettings particleQa;
        if (node["particleQa"] && node["particleQa"].IsMap()) {
            const YAML::Node qaNode = node["particleQa"];
            for (const ParticleQaField& field : particleQaFields()) {
                if (!qaNode[field.yaml]) continue;
                if (field.real) particleQa.*field.real = std::clamp(YamlNumbers::as<float>(qaNode[field.yaml]), (float)field.minimum, (float)field.maximum);
                else particleQa.*field.whole = std::clamp(YamlNumbers::as<int>(qaNode[field.yaml]), (int)field.minimum, (int)field.maximum);
            }
        }
        project->setParticleQaSettings(particleQa);
        procmat::ProcMatQaSettings procMatQa;
        if (node["proceduralMaterialQa"] && node["proceduralMaterialQa"].IsMap()) {
            const YAML::Node qaNode = node["proceduralMaterialQa"];
            for (const procmat::ProcMatQaField& field : procmat::procMatQaFields()) {
                if (!qaNode[field.yaml]) continue;
                if (field.real) procMatQa.*field.real = std::clamp(YamlNumbers::as<float>(qaNode[field.yaml]), (float)field.minimum, (float)field.maximum);
                else procMatQa.*field.whole = std::clamp(YamlNumbers::as<int>(qaNode[field.yaml]), (int)field.minimum, (int)field.maximum);
            }
        }
        project->setProcMatQaSettings(procMatQa);
    }

    {
        ShaderCreationProjectSettings shaderCreation;
        if (node["shaderCreation"] && node["shaderCreation"].IsMap()){
            reflect::DecodeContext ctx;
            ctx.component = "shaderCreation";
            reflect::decode(reflect::shaderCreationStruct(), node["shaderCreation"], &shaderCreation, ctx);
            shaderCreation.clamp();
        }
        project->setShaderCreationSettings(shaderCreation);
    }

    project->setNavigationSettings(decodeNavigationProjectSettings(node["navigation"]));

    {
        TextureProjectSettings textureSettings;
        decodeTextureProjectSettings(node["texture"], textureSettings);
        project->setTextureSettings(textureSettings);
        ModelProjectSettings modelSettings;
        decodeModelProjectSettings(node["mesh"], modelSettings);
        project->setModelSettings(modelSettings);
        AudioSettings audio;
        if (node["audio"] && node["audio"].IsMap()) {
            const YAML::Node audioNode = node["audio"];
            if (audioNode["mixer"]) audio.mixer = audioNode["mixer"].as<std::string>();
            if (audioNode["maxRealVoices"]) audio.maxRealVoices = YamlNumbers::as<int>(audioNode["maxRealVoices"]);
            if (audioNode["maxVoices"]) audio.maxVoices = YamlNumbers::as<int>(audioNode["maxVoices"]);
            if (audioNode["seed"]) audio.seed = YamlNumbers::as<int>(audioNode["seed"]);
            if (audioNode["captureMaxSeconds"]) audio.captureMaxSeconds = YamlNumbers::as<int>(audioNode["captureMaxSeconds"]);
            if (audioNode["captureKeep"]) audio.captureKeep = YamlNumbers::as<int>(audioNode["captureKeep"]);
            if (audioNode["captureStems"]) audio.captureStems = audioNode["captureStems"].as<bool>();
            if (audioNode["qa"] && audioNode["qa"].IsMap()) {
                const YAML::Node qaNode = audioNode["qa"];
                for (const AudioQaField& field : audioQaFields()) {
                    if (!qaNode[field.yaml]) continue;
                    if (field.real) audio.qa.*field.real = YamlNumbers::as<float>(qaNode[field.yaml]);
                    else audio.qa.*field.whole = YamlNumbers::as<int>(qaNode[field.yaml]);
                }
                if (qaNode["musicGroup"]) audio.qa.musicGroup = qaNode["musicGroup"].as<std::string>();
            }
            if (audioNode["sampleRate"]) audio.sampleRate = YamlNumbers::as<int>(audioNode["sampleRate"]);
            if (audioNode["defaultOutputGroup"]) audio.defaultOutputGroup = audioNode["defaultOutputGroup"].as<std::string>();
            if (audioNode["musicGroup"]) audio.musicGroup = audioNode["musicGroup"].as<std::string>();
            if (audioNode["creditsFile"]) audio.creditsFile = audioNode["creditsFile"].as<bool>();
            if (audioNode["loudnessTargetLufs"]) audio.loudnessTargetLufs = YamlNumbers::as<float>(audioNode["loudnessTargetLufs"]);
            if (audioNode["loudnessToleranceLu"]) audio.loudnessToleranceLu = YamlNumbers::as<float>(audioNode["loudnessToleranceLu"]);
            if (audioNode["truePeakCeilingDbtp"]) audio.truePeakCeilingDbtp = YamlNumbers::as<float>(audioNode["truePeakCeilingDbtp"]);
            if (audioNode["outputStage"]) audio.outputStage = audioNode["outputStage"].as<std::string>();
            if (audioNode["iosSessionCategory"]) audio.iosSessionCategory = audioNode["iosSessionCategory"].as<std::string>();
        }
        // also without a node: a project opened after another one does not keep its mixer
        project->setAudioSettings(audio);
    }

    // Project Settings > Debug by its reflection table; a value that does not read keeps its
    // default (an enum's fallback is its default; reported)
    {
        DebugSettings debug;
        if (node["debug"] && node["debug"].IsMap()) {
            reflect::DecodeContext ctx;
            ctx.component = "debug";
            reflect::decode(reflect::debugSettingsStruct(), node["debug"], &debug, ctx);
            debug.sanitize([](const std::string& key, const std::string& value) {
                Out::warning("Ignoring debug.%s \"%s\" in project settings: not a key name", key.c_str(), value.c_str());
            });
        }
        // a project without the node (older ones) gets the defaults
        project->setDebugSettings(debug);
    }

    if (node["standalonePrefabs"]) {
        std::vector<fs::path> prefabPaths;
        for (const auto& pathNode : node["standalonePrefabs"]) {
            // {path, guid}, or a plain path from an older file
            fs::path path = pathNode.IsMap() ? fs::path(pathNode["path"].as<std::string>("")) : fs::path(pathNode.as<std::string>());
            const std::string guid = pathNode.IsMap() && pathNode["guid"] ? pathNode["guid"].as<std::string>() : std::string();
            fs::path resolved = project->resolvePrefabPath(guid, path);
            prefabPaths.push_back(resolved.empty() ? path : resolved);
        }
        project->setStandalonePrefabs(std::move(prefabPaths));
    }

    if (workspace.hasTerrainEditorSettings()) {
        project->setTerrainEditorSettings(workspace.getTerrainEditorSettings());
    }
    project->getPhysicsPlacementSettings() = workspace.getPhysicsPlacementSettings();
    project->getSpriteNormalsSettings() = workspace.getSpriteNormalsSettings();

    project->setSimulateTouch(workspace.isSimulateTouch());
    project->setCustomResolutions(workspace.getCustomResolutions());

    // Which scenes are open is a per-user choice, so the tab list decides it. With no
    // workspace yet, the first scene opens, as it does for a fresh clone.
    std::set<std::string> openedScenePaths;
    // the Project Hub's Open Without Last Scene (S27 R15): as with no workspace, the first scene
    const bool hasTabs = workspace.hasTabs() && !project->takeSkipWorkspaceTabs();
    if (hasTabs) {
        for (const TabEntry& tab : workspace.getTabs()) {
            if (tab.type == TabType::SCENE) {
                openedScenePaths.insert(tab.filepath);
            }
            project->addTab(tab.type, tab.filepath);
        }
    }

    bool isFirstScene = true;
    size_t listPosition = 0;
    std::unordered_set<std::string> listedFiles;
    if (node["scenes"]) {
        for (const auto& sceneNode : node["scenes"]) {
            if (!sceneNode["filepath"]) {
                continue;
            }

            std::string relStr = sceneNode["filepath"].as<std::string>();
            const size_t position = listPosition++;
            // S04: a scene file moved since is found by its GUID
            if (sceneNode["filepathGuid"] && !fs::exists(project->getProjectPath() / relStr)) {
                const fs::path moved = project->getAssetDatabase().getPath(Guid::parse(sceneNode["filepathGuid"].as<std::string>()));
                if (!moved.empty()) {
                    Out::info("Scene \"%s\" moved to \"%s\" (found by its GUID)", relStr.c_str(), moved.generic_string().c_str());
                    relStr = moved.generic_string();
                }
            }
            // a second entry for one file would build a second scene saving into it
            if (!listedFiles.insert(project->sceneFileKey(relStr)).second) {
                Out::warning("project.yaml lists %s twice: the second entry is ignored (the next save writes it once)", relStr.c_str());
                continue;
            }
            fs::path scenePath = relStr;
            if (scenePath.is_relative()) {
                scenePath = project->getProjectPath() / scenePath;
            }

            const bool opened = hasTabs ? openedScenePaths.count(relStr) > 0 : isFirstScene;

            // Absent or unreadable, the scene is one this checkout cannot show, and
            // remembering it stops a later save from taking it from everyone else.
            // loadScene() drops what it built, so scenes.back() is an earlier scene.
            if (!fs::exists(scenePath)) {
                Out::warning("Scene file not found, keeping it listed in the project: %s", relStr.c_str());
                project->addUnresolvedScene(position, relStr);
                continue;
            }
            if (!project->loadScene(scenePath, opened, true, opened, true)) {
                project->addUnresolvedScene(position, relStr);
                continue;
            }

            // Only a scene that opened uses up the slot, or a checkout whose first
            // scene is missing would start with nothing open
            isFirstScene = false;

            auto& scenes = project->getScenes();
            if (scenes.empty()) {
                continue;
            }

            const Workspace::SceneState* state = workspace.getSceneState(relStr);
            if (!state) {
                continue;
            }

            SceneProject& loadedScene = scenes.back();
            if (state->hasDisplaySettings) {
                loadedScene.displaySettings = state->displaySettings;
            }
            // the Scene view's own state (S26): applied once the camera below is in place
            loadedScene.viewState = state->viewState;
            if (!state->editorCamera.IsDefined()) {
                continue;
            }

            loadedScene.editorCameraState = YAML::Clone(state->editorCamera);
            if (!loadedScene.sceneRender) {
                continue;
            }
            Camera* editorCam = loadedScene.sceneRender->getCamera();
            if (!editorCam) {
                continue;
            }

            float zoom = 0.0f;
            float walkSpeedOffset = 0.0f;
            Stream::decodeEditorCamera(editorCam, state->editorCamera, zoom, walkSpeedOffset);
            if ((loadedScene.sceneType == SceneType::SCENE_2D || loadedScene.sceneType == SceneType::SCENE_UI) && zoom > 0.0f) {
                static_cast<SceneRender2D*>(loadedScene.sceneRender)->setZoom(zoom);
            }
            if (loadedScene.sceneType == SceneType::SCENE_3D) {
                static_cast<SceneRender3D*>(loadedScene.sceneRender)->setWalkSpeedOffset(walkSpeedOffset);
            }
        }
    }

    // Every scene has had its turn at selecting itself, so the saved choice goes back
    // now. One naming a scene this checkout cannot open leaves the loop's standing.
    if (workspace.hasSelectedScene()) {
        const uint32_t savedSelection = workspace.getSelectedScene();
        const SceneProject* saved = project->getScene(savedSelection);
        if (saved && saved->opened) {
            project->setSelectedSceneId(savedSelection);
        }
    }
}

YAML::Node editor::Stream::encodePlaySnapshot(const Project* project, const SceneProject* sceneProject) {
    reflect::PlaySnapshotEncodeScope snapshot;
    return encodeSceneProject(project, sceneProject);
}

YAML::Node editor::Stream::encodeSceneProject(const Project* project, const SceneProject* sceneProject) {
    // a scene file: without the values loading computes (S29 R20)
    reflect::DocumentEncodeScope document;
    YAML::Node root;
    root[StreamMigrations::versionKey(DocumentKind::Scene)] = StreamMigrations::currentVersion(DocumentKind::Scene);
    root["id"] = sceneProject->id;
    root["name"] = sceneProject->name;
    YAML::Node sceneNode = encodeScene(sceneProject->scene);
    StreamSave::applyBlock(sceneProject->scene, kSceneSettingsBlock, sceneNode);
    root["scene"] = sceneNode;
    root["sceneType"] = sceneTypeToString(sceneProject->sceneType);
    root["mainCamera"] = sceneProject->mainCamera;

    // an older editor's capacities and collider caps, as read (nothing reads them; a scene saved
    // since has none)
    if (sceneProject->legacyMaxValues.IsDefined() && !sceneProject->legacyMaxValues.IsNull()) {
        root["maxValues"] = YAML::Clone(sceneProject->legacyMaxValues);
    }

    YAML::Node shaderKeysNode = Stream::encodeShaderKeys(sceneProject->shaderKeys);
    if (shaderKeysNode.size()) {
        root["shaderKeys"] = shaderKeysNode;
    }

    if (!sceneProject->childScenes.empty()) {
        YAML::Node childScenesNode;
        for (const ChildSceneRef& childSceneRef : sceneProject->childScenes) {
            YAML::Node childSceneNode;
            childSceneNode["id"] = childSceneRef.id;
            childSceneNode["startActive"] = childSceneRef.startActive;
            childScenesNode.push_back(childSceneNode);
        }
        root["childScenes"] = childScenesNode;
    }

    if (!sceneProject->cppScripts.empty()) {
        YAML::Node scriptsNode;
        for (const auto& script : sceneProject->cppScripts) {
            YAML::Node scriptNode;
            scriptNode["path"] = script.path.generic_string();
            if (!script.headerPath.empty()) {
                scriptNode["headerPath"] = script.headerPath.generic_string();
            }
            if (!script.className.empty()) {
                scriptNode["className"] = script.className;
            }

            if (!script.properties.empty()) {
                YAML::Node propertiesNode;
                for (const auto& prop : script.properties) {
                    YAML::Node propNode;
                    propNode["name"] = prop.name;
                    propNode["isPtr"] = prop.isPtr;
                    if (!prop.ptrTypeName.empty()) {
                        propNode["ptrTypeName"] = prop.ptrTypeName;
                    }
                    propertiesNode.push_back(propNode);
                }
                scriptNode["properties"] = propertiesNode;
            }

            scriptsNode.push_back(scriptNode);
        }
        root["cppScripts"] = scriptsNode;
    }

    if (!sceneProject->prefabs.empty()) {
        YAML::Node prefabsNode;
        for (const auto& prefab : sceneProject->prefabs) {
            // the GUID finds the prefab, the path is a hint (a plain path in older files)
            YAML::Node prefabNode;
            prefabNode["path"] = prefab.prefabPath.generic_string();
            if (!prefab.guid.empty()) {
                prefabNode["guid"] = prefab.guid;
            }
            prefabsNode.push_back(prefabNode);
        }
        root["prefabs"] = prefabsNode;
    }

    if (!sceneProject->imports.empty()) {
        YAML::Node importsNode;
        for (const auto& [path, id] : sceneProject->imports) {
            importsNode[path] = id;
        }
        root["imports"] = importsNode;
    }

    // Every GameObject of the scene goes into the node, or nothing does: a save (or an autosave,
    // a Play snapshot) that would lose one fails, naming them, and the file stays as it was
    SavedEntitiesCheck check(sceneProject);
    YAML::Node entitiesNode;
    for (Entity entity : sceneProject->entities) {
        // Skip prefab children (they are stored in the prefab file)
        if (project) {
            fs::path prefabPath = project->findPrefabPathFor(sceneProject->id, entity);
            if (!prefabPath.empty()) {
                const Prefab* prefab = project->getPrefab(prefabPath);
                if (prefab && prefab->getRootEntity(sceneProject->id, entity) != entity) {
                    continue;
                }
            }
        }

        if (Transform* transform = sceneProject->scene->findComponent<Transform>(entity)) {
            if (transform->parent == NULL_ENTITY) {
                entitiesNode.push_back(encodeEntity(entity, sceneProject->scene, project, sceneProject));
            }
        }else{
            entitiesNode.push_back(encodeEntity(entity, sceneProject->scene, project, sceneProject));
        }
    }

    const std::vector<Entity> left = check.leftOut(project);
    if (!left.empty()) {
        throw Stream::IncompleteSave(check.describe(left, "scene '" + sceneProject->name + "'") +
            " (an editor fault: please report it); nothing was written");
    }
    // Complete, but the order every move and encode reads is broken: said, for a report
    if (const Entity split = ProjectUtils::findSplitBranch(sceneProject->scene); split != NULL_ENTITY) {
        Out::warning("Scene '%s': the Transform order splits the branch of '%s' (%u), an editor fault (every GameObject was written)",
                     sceneProject->name.c_str(), sceneProject->scene->getEntityName(split).c_str(), split);
    }

    root["entities"] = entitiesNode;

    // the root keys of the file this editor does not know, after its own
    StreamSave::applyRootKeys(sceneProject->scene, NULL_ENTITY, root);

    return root;
}

namespace {
    // The innermost check on this thread (a save on another thread has its own)
    thread_local editor::Stream::SavedEntitiesCheck* savedEntitiesCheck = nullptr;
}

editor::Stream::SavedEntitiesCheck::SavedEntitiesCheck(const SceneProject* sceneProject):
        sceneProject(sceneProject), outer(savedEntitiesCheck) {
    savedEntitiesCheck = this;
}

editor::Stream::SavedEntitiesCheck::~SavedEntitiesCheck() {
    savedEntitiesCheck = outer;
}

void editor::Stream::SavedEntitiesCheck::written(const EntityRegistry* registry, Entity entity) {
    SavedEntitiesCheck* check = savedEntitiesCheck;
    if (check && check->sceneProject && check->sceneProject->scene == registry) {
        check->recorded.insert(entity);
    }
}

std::vector<Entity> editor::Stream::SavedEntitiesCheck::leftOut(const Project* project) const {
    std::vector<Entity> left;
    if (!sceneProject || !sceneProject->scene) {
        return left;
    }
    const Scene* scene = sceneProject->scene;
    std::unordered_set<Entity> covered = recorded;
    if (project) {
        // An instance whose root is in the file holds its members, a nested instance's root
        // among them: as deep as the nesting goes
        std::vector<const Prefab::Instance*> instances;
        for (const auto& [path, prefab] : project->getPrefabs(sceneProject->id)) {
            auto it = prefab->instances.find(sceneProject->id);
            if (it != prefab->instances.end()) {
                for (const Prefab::Instance& instance : it->second) {
                    instances.push_back(&instance);
                }
            }
        }
        std::vector<bool> taken(instances.size(), false);
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t i = 0; i < instances.size(); i++) {
                if (taken[i] || !covered.count(instances[i]->rootEntity)) {
                    continue;
                }
                taken[i] = true;
                grew = true;
                for (const Prefab::EntityMember& member : instances[i]->members) {
                    covered.insert(member.localEntity);
                }
            }
        }
    }
    for (Entity entity : sceneProject->entities) {
        if (scene->isEntityCreated(entity) && !covered.count(entity)) {
            left.push_back(entity);
        }
    }
    return left;
}

std::string editor::Stream::SavedEntitiesCheck::describe(const std::vector<Entity>& left, const std::string& where) const {
    Scene* scene = sceneProject->scene;
    std::string names;
    for (size_t i = 0; i < left.size() && i < 8; i++) {
        names += (i ? ", '" : "'") + scene->getEntityName(left[i]) + "' " + std::to_string(left[i]);
    }
    if (left.size() > 8) {
        names += ", ...";
    }
    std::string text = std::to_string(left.size()) + " GameObject(s) of " + where + " would be left out (" + names + ")";
    if (const Entity split = ProjectUtils::findSplitBranch(scene); split != NULL_ENTITY) {
        text += "; the Transform order splits the branch of '" + scene->getEntityName(split) + "' " + std::to_string(split);
    }
    return text;
}

// The C++ scripts a scene file's header lists (what the scene had when saved)
void editor::Stream::decodeSceneCppScripts(SceneProject* sceneProject, const YAML::Node& node) {
    sceneProject->cppScripts.clear();
    if (node["cppScripts"]) {
        for (const auto& scriptNode : node["cppScripts"]) {
            SceneScriptSource script;

            if (scriptNode["path"]) {
                script.path = fs::path(scriptNode["path"].as<std::string>());
            }
            if (scriptNode["headerPath"]) {
                script.headerPath = fs::path(scriptNode["headerPath"].as<std::string>());
            }
            if (scriptNode["className"]) {
                script.className = scriptNode["className"].as<std::string>();
            }
            if (scriptNode["properties"]) {
                for (const auto& propNode : scriptNode["properties"]) {
                    ScriptPropertyInfo prop;
                    if (propNode["name"]) {
                        prop.name = propNode["name"].as<std::string>();
                    }
                    if (propNode["isPtr"]) {
                        prop.isPtr = YamlNumbers::as<bool>(propNode["isPtr"]);
                    }
                    if (propNode["ptrTypeName"]) {
                        prop.ptrTypeName = propNode["ptrTypeName"].as<std::string>();
                    }
                    script.properties.push_back(prop);
                }
            }

            sceneProject->cppScripts.push_back(script);
        }
    } else if (node["cppScriptPaths"]) {
        for (const auto& pathNode : node["cppScriptPaths"]) {
            SceneScriptSource script;
            script.path = fs::path(pathNode.as<std::string>());
            sceneProject->cppScripts.push_back(script);
        }
    }
}

void editor::Stream::decodeSceneProject(SceneProject* sceneProject, const YAML::Node& node, bool loadScene) {
    if (node["id"]) sceneProject->id = YamlNumbers::as<uint32_t>(node["id"]);
    if (node["name"]) sceneProject->name = node["name"].as<std::string>();
    if (loadScene){
        if (node["scene"]) {
            if (!sceneProject->scene) {
                sceneProject->scene = new Scene();
            }
            // the settings block keeps what it does not read like a component does
            StreamLoad::BlockScope block(sceneProject->scene, kSceneSettingsBlock, node["scene"], RootKind::SceneSettings);
            decodeScene(sceneProject->scene, node["scene"]);
            block.finish(encodeScene(sceneProject->scene));
        }
        if (sceneProject->scene) {
            StreamLoad::preserveRootKeys(sceneProject->scene, NULL_ENTITY, node, RootKind::Scene);
        }
    }
    if (node["sceneType"]) sceneProject->sceneType = decodeEnum(node, "sceneType", StreamSchema::sceneType());
    if (node["mainCamera"]) sceneProject->mainCamera = YamlNumbers::as<Entity>(node["mainCamera"]);

    sceneProject->legacyMaxValues = node["maxValues"] ? YAML::Clone(node["maxValues"]) : YAML::Node();

    sceneProject->shaderKeys.clear();
    for (ShaderKey key : Stream::decodeShaderKeys(node["shaderKeys"])) {
        sceneProject->shaderKeys.insert(key);
    }

    sceneProject->childScenes.clear();
    if (node["childScenes"]) {
        for (const auto& childSceneNode : node["childScenes"]) {
            ChildSceneRef childSceneRef;
            if (!childSceneNode.IsMap() || !childSceneNode["id"]) {
                continue;
            }

            childSceneRef.id = YamlNumbers::as<uint32_t>(childSceneNode["id"]);
            if (childSceneNode["startActive"]) {
                childSceneRef.startActive = YamlNumbers::as<bool>(childSceneNode["startActive"]);
            }
            sceneProject->childScenes.push_back(childSceneRef);
        }
    }

    decodeSceneCppScripts(sceneProject, node);

    sceneProject->prefabs.clear();
    if (node["prefabs"]) {
        for (const auto& pathNode : node["prefabs"]) {
            PrefabSceneInfo info;
            if (pathNode.IsMap()) {
                if (pathNode["path"]) info.prefabPath = pathNode["path"].as<std::string>();
                if (pathNode["guid"]) info.guid = pathNode["guid"].as<std::string>();
            } else {
                info.prefabPath = pathNode.as<std::string>();
            }
            info.functionName = Factory::prefabToFunctionName(info.prefabPath);
            sceneProject->prefabs.push_back(std::move(info));
        }
    }

    sceneProject->imports.clear();
    if (node["imports"] && node["imports"].IsMap()) {
        for (const auto& entry : node["imports"]) {
            sceneProject->imports[entry.first.as<std::string>()] = entry.second.as<std::string>();
        }
    }
}

namespace {

// Only the keys that hold entity nodes: a component carries entity ids of its own, a model's
// bones for one, and those are not ids this scene hands out. Every id goes to `ids`.
void collectSerializedEntities(const YAML::Node& entityNode, std::unordered_set<Entity>& ids);

// The ids a prefab instance block holds: its member ids and added GameObjects, at every
// nested level
void collectInstanceBlockEntities(const YAML::Node& block, std::unordered_set<Entity>& ids) {
    if (!block || !block.IsMap()) {
        return;
    }
    if (block["memberIds"] && block["memberIds"].IsSequence()) {
        for (const auto& pair : block["memberIds"]) {
            if (pair.IsSequence() && pair.size() == 2) {
                ids.insert(editor::YamlNumbers::as<Entity>(pair[1]));
            }
        }
    }
    if (block["addedGameObjects"] && block["addedGameObjects"].IsSequence()) {
        for (const auto& added : block["addedGameObjects"]) {
            collectSerializedEntities(added, ids);
        }
    }
    if (block["nested"] && block["nested"].IsSequence()) {
        for (const auto& nested : block["nested"]) {
            collectInstanceBlockEntities(nested, ids);
        }
    }
}

void collectSerializedEntities(const YAML::Node& entityNode, std::unordered_set<Entity>& ids) {
    if (!entityNode || !entityNode.IsMap())
        return;

    if (entityNode["entity"]) {
        ids.insert(editor::YamlNumbers::as<Entity>(entityNode["entity"]));
    }

    if (entityNode["children"] && entityNode["children"].IsSequence()) {
        for (const auto& child : entityNode["children"]) {
            collectSerializedEntities(child, ids);
        }
    }

    // the scene entities an instance keeps under its prefab root, stored nowhere else, and
    // the ids its members had
    collectInstanceBlockEntities(entityNode["prefabInstance"], ids);
    if (entityNode["prefabLocalEntities"] && entityNode["prefabLocalEntities"].IsSequence()) {
        for (const auto& local : entityNode["prefabLocalEntities"]) {
            collectSerializedEntities(local, ids);
        }
    }
}

}

void editor::Stream::decodeSceneProjectEntities(Project* project, SceneProject* sceneProject, const YAML::Node& node, Entity reservedBelow){
    sceneProject->entities.clear();
    sceneProject->selectedEntities.clear();
    sceneProject->missingPrefabs.clear();
    sceneProject->prefabRefsChanged = false;

    auto entitiesNode = node["entities"];

    // a prefab instance is rebuilt as the root holding it is decoded, so its members draw from
    // the allocator while entities further down the file are still waiting to be recreated:
    // every id the file holds is reserved, and new ones start above them
    std::unordered_set<Entity> serialized;
    for (const auto& entityNode : entitiesNode){
        collectSerializedEntities(entityNode, serialized);
    }
    Entity lastEntity = NULL_ENTITY;
    for (Entity entity : serialized) {
        lastEntity = std::max(lastEntity, entity);
    }
    if (reservedBelow > EntityManager::firstUserEntity()) {
        // bounded: a huge counter only keeps new ids above it
        if (reservedBelow - EntityManager::firstUserEntity() <= 1000000) {
            for (Entity id = EntityManager::firstUserEntity(); id < reservedBelow; id++) {
                serialized.insert(id);
            }
        }
        lastEntity = std::max(lastEntity, reservedBelow - 1);
    }
    sceneProject->scene->setLastEntity(lastEntity);
    sceneProject->reservedEntityIds = std::move(serialized);

    // the file holds no world values: computed once every GameObject (and Prefab instance) is in
    WorldTransformScope worldTransforms(sceneProject->scene);
    try {
        for (const auto& entityNode : entitiesNode){
            decodeEntity(entityNode, sceneProject->scene, &sceneProject->entities, project, sceneProject);
        }
    } catch (...) {
        sceneProject->reservedEntityIds.clear();
        throw;
    }
    sceneProject->reservedEntityIds.clear();

    CameraTextureLink::resolve(sceneProject->scene);
}

YAML::Node editor::Stream::encodeEditorCamera(Camera* camera, float zoom, float walkSpeedOffset) {
    if (!camera) return YAML::Node();
    Entity camEntity = camera->getEntity();
    Scene* scene = camera->getScene();
    CameraComponent& camComp = scene->getComponent<CameraComponent>(camEntity);
    Transform& camTransform = scene->getComponent<Transform>(camEntity);
    YAML::Node camNode;
    camNode["type"] = cameraTypeToString(camComp.type);
    encodeFiniteVector3(camNode, "target", camComp.target);
    encodeFiniteVector3(camNode, "up", camComp.up);
    encodePositiveFinite(camNode, "yfov", camComp.yfov);
    encodePositiveFinite(camNode, "aspect", camComp.aspect);
    encodeFinite(camNode, "nearClip", camComp.nearClip);
    encodeFinite(camNode, "farClip", camComp.farClip);
    encodeFiniteVector3(camNode, "position", camTransform.position);
    encodePositiveFinite(camNode, "zoom", zoom);
    if (walkSpeedOffset != 0.0f) {
        encodeFinite(camNode, "walkSpeed", walkSpeedOffset);
    }
    return camNode;
}

void editor::Stream::decodeEditorCamera(Camera* camera, const YAML::Node& node, float& zoom, float& walkSpeedOffset) {
    zoom = 0.0f;
    walkSpeedOffset = 0.0f;
    if (!camera || !node) return;
    Entity camEntity = camera->getEntity();
    Scene* scene = camera->getScene();
    CameraComponent& camComp = scene->getComponent<CameraComponent>(camEntity);
    Transform& camTransform = scene->getComponent<Transform>(camEntity);
    if (node["type"]) camComp.type = decodeEnum(node, "type", StreamSchema::cameraType());
    camComp.target = decodeFiniteVector3(node["target"], camComp.target);
    camComp.up = decodeFiniteVector3(node["up"], camComp.up);
    camComp.yfov = decodePositiveFinite(node["yfov"], camComp.yfov);
    camComp.aspect = decodePositiveFinite(node["aspect"], camComp.aspect);
    camComp.nearClip = decodeFinite(node["nearClip"], camComp.nearClip);
    camComp.farClip = decodeFinite(node["farClip"], camComp.farClip);
    camTransform.position = decodeFiniteVector3(node["position"], camTransform.position);
    camComp.needUpdate = true;
    if (node["zoom"]) {
        float decodedZoom = decodeFinite(node["zoom"], 0.0f);
        if (decodedZoom > 0.0f) {
            zoom = decodedZoom;
        }
    }
    walkSpeedOffset = decodeFinite(node["walkSpeed"], 0.0f);
}

// scene keys of the default shaders' property values (Scene > Default Shaders)
static const std::pair<ShaderType, const char*> defaultShaderUniformKeys[] = {
    {ShaderType::MESH, "defaultMeshShaderUniforms"},
    {ShaderType::UI, "defaultUIShaderUniforms"},
    {ShaderType::SKYBOX, "defaultSkyShaderUniforms"},
    {ShaderType::POINTS, "defaultPointsShaderUniforms"},
    {ShaderType::LINES, "defaultLinesShaderUniforms"},
};

YAML::Node editor::Stream::encodeScene(Scene* scene) {
    YAML::Node sceneNode;

    sceneNode["backgroundColor"] = encodeVector4(scene->getBackgroundColor());
    sceneNode["shadowQuality"] = shadowQualityToString(scene->getShadowQuality());
    // written only away from their defaults (scenes saved before them re-save unchanged)
    if (scene->getShadowDistance() != SceneSettings().shadowDistance)
        sceneNode["shadowDistance"] = scene->getShadowDistance();
    if (scene->getShadowFadeFraction() != SceneSettings().shadowFadeFraction)
        sceneNode["shadowFadeFraction"] = scene->getShadowFadeFraction();
    if (scene->isShadowCascadeBlend() != SceneSettings().shadowCascadeBlend)
        sceneNode["shadowCascadeBlend"] = scene->isShadowCascadeBlend();
    sceneNode["meshLodEnabled"] = scene->isMeshLodEnabled();
    sceneNode["meshLodThreshold"] = scene->getMeshLodThreshold();
    // written only away from their defaults (scenes saved before them re-save unchanged)
    if (scene->isMeshLodCrossFade() != SceneSettings().meshLodCrossFade)
        sceneNode["meshLodCrossFade"] = scene->isMeshLodCrossFade();
    if (scene->getMeshLodCrossFadeDuration() != SceneSettings().meshLodCrossFadeDuration)
        sceneNode["meshLodCrossFadeDuration"] = scene->getMeshLodCrossFadeDuration();
    sceneNode["depthPrepassEnabled"] = scene->isDepthPrepassEnabled();
    // written only when not the default (half), so scenes saved before it existed re-save unchanged
    if (scene->getSceneColorScale() != SceneSettings().sceneColorScale)
        sceneNode["sceneColorScale"] = sceneColorScaleToString(scene->getSceneColorScale());
    // planar reflections: written only away from their defaults
    if (scene->getPlanarReflectionResolution() != SceneSettings().planarReflectionResolution)
        sceneNode["planarReflectionResolution"] = StreamSchema::planarReflectionResolution().toString(scene->getPlanarReflectionResolution());
    if (scene->getPlanarReflectionLayers() != SceneSettings().planarReflectionLayers)
        sceneNode["planarReflectionLayers"] = scene->getPlanarReflectionLayers();
    if (scene->getPlanarReflectionShadows() != SceneSettings().planarReflectionShadows)
        sceneNode["planarReflectionShadows"] = scene->getPlanarReflectionShadows();
    if (scene->getPlanarReflectionFarClip() != SceneSettings().planarReflectionFarClip)
        sceneNode["planarReflectionFarClip"] = scene->getPlanarReflectionFarClip();
    if (scene->getPlanarReflectionClipOffset() != SceneSettings().planarReflectionClipOffset)
        sceneNode["planarReflectionClipOffset"] = scene->getPlanarReflectionClipOffset();
    if (scene->getMaxPlanarReflections() != SceneSettings().maxPlanarReflections)
        sceneNode["maxPlanarReflections"] = scene->getMaxPlanarReflections();
    // the Water Wakes' map: written only away from its defaults
    if (scene->getWaterWakeResolution() != SceneSettings().waterWakeResolution)
        sceneNode["waterWakeResolution"] = StreamSchema::waterWakeResolution().toString(scene->getWaterWakeResolution());
    if (scene->getWaterWakeArea() != SceneSettings().waterWakeArea)
        sceneNode["waterWakeArea"] = scene->getWaterWakeArea();
    if (scene->getWaterWakeFade() != SceneSettings().waterWakeFade)
        sceneNode["waterWakeFade"] = scene->getWaterWakeFade();
    sceneNode["lightState"] = lightStateToString(scene->getLightState());
    sceneNode["globalIlluminationIntensity"] = scene->getGlobalIlluminationIntensity();
    sceneNode["globalIlluminationColor"] = encodeVector3(scene->getGlobalIlluminationColor());
    if (scene->getMaxLightsPerObject() != SceneSettings().maxLightsPerObject)
        sceneNode["maxLightsPerObject"] = scene->getMaxLightsPerObject();
    // a scene without the key is Standard (it was drawn so before Lit Quality existed); new
    // scenes are High, which is written
    if (scene->getLitQuality() != LitQuality::STANDARD)
        sceneNode["litQuality"] = StreamSchema::litQuality().toString(scene->getLitQuality());
    if (scene->getPhysicalLightEV100() != SceneSettings().physicalLightEV100)
        sceneNode["physicalLightEV100"] = YamlNumbers::text(scene->getPhysicalLightEV100());
    // capsule shadows (S57 R2): written only away from their defaults
    if (scene->isCapsuleShadowsEnabled() != SceneSettings().capsuleShadowsEnabled)
        sceneNode["capsuleShadowsEnabled"] = scene->isCapsuleShadowsEnabled();
    if (scene->getCapsuleShadowIntensity() != SceneSettings().capsuleShadowIntensity)
        sceneNode["capsuleShadowIntensity"] = YamlNumbers::text(scene->getCapsuleShadowIntensity());
    if (scene->getCapsuleShadowSoftness() != SceneSettings().capsuleShadowSoftness)
        sceneNode["capsuleShadowSoftness"] = YamlNumbers::text(scene->getCapsuleShadowSoftness());
    sceneNode["ambientLight2DIntensity"] = scene->getAmbientLight2DIntensity();
    sceneNode["ambientLight2DColor"] = encodeVector3(scene->getAmbientLight2DColor());
    sceneNode["shadow2DQuality"] = shadowQualityToString(scene->getShadow2DQuality());
    sceneNode["ssaoEnabled"] = scene->isSSAOEnabled();
    sceneNode["ssaoRadius"] = scene->getSSAORadius();
    sceneNode["ssaoIntensity"] = scene->getSSAOIntensity();
    sceneNode["ssaoBias"] = scene->getSSAOBias();
    sceneNode["ssrEnabled"] = scene->isSSREnabled();
    sceneNode["ssrMaxDistance"] = scene->getSSRMaxDistance();
    sceneNode["ssrThickness"] = scene->getSSRThickness();
    sceneNode["ssrMaxSteps"] = scene->getSSRMaxSteps();
    sceneNode["ssrIntensity"] = scene->getSSRIntensity();
    sceneNode["ssrBlur"] = scene->getSSRBlur();
    sceneNode["gravity2D"] = encodeVector2(scene->getGravity2D());
    sceneNode["gravity3D"] = encodeVector3(scene->getGravity3D());
    if (scene->isPhysicsInterpolation() != SceneSettings().physicsInterpolation)
        sceneNode["physicsInterpolation"] = scene->isPhysicsInterpolation();
    if (scene->getPhysics2DSubSteps() != SceneSettings().physics2DSubSteps)
        sceneNode["physics2DSubSteps"] = scene->getPhysics2DSubSteps();
    sceneNode["fixedResolutionEnabled"] = scene->isFixedResolutionEnabled();
    sceneNode["fixedResolutionWidth"] = scene->getFixedResolutionWidth();
    sceneNode["fixedResolutionHeight"] = scene->getFixedResolutionHeight();
    sceneNode["fixedResolutionFilter"] = textureFilterToString(scene->getFixedResolutionFilter());
    sceneNode["enableUIEvents"] = uiEventStateToString(scene->getEnableUIEvents());
    // absent = the project's UI Scale (scenes from before the Canvas Scaler stay unchanged)
    if (!scene->isCanvasScalerUseProjectDefault() || scene->getCanvasScaler() != CanvasScalerSettings()) {
        YAML::Node scaler = encodeCanvasScaler(scene->getCanvasScaler());
        scaler["useProjectDefault"] = scene->isCanvasScalerUseProjectDefault();
        sceneNode["canvasScaler"] = scaler;
    }

    if (!scene->getDefaultMeshShader().empty())
        encodeShaderRef(sceneNode, "defaultMeshShader", scene->getDefaultShaderRef(ShaderType::MESH));
    if (!scene->getDefaultUIShader().empty())
        encodeShaderRef(sceneNode, "defaultUIShader", scene->getDefaultShaderRef(ShaderType::UI));
    if (!scene->getDefaultSkyShader().empty())
        encodeShaderRef(sceneNode, "defaultSkyShader", scene->getDefaultShaderRef(ShaderType::SKYBOX));
    if (!scene->getDefaultPointsShader().empty())
        encodeShaderRef(sceneNode, "defaultPointsShader", scene->getDefaultShaderRef(ShaderType::POINTS));
    if (!scene->getDefaultLinesShader().empty())
        encodeShaderRef(sceneNode, "defaultLinesShader", scene->getDefaultShaderRef(ShaderType::LINES));
    // the default shaders' property values, written only when set (older scenes re-save unchanged)
    for (const auto& [type, key] : defaultShaderUniformKeys) {
        const ShaderUniformValues& values = scene->getDefaultShaderUniforms(type);
        if (!values.empty())
            sceneNode[key] = encodeShaderUniforms(values);
    }

    for (const PostProcessPass& pass : scene->getPostProcessPasses()) {
        YAML::Node passNode;
        encodeShaderRef(passNode, "shader", pass.shader);
        passNode["enabled"] = pass.enabled;
        if (!pass.uniforms.empty())
            passNode["uniforms"] = encodeShaderUniforms(pass.uniforms);
        // (phase 4) written only when set, so a chain saved before them re-saves unchanged
        if (!pass.textures.empty()) {
            YAML::Node textures = encodeShaderTextures(pass.textures, false);
            if (textures.size() > 0) passNode["textures"] = textures;
        }
        if (pass.scale != 1.0f)
            passNode["scale"] = pass.scale;
        if (!pass.shaderGlobalBindings.empty())
            passNode["shaderGlobalBindings"] = encodeShaderGlobalBindings(pass.shaderGlobalBindings);
        sceneNode["postProcess"].push_back(passNode);
    }

    // the engine's post-processing: only the fields away from their defaults, and no key at all
    // while everything is default, so scenes saved before it existed re-save unchanged
    const YAML::Node postProcessingNode = encodePostProcessing(scene->getPostProcessing());
    if (postProcessingNode.size() > 0)
        sceneNode["postProcessing"] = postProcessingNode;

    return sceneNode;
}

// The post-processing block by its reflection table (editor/reflect/PostProcessingReflect.cpp):
// the fields away from their defaults, an empty map when none is
YAML::Node editor::Stream::encodePostProcessing(const PostProcessingSettings& settings) {
    YAML::Node node = reflect::encode(reflect::postProcessingStruct(), &settings, reflect::EncodeContext{});
    if (!node.IsMap()) node = YAML::Node(YAML::NodeType::Map);
    return node;
}

PostProcessingSettings editor::Stream::decodePostProcessing(const YAML::Node& node) {
    PostProcessingSettings settings;
    if (!node || !node.IsMap()) return settings;
    reflect::DecodeContext ctx;
    ctx.component = "postProcessing";
    reflect::decode(reflect::postProcessingStruct(), node, &settings, ctx);
    settings.clamp();
    return settings;
}

Scene* editor::Stream::decodeScene(Scene* scene, const YAML::Node& node) {
    if (!scene){
        scene = new Scene();
    }

    if (node["backgroundColor"]) {
        scene->setBackgroundColor(decodeVector4(node["backgroundColor"]));
    }

    if (node["shadowQuality"]) {
        scene->setShadowQuality(decodeEnum(node, "shadowQuality", StreamSchema::shadowQuality()));
    }
    scene->setShadowDistance(node["shadowDistance"] ? YamlNumbers::as<float>(node["shadowDistance"]) : SceneSettings().shadowDistance);
    scene->setShadowFadeFraction(node["shadowFadeFraction"] ? YamlNumbers::as<float>(node["shadowFadeFraction"]) : SceneSettings().shadowFadeFraction);
    scene->setShadowCascadeBlend(node["shadowCascadeBlend"] ? YamlNumbers::as<bool>(node["shadowCascadeBlend"]) : SceneSettings().shadowCascadeBlend);
    // every scene saved since mesh LOD existed writes it; one saved before has LOD off, as it was
    // then (the default is on since spec S54 R1, R19)
    scene->setMeshLodEnabled(node["meshLodEnabled"] ? YamlNumbers::as<bool>(node["meshLodEnabled"]) : false);
    if (node["meshLodThreshold"]) {
        scene->setMeshLodThreshold(decodeFinite(node["meshLodThreshold"], scene->getMeshLodThreshold()));
    }
    scene->setMeshLodCrossFade(node["meshLodCrossFade"] ? YamlNumbers::as<bool>(node["meshLodCrossFade"]) : SceneSettings().meshLodCrossFade);
    scene->setMeshLodCrossFadeDuration(decodeFinite(node["meshLodCrossFadeDuration"], SceneSettings().meshLodCrossFadeDuration));
    if (node["depthPrepassEnabled"]) {
        scene->setDepthPrepassEnabled(YamlNumbers::as<bool>(node["depthPrepassEnabled"]));
    }
    // absent = the default (half): it is only written when it differs
    scene->setSceneColorScale(node["sceneColorScale"] ? decodeEnum(node, "sceneColorScale", StreamSchema::sceneColorScale()) : SceneSettings().sceneColorScale);
    // planar reflections: absent = the defaults
    const SceneSettings planarDefaults;
    scene->setPlanarReflectionResolution(node["planarReflectionResolution"]
        ? decodeEnum(node, "planarReflectionResolution", StreamSchema::planarReflectionResolution()) : planarDefaults.planarReflectionResolution);
    scene->setPlanarReflectionLayers(node["planarReflectionLayers"] ? YamlNumbers::as<uint32_t>(node["planarReflectionLayers"]) : planarDefaults.planarReflectionLayers);
    scene->setPlanarReflectionShadows(node["planarReflectionShadows"] ? YamlNumbers::as<bool>(node["planarReflectionShadows"]) : planarDefaults.planarReflectionShadows);
    scene->setPlanarReflectionFarClip(node["planarReflectionFarClip"] ? decodeFinite(node["planarReflectionFarClip"], planarDefaults.planarReflectionFarClip)
                                                                      : planarDefaults.planarReflectionFarClip);
    scene->setPlanarReflectionClipOffset(node["planarReflectionClipOffset"] ? decodeFinite(node["planarReflectionClipOffset"], planarDefaults.planarReflectionClipOffset)
                                                                            : planarDefaults.planarReflectionClipOffset);
    scene->setMaxPlanarReflections(node["maxPlanarReflections"] ? YamlNumbers::as<int>(node["maxPlanarReflections"]) : planarDefaults.maxPlanarReflections);
    // the Water Wakes' map: absent = the defaults
    scene->setWaterWakeResolution(node["waterWakeResolution"]
        ? decodeEnum(node, "waterWakeResolution", StreamSchema::waterWakeResolution()) : planarDefaults.waterWakeResolution);
    scene->setWaterWakeArea(node["waterWakeArea"] ? decodeFinite(node["waterWakeArea"], planarDefaults.waterWakeArea) : planarDefaults.waterWakeArea);
    scene->setWaterWakeFade(node["waterWakeFade"] ? decodeFinite(node["waterWakeFade"], planarDefaults.waterWakeFade) : planarDefaults.waterWakeFade);

    if (node["lightState"]) {
        scene->setLightState(decodeEnum(node, "lightState", StreamSchema::lightState()));
    }

    if (node["globalIlluminationIntensity"] && node["globalIlluminationColor"]) {
        scene->setGlobalIllumination(
            YamlNumbers::as<float>(node["globalIlluminationIntensity"]),
            decodeVector3(node["globalIlluminationColor"])
        );
    } else if (node["globalIlluminationIntensity"]) {
        scene->setGlobalIllumination(YamlNumbers::as<float>(node["globalIlluminationIntensity"]));
    } else if (node["globalIlluminationColor"]) {
        scene->setGlobalIllumination(decodeVector3(node["globalIlluminationColor"]));
    }

    // absent key restores the engine default so decoding into a reused scene (play restore) is exact
    scene->setMaxLightsPerObject(node["maxLightsPerObject"] ? YamlNumbers::as<int>(node["maxLightsPerObject"]) : SceneSettings().maxLightsPerObject);
    scene->setLitQuality(node["litQuality"] ? decodeEnum(node, "litQuality", StreamSchema::litQuality()) : LitQuality::STANDARD);
    scene->setPhysicalLightEV100(decodeFinite(node["physicalLightEV100"], SceneSettings().physicalLightEV100));
    scene->setCapsuleShadowsEnabled(node["capsuleShadowsEnabled"] ? YamlNumbers::as<bool>(node["capsuleShadowsEnabled"]) : SceneSettings().capsuleShadowsEnabled);
    scene->setCapsuleShadowIntensity(decodeFinite(node["capsuleShadowIntensity"], SceneSettings().capsuleShadowIntensity));
    scene->setCapsuleShadowSoftness(decodeFinite(node["capsuleShadowSoftness"], SceneSettings().capsuleShadowSoftness));

    if (node["ambientLight2DIntensity"] && node["ambientLight2DColor"]) {
        scene->setAmbientLight2D(
            YamlNumbers::as<float>(node["ambientLight2DIntensity"]),
            decodeVector3(node["ambientLight2DColor"])
        );
    } else if (node["ambientLight2DIntensity"]) {
        scene->setAmbientLight2D(YamlNumbers::as<float>(node["ambientLight2DIntensity"]));
    } else if (node["ambientLight2DColor"]) {
        scene->setAmbientLight2D(decodeVector3(node["ambientLight2DColor"]));
    }

    if (node["shadow2DQuality"]) {
        scene->setShadow2DQuality(decodeEnum(node, "shadow2DQuality", StreamSchema::shadowQuality()));
    }

    if (node["ssaoEnabled"]) {
        scene->setSSAOEnabled(YamlNumbers::as<bool>(node["ssaoEnabled"]));
    }
    if (node["ssaoRadius"]) {
        scene->setSSAORadius(YamlNumbers::as<float>(node["ssaoRadius"]));
    }
    if (node["ssaoIntensity"]) {
        scene->setSSAOIntensity(YamlNumbers::as<float>(node["ssaoIntensity"]));
    }
    if (node["ssaoBias"]) {
        scene->setSSAOBias(YamlNumbers::as<float>(node["ssaoBias"]));
    }
    if (node["ssrEnabled"]) {
        scene->setSSREnabled(YamlNumbers::as<bool>(node["ssrEnabled"]));
    }
    if (node["ssrMaxDistance"]) {
        scene->setSSRMaxDistance(YamlNumbers::as<float>(node["ssrMaxDistance"]));
    }
    if (node["ssrThickness"]) {
        scene->setSSRThickness(YamlNumbers::as<float>(node["ssrThickness"]));
    }
    if (node["ssrMaxSteps"]) {
        scene->setSSRMaxSteps(YamlNumbers::as<int>(node["ssrMaxSteps"]));
    }
    if (node["ssrIntensity"]) {
        scene->setSSRIntensity(YamlNumbers::as<float>(node["ssrIntensity"]));
    }
    if (node["ssrBlur"]) {
        scene->setSSRBlur(YamlNumbers::as<float>(node["ssrBlur"]));
    }

    // absent key restores the engine default so decoding into a reused scene (play restore) is exact
    scene->setGravity2D(node["gravity2D"] ? decodeVector2(node["gravity2D"]) : Vector2(0.0f, -9.81f));
    scene->setGravity3D(node["gravity3D"] ? decodeVector3(node["gravity3D"]) : Vector3(0.0f, -9.81f, 0.0f));
    scene->setPhysicsInterpolation(node["physicsInterpolation"] ? YamlNumbers::as<bool>(node["physicsInterpolation"]) : SceneSettings().physicsInterpolation);
    scene->setPhysics2DSubSteps(node["physics2DSubSteps"] ? YamlNumbers::as<int>(node["physics2DSubSteps"]) : SceneSettings().physics2DSubSteps);

    // absent restores the defaults, so decoding into a reused scene (play restore) is exact
    scene->setCanvasScaler(decodeCanvasScaler(node["canvasScaler"]));
    scene->setCanvasScalerUseProjectDefault(node["canvasScaler"] && node["canvasScaler"]["useProjectDefault"]
        ? YamlNumbers::as<bool>(node["canvasScaler"]["useProjectDefault"]) : true);
    if (node["fixedResolutionEnabled"]) {
        scene->setFixedResolutionEnabled(YamlNumbers::as<bool>(node["fixedResolutionEnabled"]));
    }
    if (node["fixedResolutionWidth"]) {
        scene->setFixedResolutionWidth(YamlNumbers::as<unsigned int>(node["fixedResolutionWidth"]));
    }
    if (node["fixedResolutionHeight"]) {
        scene->setFixedResolutionHeight(YamlNumbers::as<unsigned int>(node["fixedResolutionHeight"]));
    }
    if (node["fixedResolutionFilter"]) {
        scene->setFixedResolutionFilter(decodeEnum(node, "fixedResolutionFilter", StreamSchema::textureFilter()));
    }

    if (node["enableUIEvents"]) {
        scene->setEnableUIEvents(decodeEnum(node, "enableUIEvents", StreamSchema::uiEventState()));
    }

    // absent key clears the default so decoding into a reused scene (play restore) is exact
    scene->setDefaultShaderRef(ShaderType::MESH, decodeShaderRef(node, "defaultMeshShader"));
    scene->setDefaultShaderRef(ShaderType::UI, decodeShaderRef(node, "defaultUIShader"));
    scene->setDefaultShaderRef(ShaderType::SKYBOX, decodeShaderRef(node, "defaultSkyShader"));
    scene->setDefaultShaderRef(ShaderType::POINTS, decodeShaderRef(node, "defaultPointsShader"));
    scene->setDefaultShaderRef(ShaderType::LINES, decodeShaderRef(node, "defaultLinesShader"));
    for (const auto& [type, key] : defaultShaderUniformKeys) {
        scene->setDefaultShaderUniforms(type, decodeShaderUniforms(node[key]));
    }

    // absent key clears the chain so decoding into a reused scene (play restore) is exact
    std::vector<PostProcessPass> postProcess;
    if (node["postProcess"]) {
        for (const auto& passNode : node["postProcess"]) {
            PostProcessPass pass;
            pass.shader = decodeShaderRef(passNode, "shader");
            pass.enabled = passNode["enabled"] ? YamlNumbers::as<bool>(passNode["enabled"]) : true;
            pass.uniforms = decodeShaderUniforms(passNode["uniforms"]);
            pass.textures = decodeShaderTextures(passNode["textures"]);
            pass.scale = decodeFinite(passNode["scale"], 1.0f);
            pass.shaderGlobalBindings = decodeShaderGlobalBindings(passNode["shaderGlobalBindings"]);
            postProcess.push_back(pass);
        }
    }
    scene->setPostProcessPasses(postProcess);

    // absent key (or field) restores the defaults so decoding into a reused scene (play restore) is exact
    scene->setPostProcessing(decodePostProcessing(node["postProcessing"]));

    return scene;
}

YAML::Node editor::Stream::encodeEntitySelection(const std::vector<Entity>& entities, const EntityRegistry* registry, const Project* project, const SceneProject* sceneProject) {
    if (entities.empty()) {
        return YAML::Node();
    }

    if (entities.size() == 1) {
        return encodeEntity(entities[0], registry, project, sceneProject);
    }

    YAML::Node prefabNode;
    prefabNode["type"] = "Prefab";
    YAML::Node membersNode(YAML::NodeType::Sequence);
    for (Entity entity : entities) {
        membersNode.push_back(encodeEntity(entity, registry, project, sceneProject));
    }
    prefabNode["members"] = membersNode;

    return prefabNode;
}

std::vector<Entity> editor::Stream::decodeEntitySelection(const YAML::Node& entityNode, EntityRegistry* registry, std::vector<Entity>* entities, Project* project, SceneProject* sceneProject, Entity parent, bool createNewIfExists, std::unordered_map<Entity, Entity>* entityRemap) {
    if (!entityNode || !entityNode.IsMap()) {
        return {};
    }
    // their world values, once the selection is in (WorldTransforms.h)
    WorldTransformScope worldTransforms(registry);

    if (entityNode["members"] && entityNode["members"].IsSequence()) {
        std::vector<Entity> allEntities;
        for (const auto& memberNode : entityNode["members"]) {
            std::vector<Entity> memberEntities = decodeEntitySelection(memberNode, registry, entities, project, sceneProject, parent, createNewIfExists, entityRemap);
            allEntities.insert(allEntities.end(), memberEntities.begin(), memberEntities.end());
        }
        return allEntities;
    }

    return decodeEntity(entityNode, registry, entities, project, sceneProject, parent, createNewIfExists, false, entityRemap);
}

YAML::Node editor::Stream::encodeEntity(const Entity entity, const EntityRegistry* registry,
        const Project* project, const SceneProject* sceneProject) {
    return encodeEntity(entity, registry, project, sceneProject, false);
}

YAML::Node editor::Stream::encodeEntity(const Entity entity, const EntityRegistry* registry,
        const Project* project, const SceneProject* sceneProject, bool ignoreContainingPrefab) {
    std::map<Entity, YAML::Node> entityNodes;

    auto importedPrefabPath = [&](Entity candidate) {
        fs::path path;
        if (!project || !sceneProject) {
            return path;
        }
        Signature signature = registry->getSignature(candidate);
        if (!signature.test(registry->getComponentId<PrefabComponent>())) {
            return path;
        }
        const PrefabComponent& component = registry->getComponent<PrefabComponent>(candidate);
        const Prefab* prefab = project->getPrefab(component.path);
        const Prefab::Instance* instance = prefab ?
            prefab->getInstance(sceneProject->id, candidate) : nullptr;
        if (instance && instance->rootEntity == candidate) {
            path = component.path;
        }
        return path;
    };

    bool hasCurrentEntity = !sceneProject ||
        std::find(sceneProject->entities.begin(), sceneProject->entities.end(), entity) != sceneProject->entities.end();

    if (hasCurrentEntity) {
        fs::path currentPrefabPath = ignoreContainingPrefab ? fs::path{} : importedPrefabPath(entity);
        YAML::Node& currentNode = entityNodes[entity];
        currentNode = ignoreContainingPrefab ? encodeEntityAux(entity, registry) :
            encodeEntityAux(entity, registry, project, sceneProject, currentPrefabPath);
        encodeMissingPrefab(currentNode, entity, project, sceneProject);

        // Imported children are represented by the prefab root and its overrides.
        if (!ignoreContainingPrefab && project && registry->getSignature(entity).test(registry->getComponentId<PrefabComponent>()) &&
            (!sceneProject || !currentPrefabPath.empty())) {
            return entityNodes[entity];
        }

        Signature signature = registry->getSignature(entity);

        if (signature.test(registry->getComponentId<Transform>())) {
            auto transforms = registry->getComponentArray<Transform>();
            size_t firstIndex = transforms->getIndex(entity);
            std::unordered_set<Entity> skippedPrefabBranch;

            for (size_t i = firstIndex + 1; i < transforms->size(); ++i) {
                Entity currentEntity = transforms->getEntity(i);
                const Transform& transform = transforms->getComponentFromIndex(i);

                if (skippedPrefabBranch.count(transform.parent)) {
                    skippedPrefabBranch.insert(currentEntity);
                    continue;
                }

                hasCurrentEntity = !sceneProject ||
                    std::find(sceneProject->entities.begin(), sceneProject->entities.end(), currentEntity) != sceneProject->entities.end();
                if (hasCurrentEntity) {
                    // Past the branch: stop before encoding an entity outside it
                    auto parentIt = entityNodes.find(transform.parent);
                    if (parentIt == entityNodes.end()) {
                        break; // No more childs
                    }
                    YAML::Node& parentNode = parentIt->second;

                    fs::path currentPrefabPath = importedPrefabPath(currentEntity);
                    YAML::Node& currentNode = entityNodes[currentEntity];
                    currentNode = ignoreContainingPrefab && currentPrefabPath.empty() ?
                        encodeEntityAux(currentEntity, registry) :
                        encodeEntityAux(currentEntity, registry, project, sceneProject, currentPrefabPath);
                    encodeMissingPrefab(currentNode, currentEntity, project, sceneProject);

                    if (!parentNode["children"]) {
                        parentNode["children"] = YAML::Node();
                    }
                    parentNode["children"].push_back(currentNode);
                    if (project && registry->getSignature(currentEntity).test(registry->getComponentId<PrefabComponent>()) &&
                        (!sceneProject || !currentPrefabPath.empty())) {
                        skippedPrefabBranch.insert(currentEntity);
                    }
                }
            }
        }
    }

    return entityNodes[entity];
}

YAML::Node editor::Stream::encodeEntityAux(const Entity entity, const EntityRegistry* registry,
        const Project* project, const SceneProject* sceneProject, const std::filesystem::path& prefabPathOverride) {
    YAML::Node entityNode;

    fs::path prefabPath = prefabPathOverride;
    if (prefabPath.empty() && project && sceneProject) {
        prefabPath = project->findPrefabPathFor(sceneProject->id, entity);
    }

    if (!prefabPath.empty()) {
        const Prefab* prefab = project->getPrefab(prefabPath);
        const Entity rootEntity = prefab->getRootEntity(sceneProject->id, entity);

        if (rootEntity == entity) {
                // Prefab root: encode as normal Entity (PrefabComponent carries the path)
                entityNode["type"] = "Entity";
                entityNode["entity"] = YamlNumbers::integerText(entity);
                SavedEntitiesCheck::written(registry, entity);
                entityNode["name"] = registry->getEntityName(entity);
                encodeEntityState(entityNode, entity, registry);

                Signature signature = registry->getSignature(entity);
                YAML::Node components = encodeComponents(entity, registry, signature);

                if ((components.IsMap() && components.size() > 0)){
                    entityNode["components"] = components;
                }

                // Encode prefab overrides and local entities
                encodePrefabInstance(entityNode, entity, registry, project, sceneProject, prefab);
            }
        // Prefab children are not encoded (they live in the prefab file)
    }else{
        entityNode["type"] = "Entity";

        entityNode["entity"] = YamlNumbers::integerText(entity);
        SavedEntitiesCheck::written(registry, entity);
        entityNode["name"] = registry->getEntityName(entity);
        encodeEntityState(entityNode, entity, registry);

        Signature signature = registry->getSignature(entity);
        YAML::Node components = encodeComponents(entity, registry, signature);

        if ((components.IsMap() && components.size() > 0)){
            entityNode["components"] = components;
        }
    }

    if (entityNode.IsMap()) {
        StreamSave::applyRootKeys(registry, entity, entityNode);
    }

    return entityNode;
}

void editor::Stream::encodeEntityState(YAML::Node& entityNode, const Entity entity, const EntityRegistry* registry) {
    // only what differs from a new GameObject is written
    if (!registry->isEntityActive(entity)) {
        entityNode["active"] = false;
    }
    std::string tag = registry->getEntityTag(entity);
    if (tag != DEFAULT_ENTITY_TAG) {
        entityNode["tag"] = tag;
    }
    int layer = registry->getEntityLayer(entity);
    if (layer != 0) {
        entityNode["layer"] = layer;
    }
}

void editor::Stream::decodeEntityState(Entity entity, EntityRegistry* registry, const YAML::Node& entityNode) {
    // missing keys reset to defaults, decoding over a live entity restores it fully
    registry->setEntityActive(entity, entityNode["active"] ? YamlNumbers::as<bool>(entityNode["active"]) : true);
    registry->setEntityTag(entity, entityNode["tag"] ? entityNode["tag"].as<std::string>() : std::string(DEFAULT_ENTITY_TAG));
    registry->setEntityLayer(entity, entityNode["layer"] ? YamlNumbers::as<int>(entityNode["layer"]) : 0);
}

YAML::Node editor::Stream::encodeScriptProperty(const ScriptProperty& prop) {
    YAML::Node node;
    node["name"] = prop.name;
    node["displayName"] = prop.displayName;
    node["type"] = scriptPropertyTypeToString(prop.type);

    // Store ptrTypeName if it's a pointer type
    if (prop.type == ScriptPropertyType::EntityReference && !prop.ptrTypeName.empty()) {
        node["ptrTypeName"] = prop.ptrTypeName;
    }

    // getValue falls back to the type default instead of throwing on a stale value
    switch (prop.type) {
        case ScriptPropertyType::Bool:
            node["value"] = prop.getValue<bool>();
            node["defaultValue"] = prop.getDefaultValue<bool>();
            break;
        case ScriptPropertyType::Int:
            node["value"] = prop.getValue<int>();
            node["defaultValue"] = prop.getDefaultValue<int>();
            break;
        case ScriptPropertyType::Float:
            node["value"] = prop.getValue<float>();
            node["defaultValue"] = prop.getDefaultValue<float>();
            break;
        case ScriptPropertyType::String:
            node["value"] = prop.getValue<std::string>();
            node["defaultValue"] = prop.getDefaultValue<std::string>();
            break;
        case ScriptPropertyType::Vector2:
            node["value"] = encodeVector2(prop.getValue<Vector2>());
            node["defaultValue"] = encodeVector2(prop.getDefaultValue<Vector2>());
            break;
        case ScriptPropertyType::Vector3:
        case ScriptPropertyType::Color3:
            node["value"] = encodeVector3(prop.getValue<Vector3>());
            node["defaultValue"] = encodeVector3(prop.getDefaultValue<Vector3>());
            break;
        case ScriptPropertyType::Vector4:
        case ScriptPropertyType::Color4:
            node["value"] = encodeVector4(prop.getValue<Vector4>());
            node["defaultValue"] = encodeVector4(prop.getDefaultValue<Vector4>());
            break;
        case ScriptPropertyType::EntityReference: {
            const EntityReference entRef = prop.getValue<EntityReference>();
            node["value"] = entRef.entity;
            node["defaultValue"] = prop.getDefaultValue<EntityReference>().entity;
            if (entRef.sceneId != 0) {
                node["sceneId"] = entRef.sceneId;
            }
            break;
        }
        case ScriptPropertyType::Prefab: {
            // by GUID, the path a hint (PREFABS.md): {guid, path}, or empty for None
            auto encodePrefab = [](const PrefabAsset& prefab) {
                YAML::Node value(YAML::NodeType::Map);
                if (!prefab.getGuid().empty()) value["guid"] = prefab.getGuid();
                if (!prefab.getPath().empty()) value["path"] = prefab.getPath();
                return value;
            };
            node["value"] = encodePrefab(prop.getValue<PrefabAsset>());
            node["defaultValue"] = encodePrefab(prop.getDefaultValue<PrefabAsset>());
            break;
        }
        case ScriptPropertyType::Asset: {
            // S04: {guid, path} like a prefab, empty for None; assetType filters what fits
            auto encodeAsset = [](const AssetRef& ref) {
                YAML::Node value(YAML::NodeType::Map);
                if (!ref.guid.isNull()) value["guid"] = ref.guid.str();
                if (!ref.path.empty()) value["path"] = ref.path;
                return value;
            };
            if (!prop.assetType.empty()) node["assetType"] = prop.assetType;
            node["value"] = encodeAsset(prop.getValue<AssetRef>());
            node["defaultValue"] = encodeAsset(prop.getDefaultValue<AssetRef>());
            break;
        }
    }

    return node;
}

ScriptProperty editor::Stream::decodeScriptProperty(const YAML::Node& node) {
    ScriptProperty prop;

    if (node["name"]) prop.name = node["name"].as<std::string>();
    if (node["displayName"]) prop.displayName = node["displayName"].as<std::string>();
    if (node["type"]) prop.type = decodeEnum(node, "type", StreamSchema::scriptPropertyType());

    // Restore ptrTypeName if it exists
    if (node["ptrTypeName"]) {
        prop.ptrTypeName = node["ptrTypeName"].as<std::string>();
    }
    if (node["assetType"]) {
        prop.assetType = node["assetType"].as<std::string>();
    }

    if (node["value"]) {
        switch (prop.type) {
            case ScriptPropertyType::Bool:
                prop.value = YamlNumbers::as<bool>(node["value"]);
                prop.defaultValue = node["defaultValue"] ? YamlNumbers::as<bool>(node["defaultValue"]) : false;
                break;
            case ScriptPropertyType::Int:
                prop.value = YamlNumbers::as<int>(node["value"]);
                prop.defaultValue = node["defaultValue"] ? YamlNumbers::as<int>(node["defaultValue"]) : 0;
                break;
            case ScriptPropertyType::Float:
                prop.value = YamlNumbers::as<float>(node["value"]);
                prop.defaultValue = node["defaultValue"] ? YamlNumbers::as<float>(node["defaultValue"]) : 0.0f;
                break;
            case ScriptPropertyType::String:
                prop.value = node["value"].as<std::string>();
                prop.defaultValue = node["defaultValue"] ? node["defaultValue"].as<std::string>() : std::string("");
                break;
            case ScriptPropertyType::Vector2:
                prop.value = decodeVector2(node["value"]);
                prop.defaultValue = node["defaultValue"] ? decodeVector2(node["defaultValue"]) : Vector2();
                break;
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Color3:
                prop.value = decodeVector3(node["value"]);
                prop.defaultValue = node["defaultValue"] ? decodeVector3(node["defaultValue"]) : Vector3();
                break;
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color4:
                prop.value = decodeVector4(node["value"]);
                prop.defaultValue = node["defaultValue"] ? decodeVector4(node["defaultValue"]) : Vector4();
                break;
            case ScriptPropertyType::EntityReference: {
                uint32_t sid = node["sceneId"] ? node["sceneId"].as<uint32_t>(0) : 0;
                prop.value = EntityReference{node["value"].as<Entity>(NULL_ENTITY), sid};
                prop.defaultValue = EntityReference{node["defaultValue"] ? node["defaultValue"].as<Entity>(NULL_ENTITY) : Entity(NULL_ENTITY), 0};
                break;
            }
            case ScriptPropertyType::Prefab: {
                auto decodePrefab = [](const YAML::Node& value) {
                    if (!value || !value.IsMap()) {
                        return PrefabAsset();
                    }
                    return PrefabAsset(value["guid"] ? value["guid"].as<std::string>() : std::string(),
                                       value["path"] ? value["path"].as<std::string>() : std::string());
                };
                prop.value = decodePrefab(node["value"]);
                prop.defaultValue = decodePrefab(node["defaultValue"]);
                break;
            }
            case ScriptPropertyType::Asset: {
                auto decodeAsset = [](const YAML::Node& value) {
                    if (!value || !value.IsMap()) {
                        return AssetRef();
                    }
                    return AssetRef(value["path"] ? value["path"].as<std::string>() : std::string(),
                                    value["guid"] ? Guid::parse(value["guid"].as<std::string>()) : Guid());
                };
                prop.value = decodeAsset(node["value"]);
                prop.defaultValue = decodeAsset(node["defaultValue"]);
                break;
            }
        }
    } else {
        // Initialize with default values if no value is provided
        switch (prop.type) {
            case ScriptPropertyType::Bool: prop.value = false; prop.defaultValue = false; break;
            case ScriptPropertyType::Int: prop.value = 0; prop.defaultValue = 0; break;
            case ScriptPropertyType::Float: prop.value = 0.0f; prop.defaultValue = 0.0f; break;
            case ScriptPropertyType::String: prop.value = std::string(""); prop.defaultValue = std::string(""); break;
            case ScriptPropertyType::Vector2: prop.value = Vector2(); prop.defaultValue = Vector2(); break;
            case ScriptPropertyType::Vector3:
            case ScriptPropertyType::Color3: prop.value = Vector3(); prop.defaultValue = Vector3(); break;
            case ScriptPropertyType::Vector4:
            case ScriptPropertyType::Color4: prop.value = Vector4(); prop.defaultValue = Vector4(); break;
            case ScriptPropertyType::EntityReference: prop.value = EntityReference{NULL_ENTITY, 0}; prop.defaultValue = EntityReference{NULL_ENTITY, 0}; break;
            case ScriptPropertyType::Prefab: prop.value = PrefabAsset(); prop.defaultValue = PrefabAsset(); break;
            case ScriptPropertyType::Asset: prop.value = AssetRef(); prop.defaultValue = AssetRef(); break;
        }
    }

    return prop;
}

std::vector<Entity> editor::Stream::decodeEntity(const YAML::Node& entityNode, EntityRegistry* registry, std::vector<Entity>* entities, Project* project, SceneProject* sceneProject, Entity parent, bool createNewIfExists, bool removeMissingComponents, std::unordered_map<Entity, Entity>* entityRemap, bool keepSameMeshes) {
    editor::getEditorHost().reportLoadingProgress();
    // its world values (and its children's), once it is in (WorldTransforms.h)
    WorldTransformScope worldTransforms(registry);

    std::vector<Entity> allEntities;

    if (!entityNode || !entityNode.IsMap() || !entityNode["type"]) {
        return allEntities;
    }

    std::string entityType = entityNode["type"].as<std::string>();

    if (entityType == "Entity") {

        Entity entity = NULL_ENTITY;
        if (entityNode["entity"]){
            Entity serializedEntity = YamlNumbers::as<Entity>(entityNode["entity"]);
            entity = serializedEntity;
            if (!registry->recreateEntity(entity)){
                if (createNewIfExists){
                    entity = registry->createUserEntity();
                }
            }
            if (entityRemap && entity != serializedEntity) {
                (*entityRemap)[serializedEntity] = entity;
            }
        }else{
            entity = registry->createUserEntity();
        }

        allEntities.push_back(entity);

        if (entities){
            entities->push_back(entity);
        }

        std::string name = entityNode["name"] ? entityNode["name"].as<std::string>() : "Entity";
        registry->setEntityName(entity, name);
        decodeEntityState(entity, registry, entityNode);

        // the node is the whole entity: what was preserved for it before (a live entity
        // restored by undo) is replaced by what this node holds
        if (PreservedStore* store = findPreservedStore(registry)) {
            store->reset(entity);
        }
        StreamLoad::EntityScope scope(registry, entity);
        const bool hadTransform = registry->getSignature(entity).test(registry->getComponentId<Transform>());

        if (entityNode["components"]){
            // yaml-cpp only names the key it failed on, so point at the entity holding it
            try {
                decodeComponents(entity, parent, registry, entityNode["components"], keepSameMeshes);
            } catch (const std::exception& e) {
                throw std::runtime_error("entity '" + name + "' (id " + std::to_string(entity) + "): " + e.what());
            }
        }
        StreamLoad::preserveRootKeys(registry, entity, entityNode, RootKind::Entity);

        // Components added to a live entity after it was encoded, like a script calling
        // getRigidbody(), are not removed by decodeComponents
        if (removeMissingComponents){
            YAML::Node savedComponents = entityNode["components"];
            std::vector<Entity> unusedEntities;
            std::vector<Entity>& sceneEntities = entities ? *entities : (sceneProject ? sceneProject->entities : unusedEntities);

            for (ComponentType cpType : Catalog::findComponents(registry, entity)){
                if (savedComponents && savedComponents[Catalog::getComponentName(cpType, true)]) continue;
                ProjectUtils::removeEntityComponent(registry, entity, cpType, sceneEntities);
            }
        }

        decodePrefabInstance(entityNode, entity, registry, entities, project, sceneProject, entityRemap, allEntities);

        // Decode children from actualNode
        if (entityNode["children"]) {
            for (const auto& childNode : entityNode["children"]) {
                std::vector<Entity> childEntities = decodeEntity(childNode, registry, entities, project, sceneProject, entity, createNewIfExists, removeMissingComponents, entityRemap, keepSameMeshes);
                std::copy(childEntities.begin(), childEntities.end(), std::back_inserter(allEntities));
            }
        }

        // Its Transform, new, went to the end of the array, its branch after it: under a parent
        // whose branch ends elsewhere (a prefab instance imported again, a GameObject put back
        // below one), the branch goes to the end of the parent's, where every reader of the
        // array (the moves, the encoders) looks for it. One decoded over a live one keeps its place
        if (!hadTransform && parent != NULL_ENTITY) {
            ProjectUtils::keepInParentBranch(registry, entity);
        }
    } else {
        // the next save leaves it out
        StreamLoad::unknownEntityType(entityNode, registry, parent);
    }

    return allEntities;
}

YAML::Node editor::Stream::encodeMaterial(const Material& material, bool embedTextureData) {
    YAML::Node node;

    // Encode shader part properties
    node["baseColorFactor"] = encodeVector4(material.baseColorFactor);
    node["metallicFactor"] = YamlNumbers::text(material.metallicFactor);
    node["roughnessFactor"] = YamlNumbers::text(material.roughnessFactor);
    node["alphaCutoff"] = YamlNumbers::text(material.alphaCutoff);
    node["foliageTransmission"] = YamlNumbers::text(material.foliageTransmission);
    node["alphaMode"] = materialAlphaModeToString(material.alphaMode);
    node["emissiveFactor"] = encodeVector3(material.emissiveFactor);

    // Encode textures using the helper method. A camera link is one even unbound (a prefab
    // registry, its camera a stripped object or not resolved yet)
    auto present = [](const Texture& texture) {
        return !texture.empty() || hasTexturePath(texture) || CameraTextureLink::isLinkTexture(texture);
    };
    if (present(material.baseColorTexture)) {
        node["baseColorTexture"] = encodeTexture(material.baseColorTexture, embedTextureData);
    }

    if (present(material.emissiveTexture)) {
        node["emissiveTexture"] = encodeTexture(material.emissiveTexture, embedTextureData);
    }

    if (present(material.metallicRoughnessTexture)) {
        node["metallicRoughnessTexture"] = encodeTexture(material.metallicRoughnessTexture, embedTextureData);
    }

    if (present(material.occlusionTexture)) {
        node["occlusionTexture"] = encodeTexture(material.occlusionTexture, embedTextureData);
    }

    if (present(material.normalTexture)) {
        node["normalTexture"] = encodeTexture(material.normalTexture, embedTextureData);
    }

    // Per-texture UV set (glTF texCoord): 0 = a_texcoord1, 1 = a_texcoord2
    node["baseColorTexCoord"] = YamlNumbers::integerText(material.baseColorTexCoord);
    node["metallicRoughnessTexCoord"] = YamlNumbers::integerText(material.metallicRoughnessTexCoord);
    node["occlusionTexCoord"] = YamlNumbers::integerText(material.occlusionTexCoord);
    node["emissiveTexCoord"] = YamlNumbers::integerText(material.emissiveTexCoord);
    node["normalTexCoord"] = YamlNumbers::integerText(material.normalTexCoord);

    // Encode material name
    encodeAssetRef(node, "name", material.name);

    // Shader library: written only away from their defaults, so a material without them is
    // stored exactly as before they existed
    if (!material.shader.empty()) encodeShaderRef(node, "shader", material.shader);
    if (!material.shaderParams.empty()) node["shaderParams"] = encodeShaderUniforms(material.shaderParams);
    if (!material.shaderTextures.empty()) {
        YAML::Node textures = encodeShaderTextures(material.shaderTextures, embedTextureData);
        if (textures.size() > 0) node["shaderTextures"] = textures;
    }
    if (!material.shaderFeatures.empty()) node["shaderFeatures"] = encodeShaderFeatures(material.shaderFeatures);
    if (!material.shaderGlobalBindings.empty()) node["shaderGlobalBindings"] = encodeShaderGlobalBindings(material.shaderGlobalBindings);
    if (material.blendMode != MaterialBlendMode::AUTO) node["blendMode"] = materialBlendModeToString(material.blendMode);
    if (material.cullMode != MaterialCullMode::AUTO) node["cullMode"] = materialCullModeToString(material.cullMode);
    if (material.depthWrite != MaterialDepthWrite::AUTO) node["depthWrite"] = materialDepthWriteToString(material.depthWrite);
    if (material.depthTest != MaterialDepthTest::AUTO) node["depthTest"] = materialDepthTestToString(material.depthTest);
    if (material.depthOffsetFactor != 0.0f) node["depthOffsetFactor"] = YamlNumbers::text(finiteOr(material.depthOffsetFactor));
    if (material.depthOffsetUnits != 0.0f) node["depthOffsetUnits"] = YamlNumbers::text(finiteOr(material.depthOffsetUnits));
    if (material.sortPriority != 0) node["sortPriority"] = YamlNumbers::integerText(material.sortPriority);
    if (material.lighting != MaterialLighting::AUTO) node["lighting"] = materialLightingToString(material.lighting);

    // Stochastic Tiling the same way: each value only away from its default
    static const Material defaults = Material();
    if (material.stochasticTiling) node["stochasticTiling"] = true;
    if (material.stochasticCellSize != defaults.stochasticCellSize) node["stochasticCellSize"] = YamlNumbers::text(finiteOr(material.stochasticCellSize));
    if (material.stochasticBlendSharpness != defaults.stochasticBlendSharpness) node["stochasticBlendSharpness"] = YamlNumbers::text(finiteOr(material.stochasticBlendSharpness));
    if (material.stochasticContrast != defaults.stochasticContrast) node["stochasticContrast"] = YamlNumbers::text(finiteOr(material.stochasticContrast));
    if (material.stochasticRotation != defaults.stochasticRotation) node["stochasticRotation"] = YamlNumbers::text(finiteOr(material.stochasticRotation));

    // Phase 2, the same way: stencil, colour write and the extra pass when set
    encodeStencilAndColorWrite(node, material.stencil, material.colorWrite);
    if (material.extraPass != MaterialPass()) node["extraPass"] = encodeMaterialPass(material.extraPass, embedTextureData);

    // the extended lobes and the emission intensity, the same way (a material without them is
    // stored as it was before they existed)
    if (material.emissiveStrength != 1.0f) node["emissiveStrength"] = YamlNumbers::text(finiteOr(material.emissiveStrength, 1.0f));
    if (material.pbr != MaterialPbrExtensions()) {
        YAML::Node pbr = encodeMaterialPbr(material.pbr, embedTextureData);
        if (pbr.size() > 0) node["pbr"] = pbr;
    }

    return node;
}

YAML::Node editor::Stream::encodeMaterialPbr(const MaterialPbrExtensions& pbr, bool embedTextureData) {
    static const MaterialPbrExtensions defaults;
    MaterialPbrExtensions& values = const_cast<MaterialPbrExtensions&>(pbr);
    MaterialPbrExtensions& defaultValues = const_cast<MaterialPbrExtensions&>(defaults);
    YAML::Node node;
    for (const PbrLobeInfo& lobe : MaterialPbr::getLobes()) {
        YAML::Node lobeNode;
        for (const PbrField& field : MaterialPbr::getFields()) {
            if (field.lobe != lobe.lobe) continue;
            const void* value = field.ref(values);
            const void* def = field.ref(defaultValues);
            switch (field.type) {
                case PbrFieldType::Bool:
                    if (*(const bool*)value != *(const bool*)def) lobeNode[field.name] = *(const bool*)value;
                    break;
                case PbrFieldType::Float:
                    // an infinity (the attenuation distance's default) is only ever the default
                    if (*(const float*)value != *(const float*)def && std::isfinite(*(const float*)value))
                        lobeNode[field.name] = YamlNumbers::text(*(const float*)value);
                    break;
                case PbrFieldType::Vector2:
                    if (*(const Vector2*)value != *(const Vector2*)def) lobeNode[field.name] = encodeVector2(*(const Vector2*)value);
                    break;
                case PbrFieldType::Color:
                    if (*(const Vector3*)value != *(const Vector3*)def) lobeNode[field.name] = encodeVector3(*(const Vector3*)value);
                    break;
                case PbrFieldType::Texture:
                    // a map packed from a model's images (TexturePack) is made again when the
                    // model loads: not written
                    if ((!((const Texture*)value)->empty() || hasTexturePath(*(const Texture*)value)) &&
                            !TexturePack::isPackedId(((const Texture*)value)->getId()))
                        lobeNode[field.name] = encodeTexture(*(const Texture*)value, embedTextureData);
                    break;
                case PbrFieldType::TexCoord:
                    if (*(const int*)value != *(const int*)def) lobeNode[field.name] = YamlNumbers::integerText(*(const int*)value);
                    break;
            }
        }
        if (lobeNode.size() > 0) node[lobe.name] = lobeNode;
    }
    return node;
}

MaterialPbrExtensions editor::Stream::decodeMaterialPbr(const YAML::Node& node) {
    MaterialPbrExtensions pbr;
    if (!node || !node.IsMap()) return pbr;
    for (const PbrLobeInfo& lobe : MaterialPbr::getLobes()) {
        const YAML::Node lobeNode = node[lobe.name];
        if (!lobeNode || !lobeNode.IsMap()) continue;
        for (const PbrField& field : MaterialPbr::getFields()) {
            if (field.lobe != lobe.lobe) continue;
            const YAML::Node value = lobeNode[field.name];
            if (!value) continue;
            void* target = field.ref(pbr);
            switch (field.type) {
                case PbrFieldType::Bool: *(bool*)target = YamlNumbers::as<bool>(value); break;
                case PbrFieldType::Float: *(float*)target = decodeFinite(value, *(float*)target); break;
                case PbrFieldType::Vector2: *(Vector2*)target = decodeVector2(value); break;
                case PbrFieldType::Color: *(Vector3*)target = decodeVector3(value); break;
                case PbrFieldType::Texture: *(Texture*)target = decodeTexture(value); break;
                case PbrFieldType::TexCoord: *(int*)target = YamlNumbers::as<int>(value); break;
            }
        }
    }
    // out of range from a hand edit or a newer writer: into range (the glTF loader warns, a
    // file does not)
    MaterialPbr::clamp(pbr);
    return pbr;
}

Material editor::Stream::decodeMaterial(const YAML::Node& node, const Material* oldMaterial) {
    StreamLoad::checkNestedKeys(node, NestedKind::Material);
    Material material;
    if (oldMaterial) {
        material = *oldMaterial;
    }

    // Scalars are optional, so a partial block only changes the ones it names
    if (node["baseColorFactor"]) material.baseColorFactor = decodeVector4(node["baseColorFactor"]);
    material.metallicFactor = decodeFinite(node["metallicFactor"], material.metallicFactor);
    material.roughnessFactor = decodeFinite(node["roughnessFactor"], material.roughnessFactor);
    material.alphaCutoff = decodeFinite(node["alphaCutoff"], material.alphaCutoff);
    material.foliageTransmission = std::clamp(decodeFinite(node["foliageTransmission"], material.foliageTransmission), 0.0f, 1.0f);
    if (node["alphaMode"]) {
        material.alphaMode = decodeEnum(node, "alphaMode", StreamSchema::materialAlphaMode());
    }
    if (node["emissiveFactor"]) material.emissiveFactor = decodeVector3(node["emissiveFactor"]);

    // Not merged: encodeMaterial omits an empty texture, so an absent key means none
    auto texture = [&node](const char* name) {
        return node[name] ? decodeTexture(node[name]) : Texture();
    };
    material.baseColorTexture = texture("baseColorTexture");
    material.emissiveTexture = texture("emissiveTexture");
    material.metallicRoughnessTexture = texture("metallicRoughnessTexture");
    material.occlusionTexture = texture("occlusionTexture");
    material.normalTexture = texture("normalTexture");

    if (node["baseColorTexCoord"]) material.baseColorTexCoord = YamlNumbers::as<int>(node["baseColorTexCoord"]);
    if (node["metallicRoughnessTexCoord"]) material.metallicRoughnessTexCoord = YamlNumbers::as<int>(node["metallicRoughnessTexCoord"]);
    if (node["occlusionTexCoord"]) material.occlusionTexCoord = YamlNumbers::as<int>(node["occlusionTexCoord"]);
    if (node["emissiveTexCoord"]) material.emissiveTexCoord = YamlNumbers::as<int>(node["emissiveTexCoord"]);
    if (node["normalTexCoord"]) material.normalTexCoord = YamlNumbers::as<int>(node["normalTexCoord"]);

    if (node["name"]) material.name = decodeAssetRef(node, "name");

    // Not merged: encodeMaterial omits these at their defaults, so an absent key is the default
    material.shader = decodeShaderRef(node, "shader");
    material.shaderParams = decodeShaderUniforms(node["shaderParams"]);
    material.shaderTextures = decodeShaderTextures(node["shaderTextures"]);
    material.shaderFeatures = decodeShaderFeatures(node["shaderFeatures"]);
    material.shaderGlobalBindings = decodeShaderGlobalBindings(node["shaderGlobalBindings"]);
    material.blendMode = node["blendMode"] ? decodeEnum(node, "blendMode", StreamSchema::materialBlendMode()) : MaterialBlendMode::AUTO;
    material.cullMode = node["cullMode"] ? decodeEnum(node, "cullMode", StreamSchema::materialCullMode()) : MaterialCullMode::AUTO;
    material.depthWrite = node["depthWrite"] ? decodeEnum(node, "depthWrite", StreamSchema::materialDepthWrite()) : MaterialDepthWrite::AUTO;
    material.depthTest = node["depthTest"] ? decodeEnum(node, "depthTest", StreamSchema::materialDepthTest()) : MaterialDepthTest::AUTO;
    material.depthOffsetFactor = decodeFinite(node["depthOffsetFactor"], 0.0f);
    material.depthOffsetUnits = decodeFinite(node["depthOffsetUnits"], 0.0f);
    material.sortPriority = node["sortPriority"] ? YamlNumbers::as<int>(node["sortPriority"]) : 0;
    material.lighting = node["lighting"] ? decodeEnum(node, "lighting", StreamSchema::materialLighting()) : MaterialLighting::AUTO;
    static const Material defaults = Material();
    material.stochasticTiling = node["stochasticTiling"] ? YamlNumbers::as<bool>(node["stochasticTiling"]) : false;
    material.stochasticCellSize = decodeFinite(node["stochasticCellSize"], defaults.stochasticCellSize);
    material.stochasticBlendSharpness = decodeFinite(node["stochasticBlendSharpness"], defaults.stochasticBlendSharpness);
    material.stochasticContrast = decodeFinite(node["stochasticContrast"], defaults.stochasticContrast);
    material.stochasticRotation = decodeFinite(node["stochasticRotation"], defaults.stochasticRotation);
    decodeStencilAndColorWrite(node, material.stencil, material.colorWrite);
    material.extraPass = decodeMaterialPass(node["extraPass"]);
    material.emissiveStrength = std::max(decodeFinite(node["emissiveStrength"], 1.0f), 0.0f);
    material.pbr = decodeMaterialPbr(node["pbr"]);

    return material;
}

// A mesh whose geometry is regenerated from a model's source file on load must not
// serialize its buffers/embedded textures (they are large and redundant). This holds
// for an entity carrying the ModelComponent itself, and for the per-node child mesh
// entities a multi-node model spreads its geometry across (tracked in meshNodesMapping).
bool editor::Stream::isModelBackedMesh(const Entity entity, const EntityRegistry* registry, Signature signature) {
    const auto modelId = registry->getComponentId<ModelComponent>();
    const auto transformId = registry->getComponentId<Transform>();

    if (signature.test(modelId) && !registry->getComponent<ModelComponent>(entity).filename.empty()) {
        return true;
    }
    if (!signature.test(transformId)) {
        return false;
    }
    // Walk up to the owning model: a child mesh is model-backed only if that model has a
    // source file and lists this entity among its generated nodes (manually added child
    // meshes are not in meshNodesMapping and must still serialize their geometry).
    Entity ancestor = registry->getComponent<Transform>(entity).parent;
    while (ancestor != NULL_ENTITY) {
        Signature aSig = registry->getSignature(ancestor);
        if (aSig.test(modelId)) {
            const ModelComponent& model = registry->getComponent<ModelComponent>(ancestor);
            if (!model.filename.empty()) {
                for (const auto& [nodeIdx, mappedEntity] : model.meshNodesMapping) {
                    if (mappedEntity == entity) {
                        return true;
                    }
                }
            }
            return false; // owning model found, but this entity is not one of its meshes
        }
        if (!aSig.test(transformId)) {
            break;
        }
        ancestor = registry->getComponent<Transform>(ancestor).parent;
    }
    return false;
}

bool editor::Stream::isGeneratedMesh(const EntityRegistry* registry, Signature signature) {
    return signature.test(registry->getComponentId<SpriteComponent>()) ||
           signature.test(registry->getComponentId<TilemapComponent>()) ||
           signature.test(registry->getComponentId<MeshPolygonComponent>()) ||
           signature.test(registry->getComponentId<TerrainComponent>()) ||
           // the Spline Extrude's Mesh is its chunks' template: material and flags, no geometry
           signature.test(registry->getComponentId<SplineExtrudeComponent>());
}

unsigned int editor::Stream::savedSubmeshCount(const MeshComponent& mesh, const EntityRegistry* registry, Signature signature) {
    // a Terrain has its two (full and half resolution) only once it first ran (MeshSystem::createTerrain)
    if (signature.test(registry->getComponentId<TerrainComponent>())) {
        return std::max(mesh.numSubmeshes, 2u);
    }
    // a Spline Extrude's template has the one holding its chunks' material only once SplineSystem ran
    if (signature.test(registry->getComponentId<SplineExtrudeComponent>())) {
        return std::max(mesh.numSubmeshes, 1u);
    }
    return mesh.numSubmeshes;
}

bool editor::Stream::isGeneratedUI(const EntityRegistry* registry, Signature signature) {
    return signature.test(registry->getComponentId<ImageComponent>()) ||
           signature.test(registry->getComponentId<TextComponent>()) ||
           signature.test(registry->getComponentId<PolygonComponent>());
}

YAML::Node editor::Stream::encodeComponents(const Entity entity, const EntityRegistry* registry, Signature signature) {
    YAML::Node compNode = encodeComponentsLive(entity, registry, signature);
    // what the file held that this editor could not read goes back in; whole unknown
    // components only with a full encode (a single component's copy leaves them out)
    StreamSave::applyToComponents(registry, entity, compNode, signature == registry->getSignature(entity));
    return compNode;
}

YAML::Node editor::Stream::encodeComponentsLive(const Entity entity, const EntityRegistry* registry, Signature signature) {
    YAML::Node compNode;
    // in the order of the components map (editor/reflect/Registry.cpp, serializationOrder); a
    // component reads its entity's other components (a mesh made from a model file or a sprite
    // writes no geometry, a text's UI no texture)
    reflect::EncodeContext ctx;
    ctx.registry = registry;
    ctx.entity = entity;
    ctx.signature = signature;
    EntityRegistry* mutableRegistry = const_cast<EntityRegistry*>(registry);
    for (const reflect::ComponentDesc* desc : reflect::serializationOrder()) {
        if (!signature.test(desc->componentId(registry))) continue;
        compNode[desc->key] = reflect::encode(*desc, desc->find(mutableRegistry, entity), ctx);
    }
    return compNode;
}

namespace {

    namespace reflect = jasperengine::editor::reflect;
    using jasperengine::editor::Catalog;
    using jasperengine::editor::ComponentType;

    // Two encodings say the same (map keys in any order). Both come from one encoder, so map keys
    // are taken in order and looked up only where the orders part (a yaml-cpp map lookup converts
    // every key it passes). `except`: a top-level key left out
    bool sameEncoding(const YAML::Node& a, const YAML::Node& b, const char* except = nullptr) {
        if (a.Type() != b.Type()) {
            return false;
        }
        switch (a.Type()) {
            case YAML::NodeType::Scalar:
                return a.Scalar() == b.Scalar();
            case YAML::NodeType::Sequence:
                if (a.size() != b.size()) {
                    return false;
                }
                for (size_t i = 0; i < a.size(); i++) {
                    if (!sameEncoding(a[i], b[i])) {
                        return false;
                    }
                }
                return true;
            case YAML::NodeType::Map: {
                if (a.size() != b.size()) {
                    return false;
                }
                YAML::const_iterator next = b.begin();
                for (YAML::const_iterator entry = a.begin(); entry != a.end(); ++entry) {
                    const std::string& key = entry->first.Scalar();
                    const bool inOrder = next != b.end() && next->first.Scalar() == key;
                    const YAML::Node other = inOrder ? YAML::Node(next->second) : b[key];
                    if (inOrder) {
                        ++next;
                    }
                    if (!other || (!(except && key == except) && !sameEncoding(entry->second, other))) {
                        return false;
                    }
                }
                return true;
            }
            default:
                return true;
        }
    }

    // A Mesh that encodes as the node has it but for its worldAABB (a Play that moved its
    // GameObject, or left it alone) takes the node's worldAABB and stays: the decode would set
    // only what the encoding holds, and run update flags that always reload a Mesh
    bool keepSameMesh(const reflect::ComponentDesc& desc, Entity entity, EntityRegistry* registry, void* existing, const YAML::Node& componentNode) {
        reflect::EncodeContext ctx;
        ctx.registry = registry;
        ctx.entity = entity;
        ctx.signature = registry->getSignature(entity);
        const YAML::Node worldAABB = componentNode["worldAABB"];
        if (!worldAABB || !sameEncoding(reflect::encode(desc, existing, ctx), componentNode, "worldAABB")) {
            return false;
        }
        static_cast<MeshComponent*>(existing)->worldAABB = editor::Stream::decodeAABB(worldAABB);
        return true;
    }

    // One component decoded over the entity's own (or a default one) and stored: a new one with
    // its structural update flags, an existing one with the flags of what changed (or marked
    // changed only, DecodeApply::MarkOnly)
    void decodeComponentInto(const reflect::ComponentDesc& desc, Entity entity, Entity parent, EntityRegistry* registry, bool had,
                             const YAML::Node& componentNode, bool keepSameMeshes) {
        void* existing = had ? desc.find(registry, entity) : nullptr;
        if (keepSameMeshes && existing && desc.type == ComponentType::MeshComponent &&
                keepSameMesh(desc, entity, registry, existing, componentNode)) {
            return;
        }
        reflect::Value value(desc.body.ops, existing);
        reflect::DecodeContext ctx;
        ctx.registry = registry;
        ctx.entity = entity;
        ctx.old = existing;
        ctx.component = desc.key;
        reflect::DecodeReport report;
        reflect::decode(desc, componentNode, value.get(), ctx, report);
        const bool isTransform = desc.type == ComponentType::Transform;
        if (isTransform) {
            static_cast<Transform*>(value.get())->parent = parent;
        }
        if (!existing) {
            desc.add(registry, entity, value.get());
            if (desc.structuralUpdateFlags) {
                Catalog::updateEntity(registry, entity, desc.structuralUpdateFlags);
            }
            return;
        }
        const bool update = desc.decodeApply == reflect::DecodeApply::UpdateFlags;
        uint64_t flags = update ? Catalog::getChangedUpdateFlags(desc.type, existing, value.get()) : 0;
        // a mesh once held all its submesh slots, whose materials always count as changed, so every
        // decode over a live mesh reloaded it; a mesh of no submeshes yet still does
        if (update && desc.type == ComponentType::MeshComponent) flags |= editor::UpdateFlags_Mesh_Reload | editor::UpdateFlags_Mesh_Texture;
        if (isTransform) {
            registry->addEntityChild(parent, entity, false);
        }
        desc.body.ops->move(existing, value.get());
        registry->markChanged(desc.componentId(registry), entity);
        if (update) {
            Catalog::updateEntity(registry, entity, flags);
        }
    }

}

void editor::Stream::decodeComponents(Entity entity, Entity parent, EntityRegistry* registry, const YAML::Node& compNode, bool keepSameMeshes){
    const Signature signature = registry->getSignature(entity);

    // Each component through StreamLoad::decodeComponent: a value that does not decode, or a
    // key or component this editor does not know, is kept (StreamPreserved.h) instead of
    // failing the load or vanishing on the next save
    for (const reflect::ComponentDesc* desc : reflect::serializationOrder()) {
        const bool had = signature.test(desc->componentId(registry));
        StreamLoad::decodeComponent(desc->type, entity, registry, compNode, [&](const YAML::Node& componentNode) {
            decodeComponentInto(*desc, entity, parent, registry, had, componentNode, keepSameMeshes);
        });
    }

    StreamLoad::preserveUnknownComponents(entity, registry, compNode);
}

YAML::Node editor::Stream::encodeTransform(const Transform& transform) {
    return reflect::encodeValue(ComponentType::Transform, &transform);
}

Transform editor::Stream::decodeTransform(const YAML::Node& node, const Transform* oldTransform) {
    return reflect::decodeValue<Transform>(ComponentType::Transform, node, oldTransform);
}

// ---- upstream's codecs of its new components (safe-area, ui-scaling-text, video-mp4, camera
// shake): the reflection tables' codecs of those components (editor/reflect)

YAML::Node editor::Stream::encodeSafeAreaComponent(const SafeAreaComponent& safeArea) {
    // an empty map when every field is the default: `safearea: {}`
    YAML::Node node(YAML::NodeType::Map);
    const SafeAreaComponent defaults;
    if (safeArea.enabled != defaults.enabled) node["enabled"] = YamlNumbers::text(safeArea.enabled);
    if (safeArea.left != defaults.left) node["left"] = YamlNumbers::text(safeArea.left);
    if (safeArea.top != defaults.top) node["top"] = YamlNumbers::text(safeArea.top);
    if (safeArea.right != defaults.right) node["right"] = YamlNumbers::text(safeArea.right);
    if (safeArea.bottom != defaults.bottom) node["bottom"] = YamlNumbers::text(safeArea.bottom);
    if (safeArea.includeExtraInsets != defaults.includeExtraInsets) node["includeExtraInsets"] = YamlNumbers::text(safeArea.includeExtraInsets);
    if (node.size() == 0) node.SetStyle(YAML::EmitterStyle::Flow);
    return node;
}

SafeAreaComponent editor::Stream::decodeSafeAreaComponent(const YAML::Node& node, const SafeAreaComponent* oldSafeArea) {
    // from the defaults (a field left out is the default), the runtime state kept
    SafeAreaComponent safeArea;
    if (oldSafeArea) safeArea.appliedRevision = oldSafeArea->appliedRevision;
    if (!node.IsMap()) return safeArea;
    if (node["enabled"]) safeArea.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["left"]) safeArea.left = YamlNumbers::as<bool>(node["left"]);
    if (node["top"]) safeArea.top = YamlNumbers::as<bool>(node["top"]);
    if (node["right"]) safeArea.right = YamlNumbers::as<bool>(node["right"]);
    if (node["bottom"]) safeArea.bottom = YamlNumbers::as<bool>(node["bottom"]);
    if (node["includeExtraInsets"]) safeArea.includeExtraInsets = YamlNumbers::as<bool>(node["includeExtraInsets"]);
    return safeArea;
}

YAML::Node editor::Stream::encodeTextEffectLayer(const TextEffectLayer& layer) {
    const TextEffectLayer d;
    YAML::Node node(YAML::NodeType::Map);
    node["type"] = TextEffects::typeName(layer.type);
    if (layer.enabled != d.enabled) node["enabled"] = layer.enabled;
    // (the default amplitude is the type's: 1 for Grow and Unfold)
    if (layer.amplitude != TextEffects::defaultAmplitude(layer.type)) node["amplitude"] = layer.amplitude;
    if (layer.duration != d.duration) node["duration"] = layer.duration;
    if (layer.spread != d.spread) node["spread"] = layer.spread;
    if (layer.phaseStep != d.phaseStep) node["phaseStep"] = layer.phaseStep;
    if (layer.frequency != d.frequency) node["frequency"] = layer.frequency;
    if (layer.charactersPerSecond != d.charactersPerSecond) node["charactersPerSecond"] = layer.charactersPerSecond;
    if (layer.fadeTime != d.fadeTime) node["fadeTime"] = layer.fadeTime;
    if (!layer.gradient.empty()) {
        YAML::Node stops(YAML::NodeType::Sequence);
        for (const TextEffectGradientStop& stop : layer.gradient) {
            YAML::Node entry;
            entry["color"] = encodeVector4(stop.color);
            entry["position"] = stop.position;
            stops.push_back(entry);
        }
        node["gradient"] = stops;
    }
    if (layer.mix != d.mix) node["mix"] = TextEffects::mixName(layer.mix);
    if (layer.saturation != d.saturation) node["saturation"] = layer.saturation;
    if (layer.lightness != d.lightness) node["lightness"] = layer.lightness;
    return node;
}

TextEffectLayer editor::Stream::decodeTextEffectLayer(const YAML::Node& node) {
    TextEffectLayer layer;
    if (!node || !node.IsMap()) return layer;
    if (node["type"] && !TextEffects::parseType(node["type"].as<std::string>().c_str(), layer.type)) {
        Log::warn("Unknown Text Effect type '%s'", node["type"].as<std::string>().c_str());
    }
    if (node["enabled"]) layer.enabled = YamlNumbers::as<bool>(node["enabled"]);
    layer.amplitude = TextEffects::defaultAmplitude(layer.type);
    if (node["amplitude"]) layer.amplitude = decodeFinite(node["amplitude"], layer.amplitude);
    if (node["duration"]) layer.duration = decodeFinite(node["duration"], layer.duration);
    if (node["spread"]) layer.spread = decodeFinite(node["spread"], layer.spread);
    if (node["phaseStep"]) layer.phaseStep = decodeFinite(node["phaseStep"], layer.phaseStep);
    if (node["frequency"]) layer.frequency = decodeFinite(node["frequency"], layer.frequency);
    if (node["charactersPerSecond"]) layer.charactersPerSecond = decodeFinite(node["charactersPerSecond"], layer.charactersPerSecond);
    if (node["fadeTime"]) layer.fadeTime = decodeFinite(node["fadeTime"], layer.fadeTime);
    if (node["gradient"] && node["gradient"].IsSequence()) {
        for (const YAML::Node& entry : node["gradient"]) {
            TextEffectGradientStop stop;
            if (entry["color"]) stop.color = decodeVector4(entry["color"]);
            if (entry["position"]) stop.position = decodeFinite(entry["position"], 0.0f);
            layer.gradient.push_back(stop);
        }
    }
    if (node["mix"] && !TextEffects::parseMix(node["mix"].as<std::string>().c_str(), layer.mix)) {
        Log::warn("Unknown Text Effect mix '%s'", node["mix"].as<std::string>().c_str());
    }
    if (node["saturation"]) layer.saturation = decodeFinite(node["saturation"], layer.saturation);
    if (node["lightness"]) layer.lightness = decodeFinite(node["lightness"], layer.lightness);
    return layer;
}

YAML::Node editor::Stream::encodeTextEffectComponent(const TextEffectComponent& effect) {
    const TextEffectComponent d;
    YAML::Node node(YAML::NodeType::Map);
    if (effect.enabled != d.enabled) node["enabled"] = effect.enabled;
    // the layers in use, up to the last one that is not None
    size_t used = 0;
    for (size_t i = 0; i < effect.layers.size(); i++) {
        if (effect.layers[i] != TextEffectLayer()) used = i + 1;
    }
    if (used > 0) {
        YAML::Node layers(YAML::NodeType::Sequence);
        for (size_t i = 0; i < used; i++) layers.push_back(encodeTextEffectLayer(effect.layers[i]));
        node["layers"] = layers;
    }
    if (effect.autoPlay != d.autoPlay) node["autoPlay"] = effect.autoPlay;
    if (effect.loop != d.loop) node["loop"] = effect.loop;
    if (effect.useUnscaledTime != d.useUnscaledTime) node["useUnscaledTime"] = effect.useUnscaledTime;
    if (effect.startIndex != d.startIndex) node["startIndex"] = effect.startIndex;
    if (effect.length != d.length) node["length"] = effect.length;
    if (!effect.forWords.empty()) node["forWords"] = effect.forWords;
    return node;
}

TextEffectComponent editor::Stream::decodeTextEffectComponent(const YAML::Node& node, const TextEffectComponent* oldEffect) {
    TextEffectComponent effect;
    if (oldEffect) {
        effect = *oldEffect;
    }
    if (!node || !node.IsMap()) return effect;
    // absent = the default, also over an old effect (fields are written only when set)
    const TextEffectComponent d;
    effect.enabled = node["enabled"] ? YamlNumbers::as<bool>(node["enabled"]) : d.enabled;
    effect.layers = d.layers;
    if (node["layers"] && node["layers"].IsSequence()) {
        for (size_t i = 0; i < node["layers"].size() && i < effect.layers.size(); i++) {
            effect.layers[i] = decodeTextEffectLayer(node["layers"][i]);
        }
    }
    for (size_t i = 0; i < effect.layers.size(); i++)
        effect.editedTypes[i] = effect.layers[i].type;
    effect.autoPlay = node["autoPlay"] ? YamlNumbers::as<bool>(node["autoPlay"]) : d.autoPlay;
    effect.loop = node["loop"] ? YamlNumbers::as<bool>(node["loop"]) : d.loop;
    effect.useUnscaledTime = node["useUnscaledTime"] ? YamlNumbers::as<bool>(node["useUnscaledTime"]) : d.useUnscaledTime;
    effect.startIndex = node["startIndex"] ? std::max(YamlNumbers::as<int>(node["startIndex"]), 0) : d.startIndex;
    effect.length = node["length"] ? std::max(YamlNumbers::as<int>(node["length"]), -1) : d.length;
    effect.forWords = node["forWords"] ? node["forWords"].as<std::string>() : d.forWords;
    effect.needApply = true;
    return effect;
}

namespace {

    const char* videoPlayerSourceName(VideoPlayerSource value){
        return value == VideoPlayerSource::URL ? "url" : "clip";
    }
    VideoPlayerSource videoPlayerSourceOf(const std::string& text, VideoPlayerSource fallback){
        if (text == "url") return VideoPlayerSource::URL;
        if (text == "clip") return VideoPlayerSource::CLIP;
        return fallback;
    }
    const char* videoTimeUpdateModeName(VideoTimeUpdateMode value){
        return value == VideoTimeUpdateMode::UNSCALED_GAME_TIME ? "unscaled_game_time" : "game_time";
    }
    VideoTimeUpdateMode videoTimeUpdateModeOf(const std::string& text, VideoTimeUpdateMode fallback){
        if (text == "unscaled_game_time") return VideoTimeUpdateMode::UNSCALED_GAME_TIME;
        if (text == "game_time") return VideoTimeUpdateMode::GAME_TIME;
        return fallback;
    }
    const char* videoRenderModeName(VideoRenderMode value){
        switch (value){
            case VideoRenderMode::CAMERA_FAR_PLANE: return "camera_far_plane";
            case VideoRenderMode::CAMERA_NEAR_PLANE: return "camera_near_plane";
            case VideoRenderMode::RENDER_TEXTURE: return "render_texture";
            case VideoRenderMode::MATERIAL_OVERRIDE: return "material_override";
            case VideoRenderMode::API_ONLY: return "api_only";
        }
        return "render_texture";
    }
    VideoRenderMode videoRenderModeOf(const std::string& text, VideoRenderMode fallback){
        for (VideoRenderMode mode : {VideoRenderMode::CAMERA_FAR_PLANE, VideoRenderMode::CAMERA_NEAR_PLANE, VideoRenderMode::RENDER_TEXTURE,
                                     VideoRenderMode::MATERIAL_OVERRIDE, VideoRenderMode::API_ONLY}){
            if (text == videoRenderModeName(mode)) return mode;
        }
        return fallback;
    }
    const char* videoAspectRatioName(VideoAspectRatio value){
        switch (value){
            case VideoAspectRatio::NO_SCALING: return "no_scaling";
            case VideoAspectRatio::FIT_VERTICALLY: return "fit_vertically";
            case VideoAspectRatio::FIT_HORIZONTALLY: return "fit_horizontally";
            case VideoAspectRatio::FIT_INSIDE: return "fit_inside";
            case VideoAspectRatio::FIT_OUTSIDE: return "fit_outside";
            case VideoAspectRatio::STRETCH: return "stretch";
        }
        return "fit_inside";
    }
    VideoAspectRatio videoAspectRatioOf(const std::string& text, VideoAspectRatio fallback){
        for (VideoAspectRatio fit : {VideoAspectRatio::NO_SCALING, VideoAspectRatio::FIT_VERTICALLY, VideoAspectRatio::FIT_HORIZONTALLY,
                                     VideoAspectRatio::FIT_INSIDE, VideoAspectRatio::FIT_OUTSIDE, VideoAspectRatio::STRETCH}){
            if (text == videoAspectRatioName(fit)) return fit;
        }
        return fallback;
    }
    const char* videoAudioOutputModeName(VideoAudioOutputMode value){
        switch (value){
            case VideoAudioOutputMode::NONE: return "none";
            case VideoAudioOutputMode::AUDIO_SOURCE: return "audio_source";
            case VideoAudioOutputMode::DIRECT: return "direct";
        }
        return "direct";
    }
    VideoAudioOutputMode videoAudioOutputModeOf(const std::string& text, VideoAudioOutputMode fallback){
        if (text == "none") return VideoAudioOutputMode::NONE;
        if (text == "audio_source") return VideoAudioOutputMode::AUDIO_SOURCE;
        if (text == "direct") return VideoAudioOutputMode::DIRECT;
        return fallback;
    }

}

YAML::Node editor::Stream::encodeVideoPlayerComponent(const VideoPlayerComponent& player) {
    YAML::Node node;
    node["enabled"] = player.enabled;

    // only what differs from the defaults (Unity's); decode restores the defaults
    const VideoPlayerComponent defaults;
    if (player.source != defaults.source) node["source"] = videoPlayerSourceName(player.source);
    if (player.clip != defaults.clip) node["clip"] = player.clip;
    if (player.url != defaults.url) node["url"] = player.url;
    if (player.playOnAwake != defaults.playOnAwake) node["playOnAwake"] = player.playOnAwake;
    if (player.waitForFirstFrame != defaults.waitForFirstFrame) node["waitForFirstFrame"] = player.waitForFirstFrame;
    if (player.isLooping != defaults.isLooping) node["isLooping"] = player.isLooping;
    if (player.skipOnDrop != defaults.skipOnDrop) node["skipOnDrop"] = player.skipOnDrop;
    if (player.playbackSpeed != defaults.playbackSpeed) encodeFinite(node, "playbackSpeed", player.playbackSpeed);
    if (player.timeUpdateMode != defaults.timeUpdateMode) node["timeUpdateMode"] = videoTimeUpdateModeName(player.timeUpdateMode);
    if (player.renderMode != defaults.renderMode) node["renderMode"] = videoRenderModeName(player.renderMode);
    if (player.targetCamera != defaults.targetCamera) node["targetCamera"] = player.targetCamera;
    if (player.targetCameraAlpha != defaults.targetCameraAlpha) encodeFinite(node, "targetCameraAlpha", player.targetCameraAlpha);
    if (player.aspectRatio != defaults.aspectRatio) node["aspectRatio"] = videoAspectRatioName(player.aspectRatio);
    if (player.targetMaterialRenderer != defaults.targetMaterialRenderer) node["targetMaterialRenderer"] = player.targetMaterialRenderer;
    if (player.targetMaterialProperty != defaults.targetMaterialProperty) node["targetMaterialProperty"] = player.targetMaterialProperty;
    if (player.targetSubmesh != defaults.targetSubmesh) node["targetSubmesh"] = player.targetSubmesh;
    if (player.renderTextureWidth != defaults.renderTextureWidth) node["renderTextureWidth"] = player.renderTextureWidth;
    if (player.renderTextureHeight != defaults.renderTextureHeight) node["renderTextureHeight"] = player.renderTextureHeight;
    if (player.audioOutputMode != defaults.audioOutputMode) node["audioOutputMode"] = videoAudioOutputModeName(player.audioOutputMode);
    if (player.directVolume != defaults.directVolume) encodeFinite(node, "directVolume", player.directVolume);
    if (player.directMute != defaults.directMute) node["directMute"] = player.directMute;
    if (player.targetAudioSource != defaults.targetAudioSource) node["targetAudioSource"] = player.targetAudioSource;
    return node;
}

VideoPlayerComponent editor::Stream::decodeVideoPlayerComponent(const YAML::Node& node, const VideoPlayerComponent* oldPlayer) {
    // the old one's runtime state (its render texture, which textures linked to "video|<entity>"
    // show, and its listeners) stays
    VideoPlayerComponent player;
    if (oldPlayer) {
        player = *oldPlayer;
    }

    if (node["enabled"]) player.enabled = YamlNumbers::as<bool>(node["enabled"]);

    // keys are omitted at their defaults: a missing key is the default, not the old value
    const VideoPlayerComponent defaults;
    auto given = [&node](const char* key) {
        const YAML::Node value = node[key];
        return value && !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    };
    player.source = given("source") ? videoPlayerSourceOf(node["source"].as<std::string>(), defaults.source) : defaults.source;
    player.clip = given("clip") ? node["clip"].as<std::string>() : defaults.clip;
    player.url = given("url") ? node["url"].as<std::string>() : defaults.url;
    player.playOnAwake = given("playOnAwake") ? YamlNumbers::as<bool>(node["playOnAwake"]) : defaults.playOnAwake;
    player.waitForFirstFrame = given("waitForFirstFrame") ? YamlNumbers::as<bool>(node["waitForFirstFrame"]) : defaults.waitForFirstFrame;
    player.isLooping = given("isLooping") ? YamlNumbers::as<bool>(node["isLooping"]) : defaults.isLooping;
    player.skipOnDrop = given("skipOnDrop") ? YamlNumbers::as<bool>(node["skipOnDrop"]) : defaults.skipOnDrop;
    player.playbackSpeed = given("playbackSpeed") ? std::clamp(decodeFinite(node["playbackSpeed"], defaults.playbackSpeed), 0.0625f, 16.0f) : defaults.playbackSpeed;
    player.timeUpdateMode = given("timeUpdateMode") ? videoTimeUpdateModeOf(node["timeUpdateMode"].as<std::string>(), defaults.timeUpdateMode) : defaults.timeUpdateMode;
    player.renderMode = given("renderMode") ? videoRenderModeOf(node["renderMode"].as<std::string>(), defaults.renderMode) : defaults.renderMode;
    player.targetCamera = given("targetCamera") ? YamlNumbers::as<Entity>(node["targetCamera"]) : defaults.targetCamera;
    player.targetCameraAlpha = given("targetCameraAlpha") ? std::clamp(decodeFinite(node["targetCameraAlpha"], defaults.targetCameraAlpha), 0.0f, 1.0f) : defaults.targetCameraAlpha;
    player.aspectRatio = given("aspectRatio") ? videoAspectRatioOf(node["aspectRatio"].as<std::string>(), defaults.aspectRatio) : defaults.aspectRatio;
    player.targetMaterialRenderer = given("targetMaterialRenderer") ? YamlNumbers::as<Entity>(node["targetMaterialRenderer"]) : defaults.targetMaterialRenderer;
    player.targetMaterialProperty = given("targetMaterialProperty") ? node["targetMaterialProperty"].as<std::string>() : defaults.targetMaterialProperty;
    player.targetSubmesh = given("targetSubmesh") ? YamlNumbers::as<int>(node["targetSubmesh"]) : defaults.targetSubmesh;
    player.renderTextureWidth = given("renderTextureWidth") ? YamlNumbers::as<unsigned int>(node["renderTextureWidth"]) : defaults.renderTextureWidth;
    player.renderTextureHeight = given("renderTextureHeight") ? YamlNumbers::as<unsigned int>(node["renderTextureHeight"]) : defaults.renderTextureHeight;
    player.audioOutputMode = given("audioOutputMode") ? videoAudioOutputModeOf(node["audioOutputMode"].as<std::string>(), defaults.audioOutputMode) : defaults.audioOutputMode;
    player.directVolume = given("directVolume") ? std::clamp(decodeFinite(node["directVolume"], defaults.directVolume), 0.0f, 1.0f) : defaults.directVolume;
    player.directMute = given("directMute") ? YamlNumbers::as<bool>(node["directMute"]) : defaults.directMute;
    player.targetAudioSource = given("targetAudioSource") ? YamlNumbers::as<Entity>(node["targetAudioSource"]) : defaults.targetAudioSource;
    return player;
}

YAML::Node editor::Stream::encodeCameraShakerComponent(const CameraShakerComponent& shaker) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(shaker.enabled);
    node["global"] = YamlNumbers::text(shaker.global);
    node["unscaledTime"] = YamlNumbers::text(shaker.unscaledTime);
    node["mode"] = CameraShaker::modeToString(shaker.mode);
    encodeFinite(node, "positionScale", shaker.positionScale);
    encodeFinite(node, "rotationScale", shaker.rotationScale);
    node["startPreset"] = shaker.startPreset;
    encodeFinite(node, "traumaDecay", shaker.traumaDecay);
    encodeFinite(node, "traumaPower", shaker.traumaPower);
    node["traumaMaxOffset"] = encodeVector3(shaker.traumaMaxOffset);
    node["traumaMaxAngle"] = encodeVector3(shaker.traumaMaxAngle);
    encodeFinite(node, "traumaFrequency", shaker.traumaFrequency);
    return node;
}

CameraShakerComponent editor::Stream::decodeCameraShakerComponent(const YAML::Node& node, const CameraShakerComponent* oldShaker) {
    // a missing key is the default; an old component's shakes play on
    CameraShakerComponent shaker;
    if (oldShaker) {
        shaker.runtime = oldShaker->runtime;
    }
    if (node["enabled"]) shaker.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["global"]) shaker.global = YamlNumbers::as<bool>(node["global"]);
    if (node["unscaledTime"]) shaker.unscaledTime = YamlNumbers::as<bool>(node["unscaledTime"]);
    if (node["mode"]) {
        CameraShakeMode mode;
        if (CameraShaker::modeFromString(node["mode"].as<std::string>(), mode)) shaker.mode = mode;
    }
    if (node["positionScale"]) shaker.positionScale = std::max(0.0f, decodeFinite(node["positionScale"], shaker.positionScale));
    if (node["rotationScale"]) shaker.rotationScale = std::max(0.0f, decodeFinite(node["rotationScale"], shaker.rotationScale));
    if (node["startPreset"]) shaker.startPreset = node["startPreset"].as<std::string>();
    if (node["traumaDecay"]) shaker.traumaDecay = std::max(0.0f, decodeFinite(node["traumaDecay"], shaker.traumaDecay));
    if (node["traumaPower"]) shaker.traumaPower = std::max(1.0f, std::min(4.0f, decodeFinite(node["traumaPower"], shaker.traumaPower)));
    if (node["traumaMaxOffset"]) shaker.traumaMaxOffset = decodeVector3(node["traumaMaxOffset"]);
    if (node["traumaMaxAngle"]) shaker.traumaMaxAngle = decodeVector3(node["traumaMaxAngle"]);
    if (node["traumaFrequency"]) shaker.traumaFrequency = std::max(0.0f, decodeFinite(node["traumaFrequency"], shaker.traumaFrequency));
    return shaker;
}

YAML::Node editor::Stream::encodeCameraFollowComponent(const CameraFollowComponent& follow) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(follow.enabled);
    node["target"] = follow.target;
    node["offset"] = encodeVector3(follow.offset);
    node["offsetSpace"] = CameraFollow::offsetSpaceToString(follow.offsetSpace);
    node["damping"] = encodeVector3(follow.damping);
    encodeFinite(node, "maxSpeed", follow.maxSpeed);
    node["lookAt"] = YamlNumbers::text(follow.lookAt);
    node["lookAtOffset"] = encodeVector3(follow.lookAtOffset);
    encodeFinite(node, "rotationDamping", follow.rotationDamping);
    node["deadZone"] = encodeVector2(follow.deadZone);
    node["useBounds"] = YamlNumbers::text(follow.useBounds);
    node["boundsMin"] = encodeVector3(follow.boundsMin);
    node["boundsMax"] = encodeVector3(follow.boundsMax);
    node["unscaledTime"] = YamlNumbers::text(follow.unscaledTime);
    return node;
}

CameraFollowComponent editor::Stream::decodeCameraFollowComponent(const YAML::Node& node, const CameraFollowComponent* oldFollow) {
    // a missing key is the default; an old component's motion (velocity, placement) goes on
    CameraFollowComponent follow;
    if (oldFollow) {
        follow = *oldFollow;
        const CameraFollowComponent defaults;
        follow.enabled = defaults.enabled;
        follow.target = defaults.target;
        follow.offset = defaults.offset;
        follow.offsetSpace = defaults.offsetSpace;
        follow.damping = defaults.damping;
        follow.maxSpeed = defaults.maxSpeed;
        follow.lookAt = defaults.lookAt;
        follow.lookAtOffset = defaults.lookAtOffset;
        follow.rotationDamping = defaults.rotationDamping;
        follow.deadZone = defaults.deadZone;
        follow.useBounds = defaults.useBounds;
        follow.boundsMin = defaults.boundsMin;
        follow.boundsMax = defaults.boundsMax;
        follow.unscaledTime = defaults.unscaledTime;
        // a scene file names a GameObject of its own scene
        follow.targetPersistent = false;
    }
    if (node["enabled"]) follow.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["target"]) follow.target = YamlNumbers::as<Entity>(node["target"]);
    if (node["offset"]) follow.offset = decodeVector3(node["offset"]);
    if (node["offsetSpace"]) {
        FollowOffsetSpace space;
        if (CameraFollow::offsetSpaceFromString(node["offsetSpace"].as<std::string>(), space)) follow.offsetSpace = space;
    }
    if (node["damping"]) {
        const Vector3 damping = decodeVector3(node["damping"]);
        follow.damping = Vector3(std::max(0.0f, damping.x), std::max(0.0f, damping.y), std::max(0.0f, damping.z));
    }
    if (node["maxSpeed"]) follow.maxSpeed = std::max(0.0f, decodeFinite(node["maxSpeed"], follow.maxSpeed));
    if (node["lookAt"]) follow.lookAt = YamlNumbers::as<bool>(node["lookAt"]);
    if (node["lookAtOffset"]) follow.lookAtOffset = decodeVector3(node["lookAtOffset"]);
    if (node["rotationDamping"]) follow.rotationDamping = std::max(0.0f, decodeFinite(node["rotationDamping"], follow.rotationDamping));
    if (node["deadZone"]) {
        const Vector2 zone = decodeVector2(node["deadZone"]);
        follow.deadZone = Vector2(std::max(0.0f, zone.x), std::max(0.0f, zone.y));
    }
    if (node["useBounds"]) follow.useBounds = YamlNumbers::as<bool>(node["useBounds"]);
    if (node["boundsMin"]) follow.boundsMin = decodeVector3(node["boundsMin"]);
    if (node["boundsMax"]) follow.boundsMax = decodeVector3(node["boundsMax"]);
    if (node["unscaledTime"]) follow.unscaledTime = YamlNumbers::as<bool>(node["unscaledTime"]);
    return follow;
}
