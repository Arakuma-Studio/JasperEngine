// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ShaderRender_h
#define ShaderRender_h

#include "Render.h"
#include "sokol/SokolShader.h"

#include <atomic>

namespace jasperengine{
    class JASPERENGINE_API ShaderRender{

    private:
        // render callback clean function
        static void cleanupShader(void* data);

    public:
        //***Backend***
        SokolShader backend;
        //***
        ShaderData shaderData; //For reflection info
        //Deferred MAKE_SHADER in flight; blocks re-entrant createShader(), and another thread's:
        //whoever sets it makes the shader
        std::atomic<bool> loading{false};

        ShaderRender();
        ShaderRender(const ShaderRender& rhs);
        ShaderRender& operator=(const ShaderRender& rhs);
        virtual ~ShaderRender();

        bool createShader(ShaderData& shaderData);
        void destroyShader();
        bool isCreated();
        // its GPU object is being created off the render thread (spec S32): not created, not failed
        bool isPending();
        bool isFailed();
    };
}


#endif /* ShaderRender_h */
