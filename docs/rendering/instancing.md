# Instanced meshes and terrain foliage

An **Instanced Mesh** draws many copies of one mesh in one draw per material: grass, rocks, crowds,
the chunks of a terrain's foliage layers. This page says how its copies fade with distance, how they
face the camera, and how terrain foliage uses both. Lua names are on `Mesh`
(`gameObject:getMesh()` or a `Primitive`, after `createInstancedMesh()`).

## Distance fade

With **Distance Fade** on (Lua `mesh.distanceFade`), each instance shrinks toward its base (its
origin) by its horizontal distance from the camera, measured in the mesh's local space:

| Inspector | Lua | What it does |
|---|---|---|
| Fade Start, Fade End | `fadeStart`, `fadeEnd`, `setFadeRange(start, end)` | full size up to Fade Start, nothing from Fade End on |
| Near Fade Start, Near Fade End | `nearFadeStart`, `nearFadeEnd`, `setNearFadeRange(start, end)` | nothing up to Near Fade Start, full size from Near Fade End on |

A range whose end is not past its start is off. With both ranges an instance shows in a band: the
inner edge of a far level of detail. Two batches cross-fade when the near one's Fade Start/End
equal the far one's Near Fade Start/End: one shrinks while the other grows over the same distances.

The fade applies in every pass that draws the instance: colour, shadows, the depth pre-pass and the
SSR G-buffer. With the built-in shaders the engine also drops, on the CPU, the instances the fade
has shrunk to nothing; it rebuilds that list only when the camera has moved past a small margin
(0.25 m at least, 4 % of the fade band), so walking costs little. A forked mesh shader made before
the near fade existed ignores it; fork it again to get it.

## Billboards

**Billboard** turns every instance to face the camera (one rotation for the batch: the view's).
**Cylindrical Billboard** turns them about the mesh's up axis only, toward the camera's side: trees
and grass cards that stay upright. A camera looking straight along the up axis keeps the last turn.
Both follow the camera every frame it moves. Lua: `mesh.instancedBillboard`,
`mesh.instancedCylindricalBillboard`, `mesh:setInstancedBillboard(billboard, cylindrical)`.

## Terrain foliage

Each foliage layer of a Terrain is drawn as square chunks of instances around the main camera. In
the Terrain window's foliage settings, **Draw range** sets two distances in world units:

- **Max** (`drawDistance`): instances fade out over the last quarter before it.
- **Min** (`minDrawDistance`, default 0): instances fade in over the last quarter before it, and
  chunks entirely inside it are not filled.

A far level of detail: copy the layer, give the copy a cheaper mesh (a billboard card), set the
copy's Min to the original's Max and its Max further out. The two cross-fade.

The chunks are kept in a ring sized for the widest view, so walking only refills the chunks that
enter it.

## Foliage Distance (quality)

**Project Settings > Quality > Foliage Distance (Desktop / Mobile / Web)** multiplies every foliage
layer's Draw range on that platform group (Unity's `QualitySettings.terrainDetailDistanceFactor`).
The default is 1; 0.5 to 0.7 suits phones and the web. 0 draws no foliage. The editor's views use
the desktop value; the Terrain window's preview ignores it. A game changes it at runtime from its
settings menu:

```lua
Engine.foliageDistanceScale = 0.6      -- 0 to 10
Engine.resetFoliageDistanceScale()     -- back to this platform's Project Settings value
```

The runtime value resets when Play stops. MCP: `set_project_settings` with
`quality.foliage_distance_desktop`, `_mobile`, `_web`.
