// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract
// - Purpose: one GPU buffer for every immutable vertex or index buffer showing the same bytes of
//   the same immutable source, instead of one per MeshComponent: the buffers of a model file's
//   parse (ModelPool, shared by every Model of the file: glTF buffer views, the converted and
//   baked copies a load appends to the parse, an OBJ's vertices and indices), a MeshLodPool level
//   set, generated a_smoothNormal data (by content hash). 5,000 Models of one file draw from one
//   set of vertex and index buffers (a sokol pool holds a limited number of buffers).
// - A Buffer opts in with Buffer::setShareSource (ExternalBuffer::setData with an owner); any
//   write of its bytes drops the source, so an edited buffer gets a GPU buffer of its own (copy
//   on write). Dynamic and stream buffers (procedural meshes, terrain, sprites, instances, bones)
//   are never shared: only IMMUTABLE vertex and index buffers are.
// - Ref-counted: a BufferRender holds its SharedGpuBuffer (BufferRender::shared, copies hold it
//   too); destroyBuffer lets it go; the GPU buffer is destroyed with its last holder, on the
//   thread that lets it go (the async loading thread queues it, as SokolBuffer does).
// - Keyed by the owner's address, the bytes' id in it (their address for an ExternalBuffer), size
//   and type; the entry keeps a weak reference to the owner, so a new owner at the address of a
//   dead one (a model file reloaded) never finds the old bytes' buffer. A reloaded or invalidated
//   model file (MeshSystem::invalidateModelFile) is parsed into a new owner: its meshes get new
//   buffers as they reload, the old ones go with the last mesh still showing the old parse.
// - Thread-safety: mutex-guarded; acquire runs on the main thread or on a game's loading thread
//   (Engine::isAsyncThread: the buffer is made through SokolCmdQueue). The main thread does not
//   take a buffer the loading thread made and the queue has not run yet: it maps a new one.
// - Engine::clearPools / clearUnusedPools call clear / clearUnused (holders keep their buffers).

#ifndef MESHBUFFERPOOL_H
#define MESHBUFFERPOOL_H

#include "Export.h"
#include "render/Render.h"
#include "sokol/SokolBuffer.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace jasperengine{

    class Buffer;
    class BufferRender;

    // what a shared GPU buffer holds
    struct GpuBufferShareKey{
        const void* owner = nullptr; // address of the bytes' owner (null: id is a content hash)
        uint64_t id = 0;             // the bytes in the owner (or their content hash)
        size_t size = 0;
        BufferType type = BufferType::VERTEX_BUFFER;

        bool operator<(const GpuBufferShareKey& rhs) const;
        bool operator==(const GpuBufferShareKey& rhs) const;
    };

    // one shared GPU buffer, destroyed with its last holder
    class JASPERENGINE_API SharedGpuBuffer{
    public:
        GpuBufferShareKey key;
        std::weak_ptr<const void> owner;
        SokolBuffer buffer;
        uint64_t context = 0; // SystemRender::getContextId it was made in

        SharedGpuBuffer() = default;
        SharedGpuBuffer(const SharedGpuBuffer&) = delete;
        SharedGpuBuffer& operator=(const SharedGpuBuffer&) = delete;
        ~SharedGpuBuffer();
    };

    class JASPERENGINE_API MeshBufferPool{
    public:
        // an IMMUTABLE vertex or index buffer with bytes and a live share source
        static bool isShareable(const Buffer& buffer);

        // Makes the GPU buffer of `buffer` (its render): the shared one of its source when it is
        // shareable, else one of its own (BufferRender::createBuffer). False when it cannot be
        // made (a sokol pool is full: SokolSystem logs it), or the bytes were released and no one
        // holds their GPU buffer (ModelCpuData::ensure brings them back).
        static bool createRender(Buffer& buffer);

        // whether `render` holds the shared buffer of `buffer`'s current bytes (false when it
        // holds none, or one of other bytes: they were written or re-pointed since)
        static bool holdsSource(const BufferRender& render, const Buffer& buffer);

        // `render` draws from the shared buffer of `size` bytes of generated data whose content
        // hash is `hash` (made from `data` when no one holds it)
        static bool acquireContent(BufferRender& render, uint64_t hash, BufferType type, const void* data, size_t size);

        // shared GPU buffers alive
        static size_t getBufferCount();

        // necessary for engine shutdown: forgets every buffer (holders keep theirs)
        static void clear();
        // forgets the buffers no one holds any more
        static void clearUnused();

        // (SharedGpuBuffer's destructor)
        static void released(const SharedGpuBuffer* buffer);
    };
}

#endif /* MESHBUFFERPOOL_H */
