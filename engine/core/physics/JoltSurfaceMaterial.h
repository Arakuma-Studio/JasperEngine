// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The surface of one collider of a 3D body as Jolt sees it (physics-3d 6.1.4): one per collider,
// set on its leaf shape (ConvexShape::SetMaterial, the mesh and heightfield material lists), so
// the contact listener and the combine functions find it from a contact's sub-shape id.
// - friction / restitution / the combine modes: the collider's resolved PhysicsMaterial (its own,
//   else the body's); NONE modes = no material, the body's legacy friction and restitution apply.
// - isTrigger: contacts with this collider are sensor contacts (Unity Collider.isTrigger).
// - Written only on the main thread between steps; Jolt's job threads read it during a step.

#ifndef JOLT_SURFACE_MATERIAL_H
#define JOLT_SURFACE_MATERIAL_H

#ifdef JASPERENGINE_PHYSICS_3D

#include "component/PhysicsCommon.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Collision/PhysicsMaterial.h"

#include <cstdint>

namespace jasperengine{

    class JoltSurfaceMaterial : public JPH::PhysicsMaterial{
    public:
        // its debug name, by address: what tells it from Jolt's own materials (one virtual call,
        // no JPH RTTI to register)
        static inline const char* const typeName = "JoltSurfaceMaterial";

        float friction = 0.0f;
        float restitution = 0.0f;
        PhysicsMaterialCombine frictionCombine = PhysicsMaterialCombine::NONE;
        PhysicsMaterialCombine bounceCombine = PhysicsMaterialCombine::NONE;
        bool isTrigger = false;
        uint32_t colliderIndex = 0;

        virtual const char* GetDebugName() const override { return typeName; }

        bool hasMaterial() const { return frictionCombine != PhysicsMaterialCombine::NONE; }

        // the surface of a sub-shape of a body, null when the leaf has none of ours
        static const JoltSurfaceMaterial* of(const JPH::PhysicsMaterial* material){
            return (material && material->GetDebugName() == typeName) ? static_cast<const JoltSurfaceMaterial*>(material) : nullptr;
        }
    };

}

#endif

#endif //JOLT_SURFACE_MATERIAL_H
