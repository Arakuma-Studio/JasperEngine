// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "reflect/TypeOps.h"
#include "util/LightPhotometry.h"

#include "Factory.h"
#include "Stream.h"
#include "StreamPreserved.h"
#include "stream/CurveCodec.h"
#include "util/YamlNumbers.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <cmath>
#include <cstdint>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::reflect;

namespace {

    // node.as<T>() through YamlNumbers; false with the reason when the node holds no T
    template<typename T>
    bool readScalar(const YAML::Node& node, T& out, std::string& reason) {
        try {
            out = YamlNumbers::as<T>(node);
            return true;
        } catch (const YAML::Exception& e) {
            reason = e.msg;
            return false;
        }
    }

    // A value that does not read: S03 keeps the authored text (a stand-in at a component's top
    // level) and the field keeps its base value
    bool unreadable(const YAML::Node& node, const std::string& reason, std::string& out) {
        out = reason.empty() ? std::string("bad conversion") : reason;
        StreamLoad::unreadableValue(node, out);
        return false;
    }

}

// ---------------------------------------------------------------- comparisons

bool reflect::detail::floatEquals(float a, float b, float epsilon) {
    if (epsilon < 0.0f) return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
    if (epsilon == 0.0f) return a == b;
    return std::fabs(a - b) <= epsilon;
}

bool reflect::detail::doubleEquals(double a, double b, float epsilon) {
    if (epsilon < 0.0f) return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
    if (epsilon == 0.0f) return a == b;
    return std::fabs(a - b) <= epsilon;
}

bool reflect::detail::floatsEqual(const float* a, const float* b, size_t count, float epsilon) {
    for (size_t i = 0; i < count; i++) {
        if (!floatEquals(a[i], b[i], epsilon)) return false;
    }
    return true;
}

bool reflect::detail::equalFloat(const void* a, const void* b, float epsilon) {
    return floatEquals(*static_cast<const float*>(a), *static_cast<const float*>(b), epsilon);
}

bool reflect::detail::equalDouble(const void* a, const void* b, float epsilon) {
    return doubleEquals(*static_cast<const double*>(a), *static_cast<const double*>(b), epsilon);
}

bool reflect::detail::equalMatrix4(const void* a, const void* b, float epsilon) {
    const jasperengine::Matrix4& x = *static_cast<const jasperengine::Matrix4*>(a);
    const jasperengine::Matrix4& y = *static_cast<const jasperengine::Matrix4*>(b);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (!floatEquals(x[i][j], y[i][j], epsilon)) return false;
        }
    }
    return true;
}

bool reflect::detail::equalRect(const void* a, const void* b, float epsilon) {
    const jasperengine::Rect& x = *static_cast<const jasperengine::Rect*>(a);
    const jasperengine::Rect& y = *static_cast<const jasperengine::Rect*>(b);
    return floatEquals(x.getX(), y.getX(), epsilon) && floatEquals(x.getY(), y.getY(), epsilon) &&
           floatEquals(x.getWidth(), y.getWidth(), epsilon) && floatEquals(x.getHeight(), y.getHeight(), epsilon);
}

bool reflect::detail::equalQuat(const void* a, const void* b, float epsilon) {
    const Quaternion& x = *static_cast<const Quaternion*>(a);
    const Quaternion& y = *static_cast<const Quaternion*>(b);
    return floatEquals(x.w, y.w, epsilon) && floatEquals(x.x, y.x, epsilon) && floatEquals(x.y, y.y, epsilon) &&
           floatEquals(x.z, y.z, epsilon);
}

bool reflect::detail::equalTexture(const void* a, const void* b, float) {
    return *static_cast<const jasperengine::Texture*>(a) == *static_cast<const jasperengine::Texture*>(b);
}

bool reflect::detail::equalEase(const void* a, const void* b, float) {
    return *static_cast<const jasperengine::Ease*>(a) == *static_cast<const jasperengine::Ease*>(b);
}

bool reflect::detail::equalEntityReference(const void* a, const void* b, float) {
    const jasperengine::EntityReference& x = *static_cast<const jasperengine::EntityReference*>(a);
    const jasperengine::EntityReference& y = *static_cast<const jasperengine::EntityReference*>(b);
    return x.entity == y.entity && x.sceneId == y.sceneId;
}

bool reflect::detail::equalAssetRef(const void* a, const void* b, float epsilon) {
    const jasperengine::AssetRef& x = *static_cast<const jasperengine::AssetRef*>(a);
    const jasperengine::AssetRef& y = *static_cast<const jasperengine::AssetRef*>(b);
    // the Inspector compares paths (a GUID is filled in from the file at the path)
    return epsilon > 0.0f ? x.path == y.path : x == y;
}

bool reflect::detail::equalFonts(const void* a, const void* b, float epsilon) {
    const FontArray& x = *static_cast<const FontArray*>(a);
    const FontArray& y = *static_cast<const FontArray*>(b);
    for (size_t i = 0; i < x.size(); i++) {
        if (!equalAssetRef(&x[i], &y[i], epsilon)) return false;
    }
    return true;
}

bool reflect::detail::structEqual(const StructDesc& desc, const void* a, const void* b, float epsilon) {
    for (const FieldDesc& field : desc.fields) {
        if (field.flags & (Proxy | NoSave)) continue;
        void* x = field.ref(const_cast<void*>(a));
        void* y = field.ref(const_cast<void*>(b));
        if (!field.ops->equals || !field.ops->equals(x, y, epsilon)) return false;
    }
    return true;
}

bool reflect::detail::finiteFloat(const void* value) {
    return std::isfinite(*static_cast<const float*>(value));
}

bool reflect::detail::finiteDouble(const void* value) {
    return std::isfinite(*static_cast<const double*>(value));
}

bool reflect::detail::finiteVector3(const void* value) {
    return static_cast<const jasperengine::Vector3*>(value)->isValid();
}

// ---------------------------------------------------------------- YAML

YAML::Node reflect::detail::boolToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return YAML::Node(YamlNumbers::text(*static_cast<const bool*>(value)));
}

bool reflect::detail::boolFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    bool read = false;
    std::string why;
    if (!readScalar(node, read, why)) return unreadable(node, why, reason);
    *static_cast<bool*>(value) = read;
    return true;
}

YAML::Node reflect::detail::intToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return YAML::Node(YamlNumbers::integerText(*static_cast<const int*>(value)));
}

bool reflect::detail::intFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    int read = 0;
    std::string why;
    if (!readScalar(node, read, why)) return unreadable(node, why, reason);
    *static_cast<int*>(value) = read;
    return true;
}

YAML::Node reflect::detail::uintToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return YAML::Node(YamlNumbers::integerText(*static_cast<const unsigned int*>(value)));
}

bool reflect::detail::uintFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    unsigned int read = 0;
    std::string why;
    if (!readScalar(node, read, why)) return unreadable(node, why, reason);
    *static_cast<unsigned int*>(value) = read;
    return true;
}

YAML::Node reflect::detail::floatToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return YAML::Node(YamlNumbers::text(*static_cast<const float*>(value)));
}

bool reflect::detail::floatFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    float read = 0.0f;
    std::string why;
    if (!readScalar(node, read, why)) return unreadable(node, why, reason);
    *static_cast<float*>(value) = read;
    return true;
}

bool reflect::detail::finiteFloatFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    float& out = *static_cast<float*>(value);
    out = Stream::decodeFinite(node, out);
    return true;
}

bool reflect::detail::positiveFiniteFloatFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    float& out = *static_cast<float*>(value);
    out = Stream::decodePositiveFinite(node, out);
    return true;
}

bool reflect::detail::skipNotPositiveFinite(const FieldDesc&, const void* value) {
    const float number = *static_cast<const float*>(value);
    return !(std::isfinite(number) && number > 0.0f);
}

YAML::Node reflect::detail::doubleToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return YAML::Node(YamlNumbers::text(*static_cast<const double*>(value)));
}

bool reflect::detail::doubleFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    double read = 0.0;
    std::string why;
    if (!readScalar(node, read, why)) return unreadable(node, why, reason);
    *static_cast<double*>(value) = read;
    return true;
}

bool reflect::detail::finiteDoubleFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    double& out = *static_cast<double*>(value);
    out = Stream::decodeFiniteDouble(node, out);
    return true;
}

YAML::Node reflect::detail::vector2ToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeVector2(*static_cast<const jasperengine::Vector2*>(value));
}

bool reflect::detail::vector2FromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    *static_cast<jasperengine::Vector2*>(value) = Stream::decodeVector2(node);
    return true;
}

YAML::Node reflect::detail::vector3ToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeVector3(*static_cast<const jasperengine::Vector3*>(value));
}

bool reflect::detail::vector3FromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    *static_cast<jasperengine::Vector3*>(value) = Stream::decodeVector3(node);
    return true;
}

bool reflect::detail::finiteVector3FromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    jasperengine::Vector3& out = *static_cast<jasperengine::Vector3*>(value);
    out = Stream::decodeFiniteVector3(node, out);
    return true;
}

YAML::Node reflect::detail::vector4ToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeVector4(*static_cast<const jasperengine::Vector4*>(value));
}

bool reflect::detail::vector4FromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    *static_cast<jasperengine::Vector4*>(value) = Stream::decodeVector4(node);
    return true;
}

YAML::Node reflect::detail::quatToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeQuaternion(*static_cast<const Quaternion*>(value));
}

bool reflect::detail::quatFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    *static_cast<Quaternion*>(value) = Stream::decodeQuaternion(node);
    return true;
}

YAML::Node reflect::detail::matrix4ToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeMatrix4(*static_cast<const jasperengine::Matrix4*>(value));
}

bool reflect::detail::matrix4FromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<jasperengine::Matrix4*>(value) = Stream::decodeMatrix4(node);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

YAML::Node reflect::detail::rectToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeRect(*static_cast<const jasperengine::Rect*>(value));
}

bool reflect::detail::rectFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    *static_cast<jasperengine::Rect*>(value) = Stream::decodeRect(node);
    return true;
}

YAML::Node reflect::detail::stringToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return YAML::Node(*static_cast<const std::string*>(value));
}

bool reflect::detail::stringFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<std::string*>(value) = node.as<std::string>();
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

YAML::Node reflect::detail::layerMaskToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return YAML::Node(YamlNumbers::integerText(*static_cast<const uint32_t*>(value)));
}

bool reflect::detail::layerMaskFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    uint32_t& out = *static_cast<uint32_t*>(value);
    out = Stream::decodeLayerMask(node, out);
    return true;
}

YAML::Node reflect::detail::easeToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeEase(*static_cast<const jasperengine::Ease*>(value));
}

bool reflect::detail::easeFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    jasperengine::Ease& out = *static_cast<jasperengine::Ease*>(value);
    const jasperengine::Ease old = out;
    out = Stream::decodeEase(node, &old);
    return true;
}

YAML::Node reflect::detail::curveToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return curvecodec::curveToYaml(*static_cast<const jasperengine::Curve*>(value));
}

bool reflect::detail::curveFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    return curvecodec::curveFromYaml(node, *static_cast<jasperengine::Curve*>(value), reason);
}

YAML::Node reflect::detail::gradientToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return curvecodec::gradientToYaml(*static_cast<const jasperengine::Gradient*>(value));
}

bool reflect::detail::gradientFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    return curvecodec::gradientFromYaml(node, *static_cast<jasperengine::Gradient*>(value), reason);
}

YAML::Node reflect::detail::textureToYaml(const FieldDesc&, const void* value, const EncodeContext& ctx) {
    return Stream::encodeTexture(*static_cast<const jasperengine::Texture*>(value), ctx.embedTextureData);
}

bool reflect::detail::textureFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<jasperengine::Texture*>(value) = Stream::decodeTexture(node);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

void reflect::detail::assetRefEncodeKey(const FieldDesc&, const void* value, YAML::Node& map, const std::string& key, const EncodeContext&) {
    Stream::encodeAssetRef(map, key, *static_cast<const jasperengine::AssetRef*>(value));
}

bool reflect::detail::assetRefDecodeKey(const FieldDesc&, const YAML::Node& map, const std::string& key, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<jasperengine::AssetRef*>(value) = Stream::decodeAssetRef(map, key);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(map[key], e.msg, reason);
    }
}

void reflect::detail::shaderRefEncodeKey(const FieldDesc&, const void* value, YAML::Node& map, const std::string& key, const EncodeContext&) {
    Stream::encodeShaderRef(map, key, *static_cast<const jasperengine::ShaderRef*>(value));
}

bool reflect::detail::shaderRefDecodeKey(const FieldDesc&, const YAML::Node& map, const std::string& key, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<jasperengine::ShaderRef*>(value) = Stream::decodeShaderRef(map, key);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(map[key], e.msg, reason);
    }
}

YAML::Node reflect::detail::enumToYaml(const FieldDesc& field, const void* value, const EncodeContext&) {
    return YAML::Node(std::string(enumToken(*field.enumDesc, readEnum(field, value))));
}

bool reflect::detail::enumFromYaml(const FieldDesc& field, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    // an unknown name reads as the fallback, reported and kept by S03 (decodeEnumValue)
    const EnumDesc& desc = *field.enumDesc;
    if (node && node.IsScalar()) {
        if (const EnumItem* item = enumItemByToken(desc, node.Scalar())) {
            writeEnum(field, value, item->value);
            return true;
        }
    }
    StreamLoad::unknownEnumValue(node, desc.cppType, enumTokens(desc));
    writeEnum(field, value, desc.fallback);
    return true;
}

YAML::Node reflect::detail::structToYaml(const FieldDesc& field, const void* value, const EncodeContext& ctx) {
    return encode(*field.structDesc, value, ctx);
}

bool reflect::detail::structFromYaml(const FieldDesc& field, const YAML::Node& node, void* value, const DecodeContext& ctx, std::string&) {
    decode(*field.structDesc, node, value, ctx);
    return true;
}

YAML::Node reflect::detail::arrayToYaml(const FieldDesc& field, const void* value, const EncodeContext& ctx) {
    // NullIfEmpty: the list node starts null and becomes a sequence with its first element
    YAML::Node node = (field.flags & NullIfEmpty) ? YAML::Node() : YAML::Node(YAML::NodeType::Sequence);
    void* array = const_cast<void*>(value);
    const size_t size = field.arrayOps->size(array);
    for (size_t i = 0; i < size; i++) {
        const void* element = field.arrayOps->at(array, i);
        if (field.structDesc) {
            node.push_back(encode(*field.structDesc, element, ctx));
        } else {
            node.push_back(field.elementOps->toYaml(field, element, ctx));
        }
    }
    return node;
}

bool reflect::detail::arrayFromYaml(const FieldDesc& field, const YAML::Node& node, void* value, const DecodeContext& ctx, std::string&) {
    // the elements replace the base's, each read over a default element
    field.arrayOps->resize(value, 0);
    if (!node || !node.IsSequence()) {
        return true;
    }
    for (size_t i = 0; i < node.size(); i++) {
        field.arrayOps->resize(value, i + 1);
        void* element = field.arrayOps->at(value, i);
        if (field.structDesc) {
            decode(*field.structDesc, node[i], element, ctx);
        } else {
            std::string reason;
            field.elementOps->fromYaml(field, node[i], element, ctx, reason);
        }
    }
    return true;
}

// ---------------------------------------------------------------- exported code

std::string reflect::detail::boolLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatBool(*static_cast<const bool*>(value));
}

std::string reflect::detail::intLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatInt(*static_cast<const int*>(value));
}

std::string reflect::detail::uintLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatUInt(*static_cast<const unsigned int*>(value));
}

std::string reflect::detail::floatLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatFloat(*static_cast<const float*>(value));
}

std::string reflect::detail::positiveFloatLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatPositiveFloat(*static_cast<const float*>(value));
}

std::string reflect::detail::doubleLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatDouble(*static_cast<const double*>(value));
}

std::string reflect::detail::vector2Literal(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatVector2(*static_cast<const jasperengine::Vector2*>(value));
}

std::string reflect::detail::vector3Literal(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatVector3(*static_cast<const jasperengine::Vector3*>(value));
}

std::string reflect::detail::vector4Literal(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatVector4(*static_cast<const jasperengine::Vector4*>(value));
}

std::string reflect::detail::quatLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatQuaternion(*static_cast<const Quaternion*>(value));
}

std::string reflect::detail::rectLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatRect(*static_cast<const jasperengine::Rect*>(value));
}

std::string reflect::detail::stringLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatString(*static_cast<const std::string*>(value));
}

std::string reflect::detail::layerMaskLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatLayerMask(*static_cast<const uint32_t*>(value));
}

std::string reflect::detail::easeLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatEase(*static_cast<const jasperengine::Ease*>(value));
}

std::string reflect::detail::curveLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return curvecodec::curveCppLiteral(*static_cast<const jasperengine::Curve*>(value));
}

std::string reflect::detail::gradientLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return curvecodec::gradientCppLiteral(*static_cast<const jasperengine::Gradient*>(value));
}

std::string reflect::detail::entityLiteral(const FieldDesc&, const void* value, const CodegenContext& ctx) {
    return Factory::formatEntity(*static_cast<const jasperengine::Entity*>(value), ctx.entityVarNames);
}

std::string reflect::detail::assetRefLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatString(static_cast<const jasperengine::AssetRef*>(value)->path);
}

std::string reflect::detail::enumLiteral(const FieldDesc& field, const void* value, const CodegenContext&) {
    const int number = readEnum(field, value);
    const EnumItem* item = enumItem(*field.enumDesc, number);
    if (!item) item = enumItem(*field.enumDesc, field.enumDesc->fallback);
    return item ? item->cppName : std::string();
}

// ---------------------------------------------------------------- scalar texts

std::string reflect::detail::boolScalar(const FieldDesc&, const void* value) {
    return YamlNumbers::text(*static_cast<const bool*>(value));
}

std::string reflect::detail::intScalar(const FieldDesc&, const void* value) {
    return YamlNumbers::integerText(*static_cast<const int*>(value));
}

std::string reflect::detail::uintScalar(const FieldDesc&, const void* value) {
    return YamlNumbers::integerText(*static_cast<const unsigned int*>(value));
}

std::string reflect::detail::floatScalar(const FieldDesc&, const void* value) {
    return YamlNumbers::text(*static_cast<const float*>(value));
}

std::string reflect::detail::doubleScalar(const FieldDesc&, const void* value) {
    return YamlNumbers::text(*static_cast<const double*>(value));
}

std::string reflect::detail::stringScalar(const FieldDesc&, const void* value) {
    return *static_cast<const std::string*>(value);
}

std::string reflect::detail::enumScalar(const FieldDesc& field, const void* value) {
    return enumToken(*field.enumDesc, readEnum(field, value));
}

// ---------------------------------------------------------------- shader library, characters

YAML::Node reflect::detail::shaderUniformsToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeShaderUniforms(*static_cast<const jasperengine::ShaderUniformValues*>(value));
}

bool reflect::detail::shaderUniformsFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<jasperengine::ShaderUniformValues*>(value) = Stream::decodeShaderUniforms(node);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

YAML::Node reflect::detail::shaderTexturesToYaml(const FieldDesc&, const void* value, const EncodeContext& ctx) {
    return Stream::encodeShaderTextures(*static_cast<const std::vector<std::pair<std::string, jasperengine::Texture>>*>(value), ctx.embedTextureData);
}

bool reflect::detail::shaderTexturesFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<std::vector<std::pair<std::string, jasperengine::Texture>>*>(value) = Stream::decodeShaderTextures(node);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

YAML::Node reflect::detail::shaderFeaturesToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeShaderFeatures(*static_cast<const std::vector<std::string>*>(value));
}

bool reflect::detail::shaderFeaturesFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<std::vector<std::string>*>(value) = Stream::decodeShaderFeatures(node);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

YAML::Node reflect::detail::shaderGlobalBindingsToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeShaderGlobalBindings(*static_cast<const jasperengine::ShaderGlobalBindings*>(value));
}

bool reflect::detail::shaderGlobalBindingsFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<jasperengine::ShaderGlobalBindings*>(value) = Stream::decodeShaderGlobalBindings(node);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

YAML::Node reflect::detail::shaderPalettesToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return Stream::encodeShaderPalettes(*static_cast<const jasperengine::pixelart::ShaderPalettes*>(value));
}

bool reflect::detail::shaderPalettesFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        *static_cast<jasperengine::pixelart::ShaderPalettes*>(value) = Stream::decodeShaderPalettes(node);
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

bool reflect::detail::skipEmptyShaderPalettes(const FieldDesc&, const void* value) {
    return static_cast<const jasperengine::pixelart::ShaderPalettes*>(value)->empty();
}

bool reflect::detail::skipEmptyShaderGlobalBindings(const FieldDesc&, const void* value) {
    return static_cast<const jasperengine::ShaderGlobalBindings*>(value)->empty();
}

std::string reflect::detail::charScalar(const FieldDesc&, const void* value) {
    return std::string(1, *static_cast<const char*>(value));
}

bool reflect::detail::charFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    try {
        const std::string text = node.as<std::string>();
        *static_cast<char*>(value) = text.empty() ? '*' : text[0];
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

std::string reflect::detail::charLiteral(const FieldDesc&, const void* value, const CodegenContext&) {
    return Factory::formatChar(*static_cast<const char*>(value));
}

bool reflect::detail::uintFromIntYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    int read = 0;
    std::string why;
    if (!readScalar(node, read, why)) return unreadable(node, why, reason);
    *static_cast<unsigned int*>(value) = static_cast<unsigned int>(read);
    return true;
}

bool reflect::detail::skipEmptyAssetPath(const FieldDesc&, const void* value) {
    return static_cast<const jasperengine::AssetRef*>(value)->empty();
}

bool reflect::detail::intClampedFromYaml(const FieldDesc& field, const YAML::Node& node, void* value, const DecodeContext&, std::string& reason) {
    int read = 0;
    std::string why;
    if (!readScalar(node, read, why)) return unreadable(node, why, reason);
    const int low = std::isfinite(field.min) ? static_cast<int>(field.min) : std::numeric_limits<int>::min();
    const int high = std::isfinite(field.max) ? static_cast<int>(field.max) : std::numeric_limits<int>::max();
    *static_cast<int*>(value) = std::clamp(read, low, high);
    return true;
}

YAML::Node reflect::detail::kelvinToYaml(const FieldDesc&, const void* value, const EncodeContext&) {
    return YAML::Node(YamlNumbers::text(Stream::finiteOr(*static_cast<const float*>(value), LightPhotometry::WHITE_TEMPERATURE)));
}

bool reflect::detail::kelvinFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    *static_cast<float*>(value) = std::clamp(Stream::decodeFinite(node, LightPhotometry::WHITE_TEMPERATURE), LightPhotometry::MIN_TEMPERATURE,
                                             LightPhotometry::MAX_TEMPERATURE);
    return true;
}

bool reflect::detail::nonNegativeFiniteFloatFromYaml(const FieldDesc&, const YAML::Node& node, void* value, const DecodeContext&, std::string&) {
    *static_cast<float*>(value) = std::max(0.0f, Stream::decodeFinite(node, 0.0f));
    return true;
}

void reflect::detail::fontEncodeKey(const FieldDesc&, const void* value, YAML::Node& map, const std::string& key, const EncodeContext&) {
    const FontArray& fonts = *static_cast<const FontArray*>(value);
    size_t numFonts = 0;
    for (size_t i = 0; i < fonts.size(); i++) {
        if (!fonts[i].empty()) numFonts = i + 1;
    }
    map[key] = YAML::Node(YAML::NodeType::Sequence);
    map[key].SetStyle(YAML::EmitterStyle::Flow);
    bool hasGuids = false;
    for (size_t i = 0; i < numFonts; i++) {
        map[key].push_back(fonts[i].path);
        hasGuids |= !fonts[i].guid.isNull();
    }
    if (hasGuids) {
        const std::string guidKey = key + "Guid";
        map[guidKey] = YAML::Node(YAML::NodeType::Sequence);
        map[guidKey].SetStyle(YAML::EmitterStyle::Flow);
        for (size_t i = 0; i < numFonts; i++) map[guidKey].push_back(fonts[i].guid.str());
    }
}

bool reflect::detail::fontDecodeKey(const FieldDesc&, const YAML::Node& map, const std::string& key, void* value, const DecodeContext&, std::string& reason) {
    const YAML::Node node = map[key];
    try {
        FontArray fonts;
        if (node.IsSequence()) {
            const YAML::Node guids = map[key + "Guid"];
            for (size_t i = 0; i < node.size() && i < fonts.size(); i++) {
                fonts[i] = node[i].as<std::string>();
                if (guids && guids.IsSequence() && i < guids.size()) fonts[i].guid = Guid::parse(guids[i].as<std::string>());
            }
        } else {
            // before the array: a single font
            fonts[0] = node.as<std::string>();
        }
        *static_cast<FontArray*>(value) = fonts;
        return true;
    } catch (const YAML::Exception& e) {
        return unreadable(node, e.msg, reason);
    }
}

// ---------------------------------------------------------------- exported code statements

void reflect::detail::textureStatements(const FieldDesc&, const void* value, const std::string& target, const CodegenContext& ctx, std::string& out) {
    out += Factory::formatTexture(ctx.indentSpaces, *static_cast<const jasperengine::Texture*>(value), target, ctx.projectPath);
}

void reflect::detail::fontStatements(const FieldDesc&, const void* value, const std::string& target, const CodegenContext& ctx, std::string& out) {
    // the slots in use
    const FontArray& fonts = *static_cast<const FontArray*>(value);
    for (size_t i = 0; i < fonts.size(); i++) {
        if (!fonts[i].empty()) {
            out += Factory::indentation(ctx.indentSpaces) + target + "[" + std::to_string(i) + "] = " + Factory::formatString(fonts[i]) + ";\n";
        }
    }
}

void reflect::detail::pushBackStatements(const FieldDesc& field, const void* value, const std::string& target, const CodegenContext& ctx, std::string& out) {
    void* array = const_cast<void*>(value);
    const std::string ind = Factory::indentation(ctx.indentSpaces);
    const size_t size = field.arrayOps->size(array);
    for (size_t i = 0; i < size; i++) {
        out += ind + target + ".push_back(" + field.elementOps->cppLiteral(field, field.arrayOps->at(array, i), ctx) + ");\n";
    }
}

void reflect::detail::pushBackClearedStatements(const FieldDesc& field, const void* value, const std::string& target, const CodegenContext& ctx, std::string& out) {
    out += Factory::indentation(ctx.indentSpaces) + target + ".clear();\n";
    pushBackStatements(field, value, target, ctx, out);
}

void reflect::detail::structPushBackStatements(const FieldDesc& field, const void* value, const std::string& target, const CodegenContext& ctx,
                                               std::string& out) {
    void* array = const_cast<void*>(value);
    const std::string ind = Factory::indentation(ctx.indentSpaces);
    for (size_t i = 0; i < field.arrayOps->size(array); i++) {
        void* element = field.arrayOps->at(array, i);
        std::string literal = "{";
        for (size_t m = 0; m < field.structDesc->fields.size(); m++) {
            const FieldDesc& member = field.structDesc->fields[m];
            if (m > 0) literal += ", ";
            literal += member.ops->cppLiteral ? member.ops->cppLiteral(member, member.ref(element), ctx) : std::string("{}");
        }
        out += ind + target + ".push_back(" + literal + "});\n";
    }
}
