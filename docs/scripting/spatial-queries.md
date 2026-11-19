# Spatial queries: what is near, without a loop over every object

`Physics` and `Physics2D` answer "which colliders are here?" on the physics engines' own broad
phases (Jolt, Box2D): overlaps, checks and shape casts, beside `raycast`. `MeshRaycast` answers the
same for what is drawn, by bounds, with no collider (docs/specs/spatial-queries.md).

## Choosing a query

| Question | Query |
|---|---|
| Every collider in a radius / box / capsule | `Physics.overlapSphere` / `overlapBox` / `overlapCapsule` |
| Is anything there? (stops at the first) | `Physics.checkSphere` / `checkBox` / `checkCapsule` |
| What does a moving shape hit first? | `Physics.sphereCast` / `boxCast` / `capsuleCast` (`...All`: every hit) |
| 2D | `Physics2D.overlapPoint` / `overlapCircle` / `overlapBox` / `overlapCapsule` / `overlapArea`, `circleCast` / `boxCast` / `capsuleCast` |
| What is drawn here (meshes, sprites, instances), no collider | `MeshRaycast.overlapSphere` / `overlapBox` |
| A Transform-only GameObject | give it a trigger collider: queries find colliders and drawn bounds only |

```lua
local ENEMY = LayerMask.getMask("Enemy")
local function nearbyEnemies(self)
    local near = Physics.overlapSphere(self.scene, self.go.position, 8, ENEMY)   -- OverlapHit[]
    for i = 1, #near do
        local enemy = GameObject(self.scene, near[i].body)
        print(enemy.name, near[i].colliderIndex, near[i].isTrigger)
    end
    local blocked = Physics.checkBox(self.scene, self.go.position, Vector3(1, 1, 1), Quaternion.IDENTITY)
    local hit = Physics.sphereCast(self.scene, self.go.position, 0.3, Vector3(0, 0, -1), 20)
    if hit.hit then print(hit.distance, hit.point, hit.normal, hit.body) end
    return blocked
end
```

3D overlaps return every collider (one hit per collider: a body with three colliders inside the
shape gives three hits); 2D `overlapX` returns the first and `overlapXAll` every one, as in Unity.

## Triggers

Every query and raycast takes a trailing `QueryTriggerInteraction`: `Ignore` skips trigger
colliders, `Collide` reports them, `UseGlobal` (the default) follows `Physics.queriesHitTriggers`
(`Physics2D.queriesHitTriggers` for 2D), set in Project Settings > Physics > Queries Hit Triggers
(on by default, as raycasts always behaved) and changeable in Lua for the session. The editor puts
the project's value back when Play starts and stops.

```lua
local function inPickupZone(self, center)
    local zone = Physics.checkBox(self.scene, center, Vector3(2, 1, 2), Quaternion.IDENTITY, -1, QueryTriggerInteraction.Collide)
    local solidOnly = Physics.overlapSphere(self.scene, center, 3, -1, QueryTriggerInteraction.Ignore)
    local ray = Physics.raycast(self.scene, center, Vector3(0, -1, 0), 10, -1, QueryTriggerInteraction.Ignore)
    return zone, #solidOnly, ray.hit
end
```

A last optional table lists bodies the query passes through: `Physics.overlapSphere(scene, p, 5,
-1, QueryTriggerInteraction.UseGlobal, {self.entity})`.

## No garbage: NonAlloc

`...NonAlloc` forms write entity ids (integers, no object per hit) into a table you keep, clear its
entries past the count (`#results == count` afterwards) and return the count. Once the table has
grown, a call makes no Lua garbage and no C++ allocation. An optional `colliders` table receives the
collider indices.

```lua
local function scan(self)
    self.hits = self.hits or {}
    self.colliders = self.colliders or {}
    local n = Physics.overlapSphereNonAlloc(self.scene, self.go.position, 8, self.hits, -1, QueryTriggerInteraction.Ignore, self.colliders)
    for i = 1, n do
        local id, collider = self.hits[i], self.colliders[i]
        print(id, collider)
    end
    return n
end
```

The `...All` cast forms allocate their result table; the closest-hit casts return one value.

## Order

Overlap results are sorted by (body, colliderIndex); cast `All` results by distance, then (body,
colliderIndex). The same world and query give the same list on a platform, whatever order the
bodies were made in: recorded inputs and test runs replay identically. Sorting hundreds of hits is
O(n log n).

## When the world is read

Queries see the world of the last physics step (Unity's `autoSyncTransforms = false`): a Transform
moved in `onUpdate` reaches its body at the next fixed step. Inside the step (a contact callback,
`synchronousContactEvents3D`, `shouldCollide3D`) a query logs one error and finds nothing: queue it
for `onUpdate`. The C++ forms with a caller vector are safe from jobs outside the step; the Lua
forms are main-thread.

`MeshRaycast` overlaps refit the drawn bounds at most once per engine frame: a second overlap in the
same frame sees the bounds of the first (or of a later rebuild). `{current = true}` refits now.
Raycasts always refit.

## 2D units and capsules

2D positions, sizes and distances are scene units (points), as `Physics2D.raycast`; angles are
degrees. A 2D capsule is Unity's: `size` is its bounding box and its round ends lie along
`CapsuleDirection2D.Vertical` or `Horizontal`; `overlapArea` takes two opposite corners in any order.

```lua
local function look2D(self, x, y)
    local first = Physics2D.overlapCircle(self.scene, Vector2(x, y), 48)            -- OverlapHit or nil
    local all = Physics2D.overlapBoxAll(self.scene, Vector2(x, y), Vector2(64, 32), 15)
    self.hits2D = self.hits2D or {}
    local n = Physics2D.overlapCapsuleNonAlloc(self.scene, Vector2(x, y), Vector2(20, 60), CapsuleDirection2D.Vertical, 0, self.hits2D)
    local hit = Physics2D.circleCast(self.scene, Vector2(x, y), 8, Vector2(1, 0), 200)
    return first, #all, n, hit.hit
end
```

## Mesh colliders are surfaces

A mesh collider is its triangles: a sphere wholly inside a closed mesh collider does not overlap it
(as Unity). Use a convex collider (box, capsule, convex hull) for a volume a query must see from
inside. A Character Controller is found through its inner body only when Detect Collisions is on.

## Drawn objects

```lua
local function visibleAround(self, p)
    local seen = MeshRaycast.overlapSphere(self.scene, p, 15, -1, {kinds = MeshRaycast.MESH | MeshRaycast.INSTANCE})
    self.drawn = self.drawn or {}
    local m = MeshRaycast.overlapBoxNonAlloc(self.scene, p, Vector3(4, 4, 4), Quaternion.IDENTITY, self.drawn)
    return #seen, m
end
```

`MeshOverlapHit`: `entity`, `kind` (a `MeshRaycast` bit), `instance` (-1 unless an instance). The
default kinds are meshes, instances and bounds-only objects (points, lines, particles); terrains
and UI rects are left out (their bounds cover the whole map or the screen) unless `kinds` asks.

## Seeing and checking queries

- Game view toolbar > Queries (`Physics.debugDrawQueries`, MCP `configure_game_view
  {physics_debug_queries}`) draws each query of the main thread for 1 s: the shape, the cast sweep,
  hits as crosses; green with hits, red without. Queries from jobs are not drawn.
- The Stats panel shows `Physics queries: N (M hits)` for the last frame; `get_frame_times` has the
  `Physics queries` and `Physics query hits` monitors.
- MCP `query_physics` (group `physics`) runs any query in Play; `verify: true` compares an overlap
  with a brute-force test of every body (`PhysicsQueryReference`) and lists missing and extra hits.
