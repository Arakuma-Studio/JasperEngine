// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOG_RESOLVE_H
#define FOG_RESOLVE_H

// One view's fog look (docs/specs/S101-vapor.md R7-R11, R43). Layers apply in order, each for the
// fields it sets:
//   1. the Fog's own fields;
//   2. its linked profile (a runtime one replaces the saved one while set);
//   3. the scripted blend (blendTo);
//   4. Fog Zones, ascending priority, ties by entity id, each at its camera weight;
//   5. the runtime writes of Lua and animation in Play.
// The renderer reads only the result. With nothing to layer (FogScene::needsResolve false) no
// resolve runs and the Fog's own fields reach the renderer as they are.

#include "Export.h"
#include "render/FogProfile.h"
#include "component/FogZoneComponent.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace jasperengine{

    class Scene;
    struct Transform;

    enum class FogLayer : uint8_t{
        Fog,
        Profile,
        Blend,
        Zone,
        Runtime
    };

    // where a resolved field came from
    struct FogLayerSource{
        FogLayer layer = FogLayer::Fog;
        Entity zone = NULL_ENTITY;      // Zone: which

        bool operator==(const FogLayerSource& other) const { return layer == other.layer && zone == other.zone; }
    };
    // "fog", "profile", "blend", "zone:<entity>", "runtime" (MCP get_atmosphere_info's `resolved`)
    JASPERENGINE_API std::string fogLayerSourceName(const FogLayerSource& source);

    // a Fog Zone as the resolve reads it
    struct FogZoneSample{
        Entity entity = NULL_ENTITY;
        int priority = 0;
        float weight = 0.0f;                    // after R9's distance factor
        const FogProfileData* profile = nullptr;
    };

    // the scripted blend as the resolve reads it: from `from` (a whole look) to the Fog's own
    // fields under `target` (null: no profile), at t in 0..1
    struct FogBlendSample{
        const FogComponent* from = nullptr;
        const FogProfileData* target = nullptr;
        float t = 0.0f;
    };

    // the runtime writes layer (R43)
    struct FogRuntimeWrites{
        FogFieldSet set;
        FogComponent values;
    };

    struct FogResolve{
        // the Fog's fields with every layer applied (technique fields and the profile reference
        // are the Fog's own)
        FogComponent resolved;
        std::array<FogLayerSource, FOG_FIELD_COUNT> source;

        const FogLayerSource& sourceOf(FogField field) const { return source[static_cast<size_t>(field)]; }
    };

    // R7-R10 on given inputs, the same rules FogScene::resolve applies to a scene. Zones in any
    // order (sorted here by priority, then entity id); a zone of weight <= 0 or without a
    // profile does nothing. Allocates nothing for up to 64 zones.
    JASPERENGINE_API void resolveFog(const FogComponent& fog, const FogProfileData* profile, const FogBlendSample* blend,
                                     std::span<const FogZoneSample> zones, const FogRuntimeWrites* runtime, FogResolve& out);

    // R9: the camera's distance to the zone's shape (0 inside), and the zone's weight there
    JASPERENGINE_API float fogZoneDistance(const FogZoneComponent& zone, const Transform& transform, const Vector3& camera);
    JASPERENGINE_API float fogZoneWeight(const FogZoneComponent& zone, float distance);

    // What a running game layers on one Fog (FogComponent::runtime)
    struct FogRuntime{
        // Fog.profile set while the game runs, or where a blendTo ended: replaces the saved
        // profile while set ("" = none: the Fog's own fields)
        bool profileOverridden = false;
        std::string profileKey;

        // blendTo (R10): from `from` to the Fog's own fields under targetKey, over `duration`
        // seconds of game time
        bool blending = false;
        std::string targetKey;
        FogComponent from;
        double duration = 0.0;
        double elapsed = 0.0;

        // Lua and animation writes in Play (R43)
        FogRuntimeWrites writes;

        bool empty() const { return !profileOverridden && !blending && writes.set.none(); }
    };

    // a Fog Zone's state for one camera (get_atmosphere_info's `zones`)
    struct FogZoneState{
        Entity entity = NULL_ENTITY;
        int priority = 0;
        float distance = 0.0f;
        float weight = 0.0f;            // what it blends with: 0 without a readable profile
        std::string profileKey;         // as the zone names it ("" none)
        bool profileMissing = false;    // named, but missing or unreadable (R6)
    };

    struct FogBlendState{
        bool active = false;
        std::string targetKey;          // "" blends to the Fog's own fields
        double seconds = 0.0;
        double elapsed = 0.0;
        float progress = 0.0f;          // 0..1
    };

    // The resolve on a scene's data, the runtime layers, and their state for the editor, MCP and
    // Lua. Main thread only.
    class JASPERENGINE_API FogScene{
    public:
        // the scene's Fog: its first FogComponent (RenderSystem's), NULL_ENTITY when none
        static Entity findFog(Scene* scene);

        // anything to layer on the Fog: a profile, a blend, runtime writes or a Fog Zone (R11)
        static bool needsResolve(Scene* scene, const FogComponent& fog);
        // the look at a camera position; `zones` (optional): every active zone's state there
        static void resolve(Scene* scene, const FogComponent& fog, const Vector3& camera, FogResolve& out,
                            std::vector<FogZoneState>* zones = nullptr);
        // the look at the scene's camera (Lua's getResolved, the main camera of a game)
        static void resolveAtMainCamera(Scene* scene, const FogComponent& fog, FogResolve& out);
        static Vector3 mainCameraPosition(Scene* scene);
        // where a camera GameObject is, else the scene's camera (the editor reads it without the Transform header)
        static Vector3 cameraPosition(Scene* scene, Entity camera);
        // every active zone's state at a camera position, by priority then entity id
        static std::vector<FogZoneState> zoneStates(Scene* scene, const Vector3& camera);

        // the profile the Fog uses now: the runtime one while set, else the saved reference
        static std::string profileKey(const FogComponent& fog);
        static std::shared_ptr<const FogProfileData> profileOf(const FogComponent& fog);
        // the profile it uses sets the field (MCP set_properties refuses those, R43)
        static bool isFieldSetByProfile(const FogComponent& fog, FogField field);

        // the game runs the scene (Play in the editor, always in a game): Lua and animation
        // writes go to the runtime layer, Fog.profile to the runtime profile
        static bool isRunning(Scene* scene);

        // R10: cross-fade the base (own fields, profile, blend) from what it is now to the own
        // fields under `key` ("" none) over `seconds` of game time; <= 0 applies at once.
        // A blend running is re-targeted from its current mix.
        static void blendTo(FogComponent& fog, const std::string& key, double seconds);
        // the runtime profile, at once; cancels a blend
        static void setRuntimeProfile(FogComponent& fog, const std::string& key);
        static FogBlendState blendState(const FogComponent& fog);
        // every blend of the scene, dt seconds of game time on (RenderSystem::update)
        static void advance(Scene* scene, double dt);

        // the runtime writes (R43): the component a write of `field` goes to (the layer while
        // the scene runs, else the Fog itself), and the one a read comes from
        static FogComponent& writeTarget(Scene* scene, FogComponent& fog, FogField field);
        static const FogComponent& readSource(const FogComponent& fog, FogField field);
        static bool hasOverride(const FogComponent& fog, FogField field);
        static void clearOverride(FogComponent& fog, FogField field);
        static void clearOverrides(FogComponent& fog);

        // drops every Fog's runtime state of the scene (Play stopped)
        static void resetRuntime(Scene* scene);
    };

}

#endif // FOG_RESOLVE_H
