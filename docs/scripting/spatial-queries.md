# Spatial queries: what is near, without a loop over every object

`Physics` and `Physics2D` answer "which colliders are here?" on the physics engines' own broad
phases (Jolt, Box2D): overlaps, checks and shape casts, beside `raycast`. Those are S96's
([docs/mcp/guide/physics.md](../mcp/guide/physics.md) "Shape casts and overlaps", the stubs'
`PhysicsQueryOptions`); this page adds the forms that make no garbage, what is drawn
(`MeshRaycast`, no collider needed) and how to see and check the queries
(docs/specs/spatial-queries.md).

## Choosing a query

| Question | Query |
|---|---|
| Every collider in a radius / box / capsule | `Physics.overlapSphere` / `overlapBox` / `overlapCapsule` (`Collider[]`) |
| The same every frame, no garbage | `Physics.overlapSphereNonAlloc` / `overlapBoxNonAlloc` / `overlapCapsuleNonAlloc` |
| Is anything there? (stops at the first) | `Physics.checkSphere` / `checkBox` / `checkCapsule` |
| What does a moving shape hit first? | `Physics.sphereCast` / `boxCast` / `capsuleCast` (`...All`: every hit) |
| 2D | `Physics2D.overlapPoint` / `overlapCircle` / `overlapBox` / `overlapCapsule` / `overlapArea` (`...All`, `...NonAlloc`), `circleCast` / `boxCast` / `capsuleCast` |
| What is drawn here (meshes, sprites, instances, trees), no collider | `MeshRaycast.overlapSphere` / `overlapBox` (`...NonAlloc`) |
| A Transform-only GameObject | give it a trigger collider: queries find colliders and drawn bounds only |

```lua
local ENEMY = LayerMask.getMask("Enemy")
local function nearbyEnemies(self)
    local near = Physics.overlapSphere(self.scene, self.go.position, 8, ENEMY)   -- Collider[]
    for i = 1, #near do
        print(near[i].gameObject.name, near[i].index, near[i].isTrigger)
    end
    local blocked = Physics.checkBox(self.scene, self.go.position, Vector3(1, 1, 1))
    local hit = Physics.sphereCast(self.scene, self.go.position, 0.3, Vector3(0, 0, -1), 20)
    if hit then print(hit.distance, hit.point, hit.normal, hit.gameObject) end
    return blocked
end
```

Triggers, an ignore list and the other rules are the options table every query takes last:
`{triggers = QueryTriggerInteraction.IGNORE, ignore = {self.go}}`. `USE_GLOBAL` (the default)
follows `Physics.queriesHitTriggers` (`Physics2D.queriesHitTriggers` for 2D), set in Project
Settings > Physics > Queries Hit Triggers (and Physics 2D > Queries Hit Triggers; on by default,
as raycasts always behaved) and changeable in Lua for the session. The editor puts the project's
values back when Play starts and stops; a player starts with them.

## No garbage: NonAlloc

`...NonAlloc` forms take Unity's arguments and a `results` table you keep. They write GameObject
ids (integers, no object per hit) into it, clear its entries past the count (`#results == count`
afterwards) and return the count. An optional `colliders` table after the options receives each
hit's collider index on that GameObject (the array form's `Collider.index`). Once the tables have
grown, a call makes no Lua garbage and no C++ allocation. The hits are the array form's, in its
order (GameObject, then collider index).

```lua
local function scan(self)
    self.hits = self.hits or {}
    self.colliders = self.colliders or {}
    local n = Physics.overlapSphereNonAlloc(self.scene, self.go.position, 8, self.hits, -1,
        {triggers = QueryTriggerInteraction.IGNORE}, self.colliders)
    for i = 1, n do
        local id, collider = self.hits[i], self.colliders[i]
        print(id, collider)
    end
    return n
end
```

A box's orientation follows `results`, as in Unity: `Physics.overlapBoxNonAlloc(scene, center,
halfExtents, results [, orientation [, layerMask [, options [, colliders]]]])`. In 2D the shape
comes first and `results` after it:

```lua
local function look2D(self, x, y)
    self.hits2D = self.hits2D or {}
    local n = Physics2D.overlapCapsuleNonAlloc(self.scene, Vector2(x, y), Vector2(20, 60),
        CapsuleDirection2D.VERTICAL, 0, self.hits2D)
    local m = Physics2D.overlapCircleNonAlloc(self.scene, Vector2(x, y), 48, self.hits2D)
    return n, m
end
```

The casts' `...All` forms allocate their result table; the closest-hit casts return one value.

## Drawn objects

```lua
local function visibleAround(self, p)
    local seen = MeshRaycast.overlapSphere(self.scene, p, 15, -1, {kinds = MeshRaycast.MESH | MeshRaycast.INSTANCE})
    self.drawn = self.drawn or {}
    local m = MeshRaycast.overlapBoxNonAlloc(self.scene, p, Vector3(4, 4, 4), Quaternion.IDENTITY, self.drawn)
    return #seen, m
end
```

`MeshOverlapHit`: `entity`, `kind` (a `MeshRaycast` bit), `instance` (-1 unless an instance),
`foliageId` and `prototype`. The default kinds are meshes, instances, bounds-only objects (points,
lines, particles) and a Terrain's trees; terrains and UI rects are left out (their bounds cover the
whole map or the screen), and details (grass) as rays leave them out, unless `kinds` asks. A tree is
one hit by its mesh's bounds, with its Terrain as `entity`, its id in `foliageId` (what
`Terrain:removeTreeInstance` takes) and its prototype index; a detail has `foliageId` 0 and its layer
in `prototype`. Only resident trees (within Tree Distance of the camera) and placed details are found.
A NonAlloc form gives the Terrain's id for each.

These are bounds, not triangles (`MeshRaycast.raycast` is the exact tool), ordered by (entity,
instance). They refit the drawn bounds at most once per engine frame: a second overlap in the same
frame sees the bounds of the first (or of a later rebuild). `{current = true}` refits now. Main
thread.

## Seeing and checking queries

- Game view toolbar > Queries (`Physics.debugDrawQueries`, MCP `configure_game_view
  {physics_debug_queries}`) draws each query of the main thread for 1 s, raycasts included: the
  shape, the cast sweep, hits as crosses; green with hits, red without. Queries from jobs are not
  drawn; off, nothing is recorded.
- The Stats panel shows `Physics queries: N (M hits)` for the last frame; `get_frame_times` has the
  `Physics queries` and `Physics query hits` monitors.
- MCP `query_physics` (group `physics`) runs any query in Play; `mode: verify` compares an overlap
  with a brute-force test of every body (`PhysicsQueryReference`) and lists missing and extra
  colliders; `dimension: drawn` asks `MeshRaycast`'s overlaps, in edit mode too.
