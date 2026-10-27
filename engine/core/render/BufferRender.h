// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BufferRender_h
#define BufferRender_h

#include "Render.h"
#include "sokol/SokolBuffer.h"

#include <memory>

namespace jasperengine{
    class SharedGpuBuffer;

    class JASPERENGINE_API BufferRender{

    public:
        //***Backend***
        SokolBuffer backend;
        //***
        // the GPU buffer it draws from when that is shared (MeshBufferPool): held, not owned
        // alone, destroyed with its last holder (a copy of the render holds it too)
        std::shared_ptr<SharedGpuBuffer> shared;

        BufferRender();
        BufferRender(const BufferRender& rhs);
        BufferRender& operator=(const BufferRender& rhs);
        virtual ~BufferRender();

        bool createBuffer(unsigned int size, void* data, BufferType type, BufferUsage usage);
        // a shared buffer is immutable: not updated (an error)
        void updateBuffer(unsigned int size, void* data);
        // appends after what this frame appended (sokol resets the position every frame) and
        // returns the data's byte offset, -1 when not appended; not mixed with updateBuffer
        // in a frame, main thread only
        int appendBuffer(unsigned int size, const void* data);
        // a shared buffer is let go (destroyed when no one else holds it)
        void destroyBuffer();
        bool isCreated();
        bool isShared() const;
        BufferUsage getCreatedUsage() const;
    };
}


#endif /* BufferRender_h */
