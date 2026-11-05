// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MeshBufferPool.h"

#include "Engine.h"
#include "buffer/Buffer.h"
#include "render/BufferRender.h"
#include "render/SystemRender.h"

#include <atomic>
#include <map>
#include <mutex>
#include <tuple>

using namespace jasperengine;

bool GpuBufferShareKey::operator<(const GpuBufferShareKey& rhs) const{
    return std::tie(owner, id, idHi, size, type) < std::tie(rhs.owner, rhs.id, rhs.idHi, rhs.size, rhs.type);
}

bool GpuBufferShareKey::operator==(const GpuBufferShareKey& rhs) const{
    return owner == rhs.owner && id == rhs.id && idHi == rhs.idHi && size == rhs.size && type == rhs.type;
}

namespace{
    typedef std::map<GpuBufferShareKey, std::weak_ptr<SharedGpuBuffer>> shared_buffers_t;

    // recursive: letting a buffer go under the lock (a render dropping the one it held) runs
    // SharedGpuBuffer's destructor, which takes it again
    std::recursive_mutex& getMutex(){
        static std::recursive_mutex* mutex = new std::recursive_mutex();
        return *mutex;
    }

    //To prevent similar problem of static init fiasco but on deinitialization
    //https://isocpp.org/wiki/faq/ctors#static-init-order-on-first-use
    shared_buffers_t& getMap(){
        static shared_buffers_t* map = new shared_buffers_t();
        return *map;
    }

    std::atomic<size_t> aliveBuffers{0};

    // the same owner object (not merely the same address: the control block tells a new
    // owner at a dead one's address apart); two empty references (content keys) are the same
    bool sameOwner(const std::weak_ptr<const void>& a, const std::weak_ptr<const void>& b){
        return !a.owner_before(b) && !b.owner_before(a);
    }

    // whether this thread may draw from `entry` now
    bool usable(const std::shared_ptr<SharedGpuBuffer>& entry, const std::weak_ptr<const void>& owner, uint64_t context){
        if (!entry || entry->context != context || !sameOwner(entry->owner, owner))
            return false;
        if (entry->buffer.isCreated())
            return true;
        // made by the loading thread and not created yet: the loading thread's later commands
        // run after it, the main thread would draw it before
        return Engine::isAsyncThread() && entry->buffer.isPending();
    }

    bool acquire(BufferRender& render, const GpuBufferShareKey& key, const std::weak_ptr<const void>& owner, const void* data){
        if (!Engine::isViewLoaded() || key.size == 0)
            return false;

        std::lock_guard<std::recursive_mutex> lock(getMutex());
        const uint64_t context = SystemRender::getContextId();
        shared_buffers_t& map = getMap();

        std::shared_ptr<SharedGpuBuffer> entry;
        auto it = map.find(key);
        if (it != map.end()){
            entry = it->second.lock();
            if (!usable(entry, owner, context))
                entry.reset();
        }

        if (entry && render.shared == entry)
            return true;

        if (!entry){
            if (!data)
                return false; // (released bytes: only a buffer someone holds will do)
            entry = std::make_shared<SharedGpuBuffer>();
            entry->key = key;
            entry->owner = owner;
            entry->context = context;
            const bool made = entry->buffer.createBuffer((unsigned int)key.size, const_cast<void*>(data), key.type, BufferUsage::IMMUTABLE);
            if (entry->buffer.get().id != SG_INVALID_ID)
                aliveBuffers++; // (released counts it down, also when it failed)
            if (!made || (!Engine::isAsyncThread() && !entry->buffer.isCreated()))
                return false;
            // a mapping of a buffer that is gone, or that this thread cannot draw (holders keep it)
            map[key] = entry;
        }

        render.shared = entry; // lets go of a shared one it held before
        render.backend = entry->buffer;
        return true;
    }
}

SharedGpuBuffer::~SharedGpuBuffer(){
    MeshBufferPool::released(this);
}

void MeshBufferPool::released(const SharedGpuBuffer* buffer){
    std::lock_guard<std::recursive_mutex> lock(getMutex());
    if (buffer->buffer.get().id == SG_INVALID_ID)
        return; // never made
    aliveBuffers--;
    shared_buffers_t& map = getMap();
    auto it = map.find(buffer->key);
    if (it != map.end() && it->second.expired())
        map.erase(it);
    // a handle of an earlier render context means nothing in this one
    if (buffer->context == SystemRender::getContextId()){
        SokolBuffer handle = buffer->buffer;
        handle.destroyBuffer();
    }
}

bool MeshBufferPool::isShareable(const Buffer& buffer){
    return buffer.getUsage() == BufferUsage::IMMUTABLE &&
           (buffer.getType() == BufferType::VERTEX_BUFFER || buffer.getType() == BufferType::INDEX_BUFFER) &&
           buffer.getSize() > 0 &&
           // by source (whose bytes may be released: a buffer someone holds still serves), else by
           // the bytes themselves
           (buffer.hasShareSource() ? (buffer.getData() || buffer.isDataReleased()) : buffer.getData() != nullptr);
}

ContentHash128 MeshBufferPool::contentKey(const Buffer& buffer){
    return hashContent128(buffer.getData(), buffer.getSize());
}

namespace{
    GpuBufferShareKey contentShareKey(const ContentHash128& hash, size_t size, BufferType type){
        GpuBufferShareKey key;
        key.id = hash.lo;
        key.idHi = hash.hi;
        key.size = size;
        key.type = type;
        return key;
    }
}

bool MeshBufferPool::createRender(Buffer& buffer){
    BufferRender& render = *buffer.getRender();
    if (isShareable(buffer)){
        if (buffer.hasShareSource()){
            // kept alive while its bytes are uploaded
            std::shared_ptr<const void> owner = buffer.getShareOwner().lock();
            if (owner){
                GpuBufferShareKey key;
                key.owner = owner.get();
                key.id = buffer.getShareId();
                key.size = buffer.getSize();
                key.type = buffer.getType();
                return acquire(render, key, buffer.getShareOwner(), buffer.getData());
            }
        }else{
            const GpuBufferShareKey key = contentShareKey(contentKey(buffer), buffer.getSize(), buffer.getType());
            return acquire(render, key, std::weak_ptr<const void>(), buffer.getData());
        }
    }
    if (render.isShared())
        render.destroyBuffer();
    if (buffer.isDataReleased())
        return false;
    return render.createBuffer((unsigned int)buffer.getSize(), buffer.getData(), buffer.getType(), buffer.getUsage());
}

bool MeshBufferPool::holdsSource(const BufferRender& render, const Buffer& buffer){
    if (!render.shared || !isShareable(buffer))
        return false;
    const GpuBufferShareKey& key = render.shared->key;
    if (!buffer.hasShareSource()){
        // a content key: the bytes it was made of are the buffer's now
        return key.owner == nullptr && key == contentShareKey(contentKey(buffer), buffer.getSize(), buffer.getType());
    }
    std::shared_ptr<const void> owner = buffer.getShareOwner().lock();
    return owner && key.owner == owner.get() && key.id == buffer.getShareId() && key.size == buffer.getSize() &&
           key.type == buffer.getType() && sameOwner(render.shared->owner, buffer.getShareOwner());
}

bool MeshBufferPool::acquireContent(BufferRender& render, BufferType type, const void* data, size_t size){
    if (!data || size == 0)
        return false;
    return acquire(render, contentShareKey(hashContent128(data, size), size, type), std::weak_ptr<const void>(), data);
}

size_t MeshBufferPool::getBufferCount(){
    return aliveBuffers.load();
}

void MeshBufferPool::clear(){
    // the map only refers to the buffers: their holders destroy them when they let go
    std::lock_guard<std::recursive_mutex> lock(getMutex());
    getMap().clear();
}

void MeshBufferPool::clearUnused(){
    std::lock_guard<std::recursive_mutex> lock(getMutex());
    shared_buffers_t& map = getMap();
    for (auto it = map.begin(); it != map.end();){
        if (it->second.expired())
            it = map.erase(it);
        else
            ++it;
    }
}
