// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Canvas Group side of the clip property channel (ui canvas group spec 7.9, anim-properties
// spec 7): the read and write functions of the `canvasgroup` rows of the animatable property
// registry, in its `bool (*)(Scene*, Entity, float*)` shape, so PropertyBindings.cpp lists them:
//   canvasgroup.alpha               Float, continuous (Unity m_Alpha), clamped to [0, 1] on write
//   canvasgroup.interactable        Bool, discrete (m_Interactable)
//   canvasgroup.blocksRaycasts      Bool, discrete (m_BlocksRaycasts)
//   canvasgroup.ignoreParentGroups  Bool, discrete (m_IgnoreParentGroups)
//   canvasgroup.enabled             Bool, discrete (the component checkbox: hasEnabledProperty)
// The writes are plain field writes: UISystem finds the change by comparing with what it saw
// (no update flag), the same frame (its draw sync runs after the clips' flush).

#ifndef CANVAS_GROUP_TRACKS_H
#define CANVAS_GROUP_TRACKS_H

#include "Scene.h"
#include "component/CanvasGroupComponent.h"

#include <algorithm>
#include <cmath>

namespace jasperengine::anim {

    inline bool readCanvasGroupAlpha(Scene* scene, Entity entity, float* out){
        const CanvasGroupComponent* group = scene ? scene->findComponent<CanvasGroupComponent>(entity) : nullptr;
        if (!group) return false;
        out[0] = group->alpha;
        return true;
    }

    // clamped to [0, 1]; a NaN is not written (false)
    inline bool writeCanvasGroupAlpha(Scene* scene, Entity entity, const float* in){
        CanvasGroupComponent* group = scene ? scene->findComponent<CanvasGroupComponent>(entity) : nullptr;
        if (!group || std::isnan(in[0])) return false;
        group->alpha = std::clamp(in[0], 0.0f, 1.0f);
        return true;
    }

    namespace detail {
        template<bool CanvasGroupComponent::*Field>
        bool readCanvasGroupFlag(Scene* scene, Entity entity, float* out){
            const CanvasGroupComponent* group = scene ? scene->findComponent<CanvasGroupComponent>(entity) : nullptr;
            if (!group) return false;
            out[0] = (group->*Field) ? 1.0f : 0.0f;
            return true;
        }

        // a bool key is 0 or 1; anything above 0.5 is true
        template<bool CanvasGroupComponent::*Field>
        bool writeCanvasGroupFlag(Scene* scene, Entity entity, const float* in){
            CanvasGroupComponent* group = scene ? scene->findComponent<CanvasGroupComponent>(entity) : nullptr;
            if (!group || std::isnan(in[0])) return false;
            group->*Field = in[0] > 0.5f;
            return true;
        }
    }

    inline bool readCanvasGroupInteractable(Scene* scene, Entity entity, float* out){ return detail::readCanvasGroupFlag<&CanvasGroupComponent::interactable>(scene, entity, out); }
    inline bool writeCanvasGroupInteractable(Scene* scene, Entity entity, const float* in){ return detail::writeCanvasGroupFlag<&CanvasGroupComponent::interactable>(scene, entity, in); }
    inline bool readCanvasGroupBlocksRaycasts(Scene* scene, Entity entity, float* out){ return detail::readCanvasGroupFlag<&CanvasGroupComponent::blocksRaycasts>(scene, entity, out); }
    inline bool writeCanvasGroupBlocksRaycasts(Scene* scene, Entity entity, const float* in){ return detail::writeCanvasGroupFlag<&CanvasGroupComponent::blocksRaycasts>(scene, entity, in); }
    inline bool readCanvasGroupIgnoreParentGroups(Scene* scene, Entity entity, float* out){ return detail::readCanvasGroupFlag<&CanvasGroupComponent::ignoreParentGroups>(scene, entity, out); }
    inline bool writeCanvasGroupIgnoreParentGroups(Scene* scene, Entity entity, const float* in){ return detail::writeCanvasGroupFlag<&CanvasGroupComponent::ignoreParentGroups>(scene, entity, in); }
    inline bool readCanvasGroupEnabled(Scene* scene, Entity entity, float* out){ return detail::readCanvasGroupFlag<&CanvasGroupComponent::enabled>(scene, entity, out); }
    inline bool writeCanvasGroupEnabled(Scene* scene, Entity entity, const float* in){ return detail::writeCanvasGroupFlag<&CanvasGroupComponent::enabled>(scene, entity, in); }

    // The rows, for the registry and its tests: Catalog property name, Bool (else Float), the
    // Unity binding name, read, write
    struct CanvasGroupTrack{
        const char* property;
        bool isBool;
        const char* unityName;
        bool (*read)(Scene*, Entity, float*);
        bool (*write)(Scene*, Entity, const float*);
    };

    inline const CanvasGroupTrack* canvasGroupTracks(size_t& count){
        static const CanvasGroupTrack tracks[] = {
            {"alpha", false, "m_Alpha", &readCanvasGroupAlpha, &writeCanvasGroupAlpha},
            {"interactable", true, "m_Interactable", &readCanvasGroupInteractable, &writeCanvasGroupInteractable},
            {"blocksRaycasts", true, "m_BlocksRaycasts", &readCanvasGroupBlocksRaycasts, &writeCanvasGroupBlocksRaycasts},
            {"ignoreParentGroups", true, "m_IgnoreParentGroups", &readCanvasGroupIgnoreParentGroups, &writeCanvasGroupIgnoreParentGroups},
            {"enabled", true, "m_Enabled", &readCanvasGroupEnabled, &writeCanvasGroupEnabled},
        };
        count = sizeof(tracks) / sizeof(tracks[0]);
        return tracks;
    }

}

#endif //CANVAS_GROUP_TRACKS_H
