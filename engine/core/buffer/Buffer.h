#ifndef BUFFER_H
#define BUFFER_H

// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <string>
#include <map>
#include <memory>

#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "Attribute.h"
#include "render/Render.h"
#include "render/BufferRender.h"


namespace jasperengine {

    class JASPERENGINE_API Buffer {

    protected:
        std::map<AttributeType, Attribute> attributes;
        unsigned int count;
        BufferType type;
        BufferUsage usage;

        unsigned char* data;
        size_t size;
        unsigned int stride;

        bool renderAttributes;
        bool instanceBuffer;

        BufferRender render;

        // the immutable source its bytes are (setShareSource), dropped by any write
        std::weak_ptr<const void> shareOwner;
        uint64_t shareId;
        bool shareSource;
        // its bytes were freed by their owner (ModelCpuData): size, attributes and GPU buffer stay
        bool dataReleased = false;
        // the writes a renderer took (noteWrite): a copy of the bytes made before is stale once it moves
        uint64_t writeCount = 0;

    public:
        Buffer();
        virtual ~Buffer();

        Buffer(const Buffer& rhs);
        Buffer& operator=(const Buffer& rhs);

        virtual bool increase(size_t newSize);
        virtual void clearAll();
        virtual void clear();

        void importData(void* data, size_t size);
        void importData(const void* data, size_t size);

        void addAttribute(AttributeType attribute, AttributeDataType dataType, unsigned int elements, size_t offset);
        void addAttribute(AttributeType attribute, unsigned int elements, size_t offset, bool perInstance);
        void addAttribute(AttributeType attribute, unsigned int elements, size_t offset);
        void addAttribute(AttributeType attribute, Attribute Attribute);

        BufferRender* getRender();

        Attribute* getAttribute(AttributeType attribute);
        std::map<AttributeType, Attribute> getAttributes() const;

        void addUInt16(AttributeType attribute, uint16_t value);
        void addUInt32(AttributeType attribute, uint32_t value);
        void addFloat(AttributeType attribute, float value);
        void addVector2(AttributeType attribute, Vector2 vector);
        void addVector3(AttributeType attribute, Vector3 vector);
        void addVector4(AttributeType attribute, Vector4 vector);

        void addUInt16(Attribute* attribute, uint16_t value);
        void addUInt32(Attribute* attribute, uint32_t value);
        void addFloat(Attribute* attribute, float value);
        void addVector2(Attribute* attribute, Vector2 vector);
        void addVector3(Attribute* attribute, Vector3 vector);
        void addVector4(Attribute* attribute, Vector4 vector);

        void setUInt16(unsigned int index, Attribute* attribute, uint16_t value);
        void setUInt32(unsigned int index, Attribute* attribute, uint32_t value);
        void setFloat(unsigned int index, Attribute* attribute, float value);
        void setVector2(unsigned int index, Attribute* attribute, Vector2 vector);
        void setVector3(unsigned int index, Attribute* attribute, Vector3 vector);
        void setVector4(unsigned int index, Attribute* attribute, Vector4 vector);

        void setValues(unsigned int index, Attribute* attribute, unsigned int numValues, char* vector, size_t typesize);

        uint16_t getUInt16(AttributeType attribute, unsigned int index);
        uint32_t getUInt32(AttributeType attribute, unsigned int index);
        float getFloat(AttributeType attribute, unsigned int index);
        Vector2 getVector2(AttributeType attribute, unsigned int index);
        Vector3 getVector3(AttributeType attribute, unsigned int index);
        Vector4 getVector4(AttributeType attribute, unsigned int index);

        uint16_t getUInt16(Attribute* attribute, unsigned int index, int elementIndex = 0);
        uint32_t getUInt32(Attribute* attribute, unsigned int index, int elementIndex = 0);
        float getFloat(Attribute* attribute, unsigned int index, int elementIndex = 0);
        Vector2 getVector2(Attribute* attribute, unsigned int index);
        Vector3 getVector3(Attribute* attribute, unsigned int index);
        Vector4 getVector4(Attribute* attribute, unsigned int index);

        unsigned char* getData() const;

        void setSize(size_t size);
        size_t getSize() const;

        void setStride(unsigned int stride);
        unsigned int getStride() const;

        void setCount(unsigned int count);
        unsigned int getCount() const;

        void setType(BufferType type);
        BufferType getType() const;

        void setUsage(BufferUsage usage);
        BufferUsage getUsage() const;

        bool isRenderAttributes() const;
        void setRenderAttributes(bool renderAttributes);

        bool isInstanceBuffer() const;
        void setInstanceBuffer(bool instanceBuffer);

        // Its bytes are bytes `id` of `owner`, an immutable source others read too (the parse of
        // a model file in ModelPool, a MeshLodPool level set): every immutable vertex or index
        // buffer with the same source, type and size draws from one GPU buffer (MeshBufferPool).
        // Set by the loader once the bytes are in place; a write of the bytes (setValues, add*,
        // set*, importData, increase, clear, clearAll, ExternalBuffer::setData) drops it, so an
        // edited buffer gets a GPU buffer of its own. It does not keep the owner alive.
        void setShareSource(const std::shared_ptr<const void>& owner, uint64_t id);
        void clearShareSource();
        // a source is set and its owner is alive
        bool hasShareSource() const;
        const std::weak_ptr<const void>& getShareOwner() const;
        bool isDataReleased() const { return dataReleased; }
        // a write of its bytes was taken (RenderSystem, uploading a needUpdateBuffer): whoever copied
        // the bytes before (a Dynamic Batching record) compares getWriteCount to see it, whichever
        // pass uploaded it first
        void noteWrite(){ writeCount++; }
        uint64_t getWriteCount() const { return writeCount; }
        uint64_t getShareId() const;
    };

}

#endif //BUFFER_H
