// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim-properties spec 5-8)
// - Purpose: the engine's vocabulary of animatable properties, the one binding mechanism of clip
//   property tracks. A track names (component, property) with the editor's Catalog names (the
//   ones set_properties uses): `component` is the Catalog component key ("ui", "light",
//   "recttransform"...), or a pseudo component: "gameobject" (property "active") or
//   "script:<Class>" (a declared field of a Lua script's `properties`). `property` is the
//   Catalog property name, optionally with one swizzle suffix (".r .g .b .a" on colours,
//   ".x .y .z .w" on vectors). The registry rows hold the kind, the key display transform and
//   the read / write functions; exported builds use it alone (no editor code).
// - Thread-safety: parsePropertyRef, findAnimatableProperty, animatableProperties and the
//   conversions are pure (any thread: the clip loader parses on worker threads). Everything
//   taking a Scene reads or writes it: the main thread; internPropertyKey is main thread only.
//   The one exception: the Animator's wave runs a row marked workerSafe on a worker, on an entity
//   no other Animator writes (AnimatorSystem.h, S74 R8).
// - Invariants:
//   * read / write work on stored values (linear colours, cone cosines); the key-space helpers
//     (readPropertyKeySpace, writePropertyKeySpace) apply the display transform:
//     keyToStored(storedToKey(v)) == v within 1e-5 for colours in [0, 1] (P9).
//   * writes set the runtime dirty flags the engine's own Lua setters set, and do nothing when
//     the stored value is unchanged (callers compare first); `deferred` rows (gameobject.active,
//     a script's enabled) run user callbacks (onEnable / onDisable) and are applied after every
//     other write, by the ActionSystem flush and the Animator's finalize.
//   * a row's has() is the component test (bindings report missing_component when false).

#ifndef PROPERTY_BINDINGS_H
#define PROPERTY_BINDINGS_H

#include "Export.h"
#include "animation/ClipTypes.h"
#include "ecs/Entity.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine{
    class Scene;
    class EntityRegistry;
}

namespace jasperengine::anim{

    // what a parametrized row is addressed by: a literal submesh index ("submeshes[N]..."), a
    // shader member ("...shaderParams.<member>", "shaderUniforms.<member>"), a script field
    struct JASPERENGINE_API PropertyParams{
        int submesh = -1;
        std::string member;         // shader member, or the script field
        std::string scriptClass;    // script:<Class>
    };

    struct JASPERENGINE_API AnimatableProperty{
        const char* component;      // Catalog key ("ui"), "gameobject", "script"
        // the Catalog property name; parametrized rows use "submeshes[N]" and "<member>"
        // ("submeshes[N].material.baseColorFactor", "shaderUniforms.<member>")
        const char* property;
        const char* label;          // the Inspector's label ("Color", "Y FOV", "Anchored Position")
        // the Unity binding name it ports ("m_Color", "m_IsActive"), "" when none, with the value
        // conversion when there is one (the UI's layout space is y-down: "m_AnchorMax.y (1 - y)")
        const char* unityName;
        PropertyKind kind;          // None: from the keys or the bound shader/script (members, fields)
        uint8_t width;              // floats of the whole value (0 with kind None)
        bool discrete;              // stepped, highest weight wins (bool, int, frame)
        bool interpolateInt;        // an Int that interpolates and rounds (UI sizes, offsets)
        PropertyDisplay display;
        bool deferred;              // applied after every other write (runs user callbacks)
        // the entity has what the row writes (component, submesh, shader member)
        bool (*has)(EntityRegistry* registry, Entity entity, const PropertyParams& params);
        // stored values, `width` floats (the bound width for kind None rows)
        bool (*read)(Scene* scene, Entity entity, const PropertyParams& params, float* out);
        bool (*write)(Scene* scene, Entity entity, const PropertyParams& params, const float* in);
        // optional: state a write changes besides the value (a sprite's texture rect, an anchor
        // preset), saved with the rest and put back when a preview ends (4 floats)
        bool (*snapshot)(Scene* scene, Entity entity, const PropertyParams& params, float* out);
        bool (*restore)(Scene* scene, Entity entity, const PropertyParams& params, const float* in);
        // read and write touch nothing but the entity's own components (no Lua, physics world,
        // asset load or cache): the Animator may run them on a worker. Off by default: a new row
        // stays on the main thread until someone has read what it reaches
        bool workerSafe = false;
    };

    // every registry row, in listing order (Add Property, list_properties)
    JASPERENGINE_API const std::vector<AnimatableProperty>& animatableProperties();
    // the row of (component, property) without a swizzle; parametrized rows match their
    // pattern ("submeshes[0].material.baseColorFactor"); null for script fields and unknown names
    JASPERENGINE_API const AnimatableProperty* findAnimatableProperty(const std::string& component, const std::string& property);

    // (component, property) as a track names it, resolved without a scene
    struct JASPERENGINE_API PropertyRef{
        bool known = false;                     // a registry row or a script field
        const AnimatableProperty* entry = nullptr;  // the row (null for script fields)
        bool script = false;
        PropertyParams params;
        std::string base;                       // the property without its swizzle
        int swizzle = -1;                       // 0..3: the component the track keys alone
        PropertyKind kind = PropertyKind::None; // of the whole property (None: from keys / bind)
        uint8_t width = 0;                      // of the whole property (0 with kind None)
        bool discrete = false;
        bool interpolateInt = false;
        PropertyDisplay display = PropertyDisplay::Identity;
        bool deferred = false;
        // unknown: "unknown animatable property 'light.fooBar' (did you mean 'intensity'?)"
        std::string error;
    };
    JASPERENGINE_API bool parsePropertyRef(const std::string& component, const std::string& property, PropertyRef& out);

    // kind helpers
    JASPERENGINE_API uint8_t kindWidth(PropertyKind kind);
    JASPERENGINE_API bool kindIsDiscrete(PropertyKind kind);
    JASPERENGINE_API const char* kindName(PropertyKind kind);   // "bool", "int", "frame", "float", "vector2"... "color4"
    // a swizzle letter's component for a kind: rgba on colours, xyzw on vectors (-1: none)
    JASPERENGINE_API int swizzleIndex(PropertyKind kind, char letter);

    // key space <-> stored values of a whole property (`width` floats, in place)
    JASPERENGINE_API void keyToStored(PropertyKind kind, PropertyDisplay display, bool roundInt, float* values, size_t width);
    JASPERENGINE_API void storedToKey(PropertyKind kind, PropertyDisplay display, float* values, size_t width);

    // "#RRGGBB" / "#RRGGBBAA" as 3 or 4 floats in [0, 1] (false when malformed)
    JASPERENGINE_API bool parseHexColor(const std::string& text, std::vector<float>& out);

    // ---- a property bound on an entity (players, previews, Record, MCP) ----

    enum class PropertyBindStatus : uint8_t {
        Bound,
        MissingComponent,       // the component, submesh, shader member or script field is not there
        UnknownProperty         // no such row / field
    };

    struct JASPERENGINE_API PropertyBinding{
        PropertyBindStatus status = PropertyBindStatus::UnknownProperty;
        uint32_t key = 0;                       // internPropertyKey of the whole property (0: unbound)
        const AnimatableProperty* entry = nullptr;  // null: a script field
        PropertyKind kind = PropertyKind::None; // of the whole property, as bound
        uint8_t width = 0;
        int8_t swizzle = -1;
        bool discrete = false;
        bool interpolateInt = false;
        PropertyDisplay display = PropertyDisplay::Identity;
        bool deferred = false;
        // a script field: the script picked at bind (the first enabled one of the class, else
        // the first) and its instance then (the bind stamp: another instance, a reload, binds
        // again); reads and writes fail when that index holds another class
        int16_t scriptIndex = -1;
        const void* scriptInstance = nullptr;
        uint64_t scriptGeneration = 0;          // ScriptEntry::instanceGeneration then
        std::string message;                    // why it did not bind (one line)
    };
    // keys of `keyWidth` values on this binding give it an opaque alpha: a whole Color4 keyed
    // with RGB values ("#RRGGBB", 3 numbers; spec 5.4)
    inline bool keysOpaqueAlpha(const PropertyBinding& binding, size_t keyWidth){
        return binding.swizzle < 0 && binding.kind == PropertyKind::Color4 && keyWidth == 3;
    }
    // the Animator's wave may run this binding on a worker (script fields have no row: never)
    inline bool propertyWorkerSafe(const PropertyBinding& binding){
        return binding.entry && binding.entry->workerSafe && !binding.deferred;
    }
    // a script binding's script is not the instance it bound (reloaded, removed, reordered):
    // the binding should be made again (spec P4). False for other bindings
    JASPERENGINE_API bool scriptBindingStale(EntityRegistry* registry, Entity entity, const PropertyBinding& binding);

    // The parse of a property as bound on `entity`: the row's has(), a script's declared field
    // (its declared type wins over the keys': an Int rounds and steps, a Color keys sRGB), a
    // shader member's manifest type when the shader declares one. `keyWidth` is the track's
    // key width (shader members and script fields without a declaration take it).
    // `preferredScriptIndex`: a script field binding made again keeps that script while it is
    // still a Lua script of the class (the previous binding's scriptIndex; -1 picks the first
    // enabled one, else the first) (any registry: a scene, a prefab's; main thread)
    JASPERENGINE_API PropertyBinding bindProperty(EntityRegistry* registry, Entity entity, const PropertyRef& ref, size_t keyWidth,
                                                  int preferredScriptIndex = -1);

    // The interned id of a property's identity (component and base property: a swizzle track
    // and the whole-property track of the same property share it), never 0. Main thread.
    JASPERENGINE_API uint32_t internPropertyKey(const std::string& component, const PropertyRef& ref);
    struct JASPERENGINE_API PropertyKeyInfo{
        std::string component;
        std::string property;                   // base, no swizzle
        PropertyRef ref;                        // parsed without a swizzle
    };
    // the identity of an interned key (null for 0 or unknown ids). Main thread.
    JASPERENGINE_API const PropertyKeyInfo* propertyKeyInfo(uint32_t key);

    // Stored-value read / write of a bound whole property (`binding.width` floats): the row's
    // functions, or a Lua script field (LuaBinding::get/setLuaScriptProperty)
    JASPERENGINE_API bool readPropertyStored(Scene* scene, Entity entity, const PropertyBinding& binding, float* out);
    JASPERENGINE_API bool writePropertyStored(Scene* scene, Entity entity, const PropertyBinding& binding, const float* in);
    // the same in key space (display transform applied; ints rounded on write)
    JASPERENGINE_API bool readPropertyKeySpace(Scene* scene, Entity entity, const PropertyBinding& binding, float* out);
    JASPERENGINE_API bool writePropertyKeySpace(Scene* scene, Entity entity, const PropertyBinding& binding, const float* in);
    // the row's snapshot / restore (false when it has none)
    JASPERENGINE_API bool snapshotProperty(Scene* scene, Entity entity, const PropertyBinding& binding, float* out);
    JASPERENGINE_API bool restoreProperty(Scene* scene, Entity entity, const PropertyBinding& binding, const float* in);

    // ---- discovery (Add Property, MCP get_animation list_properties) ----

    struct JASPERENGINE_API AnimatablePropertyInfo{
        std::string component;                  // track component ("ui", "script:Torch")
        std::string property;                   // full name ("submeshes[0].material.baseColorFactor")
        std::string label;                      // "Color", "Material [0] : Base Color", "glow"
        PropertyKind kind = PropertyKind::Float;
        bool discrete = false;
    };
    // every property of `entity` a track can animate: GameObject Active, the enabled checkbox and
    // the rows of each component it has (submeshes expanded; shader members of a manifest), the
    // declared animatable fields (and enabled) of its Lua scripts. Main thread.
    JASPERENGINE_API std::vector<AnimatablePropertyInfo> listAnimatableProperties(Scene* scene, Entity entity);

}

#endif //PROPERTY_BINDINGS_H
