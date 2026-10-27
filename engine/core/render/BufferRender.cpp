// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "BufferRender.h"
#include "Engine.h"
#include "Log.h"
#include "pool/MeshBufferPool.h"

using namespace jasperengine;

BufferRender::BufferRender(){ }

BufferRender::BufferRender(const BufferRender& rhs) = default;

BufferRender& BufferRender::operator=(const BufferRender& rhs) = default;

BufferRender::~BufferRender(){
    //Cannot destroy because its a handle (a shared buffer goes with its last holder)
}

bool BufferRender::createBuffer(unsigned int size, void* data, BufferType type, BufferUsage usage){
    // a shared one is the pool's to make (destroyBuffer lets it go first)
    if (Engine::isViewLoaded() && !shared && !isCreated())
        return backend.createBuffer(size, data, type, usage);
    else
        return false;
}

void BufferRender::updateBuffer(unsigned int size, void* data){
    if (shared){
        Log::error("A shared GPU buffer is immutable: not updated (the buffer's bytes were written without dropping its share source)");
        return;
    }
    backend.updateBuffer(size, data);
}

int BufferRender::appendBuffer(unsigned int size, const void* data){
    if (shared)
        return -1;
    return backend.appendBuffer(size, data);
}

void BufferRender::destroyBuffer(){
    if (shared){
        // the handle is the shared buffer's: forgotten here, destroyed by its last holder
        shared.reset();
        backend = SokolBuffer();
        return;
    }
    backend.destroyBuffer();
}

bool BufferRender::isCreated(){
    return backend.isCreated();
}

bool BufferRender::isShared() const{
    return shared != nullptr;
}

BufferUsage BufferRender::getCreatedUsage() const{
    return backend.getCreatedUsage();
}
