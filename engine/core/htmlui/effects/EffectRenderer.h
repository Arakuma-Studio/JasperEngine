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
#include "render/TextureRender.h"

#include <RmlUi/Core/RenderInterface.h>

#include <map>
#include <memory>

namespace jasperengine {

    class RmlRenderer;
    class RmlFilterContext;
    class ShaderRender;

}

namespace jasperengine::htmlui {

    struct EffectSimulation;
    struct EffectStateImage;

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
            // phase 6: the states (manifest order; u_state_<name>), the step block, the scatter
            // vertex block and placement image
            std::vector<std::pair<int, int>> stateSlots;
            int stepSlot = -1;
            int scatterSlot = -1;
            std::pair<int, int> scatterPlaceSlot{-1, -1};
        };

        Program* programFor(EffectInstance& instance, bool filterForm);
        // vertexless: the filter, step and scatter forms (a fullscreen triangle, or the scatter
        // quads); the paint form draws the element's geometry
        Program* loadProgram(const std::shared_ptr<ShaderRender>& shader, bool vertexless, const EffectManifest* manifest = nullptr);
        Program* rememberProgram(EffectInstance& instance, Program* program);
        void packParams(EffectInstance& instance, Program& program);
        float scaledPx(EffectInstance& instance, const EffectScaledParam& scaled);
        bool brightPass(EffectInstance& instance, RmlFilterContext& context, float threshold);
        void uploadCommon(EffectInstance& instance, Program& program, ObjectRender& object);

        // --- phase 6: stateful effects (EffectSimulator.cpp) ---
        // Runs the instance's simulation for this frame, right before its draw: allocates or
        // resizes its state images, then the passes the clock asks for (3.5-3.6). Paint draws come
        // here inside a layer pass, which this ends; filter passes with their chain (`filter`, the
        // content). False: the effect does not draw this frame (refused, unsupported, clipped out,
        // an empty area). A stateless effect: true, nothing done.
        bool advanceSimulation(EffectInstance& instance, Program& drawProgram, RmlFilterContext* filter);
        // the current state images into a draw (or pass) program; placeholders without a simulation
        void bindStates(EffectInstance& instance, Program& program, int writingState = -1);
        Program* formProgram(EffectInstance& instance, uint32_t properties);
        bool clippedOut(const EffectInstance& instance) const;
        bool allocateStates(EffectInstance& instance, EffectSimulation& simulation, bool resample);
        void clearImage(EffectStateImage& image);
        void runInitAndWarmup(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter, double resetTime);
        void runInit(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter, double resetTime);
        void runWarmStep(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter, double resetTime,
                         int w);
        struct StepInput;
        void runStep(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter, const StepInput& input,
                     bool initPasses);
        bool runPass(EffectInstance& instance, EffectSimulation& simulation, Program& drawProgram, RmlFilterContext* filter,
                     const EffectPassDef& pass, int passIndex, int iteration, const StepInput& input);
        TextureRender* blackPlaceholder();

        RmlRenderer& renderer;
        // changes whenever a program is destroyed or replaced (instances cache their program)
        uint64_t programsGeneration = 1;
        // ShaderPool's custom-shader generation, read once a frame (beginFrame)
        uint32_t shaderGeneration = 0;
        std::map<ShaderRender*, std::unique_ptr<Program>> programs;
        std::shared_ptr<ShaderRender> brightShader;
        TextureRender black;                    // the state placeholder (a 1x1 transparent black image)
        bool blackCreated = false;
    };

}

#endif //HTMLUI_EFFECTS_EFFECTRENDERER_H
