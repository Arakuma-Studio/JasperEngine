# Pose Animation, phase 2: asset, runtime playback, bake, editor document, agent tools

Status: approved design, ready for implementation.
Branch: `pose-animation`. Builds on phase 1 (`2026-10-23-pose-animation-phase1-core-design.md`,
read sections 3-4 first: every type and rule used here is defined there).

## 1. Goal

Make pose animations real project assets that the runtime plays live, that bake to a standard
Animation, and that agents can create and edit through MCP before any window exists:

1. The `.poseanim` file (JSON) and the `.humanoid` sidecar, with load/save in the engine.
2. Building a `pose::Skeleton` and `pose::PoseSkin` from a loaded `ModelComponent`.
3. Pose metrics (`engine/core/pose/PoseMetrics`): feet, contacts, balance, limit hits.
4. The runtime **Pose Animator** component, system and Lua API (live playback, crossfade weight,
   `goToPose` live springs), scene serialisation, C++ scene code generation, export.
5. Baking to an Animation (RotateTracks per bone + hips TranslateTracks) that re-bakes in place.
6. The editor document (`editor/pose/PoseAnimationDocument`) and its undoable edit path, shared by
   MCP now and by the window and viewport later (phases 4-5).
7. MCP `get_pose_animation` and `edit_pose_animation`, their tests, the tool-budget trim, the
   instructions section and the agent guide page.
8. The fixture project with generated humanoid models used by every later phase.

## 2. File formats (contract)

### 2.1 `.poseanim` (UTF-8 JSON, 2-space indent, keys in this order, numbers rounded to 4 decimals)

```json
{
  "format": "jasperengine.poseanim",
  "version": 1,
  "preview_model": "models/hero.gltf",
  "frame_rate": 30,
  "loop": true,
  "feel": {
    "transition": {"curve": "spring", "duration": 0.12, "overshoot": 0.25, "anticipation": 0.06},
    "step_rate": 0, "overlap": 0.05, "looseness": 0.2,
    "weights": {"spine": 1, "head": 1, "arms": 1, "legs": 0}
  },
  "humanize": {"enabled": false, "joint_limits": true, "moving_holds": 0.08, "inbetweens": 0.4,
               "life": 0.6, "seed": 1, "head_follow": 0.5, "torso_follow": 0.4, "springiness": 0.5},
  "poses": [
    {"name": "Contact R", "hold": 0.1, "grounded": true, "lift": 0,
     "transition": null,
     "offset": {"left": 0, "forward": 0.03},
     "angles": {"left_upper_arm.raise": 8, "right_upper_leg.forward": 25}}
  ]
}
```

- `curve`: `spring` | `linear` | `smooth`. `transition: null` = uses the animation's transition;
  an object = custom transition (`customTransition = true`). The fields missing from a custom
  transition object take the `Transition()` defaults (spring, 0.12, 0.25, 0.06), not the animation's
  transition.
- `angles` lists only DOFs whose absolute value is >= 0.0005 degrees, in DOF index order; missing
  DOFs are 0. Unknown DOF names, unknown keys and out-of-range values: load clamps/ignores and
  collects warnings (`PoseAnimationIO::load(text, data, warnings)`), never fails on them; a wrong
  `format` or a `version` greater than 1 fails.
- `preview_model`: project-relative path of the model used for previews when no scene model is
  bound (may be empty).
- Writing is deterministic (same data -> byte-identical text), so undo snapshots and diffs are
  stable.

### 2.2 `.humanoid` sidecar (next to the model file: `models/hero.gltf.humanoid`)

```json
{"format": "jasperengine.humanoid", "version": 1,
 "bones": {"hips": "mixamorig:Hips", "left_toes": ""}}
```

Only overrides are stored: a slot present with a bone name forces that mapping, a slot present
with `""` forces it unmapped; absent slots use auto-detect. `HumanoidMapIO` reads/writes it and
`resolveHumanoid(skeleton, overrides)` = auto-detect then overrides then validation. The sidecar
ships with exports (it is a normal asset file).

Engine code in `engine/core/pose/PoseAnimationIO.{h,cpp}` and `HumanoidMapIO.{h,cpp}` (parse and
write, no engine dependency) uses the engine's bundled nlohmann json (`engine/libs/json`);
`engine/core/pose/PoseFile.{h,cpp}` has the file helpers (`loadPoseAnimationFile`,
`savePoseAnimationFile`, `loadHumanoidOverridesFile` with a `notFound` out-parameter: a missing
sidecar means all-auto, not an error, `saveHumanoidOverridesFile`) on the engine file API so reading
works in exported games (desktop, web, Android, iOS).

## 3. Engine: rig from a model, metrics

### 3.1 `pose/ModelRig.{h,cpp}`

`bool buildSkeleton(Scene*, Entity model, pose::Skeleton&, std::vector<Entity>& boneEntities)`
from `ModelComponent.skeleton` / bone entities (`BoneComponent` bind TRS as the rest local TRS,
entity names as bone names, parents from the Transform hierarchy restricted to bones).
`std::shared_ptr<PoseSkin> buildSkin(Scene*, Entity model, const Skeleton&, ...)` from the CPU
copy of the skinned meshes' positions, joints and weights (find where the engine keeps them after
upload; if a platform frees CPU buffers, keep a sample copy at load time for models whose entity
has a PoseAnimator, or fall back to the sole estimate: `skin == nullptr`). A
`PoseRigCache` keyed by (model file path, sidecar modification stamp) shares rigs between
animators using the same model.

### 3.2 `pose/PoseMetrics.{h,cpp}`

```cpp
struct FootMetrics { float height; bool contact; Vector3 position; };   // canonical, humanScale units above the floor
struct PoseMetrics { FootMetrics left, right; float lowest; Vector3 centerOfMass;
                     bool balanced; float balanceMargin; std::vector<int> limitHits; };
PoseMetrics computeMetrics(const PoseRig&, const HumanPose&, const BodyPose&);
```

- Foot height: the lowest of the foot's sole points (foot and toes bones with the sole offsets,
  or the skin vertices weighted > 0.5 to that foot/toes) above the floor, humanScale units;
  `contact` when height <= 0.02.
- Centre of mass: segment-weighted (head+neck 8%, trunk 50% spread over hips..upper chest,
  upper arm 2.7%, forearm 1.6%, hand 0.6%, thigh 10%, shank 4.6%, foot 1.4%; segment midpoints).
- Support: the convex hull of the contact feet's heel (foot bone) and toe (toes bone, or the foot
  tip estimated from the rest foot length) points projected on the floor. `balanced` = the
  centre of mass projection lies inside; `balanceMargin` = signed distance to the hull edge
  (humanScale units, negative outside); no contact -> `balanced = false`, margin = -distance to
  the nearest foot point.
- `limitHits`: DOF indices outside the 4.5 limits (before any clamping).

## 4. Runtime: Pose Animator

### 4.1 Component (`component/PoseAnimatorComponent.h`)

```cpp
struct PoseAnimatorComponent {
    std::string animation;   // project-relative .poseanim
    bool autoplay = true;
    float speed = 1.0f;
    float weight = 1.0f;     // 0..1 blend over whatever the ActionSystem wrote this frame
    // runtime only (not serialised)
    bool playing = false; float time = 0; bool needsLoad = true;
    std::shared_ptr<pose::PoseAnimatorRuntime> runtime;
};
```

Lives on the Model entity. Registered like every component (`EntityRegistry`, Catalog
properties `animation`, `autoplay`, `speed`, `weight`, `Stream` encode/decode under the YAML key
`poseanimator`, `Factory` C++ scene code generation, Inspector section, Add Component menu under
Animation, `docs/unity-glossary.md` row). The Inspector section shows the asset field (with
drag-and-drop of `.poseanim` files), autoplay, speed, weight, and a **Bake** button (2.5) and an
**Open** button (phase 4 makes it open the window; until then it is hidden).

### 4.2 System (`subsystem/PoseAnimatorSystem.{h,cpp}`)

Registered right after `ActionSystem` in `Scene::init`, so it runs after the clip blend is
flushed and before transforms and skinning are computed. Per animator:
- Load when `needsLoad` and the model's skeleton exists (models load asynchronously: retry each
  frame until ready; a missing/invalid file logs one error and disables the animator until the
  path changes). Loading builds/gets the rig, realises every key with
  `PoseRig::fromBodyPose(key.body, skin, key.grounded, key.lift)` (joint limits first when
  `humanize.jointLimits`), and computes the frames at `frame_rate` (`pose::frames`, which already
  includes in-betweens, follow-through and limits).
- Playback: `time += dt * speed` while playing; loop wraps, one-shot clamps at the end and stops
  (`playing = false`, `onFinish` Lua callback if set). The displayed pose is the slerp between the
  two frames around `time` (hips position lerped); with `stepRate > 0` the previous frame (no
  interpolation).
- Live transition (`goToPose`): from the currently displayed human pose to a key's realised pose
  with the given transition, using the phase 1 ease per bone (with overlap/looseness from the
  settings), then holds the key pose. Playback stops while a live transition is active.
- Apply: for every mapped slot bone: `local = slerp(current, pose, weight)`; hips position lerped
  the same way; unmapped bones untouched; mark transforms dirty.
- Editor: animators do not run in edit mode (the Pose Animation window drives previews itself,
  phases 4-5); they run in play mode and exported games.

### 4.3 Lua API (`script/binding/*`, stubs via `generate_lua_stubs.py`, `docs` Lua reference)

`PoseAnimator(scene, entity)` (adds the component if missing) and `model:getPoseAnimator()`:

| method | meaning |
|---|---|
| `setAnimation(path)` / `getAnimation()` | asset path (reloads) |
| `play()` `pause()` `stop()` `isPlaying()` | stop resets time to 0 and shows pose 0 |
| `setTime(t)` `getTime()` `getLength()` | seconds |
| `setSpeed(s)` `getSpeed()` `setWeight(w)` `getWeight()` | |
| `getPoseCount()` `getPoseName(i)` `findPose(name)` | 0-based index; `findPose` returns -1 if absent |
| `goToPose(indexOrName [, duration [, overshoot [, anticipation]]])` | live spring transition, then hold |
| `isLoaded()` | rig built and frames ready |
| `onFinish` (property, function) | called when a one-shot ends |

Same API on the C++ class `PoseAnimator` (`object/PoseAnimator.{h,cpp}`, an `EntityHandle`).

### 4.4 Export

`.poseanim` and `.humanoid` files ship with the project assets (check `Exporter::copyAssets`
filters and the web `.data` packaging). The generated scene code sets the component fields.

### 4.5 Bake (editor, `editor/pose/PoseBaker.{h,cpp}`)

`bake(sceneProject, modelEntity, document, name)`: frames at `frame_rate` on the model's rig;
creates or updates the Animation named `name` (default: the asset file stem) in the model's
`ModelComponent.animations`: one RotateTracks per mapped slot bone and one TranslateTracks for
the hips, one key per frame at `f / frame_rate`, step easing when `stepRate > 0` else linear,
`loop` from the asset, duration = the animation length. Re-baking finds the Animation by name
and replaces its tracks' keys in place (same entities, so references and the Animation window
selection survive). Everything goes through editor commands as **one scene undo step**. The
result plays with `Model:playAnimation(name)` and in the Animation window.

## 5. Editor document and undo (`editor/pose/PoseAnimationDocument.{h,cpp}`)

```cpp
class PoseAnimationDocument {
public:
    static std::shared_ptr<PoseAnimationDocument> open(Project*, const std::string& path, std::string* error); // shared per path
    const pose::PoseAnimationData& data() const;  uint64_t revision() const;
    // the single edit path: validates, clamps (reports), writes the file through the project
    // command history as one undo step; mergeKey != "" merges with the previous edit of the same key (drags)
    bool commit(const pose::PoseAnimationData& newData, const std::string& undoName, const std::string& mergeKey,
                std::vector<pose::ClampReport>* clamped, std::string* error);
    // realisation on a rig (preview model or a bound scene model), cached by revision + rig
    std::shared_ptr<const PoseRealisation> realise(const std::shared_ptr<pose::PoseRig>&) ;   // keys, frames, metrics
    void reloadFromDisk();          // file changed outside the editor (McpAutoSync, Project window)
    void addListener(...);          // window/viewport refresh on change, undo and redo
};
```

- Undo uses the existing project-scope file write command (`WriteFilesCmd` in
  `editor/mcp/tools/W4Shared.h`, or a sibling command in `editor/command/type/` if it must be
  usable outside MCP: move it, do not duplicate it). Undo/redo rewrites the file and notifies the
  document. Ctrl+Z in the editor and MCP `undo` with `scope: project` both undo pose edits.
- The editor loads preview models off-scene (like `editor/render/preview/ModelRender`) to build
  rigs for the preview model: `PoseRigProvider` returns a rig (and skin) for a model file path,
  cached, rebuilt when the file or sidecar changes.

## 6. MCP tools

Module `editor/mcp/tools/PoseTools.cpp` (`registerPoseTools`), wired into `McpTools.h`,
`McpServer::registerAllTools`, `CMakeLists.txt`, `tools/mcp/gen_tool_docs.py` MODULES; regenerate
`docs/mcp/TOOLS.md`. Follow `docs/mcp/DESIGN.md` (structured errors naming the argument path,
one undo step per call, rollback on failure, terse descriptions, shapes in the schema).

### 6.1 `get_pose_animation` (read-only)

Arguments: `path` (required), `poses` (array of indices; default all), `model` (scene entity
whose rig to use instead of the preview model), `detail`: `summary` | `full` (default `full`).

Result (structured; the text summary is one line per pose):
`{path, preview_model, rig: {model, mapped, missing: [slots], human_scale}, loop, frame_rate,
length, frame_count, feel: {...as the file}, humanize: {...}, preset: name|null,
poses: [{index, name, hold, grounded, lift, transition|null, start_time, offset: {left, forward},
angles: {dof: degrees >= 0.05 abs, rounded to 0.1}, metrics: {feet: {left: {height, contact},
right: {...}}, balanced, balance_margin, limit_hits: [dof names]}}]}`.
`summary` omits `angles`. Angle names follow 4.5; the description says "degrees, 0 = neutral
standing; see jasper://docs/pose-animation for the DOF table".

### 6.2 `edit_pose_animation`

Arguments: `path` (required), `create` (object, optional: `{preview_model, preset}`: creates the
file; fails if it exists), `ops` (array, applied in order, all-or-nothing), `bake` (object,
optional: `{model: entity, name}`: bakes after the ops as its own scene undo step).

Ops (`op` field + arguments; `index` is 0-based; `angles` sets absolute degrees for the named
DOFs, `add_angles` adds degrees; other DOFs keep their value):

| op | arguments |
|---|---|
| `add_pose` | `after` (-1 = first; default last), `name`, `from` (pose to copy; default `after`, or neutral when the animation is empty), `blend_to`, `blend` (0..1: angles and offset lerped, lift lerped), `hold`, `grounded`, `lift`, `offset`, `angles`, `add_angles`, `transition` (object or null) |
| `update_pose` | `index` + any of `name`, `hold`, `grounded`, `lift`, `offset`, `angles`, `add_angles`, `transition` (null clears the custom transition). Metadata changes never alter angles or offset |
| `delete_pose` | `index` (refuses to delete the last pose) |
| `move_pose` | `index`, `to` |
| `mirror_pose` | `index`, `as_new` (insert after), `name` |
| `set_settings` | `loop`, `frame_rate`, `transition`, `step_rate`, `overlap`, `looseness`, `weights {spine, head, arms, legs}`, `humanize {...}` (partial) |
| `apply_preset` | `name` (phase 1 table) |
| `auto_timing` | none |
| `set_preview_model` | `model` (asset path) |
| `set_humanoid` | `model` (asset path), `bones {slot: bone name or ""}`: writes the sidecar (same undo step) |

Result: `{path, pose_count, length, changes: [one line per op], clamped: [{field, requested,
applied}], warnings: [...], poses: [the touched poses, `full` shape]}` plus the bake result
`{animation: entity ref, frames, tracks}` when baking. Errors name the op:
`ops[2].angles: unknown DOF 'left_elbow.bend' (did you mean 'left_lower_arm.bend'?)`.
Joint limits: when `humanize.joint_limits` is on, added/updated poses are clamped and the
clamped DOFs are reported in `clamped`.

### 6.3 Budget, instructions, guide

- Keep the total tool-definition size within the test limit (`tools/mcp/tests/test_html_ui_tools.py`
  `TOTAL_LIMIT`) by trimming other descriptions (no behaviour change); add a per-tool budget line for
  the new tools in that test (as HTML UI phase 4 did). Phase 3 adds `render_pose_animation`; reserve
  room for it now (target: the two phase 2 tools plus ~2.5K chars for phase 3 fit under the limit).
- `McpInstructions.cpp`: a short "Pose animation" paragraph (what it is, the loop: get -> edit ->
  render -> check metrics, angles are degrees from neutral standing, mirror instead of
  hand-mirroring), keeping the guide under its size limit; the instructions test (every tool and
  argument named must exist) must pass.
- `docs/mcp/guide/pose-animation.md` -> `jasper://docs/pose-animation`: the DOF table with signs and
  meanings, limits, presets, ops, workflow, common mistakes.

## 7. Fixture project (`tools/fixtures/pose-animation/`)

- `make_humanoids.py` (committed, stdlib only) generates two skinned glTF humanoids (embedded
  buffers, one mesh of capsule/box limbs, 4-weight skinning, a 1.75 m tall character):
  `assets/models/hero_tpose.gltf` (Mixamo bone names `mixamorig:Hips` ..., T-pose, faces +Z,
  includes fingers and a head-top end bone) and `assets/models/robot_apose.gltf` (Blender-style
  names `pelvis`/`spine.001`/`upper_arm.L`, A-pose 40 degrees, 1.2x arm length, 0.85x leg length,
  twist helper bones between upper and lower arms, no upper chest). The script output is
  committed; rerunning it reproduces the files byte for byte.
- A project (`project.yaml`, a scene with both models standing side by side, a camera and a light)
  usable by the MCP harness and by the phase 3-6 tests.

## 8. Tests

- `jasperengine-pose-tests` (phase 1 target) gains: IO round trip (write -> load -> write is
  byte-identical; clamping and warnings; unknown DOF; bad format), sidecar overrides, metrics
  (balanced neutral; one-foot stance; airborne pose), the realisation helper.
- `tools/mcp/tests/test_pose_animation.py` (real editor under Xvfb, fixture project of 7):
  create -> add poses -> get (angles, metrics) -> update metadata keeps angles -> mirror twice ->
  move/delete -> set_settings clamping echoed -> preset -> auto_timing -> one undo per call
  (project scope) and rollback on a failing op -> set_humanoid writes the sidecar -> bake creates an
  Animation with the right tracks, re-bake keeps the entity ids, scene undo removes it -> the same
  asset on the A-pose robot -> play mode: a scene with a PoseAnimator plays (bone rotations change
  over time; `goToPose` via `eval_lua` reaches the key pose; weight 0 leaves the clip pose).
- The full existing MCP test suite still passes (`tools/mcp/run-tests.sh -j 4`), including the
  budget and instructions tests.
