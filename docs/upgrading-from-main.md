# Upgrading a game from origin/main

A game made with the editor of `origin/main` opens in this editor (the integration branch `oe-port`,
merged into `main`) and plays the same: the scenes, prefabs, scripts and settings keep their meaning.
The first open migrates the project's files to the current formats and gives every asset an
identity (a GUID); after that commit, everything stays as it is. This page says what changes, what
to commit, the few things a script has to change, and how the upgrade is tested
(`tools/ci/upgrade_from_main/`).

## Before you upgrade

1. Commit (or copy) the project as the old editor left it, with every scene saved.
2. Close the old editor. Open the project in the new one: **File > Open Project**, or
   `jasperengine-editor mcp --project <dir>` for an agent.
3. Open each scene once and save it (**File > Save All**, or `save_scene {all: true}`), then review
   and commit what changed as one "upgrade" commit (below). Do this before you change anything else,
   so the upgrade is a commit of its own.

## What changes on the first open

| File | Change | Why |
|---|---|---|
| `*.scene` | `formatVersion: 7` written first, a `guid` | scene formats 0 -> 7 ([data-safety](data-safety.md)): a version stamp; light shadow bias in shadow texels (S10: a light's Bias is reset to 1); the scene's shadow quality Low (the old default) becomes Auto; the old Particle System actions become inline effects on their targets (S20), and so do main's module (`version: 2`) Particle Systems, every module value kept (S20, Upstream absorption); the derived transform keys (`worldPosition`, `worldRotation`, `worldScale`, `localMatrix`, `modelMatrix`, `normalMatrix`) are no longer written (S29); splines (S18: nothing to convert); an Animation Player's `controller` moves to an Animator on the same GameObject, which plays it while the player is disabled (S21, Upstream absorption); the Line and Trail Renderers' width curves and colour gradients in the editor's curve form (S27, Upstream absorption: the same values) |
| `*.prefab` | `version: 8` | prefab versions 3 -> 8: the same shadow bias, particle, transform, Animation Player controller and curve changes; a prefab is written again by the first save of a scene that uses it |
| every asset (`.png`, `.gltf`, `.wav`, `.html`, `.css`, `.lua`, `.poseanim`, ...) | a `<file>.meta` next to it, holding its GUID | S04: references survive renames and moves |
| references in scenes and prefabs | a `pathGuid` next to every asset path (textures, models, sounds, scripts, clips, documents) | S04 |
| `project.yaml` | `formatVersion: 1` written first | the project format's version stamp |
| `*.material`, `*.anim` | nothing: their GUID is already in the file; a material is written in the current format (`formatVersion: 1`) only when it changes | |

Every migration is listed in the scene's load report (`get_load_report`, the Console's load lines)
as `migrated` information; nothing is dropped. The editor's own folders (`.jasperengine/`, the
generated `CMakeLists.txt`) are regenerated and stay ignored by the project's `.gitignore`.

### What to commit first

One commit, made right after the first open and Save All:

- the changed `*.scene` and `*.prefab` files (format stamps, GUIDs, migrations);
- the new `*.meta` files (one per asset; they must stay next to their files from now on: move and
  rename assets in the editor, or move the `.meta` along);
- `project.yaml`.

A second open and save of that commit rewrites nothing: `tools/ci/upgrade_from_main/check.sh`
checks it.

## Behaviour

The game plays the same. The upgrade test's game (below) prints its gameplay state, frame by frame,
at a fixed 1/60 s per frame: positions and velocities of 3D and 2D physics bodies, collected items,
HTML UI and UI component texts, Animation Clip times and events, Pose Animation times, tweens,
particles, sounds, scene changes. This editor prints the old editor's transcript exactly (no
tolerance needed: every decimal is the same) in Play, with keys sent through `send_input` and with
the game's own autopilot, in the exported and compiled Linux player and in the web player.

Read physics state from the bodies (`Rigidbody.position`, `Rigidbody2D.position`) when it has to be
exact: a GameObject's Transform is drawn between the last two fixed steps (physics interpolation),
as far as the frame's leftover time, which a frame spent loading changes. That is so on origin/main
too.

### IK solvers (S77)

Three Animator IK fixes apply to existing scenes with no opt-in: poses change where the old ones
missed. No stored field changes meaning and nothing is migrated: scenes and prefabs load as they
are, and the new fields (**Soften**, **Hinge Axis**, **Twist**, **Aim Offset**) default to today's
behaviour and are not written while at their defaults.

- **Look At** ends on its target. It used to measure one turn at the head and share it out, so a
  chain that leans missed by 0.2 to 3.3 degrees on a neck chain at 1 m and up to 8.8 degrees with a
  0.5 m lever; now each bone measures again, keeping the weights and the clamp. An upright yaw does
  not change.
- **Foot IK** keeps the centre of the sole on a slope. The sole used to sink by 0.48 cm at 20 degrees
  and 1.07 cm at 30 degrees, and 2.07 cm at 45 degrees past the default clamp, and the foot turned with
  the shin when the knee bent. The ankle now stands higher on a slope, the foot keeps its animated
  rotation turned to the ground, and the pelvis may drop a little less. Flat ground does not change
  unless the knee bends there.

- **Foot Lock** no longer turns the foot. Holding the ankle bends the knee, and the foot used to
  turn with the shin: about 4 degrees for 7 cm of hip travel in the test's rig. Under a lock on the
  same leg it also took the foot off the slope Foot IK had put it on. The foot now keeps the rotation
  it came with. The locked position does not change.

**Soften** stays off by default, because it changes where feet land.

## Fixed for the upgrade

The upgrade test found these, fixed in this editor (each with a test that failed before):

- **Prefabs stayed at version 3.** A prefab migrated on load was never written again, so it was
  migrated on every load. A scene's save now writes the migrated prefabs it uses in version 7.
- **Old particle scripts stopped.** `ps:isRunning()`, `isStopped()`, `getTimeCount()`, `ps.target`
  and `ps.speed` of the old Particle System action failed with "attempt to call a nil value"; they
  work again, with a deprecation warning.
- **Exported games with a string script property did not compile.** The generated scene code
  assigned a string literal that matched two alternatives of the script property value (a string
  and an asset reference); every export or desktop build of such a game failed in the compiler.
- **A UI scene changed on every save.** A RectTransform's scissor (the clip rectangle of the last
  draw, in the pixels of the view that drew it) was saved, so opening and saving a UI scene again
  rewrote it. It is no longer written.
- **A prebuilt Linux player drew no sky.** A Skybox that uses the editor's default sky names no
  file (its faces are built into the editor), so the prebuilt cook shipped none and the game logged
  "Texture is missing texture for face 0". The cook now ships the six default faces with the game.
- **A prebuilt player's 2D polygon colliders lost their shape.** The cook left out a 2D collider's
  vertex count, so in the player a polygon became the box between its two points (the upgrade
  test's balls fell past the end of the ground). It also left out a 2D collider's collision filter
  (category, mask, group) and a 3D body's allowed degrees of freedom and mesh collider indices; all
  are cooked now.

## Known, as on origin/main

- A save while a model is still loading writes its submeshes' vertex counts as they are at that
  moment (derived values, rebuilt on load); save once the scene has loaded to keep the file stable.

Two more that origin/main has are fixed in this editor (also found by the upgrade test):

- **A tint on a linked material was lost on reopen.** A submesh linked to a `.material` file takes
  the file's values when its scene opens, so a colour set on one field of it (`set_properties
  submeshes[N].material.baseColorFactor`, on a GameObject or a prefab instance) was saved, then
  replaced by the file's. Such an edit now makes it the GameObject's own material (unlinked, the
  file untouched), as an edit of the whole material and a material preset already did. An
  origin/main scene saved with a tint on a still linked submesh shows the file's colour, as it did
  on origin/main after a reopen: set the tint again.
- **3D contact events called Lua from the physics worker threads**, which could crash the game.
  `onContactAdded3D`, `onContactPersisted3D`, `onContactRemoved3D`, `onBodyActivated3D` and
  `onBodyDeactivated3D` are delivered on the main thread right after the physics step, in the same
  order on every run. A `combinedFriction`, `combinedRestitution` or `sensor` a handler sets on the
  delivered contact applies from the next step (it changed the contact being solved before), and
  with a `shouldCollide3D` handler the 3D step runs on the main thread only.

## Lua scripts

Old scripts keep running; a few old names print a one-time deprecation warning with the new name.

- **Particle Systems** (S20). A Particle System is a component of the GameObject it draws on now,
  no longer an action with a target. `ParticleSystem(scene, entity)` given the old action's entity
  (and a script property of type ParticleSystem that points there) still finds the system. The old
  Action members keep working with a warning: `start()`, `isRunning()`, `isStopped()`,
  `getTimeCount()` (now `time`), `speed` (now `playbackSpeed`), `target` (the system's own GameObject),
  `setTarget()`. **Change:** `ps:isPaused()` is the property `ps.isPaused` now (calling it fails with
  "attempt to call a boolean value"). Particle counts and positions come from the new simulation: a
  script must not depend on the exact particles.
- **Joints made from Lua** (S96). `Joint2D(scene)` / `Joint3D(scene)` make a GameObject named
  "Joint 2D" / "Joint 3D". Once its bodyA is set (`bodyA`, `set*Joint`), the joint goes under bodyA
  (world pose kept) and the handle stops owning it (`entityOwned` false): it outlives the handle and
  goes when bodyA's GameObject goes, as editor-made joints do. **Change:** dropping the handle no
  longer removes a joint that has a bodyA (before, the next garbage collection did); remove it with
  `joint:destroy()` or `GameObject.destroy(joint)`. A joint never given a bodyA still goes with its
  handle.

## Rendering

The first open applies rendering migrations that change how a scene looks, on purpose:

- **Shadows** (S10): the Shadow Bias of every light is reset to 1 shadow texel (the old value was
  in depth units and cannot be converted) and the scene's Shadow Quality Low becomes Auto (the
  platform's tier). Shadows are sharper and placed differently.
- **Particles** (S20): the effects are simulated and drawn by the new particle system: the same
  emission settings, different particles.
- **Sub-emitters** (BF23): a sub-emitter's particles were simulated but never drawn. Effects with
  sub-emitters now show them, each with its child effect's Renderer, so they may look different and
  cost what they always should have; they count against the world particle budget.

The upgrade test takes screenshots at the same game frames in both editors and compares them at one
size (480x270: the Game view's size follows each editor's layout, 904x509 against 869x489 here).
With the fixture's scenes, 0.2 to 1.7 % of the pixels differ by more than 24 in a channel (it varies
from run to run), all of them expected: glyph and edge resampling of the two render sizes (the HTML
UI title and HUD, the 2D sprites, every silhouette), the camera, which follows the courier's drawn
(interpolated) position, a fraction of a frame apart after a load, the shadow edges (S10's bias and
filtering) and the particles (S20's simulation). The
images and their diffs are in `build/upgrade-from-main/check/screenshots/`; gameplay, the
transcripts, does not differ at all.

## C++ scripts

A C++ script written against origin/main's API (a `ScriptBase` with `DPROPERTY` fields, a
`GameObject*` reference, engine events, `Log::print`) builds and runs unchanged: the upgrade test's
C++ fixture prints the same transcript. Prefer Lua still: C++ scripts compile against the engine's
headers, which keep changing (C++20 is the standard now, the ECS and components moved to reflection
tables), so a script that reaches into engine internals may need changes the Lua API spares you.
For example the component capacities `MAX_SUBMESHES`, `MAX_EXTERNAL_BUFFERS`, `MAX_BONES`,
`MAX_SPRITE_FRAMES`, `MAX_TILEMAP_TILES` and `MAX_TILEMAP_TILESRECT` are gone (the arrays grow with
their content): use the counts (`numSubmeshes`, `numFramesRect`, `numTiles`...). A scene file keeps
its `maxValues` block as it was; nothing reads it.

## Features that need a sync

`oe-port` holds `origin/main` up to `50b830432` (SYNC#3: Vertex Animation Textures and the engine
fixes 1-4 included). A game that uses something only a newer `origin/main` has cannot be upgraded
until the next sync brings it in. Against `origin/main` `7f48d7e8d`:

- the Sprite Normal Map (`generate_sprite_normals`, the Sprite Normal Map window, `check --normals`,
  normal maps generated for sprites, sheets and tiles; sprite-normals, -fixes, -quantize, -hires);
- the MCP write sandbox hardening (sandbox-hardening): the same project files, a stricter editor.

The fixture does not use them, so the check passes; re-run it after each sync (below).

## The upgrade test

`tools/ci/upgrade_from_main/` holds a repeatable test that a real game survives the upgrade:

- `generate.sh` builds the editor of an `origin/main` commit and makes the sample game "Upgrade
  Courier" through that editor's own MCP tools, as a user (or an agent) would: four scenes (an HTML
  UI title screen, a 3D level with a UI child scene, a 2D arena), nested prefabs, a prefab variant
  and instance overrides, Lua gameplay with input and engine events, physics in 3D and 2D, lights
  and shadows, materials and textures, an Animation Clip with an event on an Animation Player, a
  Pose Animation on a humanoid, a sound, particles, tweens and changed Project Settings. It plays the
  game in that editor (with keys sent through `send_input`, and with the game's own autopilot) and
  in its exported Linux and web players, and records the transcripts and screenshots. A second,
  one-scene project has a C++ script. Everything goes into `tools/test-projects/upgrade-from-main/`
  with the commit it came from (`SOURCE.yaml`) and the tool calls that made it (`recipe/`).
- `check.sh` opens that game in this editor and checks the upgrade: no error or warning, nothing
  but migrations in the load reports, every scene and prefab in the current format after a save,
  a GUID for every asset, every reference resolving, a second open rewriting nothing, the same
  transcripts in the editor, in the Linux player (compiled and prebuilt) and on the web.
- `tools/mcp/tests/test_upgrade_from_main.py` runs the editor part in the MCP suite.

The Linux player is checked twice: compiled from the export, and built with the prebuilt player
(S30's `jasperengine-editor build --method prebuilt`, in the no-compiler runtime image; run
`tools/build.sh players` first). The fixture does not use Vertex Animation Textures (in `oe-port`
since SYNC#3).

Run it (Docker):

```sh
tools/build.sh linux                                  # this editor
tools/build.sh players                                # the prebuilt player runtimes
tools/ci/upgrade_from_main/check.sh                   # build/upgrade-from-main/check/report.md
```

### Against a newer origin/main

Before merging into `main`, make the fixture again with the newest `origin/main` and run the check:

```sh
git fetch origin
tools/ci/upgrade_from_main/generate.sh --ref origin/main      # builds origin/main in build/upgrade-from-main/old-src
tools/ci/upgrade_from_main/check.sh
git add tools/test-projects/upgrade-from-main && git commit -m "upgrade test: fixture from origin/main <sha>"
```

`--old-src <checkout>` builds an existing checkout instead (with `--commit <sha>` naming its
commit), `--old-editor <editor-install> --commit <sha>` uses an editor already
built. A feature that only the newer `origin/main` has fails the check until it is synced: list it
under "Features that need a sync" above.

## Foliage (T5)

Scenes made before foliage's upgrade place the same grass from the same seeds and look the same, with no
conversion. A project without the new settings stays High, with no caps, on every platform. These behave
differently, on purpose:

1. Grass and detail meshes no longer enter navmesh bakes. They used to, depending on where the editor's
   camera was.
2. `MeshRaycast` no longer hits detail instances unless asked (`PICK_DETAIL`), and its hit's `entity` is the
   Terrain, not a hidden chunk.
3. Paint Trees places Trees by default in new strokes. Place As: GameObject keeps what it did.
4. A disabled Terrain hides its foliage. `sculpt_terrain`'s `place_object` keeps today's result.
