// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PARTICLES_PARTICLE_FIELDS_H
#define PARTICLES_PARTICLE_FIELDS_H

// Every field of the version 2 modules by name ("main.startLifetime", "shape.radius", ...):
// the one list the scene encoding (editor/Stream), the exported-build code (editor/Factory),
// the editor's properties (editor/Catalog: the Inspector, MCP set_properties / get_entity,
// undo, prefab overrides) and the Lua module proxies (ps.main, ps.emission, ...) walk, so
// they all name and type a field the same way (particles spec 6.2, 6.6, 6.7).

#include "Export.h"
#include "component/ParticleSystemComponent.h"

#include <string>
#include <vector>

namespace jasperengine {
namespace particles {

    enum class FieldType {
        Bool,
        Int,
        UInt,      // unsigned int (main.maxParticles, main.randomSeed)
        Float,
        Vector2,
        Vector3,
        Curve,     // MinMaxCurve
        Gradient,  // MinMaxGradient
        Enum,      // an int-sized enum, `enumNames` by value
        Bursts,    // std::vector<ParticleEmissionBurst>
        IntList,   // std::vector<int>
        String
    };

    struct JASPERENGINE_API FieldInfo {
        const char* module;   // "main", "emission", ..., "renderer"
        const char* name;     // the Unity (lowerCamel) name in the module
        FieldType type;
        void* (*ref)(ParticleSystemComponent&);
        // Enum: the value names, by value
        const std::vector<std::string>* enumNames = nullptr;
        // a change rebuilds the renderer (the capacity, the render mode)
        bool rebuild = false;
        // the rad/deg of a Curve or Float shown in degrees by the Inspector (stored in radians)
        bool radians = false;
        // Enum: the C++ enum type (the exported-build code names it)
        const char* enumType = nullptr;

        std::string path() const { return std::string(module) + "." + name; }
        void* get(ParticleSystemComponent& component) const { return ref(component); }
        const void* get(const ParticleSystemComponent& component) const { return ref(const_cast<ParticleSystemComponent&>(component)); }
    };

    // the modules in the Inspector's (Unity's) order
    JASPERENGINE_API const std::vector<std::string>& moduleNames();
    // every field, module by module
    JASPERENGINE_API const std::vector<FieldInfo>& fields();
    // "module.name", nullptr when unknown
    JASPERENGINE_API const FieldInfo* findField(const std::string& path);
    // the module's fields
    JASPERENGINE_API std::vector<const FieldInfo*> moduleFields(const std::string& module);
    // whether the module has an enabled checkbox (all but main and renderer)
    JASPERENGINE_API bool moduleHasEnabled(const std::string& module);

    // the name of an enum value of a field ("" out of range) and the value of a name (-1)
    JASPERENGINE_API std::string enumName(const FieldInfo& field, int value);
    JASPERENGINE_API int enumValue(const FieldInfo& field, const std::string& name);

    // a field's value equals the default component's
    JASPERENGINE_API bool isDefault(const FieldInfo& field, const ParticleSystemComponent& component);
    // copies the field from one component to another
    JASPERENGINE_API void copyField(const FieldInfo& field, const ParticleSystemComponent& from, ParticleSystemComponent& to);

}
}

#endif //PARTICLES_PARTICLE_FIELDS_H
