// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FLUID_SYSTEM_H
#define FLUID_SYSTEM_H

// Runs the scene's FluidSimulationComponents: creates their FluidSimulator, feeds them the
// frame's time (paused with the scene, see setPaused) and the pointer (interactive ones),
// steps and draws them in RenderSystem's frame passes (before any camera pass, so cameras and
// materials sample this frame's fluid), lends MATERIAL outputs to the entity's Sprite, Image or
// Mesh at draw time (getMaterialTexture: the component data keeps its own texture, so nothing
// of it is saved or becomes a prefab override), draws SCREEN outputs in the main camera's
// pass (over the scene, under the HTML UI documents), and frees the GPU side on removal. An existing component.simulator (made by the
// GameObject wrapper or a texture link first) is the one used.

#include "ecs/SubSystem.h"
#include "component/FluidSimulationComponent.h"
#include "math/Rect.h"
#include "render/PipelineState.h"

namespace jasperengine{

    class FramebufferRender;
    class TextureRender;

    class JASPERENGINE_API FluidSystem : public SubSystem {
    public:
        FluidSystem(Scene* scene);
        virtual ~FluidSystem();

        void load() override;
        void draw() override;
        void destroy() override;
        void update(double dt) override;

        void onComponentAdded(Entity entity, ComponentId componentId) override;
        void onComponentRemoved(Entity entity, ComponentId componentId) override;

        // the editor's Inspector preview: steps entities of a scene the editor keeps paused
        void setPreview(Entity entity, bool preview);
        bool isPreview(Entity entity) const;

        // RenderSystem's frame passes: steps and draws every active fluid (once a frame)
        void renderFluids();

        // RenderSystem, in the main camera's colour pass after the scene (before the HTML UI
        // documents): the SCREEN fluids over the pass's viewport, premultiplied (target: the
        // pass's formats; flipY: a target drawn Y-flipped, RenderSystem::isRenderingFlipped)
        void drawScreen(const RenderTargetFormat& target, bool flipY);
        // the same in a pass of its own over a target (null: the swapchain), e.g. after
        // post-processing (none without such a fluid)
        void compositeScreen(FramebufferRender* target, const Rect& viewRect, bool flipY);
        bool hasScreenFluids() const;

        // RenderSystem's draws: the image an entity's Sprite, Image or Mesh (submesh 0's base
        // colour) shows instead of its texture, its MATERIAL fluid's output (null: its own)
        // (transparent: set to whether that output is transparent, straight alpha)
        TextureRender* getMaterialTexture(Entity entity, bool* transparent = nullptr) const;
        bool hasMaterialFluids() const;

        // width / height of an entity's fluid: its aspectRatio, else automatic (the canvas in
        // SCREEN, the Sprite or Image in MATERIAL, else 1)
        float getAspectRatio(Entity entity) const;

    private:
        struct State;
        State* state;
    };

}

#endif // FLUID_SYSTEM_H
