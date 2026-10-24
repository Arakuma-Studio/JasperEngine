# Lattices (free-form deformation)

A lattice is a box cage of control points. Moving the points bends, squashes, twists or
wobbles every mesh bound to it, smoothly, on the GPU, without editing the mesh: models
(static, skinned, morphed), instanced meshes, sprites, polygons, tilemaps. Undoing, saving
and prefab overrides work per point. `edit_lattice` does every edit the Scene view does; the
generic tools (`get_entity`, `set_properties`, `add_component`) see the four components
`lattice`, `latticemodifier`, `latticefollower`, `latticetracks`.

## Concepts

- **Lattice**: a GameObject with a Transform and a `lattice` component. Its Transform is
  the cage: position, rotation, and `scale` = size. Moving or scaling it moves the cage, and
  a mesh fully inside an undeformed cage is unchanged.
- **Resolution** `[x, y, z]`: points per axis, 2..16 each (default 3, 27 points). Change it
  with `edit_lattice resolution` (it resamples the offsets and poses so the shape stays);
  `set_properties` refuses lone `resolutionX/Y/Z` writes.
- **Offsets**: one per point, all zero at rest, in lattice local units. What is stored,
  animated, overridden in prefabs. `offsets[i]` is a property per point.
- **Target**: a GameObject with a mesh that has a `latticemodifier` listing **bindings**
  (`lattice`, `weight` 0..2, `stage`, `submesh` -1 = all, `mask` = a vertex colour channel
  that scales the effect). `includeChildren` extends the bindings to every mesh below it (a
  car, a character with props). At most 4 lattices deform one mesh: its own bindings in
  order, then inherited ones; the rest are ignored (a warning says so). Each lattice deforms
  a mesh once: a child's own binding to a lattice it also inherits **replaces** the inherited
  one for it, so `bind` the child with `weight: 0` to exclude it, or another weight to tune it.
- **Follower**: a GameObject with a `latticefollower` (`follow` op) is placed each frame where
  its **rest** (an empty GameObject where it stands undeformed) ends up through the cage: a
  prop on a squashed surface, a light on a bent lamp. It is moved, not bent; a target cannot
  also be a follower.
- **Interpolation**: `cubic` (default, smoothest; the mesh follows the cage without passing
  exactly through inner points), `smooth` (no seams, softer), `sharp` (trilinear: passes
  through the points, creases at cell borders).
- **Outside**: what happens beyond the cage. `fade` (default) fades the deformation out over
  `fade_distance` local units (0.25 = a quarter of the cage); `extend` continues the border
  deformation forever.
- **Stage** (skinned models only): `after_skinning` (default) deforms the posed mesh (a cage
  in the world squashes whatever walks through it; parented to a bone it follows it);
  `before_skinning` deforms the bind pose (a big head in every animation).

## Coordinate spaces

- **Local**: the cage is the unit cube `-0.5..0.5` on each axis whatever its size. Point
  `(i, j, k)` (0-based) rests at `i/(x-1) - 0.5` (same for j, k): on a 3x3x3 cage `(0,0,0)` is
  the `-x -y -z` corner, `(1,2,1)` the top centre. Flat index = `i + x * (j + y * k)`.
  Offsets are local: `[0, 0.5, 0]` lifts a point by half the cage height. On a cage scaled
  `[2, 4, 2]` that is 2 world units.
- **World**: `space: "world"` in `set` and `move` takes world positions / vectors /
  degrees; `box`, `sphere` and `sample` are always world. Results give both
  (`max_offset.local` / `.world`, `points[].offset` local and `.world` the point's position).
- A falloff `radius` is in local units (1 = the cage's size).

## edit_lattice

Order inside one call: `create`, then `resolution`, `interpolation`, `outside`,
`fade_distance`, then `ops` in order (each sees the result of the previous one), then the
read (`sample`, `detail`). One call is one undo step; any error rolls the whole call back
and names the failing part (`ops[2].select.face: ...`). Edits are refused while playing (stop
first; to try a change in the running game, `eval_lua` the Lua methods: `object("Cage",
"Lattice"):applyPreset("squash", "y", 0.3)`); a call without edits is a read and works in play
mode too (it reads the running game).

`lattice` is optional: the single selected lattice, else the scene's only one.

```json
{"create": {"targets": ["Car"], "name": "CarCage", "resolution": [5, 3, 3]},
 "ops": [{"op": "move", "select": {"face": "+z", "j": [1, 2]}, "offset": [0, 0, -0.2],
          "falloff": {"radius": 0.5}}]}
```

`create` fits the cage around the targets' bounds (2% padding, `fit_padding` to change it; one
target: aligned to its rotation; a flat target such as a sprite gets a thin cage with 2
points on that axis) and binds every target that has a mesh (`includeChildren` on for a
target whose meshes are its children). Without targets: a unit cage at the origin (100 units
in 2D scenes). `create_entity type: "lattice"` with `targets` does the same.

### Ops

| op | Arguments | Notes |
|---|---|---|
| `set` | `points: [{ijk: [i,j,k] \| index, offset \| position: [x,y,z], space}]` | absolute; `position` is where the point goes |
| `move` | `select`, `offset`, `rotate` [x,y,z] degrees, `scale` (number or [x,y,z]), `pivot`, `space`, `falloff` | relative to the current positions |
| `reset` | `select` | back to rest |
| `preset` | `name`, `axis`, `toward`, `amount`, `seed`, `mode` (`add` / `replace`), `select` | below |
| `mirror` | `axis`, `from` (`"+"` copies the + half onto the - half, `"-"` the reverse) | the middle plane loses its axis component |
| `fit` | `targets` (default: the lattice's targets), `padding` (0.02) | moves/scales the cage; offsets unchanged |
| `bind` | `target`, `weight`, `stage`, `submesh`, `mask`, `include_children` | binding again updates it; unset values keep the existing binding's |
| `unbind` | `target` | a child that only inherits the lattice has nothing to unbind: `bind` it with `weight: 0` |
| `follow` | `target`, `rest` | below |
| `pose` | one of `save` / `apply` / `delete` (the name), `weight` (apply) | a pose is a saved set of offsets |
| `bake` | `target` (default: the only target) | writes `<model>_lattice.glb` next to the model, switches the target to it, removes the baked bindings; returns `path` and `note`. Generated meshes (primitives, sprites) cannot be baked |

`follow` adds a `latticefollower` to `target` and gives it a rest: `rest` (a GameObject), else
its current rest, else a new empty sibling where it stands ("<name> Rest"). Move the rest to
move the follower. A follower just outside a cage with `outside: "fade"` gets only part of the
deformation (warning `follower_outside`): set `outside: "extend"` or fit the cage around it.

`move`: `pivot` is `center` (default: the centroid of the selected points), `lattice` (the
cage's centre) or `[x, y, z]` in `space`. Rotation and scale turn the points about the pivot.
`falloff: {radius, curve}` also moves the unselected points within `radius` (local) of a
selected one, weighted by `curve`: `smooth` (default), `linear`, `sharp`, `constant`.

### Selectors (`select`, default `all`)

- `"all"`, `"exterior"` (the outer shell), `"interior"` (needs 3+ points per axis),
  `"selected"` (the points selected in the Scene view).
- A face: `"+x"`, `"-x"`, `"+y"`, `"-y"`, `"+z"`, `"-z"`; aliases `right` `left` `top`
  `bottom` `front` (+z) `back` (-z).
- A list of points: `[[i,j,k], ...]` or flat indices.
- An object; every key given must match (intersection): `face`, `i` / `j` / `k` (an index or
  `[lo, hi]` inclusive), `points`, `box: {min, max}` (world), `sphere: {center, radius}`
  (world, current positions). `{"j": 2, "i": [0, 1]}` = the top row's left two columns.

A selector that matches nothing is an error (it lists where the points are).

### Presets (`preset`, computed on the rest shape, then added to the offsets)

`axis` is x, y or z (default y); `toward` (bend, shear) defaults to the next axis (x -> y -> z -> x);
`"-x"` bends or shears the other way (same as a negative `amount`). Bend and twist work in the
cage's proportions (its scale), so the angles are true in the world: a 60 degree bend of a tall
cage fitted around a pole turns the pole 60 degrees and keeps its cross-section round.

- `squash` `amount` 0..0.95: shorter along the axis by `amount`, fatter across it (volume kept).
  `stretch`: longer by `amount`, thinner.
- `taper`: the +axis end shrinks across by `amount` (0.5 = half as wide at the top, a cone).
- `twist`: the +axis end turns `amount` degrees around the axis (counterclockwise seen from
  +axis), the -axis end stays.
- `bend`: the axis curves toward `toward` by `amount` degrees in total, as an arc that starts
  at the -axis end (a 90 degree bend makes an upright cage lie over); length kept. With `cubic`
  the mesh follows the arc slightly short at the far end; use 5+ points along the axis.
- `bulge`: the middle fattens by `amount` (1 = twice as wide at the middle), the ends stay.
- `shear`: the +axis end slides `amount` local units along `toward`.
- `noise`: every point moves randomly up to `amount` local units; `seed` makes it repeatable
  (same seed, same shape, on every platform).

`mode: "replace"` sets the selected points to the preset instead of adding to them.

### Results and warnings

Every call returns `lattice`, `resolution`, `interpolation`, `outside`, `fade_distance`,
`targets` (`entity`, `stage`, `weight`, `meshes` reached), `moved_points` (non-zero offsets),
`point_count`, `max_offset` (`local`, `world`), `poses`, `followers` (`entity`, `rest`; when
any), `warnings`, `ops` (per op: `points` affected, `path`/`note` for bake, `rest` for follow,
`overrides` when a bind replaces an inherited binding), `notes`. `detail: "points"` adds `points` (every moved point:
`ijk`, `index`, `offset`, `world`). `sample: [[x,y,z], ...]` (world) returns `samples`:
`point`, `deformed` (where that world point ends up) and `stretch` (volume ratio: 1 rest,
< 1 squashed, > 1 stretched, <= 0 folded), computed by the same math as the GPU.

Warnings (`kind`): `folded_cells` (cells turned inside out, listed by their lower corner
`(i, j, k)`: the mesh shows its inside there; move those points less far, reset them, or
spread the change with a falloff or more resolution), `too_many_lattices`,
`missing_lattice` (a binding to a deleted lattice), `no_mesh` (a target with nothing to
deform), `no_targets`, `disabled`, `zero_scale`, `follower_outside` (a follower's rest in the
fade zone, with its `share` of the deformation), `follower_no_rest`.

## Seeing it

`set_scene_camera display: {"show_all_lattices": true}` draws every cage in the Scene view
(the selected one is always drawn; edges are blue where squashed, red where stretched).
Then `screenshot view: "scene"`. `frame_entity` on the lattice frames the cage.

## Recipes

- **Squash-and-stretch bounce**: a cage around the ball (`create` with `targets`), parented
  under it (`move_entity`) so it travels with the bounce; save the shapes as poses in one call
  (`ops: [preset squash amount 0.4, pose save "squash", reset, preset stretch amount 0.3, pose
  save "stretch", reset]`), then one `edit_animation` with the ball's position track and a
  `lattice` track on the **cage** (not the ball): keys `points: "rest"`, `"squash"` (the pose
  name) at the landing, `"stretch"` on the way up, `"rest"`. `points` also takes
  `{"7": [0, -0.2, 0]}` or `"current"` (the cage as it is now, as the Animation window keys).
- **Prop on a squashed box**: `follow` with the prop as `target` (a rest is made where it
  stands), and `outside: "extend"` when the prop sits just outside the cage; it then rides the
  surface however the cage is edited or animated.
- **Exclude one child** of a group the cage deforms through `include_children`: `bind` that
  child with `weight: 0`.
- **Big-head character**: a cage around the head of a skinned character in its bind pose,
  `bind` the character with `stage: "before_skinning"` and `include_children: true`, then
  `move select: "all" scale: 1.6 pivot: "lattice"`: the head stays big in every animation.
  Put the cage under the head bone's GameObject instead (stage `after_skinning`) to make a
  squash follow the head.
- **Car crumple**: `create` around the car with resolution `[5, 3, 5]`, then
  `move select: {"sphere": {"center": <impact point>, "radius": 0.8}} offset: [0, 0, -0.3]
  space: "world" falloff: {"radius": 0.4}`; check `warnings` for folded cells.
- **Wind on a flag** (Lua, runs in play): a cage around the flag plane; a script on the cage
  (a 3D flag in the x-y plane waves along z; a 2D sprite's cage is flat in z, so wave along y:
  `Vector3(0, 0.08 * i * math.sin(...), 0)`):

  ```lua
  function Flag:init()
      RegisterEngineEvent(self, "onUpdate")
      self.cage = Lattice(self.scene, self.entity)
      self.t = 0
  end
  function Flag:onUpdate()
      self.t = self.t + Engine.deltatime
      local cage = self.cage
      for i = 1, cage:getResolutionX() - 1 do      -- the pole side (i = 0) stays
          for j = 0, cage:getResolutionY() - 1 do
              for k = 0, cage:getResolutionZ() - 1 do
                  cage:setOffset(i, j, k, Vector3(0, 0, 0.08 * i * math.sin(self.t * 3 - i)))
              end
          end
      end
  end
  ```
- **Jelly**: `set_properties` the cage's `lattice` `{"jiggle": true, "jiggleAnchor":
  "negative_y"}` (the bottom stays put); it wobbles when the cage accelerates (parent the cage
  to the moving object) or when a script calls `poke` / `pokeAt`. Only while playing.
- **Rock variants**: duplicate a rock glTF model, `create` a cage around each copy with
  `resolution: [4, 4, 4]`, `preset noise amount 0.12 seed <n>` (a different seed each; 0.08
  on a 3x3x3 cage barely shows; past about 0.4 x the point spacing cells fold, so check
  `warnings`), then `bake` to write a real mesh per variant (no lattice cost at runtime) and
  delete the cages.

## Costs and limits

- No cost for meshes without a binding. A bound mesh pays in its vertex shader (one cage
  evaluation per vertex and binding, more for cubic) in every pass that draws it (shadows
  too). Uploads happen only when offsets change. Resolution changes the data size, not the
  per-vertex cost. Jiggle costs CPU per point while awake (it sleeps at rest).
- A sprite bound to a cage is drawn as an 8x8 grid (`subdivisions`, 1..64, on its
  `latticemodifier`) so it can bend smoothly.
- Physics colliders keep the rest shape. Picking uses the (grown) bounds, not the bent
  triangles. Terrains, HTML UI, particles, points, lines and text are not deformed.
- Folded cells turn the mesh inside out (backfaces, broken lighting): keep `warnings` empty.
- Baking needs a glTF model (.gltf/.glb; OBJ, primitives and sprites are refused with the
  reason); after-skinning bindings of skinned models are kept, not baked (the `note` says which).
