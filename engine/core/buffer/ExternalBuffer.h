// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef EXTERNALBUFFER_H
#define EXTERNALBUFFER_H

#include "buffer/Buffer.h"
#include <string>
#include <vector>

namespace jasperengine {

    // A buffer whose bytes usually live elsewhere: setData points it at memory someone else
    // owns (a model file's data, a component's vector). Bytes written into it (importData,
    // a saved mesh being loaded) are its own instead, kept and released with the buffer and
    // copied with it.
    class JASPERENGINE_API ExternalBuffer : public Buffer {

    protected:
        std::string name;
        std::vector<unsigned char> ownedData;
        int modelBuffer = -1;
        size_t modelOffset = 0;

    public:
        ExternalBuffer();
        virtual ~ExternalBuffer();

        ExternalBuffer(const ExternalBuffer& rhs);
        ExternalBuffer& operator=(const ExternalBuffer& rhs);

        // Writing starts storage of its own: what a setData pointer showed is not carried
        // over, its owner may already have freed it
        virtual bool increase(size_t newSize);
        virtual void clearAll();

        // Points the buffer at bytes owned elsewhere, releasing any of its own
        void setData(unsigned char* data, size_t size);
        // ... bytes of `owner` that never change (a pooled model's file data, a pooled level
        // set), which other buffers may show too: they share one GPU buffer (setShareSource)
        void setData(unsigned char* data, size_t size, const std::shared_ptr<const void>& owner);
        // ... bytes `offset` of buffer `modelBuffer` of a model's parse, identified by those two (the
        // same GPU buffer when the bytes come back at another address: ModelCpuData)
        void setModelData(unsigned char* data, size_t size, const std::shared_ptr<const void>& owner, int modelBuffer, size_t offset);
        int getModelBuffer() const;
        size_t getModelOffset() const;
        // their owner frees them: no bytes to show, the size, attributes and GPU buffer stay
        void releaseData();
        // the same bytes back, at `data`
        void restoreData(unsigned char* data);

        bool ownsData() const;
        // Keeps a copy of the bytes it points at as its own, for when their owner goes (the
        // same bytes: a share source stays)
        void copyToOwnData();

        void setName(const std::string& name);
        std::string getName() const;
    };

}


#endif //EXTERNALBUFFER_H
