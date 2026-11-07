// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The components with events (FunctionSubscribe members): one list, so the clears at Stop
// (Engine::clearComponentSubscriptions) and on tag removal (Scene::removeSubscriptionsByTag)
// cannot forget one. Each component lists its own events in forEachEvent;
// tools/check_event_clears.py fails when a member or a component is missing.

#include "registry/EntityRegistry.h"

#include "component/ActionComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/AudioSourceComponent.h"
#include "component/ButtonComponent.h"
#include "component/InputFieldComponent.h"
#include "component/NavMeshAgentComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "component/OnScreenControlComponents.h"
#include "component/PanelComponent.h"
#include "component/ParticleSystemComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/ScrollbarComponent.h"
#include "component/SplineAnimateComponent.h"
#include "component/TextEffectComponent.h"
#include "component/UIComponent.h"
#include "component/UIDocumentComponent.h"
#include "component/VertexAnimatorComponent.h"
#include "component/VideoPlayerComponent.h"

#include <cstddef>

namespace jasperengine {

    template<typename... Components>
    struct ComponentTypeList {};

    using ComponentsWithEvents = ComponentTypeList<
        ActionComponent,
        AnimationPlayerComponent,
        AnimatorComponent,
        AudioSourceComponent,
        ButtonComponent,
        InputFieldComponent,
        NavMeshAgentComponent,
        NavMeshSurfaceComponent,
        OnScreenStickComponent,
        OnScreenButtonComponent,
        TouchAreaComponent,
        PanelComponent,
        ParticleSystemComponent,
        PoseAnimatorComponent,
        ScrollbarComponent,
        SplineAnimateComponent,
        TextEffectComponent,
        UIComponent,
        UIDocumentComponent,
        VertexAnimatorComponent,
        VideoPlayerComponent>;

    namespace detail {
        template<typename Component, typename F>
        void forEachEventOf(EntityRegistry& registry, F& visit) {
            auto components = registry.getComponentArray<Component>();
            for (size_t i = 0; i < components->size(); i++) {
                components->getComponentFromIndex(i).forEachEvent(visit);
            }
        }

        template<typename... Components, typename F>
        void forEachEventIn(EntityRegistry& registry, ComponentTypeList<Components...>, F& visit) {
            (forEachEventOf<Components>(registry, visit), ...);
        }
    }

    // visit(event) for every event of every component of the registry (a scene)
    template<typename F>
    void forEachComponentEvent(EntityRegistry& registry, F&& visit) {
        detail::forEachEventIn(registry, ComponentsWithEvents{}, visit);
    }

}
