// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TITLE_ART_RENDERER_COMPONENT_H
#define TITLE_ART_RENDERER_COMPONENT_H

// The Title Art Renderer (docs/specs/S81-title-art-runtime.md R1-R5, §6.1): shows a `.titleart`
// through a companion on the same GameObject, an Image (UI space) or a Sprite (world space). It
// adds no renderer of its own: TitleArtSystem lends the companion its image at draw time (the
// FluidSystem pattern, so the companion's saved texture never changes) and, while a Live Effect
// is on, gives it `library:title_fx` (UI) or `library:title_fx_sprite` (world).
//
// Always built (core): a Baked title needs nothing of the `titleart` module. `runtime` is the
// system's per-instance state (TitleArtInstance.h), never saved.

#include "Export.h"
#include "asset/AssetRef.h"
#include "math/Vector4.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    namespace titleart{
        class TitleArtInstance;
    }

    // What changing the title costs (R7, R9, R25). Stored and sent as ints: append only
    enum class TitleArtRenderMode : uint8_t { BAKED, REALTIME, GLYPH_ATLAS };
    // How a Realtime image replaces the previous one (R17)
    enum class TitleArtTransition : uint8_t { CUT, CROSSFADE };

    // One Exposed parameter's instance value (S80 §6.1 types); the property "overrides.<name>"
    struct JASPERENGINE_API TitleArtOverride{
        std::string name;                // the exposed name
        std::string type;                // "text" "color" "number" "bool" "font" "image" "color_scheme"
        Vector4 value = Vector4(0.0f, 0.0f, 0.0f, 0.0f);    // number: x; bool: x 0/1; color: linear rgb
        std::string text;                // text; font / image: asset path; color_scheme: scheme id

        bool operator==(const TitleArtOverride& other) const{
            return name == other.name && type == other.type && value == other.value && text == other.text;
        }
        bool operator!=(const TitleArtOverride& other) const{ return !(*this == other); }
    };

    struct JASPERENGINE_API TitleArtRendererComponent{
        bool enabled = true;
        AssetRef title;                  // the .titleart, project-relative (GUID tracked by the editor)
        TitleArtRenderMode renderMode = TitleArtRenderMode::BAKED;
        std::vector<TitleArtOverride> overrides;   // "overrides.<name>" properties
        // S87 R12: the language this title shows ("" = Auto: TitleArt.setLanguage, the device, the
        // project default, the document's source); a language the title lacks resolves to its best match
        std::string language;

        // size and placement
        bool pivotFromTitle = true;
        float uiScale = 1.0f;            // Set Native Size, UI
        float pixelsPerUnit = 0.0f;      // Set Native Size, world; 0 = the project's Pixels Per Unit, else 100

        // Realtime and Glyph Atlas
        float resolutionScale = 1.0f;    // 0.25..2 of the display size
        int priority = 0;                // -100..100
        bool renderWhenHidden = false;
        bool prebakeInitialState = true;
        TitleArtTransition transition = TitleArtTransition::CUT;
        float transitionDuration = 0.15f;    // 0..2 s
        std::string additionalCharacters;
        int atlasPageSize = 1024;        // 512 | 1024 | 2048
        int atlasMaxPages = 4;           // 1..8

        // Realtime 3D (S81 owns the fields; S86 R26, R37 define what they do)
        std::string qualityTier = "auto";                // "auto" | "high" | "medium" | "low"
        std::vector<std::string> runtimeEnvironments;    // extra S86 environment ids or assets the export cooks

        // Live Effects (R33-R38): drawn by the companion's one draw through title_fx
        float liveTime = 0.0f;
        bool liveUnscaledTime = false;
        bool sweepEnabled = false;
        float sweepPosition = 0.0f;
        float sweepWidth = 0.15f;
        float sweepAngle = 25.0f;
        float sweepSoftness = 0.6f;
        Vector4 sweepColor = Vector4(1.0f, 1.0f, 1.0f, 0.8f);  // linear, alpha = strength
        bool sweepAuto = false;
        float sweepSpeed = 1.5f;
        float sweepInterval = 2.5f;
        bool glowEnabled = false;
        float glowPulse = 0.0f;
        Vector4 glowColor = Vector4(1.0f, 0.85f, 0.4f, 1.0f);
        bool tintEnabled = false;
        Vector4 tintColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        float tintAmount = 0.0f;
        bool glintsEnabled = false;
        float glintAmount = 1.0f;
        float glintSize = 0.06f;
        float glintRate = 0.5f;
        uint32_t glintSeed = 0;
        Vector4 glintColor = Vector4(1.0f, 1.0f, 1.0f, 1.0f);

        // S85's animations (docs/specs/S85-title-animation.md §6.5, R27)
        bool playAutomatically = true;   // play defaultAnimation (else the document's default) when enabled
        std::string defaultAnimation;    // a name, "" the document's `animation.default`
        float animationSpeed = 1.0f;     // 0..10, multiplies every animation
        bool animationUnscaledTime = false;
        std::string driveAnimation;      // a name: a clip or an Animator sets animationTime, the title shows that time
        float animationTime = 0.0f;      // the base animation's time (keyable; a write seeks)

        // runtime, never saved: the system's handle (state, hashes, the module's instance)
        std::shared_ptr<titleart::TitleArtInstance> runtime;
        // a field the system reads changed (the title, the mode, an override): resolve again
        bool needUpdate = true;
        // a clip track wrote sweepPosition this frame: sweepAuto stays off for it (R34)
        bool sweepPositionKeyed = false;
        // a clip track or a script wrote animationTime this frame: it seeks (S85 R27)
        bool animationTimeKeyed = false;
    };

}

#endif //TITLE_ART_RENDERER_COMPONENT_H
