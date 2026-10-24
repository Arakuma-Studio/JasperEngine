# Animation Clips: shared keyframe clip assets (Unity's AnimationClip + legacy Animation component)

Status: approved direction (owner: "100% complete, like Unity's, best UX for humans and AI agents"), revised after the
adversarial review (`/tmp/clip-research/reports/SPEC-REVIEW.md`): **section 9 is binding and overrides any earlier text
it contradicts; section 8 is the work breakdown.** Branch `animation-clips` (from pose-animation, which contains main). Research behind every decision:
`/tmp/clip-research/reports/R1-unity-reference.md` (Unity/Godot/Unreal), `R2-engine-runtime.md`, `R3-editor.md`,
`R4-agent-surface.md` (read the parts your task touches; they carry file:line maps).

## 1. Goal and non-goals

Today a keyframe animation is an `AnimationComponent` entity whose tracks are child entities targeting scene entities
by id, saved inside one .scene/.prefab: it can't be shared, every glTF model instance copies every key, and editing a
clip used in two places means editing two copies. After this work:

- A **clip is a project asset** (`.anim`, JSON) that binds tracks by **path relative to the animated root**, so one
  clip plays on any hierarchy with the same child names (two prefabs, two scenes, two characters of one rig), and an
  edit to the file changes every user, in the editor and in builds.
- A new **Animation Player** component (Unity's legacy `Animation` component) on the root lists clips, plays them with
  fades, crossfades, blends, queues and wrap modes, fires **clip events** into Lua, and is driven from Lua and MCP.
- glTF/FBX animations are usable as **read-only sub-asset clips** (`assets/models/hero.gltf#Walk`) and can be
  **extracted** to editable `.anim` files; existing entity Animations **convert** to clips; pose animations **bake** to
  clips.
- The **Animation window** edits clips Unity-style (binding rows, Add Property tree, missing bindings in yellow,
  record mode, dopesheet + curves, events lane, frames/samples, multi-select, copy/paste, shortcuts), with project
  undo; the **Inspector** shows the player and a selected `.anim` asset; the **Project window** creates, opens,
  drags and renames clips safely.
- Agents get the same power through the existing tools (`edit_animation`, `get_animation`, `preview_animation`
  gain `clip`), validation of hand-written files (write_file, auto-sync `external_changes.problems`, `check --anims`)
  and a guide (`jasper://docs/animation`) — within the tool-definition and instruction budgets.

Non-goals (now): an Animator state machine/controller, humanoid (muscle) clips, generic component-property curves
(`Light.intensity`, material colours: the format reserves `channel: "property"`, see 2.3), root motion, additive
layers, avatar masks. The format leaves room for all of them (2.5).

Backward compatibility is absolute: existing scenes, prefabs, entity Animations (sequences with tweens, sprite
animations, particles, nested animations), glTF entity animations, `Model:playAnimation`, every MCP tool call that is
valid today and every existing test keep working unchanged unless a section below says otherwise.

## 2. The clip asset (`.anim`)

### 2.1 File format (canonical, deterministic, agent-writable)

UTF-8 JSON, 2-space indent, fixed key order, numbers written with the shortest round-trip form rounded to 6
significant decimals (times rounded to 1e-6 s), one key per line, tracks sorted by (path, channel), events sorted by
time then function. Writing the same data twice gives identical bytes; reading then writing a canonical file is a
no-op. Example:

```json
{
  "format": "jasperengine.anim",
  "version": 1,
  "frame_rate": 30,
  "wrap": "loop",
  "length": 1.0,
  "tracks": [
    {"path": "", "channel": "position", "keys": [
      [0, [0, 0, 0]],
      [0.5, [0, 1, 0], "quad_out"],
      [1, [0, 0, 0]]
    ]},
    {"path": "Arm/Hand", "channel": "rotation", "relative": true, "keys": [
      [0, [0, 0, 0, 1]],
      [1, {"euler": [0, 90, 0]}]
    ]},
    {"path": "Body", "channel": "morph", "keys": [[0, [0, 0.5]], [1, [1, 0]]]},
    {"path": "Cage", "channel": "lattice", "keys": [[0, {"points": [[13, [0, 0, 0]]]}], [1, {"points": [[13, [0, 0.2, 0]]]}]]}
  ],
  "events": [
    {"time": 0.5, "function": "onFootstep", "args": ["left", 1]}
  ],
  "preview": {"model": "assets/models/hero.gltf"}
}
```

- `format` and `version` are required; anything else has a default. A newer `version`, a different `format`, a
  non-object root or non-JSON text fail to load (with `line:col` for JSON syntax errors). Unity YAML (`%YAML`,
  `AnimationClip:`) is recognised and refused with: "This is a Unity .anim (YAML). JasperEngine clips are JSON
  {"format": "jasperengine.anim"}: create one with Create > Animation Clip or edit_animation {clip, create}."
- Everything else is **lenient with warnings** carrying a JSON path (`tracks[3].keys[2].value: expected [x, y, z]`):
  the bad key/track/event is dropped, the rest loads. Duplicate (path, channel) tracks merge (later keys win at equal
  times, warning). Keys are sorted by time on load (warning when they weren't); keys closer than 1e-6 s collapse.
- `frame_rate` (samples, default 30, 1..240): the snapping grid and frames display only; keys stay in seconds and a
  rate change never moves keys (the window offers "Snap keys to frames").
- `wrap`: `once` (default; plays to the end and stops, the player resets that state's time to 0), `loop`,
  `ping_pong`, `clamp_forever` (holds the last frame and keeps the state playing, Unity's ClampForever).
- `length`: optional; default = last key or event time over all tracks. An explicit length longer than the last key
  holds the end pose; shorter clips cut tracks there.
- Track keys: `[time, value]` or `[time, value, ease]`. `ease` is the easing of the segment that starts at that key
  (the existing `EaseType` names: `linear` default, `step`, `<quad|cubic|quart|quint|sine|expo|circ|back|elastic|bounce>_<in|out|in_out>`).
  Object form `{"time", "value", "ease", "in", "out"}` is also read; `in`/`out` are Hermite tangents (glTF CUBICSPLINE
  semantics, value units per second); the writer uses the object form only for keys that carry tangents.
- Values per channel: `position`/`scale` `[x, y, z]`; `rotation` quaternion `[x, y, z, w]` or `{"euler": [x, y, z]}`
  degrees (read; written back as normalized quaternions with hemisphere continuity enforced along the track);
  `morph` an array of weights (one per morph target; shorter arrays mean 0 for the rest); `lattice`
  `{"points": [[flatIndex, [x, y, z]], ...]}` sparse offsets with flat indices (the cage resolution is not stored;
  indices outside the bound cage are reported as binding problems, 4.3).
- Track options: `relative` (position/rotation/scale only: values add to the target's pose when the state starts,
  as the existing KeyframeTracks `relative`), `loop` (the track's own keys repeat within a longer clip; existing
  KeyframeTracks semantics).
- `events`: `{time, function, args}`; `args` an array of JSON scalars (number, string, bool), at most 8; `function`
  a Lua identifier. Fired as described in 3.5.
- `preview`: editor-only hints (`model`: a model or prefab path to preview the clip on when nothing binds it).
  Ignored by the runtime.
- `source` (written by Extract/Convert/Bake only): `{"kind": "model"|"entity"|"pose", "path": ..., "name": ...}` so
  the window can offer "Re-sync from source" for model extractions. Ignored by the runtime.
- Unknown top-level keys are preserved on rewrite (forward compatibility), warned once.

### 2.2 Binding paths

- A path is names from the root joined by `/`, with `#N` selecting the N-th later sibling of the same name (exactly
  `editor/util/EntityPath` grammar, `#0` never written); `""` is the root itself. Names containing `/` can't be
  addressed (documented; the window and tools warn when a clip needs one).
- Resolution walks Transform children in order from the player's entity. A name that is ambiguous where `#N` is not
  given binds the first match and is reported as ambiguous (edit-time warning).
- The engine gets its own resolver (`engine/core/animation/ClipBinding.*`), the editor's `EntityPath` gains
  `relative(registry, root, entity)` / `findRelative(registry, root, path)` that call or match it (one grammar).
- `EntityRegistry` gains a hierarchy revision counter (bumped by create/destroy entity, addEntityChild, moves,
  setEntityName); players re-resolve when it changes (cheap per-frame compare), so renames/reparents in edit or play
  mode rebind live. Unresolved bindings are silently skipped at runtime (Unity), loud at edit time (4.3).

### 2.3 Channels (v1) and the reserved one

v1 channels are exactly today's keyframe kinds: `position`, `rotation`, `scale` (Transform of the bound entity),
`morph` (MeshComponent morph weights), `lattice` (LatticeComponent offsets). `channel: "property"` with
`{"component": "...", "property": "..."}` is reserved: the loader keeps such tracks (preserved on rewrite) and warns
"property tracks are not supported yet"; nothing samples them.

### 2.4 Sub-asset clips of models

`<model path>#<animation name>` (or `#<index>` when the glTF animation has no name; names are the glTF/FBX import
names the Inspector shows) names a read-only clip built from the model's animation, with paths relative to the model
root exactly as `MeshSystem::loadGLTF` builds its entity hierarchy (bones under the skeleton, mesh nodes), frame rate
30, wrap from the importer default (once). Users reference them like files. They are never written; the window shows
"(Read-Only)" and offers **Extract to .anim**. The builder runs on the pooled tinygltf model and must be thread-safe
(the shader-variant probe loads models off the UI thread).

### 2.5 Reserved for later (do not implement, do not block)

`cycle_offset`, `loop_pose`, `range` (start/stop), `root` (root-motion node), `curves` (named float curves),
`additive_reference`. The loader preserves unknown keys, so a later version can add them.

## 3. Runtime (engine)

### 3.1 Data and cache

- `engine/core/animation/AnimationClip.{h,cpp}`: immutable `AnimationClip` (frameRate, wrap, length, tracks, events,
  warnings) plus per-track typed arrays (times, easings, values, optional tangents) in contiguous storage.
- `AnimationClipIO.{h,cpp}`: `parse(text, &warnings) -> optional<ClipData>`, `write(data) -> text` (canonical),
  shared by engine, editor document, MCP and CLI (one parser).
- Samplers: the per-type sampling now in `ActionSystem.cpp` (slerp/Hermite rotation with zero-norm guard, vector
  Hermite, morph, lattice, easing, relative) becomes pure functions over const arrays with an explicit key cursor
  (`engine/core/animation/KeyframeSampling.{h,cpp}`); ActionSystem's entity tracks call the same functions (no
  behaviour change, covered by the existing ActionTracks/ActionTiming tests).
- `AnimationClipPool` (`engine/core/pool/`): `shared_ptr<const AnimationClip>` keyed by normalized path; loads through
  `Data::open` (resource packs work); mutex-guarded; `invalidate(path)` for editor edits; `clearUnused`; registered in
  `Engine::clearPools/clearUnusedPools`. Model sub-asset clips are cached under `"<model key>#<name>"` and invalidated
  with the model (MeshSystem::invalidateModelFile).
- Preloading: `SceneResources` gains clips (editor `collectResourcesInRegistry` fills them).

### 3.2 Animation Player component

`AnimationPlayerComponent` (display "Animation Player", Add Component > Animation > Animation Player, Lua
`AnimationPlayer`, `object:getAnimationPlayer()` on GameObject wrappers). One component (Signature has 64 bits, 59
used). ComponentType appended after `PoseAnimatorComponent`; `LastComponentType` moves.

Authored (serialized) fields:
- `clips`: list of `{clip: "<asset-relative path or model#name>", name: "" (state name; default = file stem or
  sub-asset name)}`. Duplicate state names: later ones get " 2", " 3" at load with a warning.
- `defaultClip` (state name; empty = the first clip), `playAutomatically` (default true: the default clip plays when
  play mode / the game starts), `speed` (1, multiplies every state), `defaultFadeTime` (0.25 s, used by
  `Model:playAnimation` and `play(name, fade)` without an explicit fade), `cullWhenHidden` (false; when true and no
  MeshComponent under the root was visible last frame, time advances but sampling is skipped — Unity's "Based On
  Renderers").
- No other fields in v1 (no flags that do nothing).

Runtime (not serialized): per state `{time, speed, weight, targetWeight, fadeSpeed, enabled, wrap override, playing,
queued}`; per clip track the resolved Entity binding and relative base; rest values per bound channel (captured at
bind: bones use their bind pose, others their current local TRS / morph / lattice values); key cursors; hierarchy
revision; load state (needsLoad, failed + retry every 1 s, error logged once — PoseAnimator pattern); `onEvent`,
`onFinish` FunctionSubscribe callbacks.

### 3.3 Playback and blending

- A new system step inside ActionSystem (players are sampled in `ActionSystem::update` after entity animations and
  before `flushPoseBlend`, so PoseAnimatorSystem still blends over the result and existing crossfades with entity
  animations keep working). Main thread only.
- Play semantics (Unity legacy): `play(name)` stops other states (StopSameLayer: there is one layer in v1) and plays
  `name` from 0 at weight 1; `crossFade(name, fade)` fades others out and `name` in over `fade`; `blend(name, weight,
  fade)` moves one state's weight without touching others; `queue(name, fade?)` plays after the current state(s)
  finish (CompleteOthers); `stop(name?)`, `rewind(name?)`, `sample()` (apply current times once, e.g. while paused),
  `isPlaying(name?)`. Wrap per clip (`state.wrap` overrides the clip's). `speed` multiplies state speed; negative
  speeds play backwards (once: stops at 0).
- Blending per bound channel: `result = (Σ wᵢ·vᵢ + max(0, 1 − Σ wᵢ)·rest) / max(1, Σ wᵢ)` (rotations: weighted
  quaternion sum with hemisphere alignment, normalized; morph/lattice componentwise). This fixes the
  "fade-in from rest snaps" limitation for players (R2 §2.4) without changing entity-animation behaviour: player
  contributions are pre-combined per target with their rest term and enter the shared accumulator as one
  contribution of weight 1.
- When every state has stopped (or the component is disabled/removed) the bound channels keep the last sampled pose
  (Unity). Edit-mode preview always restores the snapshot taken when it started (4.4).
- Events (3.5) fire when a state's time crosses them (forward or backward, including wrap; at most one firing per
  event per crossing, also at low frame rates), only for states with weight ≥ 0.5 of the total or the dominant state
  (no double footsteps during crossfades; Unity pain point).
- Determinism: `sample(t)` of a clip is a pure function of t (no dependence on previous frames except key cursors,
  which are hints).

### 3.4 Lua API

`AnimationPlayer` class: `play(name)`, `play(name, fade)` (= crossFade when fade > 0), `crossFade(name, fade)`,
`blend(name, weight, fade)`, `queue(name)`, `queue(name, fade)`, `stop()`, `stop(name)`, `rewind(name)`, `sample()`,
`isPlaying()`, `isPlaying(name)`, `getState(name)` → table-like userdata with `time`, `normalizedTime`, `speed`,
`weight`, `length`, `wrap` ("once"|"loop"|"ping_pong"|"clamp_forever"), `enabled` (read/write where Unity allows),
`addClip(path)`, `addClip(path, name)`, `removeClip(name)`, `getClipCount()`, `getClipName(i)`, `speed`,
`defaultClip`, `onEvent` (subscribe `function(name, args)` where args is a Lua table), `onFinish(function(stateName))`.
`Model:playAnimation(name[, fade])`/`stopAnimations` look for an Animation Player on the model's entity first and
fall back to entity animations (existing behaviour unchanged when there is no player). Bindings in
`engine/core/script/binding/`, stubs via generate_lua_stubs.py (no `-- unresolved:` lines), callbacks cleared in
`Engine.cpp` with the others.

### 3.5 Events

An event calls, in order: every `onEvent` subscriber with `(functionName, argsTable)`, then the Lua method
`functionName` on each Lua script attached to the player's entity that defines it, with the args spread as
parameters (extend `LuaBinding::callLuaScriptFunction` with an args variant). An event whose function exists on no
script and has no subscriber logs a warning once per (clip, function) in play mode. Edit-time validation (4.3)
reports missing handlers against the scripts on the bound root.

### 3.6 Cloning, prefabs, export, threading

- EntityCloner: copy authored fields, reset runtime (bindings re-resolve on the copy's own hierarchy), clear callbacks.
- Prefab overrides: `clips` list and scalar fields are normal property overrides.
- Factory emitter (C++ export) emits the authored fields only; the exporter already copies every asset file.
- Decoding never loads (play startup decodes on a worker thread); loading happens in the system update (main thread)
  or in the mutex-guarded pool; the glTF sub-clip builder is safe off the main thread.

## 4. Editor

### 4.1 Clip document and plumbing (`editor/animation/`)

- `AnimationClipDocument`: a clone of `PoseAnimationDocument`'s design (one shared document per file, deterministic
  text, `commit(newData, undoName, mergeKey)` → `WriteFilesCmd` on the **project** history, merge keys for drags,
  1 s disk poll + `filesChanged`, listeners Edited/Reloaded/Removed, read-only documents for model sub-clips). Every
  committed or reloaded revision invalidates `AnimationClipPool` for the path and marks players referencing it in open
  scenes for rebinding (edit-mode preview and play mode both see edits immediately).
- `Util::isAnimationClipFile` (case-insensitive `.anim`), `isAssetFile`, `FileDialogs` flag, `ProjectWindow::FileType
  ANIMATION_CLIP` with an icon (FontAwesome film in the style of the existing generated icons), Create > Animation
  Clip (panel and main menu; undoable like Create Pose Animation), double-click opens the Animation window on the
  clip, drag-drop: onto a Scene-view/Hierarchy object adds (or reuses) an Animation Player and appends the clip (one
  scene undo step); onto the Inspector clip list; onto the Animation window (opens it / adds to the bound player).
- Rename/move/delete: references in loaded scenes, prefab stages and prefab assets are remapped
  (`visitAssetPathsInRegistry` + RenameFileCmd/CopyFileCmd/DeleteFileCmd), **and unopened .scene/.prefab files keep
  working**: reuse whatever mechanism model/material references use for closed files if one exists; otherwise
  rewrite clip paths in closed scene/prefab files as part of the same project undo step. Test it.
- McpAutoSync tracked extensions + `syncFiles` reload; clip load problems go to `external_changes.problems`
  ({path, line, message}); do the same for `.poseanim` (current gap, R4 §3.4).

### 4.2 Inspector

- **Animation Player** section: clip list (rows: state name, file field with picker/drop/clear, "(Read-Only)" tag for
  sub-clips, yellow `Missing (name)` when the file is gone, Open-in-window button), `+ Add Clip`, `Create New Clip...`,
  default clip combo, Play Automatically, Speed, Default Fade, Cull When Hidden; a **Bindings** foldout (resolved n/m,
  missing rows in WarningText with "did you mean <nearest>" and a Retarget action that rewrites the clip's path — a
  project step that changes every user, so it says "changes the clip for all N users"); in play mode, live state rows
  (time bar, weight, playing).
- **Asset Inspector for a selected `.anim`** in the Project window (none exists today: add the minimal hook so the
  Inspector can show an asset when the Project selection is a single clip and no GameObject is selected): clip
  settings (frame rate, wrap, length/auto), tracks summary (path : channel, key count), events list, preview model
  field, users (components in loaded scenes/prefabs), warnings, Open in Animation Window, Extract/Re-sync for
  sub-clips. Edits go through the document.
- **AnimationComponent (entity sequence)** section gains **Convert to Clip...** (4.5) and a link "Open in Animation
  window". **Model** section's animations table gains per-row **Extract to .anim** and "Add to Animation Player".

### 4.3 Validation (shared by window, Inspector, MCP, CLI)

`ClipValidation` (editor, pure functions over clip data + an optional bound root): per track OK / missing path
(nearest names by edit distance over the root's descendants) / ambiguous path / missing component (no Transform, no
morph targets, no LatticeComponent) / lattice index outside the cage; per event handler found / not found on the
root's scripts. Never mutates.

### 4.4 Animation window (clip mode)

The window gains a clip mode used whenever the selected GameObject (or its nearest ancestor) has an Animation
Player, or a clip was opened from the Project window/Inspector; the existing entity-Animation mode stays for
AnimationComponent sequences (tweens, sprites, nested), unchanged except that opening the window never marks a scene
modified (`autoAssignTracks` must not dirty).

Clip mode, matching Unity and fixing its pain points:
- Clip dropdown: the player's states, separator, **Create New Clip...** (save dialog in the root's model/prefab
  folder; adds to the player); the read-only banner for sub-clips with **Extract to .anim**. Target-less editing: a
  clip opened without a bound object edits the file and previews on `preview.model` when set ("Preview on" field).
- Empty state when the selection has no player: icon + "To begin animating **<Name>**, create an Animation Clip." +
  **Create...** (adds only an Animation Player and the clip: no hidden assets).
- Rows = bindings `Path : Channel` with foldouts for components (x/y/z, morph target names, lattice point count),
  an inline value column at the playhead (editing it keys when Record is on, otherwise previews the change like Unity's
  pink fields), missing bindings in WarningText with "(Missing!)" and a right-click Rebind/Remove.
- **Add Property**: searchable tree of the bound root's hierarchy → Position / Rotation / Scale / Blend Shapes /
  Lattice; adds a track with a key at the playhead holding the current value.
- Keys: click, Shift/Ctrl multi-select, box select, drag (time, snapped to frames), Delete, Ctrl+C / Ctrl+V (paste at
  playhead onto matching bindings), Ctrl+D duplicate, right-click: Add Key, Delete Keys, Interpolation (Linear, Step,
  ease presets), Copy/Paste; all edits are one document commit each (drags merge).
- Toolbar: Preview (blue timeline), **Record** (red timeline; Scene-view gizmo drags and Inspector edits on bound
  entities write keys at the playhead, one merge key per drag, scene left untouched by restoring on exit), first /
  prev key / play / next key / last, frame field (frames or seconds per the Seconds/Frames toggle), Samples (frame
  rate), Snap keys to frames, zoom.
- **Curves view** (Dopesheet / Curves toggle): sampled curves per component with the runtime samplers, key values
  draggable vertically, segment ease from the context menu, F frames selection / A all. (Free tangent handles are out
  of v1: the format reads/writes tangents, the view shows them.)
- **Events lane** under the ruler: Add Event at playhead, drag to retime, select → small editor (function with a
  combo of methods found on the root's scripts, args list), missing handler shown in WarningText.
- Shortcuts while the window is focused: Space play/pause, K key all bound channels, Shift+K key changed channels,
  `,`/`.` prev/next frame, Alt+`,`/`.` prev/next key, Delete, Ctrl+C/V/D, F/A, R record toggle, Ctrl+Z/Y on the
  project history (window owns undo like the Pose window).
- Preview never changes saved scene state: bound targets are snapshotted on start and restored on stop, scene
  modified flag untouched.

### 4.5 Converters (shared by UI and MCP, `editor/animation/ClipConverters.*`)

- **Convert to Clip** (entity Animation → .anim): keyframe tracks only, times shifted by frame startTime, targets
  turned into paths relative to the chosen root (default: the Animation's target), loop/duration mapped to wrap/length;
  tweens, sprite animations, particles, nested animations are listed as "not converted" (the user keeps the entity
  sequence for them). Result: the file (project step) + an Animation Player with the clip on the root and, if the user
  ticks "Remove the converted tracks", the converted track entities removed (scene step).
- **Extract to .anim** (model sub-clip → file, `source` kept, "Re-sync from source" rewrites tracks from the model).
- **Bake to Clip** (pose animation): PoseBaker writes `<name>.anim` (paths of the mapped humanoid bones relative to
  the model root) and adds it to the model's Animation Player; the Pose window's Bake button offers "Bake to Animation
  Clip" (primary) and "Bake into Scene" (the existing entity bake, unchanged).

## 5. MCP (agents)

Budget: tool definitions TOTAL_LIMIT 109,008 (now 107,778 with 146 tools); instructions ≤ 16,384 bytes (now 16,373).
No new tool. Every addition is paid by trims listed in R4 §2.4 (move edit_animation's grammar details to the guide,
synthesize_texture params, edit_lattice field lists, pose tool legends, edit_colliders lists; instructions: the
duplicated Unity-names pitfall and the duplicated Build Settings line), measured with tool_budget.py; caps for
edit/get/preview_animation added to test_html_ui_tools.py LIMITS; TOTAL_LIMIT only lowered, never raised.

- `edit_animation` gains `clip` (project-relative `.anim` path; file mode), `create` (bool, fails if the file exists),
  `from` (create from an entity Animation `{entity}` or a model sub-clip `"assets/m.gltf#Walk"`: Convert/Extract),
  `tracks[].path`, `on` (entity: root to resolve bindings, `"current"` lattice keys and euler-from-current against,
  and to report bindings), `events` (`[{time, function, args}]`, mode replace|merge like tracks), `wrap`, `frame_rate`,
  `length`. File mode rejects `tweens/add_actions/set_frames/remove_actions/target/autoplay/speed` with a fix-it
  message ("tweens are scene actions: use edit_animation without clip"). Results:
  `{clip, created, length, wrap, frame_rate, tracks[{index, path, channel, keys, loop, relative}], events, changes[],
  warnings[], bindings?{root, resolved, missing[{path, channel, nearest}]}, users}`; text says it is a project undo
  step and how many components share the clip. All or nothing.
- `get_animation` gains `clip` (read the file, no scene needed, includes sub-clips), `times` (sampled values per track
  at those seconds through the runtime sampler, headless), `on` (binding report). `entity` on a GameObject with a
  player lists its states with clip paths and the binding report.
- `preview_animation` accepts a GameObject with an Animation Player (+ `clip` state name when several) and returns the
  sampled transforms of up to 8 bound paths (`bound: [{path, position, rotation, scale}]`) besides today's fields.
- `set_properties`/`add_component`: `animationplayer` `clips[i].clip` validated like PoseAnimator's animation path
  (project-relative or stored form; ".anim or model#clip"; error with an example), assets paths normalized.
- `edit_pose_animation` `bake` gains `clip` (writes a shared clip instead of entities; default unchanged).
- `list_assets` type `animation`; `get_asset_info` on `.anim` (version, length, wrap, tracks, events, warnings,
  users); `view_asset` refuses with a pointer to preview_animation; `write_file` validates `.anim` (refuse invalid JSON
  or format, return warnings); `move_asset`/`delete_asset` handle clip references (4.1).
- `check --anims` CLI (+ in the default set and `--files` classification and the hook mode); docs/mcp/checks.md.
- Guide `docs/mcp/guide/animation.md` → `jasper://docs/animation` (format, paths, channels, eases, events, player,
  Lua, verification loop: edit → get times → set_properties → preview + screenshot → play, common mistakes), index
  row, test_docs list; instructions: one bullet in "2D and 3D" paid by the trims; test_autosync `pairs` gains
  `edit_animation: [clip]`.

## 6. Documentation

`docs/animation-clips.md` (user guide: create, record, keys, curves, events, player, Lua, sharing via prefabs and
paths, converting, extracting, baking, troubleshooting missing bindings), README feature bullets, unity-glossary rows
(Animation Clip `.anim`; Unity's legacy Animation component → Animation Player; Animation window clip mode; model
sub-clips), lua-api sections, DESIGN.md auto-sync list, AGENTS.md generator bullet.

## 7. Tests (all must pass; failure sets compared with /tmp/merge-tools/base-fails-current.txt)

- Engine gtest target `jasperengine-anim-tests` (`engine/tests/anim`, option JASPERENGINE_ANIM_TESTS): IO round trip
  byte-identical, canonical ordering, leniency warnings with paths, Unity-YAML refusal, euler/quaternion continuity,
  every channel sampled vs hand-computed values (linear, step, eases, Hermite), wrap modes incl. ping-pong and
  clamp_forever, length rules, blending formula incl. rest term, crossfade/blend/queue/stop timelines, event crossing
  (forward, backward, wrap, low frame rate, dominant-state rule), path resolution with `#N`, ambiguity, rebinding after
  rename/reparent via hierarchy revision, sub-clip extraction from the pose fixture glTF (100% bound on hero),
  determinism, pool invalidation, thread-safety of the sub-clip builder (concurrent builds).
- Editor gtests (`editor/tests/*Test.cpp`): document commit/undo/redo/merge keys/reload/read-only, validation,
  converters (entity → clip identical sampled values; model → clip; pose bake → clip), Stream round trip of the
  component, Factory emission, rename/move remap incl. closed scenes.
- MCP `tools/mcp/tests/test_animation_clip.py` (pose-animation fixture + scratch scenes): every new argument and
  error, project vs scene undo, sharing one clip between two prefabs/scenes and editing once, Hero vs Robot binding
  report with nearest, get_animation times, preview bound transforms, write_file validation, external_changes
  problems, move/delete asset, play → runtime movement and events into a Lua script, export emits the component.
  Window/Inspector behaviour through test hooks like test_pose_window.py (record mode keys a gizmo drag, missing rows,
  Add Property, copy/paste, events lane, read-only sub-clip banner). `test_checks.py` for `--anims`.
- Existing suites unchanged: test_world, test_lattice_anim, test_pose_*, test_fbx, test_html_ui_tools budget,
  test_autosync instructions, test_docs.

- Existing suites: behaviour unchanged, but these change on purpose (section 9, R32): test_html_ui_tools (caps, lowered
  TOTAL_LIMIT), test_autosync `pairs`, test_docs DOCS, test_synthesize_texture_tool params. Baseline failure set:
  `/tmp/merge-tools/base-fails-current.txt` (full suite of origin/main d61bed5a5 = the branch base minus the pose
  work; the merged base 139d300f2 has the same set). Any FAIL not in it is a regression.

## 8. Work breakdown (tasks, order, file ownership)

Every task gets a brief with an explicit file list; a file not in your list is read-only for you. Shared wiring files
(`editor/App.cpp`, root `CMakeLists.txt`, `engine/CMakeLists.txt`, Xcode project) are edited only by T0 (stubs and every
source-list entry for T1–T4) and later only by the task named for the hook. The T0 contract index
(`2026-10-24-animation-clips-contract.md`, "Ownership") is the authority for who owns each hook, call site and helper;
the clarifications below follow it.

1. **T0 Contract** — every new public header with compiling stubs: `engine/core/animation/{AnimationClip, AnimationClipIO,
   KeyframeSampling, ClipBinding, AnimationEvent}.h`, `engine/core/pool/AnimationClipPool.h`,
   `engine/core/component/AnimationPlayerComponent.h` (+ registration, JasperEngine.h, ComponentType enum entry +
   LastComponentType), `ActionSystem::previewPlayer/sampleClip` declarations, `EntityRegistry::hierarchyRevision()`
   declaration; editor `editor/animation/{AnimationClipDocument, ClipValidation, ClipConverters, ClipRecordHooks,
   PreviewOwner, AnimationClipInspectorHooks, AnimationClipController}.h`; App wiring stubs (stopTransientPreviews entry,
   undo routing hook, drag-drop dispatch hook); ViewTools animation hook stub; gtest target skeletons
   (`jasperengine-anim-tests`, editor tests files); every CMake/Xcode entry for files T1–T4 will add.
2. **T1a Engine core** (after T0): AnimationClipIO (parse/canonical write, GUID key, leniency rules, Unity YAML refusal),
   KeyframeSampling refactor of ActionSystem (no behaviour change), AnimationClipPool (Data::open, generations,
   invalidate incl. `<model>#*`, clearUnused, Engine pool hooks, ModelPool::invalidate lock), engine tests for these.
3. **T1b Player** (after T1a): ClipBinding resolver + hierarchy revision (all mutation paths of R9), Animation Player
   system (states, wrap, play/crossFade/blend/queue/stop/rewind/sample, events queue + dispatch rules, rest capture,
   flush rest term, deferral on model loads, previewPlayer/sampleClip), Lua bindings + stubs, Model integration,
   EntityCloner, Engine/Scene callback clearing, ScriptEvents entry (`editor/util/ScriptEvents.cpp`: T1b, not T2a),
   `LuaFunctionBase::push_value(const AnimationEvent&)`; engine tests.
4. **T1c Model sub-clips** (after T1a, parallel with T1b): the sub-clip builder (mirrors buildGLTFNodeHierarchy, thread
   safety, accessor reads), node-hierarchy layout forcing for models under a player (MeshSystem + ModelLoadCmd reload
   step), `ModelComponent.animationEntities` flag honoured by MeshSystem, new fixtures (animated hero glTF with walk,
   idle, one unnamed, two same-named animations; FBX with namespaced bones), engine tests.
5. **T2a Component plumbing** (after T1a): Catalog (props incl. `clips[i].clip/name/wrap/guid`, name, icon, enabled),
   Stream encode/decode (GUID + path hint), Factory emitter, ProjectUtils add/remove, PropertyCmd hierarchy switch,
   ComponentAddDialog, PoseViewport stripToRender, collectResources, McpValues clip validation (`editor/mcp/McpValues.cpp`
   clip-path validation is T2a's, excepted from T4); editor tests.
6. **T2b Asset plumbing** (after T0, parallel with T1*): AnimationClipDocument, Util predicates (+ ProjectWindow using
   them), FileDialogs, Project window (FileType, icon, Create, double-click, drag sources/targets via the App hook),
   AssetDatabase GUID tracking of `.anim` (JSON guid), rename/move/delete semantics (R8, R26), McpAutoSync +
   `ScriptTools::syncFiles` + problems (+ the `.poseanim` problems gap), `isTextFile` (`.anim`, `.poseanim`),
   `findClipUsers`, the `createClipAsset` App hook (main menu Assets > Create > Animation Clip), `Project::setInspectedAsset`
   on a single-asset click; document tests run after T1a is merged (IO is a stub until then); editor tests.
7. **T2c Inspector** (after T2a, T2b, T1b): Animation Player section, asset inspector + inspected-object model (R30),
   AnimationComponent "Convert to Clip...", Model "Extract to .anim" / "Use Animation Player", Bindings foldout; via
   hooks so InspectorWindow includes no window; the Inspector action preview acquires `PreviewOwner` (R7).
8. **T2d Validation + converters + bake** (after T1b, T1c, T2b): ClipValidation, ClipConverters (convert, extract, use
   player), PoseBaker bake-to-clip + Pose window Bake menu entry, composite scene commands (R4); editor tests.
9. **T3a Clip controller** (after T1b, T2a, T2b): AnimationClipController (bound root, preview snapshot of bound
   channels, PreviewOwner registry, ClipRecordHooks registries, beforeChannelWrite updating snapshots (the command call
   sites are T0's), the AnimationClipAppHooks setters stopTransientPreviews / ownsUndo / takesKeys / sceneClosed /
   projectClosed, undo routing, key drag API), gtests.
10. **T3b Window UI** (after T3a, T2d): clip mode in new files (`AnimationWindowClipSheet.cpp`, `…ClipEvents.cpp`):
    dropdown, empty state, rows, Add Property, keys (multi-select, box, drag, delete, copy/paste/duplicate, context
    menu), events lane, toolbar (frames/seconds, samples, snap), shortcuts, read-only banner + Extract, lanes made
    view-only (R31); `AnimationWindow::stopTransientPreviews` (entity mode, R6); window test hooks through
    `AnimationClipAppHooks::windowTestHook` (focus_window `animation`, wired by T0) + test_animation_window.py.
11. **T3c Curves view + Record** (after T3b): curves view (sampled curves, vertical key drag, segment ease, F/A), Record
    mode through ClipRecordHooks claims/record at the source call sites (SceneRender gizmo drag, Inspector Transform/morph
    rows, LatticeEditorOps) with the Pose window moved from `beforeTransformWrite` onto the chained `beforeChannelWrite`;
    new morph-row writes notify `before/afterComponentWrite`; tests.
12. **T4 MCP + CLI** (after T3a; owns `editor/mcp/**` except T2b's files, `editor/cli/**`, `docs/mcp/**`,
    `tools/mcp/tests/test_animation_clip.py`, test_checks/test_html_ui_tools/test_autosync/test_docs/
    test_synthesize_texture_tool edits; not `McpValues.cpp` clip validation, T2a's): the 9.R28/R29 argument design,
    results, rejection table, budget trims and caps (counting T0's focus_window `animation` test-hook property), guide,
    instructions bullet, TOOLS.md regeneration.
13. **T5 Docs** (after T3c, T4): docs/animation-clips.md, README, glossary (R52), lua-api, AGENTS.md generator, DESIGN.md.

After each task: a review (spec compliance, code quality, adversarial correctness) and fix rounds until clean; then
merge into `animation-clips`. At the end: a whole-branch review, the full MCP suite vs the baseline, ASan run of the
clip test files, then merge to pose-animation/main.

## 9. Review resolutions (binding; numbers refer to SPEC-REVIEW.md, whose "Proposed change" texts are adopted unless noted)

- **R1 Blending (adopted):** a player contributes only (target, channel[, morph index | lattice point]) entries animated
  by a state with weight > 0 whose track resolved; it adds Σwᵢ·vᵢ with weight Σwᵢ to the shared accumulator exactly like
  entity tracks and registers the entry's rest value. `flushPoseBlend` adds (1 − W)·rest before normalizing for every
  entry with a registered rest whose total weight W over all contributors is < 1 (rotations: rest aligned to the
  accumulator's reference hemisphere). Entries no playing state animates are not written. Targets without a registered
  rest behave exactly as today (entity-only output byte-identical). Replaces the 3.3 blending bullet.
- **R2 Rest (adopted):** rest values are kept per (entity, channel) for the life of the player's run (play session, or
  edit-mode preview session): taken the first time the entity is bound, before the player writes it, kept across
  rebinding, clip reloads and state changes; for every entity including bones it is the scene value (local TRS, morph
  weights, lattice offsets). In edit preview rest = the preview snapshot and follows rest edits (R6).
- **R3 Sub-clip paths (adopted):** paths come from a pure function over the tinygltf model mirroring
  `buildGLTFNodeHierarchy` (node.name or "Node <i>", parent = the node listing it, parentless nodes under the model root,
  `#k` = lower-index same-name siblings). A ModelComponent on or under an entity with an Animation Player is always
  loaded in the node-hierarchy layout (`useNodeHierarchy` forced; adding a player to a legacy-layout model reloads it as
  part of the same scene step). Merged static models animate only at `""`. PoseBaker/Convert compute paths from actual
  entities. ClipValidation reports "this model is in the legacy layout" instead of N missing paths. Fixtures as listed.
- **R4 Undo scopes (adopted):** pure clip edits (keys, tracks, events, settings, Retarget, Extract into an existing file)
  are project steps via AnimationClipDocument. Every action that also changes a scene (Create New Clip in window /
  Inspector / empty state, clip drag-drop onto an object, Convert to Clip, Bake to Clip, Use Animation Player, MCP
  `edit_animation {create: {from: entity}}`) is ONE scene-history composite command (CreatePrefabCmd pattern): writes
  the file on execute/redo, deletes it on undo only if its bytes are still what it wrote (else keeps it and logs), and
  does the component/entity changes. The Animation window routes Ctrl+Z to the project history while it has focus and
  its last action was a pure clip edit. MCP result texts name the scope. Mismatch-order tests required.
- **R5 Record (adopted):** `editor/animation/ClipRecordHooks.h` chained listeners `claims(sceneId, entity, channel)` /
  `record(sceneId, entity, channel, value, mergeKey)` called at the sources before any command is built (SceneRender
  gizmo drag, Inspector Transform rows, MeshComponent morph rows, `LatticeEditorOps::push`); a claimed edit writes the
  live preview value, no scene command, and commits a key with merge key `record#<dragSerial>`; unclaimed edits stay
  scene steps and update the preview snapshot. While Record is on, Ctrl+Z from Scene view/Inspector goes to the project
  history. MCP set_properties is never recorded. `poseInspectorHooks().beforeTransformWrite` becomes a chained
  `beforeChannelWrite` list used by both windows.
- **R6 Preview isolation (adopted):** the clip preview snapshot holds only bound channels, taken on the first write per
  target; restored on stop, scene save, play, scene switch/close, window close (AnimationWindow joins
  `App::stopTransientPreviews`, entity mode too); scene commands/undo/redo writing a previewed channel update its
  snapshot (via beforeChannelWrite); preview never writes `isModified`. Test: save during preview writes unpreviewed values.
- **R7 PreviewOwner (adopted):** one edit-mode pose owner per model root (Animation window clip/entity preview,
  Inspector action preview, PoseEditSession); acquiring stops the previous owner first.
- **R8 References (adopted):** clip refs are GUID + path hint (`"guid"` key in `.anim` right after `version`, new GUID for
  copies; AssetDatabase tracks `.anim`; `clips[]` entries `{clip, guid, name, wrap}`; load resolves GUID first and
  repairs the hint). Runtime/export use the path only. Sub-clips are path-only and remapped by the model's rename (R26).
  Delete keeps references (Inspector "Missing (name)"); undoing the delete relinks. No closed file is ever rewritten.
  This replaces the 4.1 rename bullet.
- **R9 Hierarchy revision (adopted as proposed)** incl. the validate-then-re-resolve rule and the performance test
  (100 players × 60 bones, one spawn per frame: player update < 2 ms on this host in Release).
- **R10 (adopted)** deferral on model loads; **R11 (adopted)** sub-clip naming (`"Animation <i>"`, `"<name> <k>"`
  duplicates, split at first `#` after a model extension, pool key by index; fix get_asset_info naming); **R12, R13
  (adopted)**; **R14 (adopted)** `clips[]` entries carry `wrap` (default|once|loop|ping_pong|clamp_forever).
- **R15 (recommended option adopted):** Model Inspector "Use Animation Player" (one scene step: removes the model's
  entity animations and tracks, sets new `ModelComponent.animationEntities = false` honoured by MeshSystem at load and
  reload, adds a player listing every `model#name`). New models keep `animationEntities = true` (existing behaviour).
  `Model:playAnimation` on a model with a player fades out its running entity animations too.
- **R16 Events, R17 Queue, R18 Play/stop/fade, R19 Relative-to-rest, R20 Lattice union, R21 Preview entry points
  (adopted as proposed).** Default fade 0.25 s (R51: between the engine's 0.15 and Unity's 0.3; documented).
- **R22 (adopted)** extra touch points; **R23 (adopted)** `AnimationEvent` type and C++ script subscription;
  **R24** `cullWhenHidden` removed (moved to 2.5); **R25** `enabled` added; **R26, R27 (adopted)**.
- **R28 Budget (adopted with the measured plan):** trims = edit_animation grammar/tween texts → guide (−389),
  synthesize_texture params → anyObject with fields documented in texture-synthesis.md and its test updated (−433),
  edit_lattice per-op field lists (−323), render_pose_animation legend (−86), get_pose_animation metrics (−60);
  edit_colliders untouched. TOTAL_LIMIT lowered to the measured total + 300. Instructions bullet ≤ 130 B (`- Clips
  (.anim, shared): edit_animation {clip, create, tracks [{path}]}; play with an Animation Player
  (jasper://docs/animation).`) paid by removing the Build Settings line and the duplicate Unity-names pitfall.
- **R29 Arguments (adopted):** `clip` selects file mode; `create` is an object `{from?: "<model>#<name>" | entity ref}`;
  `root` (not `on`); `events_mode: replace|merge`; file mode rejects `loop/duration/tweens/add_actions/set_frames/
  remove_actions/target/autoplay/speed`; scene mode rejects `wrap/length/frame_rate/events/root/create`; each with a
  fix-it message (two-way table in the guide and the T4 brief); preview_animation selects a state with `state`;
  get_animation `entity` optional with `clip`, entity results `{entity, players: [{states: [{name, clip, wrap}],
  bindings}], animations: [...]}`.
- **R30 Inspected object (adopted)**; **R31 view-only lanes + dropdown precedence (adopted)**; **R32 test plan
  (adopted; baseline is /tmp/merge-tools/base-fails-current.txt)**; **R33/R34 breakdown (adopted, section 8)**.
- **Minors R35–R54 adopted as proposed**, except R50: Record and the curves view's vertical drag stay in v1 (T3c, last);
  cut from v1: `cullWhenHidden`, "Re-sync from source" (the `source` key is still written), the Inspector Retarget
  button (Retarget lives in the window's row menu). Explicit v1 non-goals per R49.
