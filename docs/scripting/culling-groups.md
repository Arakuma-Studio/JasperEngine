# Culling Groups and Simulation Culling

Two ways to spend nothing on what is far or off screen
(docs/specs/culling-group-and-simulation-distance.md). Both run on the CPU, only while the scene
plays, and cost nothing in a scene that has neither.

- A **Culling Group** (Unity's `CullingGroup`) tells a script which of many bounding spheres are
  on screen and how far they are, through a callback when that changes.
- **Simulation Culling** is a component: no code needed. A GameObject beyond its Simulation
  Distance from every focus, or over ground that is not loaded, stops updating its scripts, its
  Animator and its bodies.

## Culling Groups

A group holds spheres (up to 100,000), fixed or following GameObjects. Once a frame, after the
frame's transforms, each sphere gets a band and a visibility:

- **Bands** come from up to 8 ascending distances: band 0 is nearer than the first, band n is at
  or beyond the last. The distance is from the reference point to the sphere's surface. With no
  reference point set it is the target camera's position, else the scene's main camera's.
- **Visible** means inside the frustum of the target camera, or of any enabled camera when it has
  none. There is no occlusion culling: a sphere behind a wall but inside the frustum is visible.
- `onStateChanged(index, isVisible, wasVisible, band, previousBand)` is called for each sphere
  whose band or visibility changed, in index order, after the whole group was evaluated. The
  first evaluation reports every sphere with `previousBand == -1`, so a script needs no separate
  first query.

Indices are 1-based in Lua; bands are 0-based (0 nearest), -1 before the first evaluation.

### Wake enemies when they come on screen

```lua
---@class EnemyWaker : ScriptInstance
local EnemyWaker = {}

function EnemyWaker:init()
    self.enemies = {}                                   -- filled by the spawner
    self.group = CullingGroup(self.scene)
    self.group.onStateChanged = function(index, isVisible, wasVisible, band, previousBand)
        local enemy = self.enemies[index]
        if enemy then enemy.awake = isVisible end
    end
end

function EnemyWaker:add(enemyEntity)
    self.enemies[#self.enemies + 1] = {entity = enemyEntity, awake = false}
    local index = #self.enemies
    self.group:setBoundingSphereCount(index)
    self.group:setSphereTarget(index, enemyEntity, 1.0)  -- follows the GameObject
end

return EnemyWaker
```

### Run the AI by band

```lua
local group = CullingGroup(self.scene)
group:setBoundingDistances({15, 40, 90})               -- bands 0..3
group:setDistanceReferencePoint(self.player)           -- a GameObject (followed) or a Vector3
group.bandHysteresis = 2                               -- 2 m past an edge before moving out
group.onStateChanged = function(index, isVisible, wasVisible, band, previousBand)
    self.brains[index].rate = band                     -- 0: every frame .. 3: asleep
end
```

`group.targetCamera = cameraEntity` restricts visibility to one camera (nil: any enabled one; a
disabled or destroyed target makes every sphere invisible). `group.enabled = false` stops the
evaluation and the events; the states freeze until it is enabled again, and the next evaluation
reports what changed meanwhile.

### Queries

```lua
local visibleNow = {}                                  -- keep it: no garbage per query
local n = group:queryIndices(true, visibleNow)         -- visible spheres
n = group:queryIndices(2, visibleNow)                  -- spheres in band 2
n = group:queryIndices(true, 1, visibleNow, 10)        -- visible in band 1, skipping 10 matches
print(group:isVisible(1), group:getDistance(1))
```

`queryIndices` writes `out[1..n]` and returns n; entries past n stay as they were.

### Lifetime

A group made inside a script's callback (`init`, `onUpdate`, ...) belongs to that script and is
disposed when the script instance goes (its GameObject destroyed, the scene unloaded). Any other
group lives with its scene. `group:dispose()` ends it now; `isValid()` is false afterwards.
Losing the last Lua reference does not dispose it. A scene has at most 256 groups.

Inside `onStateChanged` a script may change spheres, distances, the camera or `enabled` (applied
at the next evaluation), make another group (evaluated from the next frame) or dispose its group
(the rest of that evaluation's events are dropped).

## Simulation Culling

Add Component > Miscellaneous > **Simulation Culling** on an enemy, a car, a crowd member: its
scripts, Animator and dynamic bodies, and those of its children (down to a child with a Simulation
Culling of its own), follow these states:

| State | When | What happens |
|---|---|---|
| Full | within Reduced Rate Distance (or Simulation Distance without a reduced band) | everything runs |
| Reduced | between Reduced Rate Distance and Simulation Distance | `onUpdate` and `onPostUpdate` run on one frame in Reduced Update Interval; `onFixedUpdate`, the Animator and bodies run |
| Culled | beyond Simulation Distance, or over ground not loaded | `onUpdate`, `onPostUpdate` and `onFixedUpdate` are skipped, the Animator stops (as Cull Completely), dynamic bodies freeze in place |

- Distances are to the nearest **focus**, from the bounds' surface (Bounds Radius, or the
  GameObject's rendered or collider bounds when it starts). Moving outward needs the distance past
  the edge by more than Hysteresis.
- **Simulate When Visible**: beyond the distance but on screen in any enabled camera, it stays
  Reduced (Full without a reduced band).
- The **Cull** foldout chooses what is culled: Scripts, Animator, Physics.
- A frozen body is kinematic with zero velocity: other bodies hit it as a wall and physics queries
  find it. When it resumes it gets back its velocity. While frozen, `Rigidbody.velocity`,
  `angularVelocity` and `isKinematic` read what the body really has and setters are kept for the
  thaw, so save systems see the real state. Turn Physics off to let a body keep falling instead.
- What is **not** culled: coroutines, `Timer` callbacks, input and other engine events, component
  events, physics contact messages, Particle Systems (they have their own Culling Mode), Audio
  Sources and NavMesh Agents. Stop those in `onSimulationCulled`.
- The first evaluation runs when the scene starts, before the first physics step, so nothing over
  unloaded ground moves for a frame. A GameObject instantiated while playing is evaluated before
  its first callback. At most 64 GameObjects change state a frame (the nearest resume first, the
  farthest are culled first); culls for unloaded ground are never delayed.

### Reacting from a script

```lua
---@class Grunt : ScriptInstance
local Grunt = {}

function Grunt:onSimulationCulled(reason)              -- "distance" or "ground_not_loaded"
    NavMeshAgent(self.scene, self.entity).isStopped = true
end

function Grunt:onSimulationResumed()
    NavMeshAgent(self.scene, self.entity).isStopped = false
end

return Grunt
```

Both messages go to the scripts on the component's GameObject (enabled or not), never between
Reduced and Full. C++ scripts override `onSimulationCulled(const std::string& reason)` and
`onSimulationResumed()`.

```lua
local culling = SimulationCulling(self.scene, self.entity)   -- adds one if missing
print(culling.state, culling.reason, culling.focusDistance)  -- "culled", "ground_not_loaded", 12.5
culling.simulationDistance = 80
culling.reducedRateDistance = 30
culling.reducedUpdateInterval = 4
```

### The reduced band and Engine.deltatime

Inside a Reduced GameObject's `onUpdate` and `onPostUpdate`, `Engine.deltatime` (and
`Time.deltaTime`, C++ `Engine::getScriptDeltatime`) is the scaled time since its previous reduced
call: movement code written with `deltatime` covers the same distance at the reduced rate. Hit
Stop still applies on top. The frames are spread: a GameObject updates on frames where
`(Engine.frameCount + entity) % interval == 0`.

### Focuses and multiplayer

The main camera is the focus by default. A game adds up to 16 more, one per player:

```lua
local id = SimulationCulling.addFocus(self.scene, playerGameObject)   -- or a Vector3
SimulationCulling.setFocus(self.scene, id, Vector3(0, 0, 0))         -- a fixed point now
SimulationCulling.setCameraIsFocus(self.scene, false)                -- only the players count
SimulationCulling.removeFocus(self.scene, id)
```

A focus GameObject is never culled itself. With script focuses and the camera not a focus, a
scene with no focus culls everything by distance.

### The ground rule

With **Freeze When Ground Not Loaded** (default on) a GameObject is Culled with reason
`ground_not_loaded` while the ground under it is not in memory, so it never falls through a world
that is still streaming:

- **Terrain Worlds**: a present tile within Bounds Radius + 1 m of it that is not loaded. The
  world's edge (no tile there) never counts. A Simulation focus streams no terrain: pair
  `SimulationCulling.addFocus` with `TerrainWorld:addFocus` for each player.
- **Your own streaming** (tilemaps, voxel chunks, procedural levels):

```lua
SimulationCulling.setGroundCheck(self.scene, function(x, y, z, radius)
    return self.world:isLoaded(x, z, radius)
end)
SimulationCulling.setGroundCheck(self.scene, nil)                     -- removes it
```

The check is called only for GameObjects within simulation distance, at most 256 times a frame.

### Device Tiers and the global scale

Project Settings > Quality > Device Tiers > **Simulation Distance** multiplies every component's
distances (Low 0.6, Medium 0.8, High and Ultra 1): weak devices simulate a smaller radius.
`SimulationCulling.distanceScale` multiplies on top (a settings menu's "simulation range").

## Debugging

- The Game view toolbar's **Culling** toggle draws the Simulation Culling spheres (green Full,
  yellow Reduced, red culled by distance, grey by unloaded ground), the Culling Group spheres
  (cyan visible, dark blue not) and the focuses as crosses, while playing.
- The Stats panel shows `Culling groups: ...` and `Simulation: F full, R reduced, C culled (N
  ground)  Script updates skipped: K`.
- `DebugOverlay.getStats()` has `cullingGroups`, `cullingSpheres`, `cullingVisible`,
  `simulationFull`, `simulationReduced`, `simulationCulled`, `scriptUpdatesSkipped`.
- Agents: `get_render_stats` returns `culling`, `configure_game_view {culling_debug: true}` draws the
  spheres, `get_runtime_entity` shows the component's `state`, `reason` and `focusDistance`
  (jasper://docs/profiling).
