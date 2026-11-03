// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolShader_h
#define SokolShader_h

#include <atomic>
#include <mutex>
#include "render/Render.h"
#include "shader/ShaderData.h"
#include "sokol_gfx.h"

namespace jasperengine{

    // textures and storage buffers share the sg_bindings.views[] bindspace, but
    // shader reflection (ShaderData) keeps independent slot counters for each type,
    // so storage-buffer view slots are placed after the texture range
    enum { SOKOL_STORAGEBUFFER_VIEW_SLOT_OFFSET = SG_MAX_VIEW_BINDSLOTS - 8 };

    // A thread loading a scene and the main thread outside a frame both come here, for the same
    // shader too: the handle is atomic, whoever made it stored it after its slot was ready. What
    // that thread does with a handle is queued (SokolCmdQueue), but its looks at the slot are not:
    // destroyMutex keeps them off the main thread's destruction. The main thread itself takes no
    // lock to look, on the draws' path: nothing frees a slot under it but itself.
    class SokolShader{

    private:
        std::atomic<uint32_t> shaderId{SG_INVALID_ID};
        std::mutex destroyMutex;

        sg_resource_state queryState();

        int roundup(int val, int round_to);
        sg_uniform_type uniformToSokolType(ShaderUniformType type);
        sg_uniform_type flattenedUniformToSokolType(ShaderUniformType type);
        sg_image_sample_type textureSamplerToSokolType(TextureSamplerType type);
        sg_image_type textureToSokolType(TextureType type);
        sg_sampler_type samplerToSokolType(SamplerType type);

    public:
        SokolShader();
        SokolShader(const SokolShader& rhs);
        SokolShader& operator=(const SokolShader& rhs);

        bool createShader(ShaderData& shaderData);
        void destroyShader();
        bool isCreated();
        // created asynchronously and not ready yet (SokolAsyncCreate): neither created nor failed
        bool isPending();
        bool isFailed();

        sg_shader get();
    };
}


#endif /* SokolShader_h */
