// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Codec<T>: how a value of type T is written to and read from a cooked stream
// (docs/specs/S30-shipping.md D2). One template per type, used by the editor's cook and the
// player's loader alike. The generated field tables (RuntimeFields.gen.inc) add a Codec for each
// struct the reflection tables describe; the component codecs (codecs/) the rest.

#include "serialization/CookStream.h"

#include "action/Ease.h"
#include "asset/AssetRef.h"
#include "asset/Guid.h"
#include "math/Matrix4.h"
#include "math/Quaternion.h"
#include "math/Rect.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "script/ScriptProperty.h"
#include "texture/Texture.h"
#include "util/GrowArray.h"
#include "util/StableArray.h"

#include <array>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace jasperengine::cook {

    template<typename T, typename = void>
    struct Codec;

    template<typename T>
    void write(Writer& w, const T& value) { Codec<T>::write(w, value); }
    template<typename T>
    void read(Reader& r, T& value) { Codec<T>::read(r, value); }

    // ---------------------------------------------------------------- scalars

    template<>
    struct Codec<bool> {
        static void write(Writer& w, bool v) { w.boolean(v); }
        static void read(Reader& r, bool& v) { v = r.boolean(); }
    };

    template<typename T>
    struct Codec<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
        static void write(Writer& w, T v) {
            if constexpr (std::is_signed_v<T>) w.svarint(static_cast<int64_t>(v));
            else w.varint(static_cast<uint64_t>(v));
        }
        static void read(Reader& r, T& v) {
            if constexpr (std::is_signed_v<T>) v = static_cast<T>(r.svarint());
            else v = static_cast<T>(r.varint());
        }
    };

    template<typename T>
    struct Codec<T, std::enable_if_t<std::is_enum_v<T>>> {
        using U = std::underlying_type_t<T>;
        static void write(Writer& w, T v) { Codec<U>::write(w, static_cast<U>(v)); }
        static void read(Reader& r, T& v) {
            U raw{};
            Codec<U>::read(r, raw);
            v = static_cast<T>(raw);
        }
    };

    template<>
    struct Codec<float> {
        static void write(Writer& w, float v) { w.f32(v); }
        static void read(Reader& r, float& v) { v = r.f32(); }
    };

    template<>
    struct Codec<double> {
        static void write(Writer& w, double v) { w.f64(v); }
        static void read(Reader& r, double& v) { v = r.f64(); }
    };

    template<>
    struct Codec<std::string> {
        static void write(Writer& w, const std::string& v) { w.string(v); }
        static void read(Reader& r, std::string& v) { v = r.string(); }
    };

    // ---------------------------------------------------------------- math

    template<>
    struct Codec<Vector2> {
        static void write(Writer& w, const Vector2& v) { w.f32(v.x); w.f32(v.y); }
        static void read(Reader& r, Vector2& v) { v.x = r.f32(); v.y = r.f32(); }
    };

    template<>
    struct Codec<Vector3> {
        static void write(Writer& w, const Vector3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
        static void read(Reader& r, Vector3& v) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); }
    };

    template<>
    struct Codec<Vector4> {
        static void write(Writer& w, const Vector4& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); w.f32(v.w); }
        static void read(Reader& r, Vector4& v) { v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); v.w = r.f32(); }
    };

    template<>
    struct Codec<Quaternion> {
        static void write(Writer& w, const Quaternion& q) { w.f32(q.w); w.f32(q.x); w.f32(q.y); w.f32(q.z); }
        static void read(Reader& r, Quaternion& q) { q.w = r.f32(); q.x = r.f32(); q.y = r.f32(); q.z = r.f32(); }
    };

    template<>
    struct Codec<Matrix4> {
        static void write(Writer& w, const Matrix4& m) {
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++) w.f32(m.get(c, row));
        }
        static void read(Reader& r, Matrix4& m) {
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++) m.set(c, row, r.f32());
        }
    };

    template<>
    struct Codec<Rect> {
        static void write(Writer& w, const Rect& v) { w.f32(v.getX()); w.f32(v.getY()); w.f32(v.getWidth()); w.f32(v.getHeight()); }
        static void read(Reader& r, Rect& v) {
            const float x = r.f32(), y = r.f32(), width = r.f32(), height = r.f32();
            v.setRect(x, y, width, height);
        }
    };

    // ---------------------------------------------------------------- engine values

    template<>
    struct Codec<Guid> {
        static void write(Writer& w, const Guid& g) {
            uint8_t bytes[16];
            g.toBytes(bytes);
            w.blob(bytes, 16);
        }
        static void read(Reader& r, Guid& g) {
            const std::span<const uint8_t> bytes = r.blob();
            if (bytes.size() == 16) g = Guid::fromBytes(bytes.data());
            else if (!bytes.empty()) r.fail("a GUID of " + std::to_string(bytes.size()) + " bytes");
            else g = Guid();
        }
    };

    template<>
    struct Codec<AssetRef> {
        static void write(Writer& w, const AssetRef& v) { w.string(v.path); Codec<Guid>::write(w, v.guid); }
        static void read(Reader& r, AssetRef& v) {
            std::string path = r.string();
            Guid guid;
            Codec<Guid>::read(r, guid);
            v.set(std::move(path), guid);
        }
    };

    template<>
    struct Codec<ShaderRef> {
        static void write(Writer& w, const ShaderRef& v) {
            w.string(v.path);
            Codec<Guid>::write(w, v.vertGuid);
            Codec<Guid>::write(w, v.fragGuid);
        }
        static void read(Reader& r, ShaderRef& v) {
            std::string path = r.string();
            Guid vert, frag;
            Codec<Guid>::read(r, vert);
            Codec<Guid>::read(r, frag);
            v = ShaderRef(std::move(path), vert, frag);
        }
    };

    template<>
    struct Codec<Ease> {
        static void write(Writer& w, const Ease& e) { Codec<EaseType>::write(w, e.getType()); }
        static void read(Reader& r, Ease& e) {
            EaseType type = EaseType::LINEAR;
            Codec<EaseType>::read(r, type);
            // a Lua function cannot be cooked: a CUSTOM ease is linear until a script sets it
            e = (type == EaseType::CUSTOM) ? Ease() : Ease(type);
        }
    };

    template<>
    struct Codec<EntityReference> {
        static void write(Writer& w, const EntityReference& v) {
            w.varint(v.sceneId);
            // another scene's GameObject keeps its id (scenes keep their editor ids)
            if (v.sceneId != 0) w.varint(v.entity);
            else w.entity(v.entity);
        }
        static void read(Reader& r, EntityReference& v) {
            v.sceneId = static_cast<uint32_t>(r.varint());
            v.entity = (v.sceneId != 0) ? static_cast<Entity>(r.varint()) : r.entity();
        }
    };

    // A texture as the exported code sets one up (Factory::formatTexture): its source paths
    // (one, or a cube's six) with their GUIDs, or only the id of one made in memory, then the
    // sampler state, in the setters' order (setPath resets the SVG scale)
    template<>
    struct Codec<Texture> {
        static void write(Writer& w, const Texture& texture);
        static void read(Reader& r, Texture& texture);
    };

    // ---------------------------------------------------------------- containers

    template<typename A, typename B>
    struct Codec<std::pair<A, B>> {
        static void write(Writer& w, const std::pair<A, B>& v) { Codec<A>::write(w, v.first); Codec<B>::write(w, v.second); }
        static void read(Reader& r, std::pair<A, B>& v) { Codec<A>::read(r, v.first); Codec<B>::read(r, v.second); }
    };

    template<typename T>
    struct Codec<std::vector<T>> {
        static void write(Writer& w, const std::vector<T>& v) {
            w.varint(v.size());
            for (const T& element : v) Codec<T>::write(w, element);
        }
        static void read(Reader& r, std::vector<T>& v) {
            const uint64_t count = r.varint();
            if (count > r.remaining()) {
                r.fail("an array of " + std::to_string(count) + " elements in " + std::to_string(r.remaining()) + " bytes");
                return;
            }
            v.clear();
            v.resize(static_cast<size_t>(count));
            for (T& element : v) Codec<T>::read(r, element);
        }
    };

    template<typename K, typename V, typename C>
    struct Codec<std::map<K, V, C>> {
        static void write(Writer& w, const std::map<K, V, C>& v) {
            w.varint(v.size());
            for (const auto& [key, value] : v) {
                Codec<K>::write(w, key);
                Codec<V>::write(w, value);
            }
        }
        static void read(Reader& r, std::map<K, V, C>& v) {
            const uint64_t count = r.varint();
            v.clear();
            for (uint64_t i = 0; i < count && !r.failed(); i++) {
                K key{};
                Codec<K>::read(r, key);
                Codec<V>::read(r, v[key]);
            }
        }
    };

    // a fixed array: every element, so a count field elsewhere stays the only count; a shorter
    // player array reads what fits and drops the rest (capacities are checked before a build)
    template<typename T, size_t N>
    struct Codec<T[N]> {
        static void write(Writer& w, const T (&v)[N]) {
            w.varint(N);
            for (size_t i = 0; i < N; i++) Codec<T>::write(w, v[i]);
        }
        static void read(Reader& r, T (&v)[N]) {
            const uint64_t count = r.varint();
            for (uint64_t i = 0; i < count && !r.failed(); i++) {
                if (i < N) {
                    Codec<T>::read(r, v[i]);
                } else {
                    T dropped{};
                    Codec<T>::read(r, dropped);
                }
            }
        }
    };

    template<typename T, size_t N>
    struct Codec<std::array<T, N>> {
        static void write(Writer& w, const std::array<T, N>& v) {
            w.varint(N);
            for (const T& element : v) Codec<T>::write(w, element);
        }
        static void read(Reader& r, std::array<T, N>& v) {
            const uint64_t count = r.varint();
            for (uint64_t i = 0; i < count && !r.failed(); i++) {
                if (i < N) {
                    Codec<T>::read(r, v[i]);
                } else {
                    T dropped{};
                    Codec<T>::read(r, dropped);
                }
            }
        }
    };

    // a component's growing arrays: what they hold, read back up to ARRAY_INDEX_LIMIT elements (the
    // rest of a damaged file's count is read and dropped)
    template<typename Array, typename T>
    struct GrowingArrayCodec {
        static void write(Writer& w, const Array& v) {
            w.varint(v.stored());
            for (size_t i = 0; i < v.stored(); i++) Codec<T>::write(w, v[i]);
        }
        static void read(Reader& r, Array& v) {
            const uint64_t count = r.varint();
            for (uint64_t i = 0; i < count && !r.failed(); i++) {
                if (i < ARRAY_INDEX_LIMIT) {
                    Codec<T>::read(r, v[static_cast<size_t>(i)]);
                } else {
                    T dropped{};
                    Codec<T>::read(r, dropped);
                }
            }
        }
    };
    template<typename T>
    struct Codec<GrowArray<T>> : GrowingArrayCodec<GrowArray<T>, T> {};
    template<typename T>
    struct Codec<StableArray<T>> : GrowingArrayCodec<StableArray<T>, T> {};

    // ---------------------------------------------------------------- fields

    // every Texture below a value (the loader binds render-to-texture and fluid links with it)
    using TextureVisitor = std::function<void(Texture& texture)>;

    template<typename T, typename = void>
    struct TextureVisit {
        static void visit(T&, const TextureVisitor&) {}
    };
    template<>
    struct TextureVisit<Texture> {
        static void visit(Texture& texture, const TextureVisitor& f) { f(texture); }
    };
    template<typename T>
    struct TextureVisit<std::vector<T>> {
        static void visit(std::vector<T>& v, const TextureVisitor& f) { for (T& e : v) TextureVisit<T>::visit(e, f); }
    };
    template<typename T, size_t N>
    struct TextureVisit<T[N]> {
        static void visit(T (&v)[N], const TextureVisitor& f) { for (size_t i = 0; i < N; i++) TextureVisit<T>::visit(v[i], f); }
    };
    template<typename T, size_t N>
    struct TextureVisit<std::array<T, N>> {
        static void visit(std::array<T, N>& v, const TextureVisitor& f) { for (T& e : v) TextureVisit<T>::visit(e, f); }
    };
    template<typename T>
    struct TextureVisit<GrowArray<T>> {
        static void visit(GrowArray<T>& v, const TextureVisitor& f) { for (size_t i = 0; i < v.stored(); i++) TextureVisit<T>::visit(v[i], f); }
    };
    template<typename T>
    struct TextureVisit<StableArray<T>> {
        static void visit(StableArray<T>& v, const TextureVisitor& f) { for (size_t i = 0; i < v.stored(); i++) TextureVisit<T>::visit(v[i], f); }
    };
    template<typename A, typename B>
    struct TextureVisit<std::pair<A, B>> {
        static void visit(std::pair<A, B>& v, const TextureVisitor& f) { TextureVisit<A>::visit(v.first, f); TextureVisit<B>::visit(v.second, f); }
    };

    // One saved field of a struct the reflection tables describe (RuntimeFields.gen.inc): its id
    // (FNV-1a 32 of "<struct>.<yamlKey>"), kind, and the thunks that write and read its member
    // and visit the textures below it (null: none can be)
    struct FieldEntry {
        uint32_t id;
        FieldKind kind;
        const char* name;
        void (*write)(Writer& w, const void* owner);
        void (*read)(Reader& r, void* owner);
        void (*textures)(void* owner, const TextureVisitor& visit);
    };

    struct StructTable {
        const char* name;
        const FieldEntry* fields;
        size_t count;
    };

    // a member through its codec
    template<typename Owner, auto Member>
    void writeMember(Writer& w, const void* owner) {
        write(w, static_cast<const Owner*>(owner)->*Member);
    }
    template<typename Owner, auto Member>
    void readMember(Reader& r, void* owner) {
        read(r, static_cast<Owner*>(owner)->*Member);
    }

    template<typename Owner, auto Member>
    void visitMemberTextures(void* owner, const TextureVisitor& f) {
        auto& member = static_cast<Owner*>(owner)->*Member;
        TextureVisit<std::remove_reference_t<decltype(member)>>::visit(member, f);
    }

    // a value behind an accessor (an S08 field with a cppPath: spline.editableKnots()), the
    // accessor a function of the generated table: Access(Owner&) -> the value's reference
    template<typename Owner, auto Access>
    void writePath(Writer& w, const void* owner) {
        write(w, Access(*const_cast<Owner*>(static_cast<const Owner*>(owner))));
    }
    template<typename Owner, auto Access>
    void readPath(Reader& r, void* owner) {
        read(r, Access(*static_cast<Owner*>(owner)));
    }
    template<typename Owner, auto Access>
    void visitPathTextures(void* owner, const TextureVisitor& f) {
        auto& value = Access(*static_cast<Owner*>(owner));
        TextureVisit<std::remove_reference_t<decltype(value)>>::visit(value, f);
    }

    // an Entity member (an id, not a number: remapped through the stream's entity mapping)
    template<typename Owner, auto Member>
    void writeEntityMember(Writer& w, const void* owner) {
        w.entity(static_cast<const Owner*>(owner)->*Member);
    }
    template<typename Owner, auto Member>
    void readEntityMember(Reader& r, void* owner) {
        static_cast<Owner*>(owner)->*Member = r.entity();
    }

    // an array of Entity (std::vector<Entity>, Entity[N])
    template<typename Owner, auto Member>
    void writeEntityArrayMember(Writer& w, const void* owner) {
        const auto& array = static_cast<const Owner*>(owner)->*Member;
        w.varint(std::size(array));
        for (const Entity entity : array) w.entity(entity);
    }
    template<typename Owner, auto Member>
    void readEntityArrayMember(Reader& r, void* owner) {
        auto& array = static_cast<Owner*>(owner)->*Member;
        const uint64_t count = r.varint();
        using Array = std::remove_reference_t<decltype(array)>;
        if constexpr (requires(Array a) { a.resize(size_t()); }) {
            array.resize(static_cast<size_t>(count));
            for (auto& entity : array) entity = r.entity();
        } else {
            for (uint64_t i = 0; i < count; i++) {
                const Entity entity = r.entity();
                if (i < std::size(array)) array[static_cast<size_t>(i)] = entity;
            }
        }
    }

    // A struct's saved fields: {u32 id, u8 kind, u32 length, payload} each, then a 0 id. A
    // reader skips a field it does not know (one warning per id) or of another kind.
    JASPERENGINE_API void writeStruct(Writer& w, const StructTable& table, const void* owner);
    JASPERENGINE_API void readStruct(Reader& r, const StructTable& table, void* owner);
    JASPERENGINE_API void visitStructTextures(const StructTable& table, void* owner, const TextureVisitor& visit);

}
