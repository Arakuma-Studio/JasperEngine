---@meta _
-- Hand-written shapes of the spatial queries' Lua (engine/core/script/binding/PhysicsQueryLua.cpp),
-- appended to the generated stubs. Rules: docs/scripting/spatial-queries.md.

---The results table of Physics.overlapXNonAlloc, Physics2D.overlapXNonAlloc and
---MeshRaycast.overlapXNonAlloc: GameObject ids (integers, no garbage), 1..count; the entries past
---the count are cleared, so #results is the count. Turn an id into an object with
---GameObject(scene, id). A tree or detail found by MeshRaycast gives its Terrain's id (the array
---form tells which tree).
---@alias SpatialQueryResults integer[]

---The colliders table of a Physics / Physics2D NonAlloc overlap: colliders[i] is the index of
---results[i]'s collider on that GameObject (Collider.index of the array form's hit).
---@alias SpatialQueryColliders integer[]

---The options of MeshRaycast.overlapSphere / overlapBox (and their NonAlloc forms).
---@class MeshOverlapOptions
---@field kinds? integer # MeshRaycast.MESH | INSTANCE | BOUNDS | TREE (the default) | DETAIL | TERRAIN | UI
---@field includeInactive? boolean # also hidden, inactive and disabled objects (default false)
---@field ignore? (EntityHandle|integer)[] # objects the overlap passes through
---@field current? boolean # refit the bounds now instead of using the frame's first overlap's (default false)
