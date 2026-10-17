// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_EFFECTS_EFFECTRENDERER_H
#define HTMLUI_EFFECTS_EFFECTRENDERER_H

// The effects on the renderer (phase-5 spec 3.7): an RmlRenderer forwards the handles it is
// given with bit 0 set (CompileShader/CompileFilter named "jfx") here. A paint effect draws the
// element's geometry with its variant's program in the current transform, scissor and clip
// mask; a filter effect is a fullscreen pass of the filter chain (RmlFilterContext), after an
// optional blur prepass.

#include "Export.h"
#include "EffectRuntime.h"
#include "render/ObjectRender.h"
#include "shader/ShaderUniforms.h"

#include <RmlUi/Core/RenderInterface.h>

#include <map>
#include <memory>

namespace jasperengine {

    class RmlRenderer;
    class RmlFilterContext;
    class ShaderRender;
    class TextureRender;

}

namespace jasperengine::htmlui {

    class JASPERENGINE_INTERNAL EffectRenderer {
    public:
        explicit EffectRenderer(RmlRenderer& renderer);
        ~EffectRenderer();
        EffectRenderer(const EffectRenderer&) = delete;
        EffectRenderer& operator=(const EffectRenderer&) = delete;

        static bool isEffectHandle(uintptr_t handle) { return (handle & 1) != 0; }

        // RmlRenderer::beginFrame: what the frame's draws read once
        void beginFrame();
        static EffectInstance* instanceOf(uintptr_t handle) { return reinterpret_cast<EffectInstance*>(handle & ~uintptr_t(1)); }

        // RmlRenderer hooks: CompileShader/CompileFilter named "jfx" take the instance
        // ({"instance": VOIDPTR}) and give it back tagged; the handle owns the instance
        Rml::CompiledShaderHandle compileShader(const Rml::Dictionary& parameters);
        void renderShader(Rml::CompiledShaderHandle handle, Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation);
        void releaseShader(Rml::CompiledShaderHandle handle);
        Rml::CompiledFilterHandle compileFilter(const Rml::Dictionary& parameters);
        void releaseFilter(Rml::CompiledFilterHandle handle);
        // the filter pass inside CompositeLayers (false: nothing drawn, the chain is unchanged)
        bool applyFilter(Rml::CompiledFilterHandle handle, RmlFilterContext& context);

        // the view went away: programs are made again from the shaders when drawn next
        void releaseGpuResources();
        size_t programCount() const { return programs.size(); }

    private:
        struct Program {
            std::shared_ptr<ShaderRender> shader;
            uint32_t shaderId = 0;          // the backend shader the object was loaded for
            ObjectRender object;
            int vertexSlot = -1;
            int frameSlot = -1;
            int trailSlot = -1;
            int ripplesSlot = -1;
            CustomUniformBlock params;
            std::pair<int, int> sourceSlot{-1, -1};
            std::pair<int, int> blurredSlot{-1, -1};
        };

        Program* programFor(EffectInstance& instance, bool filterForm);
        Program* loadProgram(const std::shared_ptr<ShaderRender>& shader, bool filterForm);
        Program* rememberProgram(EffectInstance& instance, Program* program);
        void packParams(EffectInstance& instance, Program& program);
        float scaledPx(EffectInstance& instance, const EffectScaledParam& scaled);
        bool brightPass(EffectInstance& instance, RmlFilterContext& context, float threshold);
        void uploadCommon(EffectInstance& instance, Program& program, ObjectRender& object);

        RmlRenderer& renderer;
        // changes whenever a program is destroyed or replaced (instances cache their program)
        uint64_t programsGeneration = 1;
        // ShaderPool's custom-shader generation, read once a frame (beginFrame)
        uint32_t shaderGeneration = 0;
        std::map<ShaderRender*, std::unique_ptr<Program>> programs;
        std::shared_ptr<ShaderRender> brightShader;
    };

}

#endif //HTMLUI_EFFECTS_EFFECTRENDERER_H
