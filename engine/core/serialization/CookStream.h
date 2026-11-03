// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The byte streams of cooked scenes and prefabs (.jscn / .jpfb, docs/specs/S30-shipping.md D2):
// little endian, varints for integers, strings as indices of the file's string table, entity
// references through the file's entity mapping. The editor's cook writes with Writer, the
// player's CookedSceneLoader reads with Reader; both go through the same Codec<T> templates
// (CookCodecs.h), so what one writes the other reads by construction.

#include "Export.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace jasperengine::cook {

    // field kinds of the records, the editor's reflect::Kind values (append-only there too)
    enum class FieldKind : uint8_t {
        Bool, Int, UInt, Float, Double, Vector2, Vector3, Vector4, Quat, Matrix4, Rect,
        String, Enum, LayerMask, Ease, Entity, EntityReference, Texture, Font, Material, AssetPath,
        Struct, Array, Custom
    };

    class JASPERENGINE_API StringTable {
    public:
        uint32_t add(const std::string& text);
        const std::vector<std::string>& strings() const { return list; }
        void clear();
    private:
        std::vector<std::string> list;
        std::unordered_map<std::string, uint32_t> index;
    };

    class JASPERENGINE_API Writer {
    public:
        // entities: the reference a cooked entity is written as (scenes: its id, prefabs: its index
        // + 1); NULL_ENTITY is always 0... of the entity space it maps to (see Reader)
        Writer(StringTable& strings, std::function<uint32_t(Entity)> entities);

        void u8(uint8_t value) { bytes.push_back(value); }
        void u32(uint32_t value);
        void u64(uint64_t value);
        void varint(uint64_t value);
        void svarint(int64_t value);
        void f32(float value);
        void f64(double value);
        void boolean(bool value) { u8(value ? 1 : 0); }
        void string(const std::string& value);
        void blob(const void* data, size_t size);
        void entity(Entity value);

        // a u32 byte length filled in by endLength (records the reader can skip)
        size_t beginLength();
        void endLength(size_t at);

        std::vector<uint8_t>& data() { return bytes; }
        const std::vector<uint8_t>& data() const { return bytes; }
        StringTable& stringTable() { return strings; }

    private:
        std::vector<uint8_t> bytes;
        StringTable& strings;
        std::function<uint32_t(Entity)> entities;
    };

    class JASPERENGINE_API Reader {
    public:
        // entities: the runtime entity of a written reference
        Reader(std::span<const uint8_t> data, const std::vector<std::string>& strings, std::function<Entity(uint32_t)> entities);

        uint8_t u8();
        uint32_t u32();
        uint64_t u64();
        uint64_t varint();
        int64_t svarint();
        float f32();
        double f64();
        bool boolean() { return u8() != 0; }
        const std::string& string();
        std::span<const uint8_t> blob();
        Entity entity();

        // the bytes of a length-prefixed record: a reader of its own (same strings and entities)
        Reader record();
        void skip(size_t count);
        bool atEnd() const { return position >= bytes.size(); }
        size_t remaining() const { return position < bytes.size() ? bytes.size() - position : 0; }

        // a read past the end (or a bad index) fails the reader; every later read returns zeros
        bool failed() const { return failure; }
        const std::string& error() const { return message; }
        void fail(const std::string& why);

    private:
        bool take(size_t count);

        std::span<const uint8_t> bytes;
        size_t position = 0;
        const std::vector<std::string>* strings = nullptr;
        std::function<Entity(uint32_t)> entities;
        bool failure = false;
        std::string message;
        static const std::string emptyString;
    };

}
