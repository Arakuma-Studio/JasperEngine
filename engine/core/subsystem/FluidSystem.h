// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FLUID_SYSTEM_H
#define FLUID_SYSTEM_H

// Runs the scene's FluidSimulationComponents: creates their FluidSimulator, feeds them the
// frame's time (paused with the scene, see setPaused) and the pointer (interactive ones),
// steps and draws them in RenderSystem's frame passes (before any camera pass, so cameras and
// materials sample this frame's fluid), lends MATERIAL outputs to the entity's Sprite, Image or
// Mesh at draw time (getMaterialTexture: the component data keeps its own texture, so nothing
// of it is saved or becomes a prefab override), composites SCREEN outputs over the main
// camera, and frees the GPU side on removal. An existing component.simulator (made by the
// GameObject wrapper or a texture link first) is the one used.

#include "ecs/SubSystem.h"
#include "component/FluidSimulationComponent.h"
#include "math/Rect.h"

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

        // RenderSystem, after a view's main camera and its post-processing: the SCREEN fluids
        // over the view (target null: the swapchain; flipY: a target drawn Y-flipped, GL
        // offscreen ones), premultiplied, in a pass of their own (none without such a fluid)
        void compositeScreen(FramebufferRender* target, const Rect& viewRect, bool flipY);
        bool hasScreenFluids() const;

        // RenderSystem's draws: the image an entity's Sprite, Image or Mesh (submesh 0's base
        // colour) shows instead of its texture, its MATERIAL fluid's output (null: its own)
        TextureRender* getMaterialTexture(Entity entity) const;
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
