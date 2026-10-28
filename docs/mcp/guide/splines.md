# Splines (roads, paths, walls, fences, rivers, followers)

A spline is a curve of knots on a GameObject (`spline` component). What it builds is regenerated
whenever the curve, the settings, the GameObject's transform or the terrain under it changes:

| Component | Generic name | Builds |
|---|---|---|
| Spline | `spline` | the curve: `type` (linear, catmull_rom, bezier), `closed`, `upAxis` (`[0,1,0]` 3D, `[0,0,1]` 2D in XY), `knots[N].position/tangentIn/tangentOut/radius/roll/mode`, `terrain.*` (the bake) |
| Spline Extrude | `splineextrude` | a mesh swept along the curve (`shape`: flat, bevel, crown, rectangle, tube, custom), a filled area (`fill`), or water to its banks (`water_banks`). Its material and shadows are the GameObject's Mesh. |
| Spline Instantiate | `splineinstantiate` | models or prefabs along the curve: `mode` spacing, tile or fence; pools `items`, `posts`, `spans`, `gates`, `crests`, `caps` (`{asset, weight}`) |
| Spline Animate | `splineanimate` | moves its GameObject along another's spline (`spline`: that GameObject) in Play |

Knot positions are local to the GameObject; radius is the half-width in world units (times the
largest axis scale). Knot indices are 0-based everywhere (tools, `set_properties`, Lua).
Generated chunks are never saved and never in the hierarchy; clicking them selects the spline.

## Agent workflow

1. `tool_groups` enable `splines`.
2. Create and shape: `edit_spline create {name, dimension: "3d"|"2d"}` with `set_knots`
   (`[[x,y,z], ...]` or `{position, tangent_in, tangent_out, radius, roll, mode}`), or a
   `stroke {points, tolerance, smooth, snap: "terrain"}`: simplified (RDP), smoothed and snapped
   into a CatmullRom spline. A stroke starting at an end of the spline extends it; one ending on
   its start closes it. `insert [{index, knot}]`, `update [{index, ...}]`, `remove [i]` (two knots
   stay), `type`, `closed`. `space: "local"` reads and writes local values. One undo step.
3. Build: `build_spline preset: road|path|wall|fence|river|lake|scatter|circle|square|edge2d|fill2d|circle2d`
   adds and configures the components (an existing spline keeps its knots); then
   `extrude {...}`, `instantiate {...}`, `terrain {...}` take scene-file field names and values,
   e.g. `extrude {shape: crown, width: 6, conform: terrain_height_and_slope, tilesPerMetreU: 0.1}`.
   The reply has vertices, chunks, instances and the generators' warnings (each naming its knot:
   past the mitre limit, inner folds, overlapping corners, waterline rise, off-terrain stations,
   piece cap, a fill that crosses itself).
4. Terrain: `build_spline apply_terrain: true` bakes the spline into the terrain under it (or
   `terrain.terrain`): `terrain {region: path|area, extraWidth, falloff, flatten, flattenMode:
   flatten|cut_only|fill_only, heightOffset, paint, paintLayer, paintStrength, paintFalloff,
   paintNoise, clearFoliage, foliageMask}`. One undo step; `undo` restores the maps exactly. The
   road preset's bed sits `crownRise` under the road. 8-bit heightmaps step: convert them
   (Inspector › Spline › Terrain › Convert heightmap to 16-bit).
5. Verify: `sample_spline` (length, `t` and `distance` frames, `count` samples, `query_points`
   closest points and signed distances), `screenshot`, `get_entity`.

Other fields: `set_properties` on the components (`knots[3].position`, `items[0].asset`,
`spanOverrides[0].kind`).

## Shapes and placement

- Extrude: `widthSource` radius|constant (`width`), Bevel `edgeDrop/edgeInset`, Crown
  `crownRise/shoulderWidth/shoulderDrop`, Rectangle `thickness/height/grade` (racked|stepped,
  `stepHeight`, `levelCourseV`), Tube `tubeSides`, Custom `customProfile [{p:[x,y], hard,
  grounded}]` (x in half-widths), `corners` none|mitre|round (Linear splines; past 120 degrees a
  mitre is built round), `endCaps`, `endTaper`, offsets, `tilesPerMetreU/V` (U = draped length,
  closed loops snap to whole tiles), `conform` none|terrain_height|terrain_height_and_slope,
  `stationSpacing`, `collision` none|mesh|chain_2d.
- Instantiate: Spacing `spacing`, `fit` fit_to_length|fixed_pitch; Tile fits pieces end to end by
  their bounds (a piece runs along its longer horizontal axis, near-square along Z); Fence
  `postPitch`, `spanMaxStretch`, `fenceGrade`, `crestPitch`, a post on every knot,
  `spanOverrides [{knot, span, kind: gate|piece, piece}]`. Jitter (`spacingJitter`, `yawJitter`,
  `lateralJitter`, `scaleRange`), `dropout`, `seed`, `endTrim`, `plantMode`, `maxTilt`. Raising
  `dropout` never moves the pieces that remain. Models are instanced; prefab pieces are
  GameObjects in Play only (512 at most): the Inspector's Bake to GameObjects places them for
  good.
- Water: `water_banks` floods the terrain under the spline up to its banks (`maxHalfWidth`,
  `waterEdgeDrop`, `seaLevelFloor/seaLevel`); a `flat` extrude with `flow` is a channel river.
  Both write the flow and depth that `library:water`'s `FLOW` and `MESH_DEPTH` features read
  (no scene-depth pass). Water Volumes on the same GameObject and `Water.sample` follow the
  river's surface.
- 2D (`dimension: "2d"`, a 2D scene's default): `flat` is an edge in XY, `fill` a filled
  concave shape, `collision: chain_2d` a Box2D chain; Spline Animate turns about Z.

## Lua

`self.entity:getSpline()` (world space): `getLength()`, `evaluate(t)`, `evaluateAtDistance(d)`
(a frame: position, forward, right, up, radius, roll), `getClosestPoint(p)` (t, distance,
position, segment), `getSignedDistance(p)`, `getSignedDistanceXZ(x, z)`,
`getSignedDistanceBandXZ(x, z)`, `sampleUniform(n)`, `addKnot(p)`, `insertKnot(i, p)`,
`setKnotPosition(i, p)`, `removeKnot(i)`, `resample(spacing)`. `getSplineAnimate()`: `speed`,
`duration`, `loop = SplineLoop.PingPong`, `play()`, `pause()`, `restart()`,
`setOnComplete(fn)` (Once). Knot edits regenerate within the frame budget. jasper://docs/lua-api.

## Editor

GameObject › Spline › Empty, Draw Spline Tool, Road, Path, Wall, Fence, River, Lake, Circle,
Square (3D); Empty, Draw Spline Tool, Edge, Filled Shape, Circle (2D). Inspector › Spline › Edit
Spline: click knots (Shift toggles), drag a box, move/rotate/scale them with W/E/R (Scale on
one knot scales its radius), Ctrl+click the curve inserts a knot, Ctrl+click elsewhere extends
the nearer end, Delete removes (two stay), F frames, Esc clears then leaves. Draw: Freehand (a
stroke) or Click (a knot per click, drag for Bezier handles, Enter or double-click finishes,
Backspace takes back, Esc cancels). Live Terrain re-bakes while you edit, from the terrain as
the edit found it; leaving Edit Spline commits one undo step. A later bake starts from the
terrain as it then is: the trench of an earlier position stays (undo it).

## Manual QA

1. A 3D scene with a terrain: GameObject › Spline › Road, then Draw (Freehand, Snap to Terrain)
   a stroke across the terrain: the road appears within a second, on the terrain.
2. Edit Spline, drag a box over three knots, press W and move them: the road follows while
   dragging; with Live Terrain on, the bed re-bakes under it.
3. Ctrl+click the curve: a knot is inserted there. Select it, Delete: it goes. With two knots
   left, Delete is refused (the Inspector says why).
4. Ctrl+Z steps back through each edit (Draw Spline, Move Spline Knots, Insert Spline Knot,
   Delete Spline Knots, Apply Spline to Terrain).
5. Click the road mesh: the Road GameObject is selected.
6. Type Linear, Corners Mitre, move a knot so the road turns more than 120 degrees there: the
   Spline Extrude inspector warns that the knot is past the mitre limit (built Round).
