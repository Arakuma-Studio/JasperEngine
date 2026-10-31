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

        bool ownsData() const;
        // Keeps a copy of the bytes it points at as its own, for when their owner goes (the
        // same bytes: a share source stays)
        void copyToOwnData();

        void setName(const std::string& name);
        std::string getName() const;
    };

}


#endif //EXTERNALBUFFER_H
