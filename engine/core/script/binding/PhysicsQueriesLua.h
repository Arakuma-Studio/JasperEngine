// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef PHYSICSQUERIESLUA_H
#define PHYSICSQUERIESLUA_H

// The options table of the Lua physics queries (PhysicsQueriesLua.cpp), shared with
// Physics.raycast / Physics2D.raycast (CoreClassesLua.cpp): docs/specs/S96-physics-gaps.md REQ-12.

#include "Entity.h"
#include "physics/PhysicsQueryOptions.h"

#include <vector>

typedef struct lua_State lua_State;

namespace jasperengine::luaphysics {

    // { ignore = {GameObject | Rigidbody | entity, ...}, triggers = QueryTriggerInteraction.*,
    //   hitFromInside (3D), hitBackfaces (3D), startInColliders (2D), maxHits } at `index`; nil
    // or none leaves the defaults. `ignore` holds the list options.ignore points to. Raises a
    // Lua error naming the bad field.
    void readQueryOptions(lua_State* L, int index, const char* function, bool is2D, QueryOptions& options, std::vector<Entity>& ignore);

}

#endif //PHYSICSQUERIESLUA_H
