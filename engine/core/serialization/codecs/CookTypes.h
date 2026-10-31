// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Codecs of the engine values the component hooks write beside the field tables: buffers with
// their attributes and bytes (as the scene codec saves them, Stream::encodeBuffer), bounds.

#include "serialization/CookCodecs.h"

#include "buffer/Attribute.h"
#include "buffer/ExternalBuffer.h"
#include "buffer/IndexBuffer.h"
#include "buffer/InterleavedBuffer.h"
#include "math/AABB.h"

namespace jasperengine::cook {

    template<>
    struct Codec<Attribute> {
        static void write(Writer& w, const Attribute& a) {
            cook::write(w, a.getDataType());
            w.string(a.getBufferName());
            w.varint(a.getElements());
            w.varint(a.getOffset());
            w.varint(a.getCount());
            w.boolean(a.getNormalized());
            w.boolean(a.getPerInstance());
        }
        static void read(Reader& r, Attribute& a) {
            AttributeDataType dataType{};
            cook::read(r, dataType);
            const std::string bufferName = r.string();
            const unsigned int elements = static_cast<unsigned int>(r.varint());
            const size_t offset = static_cast<size_t>(r.varint());
            const unsigned int count = static_cast<unsigned int>(r.varint());
            const bool normalized = r.boolean();
            const bool perInstance = r.boolean();
            a = Attribute(dataType, bufferName, elements, offset, count, normalized, perInstance);
        }
    };

    // the settings, attributes and bytes of a buffer (Stream::encodeBuffer / decodeBuffer)
    inline void writeBuffer(Writer& w, const Buffer& buffer) {
        cook::write(w, buffer.getType());
        cook::write(w, buffer.getUsage());
        w.varint(buffer.getStride());
        w.varint(buffer.getSize());
        w.varint(buffer.getCount());
        w.boolean(buffer.isRenderAttributes());
        w.boolean(buffer.isInstanceBuffer());
        cook::write(w, buffer.getAttributes());
        if (buffer.getData() && buffer.getSize() > 0) w.blob(buffer.getData(), buffer.getSize());
        else w.blob(nullptr, 0);
    }

    inline void readBuffer(Reader& r, Buffer& buffer) {
        BufferType type{};
        BufferUsage usage{};
        cook::read(r, type);
        cook::read(r, usage);
        buffer.setType(type);
        buffer.setUsage(usage);
        buffer.setStride(static_cast<unsigned int>(r.varint()));
        buffer.setSize(static_cast<size_t>(r.varint()));
        buffer.setCount(static_cast<unsigned int>(r.varint()));
        buffer.setRenderAttributes(r.boolean());
        buffer.setInstanceBuffer(r.boolean());
        std::map<AttributeType, Attribute> attributes;
        cook::read(r, attributes);
        for (const auto& [attributeType, attribute] : attributes) buffer.addAttribute(attributeType, attribute);
        const std::span<const uint8_t> data = r.blob();
        if (!data.empty()) buffer.importData(static_cast<const void*>(data.data()), data.size());
    }

    template<>
    struct Codec<InterleavedBuffer> {
        static void write(Writer& w, const InterleavedBuffer& b) {
            writeBuffer(w, b);
            w.varint(b.getVertexSize());
        }
        static void read(Reader& r, InterleavedBuffer& b) {
            readBuffer(r, b);
            b.setVertexSize(static_cast<unsigned int>(r.varint()));
        }
    };

    template<>
    struct Codec<IndexBuffer> {
        static void write(Writer& w, const IndexBuffer& b) { writeBuffer(w, b); }
        static void read(Reader& r, IndexBuffer& b) { readBuffer(r, b); }
    };

    // a model's separate vertex streams; the bytes become the buffer's own (importData)
    template<>
    struct Codec<ExternalBuffer> {
        static void write(Writer& w, const ExternalBuffer& b) {
            writeBuffer(w, b);
            w.string(b.getName());
        }
        static void read(Reader& r, ExternalBuffer& b) {
            b.clearAll();
            readBuffer(r, b);
            b.setName(r.string());
        }
    };

    template<>
    struct Codec<AABB> {
        static void write(Writer& w, const AABB& box) {
            const uint8_t state = box.isNull() ? 0 : box.isInfinite() ? 2 : 1;
            w.u8(state);
            if (state == 1) {
                cook::write(w, box.getMinimum());
                cook::write(w, box.getMaximum());
            }
        }
        static void read(Reader& r, AABB& box) {
            const uint8_t state = r.u8();
            if (state == 0) {
                box.setNull();
            } else if (state == 2) {
                box.setInfinite();
            } else {
                Vector3 min, max;
                cook::read(r, min);
                cook::read(r, max);
                box.setExtents(min, max);
            }
        }
    };

}
