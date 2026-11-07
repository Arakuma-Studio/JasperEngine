// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The TypeOps of every value type the tables use (docs/specs/S08-reflection.md D2, D4): how a
// value is constructed, copied, compared, written to and read from YAML, exported to C++ and
// described in JSON Schema. The YAML text comes from the scene codec's own helpers (Stream's
// encodeVector3, YamlNumbers, encodeTexture, decodeLayerMask...): no new formatting code.
//
// Variants for the encodings some fields have always had (a camera's finite clip planes, a
// positive field of view) are ops of their own that a field names (FieldMeta::ops).

#include "reflect/Reflect.h"

#include "action/Ease.h"
#include "component/TextComponent.h"
#include "math/Curve.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "script/ScriptProperty.h"
#include "shader/ShaderUniforms.h"
#include "texture/Texture.h"
#include "asset/AssetRef.h"
#include "util/GrowArray.h"
#include "util/StableArray.h"

#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace jasperengine::editor::reflect {

    namespace detail {

        template<typename T> void construct(void* at) { new (at) T(); }
        template<typename T> void destroy(void* at) { static_cast<T*>(at)->~T(); }
        template<typename T> void copy(void* dst, const void* src) { *static_cast<T*>(dst) = *static_cast<const T*>(src); }
        template<typename T> void copyConstruct(void* at, const void* src) { new (at) T(*static_cast<const T*>(src)); }
        template<typename T> void moveAssign(void* dst, void* src) { *static_cast<T*>(dst) = std::move(*static_cast<T*>(src)); }
        template<typename T> bool equalByOperator(const void* a, const void* b, float) {
            return *static_cast<const T*>(a) == *static_cast<const T*>(b);
        }

        // epsilon > 0: within it; 0: operator== (-0 equals 0, NaN equals nothing); < 0: bitwise
        bool floatEquals(float a, float b, float epsilon);
        bool doubleEquals(double a, double b, float epsilon);
        bool floatsEqual(const float* a, const float* b, size_t count, float epsilon);

        template<typename T> bool equalFloats(const void* a, const void* b, float epsilon) {
            return floatsEqual(reinterpret_cast<const float*>(a), reinterpret_cast<const float*>(b), sizeof(T) / sizeof(float), epsilon);
        }
        bool equalFloat(const void* a, const void* b, float epsilon);
        bool equalDouble(const void* a, const void* b, float epsilon);
        bool equalMatrix4(const void* a, const void* b, float epsilon);
        bool equalRect(const void* a, const void* b, float epsilon);
        bool equalQuat(const void* a, const void* b, float epsilon);

        std::string boolScalar(const FieldDesc&, const void*);
        std::string intScalar(const FieldDesc&, const void*);
        std::string uintScalar(const FieldDesc&, const void*);
        std::string floatScalar(const FieldDesc&, const void*);
        std::string doubleScalar(const FieldDesc&, const void*);
        std::string stringScalar(const FieldDesc&, const void*);
        std::string enumScalar(const FieldDesc&, const void*);

        constexpr TypeOps withScalar(TypeOps ops, std::string (*scalar)(const FieldDesc&, const void*)) {
            ops.scalar = scalar;
            return ops;
        }

        // the same ops with another exported-code literal, or another skip rule
        constexpr TypeOps withLiteral(TypeOps ops, std::string (*literal)(const FieldDesc&, const void*, const CodegenContext&)) {
            ops.cppLiteral = literal;
            return ops;
        }
        constexpr TypeOps withSkip(TypeOps ops, bool (*skip)(const FieldDesc&, const void*)) {
            ops.skip = skip;
            return ops;
        }
        // the same ops with another JSON Schema
        constexpr TypeOps withSchema(TypeOps ops, Json (*schema)(const FieldDesc&)) {
            ops.schema = schema;
            return ops;
        }
        constexpr TypeOps withStatements(TypeOps ops, void (*statements)(const FieldDesc&, const void*, const std::string&,
                                                                         const CodegenContext&, std::string&)) {
            ops.cppStatements = statements;
            return ops;
        }

        void textureStatements(const FieldDesc&, const void*, const std::string&, const CodegenContext&, std::string&);
        void fontStatements(const FieldDesc&, const void*, const std::string&, const CodegenContext&, std::string&);
        // the elements of a list as push_backs (pushBackCleared: a clear() first)
        void pushBackStatements(const FieldDesc&, const void*, const std::string&, const CodegenContext&, std::string&);
        void pushBackClearedStatements(const FieldDesc&, const void*, const std::string&, const CodegenContext&, std::string&);
        // a list of structs exported as push_backs of each element's aggregate initializer (its
        // members in table order, which must be their declaration order)
        void structPushBackStatements(const FieldDesc&, const void*, const std::string&, const CodegenContext&, std::string&);

        bool skipEmptyAssetPath(const FieldDesc&, const void* value);

        bool finiteFloat(const void* value);
        bool finiteDouble(const void* value);
        bool finiteVector3(const void* value);

        // ---- YAML of each type
        YAML::Node boolToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool boolFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node intToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool intFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node uintToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool uintFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node floatToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool floatFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        bool finiteFloatFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        bool positiveFiniteFloatFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        bool skipNotPositiveFinite(const FieldDesc&, const void*);
        YAML::Node doubleToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool doubleFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        bool finiteDoubleFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node vector2ToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool vector2FromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node vector3ToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool vector3FromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        bool finiteVector3FromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node vector4ToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool vector4FromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node quatToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool quatFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node matrix4ToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool matrix4FromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node rectToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool rectFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node stringToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool stringFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node layerMaskToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool layerMaskFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node easeToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool easeFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node curveToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool curveFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node gradientToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool gradientFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node textureToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool textureFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        void assetRefEncodeKey(const FieldDesc&, const void*, YAML::Node&, const std::string&, const EncodeContext&);
        bool assetRefDecodeKey(const FieldDesc&, const YAML::Node&, const std::string&, void*, const DecodeContext&, std::string&);
        void shaderRefEncodeKey(const FieldDesc&, const void*, YAML::Node&, const std::string&, const EncodeContext&);
        bool shaderRefDecodeKey(const FieldDesc&, const YAML::Node&, const std::string&, void*, const DecodeContext&, std::string&);
        YAML::Node enumToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool enumFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node structToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool structFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node arrayToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool arrayFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);

        // ---- exported code
        std::string boolLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string intLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string uintLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string floatLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string positiveFloatLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string doubleLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string vector2Literal(const FieldDesc&, const void*, const CodegenContext&);
        std::string vector3Literal(const FieldDesc&, const void*, const CodegenContext&);
        std::string vector4Literal(const FieldDesc&, const void*, const CodegenContext&);
        std::string quatLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string rectLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string stringLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string layerMaskLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string easeLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string curveLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string gradientLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string entityLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string assetRefLiteral(const FieldDesc&, const void*, const CodegenContext&);
        std::string enumLiteral(const FieldDesc&, const void*, const CodegenContext&);

        // ---- JSON Schema
        Json boolSchema(const FieldDesc&);
        Json intSchema(const FieldDesc&);
        Json colorWriteSchema(const FieldDesc&);
        Json uintSchema(const FieldDesc&);
        Json numberSchema(const FieldDesc&);
        Json numberOrNullSchema(const FieldDesc&);
        Json vector2Schema(const FieldDesc&);
        Json vector3Schema(const FieldDesc&);
        Json vector4Schema(const FieldDesc&);
        Json quatSchema(const FieldDesc&);
        Json matrix4Schema(const FieldDesc&);
        Json rectSchema(const FieldDesc&);
        Json stringSchema(const FieldDesc&);
        Json enumSchema(const FieldDesc&);
        Json easeSchema(const FieldDesc&);
        Json curveSchema(const FieldDesc&);
        Json gradientSchema(const FieldDesc&);
        Json entitySchema(const FieldDesc&);
        Json entityReferenceSchema(const FieldDesc&);
        Json textureSchema(const FieldDesc&);
        Json fontSchema(const FieldDesc&);
        Json materialSchema(const FieldDesc&);
        Json structSchema(const FieldDesc&);
        Json arraySchema(const FieldDesc&);
        Json customSchema(const FieldDesc&);

        // the scene codec's own encodings of the shader library collections and the password character
        YAML::Node shaderUniformsToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool shaderUniformsFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node shaderTexturesToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool shaderTexturesFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node shaderFeaturesToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool shaderFeaturesFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node shaderGlobalBindingsToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool shaderGlobalBindingsFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        bool skipEmptyShaderGlobalBindings(const FieldDesc&, const void* value);
        std::string charScalar(const FieldDesc&, const void*);
        bool charFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        std::string charLiteral(const FieldDesc&, const void*, const CodegenContext&);
        bool uintFromIntYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        bool intClampedFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        bool nonNegativeFiniteFloatFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);
        YAML::Node kelvinToYaml(const FieldDesc&, const void*, const EncodeContext&);
        bool kelvinFromYaml(const FieldDesc&, const YAML::Node&, void*, const DecodeContext&, std::string&);

        void fontEncodeKey(const FieldDesc&, const void*, YAML::Node&, const std::string&, const EncodeContext&);
        bool fontDecodeKey(const FieldDesc&, const YAML::Node&, const std::string&, void*, const DecodeContext&, std::string&);

        bool equalTexture(const void* a, const void* b, float);
        bool equalEase(const void* a, const void* b, float);
        bool equalEntityReference(const void* a, const void* b, float);
        bool equalAssetRef(const void* a, const void* b, float);
        bool equalFonts(const void* a, const void* b, float);
        // every field of a struct (Struct and Array fields included) equal
        bool structEqual(const StructDesc& desc, const void* a, const void* b, float epsilon);

        template<typename S> bool equalStructT(const void* a, const void* b, float epsilon);

    }

    // ---- the ops of each type
    namespace ops {

        inline constexpr TypeOps Bool = detail::withScalar(TypeOps{"bool", Kind::Bool, PropertyType::Bool, sizeof(bool),
            &detail::construct<bool>, &detail::destroy<bool>, &detail::copy<bool>, &detail::equalByOperator<bool>,
            &detail::boolToYaml, &detail::boolFromYaml, nullptr, nullptr, nullptr, false, &detail::boolLiteral, &detail::boolSchema}, &detail::boolScalar);
        inline constexpr TypeOps Int = detail::withScalar(TypeOps{"int", Kind::Int, PropertyType::Int, sizeof(int),
            &detail::construct<int>, &detail::destroy<int>, &detail::copy<int>, &detail::equalByOperator<int>,
            &detail::intToYaml, &detail::intFromYaml, nullptr, nullptr, nullptr, false, &detail::intLiteral, &detail::intSchema}, &detail::intScalar);
        inline constexpr TypeOps UInt = detail::withScalar(TypeOps{"unsigned int", Kind::UInt, PropertyType::UInt, sizeof(unsigned int),
            &detail::construct<unsigned int>, &detail::destroy<unsigned int>, &detail::copy<unsigned int>, &detail::equalByOperator<unsigned int>,
            &detail::uintToYaml, &detail::uintFromYaml, nullptr, nullptr, nullptr, false, &detail::uintLiteral, &detail::uintSchema}, &detail::uintScalar);
        inline constexpr TypeOps Float = detail::withScalar(TypeOps{"float", Kind::Float, PropertyType::Float, sizeof(float),
            &detail::construct<float>, &detail::destroy<float>, &detail::copy<float>, &detail::equalFloat,
            &detail::floatToYaml, &detail::floatFromYaml, nullptr, nullptr, nullptr, false, &detail::floatLiteral, &detail::numberSchema,
            &detail::finiteFloat}, &detail::floatScalar);
        // a float whose infinity is a value (a joint's break force and torque, a drive's force limits:
        // none): MCP reads an infinite one as null, which its schema takes too
        // Its fields are CodegenIfChanged, as they default to infinity: Factory::formatFloat makes what is
        // not finite 0, which would break a joint at its first step
        inline constexpr TypeOps FloatOrInfinity = detail::withSchema(Float, &detail::numberOrNullSchema);
        // written only when finite (Stream::encodeFinite); a non-finite or unreadable value keeps
        // the base (made finite: Stream::decodeFinite)
        inline constexpr TypeOps FiniteFloat = detail::withScalar(TypeOps{"float", Kind::Float, PropertyType::Float, sizeof(float),
            &detail::construct<float>, &detail::destroy<float>, &detail::copy<float>, &detail::equalFloat,
            &detail::floatToYaml, &detail::finiteFloatFromYaml, nullptr, nullptr, nullptr, false, &detail::floatLiteral, &detail::numberSchema,
            &detail::finiteFloat}, &detail::floatScalar);
        // the same, also read when absent (the base made finite: the camera's clip planes)
        inline constexpr TypeOps FiniteFloatAlways = detail::withScalar(TypeOps{"float", Kind::Float, PropertyType::Float, sizeof(float),
            &detail::construct<float>, &detail::destroy<float>, &detail::copy<float>, &detail::equalFloat,
            &detail::floatToYaml, &detail::finiteFloatFromYaml, nullptr, nullptr, nullptr, true, &detail::floatLiteral, &detail::numberSchema,
            &detail::finiteFloat}, &detail::floatScalar);
        // written only when finite and positive (Stream::encodePositiveFinite), read always
        // (Stream::decodePositiveFinite: the camera's field of view and aspect)
        inline constexpr TypeOps PositiveFiniteFloatAlways = detail::withScalar(TypeOps{"float", Kind::Float, PropertyType::Float, sizeof(float),
            &detail::construct<float>, &detail::destroy<float>, &detail::copy<float>, &detail::equalFloat,
            &detail::floatToYaml, &detail::positiveFiniteFloatFromYaml, nullptr, nullptr, &detail::skipNotPositiveFinite, true,
            &detail::positiveFloatLiteral, &detail::numberSchema, &detail::finiteFloat}, &detail::floatScalar);
        inline constexpr TypeOps Double = detail::withScalar(TypeOps{"double", Kind::Double, PropertyType::Double, sizeof(double),
            &detail::construct<double>, &detail::destroy<double>, &detail::copy<double>, &detail::equalDouble,
            &detail::doubleToYaml, &detail::doubleFromYaml, nullptr, nullptr, nullptr, false, &detail::doubleLiteral, &detail::numberSchema,
            &detail::finiteDouble}, &detail::doubleScalar);
        inline constexpr TypeOps FiniteDouble = detail::withScalar(TypeOps{"double", Kind::Double, PropertyType::Double, sizeof(double),
            &detail::construct<double>, &detail::destroy<double>, &detail::copy<double>, &detail::equalDouble,
            &detail::doubleToYaml, &detail::finiteDoubleFromYaml, nullptr, nullptr, nullptr, false, &detail::doubleLiteral, &detail::numberSchema,
            &detail::finiteDouble}, &detail::doubleScalar);
        inline constexpr TypeOps Vector2{"Vector2", Kind::Vector2, PropertyType::Vector2, sizeof(jasperengine::Vector2),
            &detail::construct<jasperengine::Vector2>, &detail::destroy<jasperengine::Vector2>, &detail::copy<jasperengine::Vector2>,
            &detail::equalFloats<jasperengine::Vector2>, &detail::vector2ToYaml, &detail::vector2FromYaml, nullptr, nullptr, nullptr, false,
            &detail::vector2Literal, &detail::vector2Schema};
        inline constexpr TypeOps Vector3{"Vector3", Kind::Vector3, PropertyType::Vector3, sizeof(jasperengine::Vector3),
            &detail::construct<jasperengine::Vector3>, &detail::destroy<jasperengine::Vector3>, &detail::copy<jasperengine::Vector3>,
            &detail::equalFloats<jasperengine::Vector3>, &detail::vector3ToYaml, &detail::vector3FromYaml, nullptr, nullptr, nullptr, false,
            &detail::vector3Literal, &detail::vector3Schema, &detail::finiteVector3};
        // written only when every lane is finite, read always: the base made finite (the camera's
        // target and up, Stream::encodeFiniteVector3 / decodeFiniteVector3)
        inline constexpr TypeOps FiniteVector3Always{"Vector3", Kind::Vector3, PropertyType::Vector3, sizeof(jasperengine::Vector3),
            &detail::construct<jasperengine::Vector3>, &detail::destroy<jasperengine::Vector3>, &detail::copy<jasperengine::Vector3>,
            &detail::equalFloats<jasperengine::Vector3>, &detail::vector3ToYaml, &detail::finiteVector3FromYaml, nullptr, nullptr, nullptr, true,
            &detail::vector3Literal, &detail::vector3Schema, &detail::finiteVector3};
        inline constexpr TypeOps Vector4{"Vector4", Kind::Vector4, PropertyType::Vector4, sizeof(jasperengine::Vector4),
            &detail::construct<jasperengine::Vector4>, &detail::destroy<jasperengine::Vector4>, &detail::copy<jasperengine::Vector4>,
            &detail::equalFloats<jasperengine::Vector4>, &detail::vector4ToYaml, &detail::vector4FromYaml, nullptr, nullptr, nullptr, false,
            &detail::vector4Literal, &detail::vector4Schema};
        inline constexpr TypeOps Quat{"Quaternion", Kind::Quat, PropertyType::Quat, sizeof(Quaternion),
            &detail::construct<Quaternion>, &detail::destroy<Quaternion>, &detail::copy<Quaternion>, &detail::equalQuat,
            &detail::quatToYaml, &detail::quatFromYaml, nullptr, nullptr, nullptr, false, &detail::quatLiteral, &detail::quatSchema};
        inline constexpr TypeOps Matrix4{"Matrix4", Kind::Matrix4, PropertyType::Custom, sizeof(jasperengine::Matrix4),
            &detail::construct<jasperengine::Matrix4>, &detail::destroy<jasperengine::Matrix4>, &detail::copy<jasperengine::Matrix4>,
            &detail::equalMatrix4, &detail::matrix4ToYaml, &detail::matrix4FromYaml, nullptr, nullptr, nullptr, false, nullptr, &detail::matrix4Schema};
        inline constexpr TypeOps Rect{"Rect", Kind::Rect, PropertyType::Vector4, sizeof(jasperengine::Rect),
            &detail::construct<jasperengine::Rect>, &detail::destroy<jasperengine::Rect>, &detail::copy<jasperengine::Rect>, &detail::equalRect,
            &detail::rectToYaml, &detail::rectFromYaml, nullptr, nullptr, nullptr, false, &detail::rectLiteral, &detail::rectSchema};
        inline constexpr TypeOps String = detail::withScalar(TypeOps{"std::string", Kind::String, PropertyType::String, sizeof(std::string),
            &detail::construct<std::string>, &detail::destroy<std::string>, &detail::copy<std::string>, &detail::equalByOperator<std::string>,
            &detail::stringToYaml, &detail::stringFromYaml, nullptr, nullptr, nullptr, false, &detail::stringLiteral, &detail::stringSchema}, &detail::stringScalar);
        // a uint32_t mask, one bit per layer (written unsigned, -1 reads as every layer)
        inline constexpr TypeOps LayerMask = detail::withScalar(TypeOps{"uint32_t", Kind::LayerMask, PropertyType::UInt, sizeof(uint32_t),
            &detail::construct<uint32_t>, &detail::destroy<uint32_t>, &detail::copy<uint32_t>, &detail::equalByOperator<uint32_t>,
            &detail::layerMaskToYaml, &detail::layerMaskFromYaml, nullptr, nullptr, nullptr, false, &detail::layerMaskLiteral, &detail::uintSchema}, &detail::uintScalar);
        inline constexpr TypeOps Ease{"Ease", Kind::Ease, PropertyType::Ease, sizeof(jasperengine::Ease),
            &detail::construct<jasperengine::Ease>, &detail::destroy<jasperengine::Ease>, &detail::copy<jasperengine::Ease>, &detail::equalEase,
            &detail::easeToYaml, &detail::easeFromYaml, nullptr, nullptr, nullptr, false, &detail::easeLiteral, &detail::easeSchema};
        // an animation curve: the one curve encoding (editor/stream/CurveCodec.h)
        inline constexpr TypeOps Curve{"Curve", Kind::Custom, PropertyType::Curve, sizeof(jasperengine::Curve),
            &detail::construct<jasperengine::Curve>, &detail::destroy<jasperengine::Curve>, &detail::copy<jasperengine::Curve>,
            &detail::equalByOperator<jasperengine::Curve>, &detail::curveToYaml, &detail::curveFromYaml, nullptr, nullptr, nullptr, false,
            &detail::curveLiteral, &detail::curveSchema};
        // a colour gradient (math/Curve.h): the same encoding's gradient form
        inline constexpr TypeOps Gradient{"Gradient", Kind::Custom, PropertyType::Gradient, sizeof(jasperengine::Gradient),
            &detail::construct<jasperengine::Gradient>, &detail::destroy<jasperengine::Gradient>, &detail::copy<jasperengine::Gradient>,
            &detail::equalByOperator<jasperengine::Gradient>, &detail::gradientToYaml, &detail::gradientFromYaml, nullptr, nullptr, nullptr, false,
            &detail::gradientLiteral, &detail::gradientSchema};
        // a GameObject of the same scene (its id)
        inline constexpr TypeOps Entity = detail::withScalar(TypeOps{"Entity", Kind::Entity, PropertyType::Entity, sizeof(jasperengine::Entity),
            &detail::construct<jasperengine::Entity>, &detail::destroy<jasperengine::Entity>, &detail::copy<jasperengine::Entity>,
            &detail::equalByOperator<jasperengine::Entity>, &detail::uintToYaml, &detail::uintFromYaml, nullptr, nullptr, nullptr, false,
            &detail::entityLiteral, &detail::entitySchema}, &detail::uintScalar);
        inline constexpr TypeOps EntityReference{"EntityReference", Kind::EntityReference, PropertyType::EntityReference,
            sizeof(jasperengine::EntityReference), &detail::construct<jasperengine::EntityReference>, &detail::destroy<jasperengine::EntityReference>,
            &detail::copy<jasperengine::EntityReference>, &detail::equalEntityReference, nullptr, nullptr, nullptr, nullptr, nullptr, false,
            nullptr, &detail::entityReferenceSchema};
        inline constexpr TypeOps Texture = detail::withStatements(TypeOps{"Texture", Kind::Texture, PropertyType::Texture,
            sizeof(jasperengine::Texture), &detail::construct<jasperengine::Texture>, &detail::destroy<jasperengine::Texture>,
            &detail::copy<jasperengine::Texture>, &detail::equalTexture, &detail::textureToYaml, &detail::textureFromYaml, nullptr, nullptr,
            nullptr, false, nullptr, &detail::textureSchema}, &detail::textureStatements);
        // the main font and its fallbacks: "<key>" a flow list of paths up to the last one set (always
        // written, so a merging decode sees a cleared chain), "<key>Guid" their GUIDs when any
        inline constexpr TypeOps Font = detail::withStatements(TypeOps{"FontArray", Kind::Font, PropertyType::Font, sizeof(FontArray),
            &detail::construct<FontArray>, &detail::destroy<FontArray>, &detail::copy<FontArray>, &detail::equalFonts,
            nullptr, nullptr, &detail::fontEncodeKey, &detail::fontDecodeKey, nullptr, false, nullptr, &detail::fontSchema},
            &detail::fontStatements);
        // an asset path with its GUID (written as "<key>" and "<key>Guid", Stream::encodeAssetRef)
        inline constexpr TypeOps AssetRef{"AssetRef", Kind::AssetPath, PropertyType::Asset, sizeof(jasperengine::AssetRef),
            &detail::construct<jasperengine::AssetRef>, &detail::destroy<jasperengine::AssetRef>, &detail::copy<jasperengine::AssetRef>,
            &detail::equalAssetRef, nullptr, nullptr, &detail::assetRefEncodeKey, &detail::assetRefDecodeKey, nullptr, false,
            &detail::assetRefLiteral, &detail::stringSchema};
        inline constexpr TypeOps ShaderRef{"ShaderRef", Kind::AssetPath, PropertyType::Shader, sizeof(jasperengine::ShaderRef),
            &detail::construct<jasperengine::ShaderRef>, &detail::destroy<jasperengine::ShaderRef>, &detail::copy<jasperengine::ShaderRef>,
            &detail::equalByOperator<jasperengine::ShaderRef>, nullptr, nullptr, &detail::shaderRefEncodeKey, &detail::shaderRefDecodeKey, nullptr, false,
            nullptr, &detail::stringSchema};

        // an unsigned field read as a signed one, wrapping (the nine-patch margins have been)
        inline constexpr TypeOps UIntFromInt = detail::withScalar(TypeOps{"unsigned int", Kind::UInt, PropertyType::UInt, sizeof(unsigned int),
            &detail::construct<unsigned int>, &detail::destroy<unsigned int>, &detail::copy<unsigned int>, &detail::equalByOperator<unsigned int>,
            &detail::uintToYaml, &detail::uintFromIntYaml, nullptr, nullptr, nullptr, false, &detail::uintLiteral, &detail::uintSchema},
            &detail::uintScalar);
        // a color write mask (-1: the shader's, 0..15: r 1, g 2, b 4, a 8), which MCP also reads and
        // writes by name ("auto", "rgb"...)
        inline constexpr TypeOps ColorWrite = detail::withScalar(TypeOps{"int", Kind::Int, PropertyType::Int, sizeof(int),
            &detail::construct<int>, &detail::destroy<int>, &detail::copy<int>, &detail::equalByOperator<int>,
            &detail::intToYaml, &detail::intFromYaml, nullptr, nullptr, nullptr, false, &detail::intLiteral, &detail::colorWriteSchema},
            &detail::intScalar);
        // an int clamped to the field's hard range when read (a UI document's texture size)
        inline constexpr TypeOps IntClampedOnRead = detail::withScalar(TypeOps{"int", Kind::Int, PropertyType::Int, sizeof(int),
            &detail::construct<int>, &detail::destroy<int>, &detail::copy<int>, &detail::equalByOperator<int>,
            &detail::intToYaml, &detail::intClampedFromYaml, nullptr, nullptr, nullptr, false, &detail::intLiteral, &detail::intSchema},
            &detail::intScalar);
        // a float written when finite; read finite and not below 0 (anything else reads as 0)
        inline constexpr TypeOps NonNegativeFiniteFloat = detail::withScalar(TypeOps{"float", Kind::Float, PropertyType::Float, sizeof(float),
            &detail::construct<float>, &detail::destroy<float>, &detail::copy<float>, &detail::equalFloat,
            &detail::floatToYaml, &detail::nonNegativeFiniteFloatFromYaml, nullptr, nullptr, nullptr, false, &detail::floatLiteral,
            &detail::numberSchema, &detail::finiteFloat}, &detail::floatScalar);
        // a colour temperature in Kelvin (LightPhotometry): written finite (a non-finite one as 6500,
        // white), read finite (else 6500) and clamped to 1000..20000
        // (no scalar fast path: the text is not the value's own)
        inline constexpr TypeOps Kelvin{"float", Kind::Float, PropertyType::Float, sizeof(float),
            &detail::construct<float>, &detail::destroy<float>, &detail::copy<float>, &detail::equalFloat,
            &detail::kelvinToYaml, &detail::kelvinFromYaml, nullptr, nullptr, nullptr, false, &detail::floatLiteral,
            &detail::numberSchema, &detail::finiteFloat};
        // one character written as a one-character string (an empty one reads as '*')
        inline constexpr TypeOps Char = detail::withScalar(TypeOps{"char", Kind::String, PropertyType::Custom, sizeof(char),
            &detail::construct<char>, &detail::destroy<char>, &detail::copy<char>, &detail::equalByOperator<char>,
            nullptr, &detail::charFromYaml, nullptr, nullptr, nullptr, false, &detail::charLiteral, &detail::stringSchema},
            &detail::charScalar);
        // an asset reference written only when it has a path
        inline constexpr TypeOps AssetRefIfSet = detail::withSkip(AssetRef, &detail::skipEmptyAssetPath);
        // shaderUniforms of a component: {member: [x,y,z,w]} (reserved names skipped)
        inline constexpr TypeOps ShaderUniformValues{"ShaderUniformValues", Kind::Custom, PropertyType::Custom,
            sizeof(jasperengine::ShaderUniformValues), &detail::construct<jasperengine::ShaderUniformValues>,
            &detail::destroy<jasperengine::ShaderUniformValues>, &detail::copy<jasperengine::ShaderUniformValues>,
            &detail::equalByOperator<jasperengine::ShaderUniformValues>, &detail::shaderUniformsToYaml, &detail::shaderUniformsFromYaml,
            nullptr, nullptr, nullptr, false, nullptr, &detail::customSchema};
        // shaderTextures: {uniform: texture}
        inline constexpr TypeOps ShaderTextures{"ShaderTextures", Kind::Custom, PropertyType::Custom,
            sizeof(std::vector<std::pair<std::string, jasperengine::Texture>>), &detail::construct<std::vector<std::pair<std::string, jasperengine::Texture>>>,
            &detail::destroy<std::vector<std::pair<std::string, jasperengine::Texture>>>, &detail::copy<std::vector<std::pair<std::string, jasperengine::Texture>>>,
            &detail::equalByOperator<std::vector<std::pair<std::string, jasperengine::Texture>>>, &detail::shaderTexturesToYaml,
            &detail::shaderTexturesFromYaml, nullptr, nullptr, nullptr, false, nullptr, &detail::customSchema};
        // shaderFeatures: the defines on (and "!F" off)
        inline constexpr TypeOps ShaderFeatures{"ShaderFeatures", Kind::Custom, PropertyType::Custom, sizeof(std::vector<std::string>),
            &detail::construct<std::vector<std::string>>, &detail::destroy<std::vector<std::string>>, &detail::copy<std::vector<std::string>>,
            &detail::equalByOperator<std::vector<std::string>>, &detail::shaderFeaturesToYaml, &detail::shaderFeaturesFromYaml,
            nullptr, nullptr, nullptr, false, nullptr, &detail::customSchema};
        // shaderGlobalBindings: {member: global} (shader globals), written only when it binds one
        inline constexpr TypeOps ShaderGlobalBindings{"ShaderGlobalBindings", Kind::Custom, PropertyType::Custom,
            sizeof(jasperengine::ShaderGlobalBindings), &detail::construct<jasperengine::ShaderGlobalBindings>,
            &detail::destroy<jasperengine::ShaderGlobalBindings>, &detail::copy<jasperengine::ShaderGlobalBindings>,
            &detail::equalByOperator<jasperengine::ShaderGlobalBindings>, &detail::shaderGlobalBindingsToYaml,
            &detail::shaderGlobalBindingsFromYaml, nullptr, nullptr, &detail::skipEmptyShaderGlobalBindings, false, nullptr,
            &detail::customSchema};

        template<typename E>
        inline constexpr TypeOps Enum = detail::withScalar(TypeOps{"enum", Kind::Enum, PropertyType::Enum, sizeof(E),
            &detail::construct<E>, &detail::destroy<E>, &detail::copy<E>, &detail::equalByOperator<E>,
            &detail::enumToYaml, &detail::enumFromYaml, nullptr, nullptr, nullptr, false, &detail::enumLiteral, &detail::enumSchema}, &detail::enumScalar);

        template<typename S>
        inline constexpr TypeOps Struct{"struct", Kind::Struct, PropertyType::Custom, sizeof(S),
            &detail::construct<S>, &detail::destroy<S>, &detail::copy<S>, &detail::equalStructT<S>,
            &detail::structToYaml, &detail::structFromYaml, nullptr, nullptr, nullptr, false, nullptr, &detail::structSchema};

        // an unsigned count stored narrower or wider than unsigned int (a collider's uint16_t
        // numVertices, a body's size_t numColliders): a UInt property of the Catalog, in a
        // component with a codec of its own
        template<typename T>
        inline constexpr TypeOps UIntAs{"unsigned", Kind::UInt, PropertyType::UInt, sizeof(T),
            &detail::construct<T>, &detail::destroy<T>, &detail::copy<T>, &detail::equalByOperator<T>,
            nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, &detail::uintSchema};

        // a value the tables do not look into (a gradient, a list of bursts), compared with its
        // operator==, in a component with a codec of its own
        template<typename T>
        inline constexpr TypeOps Opaque{"value", Kind::Custom, PropertyType::Custom, sizeof(T),
            &detail::construct<T>, &detail::destroy<T>, &detail::copy<T>, &detail::equalByOperator<T>,
            nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, &detail::customSchema};

        // a struct with members (Struct<S>) that is a Material property of the Catalog
        template<typename S>
        inline constexpr TypeOps MaterialStruct{"Material", Kind::Struct, PropertyType::Material, sizeof(S),
            &detail::construct<S>, &detail::destroy<S>, &detail::copy<S>, &detail::equalStructT<S>,
            nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, &detail::materialSchema};

        // a whole component or nested struct (its value type only: no codec of its own)
        template<typename S>
        inline constexpr TypeOps Value{.typeName = "value", .kind = Kind::Struct, .propertyType = PropertyType::Custom, .size = sizeof(S),
            .construct = &detail::construct<S>, .destroy = &detail::destroy<S>, .copy = &detail::copy<S>, .equals = nullptr,
            .toYaml = nullptr, .fromYaml = nullptr, .copyConstruct = &detail::copyConstruct<S>, .move = &detail::moveAssign<S>};

    }

    namespace detail {

        template<typename T> size_t vectorSize(const void* array) { return static_cast<const std::vector<T>*>(array)->size(); }
        template<typename T> void vectorResize(void* array, size_t size) { static_cast<std::vector<T>*>(array)->resize(size); }
        template<typename T> void* vectorAt(void* array, size_t index) { return &(*static_cast<std::vector<T>*>(array))[index]; }
        template<typename T> void vectorInsert(void* array, size_t index, const void* value) {
            auto& vector = *static_cast<std::vector<T>*>(array);
            vector.insert(vector.begin() + static_cast<std::ptrdiff_t>(std::min(index, vector.size())), *static_cast<const T*>(value));
        }
        template<typename T> void vectorErase(void* array, size_t index) {
            auto& vector = *static_cast<std::vector<T>*>(array);
            if (index < vector.size()) vector.erase(vector.begin() + static_cast<std::ptrdiff_t>(index));
        }
        template<typename T> void vectorMove(void* array, size_t from, size_t to) {
            auto& vector = *static_cast<std::vector<T>*>(array);
            if (from >= vector.size() || to >= vector.size() || from == to) return;
            T value = std::move(vector[from]);
            vector.erase(vector.begin() + static_cast<std::ptrdiff_t>(from));
            vector.insert(vector.begin() + static_cast<std::ptrdiff_t>(to), std::move(value));
        }
        bool equalArray(const void* a, const void* b, float epsilon);

        template<typename T, size_t N> size_t fixedSize(const void*) { return N; }
        template<typename T, size_t N> void fixedResize(void*, size_t) {}
        template<typename T, size_t N> void* fixedAt(void* array, size_t index) { return &static_cast<T*>(array)[index]; }
        template<typename T, size_t N> void fixedInsert(void*, size_t, const void*) {}
        template<typename T, size_t N> void fixedErase(void*, size_t) {}
        template<typename T, size_t N> void fixedMove(void* array, size_t from, size_t to) {
            T* items = static_cast<T*>(array);
            if (from >= N || to >= N || from == to) return;
            T value = std::move(items[from]);
            if (from < to) {
                for (size_t i = from; i < to; i++) items[i] = std::move(items[i + 1]);
            } else {
                for (size_t i = from; i > to; i--) items[i] = std::move(items[i - 1]);
            }
            items[to] = std::move(value);
        }
        template<typename T, size_t N> void fixedConstruct(void* at) { new (at) T[N](); }
        template<typename T, size_t N> void fixedDestroy(void* at) {
            T* items = static_cast<T*>(at);
            for (size_t i = 0; i < N; i++) items[i].~T();
        }
        template<typename T, size_t N> void fixedCopy(void* dst, const void* src) {
            std::copy(static_cast<const T*>(src), static_cast<const T*>(src) + N, static_cast<T*>(dst));
        }
        template<typename T, size_t N> bool equalFixed(const void* a, const void* b, float epsilon);

        template<typename T> bool equalVector(const void* a, const void* b, float epsilon);

    }

    namespace ops {

        // a Rect seen as its four lanes (x, y, width, height: a struct of proxies the field names)
        inline constexpr TypeOps RectLanes{"Rect", Kind::Struct, PropertyType::Custom, sizeof(jasperengine::Rect),
            &detail::construct<jasperengine::Rect>, &detail::destroy<jasperengine::Rect>, &detail::copy<jasperengine::Rect>, &detail::equalRect,
            &detail::rectToYaml, &detail::rectFromYaml, nullptr, nullptr, nullptr, false, &detail::rectLiteral, &detail::structSchema};

        template<typename T>
        inline constexpr ArrayOps VectorArray{&detail::vectorSize<T>, &detail::vectorResize<T>, &detail::vectorAt<T>,
            &detail::vectorInsert<T>, &detail::vectorErase<T>, &detail::vectorMove<T>};

        // std::vector<T>: elements by the field's elementOps (equality element by element)
        template<typename T>
        inline constexpr TypeOps Array{"std::vector", Kind::Array, PropertyType::Custom, sizeof(std::vector<T>),
            &detail::construct<std::vector<T>>, &detail::destroy<std::vector<T>>, &detail::copy<std::vector<T>>, &detail::equalVector<T>,
            &detail::arrayToYaml, &detail::arrayFromYaml, nullptr, nullptr, nullptr, false, nullptr, &detail::arraySchema};

        // a list exported as push_backs of its element literals (PushBackListCleared: after a clear())
        template<typename T>
        inline constexpr TypeOps PushBackList = detail::withStatements(Array<T>, &detail::pushBackStatements);
        template<typename T>
        inline constexpr TypeOps PushBackListCleared = detail::withStatements(Array<T>, &detail::pushBackClearedStatements);

    }

    namespace ops {

        // T[N]: a fixed number of elements (runtime state such as a layout's boxes)
        template<typename T, size_t N>
        inline constexpr ArrayOps FixedArray{&detail::fixedSize<T, N>, &detail::fixedResize<T, N>, &detail::fixedAt<T, N>,
            &detail::fixedInsert<T, N>, &detail::fixedErase<T, N>, &detail::fixedMove<T, N>};
        template<typename T, size_t N>
        inline constexpr TypeOps Fixed{"array", Kind::Array, PropertyType::Custom, sizeof(T) * N,
            &detail::fixedConstruct<T, N>, &detail::fixedDestroy<T, N>, &detail::fixedCopy<T, N>, &detail::equalFixed<T, N>,
            &detail::arrayToYaml, nullptr, nullptr, nullptr, nullptr, false, nullptr, &detail::arraySchema};

    }

    namespace detail {

        // GrowArray<T> and StableArray<T>, a component's arrays that grow by index
        template<typename A> size_t growSize(const void* array) { return static_cast<const A*>(array)->stored(); }
        template<typename A> void growResize(void* array, size_t size) { static_cast<A*>(array)->resize(size); }
        template<typename A> void* growAt(void* array, size_t index) { return &(*static_cast<A*>(array))[index]; }
        template<typename A, typename T> void growInsert(void* array, size_t index, const void* value) {
            static_cast<A*>(array)->insert(index, *static_cast<const T*>(value));
        }
        template<typename A> void growErase(void* array, size_t index) { static_cast<A*>(array)->erase(index); }
        template<typename A> void growMove(void* array, size_t from, size_t to) { static_cast<A*>(array)->move(from, to); }
        // equal when every index reads the same: an element one holds and the other does not
        // compares with the default element (storage that grew past the count is no difference)
        template<typename A, typename T> bool equalGrow(const void* a, const void* b, float epsilon);

    }

    namespace ops {

        template<typename A, typename T>
        inline constexpr ArrayOps GrowArrayOps{&detail::growSize<A>, &detail::growResize<A>, &detail::growAt<A>,
            &detail::growInsert<A, T>, &detail::growErase<A>, &detail::growMove<A>, true};
        template<typename A, typename T>
        inline constexpr TypeOps Grow{"GrowArray", Kind::Array, PropertyType::Custom, sizeof(A),
            &detail::construct<A>, &detail::destroy<A>, &detail::copy<A>,
            &detail::equalGrow<A, T>, &detail::arrayToYaml, nullptr, nullptr, nullptr, nullptr, false, nullptr, &detail::arraySchema};

    }

    // ---- deduction from the member type -------------------------------------------------------

    // ops(): the TypeOps of a T member; structDesc / elementOps / arrayOps for nested kinds
    template<typename T, typename = void>
    struct ReflectTraits;

    template<> struct ReflectTraits<bool> { static constexpr const TypeOps* ops = &ops::Bool; };
    template<> struct ReflectTraits<int> { static constexpr const TypeOps* ops = &ops::Int; };
    template<> struct ReflectTraits<unsigned int> { static constexpr const TypeOps* ops = &ops::UInt; };
    template<> struct ReflectTraits<float> { static constexpr const TypeOps* ops = &ops::Float; };
    template<> struct ReflectTraits<double> { static constexpr const TypeOps* ops = &ops::Double; };
    template<> struct ReflectTraits<jasperengine::Vector2> { static constexpr const TypeOps* ops = &ops::Vector2; };
    template<> struct ReflectTraits<jasperengine::Vector3> { static constexpr const TypeOps* ops = &ops::Vector3; };
    template<> struct ReflectTraits<jasperengine::Vector4> { static constexpr const TypeOps* ops = &ops::Vector4; };
    template<> struct ReflectTraits<Quaternion> { static constexpr const TypeOps* ops = &ops::Quat; };
    template<> struct ReflectTraits<jasperengine::Matrix4> { static constexpr const TypeOps* ops = &ops::Matrix4; };
    template<> struct ReflectTraits<jasperengine::Rect> { static constexpr const TypeOps* ops = &ops::Rect; };
    template<> struct ReflectTraits<std::string> { static constexpr const TypeOps* ops = &ops::String; };
    template<> struct ReflectTraits<jasperengine::Ease> { static constexpr const TypeOps* ops = &ops::Ease; };
    template<> struct ReflectTraits<jasperengine::Curve> { static constexpr const TypeOps* ops = &ops::Curve; };
    template<> struct ReflectTraits<jasperengine::Gradient> { static constexpr const TypeOps* ops = &ops::Gradient; };
    template<> struct ReflectTraits<jasperengine::EntityReference> { static constexpr const TypeOps* ops = &ops::EntityReference; };
    template<> struct ReflectTraits<jasperengine::Texture> { static constexpr const TypeOps* ops = &ops::Texture; };
    template<> struct ReflectTraits<FontArray> { static constexpr const TypeOps* ops = &ops::Font; };
    template<> struct ReflectTraits<jasperengine::AssetRef> { static constexpr const TypeOps* ops = &ops::AssetRef; };
    template<> struct ReflectTraits<jasperengine::ShaderRef> { static constexpr const TypeOps* ops = &ops::ShaderRef; };

    template<typename E>
    struct ReflectTraits<E, std::enable_if_t<std::is_enum_v<E>>> { static constexpr const TypeOps* ops = &ops::Enum<E>; };

    template<typename T>
    struct ReflectTraits<std::vector<T>, std::void_t<decltype(ReflectTraits<T>::ops)>> {
        using Element = T;
        static constexpr const TypeOps* ops = &ops::Array<T>;
        static constexpr const TypeOps* elementOps = ReflectTraits<T>::ops;
        static constexpr const ArrayOps* arrayOps = &ops::VectorArray<T>;
    };

    template<typename T, size_t N>
    struct ReflectTraits<T[N], std::void_t<decltype(ReflectTraits<T>::ops)>> {
        using Element = T;
        static constexpr const TypeOps* ops = &ops::Fixed<T, N>;
        static constexpr const TypeOps* elementOps = ReflectTraits<T>::ops;
        static constexpr const ArrayOps* arrayOps = &ops::FixedArray<T, N>;
    };

    template<typename T>
    struct ReflectTraits<GrowArray<T>, std::void_t<decltype(ReflectTraits<T>::ops)>> {
        using Element = T;
        static constexpr const TypeOps* ops = &ops::Grow<GrowArray<T>, T>;
        static constexpr const TypeOps* elementOps = ReflectTraits<T>::ops;
        static constexpr const ArrayOps* arrayOps = &ops::GrowArrayOps<GrowArray<T>, T>;
    };

    template<typename T>
    struct ReflectTraits<StableArray<T>, std::void_t<decltype(ReflectTraits<T>::ops)>> {
        using Element = T;
        static constexpr const TypeOps* ops = &ops::Grow<StableArray<T>, T>;
        static constexpr const TypeOps* elementOps = ReflectTraits<T>::ops;
        static constexpr const ArrayOps* arrayOps = &ops::GrowArrayOps<StableArray<T>, T>;
    };

    // A nested struct's traits (its StructDesc), declared once next to its table:
    // JE_REFLECT_STRUCT_TRAITS(Submesh, kSubmeshStruct)
#define JE_REFLECT_STRUCT_TRAITS(Type, desc) \
    extern const ::jasperengine::editor::reflect::StructDesc desc; \
    template<> struct ReflectTraits<Type> { \
        static constexpr const TypeOps* ops = &::jasperengine::editor::reflect::ops::Struct<Type>; \
        static constexpr const StructDesc* structDesc = &desc; \
    }

    template<typename T, typename = void>
    struct HasStructDesc : std::false_type {};
    template<typename T>
    struct HasStructDesc<T, std::void_t<decltype(ReflectTraits<T>::structDesc)>> : std::true_type {};

    namespace detail {

        template<typename S> bool equalStructT(const void* a, const void* b, float epsilon) {
            return structEqual(*ReflectTraits<S>::structDesc, a, b, epsilon);
        }

        template<typename T, size_t N> bool equalFixed(const void* a, const void* b, float epsilon) {
            const T* x = static_cast<const T*>(a);
            const T* y = static_cast<const T*>(b);
            for (size_t i = 0; i < N; i++) {
                if constexpr (HasStructDesc<T>::value) {
                    if (!structEqual(*ReflectTraits<T>::structDesc, &x[i], &y[i], epsilon)) return false;
                } else {
                    if (!ReflectTraits<T>::ops->equals(&x[i], &y[i], epsilon)) return false;
                }
            }
            return true;
        }

        template<typename A, typename T> bool equalGrow(const void* a, const void* b, float epsilon) {
            const A& x = *static_cast<const A*>(a);
            const A& y = *static_cast<const A*>(b);
            const size_t count = x.stored() > y.stored() ? x.stored() : y.stored();
            const TypeOps* element = ReflectTraits<T>::ops;
            for (size_t i = 0; i < count; i++) {
                // (const reads: past an array's end, its default element)
                if constexpr (HasStructDesc<T>::value) {
                    if (!structEqual(*ReflectTraits<T>::structDesc, &x[i], &y[i], epsilon)) return false;
                } else {
                    if (!element->equals(&x[i], &y[i], epsilon)) return false;
                }
            }
            return true;
        }

        template<typename T> bool equalVector(const void* a, const void* b, float epsilon) {
            const auto& x = *static_cast<const std::vector<T>*>(a);
            const auto& y = *static_cast<const std::vector<T>*>(b);
            if (x.size() != y.size()) return false;
            const TypeOps* element = ReflectTraits<T>::ops;
            for (size_t i = 0; i < x.size(); i++) {
                if constexpr (HasStructDesc<T>::value) {
                    if (!structEqual(*ReflectTraits<T>::structDesc, &x[i], &y[i], epsilon)) return false;
                } else {
                    if (!element->equals(&x[i], &y[i], epsilon)) return false;
                }
            }
            return true;
        }

    }

}
