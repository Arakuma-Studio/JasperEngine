# Voxel Volumes

A **Voxel Volume** is a bounded box of smooth ground that can go anywhere a heightmap cannot: caves,
tunnels, overhangs, arches, ground a game digs at runtime. A plain hill or valley belongs on a
Terrain; a volume is for what a heightmap cannot hold, and a cave entrance into a Terrain is a volume
attached to it, not a hole in the Terrain alone. Enable the tools with
`tool_groups {enable: ["voxels"]}`.

## Units

- Sizes are in **voxels**, multiples of 16 from 16 to 1024 (`size: [x, y, z]`); `voxel_size` is metres
  per voxel (default 1). Memory follows voxels, detail follows `voxel_size`.
- Everything else is **metres in world space**: `center`, `from`, `to`, `radius`, `half_extents`,
  `blend`, `sample_voxels` points and `distance`. The volume's corner is its Transform's position;
  it spans `size * voxel_size` from there.
- A distance is negative inside solid and positive in air. A volume's scale stays 1; a volume on a
  Terrain has no rotation. Rotation is `[x,y,z,w]`, `{euler: [x,y,z]}` in degrees or `{quat: [...]}`.

## The loop

1. Preview: `edit_voxels {ops, dry_run: true}` gives the real numbers and changes nothing.
2. Apply: the same call without `dry_run`. All ops are checked first, and one call is one undo step
   whatever its op count: `undo` brings the content hash back.
3. Verify: `sample_voxels {mode: "hash"}` proves what changed, `{mode: "connectivity", from, to}` that
   a tunnel really joins two places, `visualize_voxels {mode: "slice"}` shows it. An edit that says
   `voxels_changed: 0` missed: look at `sample_voxels {mode: "bounds"}`.
4. Save the scene: it saves its dirty volumes.

## edit_voxels

`create` makes the GameObject, its `.voxelvolume` and the Voxel Volume; `ops` in the same call dig
into the new volume, one undo step for all of it:

```json
{"create": {"name": "Cave", "size": [64, 64, 64], "base": "solid_ground", "height": 40},
 "ops": [{"op": "dig", "shape": {"type": "capsule", "from": [8, 45, 32], "to": [56, 20, 32], "radius": 4}, "blend": 1}]}
```

`create`: `size` (required), `name`, `parent`, `position`, `rotation`, `voxel_size`, `base`, `seed`, `height`
(metres, for `solid_ground`, default half the volume), `terrain`, `blend_width`, `asset_path` (a new
`.voxelvolume`; default `voxels/<name>.voxelvolume`), `materials` (`[{name, color, roughness, metallic,
linked_terrain_layer}]`). `base`: `empty` (default) or `solid_ground`, directly; `terrain`, `caves`,
`overhangs`, `cave_entrance` start a job, polled with `get_job` (add ops once it ends). The result has
`entity`, `asset`, `dimensions`, `estimated_memory` and `over_budget_targets`.

An op: `{op, shape, blend, material, weight, falloff, amount, iterations}`.

| `op` | Does | Fields |
|---|---|---|
| `dig` | solid to air | `blend` (smooth-CSG radius, metres) |
| `add` | air to solid | `blend`, `material` (the slot it writes) |
| `smooth` | blurs the surface in the shape | `amount` 0-1, `iterations` 1-8, `falloff` 0-1 |
| `paint` | sets a slot, the surface stays | `material` (required), `weight` 0-1, `falloff` 0-1 |

| `shape.type` | Fields |
|---|---|
| `sphere` | `center`, `radius` |
| `box` | `center`, `half_extents`, `rotation` |
| `capsule` | `from`, `to`, `radius` |
| `mesh` | `mesh` (a GameObject with a Mesh, voxelized on first use), `scale` |
| `stamp` | `stamp` (a `.voxelvolume` path), `center`, `rotation`, `scale` |

The values are absolute, never rates: the same call gives the same bytes. `hash: true` adds
`content_hash_after`. The result sums every field, and `per_op` lists them for several ops:
`voxels_changed`, `surface_voxels_changed` (sign flipped), `blocks_touched`, `blocks_allocated`,
`blocks_collapsed`, `memory_delta`, `clamped_voxels`, `removed_volume`, `added_volume`,
`removed_by_material`, `holes_cut`, `holes_filled`, `changed_bounds` (`voxels` and `world`). An edit too
big for a second returns a `job_id`; `get_job` holds the same result.

In Play, `edit_voxels` changes only the running copy: the result says `play_only: true`, nothing goes
in the undo history, and Stop restores the volume. `create` and `generate_voxels` wait for Stop.

## sample_voxels

`mode` chooses; all read the running copy in Play.

| `mode` | Takes | Returns |
|---|---|---|
| `values` | `points` (up to 1024) | per point `distance`, `slot_a`, `slot_b`, `weight`, `inside`, `source` (`base`, `modified`) |
| `values` with `ground: true` | `points` | the ground under each x, z: `height`, `normal`, `source` (`terrain`, `voxel`, `none`) |
| `box` | `min`, `max` | `samples`, `min_distance`, `max_distance`, `solid_fraction`, `air_fraction`, `surface_samples`, `slots` |
| `box` with `stride` | `min`, `max`, `stride` (voxels) | every value in metres, x fastest, up to 64³ |
| `bounds` | | `volume`, `solid` and `surface` boxes, `voxels` and `world` |
| `stats` | | `blocks`, `memory`, `budgets` per target, `triangles`, `chunks`, `draws`, `jobs_pending`, `collision_bodies`, `base_stale` |
| `hash` | `per_block` | `content_hash`: the same content gives the same hash however it is held |
| `connectivity` | `from`, `to` | `connected`, `air_components` (`voxels`, `volume`, `bounds`, `open_to_sky`), `open_to_sky` |

A read that must resolve many blocks returns a `job_id`; `get_job` holds the answer. `connectivity` floods
the air at the finest level: a sealed cavity is its own component, with `open_to_sky: false`.

## visualize_voxels

`slice` draws one plane on the CPU, with `axis`, `position` (the world coordinate along it) and
`max_width`: the same content gives the same bytes, with `iso_line_length`, `solid_fraction` and
`air_fraction`. `wireframe`, `lod`, `materials`, `blocks`, `collision`, `holes` turn on that Scene view
overlay and return the view; `frame: true` frames the volume first, `off` clears the overlay.

## generate_voxels

Gives a volume a new base, as a job: `get_job` polls it, `cancel_job` leaves everything as it was. One
undo step.

| `source` | Needs |
|---|---|
| `noise_graph` | `graph` (a `.noisegraph`, 3D, with an `sdf` output) |
| `terrain` | `terrain` (default the volume's), `blend_width` |
| `mesh` | `mesh` (a GameObject or a model path), `transform` |
| `preset` | `preset`: `caves`, `overhangs`, `cave_entrance`, `empty`, `solid_ground`; `seed`, `params: {size_scale}`, `blend_width` |

`keep_edits` (default true) keeps what was edited over the old base; `region` (`{min, max}`, world metres)
limits what it resolves; `dry_run` evaluates a coarse level and says `estimate: true`. The result:
`blocks_non_uniform`, `memory`, `triangles_estimate`, `content_hash`, `base_changed`, `non_watertight`.
Write a graph or use a preset rather than building a landscape from ops by hand.

## Terrain attachment

`create {terrain, size, position}` attaches a volume to a Terrain: Open bounds, Terrain Cut Automatic and
one slot linked to each terrain layer. Its surface starts flush with the terrain, under it by a hair;
digging through the surface cuts terrain holes where the two no longer meet (`holes_cut`, `holes_filled`),
in the same undo step. Keep at least 3 blocks (48 voxels) of untouched surface around any cave.
`next` with `entrance_narrow` means the mouth is too narrow for the hole map to follow: widen it with a
larger dig at the mouth.

## Budgets

| Target | Max dimension | CPU memory, all volumes | Mesh memory | Runtime edit per call |
|---|---|---|---|---|
| Desktop | 1024 | 256 MiB | 128 MiB | High 2 M samples |
| Mobile | 512 | 64 MiB | 32 MiB | Low 256 k |
| Web | 256 | 32 MiB | 24 MiB | Low 256 k |

`over_budget_targets` on `create` and `budgets` in `sample_voxels {mode: "stats"}` compare a volume against
them. A dimension over a target's limit fails its export.

## Errors

A failed call changes nothing. `structuredContent` has `error`, `message` and, when it applies, `op`
(the index of the op) or `next`.

| `error` | Means | React |
|---|---|---|
| `out_of_bounds` | a shape is wholly outside the volume | `sample_voxels {mode: "bounds"}`; positions are world metres |
| `invalid_shape` | a missing or out-of-range field | the message names it; fix the op |
| `invalid_material` | a slot past the volume's slots | use a slot it has; `paint` needs one |
| `missing_asset` | a mesh or stamp did not load | check the GameObject or the path |
| `budget_exceeded` | over the memory budget or a Play call's budget | smaller shapes, or free undo steps |
| `not_loaded` | disabled, hidden, or its file did not load | the message gives the reason; do not overwrite the file |
| `volume_busy` | a job is working on this volume | `next` gives the `get_job` call; call again when it ends |
| `invalid_arguments` | `create` or a call's fields | the message names the field |

## Lua

```lua
local vol = VoxelVolume.fromGameObject(go)
local r = vol:dig(VoxelShape.sphere(center, 3), {blend = 0.5})   -- r.ok, r.error, r.voxelsChanged, r.bounds
vol:fill(VoxelShape.capsule(a, b, 1), {material = 2})
vol:smooth(VoxelShape.box(center, halfExtents, rotation), {amount = 0.5, iterations = 2})
vol:paint(VoxelShape.sphere(center, 2), {material = 3, weight = 1, falloff = 0.5})
local hit = vol:raycast(origin, dir, 100)         -- hit.hit, hit.point, hit.normal, hit.distance, hit.slot
local d = vol:getValueAt(p)                       -- metres, negative inside
local m = vol:getMaterialAt(p)                    -- m.slot, m.second, m.weight
```

The people's page, `docs/voxels.md`, has the rest of the API.
