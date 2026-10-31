// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (physics-3d spec 6.1.4, shared with physics-2d)
// - Purpose: the PhysicsMaterial asset (`<name>.physicsmaterial`, Unity PhysicsMaterial /
//   PhysicsMaterial2D): friction, bounciness and how each combines with the other collider's.
//   The one parser and canonical writer (engine pool, editor, MCP, tests), and the runtime
//   material bodies and scripts share (PhysicsMaterialRuntime).
// - Format: UTF-8 JSON {"format": "jasperengine.physicsmaterial", "version": 1, "guid",
//   "friction", "bounciness", "frictionCombine", "bounceCombine", unknown keys...}; combine
//   strings average | geometric_mean | minimum | multiply | maximum.
// - Parse: fails (error set) only for text that is not a JSON object, a different `format` or a
//   newer `version`. Lenient otherwise: friction < 0 -> 0, bounciness clamped to 0..1, an
//   unknown combine -> average, each with a warning; Unity names are accepted (dynamicFriction ->
//   friction, bounce -> bounciness, frictionCombine/bounceCombine "Average"/"Minimum"/...).
//   Missing keys take the defaults (friction 0.6, bounciness 0, average / average).
// - Write: deterministic, 2-space indent, keys in the Format order, numbers %.6g, unknown keys
//   kept (after the known ones, in their order). write(parse(write(d))) == write(d).
// - Thread-safety: the functions are pure. A PhysicsMaterialRuntime is edited on the main thread
//   only (Lua, the editor); physics reads it on the main thread between steps.

#ifndef PHYSICS_MATERIAL_H
#define PHYSICS_MATERIAL_H

#include "Export.h"
#include "component/PhysicsCommon.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    constexpr const char* PHYSICS_MATERIAL_FORMAT = "jasperengine.physicsmaterial";
    constexpr int PHYSICS_MATERIAL_VERSION = 1;
    constexpr const char* PHYSICS_MATERIAL_EXTENSION = ".physicsmaterial";

    struct JASPERENGINE_API PhysicsMaterialData{
        std::string guid;
        float friction = 0.6f;
        float bounciness = 0.0f;
        PhysicsMaterialCombine frictionCombine = PhysicsMaterialCombine::AVERAGE;
        PhysicsMaterialCombine bounceCombine = PhysicsMaterialCombine::AVERAGE;
        // keys this version does not know, as a compact JSON object ("" = none), written back
        std::string unknownKeys;

        // "Create > Physics Material": friction 0.6, average / average (Unity 3D)
        static PhysicsMaterialData preset3D();
        // "Create > Physics Material 2D": friction 0.4, geometric mean / maximum (Unity 6 2D)
        static PhysicsMaterialData preset2D();

        bool operator==(const PhysicsMaterialData& other) const;
        bool operator!=(const PhysicsMaterialData& other) const { return !(*this == other); }
    };

    struct JASPERENGINE_API PhysicsMaterialParseResult{
        bool ok = false;
        PhysicsMaterialData data;
        std::vector<std::string> warnings;
        std::string error;
    };

    namespace physicsmaterial{
        JASPERENGINE_API PhysicsMaterialParseResult parse(const std::string& text);
        JASPERENGINE_API std::string write(const PhysicsMaterialData& data);
        JASPERENGINE_API std::string generateGuid();

        // "average", "geometric_mean", ... ("" for NONE)
        JASPERENGINE_API const char* combineName(PhysicsMaterialCombine mode);
        // accepts the names above and Unity's ("Average", "GeometricMean", "Minimum", ...);
        // false when unknown
        JASPERENGINE_API bool combineFromName(const std::string& name, PhysicsMaterialCombine& mode);
    }

    // A material as bodies use it at run time: an asset loaded through PhysicsMaterialPool (path
    // set, shared by every collider that names it: Unity sharedMaterial) or one made by a script
    // (PhysicsMaterial.new, path ""). Every change bumps its generation and the global
    // revision, so the physics systems re-apply it before their next step.
    class JASPERENGINE_API PhysicsMaterialRuntime{
    private:
        std::string path;
        float friction = 0.6f;
        float bounciness = 0.0f;
        PhysicsMaterialCombine frictionCombine = PhysicsMaterialCombine::AVERAGE;
        PhysicsMaterialCombine bounceCombine = PhysicsMaterialCombine::AVERAGE;
        uint64_t generation = 1;

        static std::atomic<uint64_t> globalRevision;

        void changed();

    public:
        PhysicsMaterialRuntime() = default;
        PhysicsMaterialRuntime(const PhysicsMaterialData& data, const std::string& path);

        const std::string& getPath() const { return path; }

        float getFriction() const { return friction; }
        void setFriction(float friction);
        float getBounciness() const { return bounciness; }
        void setBounciness(float bounciness);
        PhysicsMaterialCombine getFrictionCombine() const { return frictionCombine; }
        void setFrictionCombine(PhysicsMaterialCombine mode);
        PhysicsMaterialCombine getBounceCombine() const { return bounceCombine; }
        void setBounceCombine(PhysicsMaterialCombine mode);

        uint64_t getGeneration() const { return generation; }

        PhysicsMaterialData toData() const;

        // increases on any change of any runtime material and on pool invalidation (a cheap
        // "re-resolve materials?" for the physics systems)
        static uint64_t revision();
        static void bumpRevision();
    };

    // A script's handle on a runtime material (Lua PhysicsMaterial): PhysicsMaterial.load(path)
    // shares the asset's copy with every collider naming it, PhysicsMaterial.new makes one of
    // its own. Copies of a handle share the material. An empty handle (valid() false) is "none".
    class JASPERENGINE_API PhysicsMaterial{
    private:
        std::shared_ptr<PhysicsMaterialRuntime> runtime;

    public:
        PhysicsMaterial() = default;
        explicit PhysicsMaterial(std::shared_ptr<PhysicsMaterialRuntime> runtime);

        // the asset's shared runtime copy; empty (and an error logged) when it can't be read
        static PhysicsMaterial load(const std::string& path);
        static PhysicsMaterial create(float friction, float bounciness,
                                      PhysicsMaterialCombine frictionCombine = PhysicsMaterialCombine::AVERAGE,
                                      PhysicsMaterialCombine bounceCombine = PhysicsMaterialCombine::AVERAGE);

        bool valid() const { return (bool)runtime; }
        const std::shared_ptr<PhysicsMaterialRuntime>& getRuntime() const { return runtime; }

        // "" for one made by a script
        std::string getPath() const;
        float getFriction() const;
        void setFriction(float friction);
        float getBounciness() const;
        void setBounciness(float bounciness);
        PhysicsMaterialCombine getFrictionCombine() const;
        void setFrictionCombine(PhysicsMaterialCombine mode);
        PhysicsMaterialCombine getBounceCombine() const;
        void setBounceCombine(PhysicsMaterialCombine mode);

        bool operator==(const PhysicsMaterial& other) const { return runtime == other.runtime; }
    };

    // Bit layout of the Box2D shape user material id (physics-2d 6.1.6): bits 0-3 the friction
    // mode, bits 4-7 the bounce mode; 0 = no material (legacy combine)
    inline int encodeSurfaceModes2D(PhysicsMaterialCombine friction, PhysicsMaterialCombine bounce){
        return ((int)friction & 0xF) | (((int)bounce & 0xF) << 4);
    }
    inline PhysicsMaterialCombine frictionModeOf2D(int userMaterialId){
        return (PhysicsMaterialCombine)(userMaterialId & 0xF);
    }
    inline PhysicsMaterialCombine bounceModeOf2D(int userMaterialId){
        return (PhysicsMaterialCombine)((userMaterialId >> 4) & 0xF);
    }

}

#endif //PHYSICS_MATERIAL_H
