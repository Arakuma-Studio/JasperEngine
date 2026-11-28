// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ATMOSPHERE_DIAGNOSTICS_H
#define ATMOSPHERE_DIAGNOSTICS_H

#include "Export.h"
#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{

    class Scene;
    struct AtmosphereInfo;

    enum class AtmosphereSeverity : uint8_t { Info, Warning, Error };

    // Why the fog of a scene shows less (or more) than its author expects (docs/specs/S101-vapor.md
    // R17): a stable code, a sentence naming what was found and one naming the fix. The same
    // strings are the inspector's help boxes and get_atmosphere_info's diagnostics.
    struct AtmosphereDiagnostic{
        std::string code;
        AtmosphereSeverity severity = AtmosphereSeverity::Warning;
        std::string message;
        // the GameObject it is about (a Fog, a Local Volumetric Fog, a Light, a Camera);
        // NULL_ENTITY: the scene
        Entity entity = NULL_ENTITY;
        std::string fix;
    };

    // How much of the scene the analytic fog hides along the main camera's forward ray at 10,
    // 50 and 200 m (R18), of the look that camera sees (the Fog with its profile, a blend, the
    // Fog Zones and runtime writes: FogScene::resolve): exact for the analytic fog; Local
    // Volumetric Fog and the noise are not included
    struct AtmosphereOpacity{
        bool valid = false;        // the scene has a Fog and a main camera
        float at10 = 0.0f;
        float at50 = 0.0f;
        float at200 = 0.0f;
        // 200 m along a level ray at the Height fog's Base Height under the camera (its densest
        // part; the forward value for a distance fog): fog_invisible needs both thin, so a camera
        // above a ground layer is not told the fog cannot be seen
        float levelAt200 = 0.0f;
    };

    struct AtmosphereDiagnosticsInput{
        Scene* scene = nullptr;
        // what the view ran last frame (RenderSystem::getAtmosphereInfo); null, or a record of
        // no frame, skips the checks that need a frame
        const AtmosphereInfo* info = nullptr;
        // the editor's Scene view draws without fog (its Disable fog display setting)
        bool sceneViewFogDisabled = false;
        // the game's main camera (NULL_ENTITY: the scene's camera; the editor's Game view draws
        // through the project's game camera, not the Scene view's)
        Entity camera = NULL_ENTITY;
    };

    struct AtmosphereReport{
        std::vector<AtmosphereDiagnostic> diagnostics;
        AtmosphereOpacity opacity;
    };

    // Runs only when asked (an open inspector, an MCP call): never per frame in a player (R21).
    // The checks of the look read it as the main camera sees it (FogScene::resolve at its
    // position), so a profile, a blend or a zone counts.
    //
    // Adding a code: write a check in AtmosphereDiagnostics.cpp (void(AtmosphereCheckContext&); it
    // appends to context.out) and add it to ATMOSPHERE_CHECKS there, in the order its diagnostics
    // should read. What a check needs beyond the scene and the view's AtmosphereInfo (a Fog Profile's
    // resolved look, the zones' weights) goes into AtmosphereDiagnosticsInput as an optional
    // field (null or empty: the check adds nothing), so callers that do not fill it keep working.
    class JASPERENGINE_API AtmosphereDiagnostics{
    public:
        static constexpr float INVISIBLE_OPACITY = 0.02f;   // fog_invisible: below this at 200 m
        static constexpr float OPAQUE_OPACITY = 0.95f;      // fog_opaque: above this at 10 m

        static AtmosphereReport run(const AtmosphereDiagnosticsInput& input);
        // along the forward ray of camera (NULL_ENTITY: the scene's camera)
        static AtmosphereOpacity estimateOpacity(Scene& scene, Entity camera = NULL_ENTITY);
        // "info", "warning", "error"
        static const char* severityName(AtmosphereSeverity severity);
    };

}

#endif // ATMOSPHERE_DIAGNOSTICS_H
