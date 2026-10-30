// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShaderRender.h"

#include "SystemRender.h"
#include "Engine.h"

using namespace jasperengine;

ShaderRender::ShaderRender(){ }

ShaderRender::ShaderRender(const ShaderRender& rhs) = default;

ShaderRender& ShaderRender::operator=(const ShaderRender& rhs) = default;

ShaderRender::~ShaderRender(){
    //Cannot destroy because its a handle
}

bool ShaderRender::createShader(ShaderData& shaderData){
    // Skip while a deferred create is in flight: re-assigning shaderData would
    // free the source the queued sg_shader_desc points into and corrupt the build.
    // and while the GPU object is being created (spec S32): its handle is the one that becomes ready
    if (Engine::isViewLoaded() && !isCreated() && !loading && !isPending()) {
        this->shaderData = shaderData;

        loading = true;

        bool ret = backend.createShader(this->shaderData);

        // Via the command stream so cleanup runs after MAKE_SHADER (a frame-counted
        // scheduleCleanup could fire before the build -> empty source on async path).
        SystemRender::addQueueCommand(ShaderRender::cleanupShader, this);

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

void ShaderRender::cleanupShader(void* data){
    ShaderRender* self = static_cast<ShaderRender*>(data);
    self->shaderData.releaseSourceData(); //Keep only reflection info
    self->loading = false; //Build executed; allow retry on failure
}
