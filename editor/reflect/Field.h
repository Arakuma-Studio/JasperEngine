// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Authoring the tables (docs/specs/S08-reflection.md D1-D3): fields are member pointers with
// metadata in designated initializers, in the order the YAML writes them.
//
//   static constexpr FieldDesc kFogFields[] = {
//       JE_FIELD(FogComponent, type, .enumDesc = &enums::FogType),
//       JE_FIELD(FogComponent, color, .flags = ColorLinear),
//       JE_FIELD(FogComponent, density, .step = 0.001f, .format = "%.4f"),
//       JE_PROXY(LightComponent, "shadowCameraNear", shadowCameraNearFar.x, .updateFlags = ...),
//   };
//
// The kind, TypeOps, array ops and nested StructDesc come from the member type (ReflectTraits,
// TypeOps.h). FieldMeta's members are in the order designated initializers must follow.

#include "reflect/TypeOps.h"

#include <iterator>
#include <type_traits>
#include <utility>

namespace jasperengine::editor::reflect {

    // A table pointer a field may give (`.ops = &ops::Entity`): whether it was given is a flag, not a
    // null test, since GCC's UBSan builds cannot test the address of an inline constexpr object
    // (every TypeOps) against null in a constant expression
    template<typename T>
    struct Given {
        const T* ptr = nullptr;
        bool set = false;
        constexpr Given() = default;
        constexpr Given(const T* value) : ptr(value), set(true) {}
    };

    struct FieldMeta {
        const char* yamlKey = nullptr;
        Given<TypeOps> ops;                    // instead of the member type's (an encoding quirk, Entity, LayerMask)
        const EnumDesc* enumDesc = nullptr;
        Given<StructDesc> structDesc;          // the members of a struct its type has no traits for
        uint32_t flags = 0;
        uint64_t updateFlags = 0;
        AssetKind asset = AssetKind::None;
        Unit unit = Unit::None;
        float min = -std::numeric_limits<float>::infinity();
        float max = std::numeric_limits<float>::infinity();
        float uiMin = -std::numeric_limits<float>::infinity();
        float uiMax = std::numeric_limits<float>::infinity();
        float step = 0.0f;
        const char* format = nullptr;
        const char* label = nullptr;
        const char* tooltip = nullptr;
        const char* group = nullptr;
        const char* drawer = nullptr;
        bool (*visibleIf)(const void* owner) = nullptr;
        bool (*enabledIf)(const void* owner) = nullptr;
        std::span<const char* const> aliases = {};
        int16_t uiOrder = 0;
        uint64_t enumMask = 0;
        const char* cppPath = nullptr;
        size_t (*count)(const void* owner) = nullptr;
        Given<TypeOps> elementOps;             // instead of the element type's (Entity elements)
        const void* (*peek)(const void* owner) = nullptr;
        AssetForm assetForm = AssetForm::Deduced;
        void (*assetMoved)(void* owner) = nullptr;
        size_t (*assetCount)(const void* owner) = nullptr;
        bool (*derivedIf)(const void* owner) = nullptr;
    };

    namespace detail {

        template<typename T, auto Member>
        void* memberRef(void* owner) {
            return &(static_cast<T*>(owner)->*Member);
        }

        template<typename M>
        constexpr FieldDesc makeField(const char* name, void* (*ref)(void*), const FieldMeta& meta) {
            using Traits = ReflectTraits<M>;
            FieldDesc desc{name, meta.yamlKey ? meta.yamlKey : name, Kind::Custom, nullptr, ref};
            // a type without traits needs FieldMeta::ops (a null one fails here, at compile time)
            if constexpr (requires { Traits::ops; }) {
                desc.ops = meta.ops.set ? meta.ops.ptr : Traits::ops;
                if constexpr (requires { Traits::structDesc; }) {
                    desc.structDesc = Traits::structDesc;
                }
                if constexpr (requires { Traits::arrayOps; }) {
                    desc.arrayOps = Traits::arrayOps;
                    desc.elementOps = Traits::elementOps;
                    using Element = typename Traits::Element;
                    if constexpr (requires { ReflectTraits<Element>::structDesc; }) {
                        desc.structDesc = ReflectTraits<Element>::structDesc;
                    }
                }
            } else {
                desc.ops = meta.ops.ptr;
            }
            desc.kind = desc.ops->kind;
            if (meta.structDesc.set) desc.structDesc = meta.structDesc.ptr;
            desc.enumDesc = meta.enumDesc;
            desc.flags = meta.flags;
            desc.updateFlags = meta.updateFlags;
            desc.asset = meta.asset;
            desc.unit = meta.unit;
            desc.min = meta.min;
            desc.max = meta.max;
            desc.uiMin = meta.uiMin;
            desc.uiMax = meta.uiMax;
            desc.step = meta.step;
            desc.format = meta.format;
            desc.label = meta.label;
            desc.tooltip = meta.tooltip;
            desc.group = meta.group;
            desc.drawer = meta.drawer;
            desc.visibleIf = meta.visibleIf;
            desc.derivedIf = meta.derivedIf;
            desc.enabledIf = meta.enabledIf;
            desc.aliases = meta.aliases;
            desc.uiOrder = meta.uiOrder;
            desc.enumMask = meta.enumMask;
            desc.cppPath = meta.cppPath;
            desc.count = meta.count;
            if (meta.elementOps.set) desc.elementOps = meta.elementOps.ptr;
            desc.peek = meta.peek;
            desc.assetForm = meta.assetForm;
            desc.assetMoved = meta.assetMoved;
            desc.assetCount = meta.assetCount;
            return desc;
        }

    }

    template<typename T, auto Member>
    constexpr FieldDesc field(const char* name, const FieldMeta& meta) {
        using M = std::remove_cvref_t<decltype(std::declval<T&>().*Member)>;
        return detail::makeField<M>(name, &detail::memberRef<T, Member>, meta);
    }

    // A field whose property name is not its member's name
    template<typename T, auto Member>
    constexpr FieldDesc namedField(const char* name, const char* cppPath, FieldMeta meta) {
        meta.cppPath = cppPath;
        return field<T, Member>(name, meta);
    }

    // A view onto part of another field (a lane of a vector): edited, overridden and exported under
    // its own name, never saved
    template<typename M>
    constexpr FieldDesc proxyField(const char* name, const char* cppPath, void* (*ref)(void*), FieldMeta meta) {
        meta.flags |= Proxy | NoSave;
        meta.cppPath = cppPath;
        return detail::makeField<M>(name, ref, meta);
    }

    // a default-constructed instance (not const: a material's extra pass is made on access)
    template<typename T>
    const void* defaultInstance() {
        static T value{};
        return &value;
    }

}

#define JE_FIELD(T, member, ...) \
    ::jasperengine::editor::reflect::field<T, &T::member>(#member, ::jasperengine::editor::reflect::FieldMeta{__VA_ARGS__})

// a field whose property name is not its member's name ("left" for leftClip)
#define JE_FIELD_NAMED(T, member, name, ...) \
    ::jasperengine::editor::reflect::namedField<T, &T::member>(name, #member, ::jasperengine::editor::reflect::FieldMeta{__VA_ARGS__})

#define JE_PROXY(T, name, expr, ...) \
    ::jasperengine::editor::reflect::proxyField<std::remove_cvref_t<decltype(std::declval<T&>().expr)>>( \
        name, #expr, [](void* owner) -> void* { return &(static_cast<T*>(owner)->expr); }, \
        ::jasperengine::editor::reflect::FieldMeta{__VA_ARGS__})

// FieldMeta::count of a fixed array its owner counts in a member ("numFramesRect")
#define JE_COUNT(T, member) \
    [](const void* owner) -> size_t { return static_cast<size_t>(static_cast<const T*>(owner)->member); }

// The StructDesc of a struct's table
#define JE_STRUCT_DESC(T, name, fields) \
    ::jasperengine::editor::reflect::StructDesc{name, std::span<const ::jasperengine::editor::reflect::FieldDesc>(fields), \
        &::jasperengine::editor::reflect::defaultInstance<T>, &::jasperengine::editor::reflect::ops::Value<T>, #T}
