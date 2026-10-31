// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Stream.h"
#include "AppSettings.h"
#include "EditorHost.h"
#include "Out.h"
#include "Workspace.h"

#include "Base64.h"
#include "Catalog.h"
#include "util/YamlNumbers.h"
#include "lattice/LatticeTypes.h"
#include "animation/AnimationClipIO.h"
#include "animation/ModelSubClips.h"
#include "Factory.h"
#include "pool/SoundPool.h"
#include "pool/ShaderPool.h"
#include "util/CameraTextureLink.h"
#include "util/Color.h"
#include "util/FluidTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/StringUtils.h"
#include "ecs/EntityManager.h"
#include "render/SceneRender2D.h"
#include "render/SceneRender3D.h"
#include "window/TerrainEditWindow.h"
#include "physics/PlacementSettingsLimits.h"
#include "spritenormals/SpriteNormalsRecipe.h"
#include "input/InputActions.h"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <mutex>
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
YAML::Node editor::Stream::encodeShaderKeys(const std::set<ShaderKey>& shaderKeys) {
    YAML::Node node(YAML::NodeType::Sequence);
    for (ShaderKey rawKey : shaderKeys) {
        YAML::Node entry = encodeShaderKey(ShaderPool::normalizeKey(rawKey));
        if (entry.IsMap()) node.push_back(entry);
    }
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

float editor::Stream::decodeFinite(const YAML::Node& node, float fallback) {
    if (!node || !node.IsScalar()) {
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
        return finiteOr(fallback);
    }
}

double editor::Stream::decodeFiniteDouble(const YAML::Node& node, double fallback) {
    if (!node || !node.IsScalar()) {
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
    switch (type) {
        case SceneType::SCENE_3D: return "scene_3d";
        case SceneType::SCENE_2D: return "scene_2d";
        case SceneType::SCENE_UI: return "scene_ui";
        default: return "scene_3d";
    }
}

editor::SceneType editor::Stream::stringToSceneType(const std::string& str){
    if (str == "scene_3d") return SceneType::SCENE_3D;
    if (str == "scene_2d") return SceneType::SCENE_2D;
    if (str == "scene_ui") return SceneType::SCENE_UI;
    return SceneType::SCENE_3D; // Default
}

std::string editor::Stream::primitiveTypeToString(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::TRIANGLES: return "triangles";
        case PrimitiveType::TRIANGLE_STRIP: return "triangle_strip";
        case PrimitiveType::POINTS: return "points";
        case PrimitiveType::LINES: return "lines";
        default: return "triangles";
    }
}

PrimitiveType editor::Stream::stringToPrimitiveType(const std::string& str) {
    if (str == "triangles") return PrimitiveType::TRIANGLES;
    if (str == "triangle_strip") return PrimitiveType::TRIANGLE_STRIP;
    if (str == "points") return PrimitiveType::POINTS;
    if (str == "lines") return PrimitiveType::LINES;
    return PrimitiveType::TRIANGLES; // Default
}

std::string editor::Stream::easeTypeToString(EaseType type) {
    switch (type) {
        case EaseType::LINEAR: return "LINEAR";
        case EaseType::QUAD_IN: return "QUAD_IN";
        case EaseType::QUAD_OUT: return "QUAD_OUT";
        case EaseType::QUAD_IN_OUT: return "QUAD_IN_OUT";
        case EaseType::CUBIC_IN: return "CUBIC_IN";
        case EaseType::CUBIC_OUT: return "CUBIC_OUT";
        case EaseType::CUBIC_IN_OUT: return "CUBIC_IN_OUT";
        case EaseType::QUART_IN: return "QUART_IN";
        case EaseType::QUART_OUT: return "QUART_OUT";
        case EaseType::QUART_IN_OUT: return "QUART_IN_OUT";
        case EaseType::QUINT_IN: return "QUINT_IN";
        case EaseType::QUINT_OUT: return "QUINT_OUT";
        case EaseType::QUINT_IN_OUT: return "QUINT_IN_OUT";
        case EaseType::SINE_IN: return "SINE_IN";
        case EaseType::SINE_OUT: return "SINE_OUT";
        case EaseType::SINE_IN_OUT: return "SINE_IN_OUT";
        case EaseType::EXPO_IN: return "EXPO_IN";
        case EaseType::EXPO_OUT: return "EXPO_OUT";
        case EaseType::EXPO_IN_OUT: return "EXPO_IN_OUT";
        case EaseType::CIRC_IN: return "CIRC_IN";
        case EaseType::CIRC_OUT: return "CIRC_OUT";
        case EaseType::CIRC_IN_OUT: return "CIRC_IN_OUT";
        case EaseType::ELASTIC_IN: return "ELASTIC_IN";
        case EaseType::ELASTIC_OUT: return "ELASTIC_OUT";
        case EaseType::ELASTIC_IN_OUT: return "ELASTIC_IN_OUT";
        case EaseType::BACK_IN: return "BACK_IN";
        case EaseType::BACK_OUT: return "BACK_OUT";
        case EaseType::BACK_IN_OUT: return "BACK_IN_OUT";
        case EaseType::BOUNCE_IN: return "BOUNCE_IN";
        case EaseType::BOUNCE_OUT: return "BOUNCE_OUT";
        case EaseType::BOUNCE_IN_OUT: return "BOUNCE_IN_OUT";
        case EaseType::STEP: return "STEP";
        case EaseType::CUSTOM: return "CUSTOM";
        default: return "LINEAR";
    }
}

EaseType editor::Stream::stringToEaseType(const std::string& str) {
    std::string normalized;
    normalized.reserve(str.size());
    for (char ch : str) {
        if (ch == '-') {
            normalized.push_back('_');
        } else {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }

    if (normalized == "LINEAR") return EaseType::LINEAR;
    if (normalized == "QUAD_IN") return EaseType::QUAD_IN;
    if (normalized == "QUAD_OUT") return EaseType::QUAD_OUT;
    if (normalized == "QUAD_IN_OUT") return EaseType::QUAD_IN_OUT;
    if (normalized == "CUBIC_IN") return EaseType::CUBIC_IN;
    if (normalized == "CUBIC_OUT") return EaseType::CUBIC_OUT;
    if (normalized == "CUBIC_IN_OUT") return EaseType::CUBIC_IN_OUT;
    if (normalized == "QUART_IN") return EaseType::QUART_IN;
    if (normalized == "QUART_OUT") return EaseType::QUART_OUT;
    if (normalized == "QUART_IN_OUT") return EaseType::QUART_IN_OUT;
    if (normalized == "QUINT_IN") return EaseType::QUINT_IN;
    if (normalized == "QUINT_OUT") return EaseType::QUINT_OUT;
    if (normalized == "QUINT_IN_OUT") return EaseType::QUINT_IN_OUT;
    if (normalized == "SINE_IN") return EaseType::SINE_IN;
    if (normalized == "SINE_OUT") return EaseType::SINE_OUT;
    if (normalized == "SINE_IN_OUT") return EaseType::SINE_IN_OUT;
    if (normalized == "EXPO_IN") return EaseType::EXPO_IN;
    if (normalized == "EXPO_OUT") return EaseType::EXPO_OUT;
    if (normalized == "EXPO_IN_OUT") return EaseType::EXPO_IN_OUT;
    if (normalized == "CIRC_IN") return EaseType::CIRC_IN;
    if (normalized == "CIRC_OUT") return EaseType::CIRC_OUT;
    if (normalized == "CIRC_IN_OUT") return EaseType::CIRC_IN_OUT;
    if (normalized == "ELASTIC_IN") return EaseType::ELASTIC_IN;
    if (normalized == "ELASTIC_OUT") return EaseType::ELASTIC_OUT;
    if (normalized == "ELASTIC_IN_OUT") return EaseType::ELASTIC_IN_OUT;
    if (normalized == "BACK_IN") return EaseType::BACK_IN;
    if (normalized == "BACK_OUT") return EaseType::BACK_OUT;
    if (normalized == "BACK_IN_OUT") return EaseType::BACK_IN_OUT;
    if (normalized == "BOUNCE_IN") return EaseType::BOUNCE_IN;
    if (normalized == "BOUNCE_OUT") return EaseType::BOUNCE_OUT;
    if (normalized == "BOUNCE_IN_OUT") return EaseType::BOUNCE_IN_OUT;
    if (normalized == "STEP") return EaseType::STEP;
    if (normalized == "CUSTOM") return EaseType::CUSTOM;
    return EaseType::LINEAR;
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

    EaseType type = node.IsScalar() ? stringToEaseType(node.as<std::string>()) : EaseType::LINEAR;

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
    switch (type) {
        case BufferType::VERTEX_BUFFER: return "vertex_buffer";
        case BufferType::INDEX_BUFFER: return "index_buffer";
        case BufferType::STORAGE_BUFFER: return "storage_buffer";
        default: return "vertex_buffer";
    }
}

BufferType editor::Stream::stringToBufferType(const std::string& str) {
    if (str == "vertex_buffer") return BufferType::VERTEX_BUFFER;
    if (str == "index_buffer") return BufferType::INDEX_BUFFER;
    if (str == "storage_buffer") return BufferType::STORAGE_BUFFER;
    return BufferType::VERTEX_BUFFER; // Default
}

// BufferUsage enum conversion
std::string editor::Stream::bufferUsageToString(BufferUsage usage) {
    switch (usage) {
        case BufferUsage::IMMUTABLE: return "immutable";
        case BufferUsage::DYNAMIC: return "dynamic";
        case BufferUsage::STREAM: return "stream";
        default: return "immutable"; // Default
    }
}

BufferUsage editor::Stream::stringToBufferUsage(const std::string& str) {
    if (str == "immutable") return BufferUsage::IMMUTABLE;
    if (str == "dynamic") return BufferUsage::DYNAMIC;
    if (str == "stream") return BufferUsage::STREAM;
    return BufferUsage::IMMUTABLE; // Default
}

// AttributeType enum conversion
std::string editor::Stream::attributeTypeToString(AttributeType type) {
    switch (type) {
        case AttributeType::INDEX: return "index";
        case AttributeType::POSITION: return "position";
        case AttributeType::TEXCOORD1: return "texcoord1";
        case AttributeType::TEXCOORD2: return "texcoord2";
        case AttributeType::NORMAL: return "normal";
        case AttributeType::TANGENT: return "tangent";
        case AttributeType::COLOR: return "color";
        case AttributeType::POINTSIZE: return "pointsize";
        case AttributeType::POINTROTATION: return "pointrotation";
        case AttributeType::TEXTURERECT: return "texturerect";
        case AttributeType::BONEWEIGHTS: return "boneweights";
        case AttributeType::BONEIDS: return "boneids";
        case AttributeType::MORPHTARGET0: return "morphtarget0";
        case AttributeType::MORPHTARGET1: return "morphtarget1";
        case AttributeType::MORPHTARGET2: return "morphtarget2";
        case AttributeType::MORPHTARGET3: return "morphtarget3";
        case AttributeType::MORPHTARGET4: return "morphtarget4";
        case AttributeType::MORPHTARGET5: return "morphtarget5";
        case AttributeType::MORPHTARGET6: return "morphtarget6";
        case AttributeType::MORPHTARGET7: return "morphtarget7";
        case AttributeType::MORPHNORMAL0: return "morphnormal0";
        case AttributeType::MORPHNORMAL1: return "morphnormal1";
        case AttributeType::MORPHNORMAL2: return "morphnormal2";
        case AttributeType::MORPHNORMAL3: return "morphnormal3";
        case AttributeType::MORPHTANGENT0: return "morphtangent0";
        case AttributeType::MORPHTANGENT1: return "morphtangent1";
        case AttributeType::INSTANCEMATRIXCOL1: return "instancematrixcol1";
        case AttributeType::INSTANCEMATRIXCOL2: return "instancematrixcol2";
        case AttributeType::INSTANCEMATRIXCOL3: return "instancematrixcol3";
        case AttributeType::INSTANCEMATRIXCOL4: return "instancematrixcol4";
        case AttributeType::INSTANCECOLOR: return "instancecolor";
        case AttributeType::INSTANCETEXTURERECT: return "instancetexturerect";
        case AttributeType::TERRAINNODEPOSITION: return "terrainnodeposition";
        case AttributeType::TERRAINNODESIZE: return "terrainnodesize";
        case AttributeType::TERRAINNODERANGE: return "terrainnoderange";
        case AttributeType::TERRAINNODERESOLUTION: return "terrainnoderesolution";
        case AttributeType::SMOOTHNORMAL: return "smoothnormal";
        case AttributeType::VAT_INDEX: return "vatindex";
        case AttributeType::INSTANCEVAT0: return "instancevat0";
        case AttributeType::INSTANCEVAT1: return "instancevat1";
        case AttributeType::LINETANGENTIN: return "linetangentin";
        case AttributeType::LINETANGENTOUT: return "linetangentout";
        case AttributeType::LINEPARAMS: return "lineparams";
        default: return "position"; // Default
    }
}

AttributeType editor::Stream::stringToAttributeType(const std::string& str) {
    if (str == "index") return AttributeType::INDEX;
    if (str == "position") return AttributeType::POSITION;
    if (str == "texcoord1") return AttributeType::TEXCOORD1;
    if (str == "texcoord2") return AttributeType::TEXCOORD2;
    if (str == "normal") return AttributeType::NORMAL;
    if (str == "tangent") return AttributeType::TANGENT;
    if (str == "color") return AttributeType::COLOR;
    if (str == "pointsize") return AttributeType::POINTSIZE;
    if (str == "pointrotation") return AttributeType::POINTROTATION;
    if (str == "texturerect") return AttributeType::TEXTURERECT;
    if (str == "boneweights") return AttributeType::BONEWEIGHTS;
    if (str == "boneids") return AttributeType::BONEIDS;
    if (str == "morphtarget0") return AttributeType::MORPHTARGET0;
    if (str == "morphtarget1") return AttributeType::MORPHTARGET1;
    if (str == "morphtarget2") return AttributeType::MORPHTARGET2;
    if (str == "morphtarget3") return AttributeType::MORPHTARGET3;
    if (str == "morphtarget4") return AttributeType::MORPHTARGET4;
    if (str == "morphtarget5") return AttributeType::MORPHTARGET5;
    if (str == "morphtarget6") return AttributeType::MORPHTARGET6;
    if (str == "morphtarget7") return AttributeType::MORPHTARGET7;
    if (str == "morphnormal0") return AttributeType::MORPHNORMAL0;
    if (str == "morphnormal1") return AttributeType::MORPHNORMAL1;
    if (str == "morphnormal2") return AttributeType::MORPHNORMAL2;
    if (str == "morphnormal3") return AttributeType::MORPHNORMAL3;
    if (str == "morphtangent0") return AttributeType::MORPHTANGENT0;
    if (str == "morphtangent1") return AttributeType::MORPHTANGENT1;
    if (str == "instancematrixcol1") return AttributeType::INSTANCEMATRIXCOL1;
    if (str == "instancematrixcol2") return AttributeType::INSTANCEMATRIXCOL2;
    if (str == "instancematrixcol3") return AttributeType::INSTANCEMATRIXCOL3;
    if (str == "instancematrixcol4") return AttributeType::INSTANCEMATRIXCOL4;
    if (str == "instancecolor") return AttributeType::INSTANCECOLOR;
    if (str == "instancetexturerect") return AttributeType::INSTANCETEXTURERECT;
    if (str == "terrainnodeposition") return AttributeType::TERRAINNODEPOSITION;
    if (str == "terrainnodesize") return AttributeType::TERRAINNODESIZE;
    if (str == "terrainnoderange") return AttributeType::TERRAINNODERANGE;
    if (str == "terrainnoderesolution") return AttributeType::TERRAINNODERESOLUTION;
    if (str == "smoothnormal") return AttributeType::SMOOTHNORMAL;
    if (str == "vatindex") return AttributeType::VAT_INDEX;
    if (str == "instancevat0") return AttributeType::INSTANCEVAT0;
    if (str == "instancevat1") return AttributeType::INSTANCEVAT1;
    if (str == "linetangentin") return AttributeType::LINETANGENTIN;
    if (str == "linetangentout") return AttributeType::LINETANGENTOUT;
    if (str == "lineparams") return AttributeType::LINEPARAMS;
    return AttributeType::POSITION; // Default
}

// AttributeDataType enum conversion
std::string editor::Stream::attributeDataTypeToString(AttributeDataType type) {
    switch (type) {
        case AttributeDataType::BYTE: return "byte";
        case AttributeDataType::UNSIGNED_BYTE: return "unsigned_byte";
        case AttributeDataType::SHORT: return "short";
        case AttributeDataType::UNSIGNED_SHORT: return "unsigned_short";
        case AttributeDataType::INT: return "int";
        case AttributeDataType::UNSIGNED_INT: return "unsigned_int";
        case AttributeDataType::FLOAT: return "float";
        default: return "float"; // Default
    }
}

AttributeDataType editor::Stream::stringToAttributeDataType(const std::string& str) {
    if (str == "byte") return AttributeDataType::BYTE;
    if (str == "unsigned_byte") return AttributeDataType::UNSIGNED_BYTE;
    if (str == "short") return AttributeDataType::SHORT;
    if (str == "unsigned_short") return AttributeDataType::UNSIGNED_SHORT;
    if (str == "int") return AttributeDataType::INT;
    if (str == "unsigned_int") return AttributeDataType::UNSIGNED_INT;
    if (str == "float") return AttributeDataType::FLOAT;
    return AttributeDataType::FLOAT; // Default
}

// CullingMode enum conversion
std::string editor::Stream::cullingModeToString(CullingMode mode) {
    switch (mode) {
        case CullingMode::BACK: return "back";
        case CullingMode::FRONT: return "front";
        default: return "back";
    }
}

CullingMode editor::Stream::stringToCullingMode(const std::string& str) {
    if (str == "back") return CullingMode::BACK;
    if (str == "front") return CullingMode::FRONT;
    return CullingMode::BACK;
}

// WindingOrder enum conversion
std::string editor::Stream::windingOrderToString(WindingOrder order) {
    switch (order) {
        case WindingOrder::CCW: return "ccw";
        case WindingOrder::CW: return "cw";
        default: return "ccw";
    }
}

WindingOrder editor::Stream::stringToWindingOrder(const std::string& str) {
    if (str == "ccw") return WindingOrder::CCW;
    if (str == "cw") return WindingOrder::CW;
    return WindingOrder::CCW;
}

// TextureFilter enum conversion
std::string editor::Stream::textureFilterToString(TextureFilter filter) {
    switch (filter) {
        case TextureFilter::NEAREST: return "nearest";
        case TextureFilter::LINEAR: return "linear";
        case TextureFilter::NEAREST_MIPMAP_NEAREST: return "nearest_mipmap_nearest";
        case TextureFilter::NEAREST_MIPMAP_LINEAR: return "nearest_mipmap_linear";
        case TextureFilter::LINEAR_MIPMAP_NEAREST: return "linear_mipmap_nearest";
        case TextureFilter::LINEAR_MIPMAP_LINEAR: return "linear_mipmap_linear";
        default: return "linear";
    }
}

TextureFilter editor::Stream::stringToTextureFilter(const std::string& str) {
    if (str == "nearest") return TextureFilter::NEAREST;
    if (str == "linear") return TextureFilter::LINEAR;
    if (str == "nearest_mipmap_nearest") return TextureFilter::NEAREST_MIPMAP_NEAREST;
    if (str == "nearest_mipmap_linear") return TextureFilter::NEAREST_MIPMAP_LINEAR;
    if (str == "linear_mipmap_nearest") return TextureFilter::LINEAR_MIPMAP_NEAREST;
    if (str == "linear_mipmap_linear") return TextureFilter::LINEAR_MIPMAP_LINEAR;
    return TextureFilter::LINEAR;
}

// TextureWrap enum conversion
std::string editor::Stream::textureWrapToString(TextureWrap wrap) {
    switch (wrap) {
        case TextureWrap::REPEAT: return "repeat";
        case TextureWrap::MIRRORED_REPEAT: return "mirrored_repeat";
        case TextureWrap::CLAMP_TO_EDGE: return "clamp_to_edge";
        case TextureWrap::CLAMP_TO_BORDER: return "clamp_to_border";
        default: return "repeat";
    }
}

TextureWrap editor::Stream::stringToTextureWrap(const std::string& str) {
    if (str == "repeat") return TextureWrap::REPEAT;
    if (str == "mirrored_repeat") return TextureWrap::MIRRORED_REPEAT;
    if (str == "clamp_to_edge") return TextureWrap::CLAMP_TO_EDGE;
    if (str == "clamp_to_border") return TextureWrap::CLAMP_TO_BORDER;
    return TextureWrap::REPEAT;
}

std::string editor::Stream::textureAlphaBorderToString(TextureAlphaBorder alphaBorder) {
    switch (alphaBorder) {
        case TextureAlphaBorder::FIX: return "fix";
        case TextureAlphaBorder::KEEP: return "keep";
        default: return "auto";
    }
}

TextureAlphaBorder editor::Stream::stringToTextureAlphaBorder(const std::string& str) {
    if (str == "fix") return TextureAlphaBorder::FIX;
    if (str == "keep") return TextureAlphaBorder::KEEP;
    return TextureAlphaBorder::AUTO;
}

std::string editor::Stream::anchorPresetToString(AnchorPreset preset) {
    switch (preset) {
        case AnchorPreset::NONE: return "NONE";
        case AnchorPreset::TOP_LEFT: return "TOP_LEFT";
        case AnchorPreset::TOP_RIGHT: return "TOP_RIGHT";
        case AnchorPreset::BOTTOM_LEFT: return "BOTTOM_LEFT";
        case AnchorPreset::BOTTOM_RIGHT: return "BOTTOM_RIGHT";
        case AnchorPreset::CENTER_LEFT: return "CENTER_LEFT";
        case AnchorPreset::CENTER_TOP: return "CENTER_TOP";
        case AnchorPreset::CENTER_RIGHT: return "CENTER_RIGHT";
        case AnchorPreset::CENTER_BOTTOM: return "CENTER_BOTTOM";
        case AnchorPreset::CENTER: return "CENTER";
        case AnchorPreset::LEFT_WIDE: return "LEFT_WIDE";
        case AnchorPreset::TOP_WIDE: return "TOP_WIDE";
        case AnchorPreset::RIGHT_WIDE: return "RIGHT_WIDE";
        case AnchorPreset::BOTTOM_WIDE: return "BOTTOM_WIDE";
        case AnchorPreset::VERTICAL_CENTER_WIDE: return "VERTICAL_CENTER_WIDE";
        case AnchorPreset::HORIZONTAL_CENTER_WIDE: return "HORIZONTAL_CENTER_WIDE";
        case AnchorPreset::FULL_LAYOUT: return "FULL_LAYOUT";
        default: return "NONE";
    }
}

AnchorPreset editor::Stream::stringToAnchorPreset(const std::string& str) {
    if (str == "NONE") return AnchorPreset::NONE;
    if (str == "TOP_LEFT") return AnchorPreset::TOP_LEFT;
    if (str == "TOP_RIGHT") return AnchorPreset::TOP_RIGHT;
    if (str == "BOTTOM_LEFT") return AnchorPreset::BOTTOM_LEFT;
    if (str == "BOTTOM_RIGHT") return AnchorPreset::BOTTOM_RIGHT;
    if (str == "CENTER_LEFT") return AnchorPreset::CENTER_LEFT;
    if (str == "CENTER_TOP") return AnchorPreset::CENTER_TOP;
    if (str == "CENTER_RIGHT") return AnchorPreset::CENTER_RIGHT;
    if (str == "CENTER_BOTTOM") return AnchorPreset::CENTER_BOTTOM;
    if (str == "CENTER") return AnchorPreset::CENTER;
    if (str == "LEFT_WIDE") return AnchorPreset::LEFT_WIDE;
    if (str == "TOP_WIDE") return AnchorPreset::TOP_WIDE;
    if (str == "RIGHT_WIDE") return AnchorPreset::RIGHT_WIDE;
    if (str == "BOTTOM_WIDE") return AnchorPreset::BOTTOM_WIDE;
    if (str == "VERTICAL_CENTER_WIDE") return AnchorPreset::VERTICAL_CENTER_WIDE;
    if (str == "HORIZONTAL_CENTER_WIDE") return AnchorPreset::HORIZONTAL_CENTER_WIDE;
    if (str == "FULL_LAYOUT") return AnchorPreset::FULL_LAYOUT;
    return AnchorPreset::NONE;
}

std::string editor::Stream::materialAlphaModeToString(MaterialAlphaMode mode) {
    switch (mode) {
        case MaterialAlphaMode::AUTO: return "AUTO";
        case MaterialAlphaMode::ALPHA_OPAQUE: return "ALPHA_OPAQUE";
        case MaterialAlphaMode::MASK: return "MASK";
        case MaterialAlphaMode::BLEND: return "BLEND";
        default: return "AUTO";
    }
}

MaterialAlphaMode editor::Stream::stringToMaterialAlphaMode(const std::string& str) {
    if (str == "AUTO") return MaterialAlphaMode::AUTO;
    if (str == "ALPHA_OPAQUE") return MaterialAlphaMode::ALPHA_OPAQUE;
    if (str == "MASK") return MaterialAlphaMode::MASK;
    if (str == "BLEND") return MaterialAlphaMode::BLEND;
    return MaterialAlphaMode::AUTO;
}

std::string editor::Stream::materialBlendModeToString(MaterialBlendMode mode) {
    switch (mode) {
        case MaterialBlendMode::AUTO: return "AUTO";
        case MaterialBlendMode::NONE: return "NONE";
        case MaterialBlendMode::ALPHA: return "ALPHA";
        case MaterialBlendMode::PREMULTIPLIED: return "PREMULTIPLIED";
        case MaterialBlendMode::ADDITIVE: return "ADDITIVE";
        case MaterialBlendMode::MULTIPLY: return "MULTIPLY";
    }
    return "AUTO";
}

MaterialBlendMode editor::Stream::stringToMaterialBlendMode(const std::string& str) {
    if (str == "NONE" || str == "OPAQUE") return MaterialBlendMode::NONE;
    if (str == "ALPHA") return MaterialBlendMode::ALPHA;
    if (str == "PREMULTIPLIED") return MaterialBlendMode::PREMULTIPLIED;
    if (str == "ADDITIVE") return MaterialBlendMode::ADDITIVE;
    if (str == "MULTIPLY") return MaterialBlendMode::MULTIPLY;
    return MaterialBlendMode::AUTO;
}

std::string editor::Stream::materialCullModeToString(MaterialCullMode mode) {
    switch (mode) {
        case MaterialCullMode::AUTO: return "AUTO";
        case MaterialCullMode::BACK: return "BACK";
        case MaterialCullMode::FRONT: return "FRONT";
        case MaterialCullMode::OFF: return "OFF";
    }
    return "AUTO";
}

MaterialCullMode editor::Stream::stringToMaterialCullMode(const std::string& str) {
    if (str == "BACK") return MaterialCullMode::BACK;
    if (str == "FRONT") return MaterialCullMode::FRONT;
    if (str == "OFF") return MaterialCullMode::OFF;
    return MaterialCullMode::AUTO;
}

std::string editor::Stream::materialDepthWriteToString(MaterialDepthWrite mode) {
    switch (mode) {
        case MaterialDepthWrite::AUTO: return "AUTO";
        case MaterialDepthWrite::ENABLED: return "ENABLED";
        case MaterialDepthWrite::DISABLED: return "DISABLED";
    }
    return "AUTO";
}

MaterialDepthWrite editor::Stream::stringToMaterialDepthWrite(const std::string& str) {
    if (str == "ENABLED") return MaterialDepthWrite::ENABLED;
    if (str == "DISABLED") return MaterialDepthWrite::DISABLED;
    return MaterialDepthWrite::AUTO;
}

std::string editor::Stream::materialDepthTestToString(MaterialDepthTest mode) {
    switch (mode) {
        case MaterialDepthTest::AUTO: return "AUTO";
        case MaterialDepthTest::LESS: return "LESS";
        case MaterialDepthTest::LESS_EQUAL: return "LESS_EQUAL";
        case MaterialDepthTest::EQUAL: return "EQUAL";
        case MaterialDepthTest::GREATER_EQUAL: return "GREATER_EQUAL";
        case MaterialDepthTest::GREATER: return "GREATER";
        case MaterialDepthTest::NOT_EQUAL: return "NOT_EQUAL";
        case MaterialDepthTest::ALWAYS: return "ALWAYS";
    }
    return "AUTO";
}

MaterialDepthTest editor::Stream::stringToMaterialDepthTest(const std::string& str) {
    if (str == "LESS") return MaterialDepthTest::LESS;
    if (str == "LESS_EQUAL") return MaterialDepthTest::LESS_EQUAL;
    if (str == "EQUAL") return MaterialDepthTest::EQUAL;
    if (str == "GREATER_EQUAL") return MaterialDepthTest::GREATER_EQUAL;
    if (str == "GREATER") return MaterialDepthTest::GREATER;
    if (str == "NOT_EQUAL") return MaterialDepthTest::NOT_EQUAL;
    if (str == "ALWAYS") return MaterialDepthTest::ALWAYS;
    return MaterialDepthTest::AUTO;
}

std::string editor::Stream::materialLightingToString(MaterialLighting mode) {
    switch (mode) {
        case MaterialLighting::AUTO: return "AUTO";
        case MaterialLighting::PIXEL: return "PIXEL";
        case MaterialLighting::VERTEX: return "VERTEX";
        case MaterialLighting::MIXED: return "MIXED";
        case MaterialLighting::NONE: return "NONE";
    }
    return "AUTO";
}

MaterialLighting editor::Stream::stringToMaterialLighting(const std::string& str) {
    if (str == "PIXEL") return MaterialLighting::PIXEL;
    if (str == "VERTEX") return MaterialLighting::VERTEX;
    if (str == "MIXED") return MaterialLighting::MIXED;
    if (str == "NONE") return MaterialLighting::NONE;
    return MaterialLighting::AUTO;
}

std::string editor::Stream::lightShadingToString(LightShading shading) {
    switch (shading) {
        case LightShading::BOTH: return "BOTH";
        case LightShading::PIXEL: return "PIXEL";
        case LightShading::VERTEX: return "VERTEX";
    }
    return "BOTH";
}

LightShading editor::Stream::stringToLightShading(const std::string& str) {
    if (str == "PIXEL") return LightShading::PIXEL;
    if (str == "VERTEX") return LightShading::VERTEX;
    return LightShading::BOTH;
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
    if (blendMode) *blendMode = node["blendMode"] ? stringToMaterialBlendMode(node["blendMode"].as<std::string>()) : MaterialBlendMode::AUTO;
    if (depthWrite) *depthWrite = node["depthWrite"] ? stringToMaterialDepthWrite(node["depthWrite"].as<std::string>()) : MaterialDepthWrite::AUTO;
}

std::string editor::Stream::materialStencilCompareToString(MaterialStencilCompare compare) {
    switch (compare) {
        case MaterialStencilCompare::AUTO: return "AUTO";
        case MaterialStencilCompare::DISABLED: return "DISABLED";
        case MaterialStencilCompare::NEVER: return "NEVER";
        case MaterialStencilCompare::LESS: return "LESS";
        case MaterialStencilCompare::EQUAL: return "EQUAL";
        case MaterialStencilCompare::LESS_EQUAL: return "LESS_EQUAL";
        case MaterialStencilCompare::GREATER: return "GREATER";
        case MaterialStencilCompare::NOT_EQUAL: return "NOT_EQUAL";
        case MaterialStencilCompare::GREATER_EQUAL: return "GREATER_EQUAL";
        case MaterialStencilCompare::ALWAYS: return "ALWAYS";
    }
    return "AUTO";
}

MaterialStencilCompare editor::Stream::stringToMaterialStencilCompare(const std::string& str) {
    if (str == "DISABLED") return MaterialStencilCompare::DISABLED;
    if (str == "NEVER") return MaterialStencilCompare::NEVER;
    if (str == "LESS") return MaterialStencilCompare::LESS;
    if (str == "EQUAL") return MaterialStencilCompare::EQUAL;
    if (str == "LESS_EQUAL") return MaterialStencilCompare::LESS_EQUAL;
    if (str == "GREATER") return MaterialStencilCompare::GREATER;
    if (str == "NOT_EQUAL") return MaterialStencilCompare::NOT_EQUAL;
    if (str == "GREATER_EQUAL") return MaterialStencilCompare::GREATER_EQUAL;
    if (str == "ALWAYS") return MaterialStencilCompare::ALWAYS;
    return MaterialStencilCompare::AUTO;
}

std::string editor::Stream::materialStencilOpToString(MaterialStencilOp op) {
    switch (op) {
        case MaterialStencilOp::KEEP: return "KEEP";
        case MaterialStencilOp::ZERO: return "ZERO";
        case MaterialStencilOp::REPLACE: return "REPLACE";
        case MaterialStencilOp::INCR_CLAMP: return "INCR_CLAMP";
        case MaterialStencilOp::DECR_CLAMP: return "DECR_CLAMP";
        case MaterialStencilOp::INVERT: return "INVERT";
        case MaterialStencilOp::INCR_WRAP: return "INCR_WRAP";
        case MaterialStencilOp::DECR_WRAP: return "DECR_WRAP";
    }
    return "KEEP";
}

MaterialStencilOp editor::Stream::stringToMaterialStencilOp(const std::string& str) {
    if (str == "ZERO") return MaterialStencilOp::ZERO;
    if (str == "REPLACE") return MaterialStencilOp::REPLACE;
    if (str == "INCR_CLAMP") return MaterialStencilOp::INCR_CLAMP;
    if (str == "DECR_CLAMP") return MaterialStencilOp::DECR_CLAMP;
    if (str == "INVERT") return MaterialStencilOp::INVERT;
    if (str == "INCR_WRAP") return MaterialStencilOp::INCR_WRAP;
    if (str == "DECR_WRAP") return MaterialStencilOp::DECR_WRAP;
    return MaterialStencilOp::KEEP;
}

std::string editor::Stream::materialPassOrderToString(MaterialPassOrder order) {
    return order == MaterialPassOrder::BEFORE ? "BEFORE" : "AFTER";
}

MaterialPassOrder editor::Stream::stringToMaterialPassOrder(const std::string& str) {
    return str == "BEFORE" ? MaterialPassOrder::BEFORE : MaterialPassOrder::AFTER;
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
    // not merged: encodeStencil omits the defaults
    MaterialStencil stencil;
    if (!node || !node.IsMap()) return stencil;
    if (node["compare"]) stencil.compare = stringToMaterialStencilCompare(node["compare"].as<std::string>());
    if (node["ref"]) stencil.ref = YamlNumbers::as<int>(node["ref"]);
    if (node["readMask"]) stencil.readMask = YamlNumbers::as<int>(node["readMask"]);
    if (node["writeMask"]) stencil.writeMask = YamlNumbers::as<int>(node["writeMask"]);
    if (node["pass"]) stencil.pass = stringToMaterialStencilOp(node["pass"].as<std::string>());
    if (node["fail"]) stencil.fail = stringToMaterialStencilOp(node["fail"].as<std::string>());
    if (node["depthFail"]) stencil.depthFail = stringToMaterialStencilOp(node["depthFail"].as<std::string>());
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
    if (!pass.shader.empty()) node["shader"] = pass.shader;
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
    // not merged: encodeMaterialPass omits the defaults
    MaterialPass pass;
    if (!node || !node.IsMap()) return pass;
    pass.shader = node["shader"] ? node["shader"].as<std::string>() : std::string();
    pass.shaderParams = decodeShaderUniforms(node["shaderParams"]);
    pass.shaderTextures = decodeShaderTextures(node["shaderTextures"]);
    pass.shaderFeatures = decodeShaderFeatures(node["shaderFeatures"]);
    if (node["blendMode"]) pass.blendMode = stringToMaterialBlendMode(node["blendMode"].as<std::string>());
    if (node["cullMode"]) pass.cullMode = stringToMaterialCullMode(node["cullMode"].as<std::string>());
    if (node["depthWrite"]) pass.depthWrite = stringToMaterialDepthWrite(node["depthWrite"].as<std::string>());
    if (node["depthTest"]) pass.depthTest = stringToMaterialDepthTest(node["depthTest"].as<std::string>());
    pass.depthOffsetFactor = decodeFinite(node["depthOffsetFactor"], 0.0f);
    pass.depthOffsetUnits = decodeFinite(node["depthOffsetUnits"], 0.0f);
    decodeStencilAndColorWrite(node, pass.stencil, pass.colorWrite);
    if (node["order"]) pass.order = stringToMaterialPassOrder(node["order"].as<std::string>());
    return pass;
}

YAML::Node editor::Stream::encodeShaderTextures(const std::vector<std::pair<std::string, Texture>>& textures, bool embedData) {
    YAML::Node node(YAML::NodeType::Map);
    for (const auto& [uniform, texture] : textures) {
        // an empty one binds the manifest default, like no entry
        if (texture.empty() && !CameraTextureLink::isLinkTexture(texture)) continue;
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
    switch (type) {
        case ScriptPropertyType::Bool: return "Bool";
        case ScriptPropertyType::Int: return "Int";
        case ScriptPropertyType::Float: return "Float";
        case ScriptPropertyType::String: return "String";
        case ScriptPropertyType::Vector2: return "Vector2";
        case ScriptPropertyType::Vector3: return "Vector3";
        case ScriptPropertyType::Vector4: return "Vector4";
        case ScriptPropertyType::Color3: return "Color3";
        case ScriptPropertyType::Color4: return "Color4";
        case ScriptPropertyType::EntityReference: return "EntityReference";
        case ScriptPropertyType::Prefab: return "Prefab";
        default: return "Bool";
    }
}

ScriptPropertyType editor::Stream::stringToScriptPropertyType(const std::string& str) {
    if (str == "Bool") return ScriptPropertyType::Bool;
    if (str == "Int") return ScriptPropertyType::Int;
    if (str == "Float") return ScriptPropertyType::Float;
    if (str == "String") return ScriptPropertyType::String;
    if (str == "Vector2") return ScriptPropertyType::Vector2;
    if (str == "Vector3") return ScriptPropertyType::Vector3;
    if (str == "Vector4") return ScriptPropertyType::Vector4;
    if (str == "Color3") return ScriptPropertyType::Color3;
    if (str == "Color4") return ScriptPropertyType::Color4;
    if (str == "EntityReference") return ScriptPropertyType::EntityReference;
    if (str == "Prefab") return ScriptPropertyType::Prefab;
    return ScriptPropertyType::Bool;
}

std::string editor::Stream::particleEmitterShapeToString(ParticleEmitterShape shape) {
    switch (shape) {
        case ParticleEmitterShape::Box: return "Box";
        case ParticleEmitterShape::Sphere: return "Sphere";
        case ParticleEmitterShape::Hemisphere: return "Hemisphere";
        case ParticleEmitterShape::Circle: return "Circle";
        case ParticleEmitterShape::Cone: return "Cone";
        default: return "Box";
    }
}

ParticleEmitterShape editor::Stream::stringToParticleEmitterShape(const std::string& str) {
    if (str == "Box") return ParticleEmitterShape::Box;
    if (str == "Sphere") return ParticleEmitterShape::Sphere;
    if (str == "Hemisphere") return ParticleEmitterShape::Hemisphere;
    if (str == "Circle") return ParticleEmitterShape::Circle;
    if (str == "Cone") return ParticleEmitterShape::Cone;
    return ParticleEmitterShape::Box;
}

namespace {

std::string terrainBrushModeToString(editor::TerrainBrushMode mode) {
    switch (mode) {
        case editor::TerrainBrushMode::Raise: return "Raise";
        case editor::TerrainBrushMode::Lower: return "Lower";
        case editor::TerrainBrushMode::Smooth: return "Smooth";
        case editor::TerrainBrushMode::Flatten: return "Flatten";
        case editor::TerrainBrushMode::Sharpen: return "Sharpen";
        case editor::TerrainBrushMode::Noise: return "Noise";
        case editor::TerrainBrushMode::Terrace: return "Terrace";
        case editor::TerrainBrushMode::Stamp: return "Stamp";
        case editor::TerrainBrushMode::Erode: return "Erode";
        case editor::TerrainBrushMode::Ramp: return "Ramp";
        case editor::TerrainBrushMode::PaintBase: return "PaintBase";
        case editor::TerrainBrushMode::PaintLayer: return "PaintLayer";
        case editor::TerrainBrushMode::PaintDensity: return "PaintDensity";
        case editor::TerrainBrushMode::EraseDensity: return "EraseDensity";
        case editor::TerrainBrushMode::PlaceObject: return "PlaceObject";
        case editor::TerrainBrushMode::EraseObject: return "EraseObject";
        default: return "Raise";
    }
}

editor::TerrainBrushMode stringToTerrainBrushMode(const std::string& str) {
    if (str == "Raise") return editor::TerrainBrushMode::Raise;
    if (str == "Lower") return editor::TerrainBrushMode::Lower;
    if (str == "Smooth") return editor::TerrainBrushMode::Smooth;
    if (str == "Flatten") return editor::TerrainBrushMode::Flatten;
    if (str == "Sharpen") return editor::TerrainBrushMode::Sharpen;
    if (str == "Noise") return editor::TerrainBrushMode::Noise;
    if (str == "Terrace") return editor::TerrainBrushMode::Terrace;
    if (str == "Stamp") return editor::TerrainBrushMode::Stamp;
    if (str == "Erode") return editor::TerrainBrushMode::Erode;
    if (str == "Ramp") return editor::TerrainBrushMode::Ramp;
    if (str == "PaintBase") return editor::TerrainBrushMode::PaintBase;
    if (str == "PaintLayer") return editor::TerrainBrushMode::PaintLayer;
    // Backward compatibility: the three channel brushes became one layer brush with an
    // index. Remove this branch once those editor versions are no longer supported.
    if (str == "PaintRed" || str == "PaintGreen" || str == "PaintBlue") return editor::TerrainBrushMode::PaintLayer;
    if (str == "PaintDensity") return editor::TerrainBrushMode::PaintDensity;
    if (str == "EraseDensity") return editor::TerrainBrushMode::EraseDensity;
    if (str == "PlaceObject") return editor::TerrainBrushMode::PlaceObject;
    if (str == "EraseObject") return editor::TerrainBrushMode::EraseObject;
    return editor::TerrainBrushMode::Raise;
}

std::string terrainBrushShapeToString(editor::TerrainBrushShape shape) {
    switch (shape) {
        case editor::TerrainBrushShape::Circle: return "Circle";
        case editor::TerrainBrushShape::Square: return "Square";
        default: return "Circle";
    }
}

editor::TerrainBrushShape stringToTerrainBrushShape(const std::string& str) {
    if (str == "Circle") return editor::TerrainBrushShape::Circle;
    if (str == "Square") return editor::TerrainBrushShape::Square;
    return editor::TerrainBrushShape::Circle;
}

std::string terrainBrushFalloffToString(editor::TerrainBrushFalloff falloff) {
    switch (falloff) {
        case editor::TerrainBrushFalloff::Smooth: return "Smooth";
        case editor::TerrainBrushFalloff::Linear: return "Linear";
        case editor::TerrainBrushFalloff::Constant: return "Constant";
        default: return "Smooth";
    }
}

editor::TerrainBrushFalloff stringToTerrainBrushFalloff(const std::string& str) {
    if (str == "Smooth") return editor::TerrainBrushFalloff::Smooth;
    if (str == "Linear") return editor::TerrainBrushFalloff::Linear;
    if (str == "Constant") return editor::TerrainBrushFalloff::Constant;
    return editor::TerrainBrushFalloff::Smooth;
}

} // namespace

std::string editor::Stream::layoutGroupTypeToString(LayoutGroupType type) {
    switch (type) {
        case LayoutGroupType::VERTICAL: return "vertical";
        case LayoutGroupType::HORIZONTAL: return "horizontal";
        case LayoutGroupType::VERTICAL_WRAP: return "vertical_wrap";
        case LayoutGroupType::HORIZONTAL_WRAP: return "horizontal_wrap";
        default: return "vertical";
    }
}

LayoutGroupType editor::Stream::stringToLayoutGroupType(const std::string& str) {
    if (str == "vertical") return LayoutGroupType::VERTICAL;
    if (str == "horizontal") return LayoutGroupType::HORIZONTAL;
    if (str == "vertical_wrap") return LayoutGroupType::VERTICAL_WRAP;
    if (str == "horizontal_wrap") return LayoutGroupType::HORIZONTAL_WRAP;
    return LayoutGroupType::VERTICAL;
}

std::string editor::Stream::scrollbarTypeToString(ScrollbarType type) {
    switch (type) {
        case ScrollbarType::VERTICAL: return "vertical";
        case ScrollbarType::HORIZONTAL: return "horizontal";
        default: return "vertical";
    }
}

ScrollbarType editor::Stream::stringToScrollbarType(const std::string& str) {
    if (str == "horizontal") return ScrollbarType::HORIZONTAL;
    return ScrollbarType::VERTICAL;
}

std::string editor::Stream::progressbarTypeToString(ProgressbarType type) {
    switch (type) {
        case ProgressbarType::VERTICAL: return "vertical";
        case ProgressbarType::HORIZONTAL: return "horizontal";
        default: return "horizontal";
    }
}

ProgressbarType editor::Stream::stringToProgressbarType(const std::string& str) {
    if (str == "vertical") return ProgressbarType::VERTICAL;
    return ProgressbarType::HORIZONTAL;
}

std::string editor::Stream::scalingModeToString(Scaling mode) {
    switch (mode) {
        case Scaling::FITWIDTH: return "fitwidth";
        case Scaling::FITHEIGHT: return "fitheight";
        case Scaling::LETTERBOX: return "letterbox";
        case Scaling::CROP: return "crop";
        case Scaling::STRETCH: return "stretch";
        case Scaling::NATIVE: return "native";
        default: return "fitwidth";
    }
}

Scaling editor::Stream::stringToScalingMode(const std::string& str) {
    if (str == "fitwidth") return Scaling::FITWIDTH;
    if (str == "fitheight") return Scaling::FITHEIGHT;
    if (str == "letterbox") return Scaling::LETTERBOX;
    if (str == "crop") return Scaling::CROP;
    if (str == "stretch") return Scaling::STRETCH;
    if (str == "native") return Scaling::NATIVE;
    return Scaling::FITWIDTH;
}

std::string editor::Stream::textureStrategyToString(TextureStrategy strategy) {
    switch (strategy) {
        case TextureStrategy::FIT: return "fit";
        case TextureStrategy::RESIZE: return "resize";
        case TextureStrategy::NONE: return "none";
        default: return "none";
    }
}

TextureStrategy editor::Stream::stringToTextureStrategy(const std::string& str) {
    if (str == "fit") return TextureStrategy::FIT;
    if (str == "resize") return TextureStrategy::RESIZE;
    return TextureStrategy::NONE;
}

std::string editor::Stream::windowModeToString(WindowMode mode) {
    switch (mode) {
        case WindowMode::WINDOWED: return "windowed";
        case WindowMode::MAXIMIZED: return "maximized";
        case WindowMode::FULLSCREEN: return "fullscreen";
        default: return "windowed";
    }
}

editor::WindowMode editor::Stream::stringToWindowMode(const std::string& str) {
    if (str == "windowed") return WindowMode::WINDOWED;
    if (str == "maximized") return WindowMode::MAXIMIZED;
    if (str == "fullscreen") return WindowMode::FULLSCREEN;
    return WindowMode::WINDOWED;
}

std::string editor::Stream::lightTypeToString(LightType type) {
    switch (type) {
        case LightType::DIRECTIONAL: return "directional";
        case LightType::POINT: return "point";
        case LightType::SPOT: return "spot";
        default: return "directional";
    }
}

LightType editor::Stream::stringToLightType(const std::string& str) {
    if (str == "directional") return LightType::DIRECTIONAL;
    if (str == "point") return LightType::POINT;
    if (str == "spot") return LightType::SPOT;
    return LightType::DIRECTIONAL; // Default
}

std::string editor::Stream::fogTypeToString(FogType type) {
    switch (type) {
        case FogType::LINEAR: return "linear";
        case FogType::EXPONENTIAL: return "exponential";
        case FogType::EXPONENTIALSQUARED: return "exponential_squared";
        default: return "linear";
    }
}

FogType editor::Stream::stringToFogType(const std::string& str) {
    if (str == "linear") return FogType::LINEAR;
    if (str == "exponential") return FogType::EXPONENTIAL;
    if (str == "exponential_squared" || str == "exponentialsquared" || str == "exponentialSquared") return FogType::EXPONENTIALSQUARED;
    return FogType::LINEAR;
}

static std::string shadowQualityToString(ShadowQuality quality) {
    switch (quality) {
        case ShadowQuality::NONE: return "none";
        case ShadowQuality::LOW: return "low";
        case ShadowQuality::MEDIUM: return "medium";
        case ShadowQuality::HIGH: return "high";
    }
    return "low";
}

static ShadowQuality stringToShadowQuality(const std::string& str) {
    if (str == "none") return ShadowQuality::NONE;
    if (str == "medium") return ShadowQuality::MEDIUM;
    if (str == "high") return ShadowQuality::HIGH;
    return ShadowQuality::LOW; // Default
}

static std::string sceneColorScaleToString(SceneColorScale scale) {
    switch (scale) {
        case SceneColorScale::FULL: return "full";
        case SceneColorScale::HALF: return "half";
    }
    return "half";
}

static SceneColorScale stringToSceneColorScale(const std::string& str) {
    if (str == "full") return SceneColorScale::FULL;
    return SceneColorScale::HALF; // Default
}

std::string editor::Stream::lightStateToString(LightState state) {
    switch (state) {
        case LightState::OFF: return "off";
        case LightState::ON: return "on";
        case LightState::AUTO: return "auto";
        default: return "auto";
    }
}

LightState editor::Stream::stringToLightState(const std::string& str) {
    if (str == "off") return LightState::OFF;
    if (str == "on") return LightState::ON;
    if (str == "auto") return LightState::AUTO;
    return LightState::AUTO; // Default
}

std::string editor::Stream::audioSourceStateToString(AudioSourceState state) {
    switch (state) {
        case AudioSourceState::Playing: return "playing";
        case AudioSourceState::Paused: return "paused";
        case AudioSourceState::Stopped: return "stopped";
        default: return "stopped";
    }
}

AudioSourceState editor::Stream::stringToAudioSourceState(const std::string& str) {
    if (str == "playing") return AudioSourceState::Playing;
    if (str == "paused") return AudioSourceState::Paused;
    if (str == "stopped") return AudioSourceState::Stopped;
    return AudioSourceState::Stopped;
}

std::string editor::Stream::uiDocumentRenderModeToString(UIDocumentRenderMode mode) {
    switch (mode) {
        case UIDocumentRenderMode::TEXTURE: return "texture";
        case UIDocumentRenderMode::SCREEN:
        default: return "screen";
    }
}

UIDocumentRenderMode editor::Stream::stringToUIDocumentRenderMode(const std::string& str) {
    if (str == "texture") return UIDocumentRenderMode::TEXTURE;
    return UIDocumentRenderMode::SCREEN;
}

std::string editor::Stream::fluidOutputToString(FluidOutput output) {
    switch (output) {
        case FluidOutput::SCREEN: return "screen";
        case FluidOutput::TEXTURE: return "texture";
        case FluidOutput::MATERIAL:
        default: return "material";
    }
}

FluidOutput editor::Stream::stringToFluidOutput(const std::string& str) {
    if (str == "screen") return FluidOutput::SCREEN;
    if (str == "texture") return FluidOutput::TEXTURE;
    return FluidOutput::MATERIAL;
}

std::string editor::Stream::latticeInterpolationToString(LatticeInterpolation interpolation) {
    switch (interpolation) {
        case LatticeInterpolation::SHARP: return "sharp";
        case LatticeInterpolation::SMOOTH: return "smooth";
        case LatticeInterpolation::CUBIC:
        default: return "cubic";
    }
}

LatticeInterpolation editor::Stream::stringToLatticeInterpolation(const std::string& str) {
    if (str == "sharp") return LatticeInterpolation::SHARP;
    if (str == "smooth") return LatticeInterpolation::SMOOTH;
    return LatticeInterpolation::CUBIC;
}

std::string editor::Stream::latticeOutsideToString(LatticeOutside outside) {
    switch (outside) {
        case LatticeOutside::EXTEND: return "extend";
        case LatticeOutside::FADE:
        default: return "fade";
    }
}

LatticeOutside editor::Stream::stringToLatticeOutside(const std::string& str) {
    if (str == "extend") return LatticeOutside::EXTEND;
    return LatticeOutside::FADE;
}

std::string editor::Stream::latticeJiggleAnchorToString(LatticeJiggleAnchor anchor) {
    switch (anchor) {
        case LatticeJiggleAnchor::NEGATIVE_X: return "negative_x";
        case LatticeJiggleAnchor::POSITIVE_X: return "positive_x";
        case LatticeJiggleAnchor::NEGATIVE_Y: return "negative_y";
        case LatticeJiggleAnchor::POSITIVE_Y: return "positive_y";
        case LatticeJiggleAnchor::NEGATIVE_Z: return "negative_z";
        case LatticeJiggleAnchor::POSITIVE_Z: return "positive_z";
        case LatticeJiggleAnchor::NONE:
        default: return "none";
    }
}

LatticeJiggleAnchor editor::Stream::stringToLatticeJiggleAnchor(const std::string& str) {
    if (str == "negative_x") return LatticeJiggleAnchor::NEGATIVE_X;
    if (str == "positive_x") return LatticeJiggleAnchor::POSITIVE_X;
    if (str == "negative_y") return LatticeJiggleAnchor::NEGATIVE_Y;
    if (str == "positive_y") return LatticeJiggleAnchor::POSITIVE_Y;
    if (str == "negative_z") return LatticeJiggleAnchor::NEGATIVE_Z;
    if (str == "positive_z") return LatticeJiggleAnchor::POSITIVE_Z;
    return LatticeJiggleAnchor::NONE;
}

std::string editor::Stream::latticeStageToString(LatticeStage stage) {
    switch (stage) {
        case LatticeStage::BEFORE_SKINNING: return "before_skinning";
        case LatticeStage::AFTER_SKINNING:
        default: return "after_skinning";
    }
}

LatticeStage editor::Stream::stringToLatticeStage(const std::string& str) {
    if (str == "before_skinning") return LatticeStage::BEFORE_SKINNING;
    return LatticeStage::AFTER_SKINNING;
}

std::string editor::Stream::latticeMaskChannelToString(LatticeMaskChannel mask) {
    switch (mask) {
        case LatticeMaskChannel::RED: return "red";
        case LatticeMaskChannel::GREEN: return "green";
        case LatticeMaskChannel::BLUE: return "blue";
        case LatticeMaskChannel::ALPHA: return "alpha";
        case LatticeMaskChannel::INVERTED_RED: return "inverted_red";
        case LatticeMaskChannel::INVERTED_GREEN: return "inverted_green";
        case LatticeMaskChannel::INVERTED_BLUE: return "inverted_blue";
        case LatticeMaskChannel::INVERTED_ALPHA: return "inverted_alpha";
        case LatticeMaskChannel::NONE:
        default: return "none";
    }
}

LatticeMaskChannel editor::Stream::stringToLatticeMaskChannel(const std::string& str) {
    if (str == "red") return LatticeMaskChannel::RED;
    if (str == "green") return LatticeMaskChannel::GREEN;
    if (str == "blue") return LatticeMaskChannel::BLUE;
    if (str == "alpha") return LatticeMaskChannel::ALPHA;
    if (str == "inverted_red") return LatticeMaskChannel::INVERTED_RED;
    if (str == "inverted_green") return LatticeMaskChannel::INVERTED_GREEN;
    if (str == "inverted_blue") return LatticeMaskChannel::INVERTED_BLUE;
    if (str == "inverted_alpha") return LatticeMaskChannel::INVERTED_ALPHA;
    return LatticeMaskChannel::NONE;
}

std::string editor::Stream::audioSourceAttenuationToString(AudioSourceAttenuation attenuation) {
    switch (attenuation) {
        case AudioSourceAttenuation::NO_ATTENUATION: return "no_attenuation";
        case AudioSourceAttenuation::INVERSE_DISTANCE: return "inverse_distance";
        case AudioSourceAttenuation::LINEAR_DISTANCE: return "linear_distance";
        case AudioSourceAttenuation::EXPONENTIAL_DISTANCE: return "exponential_distance";
        default: return "no_attenuation";
    }
}

AudioSourceAttenuation editor::Stream::stringToAudioSourceAttenuation(const std::string& str) {
    if (str == "no_attenuation") return AudioSourceAttenuation::NO_ATTENUATION;
    if (str == "inverse_distance") return AudioSourceAttenuation::INVERSE_DISTANCE;
    if (str == "linear_distance") return AudioSourceAttenuation::LINEAR_DISTANCE;
    if (str == "exponential_distance") return AudioSourceAttenuation::EXPONENTIAL_DISTANCE;
    return AudioSourceAttenuation::NO_ATTENUATION;
}

std::string editor::Stream::uiEventStateToString(UIEventState state) {
    switch (state) {
        case UIEventState::NOT_SET: return "not_set";
        case UIEventState::ENABLED: return "true";
        case UIEventState::DISABLED: return "false";
        default: return "not_set";
    }
}

UIEventState editor::Stream::stringToUIEventState(const std::string& str) {
    if (str == "not_set") return UIEventState::NOT_SET;
    if (str == "true") return UIEventState::ENABLED;
    if (str == "false") return UIEventState::DISABLED;
    return UIEventState::NOT_SET; // Default
}

std::string editor::Stream::cameraTypeToString(CameraType type) {
    switch (type) {
        case CameraType::CAMERA_UI: return "camera_ui";
        case CameraType::CAMERA_ORTHO: return "camera_ortho";
        case CameraType::CAMERA_PERSPECTIVE: return "camera_perspective";
        default: return "camera_perspective";
    }
}

CameraType editor::Stream::stringToCameraType(const std::string& str) {
    if (str == "camera_ui") return CameraType::CAMERA_UI;
    if (str == "camera_ortho") return CameraType::CAMERA_ORTHO;
    if (str == "camera_perspective") return CameraType::CAMERA_PERSPECTIVE;
    return CameraType::CAMERA_PERSPECTIVE;
}

std::string editor::Stream::pivotPresetToString(PivotPreset preset) {
    switch (preset) {
        case PivotPreset::CENTER:
            return "center";
        case PivotPreset::TOP_CENTER:
            return "top_center";
        case PivotPreset::BOTTOM_CENTER:
            return "bottom_center";
        case PivotPreset::LEFT_CENTER:
            return "left_center";
        case PivotPreset::RIGHT_CENTER:
            return "right_center";
        case PivotPreset::TOP_LEFT:
            return "top_left";
        case PivotPreset::BOTTOM_LEFT:
            return "bottom_left";
        case PivotPreset::TOP_RIGHT:
            return "top_right";
        case PivotPreset::BOTTOM_RIGHT:
            return "bottom_right";
        default:
            return "bottom_left";
    }
}

PivotPreset editor::Stream::stringToPivotPreset(const std::string& str) {
    if (str == "center") return PivotPreset::CENTER;
    if (str == "top_center") return PivotPreset::TOP_CENTER;
    if (str == "bottom_center") return PivotPreset::BOTTOM_CENTER;
    if (str == "left_center") return PivotPreset::LEFT_CENTER;
    if (str == "right_center") return PivotPreset::RIGHT_CENTER;
    if (str == "top_left") return PivotPreset::TOP_LEFT;
    if (str == "bottom_left") return PivotPreset::BOTTOM_LEFT;
    if (str == "top_right") return PivotPreset::TOP_RIGHT;
    if (str == "bottom_right") return PivotPreset::BOTTOM_RIGHT;
    return PivotPreset::BOTTOM_LEFT;
}

std::string editor::Stream::scriptTypeToString(ScriptType type) {
    switch (type) {
        case ScriptType::CPP: return "cpp";
        case ScriptType::LUA: return "lua";
        default:              return "cpp";
    }
}

ScriptType editor::Stream::stringToScriptType(const std::string& str) {
    if (str == "lua") return ScriptType::LUA;
    if (str == "cpp") return ScriptType::CPP;

    // Backward compatibility: projects saved before ScriptType was reduced to
    // CPP/LUA used these three values. Remove these branches once support for
    // scenes written by those editor versions is intentionally dropped.
    if (str == "script_lua") return ScriptType::LUA;
    if (str == "subclass" || str == "script_class") return ScriptType::CPP;

    return ScriptType::CPP;
}

YAML::Node editor::Stream::encodeVector2(const Vector2& vec){
    YAML::Node node;
    node.SetStyle(YAML::EmitterStyle::Flow);
    node[0] = YamlNumbers::text(finiteOr(vec.x));
    node[1] = YamlNumbers::text(finiteOr(vec.y));
    return node;
}

Vector2 editor::Stream::decodeVector2(const YAML::Node& node) {
    if (!node || !node.IsSequence() || node.size() < 2) {
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
    if (!node || !node.IsSequence() || node.size() < 3) {
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
    if (!node || !node.IsSequence() || node.size() < 4) {
        return Vector4();
    }

    return Vector4(decodeFinite(node[0], 0.0f), decodeFinite(node[1], 0.0f), decodeFinite(node[2], 0.0f), decodeFinite(node[3], 0.0f));
}

std::string editor::Stream::curveWrapToString(CurveWrap wrap) {
    switch (wrap) {
        case CurveWrap::Clamp: return "clamp";
        case CurveWrap::Loop: return "loop";
        case CurveWrap::PingPong: return "ping_pong";
    }
    return "clamp";
}

CurveWrap editor::Stream::stringToCurveWrap(const std::string& str) {
    if (str == "loop" || str == "repeat") return CurveWrap::Loop;
    if (str == "ping_pong" || str == "pingpong" || str == "pingPong") return CurveWrap::PingPong;
    return CurveWrap::Clamp;
}

YAML::Node editor::Stream::encodeCurve(const AnimationCurve& curve) {
    YAML::Node node;
    YAML::Node keys(YAML::NodeType::Sequence);
    for (const CurveKey& key : curve.keys) {
        YAML::Node keyNode;
        keyNode.SetStyle(YAML::EmitterStyle::Flow);
        keyNode.push_back(YamlNumbers::text(key.time));
        keyNode.push_back(YamlNumbers::text(key.value));
        if (key.inTangent != 0.0f || key.outTangent != 0.0f) {
            keyNode.push_back(YamlNumbers::text(key.inTangent));
            keyNode.push_back(YamlNumbers::text(key.outTangent));
        }
        keys.push_back(keyNode);
    }
    node["keys"] = keys;
    if (curve.preWrap != CurveWrap::Clamp) node["pre"] = curveWrapToString(curve.preWrap);
    if (curve.postWrap != CurveWrap::Clamp) node["post"] = curveWrapToString(curve.postWrap);
    return node;
}

AnimationCurve editor::Stream::decodeCurve(const YAML::Node& node, const AnimationCurve& fallback) {
    if (!node || node.IsNull()) return fallback;
    // a number: a constant curve
    if (node.IsScalar()) {
        try {
            return AnimationCurve::constant(node.as<float>());
        } catch (const YAML::Exception&) {
            return fallback;
        }
    }
    AnimationCurve curve;
    YAML::Node keys = node.IsSequence() ? node : node["keys"];
    if (keys && keys.IsSequence()) {
        for (const YAML::Node& keyNode : keys) {
            CurveKey key;
            try {
                if (keyNode.IsSequence() && keyNode.size() >= 2) {
                    key.time = keyNode[0].as<float>();
                    key.value = keyNode[1].as<float>();
                    if (keyNode.size() >= 4) {
                        key.inTangent = keyNode[2].as<float>();
                        key.outTangent = keyNode[3].as<float>();
                    }
                } else if (keyNode.IsMap()) {
                    key.time = keyNode["time"] ? keyNode["time"].as<float>() : 0.0f;
                    key.value = keyNode["value"] ? keyNode["value"].as<float>() : 0.0f;
                    if (keyNode["inTangent"]) key.inTangent = keyNode["inTangent"].as<float>();
                    if (keyNode["outTangent"]) key.outTangent = keyNode["outTangent"].as<float>();
                } else {
                    continue;
                }
            } catch (const YAML::Exception&) {
                continue;
            }
            curve.keys.push_back(key);
        }
    }
    if (node.IsMap()) {
        // a wrap that is not a name: the default (clamp), with a warning
        for (const char* side : {"pre", "post"}) {
            const YAML::Node wrap = node[side];
            if (!wrap) continue;
            if (!wrap.IsScalar()) {
                Out::warning("Curve: %s is not a wrap mode name (clamp, loop, ping_pong): clamp", side);
                continue;
            }
            (std::string(side) == "pre" ? curve.preWrap : curve.postWrap) = stringToCurveWrap(wrap.Scalar());
        }
    }
    curve.normalize();
    return curve;
}

bool editor::Stream::parseHexColor(const std::string& text, Vector4& srgb, bool& hasAlpha) {
    std::string hex = text;
    if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
    if (hex.size() != 6 && hex.size() != 8) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (size_t i = 0; i < hex.size() / 2; i++) {
        v[i] = static_cast<float>(std::stoi(hex.substr(i * 2, 2), nullptr, 16)) / 255.0f;
    }
    srgb = Vector4(v[0], v[1], v[2], v[3]);
    hasAlpha = hex.size() == 8;
    return true;
}

bool editor::Stream::decodeColorValue(const YAML::Node& node, Vector4& linear, bool& hasAlpha) {
    hasAlpha = false;
    if (!node) return false;
    if (node.IsScalar()) {
        Vector4 srgb;
        if (!parseHexColor(node.as<std::string>(), srgb, hasAlpha)) return false;
        const Vector3 color = Color::sRGBToLinear(Vector3(srgb.x, srgb.y, srgb.z));
        linear = Vector4(color.x, color.y, color.z, srgb.w);
        return true;
    }
    if (node.IsSequence() && node.size() >= 3) {
        linear = Vector4(decodeFinite(node[0], 0.0f), decodeFinite(node[1], 0.0f), decodeFinite(node[2], 0.0f),
                         node.size() >= 4 ? decodeFinite(node[3], 1.0f) : 1.0f);
        hasAlpha = node.size() >= 4;
        return true;
    }
    return false;
}

namespace {

    // a linear colour as "#RRGGBB" when that is exactly what it is (an 8-bit sRGB value read
    // back gives it), else [r, g, b] in linear values
    YAML::Node encodeGradientColor(const Vector3& linear) {
        const Vector3 srgb = Color::linearTosRGB(Vector3(std::max(linear.x, 0.0f), std::max(linear.y, 0.0f), std::max(linear.z, 0.0f)));
        const float channels[3] = {srgb.x, srgb.y, srgb.z};
        const float linearChannels[3] = {linear.x, linear.y, linear.z};
        int bytes[3];
        bool exact = true;
        for (int i = 0; i < 3 && exact; i++) {
            if (!(channels[i] >= 0.0f && channels[i] <= 1.0f)) {
                exact = false;
                break;
            }
            bytes[i] = (int)std::lround(channels[i] * 255.0f);
            const float back = Color::sRGBToLinear(Vector3((float)bytes[i] / 255.0f, 0.0f, 0.0f)).x;
            exact = std::fabs(back - linearChannels[i]) <= 1e-6f;
        }
        if (exact) {
            char text[8];
            std::snprintf(text, sizeof(text), "#%02X%02X%02X", bytes[0], bytes[1], bytes[2]);
            return YAML::Node(std::string(text));
        }
        YAML::Node node;
        node.SetStyle(YAML::EmitterStyle::Flow);
        node.push_back(editor::YamlNumbers::text(linear.x));
        node.push_back(editor::YamlNumbers::text(linear.y));
        node.push_back(editor::YamlNumbers::text(linear.z));
        return node;
    }

}

YAML::Node editor::Stream::encodeGradient(const Gradient& gradient) {
    YAML::Node node;
    node["mode"] = gradient.mode == GradientMode::Fixed ? "fixed" : "blend";
    YAML::Node colors(YAML::NodeType::Sequence);
    for (const GradientColorKey& key : gradient.colorKeys) {
        YAML::Node keyNode;
        keyNode.SetStyle(YAML::EmitterStyle::Flow);
        keyNode.push_back(YamlNumbers::text(key.time));
        keyNode.push_back(encodeGradientColor(key.color));
        colors.push_back(keyNode);
    }
    node["colors"] = colors;
    YAML::Node alphas(YAML::NodeType::Sequence);
    for (const GradientAlphaKey& key : gradient.alphaKeys) {
        YAML::Node keyNode;
        keyNode.SetStyle(YAML::EmitterStyle::Flow);
        keyNode.push_back(YamlNumbers::text(key.time));
        keyNode.push_back(YamlNumbers::text(key.alpha));
        alphas.push_back(keyNode);
    }
    node["alphas"] = alphas;
    return node;
}

Gradient editor::Stream::decodeGradient(const YAML::Node& node, const Gradient& fallback) {
    if (!node || node.IsNull()) return fallback;
    // a single colour: a constant gradient
    Vector4 single;
    bool singleAlpha = false;
    if ((node.IsScalar() || (node.IsSequence() && node.size() >= 3 && node[0].IsScalar())) && decodeColorValue(node, single, singleAlpha)) {
        return Gradient::constant(single);
    }
    if (!node.IsMap()) return fallback;

    Gradient gradient({}, {});
    if (node["mode"]) {
        if (node["mode"].IsScalar()) {
            gradient.mode = (node["mode"].Scalar() == "fixed") ? GradientMode::Fixed : GradientMode::Blend;
        } else {
            Out::warning("Gradient: mode is not blend or fixed: blend");
        }
    }
    if (node["colors"] && node["colors"].IsSequence()) {
        for (const YAML::Node& keyNode : node["colors"]) {
            if (!keyNode.IsSequence() || keyNode.size() < 2) continue;
            Vector4 color;
            bool hasAlpha = false;
            if (!decodeColorValue(keyNode[1], color, hasAlpha)) continue;
            gradient.colorKeys.push_back(GradientColorKey(decodeFinite(keyNode[0], 0.0f), Vector3(color.x, color.y, color.z)));
        }
    }
    if (node["alphas"] && node["alphas"].IsSequence()) {
        for (const YAML::Node& keyNode : node["alphas"]) {
            if (!keyNode.IsSequence() || keyNode.size() < 2) continue;
            gradient.alphaKeys.push_back(GradientAlphaKey(decodeFinite(keyNode[0], 0.0f), decodeFinite(keyNode[1], 1.0f)));
        }
    }
    // more than Unity's 8 keys: the file still loads, with the first 7 and the last (normalize)
    if (gradient.colorKeys.size() > Gradient::MAX_KEYS || gradient.alphaKeys.size() > Gradient::MAX_KEYS) {
        Out::warning("Gradient: %zu colour and %zu alpha keys, at most %zu each: the first %zu and the last are kept",
                     gradient.colorKeys.size(), gradient.alphaKeys.size(), Gradient::MAX_KEYS, Gradient::MAX_KEYS - 1);
    }
    gradient.normalize();
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
    if (!node || !node.IsSequence() || node.size() < 4) {
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
    if (!node || !node.IsSequence() || node.size() < 4) {
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

    if (texture.empty() || texture.isFramebuffer())
        return node;

    const bool isCube = texture.isCubeMap() || (texture.getNumFaces() == 6);
    node["textureType"] = isCube ? "CUBE" : "2D";

    // Source selection: prefer filesystem paths; fall back to id only when there is no path.
    if (!isCube) {
        const std::string path = texture.getPath(0);
        if (!path.empty()) {
            node["source"] = "path";
            node["path"] = path;
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
            node["path"] = texture.getPath(0);
        } else {
            node["cubeMode"] = "faces";
            node["source"] = "paths";
            YAML::Node pathsNode;
            pathsNode.SetStyle(YAML::EmitterStyle::Flow);
            for (int f = 0; f < 6; f++) {
                pathsNode.push_back(texture.getPath((size_t)f));
            }
            node["paths"] = pathsNode;
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

Texture editor::Stream::decodeTexture(const YAML::Node& node) {
    Texture texture;
    if (node.IsMap()) { // Check if node has data
        const std::string textureType = node["textureType"] ? node["textureType"].as<std::string>() : std::string();
        const std::string source = node["source"] ? node["source"].as<std::string>() : std::string();

        if (textureType == "CUBE") {
            const std::string cubeMode = node["cubeMode"] ? node["cubeMode"].as<std::string>() : std::string();
            if (cubeMode == "single") {
                if (node["path"]) {
                    texture.setCubeMap(node["path"].as<std::string>());
                }
            } else {
                if (node["paths"] && node["paths"].IsSequence() && node["paths"].size() == 6) {
                    std::array<std::string, 6> paths;
                    for (size_t f = 0; f < 6; f++) {
                        paths[f] = node["paths"][f].as<std::string>();
                    }

                    // Set per-face paths individually (supports incomplete cubemaps).
                    for (size_t f = 0; f < 6; f++) {
                        texture.setCubePath(f, paths[f]);
                    }
                }
            }
        } else if (textureType == "2D") {
            if (source == "path") {
                if (node["path"]) {
                    texture.setPath(node["path"].as<std::string>());
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
                    const std::string textureId = node["id"] ? node["id"].as<std::string>() : std::string();

                    if (dataSize == expectedSize && decodedData.size() >= dataSize) {
                        setEmbeddedTextureData(texture, textureId, width, height, stringToColorFormat(colorFormat, channels), channels, decodedData);
                    }
                }
            } else if (source == "id") {
                if (node["id"]) {
                    texture.setId(node["id"].as<std::string>());
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
            }
        }

        if (node["minFilter"]) texture.setMinFilter(stringToTextureFilter(node["minFilter"].as<std::string>()));
        if (node["magFilter"]) texture.setMagFilter(stringToTextureFilter(node["magFilter"].as<std::string>()));
        if (node["wrapU"]) texture.setWrapU(stringToTextureWrap(node["wrapU"].as<std::string>()));
        if (node["wrapV"]) texture.setWrapV(stringToTextureWrap(node["wrapV"].as<std::string>()));
        // Old scenes carry the scale inside the path ("?svgScale=N"), absorbed by setPath()
        if (node["svgScale"]) texture.setSvgScale(YamlNumbers::as<float>(node["svgScale"]));
        if (node["alphaBorder"]) texture.setAlphaBorder(stringToTextureAlphaBorder(node["alphaBorder"].as<std::string>()));

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
    buffer.setType(stringToBufferType(node["type"].as<std::string>()));
    buffer.setUsage(stringToBufferUsage(node["usage"].as<std::string>()));
    buffer.setStride(YamlNumbers::as<unsigned int>(node["stride"]));
    buffer.setSize(YamlNumbers::as<size_t>(node["size"]));
    buffer.setCount(YamlNumbers::as<unsigned int>(node["count"]));
    buffer.setRenderAttributes(YamlNumbers::as<bool>(node["renderAttributes"]));
    buffer.setInstanceBuffer(YamlNumbers::as<bool>(node["instanceBuffer"]));

    // Decode attributes
    if (node["attributes"]) {
        for (const auto& attrNode : node["attributes"]) {
            AttributeType type = stringToAttributeType(attrNode["type"].as<std::string>());
            AttributeDataType dataType = stringToAttributeDataType(attrNode["dataType"].as<std::string>());
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

YAML::Node editor::Stream::encodeSubmesh(const Submesh& submesh, bool embedTextureData) {
    YAML::Node node;

    node["material"] = encodeMaterial(submesh.material, embedTextureData);
    node["textureRect"] = encodeRect(submesh.textureRect);
    node["primitiveType"] = primitiveTypeToString(submesh.primitiveType);
    node["vertexCount"] = YamlNumbers::integerText(submesh.vertexCount);
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
    Submesh submesh;
    if (oldSubmesh) {
        submesh = *oldSubmesh;
    }

    // All optional: an absent key keeps the value the submesh already has
    if (node["material"]) submesh.material = decodeMaterial(node["material"], &submesh.material);
    if (node["textureRect"]) submesh.textureRect = decodeRect(node["textureRect"]);
    if (node["primitiveType"]) submesh.primitiveType = stringToPrimitiveType(node["primitiveType"].as<std::string>());
    if (node["vertexCount"]) submesh.vertexCount = YamlNumbers::as<uint32_t>(node["vertexCount"]);
    if (node["faceCulling"]) submesh.faceCulling = YamlNumbers::as<bool>(node["faceCulling"]);
    if (node["textureShadow"]) submesh.textureShadow = YamlNumbers::as<bool>(node["textureShadow"]);

    // Omitted at its default, so an absent key is a value, not a reason to keep the old one.
    submesh.overrideFields = node["overrideFields"] ? YamlNumbers::as<uint32_t>(node["overrideFields"]) : 0;

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

    if (tn["brushMode"].IsDefined())          ts.brushMode          = static_cast<int>(stringToTerrainBrushMode(tn["brushMode"].as<std::string>()));
    if (tn["brushShape"].IsDefined())         ts.brushShape         = static_cast<int>(stringToTerrainBrushShape(tn["brushShape"].as<std::string>()));
    if (tn["brushFalloff"].IsDefined())       ts.brushFalloff       = static_cast<int>(stringToTerrainBrushFalloff(tn["brushFalloff"].as<std::string>()));
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
    "rotationSnapDegrees",
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
    if (ds.showPostProcessing)
        sceneNode["showPostProcessing"] = ds.showPostProcessing;
    if (ds.showAllLattices)
        sceneNode["showAllLattices"] = ds.showAllLattices;
    sceneNode["showGrid2D"]           = ds.showGrid2D;
    encodePositiveFinite(sceneNode, "gridSpacing2D", ds.gridSpacing2D);
    encodePositiveFinite(sceneNode, "gridSpacing3D", ds.gridSpacing3D);
    sceneNode["snapToGrid"]           = ds.snapToGrid;
    sceneNode["snapTile"]             = ds.snapTile;
    sceneNode["snapRotation"]         = ds.snapRotation;
    encodePositiveFinite(sceneNode, "rotationSnapDegrees", ds.rotationSnapDegrees);
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
    ds.showPostProcessing = node["showPostProcessing"] ? YamlNumbers::as<bool>(node["showPostProcessing"]) : false;
    ds.showAllLattices = node["showAllLattices"] ? YamlNumbers::as<bool>(node["showAllLattices"]) : false;
    if (node["showGrid2D"])           ds.showGrid2D           = YamlNumbers::as<bool>(node["showGrid2D"]);
    if (node["gridSpacing2D"])        ds.gridSpacing2D        = decodePositiveFinite(node["gridSpacing2D"], ds.gridSpacing2D);
    if (node["gridSpacing3D"])        ds.gridSpacing3D        = decodePositiveFinite(node["gridSpacing3D"], ds.gridSpacing3D);
    if (node["snapToGrid"])           ds.snapToGrid           = YamlNumbers::as<bool>(node["snapToGrid"]);
    if (node["snapTile"])             ds.snapTile             = YamlNumbers::as<bool>(node["snapTile"]);
    if (node["snapRotation"])         ds.snapRotation         = YamlNumbers::as<bool>(node["snapRotation"]);
    if (node["rotationSnapDegrees"])  ds.rotationSnapDegrees  = decodePositiveFinite(node["rotationSnapDegrees"], ds.rotationSnapDegrees);
}

namespace {

// ---- Project Settings > Input in project.yaml: `input: {version: 1, actions: [...]}`, the
// shape InputActions reads as JSON (InputActions::parseActionsJson), each binding a flow map

std::string inputNumber(float value) {
    char text[32];
    snprintf(text, sizeof(text), "%.6g", static_cast<double>(value));
    return text;
}

YAML::Node encodeInputControls(const std::vector<InputControl>& controls) {
    YAML::Node list(YAML::NodeType::Sequence);
    for (const InputControl& control : controls) list.push_back(InputControlNames::name(control));
    list.SetStyle(YAML::EmitterStyle::Flow);
    return list;
}

YAML::Node encodeInputActions(const std::vector<InputAction>& actions) {
    YAML::Node input;
    input["version"] = 1;
    YAML::Node list(YAML::NodeType::Sequence);
    for (const InputAction& action : actions) {
        YAML::Node node;
        node["name"] = action.name;
        node["type"] = InputActions::typeName(action.type);
        if (!action.description.empty()) node["description"] = action.description;
        if (action.pressPoint != 0.5f) node["press_point"] = inputNumber(action.pressPoint);
        YAML::Node bindings(YAML::NodeType::Sequence);
        for (const InputBinding& binding : action.bindings) {
            YAML::Node b;
            b["kind"] = InputActions::kindName(binding.kind);
            switch (binding.kind) {
                case InputBindingKind::Buttons:
                    if (!binding.negative.empty()) b["negative"] = encodeInputControls(binding.negative);
                    b["positive"] = encodeInputControls(binding.positive);
                    if (binding.gamepad >= 0) b["gamepad"] = binding.gamepad;
                    b["gravity"] = inputNumber(binding.gravity);
                    b["sensitivity"] = inputNumber(binding.sensitivity);
                    b["dead"] = inputNumber(binding.dead);
                    if (binding.snap) b["snap"] = true;
                    break;
                case InputBindingKind::GamepadAxis:
                    b["axis"] = InputControlNames::gamepadAxisName(binding.axis);
                    if (binding.gamepad >= 0) b["gamepad"] = binding.gamepad;
                    else b["gamepad"] = "any";
                    b["sensitivity"] = inputNumber(binding.sensitivity);
                    b["dead"] = inputNumber(binding.dead);
                    break;
                case InputBindingKind::MouseDelta:
                    b["axis"] = InputActions::mouseAxisName(binding.mouseAxis);
                    b["sensitivity"] = inputNumber(binding.sensitivity);
                    break;
            }
            if (binding.invert) b["invert"] = true;
            b.SetStyle(YAML::EmitterStyle::Flow);
            bindings.push_back(b);
        }
        node["bindings"] = bindings;
        list.push_back(node);
    }
    input["actions"] = list;
    return input;
}

// YAML -> the JSON InputActions reads: names, kinds, axes and control names stay text ("0" is
// the key 0, an action may be named "1"), the rest reads as numbers and booleans
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

}

YAML::Node editor::Stream::encodeProject(Project* project) {
    YAML::Node root;

    root["name"] = project->getName();

    root["canvasWidth"] = project->getCanvasWidth();
    root["canvasHeight"] = project->getCanvasHeight();

    root["scalingMode"] = scalingModeToString(project->getScalingMode());
    root["textureStrategy"] = textureStrategyToString(project->getTextureStrategy());
    root["vsync"] = project->isVSyncEnabled();
    root["htmlUiMsaaSamples"] = project->getHtmlUiMsaaSamples();

    root["windowMode"] = windowModeToString(project->getWindowMode());
    root["windowWidth"] = project->getWindowWidth();
    root["windowHeight"] = project->getWindowHeight();
    root["windowResizable"] = project->isWindowResizable();
    if (!project->getWindowTitle().empty()) {
        root["windowTitle"] = project->getWindowTitle();
    }
    if (!project->getWindowIcon().empty()) {
        root["windowIcon"] = project->getWindowIcon().string();
    }

    if (!project->getAssetsDir().empty() && project->getAssetsDir() != ".") {
        root["assetsDir"] = project->getAssetsDir().string();
    }
    if (!project->getLuaDir().empty()) {
        root["luaDir"] = project->getLuaDir().string();
    }
    if (!project->getScriptDirs().empty()) {
        YAML::Node scriptDirsNode;
        for (const fs::path& scriptDir : project->getScriptDirs()) {
            scriptDirsNode.push_back(scriptDir.generic_string());
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
    if (project->shouldPackNativeResources() != Project::defaultPackNativeResources) {
        root["packNativeResources"] = project->shouldPackNativeResources();
    }
    if (project->hasVersionControlMetadata() != Project::defaultVersionControlMetadata) {
        root["versionControlMetadata"] = project->hasVersionControlMetadata();
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

    // Project Settings > Input: only once edited (an old project reads Unity's defaults)
    if (project->hasCustomInputActions()) {
        root["input"] = encodeInputActions(project->getInputActions());
    } else if (!project->getUnreadableInputNode().empty()) {
        try {
            root["input"] = YAML::Load(project->getUnreadableInputNode());
        } catch (const YAML::Exception&) {
        }
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
        if (ios.admobEnabled != defaultIOS.admobEnabled) iosNode["admobEnabled"] = ios.admobEnabled;
        if (!ios.admobAppId.empty()) iosNode["admobAppId"] = ios.admobAppId;
        if (iosNode.size() != 0) root["ios"] = iosNode;
    }

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
            androidNode["launcherIcon"] = android.launcherIcon.string();
        }
        if (!android.adaptiveIconForeground.empty()) {
            androidNode["adaptiveIconForeground"] = android.adaptiveIconForeground.string();
        }
        if (!android.adaptiveIconBackground.empty()) {
            androidNode["adaptiveIconBackground"] = android.adaptiveIconBackground.string();
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

        if (android.allowBackup != defaults.allowBackup) androidNode["allowBackup"] = android.allowBackup;
        if (android.fullscreen != defaults.fullscreen) androidNode["fullscreen"] = android.fullscreen;
        if (android.admobEnabled != defaults.admobEnabled) androidNode["admobEnabled"] = android.admobEnabled;
        if (!android.admobAppId.empty()) androidNode["admobAppId"] = android.admobAppId;
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

        auto appendScene = [&scenesNode, &position](const std::string& filepath) {
            YAML::Node sceneNode;
            sceneNode["filepath"] = filepath;
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

        for (const auto& sceneProject : project->getScenes()) {
            if (sceneProject.filepath.empty()) {
                continue;
            }
            appendUnresolved();
            appendScene(sceneProject.filepath.string());
        }
        appendUnresolved();
    }
    root["scenes"] = scenesNode;

    return root;
}

void editor::Stream::decodeProject(Project* project, const YAML::Node& node, const Workspace& workspace) {
    if (!node.IsMap()) return;

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
        project->setScalingMode(stringToScalingMode(node["scalingMode"].as<std::string>()));
    }

    if (node["textureStrategy"]) {
        project->setTextureStrategy(stringToTextureStrategy(node["textureStrategy"].as<std::string>()));
    }

    if (node["vsync"].IsDefined()) {
        project->setVSyncEnabled(YamlNumbers::as<bool>(node["vsync"]));
    }

    if (node["htmlUiMsaaSamples"].IsDefined()) {
        project->setHtmlUiMsaaSamples(YamlNumbers::as<int>(node["htmlUiMsaaSamples"]));
    }

    if (node["windowMode"]) {
        project->setWindowMode(stringToWindowMode(node["windowMode"].as<std::string>()));
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
        project->setWindowIcon(std::filesystem::path(node["windowIcon"].as<std::string>()));
    }

    if (node["assetsDir"]) {
        project->setAssetsDir(node["assetsDir"].as<std::string>());
    }

    if (node["luaDir"]) {
        project->setLuaDir(node["luaDir"].as<std::string>());
    }

    if (node["scriptDirs"]) {
        std::vector<fs::path> scriptDirs;
        for (const auto& dirNode : node["scriptDirs"]) {
            scriptDirs.push_back(dirNode.as<std::string>());
        }
        project->setScriptDirs(std::move(scriptDirs));
    }

    if (node["cxxStandard"]) {
        project->setCxxStandard(YamlNumbers::as<int>(node["cxxStandard"]));
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
        // Project Settings > Input; no node: Unity's Input Manager defaults
        const YAML::Node inputNode = node["input"];
        std::vector<InputAction> actions = InputActions::defaults();
        std::string error;
        if (inputNode && !inputNode.IsNull()) {
            if (!InputActions::parseActionsJson(inputYamlToJson(inputNode).dump(), actions, &error)) {
                Out::error("Project Settings > Input could not be read from project.yaml (%s): the default "
                           "actions are used, and the input node is kept as it is until the actions are edited",
                           error.c_str());
                actions = InputActions::defaults();
            }
        }
        project->setInputActions(actions);
        if (!error.empty()) project->setUnreadableInputNode(YAML::Dump(inputNode));
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
    }

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
        if (iosNode["admobEnabled"].IsDefined()) ios.admobEnabled = YamlNumbers::as<bool>(iosNode["admobEnabled"]);
        if (iosNode["admobAppId"]) ios.admobAppId = iosNode["admobAppId"].as<std::string>();
    }

    if (node["android"] && node["android"].IsMap()) {
        const YAML::Node& androidNode = node["android"];
        AndroidProjectSettings& android = project->getAndroidProjectSettings();

        if (androidNode["applicationName"]) android.applicationName = androidNode["applicationName"].as<std::string>();
        if (androidNode["packageName"]) android.packageName = androidNode["packageName"].as<std::string>();
        if (androidNode["versionCode"]) android.versionCode = YamlNumbers::as<unsigned int>(androidNode["versionCode"]);
        if (androidNode["versionName"]) android.versionName = androidNode["versionName"].as<std::string>();
        if (androidNode["launcherIcon"]) android.launcherIcon = androidNode["launcherIcon"].as<std::string>();
        if (androidNode["adaptiveIconForeground"]) android.adaptiveIconForeground = androidNode["adaptiveIconForeground"].as<std::string>();
        if (androidNode["adaptiveIconBackground"]) android.adaptiveIconBackground = androidNode["adaptiveIconBackground"].as<std::string>();
        if (androidNode["minSdk"]) android.minSdk = std::max(1u, YamlNumbers::as<unsigned int>(androidNode["minSdk"]));
        if (androidNode["targetSdk"]) android.targetSdk = std::max(android.minSdk, YamlNumbers::as<unsigned int>(androidNode["targetSdk"]));
        // the Android-only orientation of older projects becomes the shared one, unless the project
        // already has it; written back as presentation on the next save
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

        if (androidNode["allowBackup"].IsDefined()) android.allowBackup = YamlNumbers::as<bool>(androidNode["allowBackup"]);
        if (androidNode["fullscreen"].IsDefined()) android.fullscreen = YamlNumbers::as<bool>(androidNode["fullscreen"]);
        // android.keepScreenOn is dropped: exported games always kept the screen on (the native
        // window flag), which presentation.keepScreenOn (default true) keeps doing
        if (androidNode["admobEnabled"].IsDefined()) android.admobEnabled = YamlNumbers::as<bool>(androidNode["admobEnabled"]);
        if (androidNode["admobAppId"]) android.admobAppId = androidNode["admobAppId"].as<std::string>();
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
        project->getTerrainEditorSettings() = workspace.getTerrainEditorSettings();
    }
    project->getPhysicsPlacementSettings() = workspace.getPhysicsPlacementSettings();
    project->getSpriteNormalsSettings() = workspace.getSpriteNormalsSettings();

    project->setSimulateTouch(workspace.isSimulateTouch());

    // Which scenes are open is a per-user choice, so the tab list decides it. With no
    // workspace yet, the first scene opens, as it does for a fresh clone.
    std::set<std::string> openedScenePaths;
    const bool hasTabs = workspace.hasTabs();
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
    if (node["scenes"]) {
        for (const auto& sceneNode : node["scenes"]) {
            if (!sceneNode["filepath"]) {
                continue;
            }

            const std::string relStr = sceneNode["filepath"].as<std::string>();
            const size_t position = listPosition++;
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
            if (!project->loadScene(scenePath, opened, true, opened)) {
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

YAML::Node editor::Stream::encodeSceneProject(const Project* project, const SceneProject* sceneProject) {
    YAML::Node root;
    root["id"] = sceneProject->id;
    root["name"] = sceneProject->name;
    root["scene"] = encodeScene(sceneProject->scene);
    root["sceneType"] = sceneTypeToString(sceneProject->sceneType);
    root["mainCamera"] = sceneProject->mainCamera;

    YAML::Node maxValuesNode;
    maxValuesNode["maxSubmeshes"] = sceneProject->maxValues.maxSubmeshes;
    maxValuesNode["maxTilemapTilesRect"] = sceneProject->maxValues.maxTilemapTilesRect;
    maxValuesNode["maxTilemapTiles"] = sceneProject->maxValues.maxTilemapTiles;
    maxValuesNode["maxExternalBuffers"] = sceneProject->maxValues.maxExternalBuffers;
    maxValuesNode["maxSpriteFrames"] = sceneProject->maxValues.maxSpriteFrames;
    maxValuesNode["maxBones"] = sceneProject->maxValues.maxBones;
    maxValuesNode["maxColliders3D"] = sceneProject->maxValues.maxColliders3D;
    maxValuesNode["maxColliderVertices3D"] = sceneProject->maxValues.maxColliderVertices3D;
    maxValuesNode["maxColliderIndices3D"] = sceneProject->maxValues.maxColliderIndices3D;
    root["maxValues"] = maxValuesNode;

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

    root["entities"] = entitiesNode;

    return root;
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
        if (node["scene"]) sceneProject->scene = decodeScene(sceneProject->scene, node["scene"]);
    }
    if (node["sceneType"]) sceneProject->sceneType = stringToSceneType(node["sceneType"].as<std::string>());
    if (node["mainCamera"]) sceneProject->mainCamera = YamlNumbers::as<Entity>(node["mainCamera"]);

    sceneProject->maxValues = {};
    if (node["maxValues"]) {
        const YAML::Node& maxValuesNode = node["maxValues"];
        if (maxValuesNode["maxSubmeshes"]) sceneProject->maxValues.maxSubmeshes = YamlNumbers::as<unsigned int>(maxValuesNode["maxSubmeshes"]);
        if (maxValuesNode["maxTilemapTilesRect"]) sceneProject->maxValues.maxTilemapTilesRect = YamlNumbers::as<unsigned int>(maxValuesNode["maxTilemapTilesRect"]);
        if (maxValuesNode["maxTilemapTiles"]) sceneProject->maxValues.maxTilemapTiles = YamlNumbers::as<unsigned int>(maxValuesNode["maxTilemapTiles"]);
        if (maxValuesNode["maxExternalBuffers"]) sceneProject->maxValues.maxExternalBuffers = YamlNumbers::as<unsigned int>(maxValuesNode["maxExternalBuffers"]);
        if (maxValuesNode["maxSpriteFrames"]) sceneProject->maxValues.maxSpriteFrames = YamlNumbers::as<unsigned int>(maxValuesNode["maxSpriteFrames"]);
        if (maxValuesNode["maxBones"]) sceneProject->maxValues.maxBones = YamlNumbers::as<unsigned int>(maxValuesNode["maxBones"]);
        // maxColliders2D / maxColliderPoints2D of older editors: 2D colliders have no caps any more
        if (maxValuesNode["maxColliders3D"]) sceneProject->maxValues.maxColliders3D = YamlNumbers::as<unsigned int>(maxValuesNode["maxColliders3D"]);
        if (maxValuesNode["maxColliderVertices3D"]) sceneProject->maxValues.maxColliderVertices3D = YamlNumbers::as<unsigned int>(maxValuesNode["maxColliderVertices3D"]);
        if (maxValuesNode["maxColliderIndices3D"]) sceneProject->maxValues.maxColliderIndices3D = YamlNumbers::as<unsigned int>(maxValuesNode["maxColliderIndices3D"]);
    }

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
    if (node["type"]) camComp.type = stringToCameraType(node["type"].as<std::string>());
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

YAML::Node editor::Stream::encodeScene(Scene* scene) {
    YAML::Node sceneNode;

    sceneNode["backgroundColor"] = encodeVector4(scene->getBackgroundColor());
    sceneNode["shadowQuality"] = shadowQualityToString(scene->getShadowQuality());
    sceneNode["meshLodEnabled"] = scene->isMeshLodEnabled();
    sceneNode["meshLodThreshold"] = scene->getMeshLodThreshold();
    sceneNode["depthPrepassEnabled"] = scene->isDepthPrepassEnabled();
    // written only when not the default (half), so scenes saved before it existed re-save unchanged
    if (scene->getSceneColorScale() != SceneSettings().sceneColorScale)
        sceneNode["sceneColorScale"] = sceneColorScaleToString(scene->getSceneColorScale());
    sceneNode["lightState"] = lightStateToString(scene->getLightState());
    sceneNode["globalIlluminationIntensity"] = scene->getGlobalIlluminationIntensity();
    sceneNode["globalIlluminationColor"] = encodeVector3(scene->getGlobalIlluminationColor());
    if (scene->getMaxLightsPerObject() != SceneSettings().maxLightsPerObject)
        sceneNode["maxLightsPerObject"] = scene->getMaxLightsPerObject();
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

    if (!scene->getDefaultMeshShader().empty())
        sceneNode["defaultMeshShader"] = scene->getDefaultMeshShader();
    if (!scene->getDefaultUIShader().empty())
        sceneNode["defaultUIShader"] = scene->getDefaultUIShader();
    if (!scene->getDefaultSkyShader().empty())
        sceneNode["defaultSkyShader"] = scene->getDefaultSkyShader();
    if (!scene->getDefaultPointsShader().empty())
        sceneNode["defaultPointsShader"] = scene->getDefaultPointsShader();
    if (!scene->getDefaultLinesShader().empty())
        sceneNode["defaultLinesShader"] = scene->getDefaultLinesShader();

    for (const PostProcessPass& pass : scene->getPostProcessPasses()) {
        YAML::Node passNode;
        passNode["shader"] = pass.shader;
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
        sceneNode["postProcess"].push_back(passNode);
    }

    // the engine's post-processing: only the fields away from their defaults, and no key at all
    // while everything is default, so scenes saved before it existed re-save unchanged
    const YAML::Node postProcessingNode = encodePostProcessing(scene->getPostProcessing());
    if (postProcessingNode.size() > 0)
        sceneNode["postProcessing"] = postProcessingNode;

    return sceneNode;
}

YAML::Node editor::Stream::encodePostProcessing(const PostProcessingSettings& settings) {
    YAML::Node node(YAML::NodeType::Map);
    static const PostProcessingSettings defaults;
    using FieldType = PostProcessingSettings::FieldType;
    for (const PostProcessingSettings::Field& field : PostProcessingSettings::getFields()) {
        const void* value = settings.getFieldRef(field);
        const void* def = defaults.getFieldRef(field);
        switch (field.type) {
            case FieldType::Bool:
                if (*(const bool*)value != *(const bool*)def) node[field.name] = *(const bool*)value;
                break;
            case FieldType::Int:
                if (*(const int*)value != *(const int*)def) node[field.name] = *(const int*)value;
                break;
            case FieldType::Float:
                if (*(const float*)value != *(const float*)def) node[field.name] = *(const float*)value;
                break;
            case FieldType::Vector3:
                if (*(const Vector3*)value != *(const Vector3*)def) node[field.name] = encodeVector3(*(const Vector3*)value);
                break;
            case FieldType::String:
                if (*(const std::string*)value != *(const std::string*)def) node[field.name] = *(const std::string*)value;
                break;
            case FieldType::Enum: {
                const int v = *(const int*)value;
                if (v != *(const int*)def) node[field.name] = std::string(field.enumNames[v]);
                break;
            }
        }
    }
    return node;
}

PostProcessingSettings editor::Stream::decodePostProcessing(const YAML::Node& node) {
    PostProcessingSettings settings;
    if (!node || !node.IsMap()) return settings;
    using FieldType = PostProcessingSettings::FieldType;
    for (const PostProcessingSettings::Field& field : PostProcessingSettings::getFields()) {
        const YAML::Node value = node[field.name];
        if (!value) continue;
        void* ref = field.ref(settings);
        switch (field.type) {
            case FieldType::Bool: *(bool*)ref = YamlNumbers::as<bool>(value); break;
            case FieldType::Int: *(int*)ref = YamlNumbers::as<int>(value); break;
            case FieldType::Float: *(float*)ref = decodeFinite(value, *(float*)ref); break;
            case FieldType::Vector3: *(Vector3*)ref = decodeVector3(value); break;
            case FieldType::String: *(std::string*)ref = value.as<std::string>(); break;
            case FieldType::Enum: {
                const std::string name = value.as<std::string>();
                for (int i = 0; field.enumNames[i]; i++) {
                    if (name == field.enumNames[i]) *(int*)ref = i;
                }
                break;
            }
        }
    }
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
        scene->setShadowQuality(stringToShadowQuality(node["shadowQuality"].as<std::string>()));
    }
    if (node["meshLodEnabled"]) {
        scene->setMeshLodEnabled(YamlNumbers::as<bool>(node["meshLodEnabled"]));
    }
    if (node["meshLodThreshold"]) {
        scene->setMeshLodThreshold(decodeFinite(node["meshLodThreshold"], scene->getMeshLodThreshold()));
    }
    if (node["depthPrepassEnabled"]) {
        scene->setDepthPrepassEnabled(YamlNumbers::as<bool>(node["depthPrepassEnabled"]));
    }
    // absent = the default (half): it is only written when it differs
    scene->setSceneColorScale(node["sceneColorScale"] ? stringToSceneColorScale(node["sceneColorScale"].as<std::string>()) : SceneSettings().sceneColorScale);

    if (node["lightState"]) {
        scene->setLightState(stringToLightState(node["lightState"].as<std::string>()));
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
        scene->setShadow2DQuality(stringToShadowQuality(node["shadow2DQuality"].as<std::string>()));
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
        scene->setFixedResolutionFilter(stringToTextureFilter(node["fixedResolutionFilter"].as<std::string>()));
    }

    if (node["enableUIEvents"]) {
        scene->setEnableUIEvents(stringToUIEventState(node["enableUIEvents"].as<std::string>()));
    }

    // absent key clears the default so decoding into a reused scene (play restore) is exact
    scene->setDefaultMeshShader(node["defaultMeshShader"] ? node["defaultMeshShader"].as<std::string>() : "");
    scene->setDefaultUIShader(node["defaultUIShader"] ? node["defaultUIShader"].as<std::string>() : "");
    scene->setDefaultSkyShader(node["defaultSkyShader"] ? node["defaultSkyShader"].as<std::string>() : "");
    scene->setDefaultPointsShader(node["defaultPointsShader"] ? node["defaultPointsShader"].as<std::string>() : "");
    scene->setDefaultLinesShader(node["defaultLinesShader"] ? node["defaultLinesShader"].as<std::string>() : "");

    // absent key clears the chain so decoding into a reused scene (play restore) is exact
    std::vector<PostProcessPass> postProcess;
    if (node["postProcess"]) {
        for (const auto& passNode : node["postProcess"]) {
            PostProcessPass pass;
            pass.shader = passNode["shader"] ? passNode["shader"].as<std::string>() : "";
            pass.enabled = passNode["enabled"] ? YamlNumbers::as<bool>(passNode["enabled"]) : true;
            pass.uniforms = decodeShaderUniforms(passNode["uniforms"]);
            pass.textures = decodeShaderTextures(passNode["textures"]);
            pass.scale = decodeFinite(passNode["scale"], 1.0f);
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
        entityNode["name"] = registry->getEntityName(entity);
        encodeEntityState(entityNode, entity, registry);

        Signature signature = registry->getSignature(entity);
        YAML::Node components = encodeComponents(entity, registry, signature);

        if ((components.IsMap() && components.size() > 0)){
            entityNode["components"] = components;
        }
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
    }

    return node;
}

ScriptProperty editor::Stream::decodeScriptProperty(const YAML::Node& node) {
    ScriptProperty prop;

    if (node["name"]) prop.name = node["name"].as<std::string>();
    if (node["displayName"]) prop.displayName = node["displayName"].as<std::string>();
    if (node["type"]) prop.type = stringToScriptPropertyType(node["type"].as<std::string>());

    // Restore ptrTypeName if it exists
    if (node["ptrTypeName"]) {
        prop.ptrTypeName = node["ptrTypeName"].as<std::string>();
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
        }
    }

    return prop;
}

std::vector<Entity> editor::Stream::decodeEntity(const YAML::Node& entityNode, EntityRegistry* registry, std::vector<Entity>* entities, Project* project, SceneProject* sceneProject, Entity parent, bool createNewIfExists, bool removeMissingComponents, std::unordered_map<Entity, Entity>* entityRemap) {
    editor::getEditorHost().reportLoadingProgress();

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

        if (entityNode["components"]){
            // yaml-cpp only names the key it failed on, so point at the entity holding it
            try {
                decodeComponents(entity, parent, registry, entityNode["components"]);
            } catch (const std::exception& e) {
                throw std::runtime_error("entity '" + name + "' (id " + std::to_string(entity) + "): " + e.what());
            }
        }

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
                std::vector<Entity> childEntities = decodeEntity(childNode, registry, entities, project, sceneProject, entity, createNewIfExists, removeMissingComponents, entityRemap);
                std::copy(childEntities.begin(), childEntities.end(), std::back_inserter(allEntities));
            }
        }
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
        return !texture.empty() || CameraTextureLink::isLinkTexture(texture);
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
    node["name"] = material.name;

    // Shader library: written only away from their defaults, so a material without them is
    // stored exactly as before they existed
    if (!material.shader.empty()) node["shader"] = material.shader;
    if (!material.shaderParams.empty()) node["shaderParams"] = encodeShaderUniforms(material.shaderParams);
    if (!material.shaderTextures.empty()) {
        YAML::Node textures = encodeShaderTextures(material.shaderTextures, embedTextureData);
        if (textures.size() > 0) node["shaderTextures"] = textures;
    }
    if (!material.shaderFeatures.empty()) node["shaderFeatures"] = encodeShaderFeatures(material.shaderFeatures);
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

    return node;
}

Material editor::Stream::decodeMaterial(const YAML::Node& node, const Material* oldMaterial) {
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
        material.alphaMode = stringToMaterialAlphaMode(node["alphaMode"].as<std::string>());
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

    if (node["name"]) material.name = node["name"].as<std::string>();

    // Not merged: encodeMaterial omits these at their defaults, so an absent key is the default
    material.shader = node["shader"] ? node["shader"].as<std::string>() : std::string();
    material.shaderParams = decodeShaderUniforms(node["shaderParams"]);
    material.shaderTextures = decodeShaderTextures(node["shaderTextures"]);
    material.shaderFeatures = decodeShaderFeatures(node["shaderFeatures"]);
    material.blendMode = node["blendMode"] ? stringToMaterialBlendMode(node["blendMode"].as<std::string>()) : MaterialBlendMode::AUTO;
    material.cullMode = node["cullMode"] ? stringToMaterialCullMode(node["cullMode"].as<std::string>()) : MaterialCullMode::AUTO;
    material.depthWrite = node["depthWrite"] ? stringToMaterialDepthWrite(node["depthWrite"].as<std::string>()) : MaterialDepthWrite::AUTO;
    material.depthTest = node["depthTest"] ? stringToMaterialDepthTest(node["depthTest"].as<std::string>()) : MaterialDepthTest::AUTO;
    material.depthOffsetFactor = decodeFinite(node["depthOffsetFactor"], 0.0f);
    material.depthOffsetUnits = decodeFinite(node["depthOffsetUnits"], 0.0f);
    material.sortPriority = node["sortPriority"] ? YamlNumbers::as<int>(node["sortPriority"]) : 0;
    material.lighting = node["lighting"] ? stringToMaterialLighting(node["lighting"].as<std::string>()) : MaterialLighting::AUTO;
    static const Material defaults = Material();
    material.stochasticTiling = node["stochasticTiling"] ? YamlNumbers::as<bool>(node["stochasticTiling"]) : false;
    material.stochasticCellSize = decodeFinite(node["stochasticCellSize"], defaults.stochasticCellSize);
    material.stochasticBlendSharpness = decodeFinite(node["stochasticBlendSharpness"], defaults.stochasticBlendSharpness);
    material.stochasticContrast = decodeFinite(node["stochasticContrast"], defaults.stochasticContrast);
    material.stochasticRotation = decodeFinite(node["stochasticRotation"], defaults.stochasticRotation);
    decodeStencilAndColorWrite(node, material.stencil, material.colorWrite);
    material.extraPass = decodeMaterialPass(node["extraPass"]);

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
           signature.test(registry->getComponentId<TerrainComponent>());
}

bool editor::Stream::isGeneratedUI(const EntityRegistry* registry, Signature signature) {
    return signature.test(registry->getComponentId<ImageComponent>()) ||
           signature.test(registry->getComponentId<TextComponent>()) ||
           signature.test(registry->getComponentId<PolygonComponent>());
}

YAML::Node editor::Stream::encodeComponents(const Entity entity, const EntityRegistry* registry, Signature signature) {
    YAML::Node compNode;

    if (signature.test(registry->getComponentId<Transform>())) {
        const Transform& transform = registry->getComponent<Transform>(entity);
        compNode[Catalog::getComponentName(ComponentType::Transform, true)] = encodeTransform(transform);
    }

    if (signature.test(registry->getComponentId<MeshComponent>())) {
        // Only treat the mesh as model-backed (and skip encoding its buffers and
        // embedded textures) when its geometry is regenerated from a model source file
        // at runtime. This covers the ModelComponent root and the per-node child mesh
        // entities a multi-node model spreads its geometry across. An empty filename (or
        // a manually added mesh) loads no geometry, so the mesh data must still be
        // encoded to remain visible.
        bool isModel = isModelBackedMesh(entity, registry, signature);
        // a generated mesh rebuilds its buffers on load, but not its textures
        bool isGenerated = isModel || isGeneratedMesh(registry, signature);
        const MeshComponent& mesh = registry->getComponent<MeshComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::MeshComponent, true)] = encodeMeshComponent(mesh, !isGenerated, !isModel, !isGenerated);
    }

    if (signature.test(registry->getComponentId<UIComponent>())) {
        const bool isText = signature.test(registry->getComponentId<TextComponent>());
        bool isGenerated = isGeneratedUI(registry, signature);
        const UIComponent& ui = registry->getComponent<UIComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::UIComponent, true)] = encodeUIComponent(ui, !isGenerated, !isText);
    }

    if (signature.test(registry->getComponentId<ButtonComponent>())) {
        const ButtonComponent& button = registry->getComponent<ButtonComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ButtonComponent, true)] = encodeButtonComponent(button);
    }

    if (signature.test(registry->getComponentId<ScrollbarComponent>())) {
        const ScrollbarComponent& scrollbar = registry->getComponent<ScrollbarComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ScrollbarComponent, true)] = encodeScrollbarComponent(scrollbar);
    }

    if (signature.test(registry->getComponentId<ProgressbarComponent>())) {
        const ProgressbarComponent& progressbar = registry->getComponent<ProgressbarComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ProgressbarComponent, true)] = encodeProgressbarComponent(progressbar);
    }

    if (signature.test(registry->getComponentId<InputFieldComponent>())) {
        const InputFieldComponent& inputfield = registry->getComponent<InputFieldComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::InputFieldComponent, true)] = encodeInputFieldComponent(inputfield);
    }

    if (signature.test(registry->getComponentId<PanelComponent>())) {
        const PanelComponent& panel = registry->getComponent<PanelComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::PanelComponent, true)] = encodePanelComponent(panel);
    }

    if (signature.test(registry->getComponentId<RectTransformComponent>())) {
        const RectTransformComponent& layout = registry->getComponent<RectTransformComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::RectTransformComponent, true)] = encodeRectTransformComponent(layout);
    }

    if (signature.test(registry->getComponentId<LayoutGroupComponent>())) {
        const LayoutGroupComponent& container = registry->getComponent<LayoutGroupComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::LayoutGroupComponent, true)] = encodeLayoutGroupComponent(container);
    }

    if (signature.test(registry->getComponentId<TextComponent>())) {
        const TextComponent& text = registry->getComponent<TextComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::TextComponent, true)] = encodeTextComponent(text);
    }

    if (signature.test(registry->getComponentId<ImageComponent>())) {
        const ImageComponent& image = registry->getComponent<ImageComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ImageComponent, true)] = encodeImageComponent(image);
    }

    if (signature.test(registry->getComponentId<SpriteComponent>())) {
        const SpriteComponent& sprite = registry->getComponent<SpriteComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::SpriteComponent, true)] = encodeSpriteComponent(sprite);
    }

    if (signature.test(registry->getComponentId<TilemapComponent>())) {
        const TilemapComponent& tilemap = registry->getComponent<TilemapComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::TilemapComponent, true)] = encodeTilemapComponent(tilemap);
    }

    if (signature.test(registry->getComponentId<TerrainComponent>())) {
        const TerrainComponent& terrain = registry->getComponent<TerrainComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::TerrainComponent, true)] = encodeTerrainComponent(terrain);
    }

    if (signature.test(registry->getComponentId<LightComponent>())) {
        const LightComponent& light = registry->getComponent<LightComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::LightComponent, true)] = encodeLightComponent(light);
    }

    if (signature.test(registry->getComponentId<Light2DComponent>())) {
        const Light2DComponent& light2d = registry->getComponent<Light2DComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::Light2DComponent, true)] = encodeLight2DComponent(light2d);
    }

    if (signature.test(registry->getComponentId<ShadowCaster2DComponent>())) {
        const ShadowCaster2DComponent& shadowcaster2d = registry->getComponent<ShadowCaster2DComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ShadowCaster2DComponent, true)] = encodeShadowCaster2DComponent(shadowcaster2d);
    }

    if (signature.test(registry->getComponentId<FogComponent>())) {
        const FogComponent& fog = registry->getComponent<FogComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::FogComponent, true)] = encodeFogComponent(fog);
    }

    if (signature.test(registry->getComponentId<MirrorComponent>())) {
        const MirrorComponent& mirror = registry->getComponent<MirrorComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::MirrorComponent, true)] = encodeMirrorComponent(mirror);
    }

    if (signature.test(registry->getComponentId<ReflectionProbeComponent>())) {
        const ReflectionProbeComponent& probe = registry->getComponent<ReflectionProbeComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ReflectionProbeComponent, true)] = encodeReflectionProbeComponent(probe);
    }

    if (signature.test(registry->getComponentId<UIDocumentComponent>())) {
        const UIDocumentComponent& document = registry->getComponent<UIDocumentComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::UIDocumentComponent, true)] = encodeUIDocumentComponent(document);
    }

    if (signature.test(registry->getComponentId<FluidSimulationComponent>())) {
        const FluidSimulationComponent& fluid = registry->getComponent<FluidSimulationComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::FluidSimulationComponent, true)] = encodeFluidSimulationComponent(fluid);
    }

    if (signature.test(registry->getComponentId<LatticeComponent>())) {
        const LatticeComponent& lattice = registry->getComponent<LatticeComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::LatticeComponent, true)] = encodeLatticeComponent(lattice);
    }

    if (signature.test(registry->getComponentId<LatticeModifierComponent>())) {
        const LatticeModifierComponent& modifier = registry->getComponent<LatticeModifierComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::LatticeModifierComponent, true)] = encodeLatticeModifierComponent(modifier);
    }

    if (signature.test(registry->getComponentId<LatticeFollowerComponent>())) {
        const LatticeFollowerComponent& follower = registry->getComponent<LatticeFollowerComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::LatticeFollowerComponent, true)] = encodeLatticeFollowerComponent(follower);
    }

    if (signature.test(registry->getComponentId<LatticeTracksComponent>())) {
        const LatticeTracksComponent& tracks = registry->getComponent<LatticeTracksComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::LatticeTracksComponent, true)] = encodeLatticeTracksComponent(tracks);
    }

    if (signature.test(registry->getComponentId<PoseAnimatorComponent>())) {
        const PoseAnimatorComponent& animator = registry->getComponent<PoseAnimatorComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::PoseAnimatorComponent, true)] = encodePoseAnimatorComponent(animator);
    }

    if (signature.test(registry->getComponentId<AnimationPlayerComponent>())) {
        const AnimationPlayerComponent& player = registry->getComponent<AnimationPlayerComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::AnimationPlayerComponent, true)] = encodeAnimationPlayerComponent(player);
    }

    if (signature.test(registry->getComponentId<VertexAnimatorComponent>())) {
        const VertexAnimatorComponent& animator = registry->getComponent<VertexAnimatorComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::VertexAnimatorComponent, true)] = encodeVertexAnimatorComponent(animator);
    }

    if (signature.test(registry->getComponentId<TrailRendererComponent>())) {
        const TrailRendererComponent& trail = registry->getComponent<TrailRendererComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::TrailRendererComponent, true)] = encodeTrailRendererComponent(trail);
    }

    if (signature.test(registry->getComponentId<CameraComponent>())) {
        const CameraComponent& camera = registry->getComponent<CameraComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::CameraComponent, true)] = encodeCameraComponent(camera);
    }

    if (signature.test(registry->getComponentId<AudioSourceComponent>())) {
        const AudioSourceComponent& audio = registry->getComponent<AudioSourceComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::AudioSourceComponent, true)] = encodeAudioSourceComponent(audio);
    }

    if (signature.test(registry->getComponentId<ScriptComponent>())) {
        const ScriptComponent& script = registry->getComponent<ScriptComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ScriptComponent, true)] = encodeScriptComponent(script);
    }

    if (signature.test(registry->getComponentId<SkyboxComponent>())) {
        const SkyboxComponent& sky = registry->getComponent<SkyboxComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::SkyboxComponent, true)] = encodeSkyboxComponent(sky);
    }

    if (signature.test(registry->getComponentId<ModelComponent>())) {
        const ModelComponent& model = registry->getComponent<ModelComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ModelComponent, true)] = encodeModelComponent(model);
    }

    if (signature.test(registry->getComponentId<Rigidbody2DComponent>())) {
        const Rigidbody2DComponent& body = registry->getComponent<Rigidbody2DComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::Rigidbody2DComponent, true)] = encodeRigidbody2DComponent(body);
    }

    if (signature.test(registry->getComponentId<RigidbodyComponent>())) {
        const RigidbodyComponent& body = registry->getComponent<RigidbodyComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::RigidbodyComponent, true)] = encodeRigidbodyComponent(body);
    }

    if (signature.test(registry->getComponentId<Joint2DComponent>())) {
        const Joint2DComponent& joint = registry->getComponent<Joint2DComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::Joint2DComponent, true)] = encodeJoint2DComponent(joint);
    }

    if (signature.test(registry->getComponentId<Joint3DComponent>())) {
        const Joint3DComponent& joint = registry->getComponent<Joint3DComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::Joint3DComponent, true)] = encodeJoint3DComponent(joint);
    }

    if (signature.test(registry->getComponentId<ActionComponent>())) {
        const ActionComponent& action = registry->getComponent<ActionComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ActionComponent, true)] = encodeActionComponent(action);
    }

    if (signature.test(registry->getComponentId<TweenComponent>())) {
        const TweenComponent& timed = registry->getComponent<TweenComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::TweenComponent, true)] = encodeTweenComponent(timed);
    }

    if (signature.test(registry->getComponentId<PositionTweenComponent>())) {
        const PositionTweenComponent& posAction = registry->getComponent<PositionTweenComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::PositionTweenComponent, true)] = encodePositionTweenComponent(posAction);
    }

    if (signature.test(registry->getComponentId<RotationTweenComponent>())) {
        const RotationTweenComponent& rotAction = registry->getComponent<RotationTweenComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::RotationTweenComponent, true)] = encodeRotationTweenComponent(rotAction);
    }

    if (signature.test(registry->getComponentId<ScaleTweenComponent>())) {
        const ScaleTweenComponent& scaleTween = registry->getComponent<ScaleTweenComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ScaleTweenComponent, true)] = encodeScaleTweenComponent(scaleTween);
    }

    if (signature.test(registry->getComponentId<ColorTweenComponent>())) {
        const ColorTweenComponent& colorTween = registry->getComponent<ColorTweenComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ColorTweenComponent, true)] = encodeColorTweenComponent(colorTween);
    }

    if (signature.test(registry->getComponentId<AlphaTweenComponent>())) {
        const AlphaTweenComponent& alphaTween = registry->getComponent<AlphaTweenComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::AlphaTweenComponent, true)] = encodeAlphaTweenComponent(alphaTween);
    }

    if (signature.test(registry->getComponentId<SpriteAnimationComponent>())) {
        const SpriteAnimationComponent& spriteanim = registry->getComponent<SpriteAnimationComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::SpriteAnimationComponent, true)] = encodeSpriteAnimationComponent(spriteanim);
    }

    if (signature.test(registry->getComponentId<AnimationComponent>())) {
        const AnimationComponent& animation = registry->getComponent<AnimationComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::AnimationComponent, true)] = encodeAnimationComponent(animation);
    }

    if (signature.test(registry->getComponentId<BoneComponent>())) {
        const BoneComponent& bone = registry->getComponent<BoneComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::BoneComponent, true)] = encodeBoneComponent(bone);
    }

    if (signature.test(registry->getComponentId<KeyframeTracksComponent>())) {
        const KeyframeTracksComponent& tracks = registry->getComponent<KeyframeTracksComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::KeyframeTracksComponent, true)] = encodeKeyframeTracksComponent(tracks);
    }

    if (signature.test(registry->getComponentId<TranslateTracksComponent>())) {
        const TranslateTracksComponent& tracks = registry->getComponent<TranslateTracksComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::TranslateTracksComponent, true)] = encodeTranslateTracksComponent(tracks);
    }

    if (signature.test(registry->getComponentId<RotateTracksComponent>())) {
        const RotateTracksComponent& tracks = registry->getComponent<RotateTracksComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::RotateTracksComponent, true)] = encodeRotateTracksComponent(tracks);
    }

    if (signature.test(registry->getComponentId<ScaleTracksComponent>())) {
        const ScaleTracksComponent& tracks = registry->getComponent<ScaleTracksComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ScaleTracksComponent, true)] = encodeScaleTracksComponent(tracks);
    }

    if (signature.test(registry->getComponentId<MorphTracksComponent>())) {
        const MorphTracksComponent& tracks = registry->getComponent<MorphTracksComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::MorphTracksComponent, true)] = encodeMorphTracksComponent(tracks);
    }

    if (signature.test(registry->getComponentId<ParticleSystemComponent>())) {
        const ParticleSystemComponent& particles = registry->getComponent<ParticleSystemComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::ParticleSystemComponent, true)] = encodeParticleSystemComponent(particles);
    }

    if (signature.test(registry->getComponentId<PointsComponent>())) {
        const PointsComponent& pts = registry->getComponent<PointsComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::PointsComponent, true)] = encodePointsComponent(pts);
    }

    if (signature.test(registry->getComponentId<LineRendererComponent>())) {
        const LineRendererComponent& lines = registry->getComponent<LineRendererComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::LineRendererComponent, true)] = encodeLineRendererComponent(lines);
    }

    if (signature.test(registry->getComponentId<PolygonComponent>())) {
        const PolygonComponent& polygon = registry->getComponent<PolygonComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::PolygonComponent, true)] = encodePolygonComponent(polygon);
    }

    if (signature.test(registry->getComponentId<MeshPolygonComponent>())) {
        const MeshPolygonComponent& polygon = registry->getComponent<MeshPolygonComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::MeshPolygonComponent, true)] = encodeMeshPolygonComponent(polygon);
    }

    if (signature.test(registry->getComponentId<InstancedMeshComponent>())) {
        const InstancedMeshComponent& instmesh = registry->getComponent<InstancedMeshComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::InstancedMeshComponent, true)] = encodeInstancedMeshComponent(instmesh);
    }

    if (signature.test(registry->getComponentId<PrefabComponent>())) {
        const PrefabComponent& prefab = registry->getComponent<PrefabComponent>(entity);
        compNode[Catalog::getComponentName(ComponentType::PrefabComponent, true)] = encodePrefabComponent(prefab);
    }

    return compNode;
}

void editor::Stream::decodeComponents(Entity entity, Entity parent, EntityRegistry* registry, const YAML::Node& compNode){
    std::string compName;

    Signature signature = registry->getSignature(entity);

    compName = Catalog::getComponentName(ComponentType::Transform, true);
    if (compNode[compName]) {
        Transform* existing = registry->findComponent<Transform>(entity);
        Transform transform = decodeTransform(compNode[compName], existing);
        transform.parent = parent;
        if (!signature.test(registry->getComponentId<Transform>())){
            registry->addComponent<Transform>(entity, std::move(transform));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Transform, existing, &transform);
            registry->addEntityChild(transform.parent, entity, false);
            registry->getComponent<Transform>(entity) = std::move(transform);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::MeshComponent, true);
    if (compNode[compName]) {
        MeshComponent* existing = registry->findComponent<MeshComponent>(entity);
        MeshComponent mesh = decodeMeshComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<MeshComponent>())){
            registry->addComponent<MeshComponent>(entity, std::move(mesh));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::MeshComponent, existing, &mesh);
            registry->getComponent<MeshComponent>(entity) = std::move(mesh);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::UIComponent, true);
    if (compNode[compName]) {
        UIComponent* existing = registry->findComponent<UIComponent>(entity);
        UIComponent ui = decodeUIComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<UIComponent>())){
            registry->addComponent<UIComponent>(entity, std::move(ui));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::UIComponent, existing, &ui);
            registry->getComponent<UIComponent>(entity) = std::move(ui);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ButtonComponent, true);
    if (compNode[compName]) {
        ButtonComponent* existing = registry->findComponent<ButtonComponent>(entity);
        ButtonComponent button = decodeButtonComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ButtonComponent>())){
            registry->addComponent<ButtonComponent>(entity, std::move(button));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ButtonComponent, existing, &button);
            registry->getComponent<ButtonComponent>(entity) = std::move(button);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ScrollbarComponent, true);
    if (compNode[compName]) {
        ScrollbarComponent* existing = registry->findComponent<ScrollbarComponent>(entity);
        ScrollbarComponent scrollbar = decodeScrollbarComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ScrollbarComponent>())){
            registry->addComponent<ScrollbarComponent>(entity, std::move(scrollbar));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ScrollbarComponent, existing, &scrollbar);
            registry->getComponent<ScrollbarComponent>(entity) = std::move(scrollbar);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ProgressbarComponent, true);
    if (compNode[compName]) {
        ProgressbarComponent* existing = registry->findComponent<ProgressbarComponent>(entity);
        ProgressbarComponent progressbar = decodeProgressbarComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ProgressbarComponent>())){
            registry->addComponent<ProgressbarComponent>(entity, std::move(progressbar));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ProgressbarComponent, existing, &progressbar);
            registry->getComponent<ProgressbarComponent>(entity) = std::move(progressbar);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::InputFieldComponent, true);
    if (compNode[compName]) {
        InputFieldComponent* existing = registry->findComponent<InputFieldComponent>(entity);
        InputFieldComponent inputfield = decodeInputFieldComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<InputFieldComponent>())){
            registry->addComponent<InputFieldComponent>(entity, std::move(inputfield));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::InputFieldComponent, existing, &inputfield);
            registry->getComponent<InputFieldComponent>(entity) = std::move(inputfield);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::PanelComponent, true);
    if (compNode[compName]) {
        PanelComponent* existing = registry->findComponent<PanelComponent>(entity);
        PanelComponent panel = decodePanelComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<PanelComponent>())){
            registry->addComponent<PanelComponent>(entity, std::move(panel));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::PanelComponent, existing, &panel);
            registry->getComponent<PanelComponent>(entity) = std::move(panel);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::RectTransformComponent, true);
    if (compNode[compName]) {
        RectTransformComponent* existing = registry->findComponent<RectTransformComponent>(entity);
        RectTransformComponent layout = decodeRectTransformComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<RectTransformComponent>())){
            registry->addComponent<RectTransformComponent>(entity, std::move(layout));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::RectTransformComponent, existing, &layout);
            registry->getComponent<RectTransformComponent>(entity) = std::move(layout);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::LayoutGroupComponent, true);
    if (compNode[compName]) {
        LayoutGroupComponent* existing = registry->findComponent<LayoutGroupComponent>(entity);
        LayoutGroupComponent container = decodeLayoutGroupComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<LayoutGroupComponent>())){
            registry->addComponent<LayoutGroupComponent>(entity, std::move(container));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::LayoutGroupComponent, existing, &container);
            registry->getComponent<LayoutGroupComponent>(entity) = std::move(container);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::TextComponent, true);
    if (compNode[compName]) {
        TextComponent* existing = registry->findComponent<TextComponent>(entity);
        TextComponent text = decodeTextComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<TextComponent>())){
            registry->addComponent<TextComponent>(entity, std::move(text));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::TextComponent, existing, &text);
            registry->getComponent<TextComponent>(entity) = std::move(text);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ImageComponent, true);
    if (compNode[compName]) {
        ImageComponent* existing = registry->findComponent<ImageComponent>(entity);
        ImageComponent image = decodeImageComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ImageComponent>())){
            registry->addComponent<ImageComponent>(entity, std::move(image));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ImageComponent, existing, &image);
            registry->getComponent<ImageComponent>(entity) = std::move(image);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::SpriteComponent, true);
    if (compNode[compName]) {
        SpriteComponent* existing = registry->findComponent<SpriteComponent>(entity);
        SpriteComponent sprite = decodeSpriteComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<SpriteComponent>())){
            registry->addComponent<SpriteComponent>(entity, std::move(sprite));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::SpriteComponent, existing, &sprite);
            registry->getComponent<SpriteComponent>(entity) = std::move(sprite);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::TilemapComponent, true);
    if (compNode[compName]) {
        TilemapComponent* existing = registry->findComponent<TilemapComponent>(entity);
        TilemapComponent tilemap = decodeTilemapComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<TilemapComponent>())){
            registry->addComponent<TilemapComponent>(entity, std::move(tilemap));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::TilemapComponent, existing, &tilemap);
            registry->getComponent<TilemapComponent>(entity) = std::move(tilemap);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::TerrainComponent, true);
    if (compNode[compName]) {
        TerrainComponent* existing = registry->findComponent<TerrainComponent>(entity);
        TerrainComponent terrain = decodeTerrainComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<TerrainComponent>())){
            registry->addComponent<TerrainComponent>(entity, std::move(terrain));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::TerrainComponent, existing, &terrain);
            registry->getComponent<TerrainComponent>(entity) = std::move(terrain);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::LightComponent, true);
    if (compNode[compName]) {
        LightComponent* existing = registry->findComponent<LightComponent>(entity);
        LightComponent light = decodeLightComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<LightComponent>())){
            registry->addComponent<LightComponent>(entity, std::move(light));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::LightComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::LightComponent, existing, &light);
            registry->getComponent<LightComponent>(entity) = std::move(light);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::Light2DComponent, true);
    if (compNode[compName]) {
        Light2DComponent* existing = registry->findComponent<Light2DComponent>(entity);
        Light2DComponent light2d = decodeLight2DComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<Light2DComponent>())){
            registry->addComponent<Light2DComponent>(entity, std::move(light2d));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::Light2DComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Light2DComponent, existing, &light2d);
            registry->getComponent<Light2DComponent>(entity) = std::move(light2d);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ShadowCaster2DComponent, true);
    if (compNode[compName]) {
        ShadowCaster2DComponent* existing = registry->findComponent<ShadowCaster2DComponent>(entity);
        ShadowCaster2DComponent shadowcaster2d = decodeShadowCaster2DComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ShadowCaster2DComponent>())){
            registry->addComponent<ShadowCaster2DComponent>(entity, std::move(shadowcaster2d));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::ShadowCaster2DComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ShadowCaster2DComponent, existing, &shadowcaster2d);
            registry->getComponent<ShadowCaster2DComponent>(entity) = std::move(shadowcaster2d);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::FogComponent, true);
    if (compNode[compName]) {
        FogComponent* existing = registry->findComponent<FogComponent>(entity);
        FogComponent fog = decodeFogComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<FogComponent>())){
            registry->addComponent<FogComponent>(entity, std::move(fog));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::FogComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::FogComponent, existing, &fog);
            registry->getComponent<FogComponent>(entity) = std::move(fog);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::MirrorComponent, true);
    if (compNode[compName]) {
        MirrorComponent* existing = registry->findComponent<MirrorComponent>(entity);
        MirrorComponent mirror = decodeMirrorComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<MirrorComponent>())){
            registry->addComponent<MirrorComponent>(entity, std::move(mirror));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::MirrorComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::MirrorComponent, existing, &mirror);
            registry->getComponent<MirrorComponent>(entity) = std::move(mirror);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ReflectionProbeComponent, true);
    if (compNode[compName]) {
        ReflectionProbeComponent* existing = registry->findComponent<ReflectionProbeComponent>(entity);
        ReflectionProbeComponent probe = decodeReflectionProbeComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ReflectionProbeComponent>())){
            registry->addComponent<ReflectionProbeComponent>(entity, std::move(probe));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::ReflectionProbeComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ReflectionProbeComponent, existing, &probe);
            registry->getComponent<ReflectionProbeComponent>(entity) = std::move(probe);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::UIDocumentComponent, true);
    if (compNode[compName]) {
        UIDocumentComponent* existing = registry->findComponent<UIDocumentComponent>(entity);
        UIDocumentComponent document = decodeUIDocumentComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<UIDocumentComponent>())){
            registry->addComponent<UIDocumentComponent>(entity, std::move(document));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::UIDocumentComponent, existing, &document);
            registry->getComponent<UIDocumentComponent>(entity) = std::move(document);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::FluidSimulationComponent, true);
    if (compNode[compName]) {
        FluidSimulationComponent* existing = registry->findComponent<FluidSimulationComponent>(entity);
        FluidSimulationComponent fluid = decodeFluidSimulationComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<FluidSimulationComponent>())){
            registry->addComponent<FluidSimulationComponent>(entity, std::move(fluid));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::FluidSimulationComponent, existing, &fluid);
            registry->getComponent<FluidSimulationComponent>(entity) = std::move(fluid);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::LatticeComponent, true);
    if (compNode[compName]) {
        LatticeComponent* existing = registry->findComponent<LatticeComponent>(entity);
        LatticeComponent lattice = decodeLatticeComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<LatticeComponent>())){
            registry->addComponent<LatticeComponent>(entity, std::move(lattice));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::LatticeComponent, existing, &lattice);
            registry->getComponent<LatticeComponent>(entity) = std::move(lattice);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::LatticeModifierComponent, true);
    if (compNode[compName]) {
        LatticeModifierComponent* existing = registry->findComponent<LatticeModifierComponent>(entity);
        LatticeModifierComponent modifier = decodeLatticeModifierComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<LatticeModifierComponent>())){
            registry->addComponent<LatticeModifierComponent>(entity, std::move(modifier));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::LatticeModifierComponent, existing, &modifier);
            registry->getComponent<LatticeModifierComponent>(entity) = std::move(modifier);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::LatticeFollowerComponent, true);
    if (compNode[compName]) {
        LatticeFollowerComponent* existing = registry->findComponent<LatticeFollowerComponent>(entity);
        LatticeFollowerComponent follower = decodeLatticeFollowerComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<LatticeFollowerComponent>())){
            registry->addComponent<LatticeFollowerComponent>(entity, std::move(follower));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::LatticeFollowerComponent, existing, &follower);
            registry->getComponent<LatticeFollowerComponent>(entity) = std::move(follower);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::LatticeTracksComponent, true);
    if (compNode[compName]) {
        LatticeTracksComponent* existing = registry->findComponent<LatticeTracksComponent>(entity);
        LatticeTracksComponent tracks = decodeLatticeTracksComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<LatticeTracksComponent>())){
            registry->addComponent<LatticeTracksComponent>(entity, std::move(tracks));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::LatticeTracksComponent, existing, &tracks);
            registry->getComponent<LatticeTracksComponent>(entity) = std::move(tracks);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::PoseAnimatorComponent, true);
    if (compNode[compName]) {
        PoseAnimatorComponent* existing = registry->findComponent<PoseAnimatorComponent>(entity);
        PoseAnimatorComponent animator = decodePoseAnimatorComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<PoseAnimatorComponent>())){
            registry->addComponent<PoseAnimatorComponent>(entity, std::move(animator));
        }else{
            registry->getComponent<PoseAnimatorComponent>(entity) = std::move(animator);
        }
    }

    compName = Catalog::getComponentName(ComponentType::AnimationPlayerComponent, true);
    if (compNode[compName]) {
        AnimationPlayerComponent* existing = registry->findComponent<AnimationPlayerComponent>(entity);
        AnimationPlayerComponent player = decodeAnimationPlayerComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<AnimationPlayerComponent>())){
            registry->addComponent<AnimationPlayerComponent>(entity, std::move(player));
        }else{
            registry->getComponent<AnimationPlayerComponent>(entity) = std::move(player);
        }
    }

    compName = Catalog::getComponentName(ComponentType::VertexAnimatorComponent, true);
    if (compNode[compName]) {
        VertexAnimatorComponent* existing = registry->findComponent<VertexAnimatorComponent>(entity);
        VertexAnimatorComponent animator = decodeVertexAnimatorComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<VertexAnimatorComponent>())){
            registry->addComponent<VertexAnimatorComponent>(entity, std::move(animator));
        }else{
            registry->getComponent<VertexAnimatorComponent>(entity) = std::move(animator);
        }
    }

    compName = Catalog::getComponentName(ComponentType::TrailRendererComponent, true);
    if (compNode[compName]) {
        TrailRendererComponent* existing = registry->findComponent<TrailRendererComponent>(entity);
        TrailRendererComponent trail = decodeTrailRendererComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<TrailRendererComponent>())){
            registry->addComponent<TrailRendererComponent>(entity, std::move(trail));
        }else{
            registry->getComponent<TrailRendererComponent>(entity) = std::move(trail);
        }
    }

    compName = Catalog::getComponentName(ComponentType::CameraComponent, true);
    if (compNode[compName]) {
        CameraComponent* existing = registry->findComponent<CameraComponent>(entity);
        CameraComponent camera = decodeCameraComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<CameraComponent>())){
            registry->addComponent<CameraComponent>(entity, std::move(camera));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::CameraComponent, existing, &camera);
            registry->getComponent<CameraComponent>(entity) = std::move(camera);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::AudioSourceComponent, true);
    if (compNode[compName]) {
        AudioSourceComponent* existing = registry->findComponent<AudioSourceComponent>(entity);
        AudioSourceComponent audio = decodeAudioSourceComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<AudioSourceComponent>())){
            registry->addComponent<AudioSourceComponent>(entity, std::move(audio));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::AudioSourceComponent, existing, &audio);
            registry->getComponent<AudioSourceComponent>(entity) = std::move(audio);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ScriptComponent, true);
    if (compNode[compName]) {
        ScriptComponent* existing = registry->findComponent<ScriptComponent>(entity);
        ScriptComponent script = decodeScriptComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ScriptComponent>())){
            registry->addComponent<ScriptComponent>(entity, std::move(script));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ScriptComponent, existing, &script);
            registry->getComponent<ScriptComponent>(entity) = std::move(script);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::SkyboxComponent, true);
    if (compNode[compName]) {
        SkyboxComponent* existing = registry->findComponent<SkyboxComponent>(entity);
        SkyboxComponent sky = decodeSkyboxComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<SkyboxComponent>())){
            registry->addComponent<SkyboxComponent>(entity, std::move(sky));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::SkyboxComponent, existing, &sky);
            registry->getComponent<SkyboxComponent>(entity) = std::move(sky);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ModelComponent, true);
    if (compNode[compName]) {
        ModelComponent* existing = registry->findComponent<ModelComponent>(entity);
        ModelComponent model = decodeModelComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ModelComponent>())){
            registry->addComponent<ModelComponent>(entity, std::move(model));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ModelComponent, existing, &model);
            registry->getComponent<ModelComponent>(entity) = std::move(model);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::Rigidbody2DComponent, true);
    if (compNode[compName]) {
        Rigidbody2DComponent* existing = registry->findComponent<Rigidbody2DComponent>(entity);
        Rigidbody2DComponent body = decodeRigidbody2DComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<Rigidbody2DComponent>())){
            registry->addComponent<Rigidbody2DComponent>(entity, std::move(body));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Rigidbody2DComponent, existing, &body);
            registry->getComponent<Rigidbody2DComponent>(entity) = std::move(body);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::RigidbodyComponent, true);
    if (compNode[compName]) {
        RigidbodyComponent* existing = registry->findComponent<RigidbodyComponent>(entity);
        Transform* bodyTransform = registry->findComponent<Transform>(entity);
        const Vector3 bodyScale = bodyTransform ? bodyTransform->scale : Vector3::UNIT_SCALE;
        RigidbodyComponent body = decodeRigidbodyComponent(compNode[compName], existing, bodyScale);
        if (!signature.test(registry->getComponentId<RigidbodyComponent>())){
            registry->addComponent<RigidbodyComponent>(entity, std::move(body));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::RigidbodyComponent, existing, &body);
            registry->getComponent<RigidbodyComponent>(entity) = std::move(body);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::Joint2DComponent, true);
    if (compNode[compName]) {
        Joint2DComponent* existing = registry->findComponent<Joint2DComponent>(entity);
        Joint2DComponent joint = decodeJoint2DComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<Joint2DComponent>())){
            registry->addComponent<Joint2DComponent>(entity, std::move(joint));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Joint2DComponent, existing, &joint);
            registry->getComponent<Joint2DComponent>(entity) = std::move(joint);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::Joint3DComponent, true);
    if (compNode[compName]) {
        Joint3DComponent* existing = registry->findComponent<Joint3DComponent>(entity);
        Joint3DComponent joint = decodeJoint3DComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<Joint3DComponent>())){
            registry->addComponent<Joint3DComponent>(entity, std::move(joint));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Joint3DComponent, existing, &joint);
            registry->getComponent<Joint3DComponent>(entity) = std::move(joint);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ActionComponent, true);
    if (compNode[compName]) {
        ActionComponent* existing = registry->findComponent<ActionComponent>(entity);
        ActionComponent action = decodeActionComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ActionComponent>())){
            registry->addComponent<ActionComponent>(entity, std::move(action));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ActionComponent, existing, &action);
            registry->getComponent<ActionComponent>(entity) = std::move(action);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::TweenComponent, true);
    if (compNode[compName]) {
        TweenComponent* existing = registry->findComponent<TweenComponent>(entity);
        TweenComponent timed = decodeTweenComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<TweenComponent>())){
            registry->addComponent<TweenComponent>(entity, std::move(timed));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::TweenComponent, existing, &timed);
            registry->getComponent<TweenComponent>(entity) = std::move(timed);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::PositionTweenComponent, true);
    if (compNode[compName]) {
        PositionTweenComponent* existing = registry->findComponent<PositionTweenComponent>(entity);
        PositionTweenComponent posAction = decodePositionTweenComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<PositionTweenComponent>())){
            registry->addComponent<PositionTweenComponent>(entity, std::move(posAction));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::PositionTweenComponent, existing, &posAction);
            registry->getComponent<PositionTweenComponent>(entity) = std::move(posAction);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::RotationTweenComponent, true);
    if (compNode[compName]) {
        RotationTweenComponent* existing = registry->findComponent<RotationTweenComponent>(entity);
        RotationTweenComponent rotAction = decodeRotationTweenComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<RotationTweenComponent>())){
            registry->addComponent<RotationTweenComponent>(entity, std::move(rotAction));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::RotationTweenComponent, existing, &rotAction);
            registry->getComponent<RotationTweenComponent>(entity) = std::move(rotAction);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ScaleTweenComponent, true);
    if (compNode[compName]) {
        ScaleTweenComponent* existing = registry->findComponent<ScaleTweenComponent>(entity);
        ScaleTweenComponent scaleTween = decodeScaleTweenComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ScaleTweenComponent>())){
            registry->addComponent<ScaleTweenComponent>(entity, std::move(scaleTween));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ScaleTweenComponent, existing, &scaleTween);
            registry->getComponent<ScaleTweenComponent>(entity) = std::move(scaleTween);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ColorTweenComponent, true);
    if (compNode[compName]) {
        ColorTweenComponent* existing = registry->findComponent<ColorTweenComponent>(entity);
        ColorTweenComponent colorTween = decodeColorTweenComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ColorTweenComponent>())){
            registry->addComponent<ColorTweenComponent>(entity, std::move(colorTween));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ColorTweenComponent, existing, &colorTween);
            registry->getComponent<ColorTweenComponent>(entity) = std::move(colorTween);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::AlphaTweenComponent, true);
    if (compNode[compName]) {
        AlphaTweenComponent* existing = registry->findComponent<AlphaTweenComponent>(entity);
        AlphaTweenComponent alphaTween = decodeAlphaTweenComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<AlphaTweenComponent>())){
            registry->addComponent<AlphaTweenComponent>(entity, std::move(alphaTween));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::AlphaTweenComponent, existing, &alphaTween);
            registry->getComponent<AlphaTweenComponent>(entity) = std::move(alphaTween);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::SpriteAnimationComponent, true);
    if (compNode[compName]) {
        SpriteAnimationComponent* existing = registry->findComponent<SpriteAnimationComponent>(entity);
        SpriteAnimationComponent spriteanim = decodeSpriteAnimationComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<SpriteAnimationComponent>())){
            registry->addComponent<SpriteAnimationComponent>(entity, std::move(spriteanim));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::SpriteAnimationComponent, existing, &spriteanim);
            registry->getComponent<SpriteAnimationComponent>(entity) = std::move(spriteanim);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::AnimationComponent, true);
    if (compNode[compName]) {
        AnimationComponent* existing = registry->findComponent<AnimationComponent>(entity);
        AnimationComponent animation = decodeAnimationComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<AnimationComponent>())){
            registry->addComponent<AnimationComponent>(entity, std::move(animation));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::AnimationComponent, existing, &animation);
            registry->getComponent<AnimationComponent>(entity) = std::move(animation);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::BoneComponent, true);
    if (compNode[compName]) {
        BoneComponent* existing = registry->findComponent<BoneComponent>(entity);
        BoneComponent bone = decodeBoneComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<BoneComponent>())){
            registry->addComponent<BoneComponent>(entity, std::move(bone));
        }else{
            registry->getComponent<BoneComponent>(entity) = std::move(bone);
        }
    }

    compName = Catalog::getComponentName(ComponentType::KeyframeTracksComponent, true);
    if (compNode[compName]) {
        KeyframeTracksComponent* existing = registry->findComponent<KeyframeTracksComponent>(entity);
        KeyframeTracksComponent tracks = decodeKeyframeTracksComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<KeyframeTracksComponent>())){
            registry->addComponent<KeyframeTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<KeyframeTracksComponent>(entity) = std::move(tracks);
        }
    }

    compName = Catalog::getComponentName(ComponentType::TranslateTracksComponent, true);
    if (compNode[compName]) {
        TranslateTracksComponent* existing = registry->findComponent<TranslateTracksComponent>(entity);
        TranslateTracksComponent tracks = decodeTranslateTracksComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<TranslateTracksComponent>())){
            registry->addComponent<TranslateTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<TranslateTracksComponent>(entity) = std::move(tracks);
        }
    }

    compName = Catalog::getComponentName(ComponentType::RotateTracksComponent, true);
    if (compNode[compName]) {
        RotateTracksComponent* existing = registry->findComponent<RotateTracksComponent>(entity);
        RotateTracksComponent tracks = decodeRotateTracksComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<RotateTracksComponent>())){
            registry->addComponent<RotateTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<RotateTracksComponent>(entity) = std::move(tracks);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ScaleTracksComponent, true);
    if (compNode[compName]) {
        ScaleTracksComponent* existing = registry->findComponent<ScaleTracksComponent>(entity);
        ScaleTracksComponent tracks = decodeScaleTracksComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ScaleTracksComponent>())){
            registry->addComponent<ScaleTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<ScaleTracksComponent>(entity) = std::move(tracks);
        }
    }

    compName = Catalog::getComponentName(ComponentType::MorphTracksComponent, true);
    if (compNode[compName]) {
        MorphTracksComponent* existing = registry->findComponent<MorphTracksComponent>(entity);
        MorphTracksComponent tracks = decodeMorphTracksComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<MorphTracksComponent>())){
            registry->addComponent<MorphTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<MorphTracksComponent>(entity) = std::move(tracks);
        }
    }

    compName = Catalog::getComponentName(ComponentType::ParticleSystemComponent, true);
    if (compNode[compName]) {
        ParticleSystemComponent* existing = registry->findComponent<ParticleSystemComponent>(entity);
        ParticleSystemComponent particles = decodeParticleSystemComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<ParticleSystemComponent>())){
            registry->addComponent<ParticleSystemComponent>(entity, std::move(particles));
        }else{
            registry->getComponent<ParticleSystemComponent>(entity) = std::move(particles);
        }
    }

    compName = Catalog::getComponentName(ComponentType::PointsComponent, true);
    if (compNode[compName]) {
        PointsComponent* existing = registry->findComponent<PointsComponent>(entity);
        PointsComponent pts = decodePointsComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<PointsComponent>())) {
            registry->addComponent<PointsComponent>(entity, std::move(pts));
        } else {
            registry->getComponent<PointsComponent>(entity) = std::move(pts);
        }
    }

    compName = Catalog::getComponentName(ComponentType::LineRendererComponent, true);
    if (compNode[compName]) {
        LineRendererComponent* existing = registry->findComponent<LineRendererComponent>(entity);
        LineRendererComponent lines = decodeLineRendererComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<LineRendererComponent>())) {
            registry->addComponent<LineRendererComponent>(entity, std::move(lines));
        } else {
            registry->getComponent<LineRendererComponent>(entity) = std::move(lines);
        }
    }

    compName = Catalog::getComponentName(ComponentType::PolygonComponent, true);
    if (compNode[compName]) {
        PolygonComponent* existing = registry->findComponent<PolygonComponent>(entity);
        PolygonComponent polygon = decodePolygonComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<PolygonComponent>())) {
            registry->addComponent<PolygonComponent>(entity, std::move(polygon));
        } else {
            registry->getComponent<PolygonComponent>(entity) = std::move(polygon);
        }
    }

    compName = Catalog::getComponentName(ComponentType::MeshPolygonComponent, true);
    if (compNode[compName]) {
        MeshPolygonComponent* existing = registry->findComponent<MeshPolygonComponent>(entity);
        MeshPolygonComponent polygon = decodeMeshPolygonComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<MeshPolygonComponent>())) {
            registry->addComponent<MeshPolygonComponent>(entity, std::move(polygon));
        } else {
            registry->getComponent<MeshPolygonComponent>(entity) = std::move(polygon);
        }
    }

    compName = Catalog::getComponentName(ComponentType::InstancedMeshComponent, true);
    if (compNode[compName]) {
        InstancedMeshComponent* existing = registry->findComponent<InstancedMeshComponent>(entity);
        InstancedMeshComponent instmesh = decodeInstancedMeshComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<InstancedMeshComponent>())){
            registry->addComponent<InstancedMeshComponent>(entity, std::move(instmesh));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::InstancedMeshComponent, existing, &instmesh);
            registry->getComponent<InstancedMeshComponent>(entity) = std::move(instmesh);
            Catalog::updateEntity(registry, entity, flags);
        }
    }

    compName = Catalog::getComponentName(ComponentType::PrefabComponent, true);
    if (compNode[compName]) {
        PrefabComponent* existing = registry->findComponent<PrefabComponent>(entity);
        PrefabComponent prefab = decodePrefabComponent(compNode[compName], existing);
        if (!signature.test(registry->getComponentId<PrefabComponent>())){
            registry->addComponent<PrefabComponent>(entity, std::move(prefab));
        }else{
            registry->getComponent<PrefabComponent>(entity) = std::move(prefab);
        }
    }
}

YAML::Node editor::Stream::encodeTransform(const Transform& transform) {
    YAML::Node transformNode;

    transformNode["position"] = encodeVector3(transform.position);
    transformNode["rotation"] = encodeQuaternion(transform.rotation);
    transformNode["scale"] = encodeVector3(transform.scale);
    transformNode["worldPosition"] = encodeVector3(transform.worldPosition);
    transformNode["worldRotation"] = encodeQuaternion(transform.worldRotation);
    transformNode["worldScale"] = encodeVector3(transform.worldScale);
    transformNode["localMatrix"] = encodeMatrix4(transform.localMatrix);
    transformNode["modelMatrix"] = encodeMatrix4(transform.modelMatrix);
    transformNode["normalMatrix"] = encodeMatrix4(transform.normalMatrix);
    transformNode["visible"] = YamlNumbers::text(transform.visible);
    transformNode["billboardRotation"] = encodeQuaternion(transform.billboardRotation);
    transformNode["billboard"] = YamlNumbers::text(transform.billboard);
    transformNode["fakeBillboard"] = YamlNumbers::text(transform.fakeBillboard);
    transformNode["cylindricalBillboard"] = YamlNumbers::text(transform.cylindricalBillboard);

    return transformNode;
}

Transform editor::Stream::decodeTransform(const YAML::Node& node, const Transform* oldTransform) {
    Transform transform;

    // Use old values as defaults if provided
    if (oldTransform) {
        transform = *oldTransform;
    }

    if (node["position"]) transform.position = decodeVector3(node["position"]);
    if (node["rotation"]) transform.rotation = decodeQuaternion(node["rotation"]);
    if (node["scale"]) transform.scale = decodeVector3(node["scale"]);
    if (node["worldPosition"]) transform.worldPosition = decodeVector3(node["worldPosition"]);
    if (node["worldRotation"]) transform.worldRotation = decodeQuaternion(node["worldRotation"]);
    if (node["worldScale"]) transform.worldScale = decodeVector3(node["worldScale"]);
    if (node["localMatrix"]) transform.localMatrix = decodeMatrix4(node["localMatrix"]);
    if (node["modelMatrix"]) transform.modelMatrix = decodeMatrix4(node["modelMatrix"]);
    if (node["normalMatrix"]) transform.normalMatrix = decodeMatrix4(node["normalMatrix"]);
    if (node["visible"]) transform.visible = YamlNumbers::as<bool>(node["visible"]);
    if (node["billboardRotation"]) transform.billboardRotation = decodeQuaternion(node["billboardRotation"]);
    if (node["billboard"]) transform.billboard = YamlNumbers::as<bool>(node["billboard"]);
    if (node["fakeBillboard"]) transform.fakeBillboard = YamlNumbers::as<bool>(node["fakeBillboard"]);
    if (node["cylindricalBillboard"]) transform.cylindricalBillboard = YamlNumbers::as<bool>(node["cylindricalBillboard"]);

    return transform;
}

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
    node["transparent"] = YamlNumbers::text(mesh.transparent);
    node["autoTransparency"] = YamlNumbers::text(mesh.autoTransparency);

    node["cullingMode"] = cullingModeToString(mesh.cullingMode);
    node["windingOrder"] = windingOrderToString(mesh.windingOrder);
    if (mesh.mirroredWinding)
        node["mirroredWinding"] = true;

    if (!mesh.customShader.empty())
        node["customShader"] = mesh.customShader;
    if (!mesh.customDepthShader.empty())
        node["customDepthShader"] = mesh.customDepthShader;
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
    if (node["renderInReflectionProbes"]) mesh.renderInReflectionProbes = YamlNumbers::as<bool>(node["renderInReflectionProbes"]);
    if (node["shadowsBillboard"]) mesh.shadowsBillboard = YamlNumbers::as<bool>(node["shadowsBillboard"]);
    if (node["transparent"]) mesh.transparent = YamlNumbers::as<bool>(node["transparent"]);
    if (node["autoTransparency"]) mesh.autoTransparency = YamlNumbers::as<bool>(node["autoTransparency"]);

    if (node["cullingMode"]) mesh.cullingMode = stringToCullingMode(node["cullingMode"].as<std::string>());
    if (node["windingOrder"]) mesh.windingOrder = stringToWindingOrder(node["windingOrder"].as<std::string>());
    if (node["windingOrder"]) mesh.mirroredWinding = node["mirroredWinding"] && YamlNumbers::as<bool>(node["mirroredWinding"]);

    if (node["customShader"]) mesh.customShader = node["customShader"].as<std::string>();
    if (node["customDepthShader"]) mesh.customDepthShader = node["customDepthShader"].as<std::string>();
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
        node["customShader"] = ui.customShader;
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

    if (node["primitiveType"]) ui.primitiveType = stringToPrimitiveType(node["primitiveType"].as<std::string>());
    if (node["vertexCount"]) ui.vertexCount = YamlNumbers::as<unsigned int>(node["vertexCount"]);

    if (node["aabb"]) ui.aabb = decodeAABB(node["aabb"]);
    if (node["worldAABB"]) ui.worldAABB = decodeAABB(node["worldAABB"]);

    if (node["texture"]) ui.texture = decodeTexture(node["texture"]);
    if (node["color"]) ui.color = decodeVector4(node["color"]);

    if (node["automaticFlipY"]) ui.automaticFlipY = YamlNumbers::as<bool>(node["automaticFlipY"]);
    if (node["flipY"]) ui.flipY = YamlNumbers::as<bool>(node["flipY"]);

    if (node["pointerMoved"]) ui.pointerMoved = YamlNumbers::as<bool>(node["pointerMoved"]);
    if (node["focused"]) ui.focused = YamlNumbers::as<bool>(node["focused"]);

    if (node["customShader"]) ui.customShader = node["customShader"].as<std::string>();
    ui.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    ui.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, ui.shaderTextures, ui.shaderFeatures, &ui.blendMode, &ui.depthWrite);
    decodeStencilAndColorWrite(node, ui.stencil, ui.colorWrite);

    if (node["enabled"]) ui.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return ui;
}

YAML::Node editor::Stream::encodeButtonComponent(const ButtonComponent& button) {
    YAML::Node node;
    node["label"] = button.label;
    node["textureNormal"] = encodeTexture(button.textureNormal);
    node["textureHovered"] = encodeTexture(button.textureHovered);
    node["texturePressed"] = encodeTexture(button.texturePressed);
    node["textureDisabled"] = encodeTexture(button.textureDisabled);
    node["colorNormal"] = encodeVector4(button.colorNormal);
    node["colorHovered"] = encodeVector4(button.colorHovered);
    node["colorPressed"] = encodeVector4(button.colorPressed);
    node["colorDisabled"] = encodeVector4(button.colorDisabled);
    node["disabled"] = button.disabled;
    return node;
}

ButtonComponent editor::Stream::decodeButtonComponent(const YAML::Node& node, const ButtonComponent* oldButton) {
    ButtonComponent button;

    if (oldButton) {
        button = *oldButton;
    }

    if (node["label"]) button.label = YamlNumbers::as<Entity>(node["label"]);
    if (node["textureNormal"]) button.textureNormal = decodeTexture(node["textureNormal"]);
    if (node["textureHovered"]) button.textureHovered = decodeTexture(node["textureHovered"]);
    if (node["texturePressed"]) button.texturePressed = decodeTexture(node["texturePressed"]);
    if (node["textureDisabled"]) button.textureDisabled = decodeTexture(node["textureDisabled"]);
    if (node["colorNormal"]) button.colorNormal = decodeVector4(node["colorNormal"]);
    if (node["colorHovered"]) button.colorHovered = decodeVector4(node["colorHovered"]);
    if (node["colorPressed"]) button.colorPressed = decodeVector4(node["colorPressed"]);
    if (node["colorDisabled"]) button.colorDisabled = decodeVector4(node["colorDisabled"]);
    if (node["disabled"]) button.disabled = YamlNumbers::as<bool>(node["disabled"]);

    return button;
}

YAML::Node editor::Stream::encodeScrollbarComponent(const ScrollbarComponent& scrollbar) {
    YAML::Node node;
    node["bar"] = scrollbar.bar;
    node["type"] = scrollbarTypeToString(scrollbar.type);
    node["barSize"] = scrollbar.barSize;
    node["step"] = scrollbar.step;
    node["barMarginLeft"] = scrollbar.barMarginLeft;
    node["barMarginRight"] = scrollbar.barMarginRight;
    node["barMarginTop"] = scrollbar.barMarginTop;
    node["barMarginBottom"] = scrollbar.barMarginBottom;
    return node;
}

ScrollbarComponent editor::Stream::decodeScrollbarComponent(const YAML::Node& node, const ScrollbarComponent* oldScrollbar) {
    ScrollbarComponent scrollbar;

    if (oldScrollbar) {
        scrollbar = *oldScrollbar;
    }

    if (node["bar"]) scrollbar.bar = YamlNumbers::as<Entity>(node["bar"]);
    if (node["type"]) scrollbar.type = stringToScrollbarType(node["type"].as<std::string>());
    if (node["barSize"]) scrollbar.barSize = YamlNumbers::as<float>(node["barSize"]);
    if (node["step"]) scrollbar.step = YamlNumbers::as<float>(node["step"]);
    if (node["barMarginLeft"]) scrollbar.barMarginLeft = YamlNumbers::as<int>(node["barMarginLeft"]);
    if (node["barMarginRight"]) scrollbar.barMarginRight = YamlNumbers::as<int>(node["barMarginRight"]);
    if (node["barMarginTop"]) scrollbar.barMarginTop = YamlNumbers::as<int>(node["barMarginTop"]);
    if (node["barMarginBottom"]) scrollbar.barMarginBottom = YamlNumbers::as<int>(node["barMarginBottom"]);

    scrollbar.needUpdateScrollbar = true;

    return scrollbar;
}

YAML::Node editor::Stream::encodeProgressbarComponent(const ProgressbarComponent& progressbar) {
    YAML::Node node;
    node["fill"] = progressbar.fill;
    node["type"] = progressbarTypeToString(progressbar.type);
    node["value"] = progressbar.value;
    node["fillMarginLeft"] = progressbar.fillMarginLeft;
    node["fillMarginRight"] = progressbar.fillMarginRight;
    node["fillMarginTop"] = progressbar.fillMarginTop;
    node["fillMarginBottom"] = progressbar.fillMarginBottom;
    return node;
}

ProgressbarComponent editor::Stream::decodeProgressbarComponent(const YAML::Node& node, const ProgressbarComponent* oldProgressbar) {
    ProgressbarComponent progressbar;

    if (oldProgressbar) {
        progressbar = *oldProgressbar;
    }

    if (node["fill"]) progressbar.fill = YamlNumbers::as<Entity>(node["fill"]);
    if (node["type"]) progressbar.type = stringToProgressbarType(node["type"].as<std::string>());
    if (node["value"]) progressbar.value = YamlNumbers::as<float>(node["value"]);
    if (node["fillMarginLeft"]) progressbar.fillMarginLeft = YamlNumbers::as<int>(node["fillMarginLeft"]);
    if (node["fillMarginRight"]) progressbar.fillMarginRight = YamlNumbers::as<int>(node["fillMarginRight"]);
    if (node["fillMarginTop"]) progressbar.fillMarginTop = YamlNumbers::as<int>(node["fillMarginTop"]);
    if (node["fillMarginBottom"]) progressbar.fillMarginBottom = YamlNumbers::as<int>(node["fillMarginBottom"]);

    progressbar.needUpdateProgressbar = true;

    return progressbar;
}

YAML::Node editor::Stream::encodeInputFieldComponent(const InputFieldComponent& inputfield) {
    YAML::Node node;
    node["text"] = inputfield.text;
    node["selection"] = inputfield.selection;
    node["cursor"] = inputfield.cursor;
    node["cursorBlink"] = inputfield.cursorBlink;
    node["cursorWidth"] = inputfield.cursorWidth;
    node["cursorColor"] = encodeVector4(inputfield.cursorColor);
    node["selectionColor"] = encodeVector4(inputfield.selectionColor);
    node["placeholderColor"] = encodeVector4(inputfield.placeholderColor);
    node["placeholder"] = inputfield.placeholder;
    node["passwordChar"] = std::string(1, inputfield.passwordChar);
    node["cursorIndex"] = inputfield.cursorIndex;
    node["selectionAnchor"] = inputfield.selectionAnchor;
    node["disabled"] = inputfield.disabled;
    node["password"] = inputfield.password;
    return node;
}

InputFieldComponent editor::Stream::decodeInputFieldComponent(const YAML::Node& node, const InputFieldComponent* oldInputField) {
    InputFieldComponent inputfield;

    if (oldInputField) {
        inputfield = *oldInputField;
    }

    if (node["text"]) inputfield.text = YamlNumbers::as<Entity>(node["text"]);
    if (node["selection"]) inputfield.selection = YamlNumbers::as<Entity>(node["selection"]);
    if (node["cursor"]) inputfield.cursor = YamlNumbers::as<Entity>(node["cursor"]);
    if (node["cursorBlink"]) inputfield.cursorBlink = YamlNumbers::as<float>(node["cursorBlink"]);
    if (node["cursorWidth"]) inputfield.cursorWidth = YamlNumbers::as<float>(node["cursorWidth"]);
    if (node["cursorColor"]) inputfield.cursorColor = decodeVector4(node["cursorColor"]);
    if (node["selectionColor"]) inputfield.selectionColor = decodeVector4(node["selectionColor"]);
    if (node["placeholderColor"]) inputfield.placeholderColor = decodeVector4(node["placeholderColor"]);
    if (node["placeholder"]) inputfield.placeholder = node["placeholder"].as<std::string>();
    if (node["passwordChar"]) {
        std::string passwordChar = node["passwordChar"].as<std::string>();
        inputfield.passwordChar = passwordChar.empty() ? '*' : passwordChar[0];
    }
    if (node["cursorIndex"]) inputfield.cursorIndex = YamlNumbers::as<int>(node["cursorIndex"]);
    if (node["selectionAnchor"]) inputfield.selectionAnchor = YamlNumbers::as<int>(node["selectionAnchor"]);
    if (node["disabled"]) inputfield.disabled = YamlNumbers::as<bool>(node["disabled"]);
    if (node["password"]) inputfield.password = YamlNumbers::as<bool>(node["password"]);

    inputfield.needUpdateInputField = true;

    return inputfield;
}

YAML::Node editor::Stream::encodePanelComponent(const PanelComponent& panel) {
    YAML::Node node;
    node["headerimage"] = panel.headerimage;
    node["headercontainer"] = panel.headercontainer;
    node["headertext"] = panel.headertext;
    node["titleAnchorPreset"] = anchorPresetToString(panel.titleAnchorPreset);
    node["minWidth"] = panel.minWidth;
    node["minHeight"] = panel.minHeight;
    node["headerMarginLeft"] = panel.headerMarginLeft;
    node["headerMarginTop"] = panel.headerMarginTop;
    node["headerMarginRight"] = panel.headerMarginRight;
    node["headerMarginBottom"] = panel.headerMarginBottom;
    node["defaultHeaderMargin"] = panel.defaultHeaderMargin;
    node["resizeMargin"] = panel.resizeMargin;
    node["canMove"] = panel.canMove;
    node["canResize"] = panel.canResize;
    node["canBringToFront"] = panel.canBringToFront;
    return node;
}

PanelComponent editor::Stream::decodePanelComponent(const YAML::Node& node, const PanelComponent* oldPanel) {
    PanelComponent panel;

    if (oldPanel) {
        panel = *oldPanel;
    }

    if (node["headerimage"]) panel.headerimage = YamlNumbers::as<Entity>(node["headerimage"]);
    if (node["headercontainer"]) panel.headercontainer = YamlNumbers::as<Entity>(node["headercontainer"]);
    if (node["headertext"]) panel.headertext = YamlNumbers::as<Entity>(node["headertext"]);
    if (node["titleAnchorPreset"]) panel.titleAnchorPreset = stringToAnchorPreset(node["titleAnchorPreset"].as<std::string>());
    if (node["minWidth"]) panel.minWidth = YamlNumbers::as<unsigned int>(node["minWidth"]);
    if (node["minHeight"]) panel.minHeight = YamlNumbers::as<unsigned int>(node["minHeight"]);
    if (node["headerMarginLeft"]) panel.headerMarginLeft = YamlNumbers::as<int>(node["headerMarginLeft"]);
    if (node["headerMarginTop"]) panel.headerMarginTop = YamlNumbers::as<int>(node["headerMarginTop"]);
    if (node["headerMarginRight"]) panel.headerMarginRight = YamlNumbers::as<int>(node["headerMarginRight"]);
    if (node["headerMarginBottom"]) panel.headerMarginBottom = YamlNumbers::as<int>(node["headerMarginBottom"]);
    if (node["defaultHeaderMargin"]) panel.defaultHeaderMargin = YamlNumbers::as<bool>(node["defaultHeaderMargin"]);
    if (node["resizeMargin"]) panel.resizeMargin = YamlNumbers::as<int>(node["resizeMargin"]);
    if (node["canMove"]) panel.canMove = YamlNumbers::as<bool>(node["canMove"]);
    if (node["canResize"]) panel.canResize = YamlNumbers::as<bool>(node["canResize"]);
    if (node["canBringToFront"]) panel.canBringToFront = YamlNumbers::as<bool>(node["canBringToFront"]);

    panel.needUpdatePanel = true;

    return panel;
}

YAML::Node editor::Stream::encodeRectTransformComponent(const RectTransformComponent& layout) {
    YAML::Node node;
    node["width"] = layout.width;
    node["height"] = layout.height;
    node["anchorPointLeft"] = layout.anchorPointLeft;
    node["anchorPointTop"] = layout.anchorPointTop;
    node["anchorPointRight"] = layout.anchorPointRight;
    node["anchorPointBottom"] = layout.anchorPointBottom;
    node["anchorOffsetLeft"] = layout.anchorOffsetLeft;
    node["anchorOffsetTop"] = layout.anchorOffsetTop;
    node["anchorOffsetRight"] = layout.anchorOffsetRight;
    node["anchorOffsetBottom"] = layout.anchorOffsetBottom;
    node["positionOffset"] = encodeVector2(layout.positionOffset);
    node["pivot"] = encodeVector2(layout.pivot);
    node["anchorPreset"] = anchorPresetToString(layout.anchorPreset);
    node["usingAnchors"] = layout.usingAnchors;
    node["panel"] = layout.panel;
    node["containerBoxIndex"] = layout.containerBoxIndex;
    node["scissor"] = encodeRect(layout.scissor);
    node["ignoreScissor"] = layout.ignoreScissor;
    node["ignoreEvents"] = layout.ignoreEvents;

    return node;
}

RectTransformComponent editor::Stream::decodeRectTransformComponent(const YAML::Node& node, const RectTransformComponent* oldLayout) {
    RectTransformComponent layout;

    // Use old values as defaults if provided
    if (oldLayout) {
        layout = *oldLayout;
    }

    if (node["width"]) layout.width = YamlNumbers::as<unsigned int>(node["width"]);
    if (node["height"]) layout.height = YamlNumbers::as<unsigned int>(node["height"]);
    if (node["anchorPointLeft"]) layout.anchorPointLeft = YamlNumbers::as<float>(node["anchorPointLeft"]);
    if (node["anchorPointTop"]) layout.anchorPointTop = YamlNumbers::as<float>(node["anchorPointTop"]);
    if (node["anchorPointRight"]) layout.anchorPointRight = YamlNumbers::as<float>(node["anchorPointRight"]);
    if (node["anchorPointBottom"]) layout.anchorPointBottom = YamlNumbers::as<float>(node["anchorPointBottom"]);
    if (node["anchorOffsetLeft"]) layout.anchorOffsetLeft = YamlNumbers::as<int>(node["anchorOffsetLeft"]);
    if (node["anchorOffsetTop"]) layout.anchorOffsetTop = YamlNumbers::as<int>(node["anchorOffsetTop"]);
    if (node["anchorOffsetRight"]) layout.anchorOffsetRight = YamlNumbers::as<int>(node["anchorOffsetRight"]);
    if (node["anchorOffsetBottom"]) layout.anchorOffsetBottom = YamlNumbers::as<int>(node["anchorOffsetBottom"]);
    if (node["positionOffset"]) layout.positionOffset = decodeVector2(node["positionOffset"]);
    if (node["pivot"]) layout.pivot = decodeVector2(node["pivot"]);
    if (node["anchorPreset"]) layout.anchorPreset = stringToAnchorPreset(node["anchorPreset"].as<std::string>());
    if (node["usingAnchors"]) layout.usingAnchors = YamlNumbers::as<bool>(node["usingAnchors"]);
    if (node["panel"]) layout.panel = YamlNumbers::as<Entity>(node["panel"]);
    if (node["containerBoxIndex"]) layout.containerBoxIndex = YamlNumbers::as<int>(node["containerBoxIndex"]);
    if (node["scissor"]) layout.scissor = decodeRect(node["scissor"]);
    if (node["ignoreScissor"]) layout.ignoreScissor = YamlNumbers::as<bool>(node["ignoreScissor"]);
    if (node["ignoreEvents"]) layout.ignoreEvents = YamlNumbers::as<bool>(node["ignoreEvents"]);

    return layout;
}

YAML::Node editor::Stream::encodeLayoutGroupComponent(const LayoutGroupComponent& container) {
    YAML::Node node;
    node["type"] = layoutGroupTypeToString(container.type);
    node["useAllWrapSpace"] = container.useAllWrapSpace;
    node["wrapCellWidth"] = container.wrapCellWidth;
    node["wrapCellHeight"] = container.wrapCellHeight;
    return node;
}

LayoutGroupComponent editor::Stream::decodeLayoutGroupComponent(const YAML::Node& node, const LayoutGroupComponent* oldContainer) {
    LayoutGroupComponent container;

    if (oldContainer) {
        container = *oldContainer;
    }

    if (node["type"]) container.type = stringToLayoutGroupType(node["type"].as<std::string>());
    if (node["useAllWrapSpace"]) container.useAllWrapSpace = YamlNumbers::as<bool>(node["useAllWrapSpace"]);
    if (node["fillAvailableSpace"]) container.useAllWrapSpace = YamlNumbers::as<bool>(node["fillAvailableSpace"]);
    if (node["wrapCellWidth"]) container.wrapCellWidth = YamlNumbers::as<unsigned int>(node["wrapCellWidth"]);
    if (node["wrapCellHeight"]) container.wrapCellHeight = YamlNumbers::as<unsigned int>(node["wrapCellHeight"]);

    return container;
}

YAML::Node editor::Stream::encodeTextComponent(const TextComponent& text) {
    YAML::Node node;

    // cut after the last font in use, but always written so a merging decode sees a cleared chain
    size_t numFonts = 0;
    for (size_t i = 0; i < text.font.size(); i++){
        if (!text.font[i].empty()) numFonts = i + 1;
    }
    node["font"] = YAML::Node(YAML::NodeType::Sequence);
    node["font"].SetStyle(YAML::EmitterStyle::Flow);
    for (size_t i = 0; i < numFonts; i++){
        node["font"].push_back(text.font[i]);
    }
    node["text"] = text.text;
    node["fontSize"] = text.fontSize;
    node["multiline"] = text.multiline;
    node["maxTextSize"] = text.maxTextSize;
    node["fixedWidth"] = text.fixedWidth;
    node["fixedHeight"] = text.fixedHeight;
    node["pivotBaseline"] = text.pivotBaseline;
    node["pivotCentered"] = text.pivotCentered;

    return node;
}

TextComponent editor::Stream::decodeTextComponent(const YAML::Node& node, const TextComponent* oldText) {
    TextComponent text;

    if (oldText) {
        text = *oldText;
    }

    if (node["font"]){
        text.font = FontArray();
        if (node["font"].IsSequence()){
            for (size_t i = 0; i < node["font"].size() && i < text.font.size(); i++){
                text.font[i] = node["font"][i].as<std::string>();
            }
        }else{
            // before the array: a single font plus a ";" separated fontFallbacks
            text.font[0] = node["font"].as<std::string>();
        }
    }
    if (node["text"]) text.text = node["text"].as<std::string>();
    if (node["fontSize"]) text.fontSize = YamlNumbers::as<unsigned int>(node["fontSize"]);
    if (node["multiline"]) text.multiline = YamlNumbers::as<bool>(node["multiline"]);
    if (node["maxTextSize"]) text.maxTextSize = YamlNumbers::as<unsigned int>(node["maxTextSize"]);
    if (node["fixedWidth"]) text.fixedWidth = YamlNumbers::as<bool>(node["fixedWidth"]);
    if (node["fixedHeight"]) text.fixedHeight = YamlNumbers::as<bool>(node["fixedHeight"]);
    if (node["pivotBaseline"]) text.pivotBaseline = YamlNumbers::as<bool>(node["pivotBaseline"]);
    if (node["pivotCentered"]) text.pivotCentered = YamlNumbers::as<bool>(node["pivotCentered"]);

    return text;
}

YAML::Node editor::Stream::encodeImageComponent(const ImageComponent& image) {
    YAML::Node node;
    node["patchMarginLeft"] = image.patchMarginLeft;
    node["patchMarginRight"] = image.patchMarginRight;
    node["patchMarginTop"] = image.patchMarginTop;
    node["patchMarginBottom"] = image.patchMarginBottom;
    node["textureScaleFactor"] = image.textureScaleFactor;

    return node;
}

ImageComponent editor::Stream::decodeImageComponent(const YAML::Node& node, const ImageComponent* oldImage) {
    ImageComponent image;

    // Use old values as defaults if provided
    if (oldImage) {
        image = *oldImage;
    }

    if (node["patchMarginLeft"]) image.patchMarginLeft = YamlNumbers::as<int>(node["patchMarginLeft"]);
    if (node["patchMarginRight"]) image.patchMarginRight = YamlNumbers::as<int>(node["patchMarginRight"]);
    if (node["patchMarginTop"]) image.patchMarginTop = YamlNumbers::as<int>(node["patchMarginTop"]);
    if (node["patchMarginBottom"]) image.patchMarginBottom = YamlNumbers::as<int>(node["patchMarginBottom"]);
    if (node["textureScaleFactor"]) image.textureScaleFactor = YamlNumbers::as<float>(node["textureScaleFactor"]);

    return image;
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
    if (node["pivotPreset"]) sprite.pivotPreset = stringToPivotPreset(node["pivotPreset"].as<std::string>());

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
    return layer;
}

YAML::Node editor::Stream::encodeTerrainFoliageLayer(const TerrainFoliageLayer& layer) {
    YAML::Node node;
    node["meshPath"] = layer.meshPath;
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
    if (node["meshPath"]) layer.meshPath = node["meshPath"].as<std::string>();
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
    node["filename"] = model.filename;
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
    if (node["filename"]) model.filename = node["filename"].as<std::string>();
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

YAML::Node editor::Stream::encodeLightComponent(const LightComponent& light) {
    YAML::Node node;
    node["enabled"] = light.enabled;

    node["type"] = lightTypeToString(light.type);
    node["direction"] = encodeVector3(light.direction);
    node["worldDirection"] = encodeVector3(light.worldDirection);
    node["color"] = encodeVector3(light.color);
    node["range"] = light.range;
    node["intensity"] = light.intensity;
    node["innerConeCos"] = light.innerConeCos;
    node["outerConeCos"] = light.outerConeCos;
    node["spotMask"] = encodeTexture(light.spotMask);
    node["cullingMask"] = light.cullingMask;
    node["shadows"] = light.shadows;
    node["automaticShadowCamera"] = light.automaticShadowCamera;
    node["shadowBias"] = light.shadowBias;
    node["mapResolution"] = light.mapResolution;
    node["shadowCameraNearFar"] = encodeVector2(light.shadowCameraNearFar);
    node["numShadowCascades"] = light.numShadowCascades;
    if (light.shading != LightShading::BOTH)
        node["shading"] = lightShadingToString(light.shading);

    return node;
}

LightComponent editor::Stream::decodeLightComponent(const YAML::Node& node, const LightComponent* oldLight) {
    LightComponent light;

    // Use old values as defaults if provided
    if (oldLight) {
        light = *oldLight;
    }

    if (node["type"]) light.type = stringToLightType(node["type"].as<std::string>());
    if (node["direction"]) light.direction = decodeVector3(node["direction"]);
    if (node["worldDirection"]) light.worldDirection = decodeVector3(node["worldDirection"]);
    if (node["color"]) light.color = decodeVector3(node["color"]);
    if (node["range"]) light.range = YamlNumbers::as<float>(node["range"]);
    if (node["intensity"]) light.intensity = YamlNumbers::as<float>(node["intensity"]);
    if (node["innerConeCos"]) light.innerConeCos = YamlNumbers::as<float>(node["innerConeCos"]);
    if (node["outerConeCos"]) light.outerConeCos = YamlNumbers::as<float>(node["outerConeCos"]);
    if (node["spotMask"]) light.spotMask = decodeTexture(node["spotMask"]);
    if (node["cullingMask"]) light.cullingMask = decodeLayerMask(node["cullingMask"], light.cullingMask);
    if (node["shadows"]) light.shadows = YamlNumbers::as<bool>(node["shadows"]);
    if (node["automaticShadowCamera"]) light.automaticShadowCamera = YamlNumbers::as<bool>(node["automaticShadowCamera"]);
    if (node["shadowBias"]) light.shadowBias = YamlNumbers::as<float>(node["shadowBias"]);
    if (node["mapResolution"]) light.mapResolution = YamlNumbers::as<unsigned int>(node["mapResolution"]);
    if (node["shadowCameraNearFar"]) light.shadowCameraNearFar = decodeVector2(node["shadowCameraNearFar"]);
    if (node["numShadowCascades"]) light.numShadowCascades = YamlNumbers::as<unsigned int>(node["numShadowCascades"]);
    // omitted at its default, so an absent key is the default
    light.shading = node["shading"] ? stringToLightShading(node["shading"].as<std::string>()) : LightShading::BOTH;

    if (node["enabled"]) light.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return light;
}

YAML::Node editor::Stream::encodeLight2DComponent(const Light2DComponent& light) {
    YAML::Node node;
    node["enabled"] = light.enabled;

    node["color"] = encodeVector3(light.color);
    node["intensity"] = light.intensity;
    node["range"] = light.range;
    node["falloff"] = light.falloff;
    node["height"] = light.height;
    node["shadows"] = light.shadows;
    node["shadowBias"] = light.shadowBias;
    node["shadowSoftness"] = light.shadowSoftness;
    node["mapResolution"] = light.mapResolution;

    return node;
}

Light2DComponent editor::Stream::decodeLight2DComponent(const YAML::Node& node, const Light2DComponent* oldLight) {
    Light2DComponent light;

    // Use old values as defaults if provided
    if (oldLight) {
        light = *oldLight;
    }

    if (node["color"]) light.color = decodeVector3(node["color"]);
    if (node["intensity"]) light.intensity = YamlNumbers::as<float>(node["intensity"]);
    if (node["range"]) light.range = YamlNumbers::as<float>(node["range"]);
    if (node["falloff"]) light.falloff = YamlNumbers::as<float>(node["falloff"]);
    if (node["height"]) light.height = YamlNumbers::as<float>(node["height"]);
    if (node["shadows"]) light.shadows = YamlNumbers::as<bool>(node["shadows"]);
    if (node["shadowBias"]) light.shadowBias = YamlNumbers::as<float>(node["shadowBias"]);
    if (node["shadowSoftness"]) light.shadowSoftness = YamlNumbers::as<float>(node["shadowSoftness"]);
    if (node["mapResolution"]) light.mapResolution = YamlNumbers::as<unsigned int>(node["mapResolution"]);

    if (node["enabled"]) light.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return light;
}

static std::string shadowCaster2DShapeToString(ShadowCaster2DShape shape) {
    switch (shape) {
        case ShadowCaster2DShape::AUTO_QUAD: return "autoquad";
        case ShadowCaster2DShape::POLYGON: return "polygon";
    }
    return "autoquad";
}

static ShadowCaster2DShape stringToShadowCaster2DShape(const std::string& str) {
    if (str == "polygon") return ShadowCaster2DShape::POLYGON;
    return ShadowCaster2DShape::AUTO_QUAD;
}

YAML::Node editor::Stream::encodeShadowCaster2DComponent(const ShadowCaster2DComponent& occluder) {
    YAML::Node node;

    node["shape"] = shadowCaster2DShapeToString(occluder.shape);
    node["closed"] = occluder.closed;
    node["enabled"] = occluder.enabled;

    YAML::Node pointsNode(YAML::NodeType::Sequence);
    for (const Vector2& point : occluder.points) {
        pointsNode.push_back(encodeVector2(point));
    }
    node["points"] = pointsNode;

    return node;
}

ShadowCaster2DComponent editor::Stream::decodeShadowCaster2DComponent(const YAML::Node& node, const ShadowCaster2DComponent* oldOccluder) {
    ShadowCaster2DComponent occluder;

    if (oldOccluder) {
        occluder = *oldOccluder;
    }

    if (node["shape"]) occluder.shape = stringToShadowCaster2DShape(node["shape"].as<std::string>());
    if (node["closed"]) occluder.closed = YamlNumbers::as<bool>(node["closed"]);
    if (node["enabled"]) occluder.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["points"]) {
        occluder.points.clear();
        for (const auto& pointNode : node["points"]) {
            occluder.points.push_back(decodeVector2(pointNode));
        }
    }

    return occluder;
}

YAML::Node editor::Stream::encodeFogComponent(const FogComponent& fog) {
    YAML::Node node;

    node["type"] = fogTypeToString(fog.type);
    node["color"] = encodeVector3(fog.color);
    node["density"] = fog.density;
    node["linearStart"] = fog.linearStart;
    node["linearEnd"] = fog.linearEnd;

    return node;
}

FogComponent editor::Stream::decodeFogComponent(const YAML::Node& node, const FogComponent* oldFog) {
    FogComponent fog;

    if (oldFog) {
        fog = *oldFog;
    }

    if (node["type"]) fog.type = stringToFogType(node["type"].as<std::string>());
    if (node["color"]) fog.color = decodeVector3(node["color"]);
    if (node["density"]) fog.density = YamlNumbers::as<float>(node["density"]);
    if (node["linearStart"]) fog.linearStart = YamlNumbers::as<float>(node["linearStart"]);
    if (node["linearEnd"]) fog.linearEnd = YamlNumbers::as<float>(node["linearEnd"]);

    return fog;
}

YAML::Node editor::Stream::encodeMirrorComponent(const MirrorComponent& mirror) {
    YAML::Node node;

    node["normal"] = encodeVector3(mirror.normal);

    return node;
}

MirrorComponent editor::Stream::decodeMirrorComponent(const YAML::Node& node, const MirrorComponent* oldMirror) {
    MirrorComponent mirror;

    if (oldMirror) {
        mirror = *oldMirror;
    }

    if (node["normal"]) mirror.normal = decodeVector3(node["normal"]);

    return mirror;
}

YAML::Node editor::Stream::encodeReflectionProbeComponent(const ReflectionProbeComponent& probe) {
    YAML::Node node;

    node["mode"] = probe.mode == ReflectionProbeMode::DYNAMIC ? "dynamic" : "static";
    switch (probe.updateMode) {
        case ReflectionProbeUpdateMode::ON_MOVE: node["updateMode"] = "on_move"; break;
        case ReflectionProbeUpdateMode::INTERVAL: node["updateMode"] = "interval"; break;
        case ReflectionProbeUpdateMode::MANUAL: node["updateMode"] = "manual"; break;
        default: node["updateMode"] = "on_load"; break;
    }
    node["boxOffset"] = encodeVector3(probe.boxOffset);
    node["boxSize"] = encodeVector3(probe.boxSize);
    node["blendDistance"] = probe.blendDistance;
    node["intensity"] = probe.intensity;
    node["priority"] = probe.priority;
    node["resolution"] = probe.resolution;
    node["nearClip"] = probe.nearClip;
    node["farClip"] = probe.farClip;
    node["updateInterval"] = probe.updateInterval;
    node["includeSky"] = probe.includeSky;
    node["cullingMask"] = probe.cullingMask;
    node["texture"] = encodeTexture(probe.texture);

    return node;
}

YAML::Node editor::Stream::encodeUIDocumentComponent(const UIDocumentComponent& document) {
    YAML::Node node;
    node["enabled"] = document.enabled;

    // written when empty too: decoding over a component with a file must clear it
    node["filename"] = document.filename;
    node["sortOrder"] = document.sortOrder;

    // phase 1: only what differs from the defaults (decode restores the defaults)
    const UIDocumentComponent defaults;
    if (document.interactive != defaults.interactive) node["interactive"] = document.interactive;
    if (document.renderMode != defaults.renderMode) node["renderMode"] = uiDocumentRenderModeToString(document.renderMode);
    if (document.textureWidth != defaults.textureWidth) node["textureWidth"] = document.textureWidth;
    if (document.textureHeight != defaults.textureHeight) node["textureHeight"] = document.textureHeight;
    if (document.inputSurface != defaults.inputSurface) node["inputSurface"] = document.inputSurface;
    if (document.pixelRatio != defaults.pixelRatio) encodeFinite(node, "pixelRatio", document.pixelRatio);

    return node;
}

UIDocumentComponent editor::Stream::decodeUIDocumentComponent(const YAML::Node& node, const UIDocumentComponent* oldDocument) {
    // the old one's runtime state (its loaded document, onLoad/onUnload) stays while the file
    // and the render settings are the same
    UIDocumentComponent document;

    if (oldDocument) {
        document = *oldDocument;
    }

    if (node["filename"]) document.filename = node["filename"].IsNull() ? std::string() : node["filename"].as<std::string>();
    if (node["sortOrder"]) document.sortOrder = YamlNumbers::as<int>(node["sortOrder"]);
    if (node["enabled"]) document.enabled = YamlNumbers::as<bool>(node["enabled"]);

    // phase-1 keys are omitted at their defaults, so a missing key is the default, not the
    // old value (paste and play-stop restore decode over an existing component); the
    // clipboard fills a key it has no default for with an empty scalar, which is the default too
    const UIDocumentComponent defaults;
    auto given = [&node](const char* key) {
        const YAML::Node value = node[key];
        return value && !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    };
    document.interactive = given("interactive") ? YamlNumbers::as<bool>(node["interactive"]) : defaults.interactive;
    document.renderMode = given("renderMode") ? stringToUIDocumentRenderMode(node["renderMode"].as<std::string>()) : defaults.renderMode;
    document.textureWidth = given("textureWidth") ? std::clamp(YamlNumbers::as<int>(node["textureWidth"]), 1, 8192) : defaults.textureWidth;
    document.textureHeight = given("textureHeight") ? std::clamp(YamlNumbers::as<int>(node["textureHeight"]), 1, 8192) : defaults.textureHeight;
    document.inputSurface = given("inputSurface") ? YamlNumbers::as<Entity>(node["inputSurface"]) : defaults.inputSurface;
    document.pixelRatio = given("pixelRatio") ? std::max(0.0f, decodeFinite(node["pixelRatio"], defaults.pixelRatio)) : defaults.pixelRatio;

    if (oldDocument && (oldDocument->filename != document.filename || oldDocument->renderMode != document.renderMode ||
                        oldDocument->textureWidth != document.textureWidth || oldDocument->textureHeight != document.textureHeight ||
                        oldDocument->pixelRatio != document.pixelRatio)) {
        document.needReload = true;
    }

    return document;
}

YAML::Node editor::Stream::encodeFluidSimulationComponent(const FluidSimulationComponent& fluid) {
    YAML::Node node;
    node["enabled"] = fluid.enabled;

    // only what differs from the defaults (Pavel's); decode restores the defaults
    const FluidSimulationComponent defaults;
    if (fluid.output != defaults.output) node["output"] = fluidOutputToString(fluid.output);
    if (fluid.interactive != defaults.interactive) node["interactive"] = fluid.interactive;
    if (fluid.aspectRatio != defaults.aspectRatio) encodeFinite(node, "aspectRatio", fluid.aspectRatio);
    if (fluid.simResolution != defaults.simResolution) node["simResolution"] = fluid.simResolution;
    if (fluid.dyeResolution != defaults.dyeResolution) node["dyeResolution"] = fluid.dyeResolution;
    if (fluid.densityDissipation != defaults.densityDissipation) encodeFinite(node, "densityDissipation", fluid.densityDissipation);
    if (fluid.velocityDissipation != defaults.velocityDissipation) encodeFinite(node, "velocityDissipation", fluid.velocityDissipation);
    if (fluid.pressure != defaults.pressure) encodeFinite(node, "pressure", fluid.pressure);
    if (fluid.pressureIterations != defaults.pressureIterations) node["pressureIterations"] = fluid.pressureIterations;
    if (fluid.curl != defaults.curl) encodeFinite(node, "curl", fluid.curl);
    if (fluid.gradientScale != defaults.gradientScale) encodeFinite(node, "gradientScale", fluid.gradientScale);
    if (fluid.simulationRate != defaults.simulationRate) encodeFinite(node, "simulationRate", fluid.simulationRate);
    if (fluid.maxSubsteps != defaults.maxSubsteps) node["maxSubsteps"] = fluid.maxSubsteps;
    if (fluid.timeScale != defaults.timeScale) encodeFinite(node, "timeScale", fluid.timeScale);
    if (fluid.paused != defaults.paused) node["paused"] = fluid.paused;
    if (fluid.splatRadius != defaults.splatRadius) encodeFinite(node, "splatRadius", fluid.splatRadius);
    if (fluid.splatForce != defaults.splatForce) encodeFinite(node, "splatForce", fluid.splatForce);
    if (fluid.colorful != defaults.colorful) node["colorful"] = fluid.colorful;
    if (fluid.colorUpdateSpeed != defaults.colorUpdateSpeed) encodeFinite(node, "colorUpdateSpeed", fluid.colorUpdateSpeed);
    if (fluid.splatsOnStart != defaults.splatsOnStart) node["splatsOnStart"] = fluid.splatsOnStart;
    if (fluid.randomSplatCount != defaults.randomSplatCount) node["randomSplatCount"] = fluid.randomSplatCount;
    if (fluid.seed != defaults.seed) node["seed"] = fluid.seed;
    if (fluid.shading != defaults.shading) node["shading"] = fluid.shading;
    if (fluid.backColor != defaults.backColor) encodeFiniteVector3(node, "backColor", fluid.backColor);
    if (fluid.transparent != defaults.transparent) node["transparent"] = fluid.transparent;
    if (fluid.bloom != defaults.bloom) node["bloom"] = fluid.bloom;
    if (fluid.bloomIterations != defaults.bloomIterations) node["bloomIterations"] = fluid.bloomIterations;
    if (fluid.bloomResolution != defaults.bloomResolution) node["bloomResolution"] = fluid.bloomResolution;
    if (fluid.bloomIntensity != defaults.bloomIntensity) encodeFinite(node, "bloomIntensity", fluid.bloomIntensity);
    if (fluid.bloomThreshold != defaults.bloomThreshold) encodeFinite(node, "bloomThreshold", fluid.bloomThreshold);
    if (fluid.bloomSoftKnee != defaults.bloomSoftKnee) encodeFinite(node, "bloomSoftKnee", fluid.bloomSoftKnee);
    if (fluid.sunrays != defaults.sunrays) node["sunrays"] = fluid.sunrays;
    if (fluid.sunraysResolution != defaults.sunraysResolution) node["sunraysResolution"] = fluid.sunraysResolution;
    if (fluid.sunraysWeight != defaults.sunraysWeight) encodeFinite(node, "sunraysWeight", fluid.sunraysWeight);

    return node;
}

FluidSimulationComponent editor::Stream::decodeFluidSimulationComponent(const YAML::Node& node, const FluidSimulationComponent* oldFluid) {
    // the old one's runtime state (its simulator, which textures linked to "fluid|<entity>"
    // show, and what is queued for it) stays
    FluidSimulationComponent fluid;
    if (oldFluid) {
        fluid = *oldFluid;
    }

    if (node["enabled"]) fluid.enabled = YamlNumbers::as<bool>(node["enabled"]);

    // keys are omitted at their defaults, so a missing key is the default, not the old value
    // (paste and play-stop restore decode over an existing component); the clipboard fills a
    // key it has no default for with an empty scalar, which is the default too
    const FluidSimulationComponent defaults;
    auto given = [&node](const char* key) {
        const YAML::Node value = node[key];
        return value && !value.IsNull() && !(value.IsScalar() && value.Scalar().empty());
    };
    fluid.output = given("output") ? stringToFluidOutput(node["output"].as<std::string>()) : defaults.output;
    fluid.interactive = given("interactive") ? YamlNumbers::as<bool>(node["interactive"]) : defaults.interactive;
    fluid.aspectRatio = given("aspectRatio") ? decodeFinite(node["aspectRatio"], defaults.aspectRatio) : defaults.aspectRatio;
    fluid.simResolution = given("simResolution") ? YamlNumbers::as<unsigned int>(node["simResolution"]) : defaults.simResolution;
    fluid.dyeResolution = given("dyeResolution") ? YamlNumbers::as<unsigned int>(node["dyeResolution"]) : defaults.dyeResolution;
    fluid.densityDissipation = given("densityDissipation") ? decodeFinite(node["densityDissipation"], defaults.densityDissipation) : defaults.densityDissipation;
    fluid.velocityDissipation = given("velocityDissipation") ? decodeFinite(node["velocityDissipation"], defaults.velocityDissipation) : defaults.velocityDissipation;
    fluid.pressure = given("pressure") ? decodeFinite(node["pressure"], defaults.pressure) : defaults.pressure;
    fluid.pressureIterations = given("pressureIterations") ? YamlNumbers::as<unsigned int>(node["pressureIterations"]) : defaults.pressureIterations;
    fluid.curl = given("curl") ? decodeFinite(node["curl"], defaults.curl) : defaults.curl;
    fluid.gradientScale = given("gradientScale") ? decodeFinite(node["gradientScale"], defaults.gradientScale) : defaults.gradientScale;
    fluid.simulationRate = given("simulationRate") ? decodeFinite(node["simulationRate"], defaults.simulationRate) : defaults.simulationRate;
    fluid.maxSubsteps = given("maxSubsteps") ? YamlNumbers::as<unsigned int>(node["maxSubsteps"]) : defaults.maxSubsteps;
    fluid.timeScale = given("timeScale") ? decodeFinite(node["timeScale"], defaults.timeScale) : defaults.timeScale;
    fluid.paused = given("paused") ? YamlNumbers::as<bool>(node["paused"]) : defaults.paused;
    fluid.splatRadius = given("splatRadius") ? decodeFinite(node["splatRadius"], defaults.splatRadius) : defaults.splatRadius;
    fluid.splatForce = given("splatForce") ? decodeFinite(node["splatForce"], defaults.splatForce) : defaults.splatForce;
    fluid.colorful = given("colorful") ? YamlNumbers::as<bool>(node["colorful"]) : defaults.colorful;
    fluid.colorUpdateSpeed = given("colorUpdateSpeed") ? decodeFinite(node["colorUpdateSpeed"], defaults.colorUpdateSpeed) : defaults.colorUpdateSpeed;
    fluid.splatsOnStart = given("splatsOnStart") ? YamlNumbers::as<bool>(node["splatsOnStart"]) : defaults.splatsOnStart;
    fluid.randomSplatCount = given("randomSplatCount") ? YamlNumbers::as<unsigned int>(node["randomSplatCount"]) : defaults.randomSplatCount;
    fluid.seed = given("seed") ? YamlNumbers::as<uint32_t>(node["seed"]) : defaults.seed;
    fluid.shading = given("shading") ? YamlNumbers::as<bool>(node["shading"]) : defaults.shading;
    fluid.backColor = given("backColor") ? decodeFiniteVector3(node["backColor"], defaults.backColor) : defaults.backColor;
    fluid.transparent = given("transparent") ? YamlNumbers::as<bool>(node["transparent"]) : defaults.transparent;
    fluid.bloom = given("bloom") ? YamlNumbers::as<bool>(node["bloom"]) : defaults.bloom;
    fluid.bloomIterations = given("bloomIterations") ? YamlNumbers::as<unsigned int>(node["bloomIterations"]) : defaults.bloomIterations;
    fluid.bloomResolution = given("bloomResolution") ? YamlNumbers::as<unsigned int>(node["bloomResolution"]) : defaults.bloomResolution;
    fluid.bloomIntensity = given("bloomIntensity") ? decodeFinite(node["bloomIntensity"], defaults.bloomIntensity) : defaults.bloomIntensity;
    fluid.bloomThreshold = given("bloomThreshold") ? decodeFinite(node["bloomThreshold"], defaults.bloomThreshold) : defaults.bloomThreshold;
    fluid.bloomSoftKnee = given("bloomSoftKnee") ? decodeFinite(node["bloomSoftKnee"], defaults.bloomSoftKnee) : defaults.bloomSoftKnee;
    fluid.sunrays = given("sunrays") ? YamlNumbers::as<bool>(node["sunrays"]) : defaults.sunrays;
    fluid.sunraysResolution = given("sunraysResolution") ? YamlNumbers::as<unsigned int>(node["sunraysResolution"]) : defaults.sunraysResolution;
    fluid.sunraysWeight = given("sunraysWeight") ? decodeFinite(node["sunraysWeight"], defaults.sunraysWeight) : defaults.sunraysWeight;

    // what the start depends on: a new seed or start burst restarts it (sizes and the look
    // are read by FluidSimulator each frame)
    if (oldFluid && (oldFluid->seed != fluid.seed || oldFluid->splatsOnStart != fluid.splatsOnStart ||
                     oldFluid->randomSplatCount != fluid.randomSplatCount)) {
        fluid.needReset = true;
    }

    return fluid;
}

YAML::Node editor::Stream::encodePoseAnimatorComponent(const PoseAnimatorComponent& animator) {
    YAML::Node node;
    // written when empty too: decoding over a component with an animation must clear it
    node["animation"] = animator.animation;
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
    animator.animation = given("animation") ? node["animation"].as<std::string>() : std::string();
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
        clipsNode.push_back(entryNode);
    }
    node["clips"] = clipsNode;
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
            } else {
                // a list, or a map whose clip is not a path
                continue;
            }
            clips.push_back(std::move(entry));
        }
    }
    player.clips = std::move(clips);

    player.defaultClip = given("defaultClip") && node["defaultClip"].IsScalar() ? node["defaultClip"].Scalar() : std::string();
    player.playAutomatically = boolean("playAutomatically", defaults.playAutomatically);
    player.speed = given("speed") ? decodeFinite(node["speed"], defaults.speed) : defaults.speed;
    player.defaultFadeTime = given("defaultFadeTime") ? std::max(0.0f, decodeFinite(node["defaultFadeTime"], defaults.defaultFadeTime)) : defaults.defaultFadeTime;

    if (oldPlayer && oldPlayer->clips != player.clips) {
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

ReflectionProbeComponent editor::Stream::decodeReflectionProbeComponent(const YAML::Node& node, const ReflectionProbeComponent* oldProbe) {
    ReflectionProbeComponent probe;
    if (oldProbe) probe = *oldProbe;

    if (node["mode"]) {
        probe.mode = node["mode"].as<std::string>() == "dynamic" ? ReflectionProbeMode::DYNAMIC : ReflectionProbeMode::STATIC;
    }
    if (node["updateMode"]) {
        const std::string value = node["updateMode"].as<std::string>();
        if (value == "on_move") probe.updateMode = ReflectionProbeUpdateMode::ON_MOVE;
        else if (value == "interval") probe.updateMode = ReflectionProbeUpdateMode::INTERVAL;
        else if (value == "manual") probe.updateMode = ReflectionProbeUpdateMode::MANUAL;
        else probe.updateMode = ReflectionProbeUpdateMode::ON_LOAD;
    }
    if (node["boxOffset"]) probe.boxOffset = decodeVector3(node["boxOffset"]);
    if (node["boxSize"]) probe.boxSize = decodeVector3(node["boxSize"]);
    if (node["blendDistance"]) probe.blendDistance = YamlNumbers::as<float>(node["blendDistance"]);
    if (node["intensity"]) probe.intensity = YamlNumbers::as<float>(node["intensity"]);
    if (node["priority"]) probe.priority = YamlNumbers::as<int>(node["priority"]);
    if (node["resolution"]) probe.resolution = YamlNumbers::as<unsigned int>(node["resolution"]);
    if (node["nearClip"]) probe.nearClip = YamlNumbers::as<float>(node["nearClip"]);
    if (node["farClip"]) probe.farClip = YamlNumbers::as<float>(node["farClip"]);
    if (node["updateInterval"]) probe.updateInterval = YamlNumbers::as<float>(node["updateInterval"]);
    if (node["includeSky"]) probe.includeSky = YamlNumbers::as<bool>(node["includeSky"]);
    if (node["cullingMask"]) probe.cullingMask = decodeLayerMask(node["cullingMask"], probe.cullingMask);
    if (node["texture"]) probe.texture = decodeTexture(node["texture"]);
    probe.needUpdate = true;

    return probe;
}

YAML::Node editor::Stream::encodeCameraComponent(const CameraComponent& camera) {
    YAML::Node node;
    node["enabled"] = camera.enabled;

    node["type"] = cameraTypeToString(camera.type);
    node["cullingMask"] = camera.cullingMask;
    encodeFiniteVector3(node, "target", camera.target);
    encodeFiniteVector3(node, "up", camera.up);
    encodeFinite(node, "leftClip", camera.leftClip);
    encodeFinite(node, "rightClip", camera.rightClip);
    encodeFinite(node, "bottomClip", camera.bottomClip);
    encodeFinite(node, "topClip", camera.topClip);
    encodePositiveFinite(node, "yfov", camera.yfov);
    encodePositiveFinite(node, "aspect", camera.aspect);
    encodeFinite(node, "nearClip", camera.nearClip);
    encodeFinite(node, "farClip", camera.farClip);
    node["renderToTexture"] = camera.renderToTexture;
    node["transparentSort"] = camera.transparentSort;
    node["depthTest"] = camera.depthTest;
    node["useTarget"] = camera.useTarget;
    node["autoResize"] = camera.autoResize;
    node["framebufferWidth"] = camera.framebufferWidth;
    node["framebufferHeight"] = camera.framebufferHeight;
    node["framebufferFilter"] = textureFilterToString(camera.framebufferFilter);

    return node;
}

CameraComponent editor::Stream::decodeCameraComponent(const YAML::Node& node, const CameraComponent* oldCamera) {
    CameraComponent camera;

    // Use old values as defaults if provided
    if (oldCamera) {
        camera = *oldCamera;
    }

    if (node["type"]) camera.type = stringToCameraType(node["type"].as<std::string>());
    if (node["cullingMask"]) camera.cullingMask = decodeLayerMask(node["cullingMask"], camera.cullingMask);
    camera.target = decodeFiniteVector3(node["target"], camera.target);
    camera.up = decodeFiniteVector3(node["up"], camera.up);
    camera.leftClip = decodeFinite(node["leftClip"], camera.leftClip);
    camera.rightClip = decodeFinite(node["rightClip"], camera.rightClip);
    camera.bottomClip = decodeFinite(node["bottomClip"], camera.bottomClip);
    camera.topClip = decodeFinite(node["topClip"], camera.topClip);
    camera.yfov = decodePositiveFinite(node["yfov"], camera.yfov);
    camera.aspect = decodePositiveFinite(node["aspect"], camera.aspect);
    camera.nearClip = decodeFinite(node["nearClip"], camera.nearClip);
    camera.farClip = decodeFinite(node["farClip"], camera.farClip);
    if (node["renderToTexture"]) camera.renderToTexture = YamlNumbers::as<bool>(node["renderToTexture"]);
    if (node["transparentSort"]) camera.transparentSort = YamlNumbers::as<bool>(node["transparentSort"]);
    if (node["depthTest"]) camera.depthTest = YamlNumbers::as<bool>(node["depthTest"]);
    if (node["useTarget"]) camera.useTarget = YamlNumbers::as<bool>(node["useTarget"]);
    if (node["autoResize"]) camera.autoResize = YamlNumbers::as<bool>(node["autoResize"]);
    if (node["framebufferWidth"]) camera.framebufferWidth = YamlNumbers::as<unsigned int>(node["framebufferWidth"]);
    if (node["framebufferHeight"]) camera.framebufferHeight = YamlNumbers::as<unsigned int>(node["framebufferHeight"]);
    if (node["framebufferFilter"]) camera.framebufferFilter = stringToTextureFilter(node["framebufferFilter"].as<std::string>());

    if (node["enabled"]) camera.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return camera;
}

YAML::Node editor::Stream::encodeAudioSourceComponent(const AudioSourceComponent& audio) {
    YAML::Node node;
    node["enabled"] = audio.enabled;

    node["state"] = audioSourceStateToString(audio.state);
    if (!audio.filename.empty()) node["filename"] = audio.filename;
    node["enableClocked"] = audio.enableClocked;
    encodeFinite(node, "volume", audio.volume);
    encodeFinite(node, "speed", audio.speed);
    encodeFinite(node, "pan", audio.pan);
    node["looping"] = audio.looping;
    encodeFinite(node, "loopingPoint", audio.loopingPoint);
    node["protectVoice"] = audio.protectVoice;
    node["inaudibleBehaviorMustTick"] = audio.inaudibleBehaviorMustTick;
    node["inaudibleBehaviorKill"] = audio.inaudibleBehaviorKill;
    encodeFinite(node, "minDistance", audio.minDistance);
    encodeFinite(node, "maxDistance", audio.maxDistance);
    node["attenuationModel"] = audioSourceAttenuationToString(audio.attenuationModel);
    encodeFinite(node, "attenuationRolloffFactor", audio.attenuationRolloffFactor);
    encodeFinite(node, "dopplerFactor", audio.dopplerFactor);

    return node;
}

AudioSourceComponent editor::Stream::decodeAudioSourceComponent(const YAML::Node& node, const AudioSourceComponent* oldAudio) {
    AudioSourceComponent audio;
    std::string oldFilename;

    if (oldAudio) {
        audio = *oldAudio;
        oldFilename = oldAudio->filename;
    }

    if (node["state"]) audio.state = stringToAudioSourceState(node["state"].as<std::string>());
    if (node["filename"]) audio.filename = node["filename"].as<std::string>();
    if (node["enableClocked"]) audio.enableClocked = YamlNumbers::as<bool>(node["enableClocked"]);
    if (node["volume"]) audio.volume = decodeFiniteDouble(node["volume"], audio.volume);
    if (node["speed"]) audio.speed = decodeFinite(node["speed"], audio.speed);
    if (node["pan"]) audio.pan = decodeFinite(node["pan"], audio.pan);
    if (node["looping"]) audio.looping = YamlNumbers::as<bool>(node["looping"]);
    if (node["loopingPoint"]) audio.loopingPoint = decodeFiniteDouble(node["loopingPoint"], audio.loopingPoint);
    if (node["protectVoice"]) audio.protectVoice = YamlNumbers::as<bool>(node["protectVoice"]);
    if (node["inaudibleBehaviorMustTick"]) audio.inaudibleBehaviorMustTick = YamlNumbers::as<bool>(node["inaudibleBehaviorMustTick"]);
    if (node["inaudibleBehaviorKill"]) audio.inaudibleBehaviorKill = YamlNumbers::as<bool>(node["inaudibleBehaviorKill"]);
    if (node["minDistance"]) audio.minDistance = decodeFinite(node["minDistance"], audio.minDistance);
    if (node["maxDistance"]) audio.maxDistance = decodeFinite(node["maxDistance"], audio.maxDistance);
    if (node["attenuationModel"]) audio.attenuationModel = stringToAudioSourceAttenuation(node["attenuationModel"].as<std::string>());
    if (node["attenuationRolloffFactor"]) audio.attenuationRolloffFactor = decodeFinite(node["attenuationRolloffFactor"], audio.attenuationRolloffFactor);
    if (node["dopplerFactor"]) audio.dopplerFactor = decodeFinite(node["dopplerFactor"], audio.dopplerFactor);

    bool keepLoadedSample = oldAudio && oldFilename == audio.filename && oldAudio->sample && oldAudio->loaded;
    if (!keepLoadedSample && oldAudio && !oldFilename.empty()) {
        audio.sample.reset();
        SoundPool::remove(oldFilename);
    }
    audio.loaded = keepLoadedSample;
    audio.loadFailed = false;
    audio.length = keepLoadedSample ? oldAudio->length : 0;
    audio.handle = 0;
    audio.playingTime = 0;
    audio.lastPosition = Vector3(0, 0, 0);
    audio.startTrigger = false;
    audio.pauseTrigger = false;
    audio.stopTrigger = false;
    audio.needUpdate = true;

    if (node["enabled"]) audio.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return audio;
}

YAML::Node editor::Stream::encodeScriptComponent(const ScriptComponent& script) {
    YAML::Node node;

    if (!script.scripts.empty()) {
        YAML::Node scriptsNode;
        for (const auto& scriptEntry : script.scripts) {
            YAML::Node scriptNode;

            scriptNode["type"] = scriptTypeToString(scriptEntry.type);

            if (!scriptEntry.path.empty())
                scriptNode["path"] = scriptEntry.path;

            if (!scriptEntry.headerPath.empty())
                scriptNode["headerPath"] = scriptEntry.headerPath;

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
            std::string typeStr = scriptNode["type"].as<std::string>();
            entry.type = stringToScriptType(typeStr);
        }

        if (scriptNode["path"])
            entry.path = scriptNode["path"].as<std::string>();

        if (scriptNode["headerPath"])
            entry.headerPath = scriptNode["headerPath"].as<std::string>();

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
        node["customShader"] = sky.customShader;
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
            if (texNode["minFilter"]) sky.texture.setMinFilter(stringToTextureFilter(texNode["minFilter"].as<std::string>()));
            if (texNode["magFilter"]) sky.texture.setMagFilter(stringToTextureFilter(texNode["magFilter"].as<std::string>()));
            if (texNode["wrapU"]) sky.texture.setWrapU(stringToTextureWrap(texNode["wrapU"].as<std::string>()));
            if (texNode["wrapV"]) sky.texture.setWrapV(stringToTextureWrap(texNode["wrapV"].as<std::string>()));
            if (texNode["releaseDataAfterLoad"]) sky.texture.setReleaseDataAfterLoad(YamlNumbers::as<bool>(texNode["releaseDataAfterLoad"]));
        } else {
            sky.texture = decodeTexture(node["texture"]);
        }
    }
    if (node["color"]) sky.color = decodeVector4(node["color"]);
    if (node["rotation"]) sky.rotation = YamlNumbers::as<float>(node["rotation"]);
    if (node["visible"]) sky.visible = YamlNumbers::as<bool>(node["visible"]);

    if (node["customShader"]) sky.customShader = node["customShader"].as<std::string>();
    sky.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    sky.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, sky.shaderTextures, sky.shaderFeatures, nullptr, nullptr);

    return sky;
}

// ==============================
// Body/Joint type string converters
// ==============================

std::string editor::Stream::bodyTypeToString(BodyType type) {
    switch (type) {
        case BodyType::STATIC: return "static";
        case BodyType::KINEMATIC: return "kinematic";
        case BodyType::DYNAMIC: return "dynamic";
        default: return "static";
    }
}

BodyType editor::Stream::stringToBodyType(const std::string& str) {
    if (str == "kinematic") return BodyType::KINEMATIC;
    if (str == "dynamic") return BodyType::DYNAMIC;
    return BodyType::STATIC;
}

std::string editor::Stream::rigidbodyMotionQualityToString(RigidbodyMotionQuality motionQuality) {
    switch (motionQuality) {
        case RigidbodyMotionQuality::LINEAR_CAST: return "linear_cast";
        case RigidbodyMotionQuality::DISCRETE:
        default: return "discrete";
    }
}

RigidbodyMotionQuality editor::Stream::stringToRigidbodyMotionQuality(const std::string& str) {
    if (str == "linear_cast") return RigidbodyMotionQuality::LINEAR_CAST;
    return RigidbodyMotionQuality::DISCRETE;
}

std::string editor::Stream::collider2DTypeToString(Collider2DType type) {
    switch (type) {
        case Collider2DType::POLYGON: return "polygon";
        case Collider2DType::CIRCLE: return "circle";
        case Collider2DType::CAPSULE: return "capsule";
        case Collider2DType::SEGMENT: return "segment";
        case Collider2DType::CHAIN: return "chain";
        default: return "polygon";
    }
}

Collider2DType editor::Stream::stringToCollider2DType(const std::string& str) {
    if (str == "circle") return Collider2DType::CIRCLE;
    if (str == "capsule") return Collider2DType::CAPSULE;
    if (str == "segment") return Collider2DType::SEGMENT;
    if (str == "chain") return Collider2DType::CHAIN;
    return Collider2DType::POLYGON;
}

std::string editor::Stream::collider3DTypeToString(Collider3DType type) {
    switch (type) {
        case Collider3DType::SPHERE: return "sphere";
        case Collider3DType::BOX: return "box";
        case Collider3DType::CAPSULE: return "capsule";
        case Collider3DType::TAPERED_CAPSULE: return "tapered_capsule";
        case Collider3DType::CYLINDER: return "cylinder";
        case Collider3DType::CONVEX_HULL: return "convex_hull";
        case Collider3DType::MESH: return "mesh";
        case Collider3DType::HEIGHTFIELD: return "heightfield";
        default: return "sphere";
    }
}

Collider3DType editor::Stream::stringToCollider3DType(const std::string& str) {
    if (str == "box") return Collider3DType::BOX;
    if (str == "capsule") return Collider3DType::CAPSULE;
    if (str == "tapered_capsule") return Collider3DType::TAPERED_CAPSULE;
    if (str == "cylinder") return Collider3DType::CYLINDER;
    if (str == "convex_hull") return Collider3DType::CONVEX_HULL;
    if (str == "mesh") return Collider3DType::MESH;
    if (str == "heightfield") return Collider3DType::HEIGHTFIELD;
    return Collider3DType::SPHERE;
}

std::string editor::Stream::joint2DTypeToString(Joint2DType type) {
    switch (type) {
        case Joint2DType::DISTANCE: return "distance";
        case Joint2DType::REVOLUTE: return "revolute";
        case Joint2DType::PRISMATIC: return "prismatic";
        case Joint2DType::MOUSE: return "mouse";
        case Joint2DType::WHEEL: return "wheel";
        case Joint2DType::WELD: return "weld";
        case Joint2DType::MOTOR: return "motor";
        default: return "distance";
    }
}

Joint2DType editor::Stream::stringToJoint2DType(const std::string& str) {
    if (str == "revolute") return Joint2DType::REVOLUTE;
    if (str == "prismatic") return Joint2DType::PRISMATIC;
    if (str == "mouse") return Joint2DType::MOUSE;
    if (str == "wheel") return Joint2DType::WHEEL;
    if (str == "weld") return Joint2DType::WELD;
    if (str == "motor") return Joint2DType::MOTOR;
    return Joint2DType::DISTANCE;
}

std::string editor::Stream::joint3DTypeToString(Joint3DType type) {
    switch (type) {
        case Joint3DType::FIXED: return "fixed";
        case Joint3DType::DISTANCE: return "distance";
        case Joint3DType::POINT: return "point";
        case Joint3DType::HINGE: return "hinge";
        case Joint3DType::CONE: return "cone";
        case Joint3DType::PRISMATIC: return "prismatic";
        case Joint3DType::SWINGTWIST: return "swingtwist";
        case Joint3DType::SIXDOF: return "sixdof";
        case Joint3DType::PATH: return "path";
        case Joint3DType::GEAR: return "gear";
        case Joint3DType::RACKANDPINON: return "rackandpinion";
        case Joint3DType::PULLEY: return "pulley";
        default: return "fixed";
    }
}

Joint3DType editor::Stream::stringToJoint3DType(const std::string& str) {
    if (str == "distance") return Joint3DType::DISTANCE;
    if (str == "point") return Joint3DType::POINT;
    if (str == "hinge") return Joint3DType::HINGE;
    if (str == "cone") return Joint3DType::CONE;
    if (str == "prismatic") return Joint3DType::PRISMATIC;
    if (str == "swingtwist") return Joint3DType::SWINGTWIST;
    if (str == "sixdof") return Joint3DType::SIXDOF;
    if (str == "path") return Joint3DType::PATH;
    if (str == "gear") return Joint3DType::GEAR;
    if (str == "rackandpinion") return Joint3DType::RACKANDPINON;
    if (str == "pulley") return Joint3DType::PULLEY;
    return Joint3DType::FIXED;
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

std::string editor::Stream::jointAnchorSpaceToString(JointAnchorSpace space) {
    return space == JointAnchorSpace::LOCAL ? "local" : "world";
}

JointAnchorSpace editor::Stream::stringToJointAnchorSpace(const std::string& str) {
    return str == "local" ? JointAnchorSpace::LOCAL : JointAnchorSpace::WORLD;
}

std::string editor::Stream::jointBreakActionToString(JointBreakAction action) {
    switch (action) {
        case JointBreakAction::IGNORE: return "ignore";
        case JointBreakAction::CALLBACK_ONLY: return "callback_only";
        case JointBreakAction::DISABLE: return "disable";
        default: return "destroy";
    }
}

JointBreakAction editor::Stream::stringToJointBreakAction(const std::string& str) {
    if (str == "ignore") return JointBreakAction::IGNORE;
    if (str == "callback_only") return JointBreakAction::CALLBACK_ONLY;
    if (str == "disable") return JointBreakAction::DISABLE;
    return JointBreakAction::DESTROY;
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

    if (node["type"]) body.type = stringToBodyType(node["type"].as<std::string>());

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
                collider.type = stringToCollider2DType(shapeNode["type"].as<std::string>());
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

    if (node["type"]) body.type = stringToBodyType(node["type"].as<std::string>());
    if (node["motionQuality"]) body.motionQuality = stringToRigidbodyMotionQuality(node["motionQuality"].as<std::string>());
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
        body.allowedDOFs = dofs == 0 ? RigidbodyAllowedDOF_All : dofs;
    }

    if (node["numColliders"]) body.numColliders = YamlNumbers::as<unsigned int>(node["numColliders"]);

    if (node["colliders"]) {
        size_t count = node["colliders"].size();
        for (size_t i = 0; i < count; i++) {
            if (node["colliders"][i]["type"]) {
                body.colliders[i].type = stringToCollider3DType(node["colliders"][i]["type"].as<std::string>());
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

YAML::Node editor::Stream::encodeJoint2DComponent(const Joint2DComponent& joint) {
    YAML::Node node;
    node["enabled"] = joint.enabled;

    node["type"] = joint2DTypeToString(joint.type);
    node["bodyA"] = joint.bodyA;
    node["bodyB"] = joint.bodyB;
    node["anchorSpace"] = jointAnchorSpaceToString(joint.anchorSpace);
    node["anchorA"] = encodeVector2(joint.anchorA);
    node["anchorB"] = encodeVector2(joint.anchorB);
    node["axis"] = encodeVector2(joint.axis);
    node["target"] = encodeVector2(joint.target);
    node["autoAnchors"] = joint.autoAnchors;
    node["rope"] = joint.rope;
    node["enableCollision"] = joint.enableCollision;
    // infinite (never breaks) is the missing key
    if (std::isfinite(joint.breakForce)) node["breakForce"] = joint.breakForce;
    if (std::isfinite(joint.breakTorque)) node["breakTorque"] = joint.breakTorque;
    node["breakAction"] = jointBreakActionToString(joint.breakAction);
    node["useLimits"] = joint.useLimits;
    node["lowerAngle"] = joint.lowerAngle;
    node["upperAngle"] = joint.upperAngle;
    node["lowerTranslation"] = joint.lowerTranslation;
    node["upperTranslation"] = joint.upperTranslation;
    node["useMotor"] = joint.useMotor;
    node["motorSpeed"] = joint.motorSpeed;
    node["maxMotorTorque"] = joint.maxMotorTorque;
    node["maxMotorForce"] = joint.maxMotorForce;
    node["enableSpring"] = joint.enableSpring;
    node["frequency"] = joint.frequency;
    node["dampingRatio"] = joint.dampingRatio;
    node["length"] = joint.length;
    node["autoLength"] = joint.autoLength;
    node["minLength"] = joint.minLength;
    node["maxLength"] = joint.maxLength;
    node["linearFrequency"] = joint.linearFrequency;
    node["angularFrequency"] = joint.angularFrequency;
    node["linearDampingRatio"] = joint.linearDampingRatio;
    node["angularDampingRatio"] = joint.angularDampingRatio;
    node["linearOffset"] = encodeVector2(joint.linearOffset);
    node["angularOffset"] = joint.angularOffset;
    node["autoOffset"] = joint.autoOffset;
    node["maxForce"] = joint.maxForce;
    node["maxTorque"] = joint.maxTorque;
    node["correctionFactor"] = joint.correctionFactor;

    return node;
}

Joint2DComponent editor::Stream::decodeJoint2DComponent(const YAML::Node& node, const Joint2DComponent* oldJoint) {
    Joint2DComponent joint;

    if (oldJoint) {
        joint = *oldJoint;
        joint.loadFailed = false;
        if (node["type"]) joint.type = stringToJoint2DType(node["type"].as<std::string>());
    } else {
        // a key missing from a scene saved before is what the joint had then (Box2D's defaults
        // and world anchors), so old scenes simulate as they did
        const Joint2DType type = node["type"] ? stringToJoint2DType(node["type"].as<std::string>()) : Joint2DType::DISTANCE;
        joint = makeLegacyJoint2D(type);
    }

    if (node["bodyA"]) joint.bodyA = YamlNumbers::as<Entity>(node["bodyA"]);
    if (node["bodyB"]) joint.bodyB = YamlNumbers::as<Entity>(node["bodyB"]);
    if (node["anchorSpace"]) joint.anchorSpace = stringToJointAnchorSpace(node["anchorSpace"].as<std::string>());
    if (node["anchorA"]) joint.anchorA = decodeVector2(node["anchorA"]);
    if (node["anchorB"]) joint.anchorB = decodeVector2(node["anchorB"]);
    if (node["axis"]) joint.axis = decodeVector2(node["axis"]);
    if (node["target"]) joint.target = decodeVector2(node["target"]);
    if (node["autoAnchors"]) joint.autoAnchors = YamlNumbers::as<bool>(node["autoAnchors"]);
    if (node["rope"]) joint.rope = YamlNumbers::as<bool>(node["rope"]);
    if (node["enableCollision"]) joint.enableCollision = YamlNumbers::as<bool>(node["enableCollision"]);
    // written only when finite: absent is "never", also over an existing component
    joint.breakForce = node["breakForce"] ? YamlNumbers::as<float>(node["breakForce"]) : std::numeric_limits<float>::infinity();
    joint.breakTorque = node["breakTorque"] ? YamlNumbers::as<float>(node["breakTorque"]) : std::numeric_limits<float>::infinity();
    if (node["breakAction"]) joint.breakAction = stringToJointBreakAction(node["breakAction"].as<std::string>());
    if (node["useLimits"]) joint.useLimits = YamlNumbers::as<bool>(node["useLimits"]);
    if (node["lowerAngle"]) joint.lowerAngle = YamlNumbers::as<float>(node["lowerAngle"]);
    if (node["upperAngle"]) joint.upperAngle = YamlNumbers::as<float>(node["upperAngle"]);
    if (node["lowerTranslation"]) joint.lowerTranslation = YamlNumbers::as<float>(node["lowerTranslation"]);
    if (node["upperTranslation"]) joint.upperTranslation = YamlNumbers::as<float>(node["upperTranslation"]);
    if (node["useMotor"]) joint.useMotor = YamlNumbers::as<bool>(node["useMotor"]);
    if (node["motorSpeed"]) joint.motorSpeed = YamlNumbers::as<float>(node["motorSpeed"]);
    if (node["maxMotorTorque"]) joint.maxMotorTorque = YamlNumbers::as<float>(node["maxMotorTorque"]);
    if (node["maxMotorForce"]) joint.maxMotorForce = YamlNumbers::as<float>(node["maxMotorForce"]);
    if (node["enableSpring"]) joint.enableSpring = YamlNumbers::as<bool>(node["enableSpring"]);
    if (node["frequency"]) joint.frequency = YamlNumbers::as<float>(node["frequency"]);
    if (node["dampingRatio"]) joint.dampingRatio = YamlNumbers::as<float>(node["dampingRatio"]);
    if (node["length"]) joint.length = YamlNumbers::as<float>(node["length"]);
    if (node["autoLength"]) joint.autoLength = YamlNumbers::as<bool>(node["autoLength"]);
    if (node["minLength"]) joint.minLength = YamlNumbers::as<float>(node["minLength"]);
    if (node["maxLength"]) joint.maxLength = YamlNumbers::as<float>(node["maxLength"]);
    if (node["linearFrequency"]) joint.linearFrequency = YamlNumbers::as<float>(node["linearFrequency"]);
    if (node["angularFrequency"]) joint.angularFrequency = YamlNumbers::as<float>(node["angularFrequency"]);
    if (node["linearDampingRatio"]) joint.linearDampingRatio = YamlNumbers::as<float>(node["linearDampingRatio"]);
    if (node["angularDampingRatio"]) joint.angularDampingRatio = YamlNumbers::as<float>(node["angularDampingRatio"]);
    if (node["linearOffset"]) joint.linearOffset = decodeVector2(node["linearOffset"]);
    if (node["angularOffset"]) joint.angularOffset = YamlNumbers::as<float>(node["angularOffset"]);
    if (node["autoOffset"]) joint.autoOffset = YamlNumbers::as<bool>(node["autoOffset"]);
    if (node["maxForce"]) joint.maxForce = YamlNumbers::as<float>(node["maxForce"]);
    if (node["maxTorque"]) joint.maxTorque = YamlNumbers::as<float>(node["maxTorque"]);
    if (node["correctionFactor"]) joint.correctionFactor = YamlNumbers::as<float>(node["correctionFactor"]);

    if (oldJoint && b2Joint_IsValid(oldJoint->joint)) {
        joint.needUpdateJoint = true;
    }
    joint.needApplyParameters = false;
    joint.broken = false;
    joint.breakArmed = true;

    if (node["enabled"]) joint.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return joint;
}

// ==============================
// Joint3DComponent encode/decode
// ==============================

YAML::Node editor::Stream::encodeJoint3DComponent(const Joint3DComponent& joint) {
    YAML::Node node;
    node["enabled"] = joint.enabled;

    node["type"] = joint3DTypeToString(joint.type);
    node["bodyA"] = joint.bodyA;
    node["bodyB"] = joint.bodyB;
    node["anchorA"] = encodeVector3(joint.anchorA);
    node["anchorB"] = encodeVector3(joint.anchorB);
    node["anchor"] = encodeVector3(joint.anchor);
    node["axis"] = encodeVector3(joint.axis);
    node["normal"] = encodeVector3(joint.normal);
    node["twistAxis"] = encodeVector3(joint.twistAxis);
    node["planeAxis"] = encodeVector3(joint.planeAxis);
    node["axisX"] = encodeVector3(joint.axisX);
    node["axisY"] = encodeVector3(joint.axisY);
    node["limitsMin"] = joint.limitsMin;
    node["limitsMax"] = joint.limitsMax;
    node["normalHalfConeAngle"] = joint.normalHalfConeAngle;
    node["planeHalfConeAngle"] = joint.planeHalfConeAngle;
    node["twistMinAngle"] = joint.twistMinAngle;
    node["twistMaxAngle"] = joint.twistMaxAngle;
    node["fixedPointA"] = encodeVector3(joint.fixedPointA);
    node["fixedPointB"] = encodeVector3(joint.fixedPointB);
    node["hingeA"] = joint.hingeA;
    node["hingeB"] = joint.hingeB;
    node["hinge"] = joint.hinge;
    node["slider"] = joint.slider;
    node["numTeethGearA"] = joint.numTeethGearA;
    node["numTeethGearB"] = joint.numTeethGearB;
    node["numTeethRack"] = joint.numTeethRack;
    node["numTeethGear"] = joint.numTeethGear;
    node["rackLength"] = joint.rackLength;
    YAML::Node pathPointsNode;
    for (const Vector3& point : joint.pathPoints){
        pathPointsNode.push_back(encodeVector3(point));
    }
    node["pathPoints"] = pathPointsNode;
    node["pathPosition"] = encodeVector3(joint.pathPosition);
    node["isLooping"] = joint.isLooping;
    node["autoAnchors"] = joint.autoAnchors;

    return node;
}

Joint3DComponent editor::Stream::decodeJoint3DComponent(const YAML::Node& node, const Joint3DComponent* oldJoint) {
    Joint3DComponent joint;

    if (oldJoint) {
        joint = *oldJoint;
    }

    if (node["type"]) joint.type = stringToJoint3DType(node["type"].as<std::string>());
    if (node["bodyA"]) joint.bodyA = YamlNumbers::as<Entity>(node["bodyA"]);
    if (node["bodyB"]) joint.bodyB = YamlNumbers::as<Entity>(node["bodyB"]);
    if (node["anchorA"]) joint.anchorA = decodeVector3(node["anchorA"]);
    if (node["anchorB"]) joint.anchorB = decodeVector3(node["anchorB"]);
    if (node["anchor"]) joint.anchor = decodeVector3(node["anchor"]);
    if (node["axis"]) joint.axis = decodeVector3(node["axis"]);
    if (node["normal"]) joint.normal = decodeVector3(node["normal"]);
    if (node["twistAxis"]) joint.twistAxis = decodeVector3(node["twistAxis"]);
    if (node["planeAxis"]) joint.planeAxis = decodeVector3(node["planeAxis"]);
    if (node["axisX"]) joint.axisX = decodeVector3(node["axisX"]);
    if (node["axisY"]) joint.axisY = decodeVector3(node["axisY"]);
    if (node["limitsMin"]) joint.limitsMin = YamlNumbers::as<float>(node["limitsMin"]);
    if (node["limitsMax"]) joint.limitsMax = YamlNumbers::as<float>(node["limitsMax"]);
    if (node["normalHalfConeAngle"]) joint.normalHalfConeAngle = YamlNumbers::as<float>(node["normalHalfConeAngle"]);
    if (node["planeHalfConeAngle"]) joint.planeHalfConeAngle = YamlNumbers::as<float>(node["planeHalfConeAngle"]);
    if (node["twistMinAngle"]) joint.twistMinAngle = YamlNumbers::as<float>(node["twistMinAngle"]);
    if (node["twistMaxAngle"]) joint.twistMaxAngle = YamlNumbers::as<float>(node["twistMaxAngle"]);
    if (node["fixedPointA"]) joint.fixedPointA = decodeVector3(node["fixedPointA"]);
    if (node["fixedPointB"]) joint.fixedPointB = decodeVector3(node["fixedPointB"]);
    if (node["hingeA"]) joint.hingeA = YamlNumbers::as<Entity>(node["hingeA"]);
    if (node["hingeB"]) joint.hingeB = YamlNumbers::as<Entity>(node["hingeB"]);
    if (node["hinge"]) joint.hinge = YamlNumbers::as<Entity>(node["hinge"]);
    if (node["slider"]) joint.slider = YamlNumbers::as<Entity>(node["slider"]);
    if (node["numTeethGearA"]) joint.numTeethGearA = YamlNumbers::as<int>(node["numTeethGearA"]);
    if (node["numTeethGearB"]) joint.numTeethGearB = YamlNumbers::as<int>(node["numTeethGearB"]);
    if (node["numTeethRack"]) joint.numTeethRack = YamlNumbers::as<int>(node["numTeethRack"]);
    if (node["numTeethGear"]) joint.numTeethGear = YamlNumbers::as<int>(node["numTeethGear"]);
    if (node["rackLength"]) joint.rackLength = YamlNumbers::as<int>(node["rackLength"]);
    if (node["pathPoints"]){
        joint.pathPoints.clear();
        for (const YAML::Node& pointNode : node["pathPoints"]){
            joint.pathPoints.push_back(decodeVector3(pointNode));
        }
    }
    if (node["pathPosition"]) joint.pathPosition = decodeVector3(node["pathPosition"]);
    if (node["isLooping"]) joint.isLooping = YamlNumbers::as<bool>(node["isLooping"]);
    if (node["autoAnchors"]) joint.autoAnchors = YamlNumbers::as<bool>(node["autoAnchors"]);

    if (oldJoint && oldJoint->joint) {
        joint.needUpdateJoint = true;
    }

    if (node["enabled"]) joint.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return joint;
}

// ── ActionComponent ──

std::string editor::Stream::actionStateToString(ActionState state) {
    switch (state) {
        case ActionState::Running: return "Running";
        case ActionState::Paused: return "Paused";
        case ActionState::Stopped: return "Stopped";
        default: return "Stopped";
    }
}

ActionState editor::Stream::stringToActionState(const std::string& str) {
    if (str == "Running") return ActionState::Running;
    if (str == "Paused") return ActionState::Paused;
    return ActionState::Stopped;
}

YAML::Node editor::Stream::encodeActionComponent(const ActionComponent& action) {
    YAML::Node node;

    node["state"] = actionStateToString(action.state);
    node["speed"] = action.speed;
    node["target"] = action.target;
    node["ownedTarget"] = action.ownedTarget;

    return node;
}

ActionComponent editor::Stream::decodeActionComponent(const YAML::Node& node, const ActionComponent* oldAction) {
    ActionComponent action;

    if (oldAction) {
        action = *oldAction;
    }

    action.timecount = 0;
    action.startTrigger = false;
    action.stopTrigger = false;
    action.pauseTrigger = false;

    if (node["state"]) action.state = stringToActionState(node["state"].as<std::string>());
    if (node["speed"]) action.speed = YamlNumbers::as<float>(node["speed"]);
    if (node["target"]) action.target = YamlNumbers::as<Entity>(node["target"]);
    if (node["ownedTarget"]) action.ownedTarget = YamlNumbers::as<bool>(node["ownedTarget"]);

    return action;
}

// ── TweenComponent ──

YAML::Node editor::Stream::encodeTweenComponent(const TweenComponent& timed) {
    YAML::Node node;

    node["duration"] = timed.duration;
    node["loop"] = timed.loop;
    node["pingPong"] = timed.pingPong;
    node["function"] = encodeEase(timed.function);

    return node;
}

TweenComponent editor::Stream::decodeTweenComponent(const YAML::Node& node, const TweenComponent* oldTimed) {
    TweenComponent timed;

    if (oldTimed) {
        timed = *oldTimed;
    }

    timed.time = 0;
    timed.value = 0;

    if (node["duration"]) timed.duration = YamlNumbers::as<float>(node["duration"]);
    if (node["loop"]) timed.loop = YamlNumbers::as<bool>(node["loop"]);
    if (node["pingPong"]) timed.pingPong = YamlNumbers::as<bool>(node["pingPong"]);
    if (node["function"]) timed.function = decodeEase(node["function"], oldTimed ? &oldTimed->function : nullptr);

    return timed;
}

// ── PositionTweenComponent ──

YAML::Node editor::Stream::encodePositionTweenComponent(const PositionTweenComponent& posAction) {
    YAML::Node node;

    node["endPosition"] = encodeVector3(posAction.endPosition);
    node["startPosition"] = encodeVector3(posAction.startPosition);

    return node;
}

PositionTweenComponent editor::Stream::decodePositionTweenComponent(const YAML::Node& node, const PositionTweenComponent* oldPosAction) {
    PositionTweenComponent posAction;

    if (oldPosAction) {
        posAction = *oldPosAction;
    }

    if (node["endPosition"]) posAction.endPosition = decodeVector3(node["endPosition"]);
    if (node["startPosition"]) posAction.startPosition = decodeVector3(node["startPosition"]);

    return posAction;
}

// ── RotationTweenComponent ──

YAML::Node editor::Stream::encodeRotationTweenComponent(const RotationTweenComponent& rotAction) {
    YAML::Node node;

    node["endRotation"] = encodeQuaternion(rotAction.endRotation);
    node["startRotation"] = encodeQuaternion(rotAction.startRotation);
    node["shortestPath"] = rotAction.shortestPath;
    node["spin"] = rotAction.spin;
    node["axis"] = encodeVector3(rotAction.axis);
    node["angle"] = rotAction.angle;

    return node;
}

RotationTweenComponent editor::Stream::decodeRotationTweenComponent(const YAML::Node& node, const RotationTweenComponent* oldRotAction) {
    RotationTweenComponent rotAction;

    if (oldRotAction) {
        rotAction = *oldRotAction;
    }

    if (node["endRotation"]) rotAction.endRotation = decodeQuaternion(node["endRotation"]);
    if (node["startRotation"]) rotAction.startRotation = decodeQuaternion(node["startRotation"]);
    if (node["shortestPath"]) rotAction.shortestPath = YamlNumbers::as<bool>(node["shortestPath"]);
    if (node["spin"]) rotAction.spin = YamlNumbers::as<bool>(node["spin"]);
    if (node["axis"]) rotAction.axis = decodeVector3(node["axis"]);
    if (node["angle"]) rotAction.angle = YamlNumbers::as<float>(node["angle"]);

    return rotAction;
}

// ── ScaleTweenComponent ──

YAML::Node editor::Stream::encodeScaleTweenComponent(const ScaleTweenComponent& scaleTween) {
    YAML::Node node;

    node["endScale"] = encodeVector3(scaleTween.endScale);
    node["startScale"] = encodeVector3(scaleTween.startScale);

    return node;
}

ScaleTweenComponent editor::Stream::decodeScaleTweenComponent(const YAML::Node& node, const ScaleTweenComponent* oldScaleTween) {
    ScaleTweenComponent scaleTween;

    if (oldScaleTween) {
        scaleTween = *oldScaleTween;
    }

    if (node["endScale"]) scaleTween.endScale = decodeVector3(node["endScale"]);
    if (node["startScale"]) scaleTween.startScale = decodeVector3(node["startScale"]);

    return scaleTween;
}

// ── ColorTweenComponent ──

YAML::Node editor::Stream::encodeColorTweenComponent(const ColorTweenComponent& colorTween) {
    YAML::Node node;

    node["endColor"] = encodeVector3(colorTween.endColor);
    node["startColor"] = encodeVector3(colorTween.startColor);
    node["useSRGB"] = colorTween.useSRGB;

    return node;
}

ColorTweenComponent editor::Stream::decodeColorTweenComponent(const YAML::Node& node, const ColorTweenComponent* oldColorTween) {
    ColorTweenComponent colorTween;

    if (oldColorTween) {
        colorTween = *oldColorTween;
    }

    if (node["endColor"]) colorTween.endColor = decodeVector3(node["endColor"]);
    if (node["startColor"]) colorTween.startColor = decodeVector3(node["startColor"]);
    if (node["useSRGB"]) colorTween.useSRGB = YamlNumbers::as<bool>(node["useSRGB"]);

    return colorTween;
}

// ── AlphaTweenComponent ──

YAML::Node editor::Stream::encodeAlphaTweenComponent(const AlphaTweenComponent& alphaTween) {
    YAML::Node node;

    node["endAlpha"] = alphaTween.endAlpha;
    node["startAlpha"] = alphaTween.startAlpha;

    return node;
}

AlphaTweenComponent editor::Stream::decodeAlphaTweenComponent(const YAML::Node& node, const AlphaTweenComponent* oldAlphaTween) {
    AlphaTweenComponent alphaTween;

    if (oldAlphaTween) {
        alphaTween = *oldAlphaTween;
    }

    if (node["endAlpha"]) alphaTween.endAlpha = YamlNumbers::as<float>(node["endAlpha"]);
    if (node["startAlpha"]) alphaTween.startAlpha = YamlNumbers::as<float>(node["startAlpha"]);

    return alphaTween;
}

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

// ── AnimationComponent ──

YAML::Node editor::Stream::encodeAnimationComponent(const AnimationComponent& animation) {
    YAML::Node node;

    node["loop"] = animation.loop;
    node["duration"] = animation.duration;
    node["ownedActions"] = animation.ownedActions;
    node["defaultFadeTime"] = animation.defaultFadeTime;

    YAML::Node actionsNode;
    for (const auto& frame : animation.actions) {
        YAML::Node frameNode;
        frameNode["track"] = frame.track;
        frameNode["startTime"] = frame.startTime;
        frameNode["duration"] = frame.duration;
        frameNode["action"] = frame.action;
        actionsNode.push_back(frameNode);
    }
    node["actions"] = actionsNode;

    return node;
}

AnimationComponent editor::Stream::decodeAnimationComponent(const YAML::Node& node, const AnimationComponent* oldAnimation) {
    AnimationComponent animation;

    if (oldAnimation) {
        animation = *oldAnimation;
    }

    if (node["loop"]) animation.loop = YamlNumbers::as<bool>(node["loop"]);
    if (node["duration"]) animation.duration = YamlNumbers::as<float>(node["duration"]);
    if (node["ownedActions"]) animation.ownedActions = YamlNumbers::as<bool>(node["ownedActions"]);
    if (node["defaultFadeTime"]) animation.defaultFadeTime = YamlNumbers::as<float>(node["defaultFadeTime"]);

    if (node["actions"]) {
        animation.actions.clear();
        for (const YAML::Node& frameNode : node["actions"]) {
            ActionFrame frame;
            if (frameNode["track"]) frame.track = YamlNumbers::as<uint32_t>(frameNode["track"]);
            if (frameNode["startTime"]) frame.startTime = YamlNumbers::as<float>(frameNode["startTime"]);
            if (frameNode["duration"]) frame.duration = YamlNumbers::as<float>(frameNode["duration"]);
            if (frameNode["action"]) frame.action = YamlNumbers::as<Entity>(frameNode["action"]);
            animation.actions.push_back(frame);
        }
    }

    return animation;
}

YAML::Node editor::Stream::encodeBoneComponent(const BoneComponent& bone) {
    YAML::Node node;

    node["model"] = static_cast<uint32_t>(bone.model);
    node["index"] = bone.index;
    node["bindPosition"] = encodeVector3(bone.bindPosition);
    node["bindRotation"] = encodeQuaternion(bone.bindRotation);
    node["bindScale"] = encodeVector3(bone.bindScale);
    node["offsetMatrix"] = encodeMatrix4(bone.offsetMatrix);

    return node;
}

BoneComponent editor::Stream::decodeBoneComponent(const YAML::Node& node, const BoneComponent* oldBone) {
    BoneComponent bone;

    if (oldBone) {
        bone = *oldBone;
    }

    if (node["model"]) bone.model = static_cast<Entity>(YamlNumbers::as<uint32_t>(node["model"]));
    if (node["index"]) bone.index = YamlNumbers::as<int>(node["index"]);
    if (node["bindPosition"]) bone.bindPosition = decodeVector3(node["bindPosition"]);
    if (node["bindRotation"]) bone.bindRotation = decodeQuaternion(node["bindRotation"]);
    if (node["bindScale"]) bone.bindScale = decodeVector3(node["bindScale"]);
    if (node["offsetMatrix"]) bone.offsetMatrix = decodeMatrix4(node["offsetMatrix"]);

    return bone;
}

YAML::Node editor::Stream::encodeKeyframeTracksComponent(const KeyframeTracksComponent& tracks) {
    YAML::Node node;

    node["index"] = tracks.index;
    node["interpolation"] = tracks.interpolation;
    node["loop"] = tracks.loop;
    node["relative"] = tracks.relative;

    YAML::Node timesNode;
    for (float t : tracks.times) {
        timesNode.push_back(t);
    }
    node["times"] = timesNode;

    // Imported linear tracks keep this list empty, while explicit entries from
    // the Properties editor must survive a save even when they are linear.
    if (!tracks.easings.empty()) {
        YAML::Node easingsNode;
        for (EaseType e : tracks.easings) {
            easingsNode.push_back(easeTypeToString(e));
        }
        node["easings"] = easingsNode;
    }

    return node;
}

KeyframeTracksComponent editor::Stream::decodeKeyframeTracksComponent(const YAML::Node& node, const KeyframeTracksComponent* oldTracks) {
    KeyframeTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    // a decoded track takes its base again when it starts
    tracks.hasBase = false;

    if (node["index"]) tracks.index = YamlNumbers::as<int>(node["index"]);
    if (node["interpolation"]) tracks.interpolation = YamlNumbers::as<float>(node["interpolation"]);
    if (node["loop"]) tracks.loop = YamlNumbers::as<bool>(node["loop"]);
    if (node["relative"]) tracks.relative = YamlNumbers::as<bool>(node["relative"]);

    if (node["times"]) {
        tracks.times.clear();
        for (const YAML::Node& t : node["times"]) {
            tracks.times.push_back(YamlNumbers::as<float>(t));
        }
    }

    tracks.easings.clear();
    if (node["easings"]) {
        for (const YAML::Node& e : node["easings"]) {
            tracks.easings.push_back(stringToEaseType(e.as<std::string>()));
        }
    }

    return tracks;
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

YAML::Node editor::Stream::encodeTranslateTracksComponent(const TranslateTracksComponent& tracks) {
    return encodeTrackLists(tracks, encodeVector3);
}

TranslateTracksComponent editor::Stream::decodeTranslateTracksComponent(const YAML::Node& node, const TranslateTracksComponent* oldTracks) {
    TranslateTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    decodeTrackLists(node, tracks, decodeVector3);

    return tracks;
}

YAML::Node editor::Stream::encodeRotateTracksComponent(const RotateTracksComponent& tracks) {
    return encodeTrackLists(tracks, encodeQuaternion);
}

RotateTracksComponent editor::Stream::decodeRotateTracksComponent(const YAML::Node& node, const RotateTracksComponent* oldTracks) {
    RotateTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    decodeTrackLists(node, tracks, decodeQuaternion);

    return tracks;
}

YAML::Node editor::Stream::encodeScaleTracksComponent(const ScaleTracksComponent& tracks) {
    return encodeTrackLists(tracks, encodeVector3);
}

ScaleTracksComponent editor::Stream::decodeScaleTracksComponent(const YAML::Node& node, const ScaleTracksComponent* oldTracks) {
    ScaleTracksComponent tracks;

    if (oldTracks) {
        tracks = *oldTracks;
    }

    decodeTrackLists(node, tracks, decodeVector3);

    return tracks;
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

// ==============================
// Lattice components encode/decode
// ==============================

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

    if (node["interpolation"]) lattice.interpolation = stringToLatticeInterpolation(node["interpolation"].as<std::string>());
    if (node["outside"]) lattice.outside = stringToLatticeOutside(node["outside"].as<std::string>());
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
    if (node["jiggleAnchor"]) lattice.jiggleAnchor = stringToLatticeJiggleAnchor(node["jiggleAnchor"].as<std::string>());
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
                if (bindingNode["stage"]) binding.stage = stringToLatticeStage(bindingNode["stage"].as<std::string>());
                if (bindingNode["submesh"]) binding.submesh = YamlNumbers::as<int>(bindingNode["submesh"]);
                if (bindingNode["mask"]) binding.mask = stringToLatticeMaskChannel(bindingNode["mask"].as<std::string>());
                modifier.bindings.push_back(binding);
            }
        }
        modifier.warnedOverflow = false;
    }
    if (node["includeChildren"]) modifier.includeChildren = YamlNumbers::as<bool>(node["includeChildren"]);
    if (node["subdivisions"]) modifier.subdivisions = std::clamp(YamlNumbers::as<int>(node["subdivisions"]), 1, 64);

    return modifier;
}

YAML::Node editor::Stream::encodeLatticeFollowerComponent(const LatticeFollowerComponent& follower) {
    YAML::Node node;
    node["enabled"] = YamlNumbers::text(follower.enabled);

    node["lattice"] = follower.lattice;
    node["rest"] = follower.rest;
    node["applyPosition"] = YamlNumbers::text(follower.applyPosition);
    node["applyRotation"] = YamlNumbers::text(follower.applyRotation);
    node["applyScale"] = YamlNumbers::text(follower.applyScale);
    encodeFinite(node, "weight", follower.weight);

    return node;
}

LatticeFollowerComponent editor::Stream::decodeLatticeFollowerComponent(const YAML::Node& node, const LatticeFollowerComponent* oldFollower) {
    LatticeFollowerComponent follower;

    if (oldFollower) {
        follower = *oldFollower;
    }

    if (node["enabled"]) follower.enabled = YamlNumbers::as<bool>(node["enabled"]);
    if (node["lattice"]) follower.lattice = YamlNumbers::as<Entity>(node["lattice"]);
    if (node["rest"]) follower.rest = YamlNumbers::as<Entity>(node["rest"]);
    if (node["applyPosition"]) follower.applyPosition = YamlNumbers::as<bool>(node["applyPosition"]);
    if (node["applyRotation"]) follower.applyRotation = YamlNumbers::as<bool>(node["applyRotation"]);
    if (node["applyScale"]) follower.applyScale = YamlNumbers::as<bool>(node["applyScale"]);
    if (node["weight"]) follower.weight = decodeFinite(node["weight"], follower.weight);

    return follower;
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

YAML::Node editor::Stream::encodeParticleSystemComponent(const ParticleSystemComponent& particles) {
    YAML::Node node;
    node["enabled"] = particles.enabled;

    node["maxParticles"] = particles.maxParticles;
    node["emitter"] = particles.emitter;
    node["loop"] = particles.loop;
    node["localSpace"] = particles.localSpace;
    node["rate"] = particles.rate;
    node["maxPerUpdate"] = particles.maxPerUpdate;

    YAML::Node bursts;
    for (const auto& b : particles.bursts) {
        YAML::Node bn;
        bn["time"] = b.time;
        bn["minCount"] = b.minCount;
        bn["maxCount"] = b.maxCount;
        bursts.push_back(bn);
    }
    node["bursts"] = bursts;

    YAML::Node lifeInit;
    lifeInit["minLife"] = particles.lifeInitializer.minLife;
    lifeInit["maxLife"] = particles.lifeInitializer.maxLife;
    node["lifeInitializer"] = lifeInit;

    YAML::Node posInit;
    posInit["shape"] = particleEmitterShapeToString(particles.positionInitializer.shape);
    posInit["minPosition"] = encodeVector3(particles.positionInitializer.minPosition);
    posInit["maxPosition"] = encodeVector3(particles.positionInitializer.maxPosition);
    posInit["radius"] = particles.positionInitializer.radius;
    posInit["innerRadius"] = particles.positionInitializer.innerRadius;
    posInit["coneAngle"] = particles.positionInitializer.coneAngle;
    posInit["coneHeight"] = particles.positionInitializer.coneHeight;
    node["positionInitializer"] = posInit;

    YAML::Node posMod;
    posMod["fromTime"] = particles.positionModifier.fromTime;
    posMod["toTime"] = particles.positionModifier.toTime;
    posMod["fromPosition"] = encodeVector3(particles.positionModifier.fromPosition);
    posMod["toPosition"] = encodeVector3(particles.positionModifier.toPosition);
    posMod["function"] = encodeEase(particles.positionModifier.function);
    node["positionModifier"] = posMod;

    YAML::Node velInit;
    velInit["minVelocity"] = encodeVector3(particles.velocityInitializer.minVelocity);
    velInit["maxVelocity"] = encodeVector3(particles.velocityInitializer.maxVelocity);
    node["velocityInitializer"] = velInit;

    YAML::Node velMod;
    velMod["fromTime"] = particles.velocityModifier.fromTime;
    velMod["toTime"] = particles.velocityModifier.toTime;
    velMod["fromVelocity"] = encodeVector3(particles.velocityModifier.fromVelocity);
    velMod["toVelocity"] = encodeVector3(particles.velocityModifier.toVelocity);
    velMod["function"] = encodeEase(particles.velocityModifier.function);
    node["velocityModifier"] = velMod;

    YAML::Node accelInit;
    accelInit["minAcceleration"] = encodeVector3(particles.accelerationInitializer.minAcceleration);
    accelInit["maxAcceleration"] = encodeVector3(particles.accelerationInitializer.maxAcceleration);
    node["accelerationInitializer"] = accelInit;

    YAML::Node accelMod;
    accelMod["fromTime"] = particles.accelerationModifier.fromTime;
    accelMod["toTime"] = particles.accelerationModifier.toTime;
    accelMod["fromAcceleration"] = encodeVector3(particles.accelerationModifier.fromAcceleration);
    accelMod["toAcceleration"] = encodeVector3(particles.accelerationModifier.toAcceleration);
    accelMod["function"] = encodeEase(particles.accelerationModifier.function);
    node["accelerationModifier"] = accelMod;

    YAML::Node colorInit;
    colorInit["minColor"] = encodeVector3(particles.colorInitializer.minColor);
    colorInit["maxColor"] = encodeVector3(particles.colorInitializer.maxColor);
    colorInit["useSRGB"] = particles.colorInitializer.useSRGB;
    node["colorInitializer"] = colorInit;

    YAML::Node colorMod;
    colorMod["fromTime"] = particles.colorModifier.fromTime;
    colorMod["toTime"] = particles.colorModifier.toTime;
    colorMod["fromColor"] = encodeVector3(particles.colorModifier.fromColor);
    colorMod["toColor"] = encodeVector3(particles.colorModifier.toColor);
    colorMod["function"] = encodeEase(particles.colorModifier.function);
    colorMod["useSRGB"] = particles.colorModifier.useSRGB;
    node["colorModifier"] = colorMod;

    YAML::Node colorGrad;
    colorGrad["useSRGB"] = particles.colorGradient.useSRGB;
    YAML::Node colorGradStops;
    for (const auto& s : particles.colorGradient.stops) {
        YAML::Node sn;
        sn["time"] = s.time;
        sn["color"] = encodeVector3(s.color);
        colorGradStops.push_back(sn);
    }
    colorGrad["stops"] = colorGradStops;
    node["colorGradient"] = colorGrad;

    YAML::Node alphaInit;
    alphaInit["minAlpha"] = particles.alphaInitializer.minAlpha;
    alphaInit["maxAlpha"] = particles.alphaInitializer.maxAlpha;
    node["alphaInitializer"] = alphaInit;

    YAML::Node alphaMod;
    alphaMod["fromTime"] = particles.alphaModifier.fromTime;
    alphaMod["toTime"] = particles.alphaModifier.toTime;
    alphaMod["fromAlpha"] = particles.alphaModifier.fromAlpha;
    alphaMod["toAlpha"] = particles.alphaModifier.toAlpha;
    alphaMod["function"] = encodeEase(particles.alphaModifier.function);
    node["alphaModifier"] = alphaMod;

    YAML::Node sizeInit;
    sizeInit["minSize"] = particles.sizeInitializer.minSize;
    sizeInit["maxSize"] = particles.sizeInitializer.maxSize;
    node["sizeInitializer"] = sizeInit;

    YAML::Node sizeMod;
    sizeMod["fromTime"] = particles.sizeModifier.fromTime;
    sizeMod["toTime"] = particles.sizeModifier.toTime;
    sizeMod["fromSize"] = particles.sizeModifier.fromSize;
    sizeMod["toSize"] = particles.sizeModifier.toSize;
    sizeMod["function"] = encodeEase(particles.sizeModifier.function);
    node["sizeModifier"] = sizeMod;

    YAML::Node spriteInit;
    YAML::Node spriteInitFrames;
    for (int f : particles.spriteInitializer.frames) spriteInitFrames.push_back(f);
    spriteInit["frames"] = spriteInitFrames;
    node["spriteInitializer"] = spriteInit;

    YAML::Node spriteMod;
    spriteMod["fromTime"] = particles.spriteModifier.fromTime;
    spriteMod["toTime"] = particles.spriteModifier.toTime;
    spriteMod["function"] = encodeEase(particles.spriteModifier.function);
    YAML::Node spriteModFrames;
    for (int f : particles.spriteModifier.frames) spriteModFrames.push_back(f);
    spriteMod["frames"] = spriteModFrames;
    node["spriteModifier"] = spriteMod;

    YAML::Node rotInit;
    rotInit["minRotation"] = encodeQuaternion(particles.rotationInitializer.minRotation);
    rotInit["maxRotation"] = encodeQuaternion(particles.rotationInitializer.maxRotation);
    rotInit["shortestPath"] = particles.rotationInitializer.shortestPath;
    node["rotationInitializer"] = rotInit;

    YAML::Node rotMod;
    rotMod["fromTime"] = particles.rotationModifier.fromTime;
    rotMod["toTime"] = particles.rotationModifier.toTime;
    rotMod["fromRotation"] = encodeQuaternion(particles.rotationModifier.fromRotation);
    rotMod["toRotation"] = encodeQuaternion(particles.rotationModifier.toRotation);
    rotMod["function"] = encodeEase(particles.rotationModifier.function);
    rotMod["shortestPath"] = particles.rotationModifier.shortestPath;
    node["rotationModifier"] = rotMod;

    YAML::Node scaleInit;
    scaleInit["minScale"] = encodeVector3(particles.scaleInitializer.minScale);
    scaleInit["maxScale"] = encodeVector3(particles.scaleInitializer.maxScale);
    scaleInit["linearSort"] = particles.scaleInitializer.linearSort;
    node["scaleInitializer"] = scaleInit;

    YAML::Node scaleMod;
    scaleMod["fromTime"] = particles.scaleModifier.fromTime;
    scaleMod["toTime"] = particles.scaleModifier.toTime;
    scaleMod["fromScale"] = encodeVector3(particles.scaleModifier.fromScale);
    scaleMod["toScale"] = encodeVector3(particles.scaleModifier.toScale);
    scaleMod["function"] = encodeEase(particles.scaleModifier.function);
    node["scaleModifier"] = scaleMod;

    return node;
}

ParticleSystemComponent editor::Stream::decodeParticleSystemComponent(const YAML::Node& node, const ParticleSystemComponent* oldParticles) {
    ParticleSystemComponent particles;
    if (oldParticles) { particles = *oldParticles; }

    if (node["maxParticles"]) particles.maxParticles = YamlNumbers::as<unsigned int>(node["maxParticles"]);
    if (node["emitter"]) particles.emitter = YamlNumbers::as<bool>(node["emitter"]);
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
        if (n["shape"]) particles.positionInitializer.shape = stringToParticleEmitterShape(n["shape"].as<std::string>());
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
        if (n["function"]) particles.positionModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->positionModifier.function : nullptr);
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
        if (n["function"]) particles.velocityModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->velocityModifier.function : nullptr);
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
        if (n["function"]) particles.accelerationModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->accelerationModifier.function : nullptr);
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
        if (n["function"]) particles.colorModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->colorModifier.function : nullptr);
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
        if (n["function"]) particles.alphaModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->alphaModifier.function : nullptr);
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
        if (n["function"]) particles.sizeModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->sizeModifier.function : nullptr);
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
        if (n["function"]) particles.spriteModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->spriteModifier.function : nullptr);
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
        if (n["function"]) particles.rotationModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->rotationModifier.function : nullptr);
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
        if (n["function"]) particles.scaleModifier.function = decodeEase(n["function"], oldParticles ? &oldParticles->scaleModifier.function : nullptr);
    }

    // Reset runtime fields
    particles.particles.clear();
    particles.newParticlesCount = 0;
    particles.lastUsedParticle = 0;

    if (node["enabled"]) particles.enabled = YamlNumbers::as<bool>(node["enabled"]);

    return particles;
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
        node["customShader"] = points.customShader;
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

    if (node["customShader"]) points.customShader = node["customShader"].as<std::string>();
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

std::string editor::Stream::lineRendererModeToString(LineRendererMode mode) {
    return mode == LineRendererMode::Polyline ? "polyline" : "segments";
}

LineRendererMode editor::Stream::stringToLineRendererMode(const std::string& str) {
    return (str == "polyline" || str == "Polyline") ? LineRendererMode::Polyline : LineRendererMode::Segments;
}

std::string editor::Stream::lineWidthUnitToString(LineWidthUnit unit) {
    return unit == LineWidthUnit::Pixels ? "pixels" : "world";
}

LineWidthUnit editor::Stream::stringToLineWidthUnit(const std::string& str) {
    return (str == "pixels" || str == "Pixels") ? LineWidthUnit::Pixels : LineWidthUnit::World;
}

std::string editor::Stream::lineAlignmentToString(LineAlignment alignment) {
    return alignment == LineAlignment::TransformZ ? "transform_z" : "view";
}

LineAlignment editor::Stream::stringToLineAlignment(const std::string& str) {
    return (str == "transform_z" || str == "TransformZ" || str == "transformZ") ? LineAlignment::TransformZ : LineAlignment::View;
}

std::string editor::Stream::lineTextureModeToString(LineTextureMode mode) {
    switch (mode) {
        case LineTextureMode::Stretch: return "stretch";
        case LineTextureMode::Tile: return "tile";
        case LineTextureMode::DistributePerSegment: return "distribute_per_segment";
        case LineTextureMode::RepeatPerSegment: return "repeat_per_segment";
    }
    return "stretch";
}

LineTextureMode editor::Stream::stringToLineTextureMode(const std::string& str) {
    if (str == "tile" || str == "Tile") return LineTextureMode::Tile;
    if (str == "distribute_per_segment" || str == "DistributePerSegment") return LineTextureMode::DistributePerSegment;
    if (str == "repeat_per_segment" || str == "RepeatPerSegment") return LineTextureMode::RepeatPerSegment;
    return LineTextureMode::Stretch;
}

// The settings LineRendererComponent and TrailRendererComponent share (width, colour,
// joints, caps, texture layout, sort order), each written away from its default only
template<typename T>
void editor::Stream::encodeRibbonFields(YAML::Node& node, const T& component, const T& defaults) {
    if (component.widthMultiplier != defaults.widthMultiplier) node["widthMultiplier"] = YamlNumbers::text(component.widthMultiplier);
    if (component.widthCurve != defaults.widthCurve) node["widthCurve"] = editor::Stream::encodeCurve(component.widthCurve);
    if (component.widthUnit != defaults.widthUnit) node["widthUnit"] = editor::Stream::lineWidthUnitToString(component.widthUnit);
    if (component.colorGradient != defaults.colorGradient) node["colorGradient"] = editor::Stream::encodeGradient(component.colorGradient);
    if (component.numCornerVertices != defaults.numCornerVertices) node["numCornerVertices"] = component.numCornerVertices;
    if (component.numCapVertices != defaults.numCapVertices) node["numCapVertices"] = component.numCapVertices;
    if (component.alignment != defaults.alignment) node["alignment"] = editor::Stream::lineAlignmentToString(component.alignment);
    if (component.textureMode != defaults.textureMode) node["textureMode"] = editor::Stream::lineTextureModeToString(component.textureMode);
    if (component.textureScale != defaults.textureScale) node["textureScale"] = editor::Stream::encodeVector2(component.textureScale);
    if (component.sortPriority != defaults.sortPriority) node["sortPriority"] = component.sortPriority;
}

template<typename T>
void editor::Stream::decodeRibbonFields(const YAML::Node& node, T& component, const T& defaults) {
    component.widthMultiplier = node["widthMultiplier"] ? std::max(0.0f, editor::Stream::decodeFinite(node["widthMultiplier"], defaults.widthMultiplier)) : defaults.widthMultiplier;
    component.widthCurve = node["widthCurve"] ? editor::Stream::decodeCurve(node["widthCurve"], defaults.widthCurve) : defaults.widthCurve;
    component.widthUnit = node["widthUnit"] ? editor::Stream::stringToLineWidthUnit(node["widthUnit"].as<std::string>()) : defaults.widthUnit;
    component.colorGradient = node["colorGradient"] ? editor::Stream::decodeGradient(node["colorGradient"], defaults.colorGradient) : defaults.colorGradient;
    component.numCornerVertices = node["numCornerVertices"]
        ? std::min(YamlNumbers::as<unsigned int>(node["numCornerVertices"]), LINE_RIBBON_MAX_FAN_VERTICES) : defaults.numCornerVertices;
    component.numCapVertices = node["numCapVertices"]
        ? std::min(YamlNumbers::as<unsigned int>(node["numCapVertices"]), LINE_RIBBON_MAX_FAN_VERTICES) : defaults.numCapVertices;
    component.alignment = node["alignment"] ? editor::Stream::stringToLineAlignment(node["alignment"].as<std::string>()) : defaults.alignment;
    component.textureMode = node["textureMode"] ? editor::Stream::stringToLineTextureMode(node["textureMode"].as<std::string>()) : defaults.textureMode;
    component.textureScale = node["textureScale"] ? editor::Stream::decodeVector2(node["textureScale"]) : defaults.textureScale;
    component.sortPriority = node["sortPriority"] ? YamlNumbers::as<int>(node["sortPriority"]) : defaults.sortPriority;
}

void editor::Stream::encodeLineRibbonSettings(YAML::Node& node, const LineRendererComponent& lines) {
    static const LineRendererComponent defaults;
    if (lines.mode != defaults.mode) node["mode"] = lineRendererModeToString(lines.mode);
    if (!lines.positions.empty()) {
        YAML::Node positions(YAML::NodeType::Sequence);
        for (const Vector3& position : lines.positions) positions.push_back(encodeVector3(position));
        node["positions"] = positions;
    }
    if (lines.loop != defaults.loop) node["loop"] = lines.loop;
    if (lines.useWorldSpace != defaults.useWorldSpace) node["useWorldSpace"] = lines.useWorldSpace;
    encodeRibbonFields(node, lines, defaults);
}

void editor::Stream::decodeLineRibbonSettings(const YAML::Node& node, LineRendererComponent& lines) {
    static const LineRendererComponent defaults;
    lines.mode = node["mode"] ? stringToLineRendererMode(node["mode"].as<std::string>()) : defaults.mode;
    lines.positions.clear();
    if (node["positions"] && node["positions"].IsSequence()) {
        for (const YAML::Node& position : node["positions"]) lines.positions.push_back(decodeVector3(position));
    }
    lines.loop = node["loop"] ? YamlNumbers::as<bool>(node["loop"]) : defaults.loop;
    lines.useWorldSpace = node["useWorldSpace"] ? YamlNumbers::as<bool>(node["useWorldSpace"]) : defaults.useWorldSpace;
    decodeRibbonFields(node, lines, defaults);
}

YAML::Node editor::Stream::encodeTrailRendererComponent(const TrailRendererComponent& trail) {
    static const TrailRendererComponent defaults;
    YAML::Node node;
    node["enabled"] = trail.enabled;
    node["time"] = YamlNumbers::text(trail.time);
    node["minVertexDistance"] = YamlNumbers::text(trail.minVertexDistance);
    // the Trail Renderer's own default width is 1 (Unity's): always written
    node["widthMultiplier"] = YamlNumbers::text(trail.widthMultiplier);
    if (trail.emitting != defaults.emitting) node["emitting"] = trail.emitting;
    if (trail.autodestruct != defaults.autodestruct) node["autodestruct"] = trail.autodestruct;
    encodeRibbonFields(node, trail, defaults);

    if (!trail.customShader.empty())
        node["customShader"] = trail.customShader;
    if (!trail.shaderUniforms.empty())
        node["shaderUniforms"] = encodeShaderUniforms(trail.shaderUniforms);
    encodeComponentShaderLibrary(node, trail.shaderTextures, trail.shaderFeatures, &trail.blendMode, &trail.depthWrite, true);
    encodeStencilAndColorWrite(node, trail.stencil, trail.colorWrite);
    if (!trail.shaderGlobalBindings.empty()) {
        YAML::Node bindings(YAML::NodeType::Map);
        for (const auto& binding : trail.shaderGlobalBindings) bindings[binding.first] = binding.second;
        node["shaderGlobalBindings"] = bindings;
    }
    return node;
}

TrailRendererComponent editor::Stream::decodeTrailRendererComponent(const YAML::Node& node, const TrailRendererComponent* oldTrail) {
    static const TrailRendererComponent defaults;
    TrailRendererComponent trail;
    if (oldTrail) { trail = *oldTrail; }

    trail.enabled = node["enabled"] ? YamlNumbers::as<bool>(node["enabled"]) : defaults.enabled;
    trail.time = node["time"] ? std::max(0.0f, decodeFinite(node["time"], defaults.time)) : defaults.time;
    trail.minVertexDistance = node["minVertexDistance"] ? std::max(0.0f, decodeFinite(node["minVertexDistance"], defaults.minVertexDistance)) : defaults.minVertexDistance;
    trail.emitting = node["emitting"] ? YamlNumbers::as<bool>(node["emitting"]) : defaults.emitting;
    trail.autodestruct = node["autodestruct"] ? YamlNumbers::as<bool>(node["autodestruct"]) : defaults.autodestruct;
    decodeRibbonFields(node, trail, defaults);

    trail.customShader = node["customShader"] ? node["customShader"].as<std::string>() : std::string();
    trail.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
    trail.needUpdateShaderUniforms = true;
    decodeComponentShaderLibrary(node, trail.shaderTextures, trail.shaderFeatures, &trail.blendMode, &trail.depthWrite);
    decodeStencilAndColorWrite(node, trail.stencil, trail.colorWrite);
    trail.shaderGlobalBindings.clear();
    if (node["shaderGlobalBindings"] && node["shaderGlobalBindings"].IsMap()) {
        for (const auto& binding : node["shaderGlobalBindings"]) {
            trail.shaderGlobalBindings.push_back({binding.first.as<std::string>(), binding.second.as<std::string>()});
        }
    }

    // the runtime starts again: no points, nothing loaded
    trail.points.reset();
    trail.hasLastHeadPosition = false;
    trail.idleTime = 0.0f;
    trail.warnedCapacity = false;
    trail.destroyRequested = false;
    trail.needUpdateRibbon = true;
    trail.needUpdateBuffer = false;
    trail.worldAABB.setNull();
    if (oldTrail && oldTrail->loaded) {
        // decoded over a loaded trail: reloaded by the render system (its GPU objects freed)
        trail.needReload = true;
    } else {
        trail.loaded = false;
        trail.loadCalled = false;
        trail.needReload = false;
        trail.loadedRibbonKey = 0;
        trail.ribbon = LineRibbonData();
    }
    return trail;
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
        node["customShader"] = lines.customShader;
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

    // not merged: an absent key is the default (encode leaves defaults out)
    decodeLineRibbonSettings(node, lines);

    if (node["customShader"]) lines.customShader = node["customShader"].as<std::string>();
    lines.shaderUniforms = decodeShaderUniforms(node["shaderUniforms"]);
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
