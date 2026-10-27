# Vertex Animation for agents (`bake_vertex_animation`)

A **Vertex Animation** (`.vat` + its generated `.vat.glb`) is animation baked into one texture and
played by the GPU: thousands of characters, each with its own clip, time, speed and crossfade, in
one instanced draw per material, with no CPU pose work. The **Vertex Animator** component
(`vertexanimator`) plays it on the GameObject whose Model is the `.vat.glb`, and on every instance
of its Instanced Mesh. User guide: [docs/vertex-animation.md](../../vertex-animation.md) in the engine repository.

Use it for crowds, flocks, background characters and imported simulations (Houdini, OpenVAT).
Keep the Animation Player for characters you control closely (clips still edited, bones as
GameObjects, root motion, IK, ragdolls, physics against the animated shape): a baked asset plays
only its baked clips and its colliders use the rest pose.

## The loop

1. **Preview** (nothing written): `bake_vertex_animation {source, clips?, mode?, fps?, ...}`.
   Read the report (sizes, error, `warnings` with fixes) and look at the contact sheet: per clip
   a row of the source and a row of the VAT at 4 times (a loop clip at 0, 1/4, 1/2, 3/4 of its
   length; other wraps at 0, 1/3, 2/3 and the end). Both rows are drawn the same way, so a
   difference is the bake's. Imports have VAT rows only.
2. **Fix** what the warnings say (lower `fps`, `normals: false`, `mode`, `preset`...) and preview
   again.
3. **Write**: the same arguments plus `output` (a `.vat` path inside the assets folder, or `true`
   for the default name next to the source). It writes exactly the previewed bake: one undo step
   whose scope the result text names (`project` for files only; `scene` with `place`).
4. **Use** it: `place: "beside"` makes the GameObject for you; otherwise
   `create_entity {type: "model", model: "<x>.vat.glb"}` then
   `add_component {component: "vertexanimator"}` and `set_properties {values: {vertexanimator:
   {animation: "<x>.vat"}}}`. Check it with a screenshot during play (edit mode stands still unless
   the Vertex Animator's `preview` is on).

A bake is a job: `wait` (default true) and `timeout_seconds` (default 120); past the timeout the
call returns `{job_id, status: "running"}`, follow it with `get_job`, stop it with `cancel_job`
(nothing written, the scene at rest). The contact sheet comes only with a call that waited for the
end. Posing runs on the editor's main thread a slice per frame, so the editor stays responsive.
Not allowed while playing.

## Arguments

| argument | meaning |
|---|---|
| `source` | what to bake: a GameObject (id or path in `scene`: its Animation Player states, entity Animations and Pose Animator), a model file (`assets/models/hero.gltf`: its clips), `model#Clip` (one clip; a list of them for several), a list of `.anim` files (with `on`), a `.poseanim` (on its preview model, or `on`), or a Houdini VAT 3 / OpenVAT export (its folder, mesh with the textures next to it, `.mat`, data JSON or `-remap_info.json`): exports are imported, not baked |
| `scene` | the scene of GameObject references (`source`, `on`) and where `place` puts the result; default the selected scene |
| `on` | what `.anim` / `.poseanim` sources play on: a model or prefab file, or a GameObject |
| `clips` | the clip names to bake, in this order (default all; errors list the source's clips) |
| `mode` | `auto` (default), `vertices`, `bones` (see "Which mode") |
| `fps` | samples per second, (0, 240], default 30; for an import, the export's frame rate when its metadata has none |
| `normals` | Vertices: a tangent-frame texel per sample (default true; false halves the texture and the fetches, the rest normals stay) |
| `precision` | Vertices: `auto` (half, float when half's error is over the tolerance), `half`, `float`; Bones payloads are always float |
| `preset` | `mobile_web` (default) or `desktop` (limits below) |
| `tolerance_mm` | the error allowed (default 1 mm, scaled up to 0.05% of the model's size); drives Auto mode and `precision: auto` |
| `sockets` | node paths below the source's root to bake as sockets (`Armature/Hips/Spine/RightHand`), read in Lua with `getSocketPose` |
| `output` | `.vat` path (the mesh goes to `<name>.vat.glb` next to it), or `true` for the default; omitted = preview |
| `overwrite` | replace an existing `output` (its GUID and, from the same source, its hand edits are kept) |
| `rebake` | a `.vat`: run its recipe again and write over it, keeping its GUID, clip names, events, speeds, wraps and socket names; it takes no other bake settings (bake again with `output` + `overwrite` to change them) |
| `place` | on write: `none` (default: files only), `beside` (a new GameObject "<name> (VAT)" at the source's transform, selected), `replace` (the same, and the source GameObject disabled; GameObject sources only), `{crowd: {count, area, seed}}` (an Instanced Mesh crowd: `count` instances in a square of side `area` metres, random yaw, seeded) |
| `import` | importer options for exports: `axis` (`unity`, `unreal`, `houdini`, `zup`: the export's target engine when its metadata doesn't tell), `axis_scale` (units per metre of the target), `upsample` (rigid exports: 0 automatic, 1..1024 exact), `clip` (the clip's name), `colors` (false drops the colour texel), file overrides `mesh`, `position`, `position2`, `rotation`, `color`, `lookup`, `vat`, `normals_texture` |
| `wait`, `timeout_seconds` | the job pattern above |

The default output (`output: true`) is `<source dir>/<source stem>.vat` (a GameObject: next to its
model; an import: next to the export when it is inside the assets folder, else the assets
folder). The existing-file rule: a `.vat` there baked from the same source is rewritten (GUID and
hand edits kept); any other file makes the bake take a free name (`hero 2.vat`). An explicit
`output` that exists is refused (`output_exists`) unless `overwrite`. The preview's
`default_output` says where `true` would write.

## Results

The report (preview and write): `{mode, mode_reason, vertices, bones, clips: [{name, frames, fps,
bytes}], texture: {width, height, format: rgba16f|rgba32f}, bytes, max_error_mm, mean_error_mm,
quantization_error_mm, tolerance_mm, fetches_per_vertex, warnings: [{kind, message, fix,
fixes?}]}`. `max_error_mm` measures every vertex at every sample and between samples (what the
GPU interpolates) against the exact pose. A preview adds `written: false`, `default_output`,
`default_output_rewrites`, and one image (the contact sheet). A write adds `written: true, vat,
mesh, files, guid, rewritten, undo: {scope, scene?}, undo_name, entity?` (the placed GameObject
`{id, name, path}`). An import's report adds `kind` (`houdini_vat3` | `openvat`), `houdini_mode`
(`soft` | `rigid` | `fluid`), `mesh_vertices`, `source_points`, `pieces`, `source_frames`,
`frames`, `upsample`, `fps`, `target`, `interpolation_error_mm`. Errors are isError results:
`kind: message. Fix: ...` lines and `errors: [{kind, message, fix}]` (plus `source_clips` when a
clip name is wrong).

## Which mode

| mode | what it stores | texel fetches per vertex (×2 during a crossfade) | use for |
|---|---|---|---|
| Bones | per frame, per bone: rotation + translation/scale (2 texels, always float) | 16 (4 influences × 2 texels × 2 frames); 4 for rigid pieces | skinned characters, rigid hierarchies (each moving part is a bone), destruction: kilobytes, exact normals, sockets |
| Vertices | per frame, per vertex: offset from rest (+ tangent frame with `normals`) | 2 (positions) / 4 (with normals) | anything that deforms beyond bones: morph targets, lattices, cloth, soft bodies, fluids; low-poly characters where 2-4 fetches beat 16 |

`auto` picks Bones when bones reproduce every vertex within the tolerance (skin-only or rigid
motion, at most 65,535 bones), else Vertices, and `mode_reason` says why (e.g. "morph targets
move", "non-uniform bone scale"). Force `vertices` for a dense crowd of low-poly characters
seen from afar (fewer fetches), `bones` to keep a heavy mesh small (memory).

## Presets and limits

| preset | texture width | max texels | memory warning | for |
|---|---|---|---|---|
| `mobile_web` (default) | 2048 | 2048 × 2048 (4.2 M) | over 32 MB | GLES3 / WebGL2 devices (guaranteed 2048-texel textures) |
| `desktop` | 4096 | 4096 × 8192 (33.5 M) | over 256 MB | desktop GPUs only |

Texels = samples × (vertices × texels per vertex sample, or bones × 2). A payload taller than the
device's texture limit doesn't upload at runtime: the mesh draws at rest and one error names the
asset. At most 64 clips per asset; vertices up to 2^24.

## Problems and fixes

Errors refuse the bake (nothing written); warnings come with a written bake too. A failed job
(`get_job` status `failed`) has the same shape: its message ends with `- kind: message. Fix: fix`
lines and its result holds `errors: [{kind, message, fix}]`.

| kind | means | fix |
|---|---|---|
| `too_large` | the payload is over the preset's texels | `fixes` lists what fits, best first: lower `fps` (to the value given), `normals: false`, `mode: "bones"`, leave out the largest clip, `preset: "desktop"` |
| `no_animation` | nothing to bake (no clips, or no track moves anything under the source) | pick a source with animations; play `.anim` clips `on` the model they were made for |
| `unbound_tracks` (warning) | some tracks address nodes that don't exist under the source | rename the nodes or fix the clip's paths (Animation window > Fix Paths, or `edit_animation`) |
| `vertex_limit` | more than 2^24 vertices | bake fewer meshes, decimate |
| `bone_limit` | Bones mode with more than 65,535 bones | `mode: "vertices"` |
| `no_mesh` | no renderer under the source | bake a GameObject or model with meshes |
| `invalid_source` | the source can't be resolved (missing file, closed scene, unknown GameObject, a model that doesn't load), also when a job's GameObject is deleted or its scene closed while the job runs | the message names it; GameObject paths are hierarchy paths in `scene`; for a vanished source, undo the deletion or reopen the scene and call again |
| `invalid_settings` | a setting out of range, an unknown clip, `place` that can't apply | the fix names the valid values |
| `output_exists` | an explicit `output` exists | `overwrite: true` (one undo step puts it back), another path, or `output: true` |
| `socket_not_found` | a `sockets` path isn't under the root | give node paths below the source's root |
| `scene_playing` | the source's scene plays | stop play mode |
| `clip_limit` | more than 64 clips | bake two assets |
| `large_interpolation_error` (warning) | motion between samples is lost (fast spins, sharp hits) | raise `fps` to the value given, or set the clip's `interpolate: false` in the `.vat` |
| `precision_switched` (warning) | half floats were not precise enough: the payload is float (twice the memory) | accept, or `tolerance_mm` higher / `mode: "bones"` |
| `memory_high` (warning) | over the preset's memory warning | as `too_large` |
| `mixed_rest` (warning) | no usable bind pose: the first frame is the rest | none needed; offsets are relative to it |
| `materials_not_vat_ready` (warning) | a project shader fork older than VAT: those meshes draw at rest | re-fork the shader, or use the library shader |
| `material_simplified` (warning) | a material setting the `.vat.glb` can't carry (stochastic tiling, a custom shader) was dropped | set it again on the baked GameObject's material |
| `events_dropped` (warning) | clip events that can't be kept | re-add them in the `.vat` (`write_file`, Inspector) |
| `not_converted` (warning) | a part of the source (instanced copies, an unsupported Animation) is not baked | bake it separately |
| `write_failed`, `bake_failed`, `project_changed` | a job's write failed, an unexpected failure, another project opened during the job | as the fix says (call again; `read_console` for the cause) |
| importer errors | `not_found`, `missing_file` (a texture or the mesh: export with that option, or pass it in `import`), `unsupported` (particle sprites), `read_failed`, `metadata_mismatch`, `unknown_target` (pass `import: {axis}`), `fps_unknown` (pass `fps`), `bad_mesh` | as the fix says |
| importer warnings | `topology_changes` (fluid: absolute positions, no interpolation), `color_animation_ignored`, `stretch_by_velocity_ignored`, `fluid_uv_from_color_ignored`, `animations_list_form`, `clip_out_of_range`, `embedded_textures_dropped`, `node_transform_baked` | informative |

Staleness (`get_asset_info` `staleness`, `check --vat`): `vat-source-changed` (a source file
changed: `rebake`), `vat-source-missing` (restore it, or bake from another source),
`vat-mesh-mismatch` and `vat-layout-mismatch` (the `.vat` and its `.vat.glb` disagree, e.g. a
generated field edited or files from two bakes: `rebake`). `check --vat` adds `vat-device-limit` (the
texture is over the recipe preset's height: 2048 rows for `mobile_web`, the WebGL2 / GLES3
guarantee; rebake smaller or for `desktop`), `vat-invalid` (the `.vat` doesn't parse) and
`vat-format` (an editable field the reader repaired, with its JSON path). With `--files`, an edited
source model also checks the `.vat` files baked from it.

## The asset and the other tools

- `get_asset_info` on a `.vat`: `vertex_animation: {guid, mode, mesh, layout, bounds, clips:
  [{name, frames, fps, length, wrap, speed, interpolate, events}], sockets, stats, recipe,
  warnings, staleness, stale, rebake?, users}` (`users`: the Vertex Animators of the open scenes
  using it). On the `.vat.glb`: `generated_by` (its `.vat`). `list_assets {type:
  "vertex_animation"}` lists the `.vat` files.
- `write_file` / `read_file` work on `.vat` (canonical JSON). Editable: clip `name`, `wrap`
  (`loop|once|ping_pong|clamp_forever`), `speed`, `interpolate`, `events` (`[{time, function,
  args}]`), socket names, removing whole clips (their rows stay in the texture until a rebake).
  Generated (`mode`, `mesh`, `layout`, `bounds`, `stats`, `recipe`, the sockets' order and count,
  clip `start` / `frames` / `fps` / `bounds`, a new clip) are refused with the rebake message
  ("generated field clips[0].frames changed: rebake ..."); empty or duplicate names are refused;
  bad editable values load with `warnings`. The Vertex Animators using it reload. Never
  edit the `.vat.glb`.
- `move_asset` / `delete_asset` keep the `.vat` and `.vat.glb` together (naming either one moves
  both; a rename rewrites the `.vat`'s `mesh`, a copy gets a new GUID; the result's `pair` /
  `with_pair` name the other file); the GUID follows a move, so Vertex Animators keep it. `view_asset` has no image
  for a `.vat`: use the preview's contact sheet or a screenshot.
- `focus_window {window: "vertex_animation_baker"}` shows the Vertex Animation Baker window to a
  human.
- Shell: `jasperengine-editor bake-vat --project . --source <path> [--clips Walk,Run] [--output
  ...] [--dry-run] [--format json]` (the same bake, `--dry-run` = the preview report) and
  `jasperengine-editor check --vat` (staleness, mesh/layout mismatches, device limits per preset;
  `docs/mcp/checks.md`).

## Runtime control

The component (`add_component` / `set_properties` `vertexanimator`): `animation` (the `.vat`;
its GUID follows it), `defaultClip` (empty = the first), `playAutomatically` (true), `speed` (1),
`randomStart` (true: every instance, and a single GameObject, starts at a random time of its
first clip, seeded by entity id and instance index, so crowds never march in lockstep),
`defaultFadeTime` (0.2), `instanceEvents` (false: per-instance events cost O(instances) per
frame), `attachments` (`[{socket, entity}]`: child GameObjects that follow a socket).

Lua (`eval_lua` while playing, or scripts): `GameObject:getVertexAnimator()` (nil without one):
`play(name[, fade])`, `crossFade(name[, fade])`, `blend(a, b, weight)`, `queue(name)`, `stop()`,
`isPlaying([name])`, properties `time`, `normalizedTime`, `speed`, `currentClip`,
`getClipNames()`, `getClipLength(name)`, `getSocketPose(name[, i])` (`{position, rotation,
valid}` in the GameObject's or instance's space); instances (from 0): `playInstance(i, name[,
fade])`, `crossFadeInstance(i, name, fade)`, `setInstanceSpeed(i, s)`, `setInstanceTime(i, t)`,
`getInstanceClip(i)`, `getInstanceNormalizedTime(i)`, `getInstanceCount()`, `playAll(name[,
fade])`, `randomizeTimes(seed)`; `isLoaded()`, `getLoadError()` (`mesh_mismatch: ...` when the
Model isn't the `.vat`'s mesh); callbacks `onEvent(event)`, `onFinish(clip)`,
`onInstanceEvent(i, event)`, `onInstanceFinish(i, clip)`. A clip event `{function: "onStep",
args: ["left"]}` also calls `self:onStep("left")` on the GameObject's scripts (single
GameObjects; instances only through `onInstanceEvent` with `instanceEvents` on).

## Recipes

**Bake hero's Walk/Run/Idle for web and spawn 500 out of sync.**
`bake_vertex_animation {source: "assets/models/hero.gltf", clips: ["Walk", "Run", "Idle"]}` →
check `mode` (Bones for a skinned hero), `bytes`, the sheet → the same plus `output:
"assets/vat/hero.vat", place: {crowd: {count: 500, area: 40, seed: 1}}`. `randomStart` puts each
instance at its own time; give them different clips with Lua: `local v =
object("hero (VAT)"):getVertexAnimator(); for i = 0, v:getInstanceCount() - 1 do
v:playInstance(i, ({"Walk", "Run", "Idle"})[i % 3 + 1]) end`.

**Convert a crowd of Animation Players.** Bake one of them as a GameObject source
(`source: "Crowd/Villager#3"`, the clips of its Animation Player), `output` + `place: "replace"`
for that one, then for the others either `set_properties` their Model to the `.vat.glb`, remove
the Animation Player and add a `vertexanimator` with the `.vat`, or (best) delete them and make
one crowd GameObject (`place: {crowd: ...}`): one draw instead of one per character.

**Import a Houdini destruction.** Copy the ROP's export folder into the project (`import_files`),
then `bake_vertex_animation {source: "assets/fx/Wall"}`: rigid-body exports become Bones (one
bone per piece, weight 1, 4 fetches). Read `upsample` and `interpolation_error_mm` (fast spins are
upsampled); a missing texture is `missing_file` with the export option to turn on. Write with
`output: true, place: "beside"`; the clip is `once`, so the last frame stays. For an export without
metadata pass `fps` and `import: {axis: "unity"}` (the target the ROP was set to).

**Attach a sword to RightHand.** Bake with `sockets: ["Armature/Hips/Spine/Chest/RightShoulder/
RightArm/RightForeArm/RightHand"]` (the node path below the root: `get_entity` or the model's
`get_asset_info` nodes). Single GameObject: `set_properties {vertexanimator: {attachments:
[{socket: "RightHand", entity: "Hero (VAT)/Sword"}]}}` (the sword is a child of the VAT
GameObject). Instances: read `getSocketPose("RightHand", i)` in a script and place the sword with
the instance's transform.

**Rebake after the model changed.** `get_asset_info {path: "assets/vat/hero.vat"}` shows `stale:
true` with `vat-source-changed` and `rebake: {rebake: "assets/vat/hero.vat"}`; call
`bake_vertex_animation {rebake: "assets/vat/hero.vat"}`. Clip renames, events, speeds and wraps
made since are kept; one undo step puts the previous files back. `check --vat` finds every stale
`.vat` of the project from a shell.

## Common mistakes

- A Model that isn't the `.vat`'s `.vat.glb` with a Vertex Animator: `getLoadError()` says
  `mesh_mismatch` and the mesh draws at rest. Use the `.vat.glb` next to the `.vat`.
- Expecting motion in edit-mode screenshots: time stands still in edit mode; play, or set the
  Vertex Animator's `preview: true`.
- Editing `.vat.glb` or generated fields: rebake instead.
- Baking a whole scene root: bake the character (its root GameObject or model), not the level.
