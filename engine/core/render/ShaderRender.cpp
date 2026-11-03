// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderRender.h"

#include "SystemRender.h"
#include "Engine.h"

using namespace jasperengine;

ShaderRender::ShaderRender(){ }

ShaderRender::ShaderRender(const ShaderRender& rhs): backend(rhs.backend), shaderData(rhs.shaderData), loading(rhs.loading.load()){ }

ShaderRender& ShaderRender::operator=(const ShaderRender& rhs){
    backend = rhs.backend;
    shaderData = rhs.shaderData;
    loading.store(rhs.loading.load());
    return *this;
}

ShaderRender::~ShaderRender(){
    //Cannot destroy because its a handle
}

bool ShaderRender::createShader(ShaderData& shaderData){
    // Skip while a deferred create is in flight: re-assigning shaderData would
    // free the source the queued sg_shader_desc points into and corrupt the build.
    // and while the GPU object is being created (spec S32): its handle is the one that becomes ready
    // Two threads may ask at once (a scene the main thread loads outside a frame, one loading off
    // it): the flag decides, and the look comes after it, when no other can be making it.
    if (Engine::isViewLoaded() && !loading.exchange(true)) {
        if (isCreated() || isPending()){
            loading.store(false);
            return false;
        }

        this->shaderData = shaderData;

        bool ret = backend.createShader(this->shaderData);

        // Via the command stream so cleanup runs after MAKE_SHADER (a frame-counted
        // scheduleCleanup could fire before the build -> empty source on async path).
        // That batch holds the shader (one of ShaderPool's: every one off the main thread):
        // MAKE_SHADER reads its sources and this writes to it, whoever lets it go meanwhile, and
        // ShaderPool::remove leaves it while it is held.
        const std::shared_ptr<ShaderRender> self = Engine::isAsyncThread() ? weak_from_this().lock() : nullptr;
        if (self){
            SystemRender::addQueueCommand(ShaderRender::cleanupHeldShader, new std::shared_ptr<ShaderRender>(self));
        }else{
            SystemRender::addQueueCommand(ShaderRender::cleanupShader, this);
        }

        return ret;
    }else{
        return false;
    }
}

void ShaderRender::destroyShader(){
    backend.destroyShader();
}

bool ShaderRender::isCreated(){
    return backend.isCreated();
}

bool ShaderRender::isPending(){
    return backend.isPending();
}

bool ShaderRender::isFailed(){
    return backend.isFailed();
}

void ShaderRender::cleanupHeldShader(void* data){
    std::shared_ptr<ShaderRender>* held = static_cast<std::shared_ptr<ShaderRender>*>(data);
    cleanupShader(held->get());
    delete held;
}

void ShaderRender::cleanupShader(void* data){
    ShaderRender* self = static_cast<ShaderRender*>(data);
    self->shaderData.releaseSourceData(); //Keep only reflection info
    self->loading = false; //Build executed; allow retry on failure
}
