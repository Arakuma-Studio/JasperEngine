// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Stream.h"
#include "log/Logger.h"
#include "StreamSchema.h"
#include "StreamPreserved.h"
#include "StreamMigrations.h"
#include "AppSettings.h"
#include "EditorHost.h"
#include "Out.h"
#include "Workspace.h"

#include "Base64.h"
#include "Catalog.h"
#include "util/YamlNumbers.h"
#include "Factory.h"
#include "pool/SoundPool.h"
#include "pool/ShaderPool.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/StringUtils.h"
#include "ecs/EntityManager.h"
#include "render/SceneRender2D.h"
#include "render/SceneRender3D.h"
#include "window/TerrainEditWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>

using namespace jasperengine;

namespace {
    // The customShaderId in a key is a session-local registry index, so a fork is
    // named by its project-relative path instead, the way scenes name theirs.
    // Both directions normalize, keeping a since-retired property bit out of the file.
    YAML::Node encodeShaderKeys(const std::set<ShaderKey>& shaderKeys) {
        YAML::Node node(YAML::NodeType::Sequence);
        for (ShaderKey rawKey : shaderKeys) {
            const ShaderKey key = ShaderPool::normalizeKey(rawKey);
            const uint16_t customId = ShaderPool::getCustomIdFromKey(key);
            std::string customShader;
            if (customId != 0) {
                // Without a name there is nothing stable to write, and dropping only
                // the id would turn the override into one for the built-in shader
                customShader = ShaderPool::getCustomShaderName(customId);
                if (customShader.empty()) continue;
            }

            const uint32_t properties = ShaderPool::getPropertiesFromKey(key);
            YAML::Node entry(YAML::NodeType::Map);
            entry["type"] = ShaderPool::getShaderTypeName(ShaderPool::getShaderTypeFromKey(key), true);
            if (properties) entry["properties"] = properties;
            if (!customShader.empty()) entry["customShader"] = customShader;
            node.push_back(entry);
        }
        return node;
    }

    std::vector<ShaderKey> decodeShaderKeys(const YAML::Node& node) {
        std::vector<ShaderKey> shaderKeys;
        if (!node || !node.IsSequence()) {
            return shaderKeys;
        }

        for (const auto& keyNode : node) {
            if (!keyNode.IsMap()) {
                // Backward compatibility: scenes saved before forks were named by path
                // stored the packed key, so only its stable half survives and the fork
                // is re-paired on the next save. Remove this branch with support for them.
                shaderKeys.push_back(ShaderPool::normalizeKey(ShaderPool::getStorageKey(editor::YamlNumbers::as<uint64_t>(keyNode))));
                continue;
            }
            ShaderType shaderType;
            if (!keyNode["type"] || !ShaderPool::parseShaderTypeToken(keyNode["type"].as<std::string>(), shaderType)) {
                continue;
            }

            // Keyed by name, so this returns the id the scenes resolve for it too
            uint16_t customId = 0;
            if (keyNode["customShader"]) {
                customId = ShaderPool::registerCustomShader(keyNode["customShader"].as<std::string>());
            }
            const uint32_t properties = keyNode["properties"] ? editor::YamlNumbers::as<uint32_t>(keyNode["properties"]) : 0;
            shaderKeys.push_back(ShaderPool::normalizeKey(ShaderPool::getShaderKey(shaderType, properties, customId)));
        }
        return shaderKeys;
    }

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

    std::string androidOrientationToString(editor::AndroidOrientation orientation) {
        return editor::StreamSchema::androidOrientation().toString(orientation);
    }

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
    StreamLoad::checkNestedKeys(node, NestedKind::MaterialPass);
    // not merged: encodeMaterialPass omits the defaults
    MaterialPass pass;
    if (!node || !node.IsMap()) return pass;
    pass.shader = node["shader"] ? node["shader"].as<std::string>() : std::string();
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
        if (texture.empty() && !CameraTextureLink::isCameraTexture(texture)) continue;
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
    StreamLoad::checkNestedKeys(node, NestedKind::Texture);
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
    if (node["showGrid2D"])           ds.showGrid2D           = YamlNumbers::as<bool>(node["showGrid2D"]);
    if (node["gridSpacing2D"])        ds.gridSpacing2D        = decodePositiveFinite(node["gridSpacing2D"], ds.gridSpacing2D);
    if (node["gridSpacing3D"])        ds.gridSpacing3D        = decodePositiveFinite(node["gridSpacing3D"], ds.gridSpacing3D);
    if (node["snapToGrid"])           ds.snapToGrid           = YamlNumbers::as<bool>(node["snapToGrid"]);
    if (node["snapTile"])             ds.snapTile             = YamlNumbers::as<bool>(node["snapTile"]);
    if (node["snapRotation"])         ds.snapRotation         = YamlNumbers::as<bool>(node["snapRotation"]);
    if (node["rotationSnapDegrees"])  ds.rotationSnapDegrees  = decodePositiveFinite(node["rotationSnapDegrees"], ds.rotationSnapDegrees);
}

YAML::Node editor::Stream::encodeProject(Project* project) {
    YAML::Node root;

    root[StreamMigrations::versionKey(DocumentKind::Project)] = StreamMigrations::currentVersion(DocumentKind::Project);
    root["name"] = project->getName();

    root["canvasWidth"] = project->getCanvasWidth();
    root["canvasHeight"] = project->getCanvasHeight();

    root["scalingMode"] = scalingModeToString(project->getScalingMode());
    root["textureStrategy"] = textureStrategyToString(project->getTextureStrategy());
    root["vsync"] = project->isVSyncEnabled();
    if (!(project->getPlayerLogging() == PlayerLoggingSettings())) {
        const PlayerLoggingSettings& logging = project->getPlayerLogging();
        YAML::Node loggingNode;
        loggingNode["level"] = logging.level;
        loggingNode["fileLog"] = logging.fileLog;
        loggingNode["backtrace"] = logging.backtrace;
        root["playerLogging"] = loggingNode;
    }
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

    {
        YAML::Node exportNode;

        const ShaderOverrides& shaders = project->getShaderOverrides();
        if (!shaders.additions.empty()) exportNode["shaderAdditions"] = encodeShaderKeys(shaders.additions);
        if (!shaders.exclusions.empty()) exportNode["shaderExclusions"] = encodeShaderKeys(shaders.exclusions);

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
        if (android.orientation != defaults.orientation) androidNode["orientation"] = androidOrientationToString(android.orientation);

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
        if (android.keepScreenOn != defaults.keepScreenOn) androidNode["keepScreenOn"] = android.keepScreenOn;
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

    if (node["textureStrategy"]) {
        project->setTextureStrategy(decodeEnum(node, "textureStrategy", StreamSchema::textureStrategy()));
    }

    if (node["vsync"].IsDefined()) {
        project->setVSyncEnabled(YamlNumbers::as<bool>(node["vsync"]));
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

    if (node["htmlUiMsaaSamples"].IsDefined()) {
        project->setHtmlUiMsaaSamples(YamlNumbers::as<int>(node["htmlUiMsaaSamples"]));
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

    if (node["export"] && node["export"].IsMap()) {
        const YAML::Node& exportNode = node["export"];

        ShaderOverrides& shaders = project->getShaderOverrides();
        for (ShaderKey key : decodeShaderKeys(exportNode["shaderAdditions"])) shaders.additions.insert(key);
        for (ShaderKey key : decodeShaderKeys(exportNode["shaderExclusions"])) {
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
        if (androidNode["orientation"]) android.orientation = decodeEnum(androidNode, "orientation", StreamSchema::androidOrientation());

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
        if (androidNode["keepScreenOn"].IsDefined()) android.keepScreenOn = YamlNumbers::as<bool>(androidNode["keepScreenOn"]);
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
    root[StreamMigrations::versionKey(DocumentKind::Scene)] = StreamMigrations::currentVersion(DocumentKind::Scene);
    root["id"] = sceneProject->id;
    root["name"] = sceneProject->name;
    YAML::Node sceneNode = encodeScene(sceneProject->scene);
    StreamSave::applyBlock(sceneProject->scene, kSceneSettingsBlock, sceneNode);
    root["scene"] = sceneNode;
    root["sceneType"] = sceneTypeToString(sceneProject->sceneType);
    root["mainCamera"] = sceneProject->mainCamera;

    YAML::Node maxValuesNode;
    maxValuesNode["maxSubmeshes"] = sceneProject->maxValues.maxSubmeshes;
    maxValuesNode["maxTilemapTilesRect"] = sceneProject->maxValues.maxTilemapTilesRect;
    maxValuesNode["maxTilemapTiles"] = sceneProject->maxValues.maxTilemapTiles;
    maxValuesNode["maxExternalBuffers"] = sceneProject->maxValues.maxExternalBuffers;
    maxValuesNode["maxSpriteFrames"] = sceneProject->maxValues.maxSpriteFrames;
    maxValuesNode["maxBones"] = sceneProject->maxValues.maxBones;
    maxValuesNode["maxColliders2D"] = sceneProject->maxValues.maxColliders2D;
    maxValuesNode["maxColliderPoints2D"] = sceneProject->maxValues.maxColliderPoints2D;
    maxValuesNode["maxColliders3D"] = sceneProject->maxValues.maxColliders3D;
    maxValuesNode["maxColliderVertices3D"] = sceneProject->maxValues.maxColliderVertices3D;
    maxValuesNode["maxColliderIndices3D"] = sceneProject->maxValues.maxColliderIndices3D;
    root["maxValues"] = maxValuesNode;

    YAML::Node shaderKeysNode = encodeShaderKeys(sceneProject->shaderKeys);
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

    // the root keys of the file this editor does not know, after its own
    StreamSave::applyRootKeys(sceneProject->scene, NULL_ENTITY, root);

    return root;
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

    sceneProject->maxValues = {};
    if (node["maxValues"]) {
        const YAML::Node& maxValuesNode = node["maxValues"];
        if (maxValuesNode["maxSubmeshes"]) sceneProject->maxValues.maxSubmeshes = YamlNumbers::as<unsigned int>(maxValuesNode["maxSubmeshes"]);
        if (maxValuesNode["maxTilemapTilesRect"]) sceneProject->maxValues.maxTilemapTilesRect = YamlNumbers::as<unsigned int>(maxValuesNode["maxTilemapTilesRect"]);
        if (maxValuesNode["maxTilemapTiles"]) sceneProject->maxValues.maxTilemapTiles = YamlNumbers::as<unsigned int>(maxValuesNode["maxTilemapTiles"]);
        if (maxValuesNode["maxExternalBuffers"]) sceneProject->maxValues.maxExternalBuffers = YamlNumbers::as<unsigned int>(maxValuesNode["maxExternalBuffers"]);
        if (maxValuesNode["maxSpriteFrames"]) sceneProject->maxValues.maxSpriteFrames = YamlNumbers::as<unsigned int>(maxValuesNode["maxSpriteFrames"]);
        if (maxValuesNode["maxBones"]) sceneProject->maxValues.maxBones = YamlNumbers::as<unsigned int>(maxValuesNode["maxBones"]);
        if (maxValuesNode["maxColliders2D"]) sceneProject->maxValues.maxColliders2D = YamlNumbers::as<unsigned int>(maxValuesNode["maxColliders2D"]);
        if (maxValuesNode["maxColliderPoints2D"]) sceneProject->maxValues.maxColliderPoints2D = YamlNumbers::as<unsigned int>(maxValuesNode["maxColliderPoints2D"]);
        if (maxValuesNode["maxColliders3D"]) sceneProject->maxValues.maxColliders3D = YamlNumbers::as<unsigned int>(maxValuesNode["maxColliders3D"]);
        if (maxValuesNode["maxColliderVertices3D"]) sceneProject->maxValues.maxColliderVertices3D = YamlNumbers::as<unsigned int>(maxValuesNode["maxColliderVertices3D"]);
        if (maxValuesNode["maxColliderIndices3D"]) sceneProject->maxValues.maxColliderIndices3D = YamlNumbers::as<unsigned int>(maxValuesNode["maxColliderIndices3D"]);
    }

    sceneProject->shaderKeys.clear();
    for (ShaderKey key : decodeShaderKeys(node["shaderKeys"])) {
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
        scene->setShadowQuality(decodeEnum(node, "shadowQuality", StreamSchema::shadowQuality()));
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
    scene->setSceneColorScale(node["sceneColorScale"] ? decodeEnum(node, "sceneColorScale", StreamSchema::sceneColorScale()) : SceneSettings().sceneColorScale);

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

        // the node is the whole entity: what was preserved for it before (a live entity
        // restored by undo) is replaced by what this node holds
        if (PreservedStore* store = findPreservedStore(registry)) {
            store->reset(entity);
        }
        StreamLoad::EntityScope scope(registry, entity);

        if (entityNode["components"]){
            // yaml-cpp only names the key it failed on, so point at the entity holding it
            try {
                decodeComponents(entity, parent, registry, entityNode["components"]);
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
                std::vector<Entity> childEntities = decodeEntity(childNode, registry, entities, project, sceneProject, entity, createNewIfExists, removeMissingComponents, entityRemap);
                std::copy(childEntities.begin(), childEntities.end(), std::back_inserter(allEntities));
            }
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
        return !texture.empty() || CameraTextureLink::isCameraTexture(texture);
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

    // Phase 2, the same way: stencil, colour write and the extra pass when set
    encodeStencilAndColorWrite(node, material.stencil, material.colorWrite);
    if (material.extraPass != MaterialPass()) node["extraPass"] = encodeMaterialPass(material.extraPass, embedTextureData);

    return node;
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

    if (node["name"]) material.name = node["name"].as<std::string>();

    // Not merged: encodeMaterial omits these at their defaults, so an absent key is the default
    material.shader = node["shader"] ? node["shader"].as<std::string>() : std::string();
    material.shaderParams = decodeShaderUniforms(node["shaderParams"]);
    material.shaderTextures = decodeShaderTextures(node["shaderTextures"]);
    material.shaderFeatures = decodeShaderFeatures(node["shaderFeatures"]);
    material.blendMode = node["blendMode"] ? decodeEnum(node, "blendMode", StreamSchema::materialBlendMode()) : MaterialBlendMode::AUTO;
    material.cullMode = node["cullMode"] ? decodeEnum(node, "cullMode", StreamSchema::materialCullMode()) : MaterialCullMode::AUTO;
    material.depthWrite = node["depthWrite"] ? decodeEnum(node, "depthWrite", StreamSchema::materialDepthWrite()) : MaterialDepthWrite::AUTO;
    material.depthTest = node["depthTest"] ? decodeEnum(node, "depthTest", StreamSchema::materialDepthTest()) : MaterialDepthTest::AUTO;
    material.depthOffsetFactor = decodeFinite(node["depthOffsetFactor"], 0.0f);
    material.depthOffsetUnits = decodeFinite(node["depthOffsetUnits"], 0.0f);
    material.sortPriority = node["sortPriority"] ? YamlNumbers::as<int>(node["sortPriority"]) : 0;
    material.lighting = node["lighting"] ? decodeEnum(node, "lighting", StreamSchema::materialLighting()) : MaterialLighting::AUTO;
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
    YAML::Node compNode = encodeComponentsLive(entity, registry, signature);
    // what the file held that this editor could not read goes back in; whole unknown
    // components only with a full encode (a single component's copy leaves them out)
    StreamSave::applyToComponents(registry, entity, compNode, signature == registry->getSignature(entity));
    return compNode;
}

YAML::Node editor::Stream::encodeComponentsLive(const Entity entity, const EntityRegistry* registry, Signature signature) {
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
    Signature signature = registry->getSignature(entity);

    // Each component through StreamLoad::decodeComponent: a value that does not decode, or a
    // key or component this editor does not know, is kept (StreamPreserved.h) instead of
    // failing the load or vanishing on the next save

    StreamLoad::decodeComponent(ComponentType::Transform, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        Transform* existing = registry->findComponent<Transform>(entity);
        Transform transform = decodeTransform(componentNode, existing);
        transform.parent = parent;
        if (!signature.test(registry->getComponentId<Transform>())){
            registry->addComponent<Transform>(entity, std::move(transform));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Transform, existing, &transform);
            registry->addEntityChild(transform.parent, entity, false);
            registry->getComponent<Transform>(entity) = std::move(transform);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::MeshComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        MeshComponent* existing = registry->findComponent<MeshComponent>(entity);
        MeshComponent mesh = decodeMeshComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<MeshComponent>())){
            registry->addComponent<MeshComponent>(entity, std::move(mesh));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::MeshComponent, existing, &mesh);
            registry->getComponent<MeshComponent>(entity) = std::move(mesh);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::UIComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        UIComponent* existing = registry->findComponent<UIComponent>(entity);
        UIComponent ui = decodeUIComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<UIComponent>())){
            registry->addComponent<UIComponent>(entity, std::move(ui));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::UIComponent, existing, &ui);
            registry->getComponent<UIComponent>(entity) = std::move(ui);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ButtonComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ButtonComponent* existing = registry->findComponent<ButtonComponent>(entity);
        ButtonComponent button = decodeButtonComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ButtonComponent>())){
            registry->addComponent<ButtonComponent>(entity, std::move(button));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ButtonComponent, existing, &button);
            registry->getComponent<ButtonComponent>(entity) = std::move(button);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ScrollbarComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ScrollbarComponent* existing = registry->findComponent<ScrollbarComponent>(entity);
        ScrollbarComponent scrollbar = decodeScrollbarComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ScrollbarComponent>())){
            registry->addComponent<ScrollbarComponent>(entity, std::move(scrollbar));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ScrollbarComponent, existing, &scrollbar);
            registry->getComponent<ScrollbarComponent>(entity) = std::move(scrollbar);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ProgressbarComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ProgressbarComponent* existing = registry->findComponent<ProgressbarComponent>(entity);
        ProgressbarComponent progressbar = decodeProgressbarComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ProgressbarComponent>())){
            registry->addComponent<ProgressbarComponent>(entity, std::move(progressbar));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ProgressbarComponent, existing, &progressbar);
            registry->getComponent<ProgressbarComponent>(entity) = std::move(progressbar);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::InputFieldComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        InputFieldComponent* existing = registry->findComponent<InputFieldComponent>(entity);
        InputFieldComponent inputfield = decodeInputFieldComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<InputFieldComponent>())){
            registry->addComponent<InputFieldComponent>(entity, std::move(inputfield));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::InputFieldComponent, existing, &inputfield);
            registry->getComponent<InputFieldComponent>(entity) = std::move(inputfield);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::PanelComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        PanelComponent* existing = registry->findComponent<PanelComponent>(entity);
        PanelComponent panel = decodePanelComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<PanelComponent>())){
            registry->addComponent<PanelComponent>(entity, std::move(panel));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::PanelComponent, existing, &panel);
            registry->getComponent<PanelComponent>(entity) = std::move(panel);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::RectTransformComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        RectTransformComponent* existing = registry->findComponent<RectTransformComponent>(entity);
        RectTransformComponent layout = decodeRectTransformComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<RectTransformComponent>())){
            registry->addComponent<RectTransformComponent>(entity, std::move(layout));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::RectTransformComponent, existing, &layout);
            registry->getComponent<RectTransformComponent>(entity) = std::move(layout);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::LayoutGroupComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        LayoutGroupComponent* existing = registry->findComponent<LayoutGroupComponent>(entity);
        LayoutGroupComponent container = decodeLayoutGroupComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<LayoutGroupComponent>())){
            registry->addComponent<LayoutGroupComponent>(entity, std::move(container));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::LayoutGroupComponent, existing, &container);
            registry->getComponent<LayoutGroupComponent>(entity) = std::move(container);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::TextComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        TextComponent* existing = registry->findComponent<TextComponent>(entity);
        TextComponent text = decodeTextComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<TextComponent>())){
            registry->addComponent<TextComponent>(entity, std::move(text));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::TextComponent, existing, &text);
            registry->getComponent<TextComponent>(entity) = std::move(text);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ImageComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ImageComponent* existing = registry->findComponent<ImageComponent>(entity);
        ImageComponent image = decodeImageComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ImageComponent>())){
            registry->addComponent<ImageComponent>(entity, std::move(image));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ImageComponent, existing, &image);
            registry->getComponent<ImageComponent>(entity) = std::move(image);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::SpriteComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        SpriteComponent* existing = registry->findComponent<SpriteComponent>(entity);
        SpriteComponent sprite = decodeSpriteComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<SpriteComponent>())){
            registry->addComponent<SpriteComponent>(entity, std::move(sprite));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::SpriteComponent, existing, &sprite);
            registry->getComponent<SpriteComponent>(entity) = std::move(sprite);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::TilemapComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        TilemapComponent* existing = registry->findComponent<TilemapComponent>(entity);
        TilemapComponent tilemap = decodeTilemapComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<TilemapComponent>())){
            registry->addComponent<TilemapComponent>(entity, std::move(tilemap));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::TilemapComponent, existing, &tilemap);
            registry->getComponent<TilemapComponent>(entity) = std::move(tilemap);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::TerrainComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        TerrainComponent* existing = registry->findComponent<TerrainComponent>(entity);
        TerrainComponent terrain = decodeTerrainComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<TerrainComponent>())){
            registry->addComponent<TerrainComponent>(entity, std::move(terrain));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::TerrainComponent, existing, &terrain);
            registry->getComponent<TerrainComponent>(entity) = std::move(terrain);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::LightComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        LightComponent* existing = registry->findComponent<LightComponent>(entity);
        LightComponent light = decodeLightComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<LightComponent>())){
            registry->addComponent<LightComponent>(entity, std::move(light));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::LightComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::LightComponent, existing, &light);
            registry->getComponent<LightComponent>(entity) = std::move(light);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::Light2DComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        Light2DComponent* existing = registry->findComponent<Light2DComponent>(entity);
        Light2DComponent light2d = decodeLight2DComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<Light2DComponent>())){
            registry->addComponent<Light2DComponent>(entity, std::move(light2d));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::Light2DComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Light2DComponent, existing, &light2d);
            registry->getComponent<Light2DComponent>(entity) = std::move(light2d);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ShadowCaster2DComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ShadowCaster2DComponent* existing = registry->findComponent<ShadowCaster2DComponent>(entity);
        ShadowCaster2DComponent shadowcaster2d = decodeShadowCaster2DComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ShadowCaster2DComponent>())){
            registry->addComponent<ShadowCaster2DComponent>(entity, std::move(shadowcaster2d));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::ShadowCaster2DComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ShadowCaster2DComponent, existing, &shadowcaster2d);
            registry->getComponent<ShadowCaster2DComponent>(entity) = std::move(shadowcaster2d);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::FogComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        FogComponent* existing = registry->findComponent<FogComponent>(entity);
        FogComponent fog = decodeFogComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<FogComponent>())){
            registry->addComponent<FogComponent>(entity, std::move(fog));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::FogComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::FogComponent, existing, &fog);
            registry->getComponent<FogComponent>(entity) = std::move(fog);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::MirrorComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        MirrorComponent* existing = registry->findComponent<MirrorComponent>(entity);
        MirrorComponent mirror = decodeMirrorComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<MirrorComponent>())){
            registry->addComponent<MirrorComponent>(entity, std::move(mirror));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::MirrorComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::MirrorComponent, existing, &mirror);
            registry->getComponent<MirrorComponent>(entity) = std::move(mirror);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ReflectionProbeComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ReflectionProbeComponent* existing = registry->findComponent<ReflectionProbeComponent>(entity);
        ReflectionProbeComponent probe = decodeReflectionProbeComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ReflectionProbeComponent>())){
            registry->addComponent<ReflectionProbeComponent>(entity, std::move(probe));
            Catalog::updateEntity(registry, entity, Catalog::getComponentStructuralUpdateFlags(ComponentType::ReflectionProbeComponent));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ReflectionProbeComponent, existing, &probe);
            registry->getComponent<ReflectionProbeComponent>(entity) = std::move(probe);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::UIDocumentComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        UIDocumentComponent* existing = registry->findComponent<UIDocumentComponent>(entity);
        UIDocumentComponent document = decodeUIDocumentComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<UIDocumentComponent>())){
            registry->addComponent<UIDocumentComponent>(entity, std::move(document));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::UIDocumentComponent, existing, &document);
            registry->getComponent<UIDocumentComponent>(entity) = std::move(document);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::CameraComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        CameraComponent* existing = registry->findComponent<CameraComponent>(entity);
        CameraComponent camera = decodeCameraComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<CameraComponent>())){
            registry->addComponent<CameraComponent>(entity, std::move(camera));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::CameraComponent, existing, &camera);
            registry->getComponent<CameraComponent>(entity) = std::move(camera);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::AudioSourceComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        AudioSourceComponent* existing = registry->findComponent<AudioSourceComponent>(entity);
        AudioSourceComponent audio = decodeAudioSourceComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<AudioSourceComponent>())){
            registry->addComponent<AudioSourceComponent>(entity, std::move(audio));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::AudioSourceComponent, existing, &audio);
            registry->getComponent<AudioSourceComponent>(entity) = std::move(audio);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ScriptComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ScriptComponent* existing = registry->findComponent<ScriptComponent>(entity);
        ScriptComponent script = decodeScriptComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ScriptComponent>())){
            registry->addComponent<ScriptComponent>(entity, std::move(script));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ScriptComponent, existing, &script);
            registry->getComponent<ScriptComponent>(entity) = std::move(script);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::SkyboxComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        SkyboxComponent* existing = registry->findComponent<SkyboxComponent>(entity);
        SkyboxComponent sky = decodeSkyboxComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<SkyboxComponent>())){
            registry->addComponent<SkyboxComponent>(entity, std::move(sky));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::SkyboxComponent, existing, &sky);
            registry->getComponent<SkyboxComponent>(entity) = std::move(sky);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ModelComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ModelComponent* existing = registry->findComponent<ModelComponent>(entity);
        ModelComponent model = decodeModelComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ModelComponent>())){
            registry->addComponent<ModelComponent>(entity, std::move(model));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ModelComponent, existing, &model);
            registry->getComponent<ModelComponent>(entity) = std::move(model);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::Rigidbody2DComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        Rigidbody2DComponent* existing = registry->findComponent<Rigidbody2DComponent>(entity);
        Rigidbody2DComponent body = decodeRigidbody2DComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<Rigidbody2DComponent>())){
            registry->addComponent<Rigidbody2DComponent>(entity, std::move(body));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Rigidbody2DComponent, existing, &body);
            registry->getComponent<Rigidbody2DComponent>(entity) = std::move(body);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::RigidbodyComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        RigidbodyComponent* existing = registry->findComponent<RigidbodyComponent>(entity);
        Transform* bodyTransform = registry->findComponent<Transform>(entity);
        const Vector3 bodyScale = bodyTransform ? bodyTransform->scale : Vector3::UNIT_SCALE;
        RigidbodyComponent body = decodeRigidbodyComponent(componentNode, existing, bodyScale);
        if (!signature.test(registry->getComponentId<RigidbodyComponent>())){
            registry->addComponent<RigidbodyComponent>(entity, std::move(body));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::RigidbodyComponent, existing, &body);
            registry->getComponent<RigidbodyComponent>(entity) = std::move(body);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::Joint2DComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        Joint2DComponent* existing = registry->findComponent<Joint2DComponent>(entity);
        Joint2DComponent joint = decodeJoint2DComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<Joint2DComponent>())){
            registry->addComponent<Joint2DComponent>(entity, std::move(joint));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Joint2DComponent, existing, &joint);
            registry->getComponent<Joint2DComponent>(entity) = std::move(joint);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::Joint3DComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        Joint3DComponent* existing = registry->findComponent<Joint3DComponent>(entity);
        Joint3DComponent joint = decodeJoint3DComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<Joint3DComponent>())){
            registry->addComponent<Joint3DComponent>(entity, std::move(joint));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::Joint3DComponent, existing, &joint);
            registry->getComponent<Joint3DComponent>(entity) = std::move(joint);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ActionComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ActionComponent* existing = registry->findComponent<ActionComponent>(entity);
        ActionComponent action = decodeActionComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ActionComponent>())){
            registry->addComponent<ActionComponent>(entity, std::move(action));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ActionComponent, existing, &action);
            registry->getComponent<ActionComponent>(entity) = std::move(action);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::TweenComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        TweenComponent* existing = registry->findComponent<TweenComponent>(entity);
        TweenComponent timed = decodeTweenComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<TweenComponent>())){
            registry->addComponent<TweenComponent>(entity, std::move(timed));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::TweenComponent, existing, &timed);
            registry->getComponent<TweenComponent>(entity) = std::move(timed);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::PositionTweenComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        PositionTweenComponent* existing = registry->findComponent<PositionTweenComponent>(entity);
        PositionTweenComponent posAction = decodePositionTweenComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<PositionTweenComponent>())){
            registry->addComponent<PositionTweenComponent>(entity, std::move(posAction));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::PositionTweenComponent, existing, &posAction);
            registry->getComponent<PositionTweenComponent>(entity) = std::move(posAction);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::RotationTweenComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        RotationTweenComponent* existing = registry->findComponent<RotationTweenComponent>(entity);
        RotationTweenComponent rotAction = decodeRotationTweenComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<RotationTweenComponent>())){
            registry->addComponent<RotationTweenComponent>(entity, std::move(rotAction));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::RotationTweenComponent, existing, &rotAction);
            registry->getComponent<RotationTweenComponent>(entity) = std::move(rotAction);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ScaleTweenComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ScaleTweenComponent* existing = registry->findComponent<ScaleTweenComponent>(entity);
        ScaleTweenComponent scaleTween = decodeScaleTweenComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ScaleTweenComponent>())){
            registry->addComponent<ScaleTweenComponent>(entity, std::move(scaleTween));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ScaleTweenComponent, existing, &scaleTween);
            registry->getComponent<ScaleTweenComponent>(entity) = std::move(scaleTween);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ColorTweenComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ColorTweenComponent* existing = registry->findComponent<ColorTweenComponent>(entity);
        ColorTweenComponent colorTween = decodeColorTweenComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ColorTweenComponent>())){
            registry->addComponent<ColorTweenComponent>(entity, std::move(colorTween));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::ColorTweenComponent, existing, &colorTween);
            registry->getComponent<ColorTweenComponent>(entity) = std::move(colorTween);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::AlphaTweenComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        AlphaTweenComponent* existing = registry->findComponent<AlphaTweenComponent>(entity);
        AlphaTweenComponent alphaTween = decodeAlphaTweenComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<AlphaTweenComponent>())){
            registry->addComponent<AlphaTweenComponent>(entity, std::move(alphaTween));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::AlphaTweenComponent, existing, &alphaTween);
            registry->getComponent<AlphaTweenComponent>(entity) = std::move(alphaTween);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::SpriteAnimationComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        SpriteAnimationComponent* existing = registry->findComponent<SpriteAnimationComponent>(entity);
        SpriteAnimationComponent spriteanim = decodeSpriteAnimationComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<SpriteAnimationComponent>())){
            registry->addComponent<SpriteAnimationComponent>(entity, std::move(spriteanim));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::SpriteAnimationComponent, existing, &spriteanim);
            registry->getComponent<SpriteAnimationComponent>(entity) = std::move(spriteanim);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::AnimationComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        AnimationComponent* existing = registry->findComponent<AnimationComponent>(entity);
        AnimationComponent animation = decodeAnimationComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<AnimationComponent>())){
            registry->addComponent<AnimationComponent>(entity, std::move(animation));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::AnimationComponent, existing, &animation);
            registry->getComponent<AnimationComponent>(entity) = std::move(animation);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::BoneComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        BoneComponent* existing = registry->findComponent<BoneComponent>(entity);
        BoneComponent bone = decodeBoneComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<BoneComponent>())){
            registry->addComponent<BoneComponent>(entity, std::move(bone));
        }else{
            registry->getComponent<BoneComponent>(entity) = std::move(bone);
        }
    });

    StreamLoad::decodeComponent(ComponentType::KeyframeTracksComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        KeyframeTracksComponent* existing = registry->findComponent<KeyframeTracksComponent>(entity);
        KeyframeTracksComponent tracks = decodeKeyframeTracksComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<KeyframeTracksComponent>())){
            registry->addComponent<KeyframeTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<KeyframeTracksComponent>(entity) = std::move(tracks);
        }
    });

    StreamLoad::decodeComponent(ComponentType::TranslateTracksComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        TranslateTracksComponent* existing = registry->findComponent<TranslateTracksComponent>(entity);
        TranslateTracksComponent tracks = decodeTranslateTracksComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<TranslateTracksComponent>())){
            registry->addComponent<TranslateTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<TranslateTracksComponent>(entity) = std::move(tracks);
        }
    });

    StreamLoad::decodeComponent(ComponentType::RotateTracksComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        RotateTracksComponent* existing = registry->findComponent<RotateTracksComponent>(entity);
        RotateTracksComponent tracks = decodeRotateTracksComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<RotateTracksComponent>())){
            registry->addComponent<RotateTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<RotateTracksComponent>(entity) = std::move(tracks);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ScaleTracksComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ScaleTracksComponent* existing = registry->findComponent<ScaleTracksComponent>(entity);
        ScaleTracksComponent tracks = decodeScaleTracksComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ScaleTracksComponent>())){
            registry->addComponent<ScaleTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<ScaleTracksComponent>(entity) = std::move(tracks);
        }
    });

    StreamLoad::decodeComponent(ComponentType::MorphTracksComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        MorphTracksComponent* existing = registry->findComponent<MorphTracksComponent>(entity);
        MorphTracksComponent tracks = decodeMorphTracksComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<MorphTracksComponent>())){
            registry->addComponent<MorphTracksComponent>(entity, std::move(tracks));
        }else{
            registry->getComponent<MorphTracksComponent>(entity) = std::move(tracks);
        }
    });

    StreamLoad::decodeComponent(ComponentType::ParticleSystemComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        ParticleSystemComponent* existing = registry->findComponent<ParticleSystemComponent>(entity);
        ParticleSystemComponent particles = decodeParticleSystemComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<ParticleSystemComponent>())){
            registry->addComponent<ParticleSystemComponent>(entity, std::move(particles));
        }else{
            registry->getComponent<ParticleSystemComponent>(entity) = std::move(particles);
        }
    });

    StreamLoad::decodeComponent(ComponentType::PointsComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        PointsComponent* existing = registry->findComponent<PointsComponent>(entity);
        PointsComponent pts = decodePointsComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<PointsComponent>())) {
            registry->addComponent<PointsComponent>(entity, std::move(pts));
        } else {
            registry->getComponent<PointsComponent>(entity) = std::move(pts);
        }
    });

    StreamLoad::decodeComponent(ComponentType::LineRendererComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        LineRendererComponent* existing = registry->findComponent<LineRendererComponent>(entity);
        LineRendererComponent lines = decodeLineRendererComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<LineRendererComponent>())) {
            registry->addComponent<LineRendererComponent>(entity, std::move(lines));
        } else {
            registry->getComponent<LineRendererComponent>(entity) = std::move(lines);
        }
    });

    StreamLoad::decodeComponent(ComponentType::PolygonComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        PolygonComponent* existing = registry->findComponent<PolygonComponent>(entity);
        PolygonComponent polygon = decodePolygonComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<PolygonComponent>())) {
            registry->addComponent<PolygonComponent>(entity, std::move(polygon));
        } else {
            registry->getComponent<PolygonComponent>(entity) = std::move(polygon);
        }
    });

    StreamLoad::decodeComponent(ComponentType::MeshPolygonComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        MeshPolygonComponent* existing = registry->findComponent<MeshPolygonComponent>(entity);
        MeshPolygonComponent polygon = decodeMeshPolygonComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<MeshPolygonComponent>())) {
            registry->addComponent<MeshPolygonComponent>(entity, std::move(polygon));
        } else {
            registry->getComponent<MeshPolygonComponent>(entity) = std::move(polygon);
        }
    });

    StreamLoad::decodeComponent(ComponentType::InstancedMeshComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        InstancedMeshComponent* existing = registry->findComponent<InstancedMeshComponent>(entity);
        InstancedMeshComponent instmesh = decodeInstancedMeshComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<InstancedMeshComponent>())){
            registry->addComponent<InstancedMeshComponent>(entity, std::move(instmesh));
        }else{
            uint64_t flags = Catalog::getChangedUpdateFlags(ComponentType::InstancedMeshComponent, existing, &instmesh);
            registry->getComponent<InstancedMeshComponent>(entity) = std::move(instmesh);
            Catalog::updateEntity(registry, entity, flags);
        }
    });

    StreamLoad::decodeComponent(ComponentType::PrefabComponent, entity, registry, compNode, [&](const YAML::Node& componentNode) {
        PrefabComponent* existing = registry->findComponent<PrefabComponent>(entity);
        PrefabComponent prefab = decodePrefabComponent(componentNode, existing);
        if (!signature.test(registry->getComponentId<PrefabComponent>())){
            registry->addComponent<PrefabComponent>(entity, std::move(prefab));
        }else{
            registry->getComponent<PrefabComponent>(entity) = std::move(prefab);
        }
    });

    StreamLoad::preserveUnknownComponents(entity, registry, compNode);
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
    node["transparent"] = YamlNumbers::text(mesh.transparent);
    node["autoTransparency"] = YamlNumbers::text(mesh.autoTransparency);

    node["cullingMode"] = cullingModeToString(mesh.cullingMode);
    node["windingOrder"] = windingOrderToString(mesh.windingOrder);

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
    if (node["transparent"]) mesh.transparent = YamlNumbers::as<bool>(node["transparent"]);
    if (node["autoTransparency"]) mesh.autoTransparency = YamlNumbers::as<bool>(node["autoTransparency"]);

    if (node["cullingMode"]) mesh.cullingMode = decodeEnum(node, "cullingMode", StreamSchema::cullingMode());
    if (node["windingOrder"]) mesh.windingOrder = decodeEnum(node, "windingOrder", StreamSchema::windingOrder());

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
    if (node["type"]) scrollbar.type = decodeEnum(node, "type", StreamSchema::scrollbarType());
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
    if (node["type"]) progressbar.type = decodeEnum(node, "type", StreamSchema::progressbarType());
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
    if (node["titleAnchorPreset"]) panel.titleAnchorPreset = decodeEnum(node, "titleAnchorPreset", StreamSchema::anchorPreset());
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
    if (node["anchorPreset"]) layout.anchorPreset = decodeEnum(node, "anchorPreset", StreamSchema::anchorPreset());
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

    if (node["type"]) container.type = decodeEnum(node, "type", StreamSchema::layoutGroupType());
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

    if (node["type"]) light.type = decodeEnum(node, "type", StreamSchema::lightType());
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
    light.shading = node["shading"] ? decodeEnum(node, "shading", StreamSchema::lightShading()) : LightShading::BOTH;

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
    return editor::StreamSchema::shadowCaster2DShape().toString(shape);
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

    if (node["shape"]) occluder.shape = decodeEnum(node, "shape", StreamSchema::shadowCaster2DShape());
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

    if (node["type"]) fog.type = decodeEnum(node, "type", StreamSchema::fogType());
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
    document.renderMode = given("renderMode") ? decodeEnum(node, "renderMode", StreamSchema::uiDocumentRenderMode()) : defaults.renderMode;
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

    if (node["type"]) camera.type = decodeEnum(node, "type", StreamSchema::cameraType());
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
    if (node["framebufferFilter"]) camera.framebufferFilter = decodeEnum(node, "framebufferFilter", StreamSchema::textureFilter());

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

    if (node["state"]) audio.state = decodeEnum(node, "state", StreamSchema::audioSourceState());
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
    if (node["attenuationModel"]) audio.attenuationModel = decodeEnum(node, "attenuationModel", StreamSchema::audioSourceAttenuation());
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
            entry.type = decodeEnum(scriptNode, "type", StreamSchema::scriptType());
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

YAML::Node editor::Stream::encodeJoint2DComponent(const Joint2DComponent& joint) {
    YAML::Node node;
    node["enabled"] = joint.enabled;

    node["type"] = joint2DTypeToString(joint.type);
    node["bodyA"] = joint.bodyA;
    node["bodyB"] = joint.bodyB;
    node["anchorA"] = encodeVector2(joint.anchorA);
    node["anchorB"] = encodeVector2(joint.anchorB);
    node["axis"] = encodeVector2(joint.axis);
    node["target"] = encodeVector2(joint.target);
    node["autoAnchors"] = joint.autoAnchors;
    node["rope"] = joint.rope;

    return node;
}

Joint2DComponent editor::Stream::decodeJoint2DComponent(const YAML::Node& node, const Joint2DComponent* oldJoint) {
    Joint2DComponent joint;

    if (oldJoint) {
        joint = *oldJoint;
    }

    if (node["type"]) joint.type = decodeEnum(node, "type", StreamSchema::joint2DType());
    if (node["bodyA"]) joint.bodyA = YamlNumbers::as<Entity>(node["bodyA"]);
    if (node["bodyB"]) joint.bodyB = YamlNumbers::as<Entity>(node["bodyB"]);
    if (node["anchorA"]) joint.anchorA = decodeVector2(node["anchorA"]);
    if (node["anchorB"]) joint.anchorB = decodeVector2(node["anchorB"]);
    if (node["axis"]) joint.axis = decodeVector2(node["axis"]);
    if (node["target"]) joint.target = decodeVector2(node["target"]);
    if (node["autoAnchors"]) joint.autoAnchors = YamlNumbers::as<bool>(node["autoAnchors"]);
    if (node["rope"]) joint.rope = YamlNumbers::as<bool>(node["rope"]);

    if (oldJoint && b2Joint_IsValid(oldJoint->joint)) {
        joint.needUpdateJoint = true;
    }

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

    if (node["type"]) joint.type = decodeEnum(node, "type", StreamSchema::joint3DType());
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
    return StreamSchema::actionState().toString(state);
}

ActionState editor::Stream::stringToActionState(const std::string& str) {
    return StreamSchema::actionState().fromString(str);
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

    if (node["state"]) action.state = decodeEnum(node, "state", StreamSchema::actionState());
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

    if (node["index"]) tracks.index = YamlNumbers::as<int>(node["index"]);
    if (node["interpolation"]) tracks.interpolation = YamlNumbers::as<float>(node["interpolation"]);

    if (node["times"]) {
        tracks.times.clear();
        for (const YAML::Node& t : node["times"]) {
            tracks.times.push_back(YamlNumbers::as<float>(t));
        }
    }

    tracks.easings.clear();
    if (node["easings"]) {
        for (const YAML::Node& e : node["easings"]) {
            tracks.easings.push_back(decodeEnumValue(e, StreamSchema::easeType()));
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

    if (node["customShader"]) lines.customShader = node["customShader"].as<std::string>();
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
