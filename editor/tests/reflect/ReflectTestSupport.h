// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Random component values for the reflection tests (docs/specs/S08-reflection.md AC3, AC4): every
// field of a table set to a random value of its type (NaN, infinities and -0 included unless
// finiteOnly).
//
// The values are data: editor/tests/reflect/parity_golden holds what they encode to. So they are
// the same on every compiler (std::mt19937's output is standard, the std:: distributions' results
// are not), and a field's values come from its name, not from the fields before it: a field added
// to a table adds its key to that data and changes no other line.

#include "reflect/Reflect.h"
#include "reflect/TypeOps.h"

#include "component/CameraComponent.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <set>
#include <type_traits>
#include <string>
#include <vector>

namespace jasperengine::editor::test {

    class RandomValues {
    public:
        explicit RandomValues(uint32_t seed, bool finiteOnly = false): rng(seed), base(seed), finite(finiteOnly) {}

        // no GameObject references (a fixture scene whose GameObjects name nothing that is not there)
        void withoutReferences() { noReferences = true; }

        int integer(int low, int high) {
            const uint64_t range = static_cast<uint64_t>(static_cast<int64_t>(high) - low + 1);
            return static_cast<int>(low + static_cast<int64_t>(rng() % range));
        }
        bool chance(int percent) { return integer(0, 99) < percent; }

        float number() {
            if (!finite) {
                switch (integer(0, 19)) {
                    case 0: return std::numeric_limits<float>::quiet_NaN();
                    case 1: return std::numeric_limits<float>::infinity();
                    case 2: return -std::numeric_limits<float>::infinity();
                    case 3: return -0.0f;
                    case 4: return std::numeric_limits<float>::denorm_min();
                    case 5: return 3.0e38f;
                    default: break;
                }
            }
            switch (integer(0, 3)) {
                case 0: return static_cast<float>(integer(-100, 100));
                case 1: return static_cast<float>(integer(-1000, 1000)) / 7.0f;
                case 2: return static_cast<float>(unit() * 2.0 - 1.0);
                default: return static_cast<float>(unit() * 10000.0);
            }
        }

        double number64() {
            if (!finite && chance(10)) return std::numeric_limits<double>::quiet_NaN();
            return static_cast<double>(number()) + (chance(50) ? 1.0 / 3.0 : 0.0);
        }

        std::string text() {
            static const char* const samples[] = {"", "a", "Hello, YAML: [1, 2]", "with \"quotes\"", "true", "123", "~", "- dash",
                "#hash", "multi\nline", "\xC3\xBCmlaut", "  padded  ", "textures/a.png", "a/b/c.html"};
            return samples[integer(0, static_cast<int>(std::size(samples)) - 1)];
        }

        std::string path() {
            static const char* const samples[] = {"", "textures/a.png", "sounds/hit.wav", "ui/menu.html", "fonts/Main.ttf", "a b/c.png"};
            return samples[integer(0, static_cast<int>(std::size(samples)) - 1)];
        }

        Guid guid() {
            if (chance(50)) return Guid();
            char text[37];
            static const char* hex = "0123456789abcdef";
            for (int i = 0; i < 36; i++) text[i] = (i == 8 || i == 13 || i == 18 || i == 23) ? '-' : hex[integer(0, 15)];
            text[36] = 0;
            return Guid::parse(text);
        }

        // a random value of the field's type at `value`
        void fill(const reflect::FieldDesc& field, const reflect::TypeOps* ops, void* value);
        // every saved and property field of a struct
        void fillStruct(const reflect::StructDesc& desc, void* owner);

    private:
        double unit() { return static_cast<double>(rng()) / 4294967296.0; }

        static uint32_t mixed(uint32_t scope, uint32_t value) {
            return (scope ^ value) * 16777619u + 2166136261u;
        }
        static uint32_t mixed(uint32_t scope, const char* name) {
            for (; *name; name++) scope = mixed(scope, static_cast<uint32_t>(static_cast<unsigned char>(*name)));
            return scope;
        }

        void fillFields(const reflect::StructDesc& desc, void* owner);

        std::mt19937 rng;
        uint32_t base;           // what the next fillStruct starts from
        uint32_t scope = 0;      // the struct being filled
        bool finite;
        bool noReferences = false;
    };

    inline void RandomValues::fill(const reflect::FieldDesc& field, const reflect::TypeOps* ops, void* value) {
        using reflect::Kind;
        switch (ops->kind) {
            case Kind::Bool: *static_cast<bool*>(value) = chance(50); return;
            case Kind::Int: *static_cast<int*>(value) = chance(10) ? integer(-100000, 100000) : integer(-20, 20); return;
            case Kind::UInt:
                *static_cast<unsigned int*>(value) = chance(5) ? 0xFFFFFFFFu : static_cast<unsigned int>(integer(0, 5000));
                return;
            case Kind::LayerMask:
                *static_cast<uint32_t*>(value) = chance(30) ? 0xFFFFFFFFu : static_cast<uint32_t>(integer(0, 0x7FFFFFFF));
                return;
            case Kind::Entity:
                *static_cast<Entity*>(value) = (noReferences || chance(30)) ? NULL_ENTITY : static_cast<Entity>(integer(1, 400));
                return;
            case Kind::Float: *static_cast<float*>(value) = number(); return;
            case Kind::Double: *static_cast<double*>(value) = number64(); return;
            case Kind::Vector2: *static_cast<Vector2*>(value) = Vector2(number(), number()); return;
            case Kind::Vector3: *static_cast<Vector3*>(value) = Vector3(number(), number(), number()); return;
            case Kind::Vector4: *static_cast<Vector4*>(value) = Vector4(number(), number(), number(), number()); return;
            case Kind::Quat: *static_cast<Quaternion*>(value) = Quaternion(number(), number(), number(), number()); return;
            case Kind::Rect: *static_cast<Rect*>(value) = Rect(number(), number(), number(), number()); return;
            case Kind::Matrix4: {
                Matrix4& matrix = *static_cast<Matrix4*>(value);
                for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) matrix[i][j] = number();
                return;
            }
            case Kind::String:
                if (ops->size == sizeof(char) && ops->propertyType == PropertyType::Custom) {
                    *static_cast<char*>(value) = static_cast<char>(integer(33, 126));
                } else {
                    *static_cast<std::string*>(value) = text();
                }
                return;
            case Kind::Enum: {
                const reflect::EnumDesc& desc = *field.enumDesc;
                reflect::writeEnum(field, value, desc.items[static_cast<size_t>(integer(0, static_cast<int>(desc.items.size()) - 1))].value);
                return;
            }
            case Kind::Ease:
                *static_cast<Ease*>(value) = Ease(static_cast<EaseType>(integer(0, static_cast<int>(EaseType::STEP))));
                return;
            case Kind::Texture: {
                Texture& texture = *static_cast<Texture*>(value);
                texture = Texture();
                if (chance(60)) {
                    const std::string file = path();
                    texture.setPath(file.empty() ? std::string("textures/b.png") : file);
                    texture.setPathGuid(0, guid());
                    texture.setMinFilter(static_cast<TextureFilter>(integer(0, 5)));
                    texture.setMagFilter(static_cast<TextureFilter>(integer(0, 1)));
                    texture.setWrapU(static_cast<TextureWrap>(integer(0, 3)));
                    texture.setWrapV(static_cast<TextureWrap>(integer(0, 3)));
                }
                return;
            }
            case Kind::AssetPath:
                if (ops->propertyType == PropertyType::Shader) {
                    ShaderRef& ref = *static_cast<ShaderRef*>(value);
                    ref = ShaderRef(path(), guid(), guid());
                } else {
                    AssetRef& ref = *static_cast<AssetRef*>(value);
                    ref.set(path(), guid());
                }
                return;
            case Kind::Font: {
                FontArray& fonts = *static_cast<FontArray*>(value);
                fonts = FontArray();
                const int count = integer(0, static_cast<int>(fonts.size()));
                for (int i = 0; i < count; i++) fonts[static_cast<size_t>(i)].set(chance(80) ? "fonts/F" + std::to_string(i) + ".ttf" : std::string(), guid());
                return;
            }
            case Kind::Struct:
                if (field.structDesc) fillFields(*field.structDesc, value);
                return;
            case Kind::Array: {
                if (!field.arrayOps || (!field.elementOps && !field.structDesc)) return;
                const size_t size = static_cast<size_t>(integer(0, 4));
                field.arrayOps->resize(value, size);
                const size_t actual = field.arrayOps->size(value);
                const uint32_t arrayScope = scope;
                for (size_t i = 0; i < actual; i++) {
                    void* element = field.arrayOps->at(value, i);
                    if (field.structDesc) {
                        scope = mixed(arrayScope, static_cast<uint32_t>(i));
                        fillFields(*field.structDesc, element);
                    } else {
                        fill(field, field.elementOps, element);
                    }
                }
                scope = arrayScope;
                return;
            }
            default:
                return;
        }
    }

    inline void RandomValues::fillStruct(const reflect::StructDesc& desc, void* owner) {
        scope = base = mixed(base, 1u);
        fillFields(desc, owner);
        // what the caller draws next does not depend on the fields either
        rng.seed(mixed(base, 2u));
    }

    inline void RandomValues::fillFields(const reflect::StructDesc& desc, void* owner) {
        const uint32_t structScope = scope;
        for (const reflect::FieldDesc& field : desc.fields) {
            if (field.flags & reflect::Proxy) continue;
            if (!reflect::isSaved(field)) continue;
            scope = mixed(structScope, field.name);
            rng.seed(scope);
            fill(field, field.ops, field.ref(owner));
        }
        scope = structScope;
    }

    // The framebuffers the camera values a test made own (CameraComponent allocates one, which only
    // RenderSystem frees): freed once each
    class OwnedFramebuffers {
    public:
        template<typename T>
        void keep(const T* comp) {
            if constexpr (std::is_same_v<T, CameraComponent>) {
                if (comp->framebuffer) framebuffers.insert(comp->framebuffer);
            }
        }
        ~OwnedFramebuffers() {
            for (Framebuffer* framebuffer : framebuffers) delete framebuffer;
        }
    private:
        std::set<Framebuffer*> framebuffers;
    };

}
