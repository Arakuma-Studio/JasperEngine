// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim-properties spec 10, 11)
// - Purpose: the editor's names of clip property tracks, one place for the Animation window
//   (Add Property, rows), the Inspector (clip track lines) and MCP (edit_animation component
//   names, get_animation list_properties labels): the group a property shows under ("Image",
//   "Light", "GameObject", "Torch (Script)", "Material [0]"), its Inspector label ("Color", "Y
//   FOV", "Anchored Position"), and the component names agents may give (Catalog keys or
//   Inspector display names: "Image" -> "ui").
// - Thread-safety: main thread (reads a registry, Catalog's per-thread name cache).

#pragma once

#include "animation/AnimationClip.h"
#include "animation/PropertyBindings.h"
#include "ecs/Entity.h"

#include <string>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
}

namespace jasperengine::editor{

    class ClipPropertyNames{
    public:
        // The group of `component` on `entity`: "GameObject", "<Class> (Script)", the Inspector
        // display name of the component ("Light", "Rect Transform"); the UI graphic's component
        // the entity shows ("Image", "Text", "Button"...); "Sprite Renderer" for a sprite's mesh
        // (registry null or entity NULL_ENTITY: from the key alone)
        static std::string groupLabel(EntityRegistry* registry, Entity entity, const std::string& component);
        // The Inspector label of a property ("Color", "Intensity", "Material [0] : Base Color";
        // a swizzle "Color (Alpha)"; a script field its name)
        static std::string propertyLabel(const std::string& component, const std::string& property);
        // "Image.Color", "GameObject.Active", "Torch (Script).glow" (the window's row suffix)
        static std::string trackLabel(EntityRegistry* registry, Entity entity, const ClipTrackData& track);
        // The component and property a track names from what an agent or the Inspector gives:
        // a Catalog key ("ui") or a display name ("Image", "Sprite Renderer", "Rect Transform"),
        // "GameObject", "script:<Class>" or "<Class> (Script)"; a UI graphic's display name
        // resolves to "ui", a sprite's colour to its mesh's submeshes[0].material.baseColorFactor.
        // False (and `error` with the animatable properties of the component) when unknown.
        static bool resolve(const std::string& component, const std::string& property, std::string& outComponent,
                            std::string& outProperty, std::string& error);
        // the animatable property names of a component key, for did-you-means ("color, intensity, range...")
        static std::string listProperties(const std::string& component);
    };

}
