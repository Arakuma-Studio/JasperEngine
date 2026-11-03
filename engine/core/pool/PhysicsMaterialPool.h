// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (physics-3d spec 6.1.4, shared with physics-2d)
// - Purpose: the shared runtime copies of .physicsmaterial assets: one
//   shared_ptr<PhysicsMaterialRuntime> per normalized path (FileData::getSystemPath, lexically
//   normal, generic separators, as AnimationClipPool), read through Data so resource packs work.
//   Every collider and script naming the path shares it (Unity sharedMaterial): a script's edit
//   of it is seen by all of them until the pool reloads the file (invalidate, clear).
// - Failures (missing file, parse error) are logged once per path and revision and not cached:
//   getOrLoad returns null and tries again after the next invalidate.
// - invalidate (the editor saved the file) drops the entry; the next getOrLoad reads the file
//   again and the global PhysicsMaterialRuntime revision changes, so bodies using it re-resolve
//   their colliders at their next step.
// - Engine::clearPools / clearUnusedPools clear it with the other pools.
// - Thread-safety: every function takes the pool mutex.

#ifndef PHYSICS_MATERIAL_POOL_H
#define PHYSICS_MATERIAL_POOL_H

#include "Export.h"
#include "physics/PhysicsMaterial.h"

#include <memory>
#include <string>

namespace jasperengine{

    class JASPERENGINE_API PhysicsMaterialPool{
    public:
        static std::string getKey(const std::string& path);

        // the cached material, null when not loaded (never loads)
        static std::shared_ptr<PhysicsMaterialRuntime> get(const std::string& path);
        // the cached material, else loads and caches it; null + `error` on failure
        static std::shared_ptr<PhysicsMaterialRuntime> getOrLoad(const std::string& path, std::string* error = nullptr);

        static void invalidate(const std::string& path);
        static void clear();
        static void clearUnused();
        static size_t size();
    };

}

#endif //PHYSICS_MATERIAL_POOL_H
