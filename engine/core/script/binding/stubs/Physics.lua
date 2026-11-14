---@meta _
-- Hand-written: the options tables the physics queries read with the Lua C API
-- (PhysicsQueriesLua.cpp), which the generator does not read, and Unity's name of a hit.
-- docs/mcp/guide/physics.md "Shape casts and overlaps".

---Unity's RaycastHit: what Physics.raycast, the casts and sweepTest return (`hit`, `distance`,
---`point`, `normal`, `body`, `colliderIndex`, `collider`, `gameObject`, `rigidbody`, `transform`)
---@alias RaycastHit RayReturn

---The options of Physics.raycast and the 3D shape queries (all optional)
---@class PhysicsQueryOptions
---@field ignore? (GameObject|Rigidbody|EntityHandle|integer)[] # their bodies are skipped (a caster's own body, say)
---@field triggers? integer # QueryTriggerInteraction.USE_GLOBAL (Physics.queriesHitTriggers, true), IGNORE or COLLIDE
---@field hitFromInside? boolean # report a collider the ray or shape starts in, at distance 0 (Unity: not reported)
---@field hitBackfaces? boolean # mesh and heightfield triangles seen from behind (default Physics.queriesHitBackfaces)
---@field maxHits? integer # the most hits of an ...All query or an overlap (256)

---The options of Physics2D.raycast and the 2D shape queries (all optional)
---@class PhysicsQueryOptions2D
---@field ignore? (GameObject|Rigidbody2D|EntityHandle|integer)[] # their bodies are skipped
---@field triggers? integer # QueryTriggerInteraction.USE_GLOBAL (Physics2D.queriesHitTriggers, true), IGNORE or COLLIDE
---@field startInColliders? boolean # report a collider the ray or shape starts in, at distance 0 (default Physics2D.queriesStartInColliders, false)
---@field maxHits? integer # the most hits of an ...All query or an overlap (256)
