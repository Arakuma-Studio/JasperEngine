// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BufferRender_h
#define BufferRender_h

#include "Render.h"
#include "sokol/SokolBuffer.h"

namespace jasperengine{
    class JASPERENGINE_API BufferRender{

    public:
        //***Backend***
        SokolBuffer backend;
        //***
        
        BufferRender();
        BufferRender(const BufferRender& rhs);
        BufferRender& operator=(const BufferRender& rhs);
        virtual ~BufferRender();

        bool createBuffer(unsigned int size, void* data, BufferType type, BufferUsage usage);
        void updateBuffer(unsigned int size, void* data);
        // appends after what this frame appended (sokol resets the position every frame) and
        // returns the data's byte offset, -1 when not appended; not mixed with updateBuffer
        // in a frame, main thread only
        int appendBuffer(unsigned int size, const void* data);
        void destroyBuffer();
        bool isCreated();
        BufferUsage getCreatedUsage() const;
    };
}


#endif /* BufferRender_h */