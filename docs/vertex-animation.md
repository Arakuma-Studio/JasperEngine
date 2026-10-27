# Vertex animation (crowds on the GPU)

A **Vertex Animation** (VAT, "vertex animation texture") is an animation baked into a texture
and played back by the GPU. The editor samples any animation the engine can play (a model's
clips, `.anim` clips, an Animation Player, a pose animation, lattice tracks, a hierarchy of moving
parts) and saves it as a `.vat` asset. The **Vertex Animator** component then plays it on the
GameObject and on every instance of its Instanced Mesh. Each instance has its own clip, time,
speed and crossfade, and the whole crowd is **one instanced draw per material**. Characters that
just loop cost no CPU work and no uploads per frame. A `play` or a `crossFade` writes 32 bytes.

Houdini (SideFX Labs VAT 3: soft bodies, rigid-body destruction, fluids) and OpenVAT (Blender)
exports are imported into the same `.vat` format, so one runtime path plays all of them.

**When to use it.** Use it for crowds, background characters, flocks, and many copies of one
animated thing. Also use it for simulations made elsewhere (cloth, destruction, liquids). Keep the
**Animation Player** (skinning on the CPU, see [animation-clips.md](animation-clips.md)) for the
characters you control closely, when you need:

- clips you still edit;
- bones as GameObjects (a script moving a head, a prop parented to a bone at runtime);
- root motion, IK or a ragdoll;
- picking or physics against the animated shape.

A baked character plays only the clips baked into its `.vat`. Its colliders and picking use the
rest pose, as with lattices.

**What it costs.**

- **Memory**: the texture holds every sampled frame. The baker shows its size before baking:
  kilobytes in Bones mode, up to tens of megabytes in Vertices mode.
- **GPU**: 2 to 16 texel fetches per vertex (table below), twice that during a crossfade.
- **Baking time**: a few seconds for a character.

The defaults fit OpenGL ES 3 and WebGL2: textures 2048 texels wide, float textures read without
filtering, no compute shaders. They run on phones and in the browser. The Desktop preset raises
the limits.

AI agents bake and import through the MCP tool `bake_vertex_animation` (preview first, then
write). The guide is [docs/mcp/guide/vertex-animation.md](mcp/guide/vertex-animation.md)
(`jasper://docs/vertex-animation`). The command line has `jasperengine-editor bake-vat` (below).

## Workflow

1. **Open the baker.** Open it in any of these ways:
   - **Window > Animation > Vertex Animation Baker** opens it empty.
   - In the Project window, right-click a model (`.gltf`, `.glb`, `.fbx`) or an `.anim` and
     choose **Bake Vertex Animation...**.
   - In the Inspector, use **Bake ▾ › Vertex Animation...** in the Animation Player, Pose Animator
     or Lattice Modifier section, or on a clip row of a model asset. The window opens filled in
     with that source. Shift-click bakes straight away with the last settings.
   - For a Houdini or OpenVAT export, right-click it in the Project window and choose **Import as
     Vertex Animation...** (see [Importing](#importing-houdini-and-openvat)).
   - Double-click a `.vat` (or use **Open in Baker** in its Inspector view). The window opens with
     the recipe the file was baked from, and **Rebake** replaces **Bake**.
2. **Source.** Drop a model, a prefab, a GameObject from the Hierarchy, an `.anim` or an export on
   the drop zone. The window says what it found, for example "hero.gltf: 1 skinned mesh, 41
   bones, 5,120 vertices, 6 clips". What each source bakes:

   | Source | What is baked |
   |---|---|
   | A model (`.gltf`, `.glb`, `.fbx`) | its animations (model clips, `hero.gltf#Walk`) |
   | `.anim` clips | the clips, played on the model, prefab or GameObject in **Play on** |
   | A GameObject | the states of its Animation Player, its entity Animations (converted like **Convert to Clip**), and its Pose Animator's animation |
   | A pose animation (`.poseanim`) | its frames, on the Pose Animator's model |
   | Lattice tracks | the deformation of the meshes bound to the animated lattice (an `.anim` `lattice` channel, or a lattice deforming the source) |
   | A Houdini VAT 3 or OpenVAT export | re-packed (see [Importing](#importing-houdini-and-openvat)) |

   Every mesh under the source is merged into **one mesh**, with one submesh per material. A
   character made of ten parts is still one draw per material.
3. **Clips.** Tick the clips to bake (**All** / **None**). Each row shows the clip's frames, length
   and wrap, and has its own sample-rate override.
4. **Settings.**
   - **Mode**: Auto, Vertices or Bones (see [Choosing a mode](#choosing-a-mode)). With Auto, the
     window shows what Auto picked and why.
   - **Sample rate**: frames per second, default 30, up to 240.
   - **Normals**: bake a tangent frame per vertex (Vertices mode). Turn it off for a flat-shaded
     look or to halve the size; the rest normals are used instead.
   - **Precision**: Auto, Half or Float. Auto bakes half floats and switches to float when half
     would move a vertex more than the tolerance (1 mm, or 0.05% of the model's size when larger).
   - **Preset**: **Mobile & Web** or **Desktop** (table below).
   - **Sockets**: nodes whose transform is baked too, so a sword can follow a hand. Select them in
     the Hierarchy and use **Add Selected**, or type a node path (`Armature/Hips/...`).
5. **Estimate.** Before anything is sampled, the window shows the texture size, the memory in
   total and per clip, the texel fetches per vertex, and a bar against the preset's limit. Each
   problem appears as a row with its fixes as buttons ("Lower to 20 fps").
6. **Preview, then Bake.** **Preview** (next to Bake) bakes in memory only: nothing is written.
   **Bake** bakes and writes the files. A progress bar ("Stage 1 of 2: Posing clip 2 of 5: Walk",
   then "Stage 2 of 2: Encoding") and **Cancel** take the button's place while it runs; Cancel
   writes nothing. The scene stays editable between slices, but an edit to the source fails the
   bake ("the scene changed during the bake: bake again"). The **Preview** section then shows the
   source and the VAT result side by side, live (the VAT drawn by the GPU as in the game), with
   play / pause, scrub and a clip dropdown. **Heatmap** colours the VAT side by each vertex's
   largest error over the frames and between them (rounding to half floats, the bones' fit,
   interpolation): green is none, yellow the tolerance, red twice it or more, with a legend and
   the largest error in mm. It is drawn unlit on a preview copy of the mesh; the asset is not
   changed. Imports have no heatmap (their error isn't measured per vertex). After a Preview run, **Bake** with the same settings
   writes that result without baking again. When the bake is written, the new `.vat` is selected
   in the Project window and the Console gets a line with what was written and a Lua call to play
   it. Nothing can be baked while a scene plays.
7. **Output.** The file goes to the **Output** path. By default that is the source's folder and
   name: `models/hero.gltf` gives `models/hero.vat` and `models/hero.vat.glb`. **Place** says what
   else happens:

   | Place | Result |
   |---|---|
   | **Files only** (default in the window and the Project window) | the two files, nothing in the scene |
   | **Beside** (default from the Inspector) | a new GameObject "*Name* (VAT)" with the source's transform, a Model (`hero.vat.glb`) and a Vertex Animator (`hero.vat`), selected |
   | **Replace** | the same, and the source GameObject is disabled (only for a GameObject source) |
   | **Crowd...** | a crowd made with the [Create Crowd](#create-crowd) settings |

   The bake is **one undo step**. Undoing it deletes the files, but only if they are unchanged
   since the bake. With Files only, the step is on the project history; otherwise it is on the
   scene's. The rules for existing files:
   - A `.vat` at the default path that was baked from the same source is **rebaked**. It keeps its
     GUID and your edits (clip names, events, speeds, wraps).
   - Any other existing file at the default path makes the bake take a free name (`hero 2.vat`).
   - An Output path you typed that exists is refused (a problem row) until you click its
     **Overwrite it** button; undo then puts the old files back.

   Textures embedded in a glTF are written next to the `.vat` as `<stem>_<image>.png`.

Dragging a `.vat` from the Project window into the Scene view or the Hierarchy creates a
GameObject that plays it, in one undo step. It gets a Model set to the `.vat.glb` and a Vertex
Animator set to the `.vat`.

### Presets

| Preset | Texture width | Largest payload | Memory warning above | For |
|---|---|---|---|---|
| **Mobile & Web** (`mobile_web`, default) | 2048 | 2048 × 2048 texels (4.2 M) | 32 MB | every platform, WebGL2 and OpenGL ES 3 included |
| **Desktop** (`desktop`) | 4096 | 4096 × 8192 texels (33.5 M) | 256 MB | desktop GPUs only; needed for long Vertices bakes and imported fluids |

A bake over the preset's limit is refused with `too_large`. The fixes are listed by how much they
save: a lower sample rate (the exact rate that fits), no normals, Bones mode, leaving out the
largest clip, then the Desktop preset. A Desktop bake that a phone can't load still plays there,
but at rest (see [Troubleshooting](#troubleshooting)).

### Choosing a mode

- **Vertices** stores every vertex's position (as an offset from the rest pose), plus its tangent
  frame when Normals is on, for every frame. It bakes anything that moves vertices: skinning,
  morph targets (blend shapes), lattices, and imported simulations. The size grows with vertices
  × frames.
- **Bones** stores each bone's rotation, translation and uniform scale for every frame. The GPU
  skins the mesh from that texture. It takes kilobytes instead of megabytes, the normals are
  exact, and sockets come for free. It works for skinned characters and rigid hierarchies, where
  each moving part becomes a bone with its vertices at weight 1. Bones-mode bones live in the
  texture, not the 128-bone skinning array, so a destruction with hundreds of pieces fits (up to
  65,535).
- **Auto** (default) bakes Bones when skinning and rigid parts explain every vertex within the
  tolerance, and Vertices otherwise. The report says why ("morph targets move vertices up to 4.1
  mm beyond what the bones explain"). Non-uniform scale or shear on a bone also picks Vertices
  (`nonuniform_bone_scale`).

Texel fetches per vertex per frame, the GPU cost (the estimate and the MCP report show it as
`fetches_per_vertex`):

| Mode | Fetches | During a crossfade or blend |
|---|---|---|
| Vertices, without normals | 2 | 4 |
| Vertices, with normals (default) | 4 | 8 |
| Bones, skinned (up to 4 bones per vertex) | 16 | 32 |
| Bones, rigid parts (one weight-1 bone per vertex) | 4 | 8 |

Pick Bones for skinned characters that don't use blend shapes: their payload is tiny and costs
nothing to download. Pick Vertices for low-poly crowds on weak GPUs (4 fetches against 16), or
when the face or clothes use blend shapes or lattices.

### Create Crowd

**Create Crowd...** in a `.vat`'s Inspector view (or **Place: Crowd...** in the baker) creates
one GameObject with a Model, an Instanced Mesh and a Vertex Animator, in one scene undo step. Its
settings:

- **Count**: the number of instances.
- **Area**: a box (**Size**, X and Z) or a disc (**Diameter**), around a **Center**. Instances
  stand on the **Selected surface** (a raycast down onto its mesh, or its heights for a terrain) or
  on the **Ground plane** through the center.
- **Min Spacing**: the smallest distance between instances. When the area is too small, fewer
  are placed, and the message says how many.
- **Random Yaw**, and a **Scale** range.
- **Clips (weights)**: which clip each instance plays, for example 70 Walk and 30 Idle (only the
  ratios count). With no weights, every instance plays the default clip.
- **Speed**: a range each instance picks its own speed from.
- **Random Start**: each instance starts at a random time of its clip, so the crowd never marches
  in step.
- **Seed**: the same settings and seed give the same crowd on every machine.
- **Instance Events**: see [Events](#events).

## The Vertex Animator component

**Add Component > Animation > Vertex Animator**. It goes on the GameObject whose Model is the
`.vat.glb`. Its Inspector section:

| Field | Meaning |
|---|---|
| **Animation** | the `.vat` |
| **Clip** | the clip **Play Automatically** starts (empty: the first). **▶ Preview** next to it plays the GameObject and its instances in the Scene view in edit mode, with a time scrubber. It is off by default, so screenshots of the scene stay the same. It stops on Play, a save or another selection, and the scene is not changed |
| **Speed** | multiplies every clip's speed. Negative plays backwards |
| **Random Start** | on by default: the GameObject, and each instance, starts at a random time of its first clip (seeded by the entity and the instance index) |
| **Play Automatically** | plays the clip when the game starts. Off: a script calls `play(name)` |
| **Default Fade** | the seconds of `crossFade(name)` called without a fade |
| **Instance Events** | off by default: fire clip events and finishes for every instance (see [Events](#events)) |
| **Attachments** | GameObjects that follow a socket every frame, such as a weapon in a hand. **+ Add Attachment** takes a socket of the `.vat` and a GameObject. A child of this GameObject takes the socket's local pose; any other GameObject is placed at the socket's world pose |

The section warns when the animation can't play:

- **`mesh_mismatch`**: the Model isn't the mesh the `.vat` was baked for. One click on **Use
  hero.vat.glb** sets it, in one undo step. Until then the mesh draws at rest.
- **The device's texture limit**: the payload is taller than this GPU allows.

In a scene or prefab file:

```yaml
vertexanimator:
  enabled: true
  animation: models/hero.vat
  guid: 5a7c0de0b1a5e5c0ffee00112233aabb
  defaultClip: Walk
  playAutomatically: true
  speed: 1
  randomStart: true
  defaultFadeTime: 0.2
  instanceEvents: false
  attachments:
    - {socket: RightHand, entity: 12}
```

Selecting a `.vat` in the Project window shows its **asset view**:

- a header with the mode, the vertices or bones, the texture size and format, the memory and the
  bake error;
- the clips table. Name, wrap, speed and interpolation are editable there. The events column
  opens the clip's events (**+ Add Event**: time, function, args);
- the sockets, which you can rename;
- the staleness banner with **Rebake**, **Open in Baker** and **Create Crowd...**.

These edits change the `.vat` file, so they are **project** undo steps. Every Vertex Animator
playing the file reloads, in edit preview and in play mode.

### Playback

- `play(name)` starts a clip now. `crossFade(name, fade)` fades from the current clip to `name`
  over `fade` seconds, linearly. At its end the new clip is the current one. A crossfade started
  during a crossfade continues from the clip that dominates (weight 0.5 or more).
- `blend(a, b, weight)` holds two clips at a fixed weight of `b`, with `b`'s phase following
  `a`'s, as when walking and running are mixed by speed. Calling it again with the same pair only
  moves the weight.
- **Wraps** (each clip's own, editable in the `.vat`):
  - `loop` repeats, interpolating from the last frame back to the first.
  - `once` stops on its last frame and calls `onFinish`. `clamp_forever` holds its last frame and
    calls `onFinish` once, when it first reaches it.
  - `ping_pong` plays forward, then backward.
- `queue(name)` plays `name` when the current clip ends, or ends its cycle for `loop` and
  `ping_pong`. With nothing playing, it plays at once. `play`, `crossFade` and `stop` clear the
  queue.
- `stop()` holds the current pose (`isPlaying()` is false) until the next `play`.
- **Interpolate** (per clip, on by default) mixes the two nearest frames. Off holds each frame
  until the next, for a stepped look; imported fluids are always stepped.
- In edit mode time stands still unless **Preview** is on. The GPU and the CPU compute the frame
  with the same formula from one clock. The clock is rebased every 2048 seconds, so precision
  doesn't degrade in long sessions.

### Events

Clip events are copied from the source `.anim` events when baking, and kept by name on a Rebake.
They are edited in the `.vat` asset view. An event fires when the clip's time crosses it (forward,
backward, across loops), once per crossing. Only the dominant clip fires: in a crossfade, the
outgoing clip until the midpoint, then the incoming one; in a `blend`, the clip with weight 0.5 or
more.

For a GameObject **without an Instanced Mesh**, the engine calls, in order:

1. every `onEvent` subscriber, with ONE `AnimationEvent` (`event.functionName`, `event.args`,
   `event.state` = the clip name, `event.time`);
2. the method with the event's function name on each enabled Lua script of the GameObject, with
   the args as parameters: `onFootstep("left")`.

This is the same as the Animation Player's events
([animation-clips.md](animation-clips.md#events)). An event nothing handles logs one warning.

A crowd's instances fire only when **Instance Events** is on. Then each instance calls
`onInstanceEvent(index, event)` and `onInstanceFinish(index, clip)`. Script methods are not
called for instances. This costs work per instance each frame, so leave it off for large crowds
that don't need it.

Events and finishes fire in play mode only, after the frame's update, in time order. Calls made
inside a handler take effect on the next frame. C++ scripts subscribe to the component's
`onEvent`, `onFinish`, `onInstanceEvent` and `onInstanceFinish` with `REGISTER_COMPONENT_EVENT`
(the script editor's event list has a "Vertex Animator" entry).

### Sockets and attachments

A socket is a node of the source (any bone in Bones mode, any node of the hierarchy in Vertices
mode) whose transform is baked alongside the animation. `getSocketPose(name)` returns its
position and rotation in the GameObject's space at the current time, blended through crossfades
exactly as the GPU blends the mesh. `getSocketPose(name, i)` samples instance `i`'s clip and time.
That pose is in the mesh's space, before the instance's own position, rotation and scale.

**Attachments** (Inspector) move a GameObject onto a socket every frame for the GameObject's own
animation. To attach something to an instance, move it from a script with `getSocketPose(name,
i)`.

## Lua

A crowd made with Create Crowd, driven by a script on the crowd GameObject. Each instance gets a
random clip and speed, waves on a key press and goes back to idle when the wave ends, and
footsteps play a sound:

```lua
---@class Crowd : ScriptInstance
local Crowd = { properties = {
    { name = "step", displayName = "Footstep", type = "AudioSource" },
    { name = "sword", displayName = "Sword", type = "GameObject" },
} }

local CLIPS = { "Walk", "Walk", "Idle", "Run" }     -- Walk twice as often

function Crowd:init()
    -- nil when the GameObject has no Vertex Animator (VertexAnimator(scene, entity) adds one)
    self.vat = GameObject(self.scene, self.entity):getVertexAnimator()
    self.started = false

    -- per instance: needs Instance Events (here, or in the Inspector)
    self.vat.instanceEvents = true
    self.vat.onInstanceEvent = function(i, event)
        if event.functionName == "onFootstep" and i < 20 then self.step:play() end
    end
    self.vat.onInstanceFinish = function(i, clip)
        if clip == "Wave" then self.vat:crossFadeInstance(i, "Idle", 0.3) end
    end

    RegisterEngineEvent(self, "onUpdate")
    RegisterEngineEvent(self, "onKeyDown")
end

function Crowd:onUpdate()
    -- the clips are known once the .vat is loaded
    if not self.started and self.vat:isLoaded() then
        self.started = true
        for i = 0, self.vat:getInstanceCount() - 1 do
            self.vat:playInstance(i, CLIPS[math.random(#CLIPS)])
            self.vat:setInstanceSpeed(i, 0.8 + 0.4 * math.random())
        end
        self.vat:randomizeTimes(42)     -- out of step; the same seed, the same times
    end

    -- a sword in instance 0's hand: the socket pose is in the instance's own space,
    -- so place it with that instance's transform (the sword is a child of this GameObject)
    local pose = self.vat:getSocketPose("RightHand", 0)
    if pose.valid then
        local instance = Model(self.scene, self.entity):getInstance(0)
        self.sword.position = instance.position + instance.rotation * (pose.position * instance.scale)
        self.sword.rotation = instance.rotation * pose.rotation
    end
end

function Crowd:onKeyDown(key)
    if key == Input.KEY_SPACE then
        for i = 0, self.vat:getInstanceCount() - 1 do
            if self.vat:getInstanceClip(i) == "Idle" then
                self.vat:crossFadeInstance(i, "Wave", 0.2)   -- Wave is a once clip
            end
        end
    elseif key == Input.KEY_R then
        self.vat:playAll("Run", 0.25)                         -- the GameObject and every instance
    end
end

return Crowd
```

For a single character (no Instanced Mesh), call `play`, `crossFade` and `queue`, and handle
events as script methods:

```lua
function Hero:init()
    self.vat = GameObject(self.scene, self.entity):getVertexAnimator()
    self.vat.onFinish = function(clip)
        if clip == "Attack" then self.vat:crossFade("Idle", 0.2) end
    end
end

function Hero:attack()
    self.vat:crossFade("Attack", 0.1)
    self.vat:queue("Idle")
end

function Hero:onFootstep(side) end     -- the clip's event {function: "onFootstep", args: ["left"]}
```

`VertexAnimator` methods (instances count from 0, like `Mesh:getInstance`):

- GameObject:
  - `play(name)` and `play(name, fade)` (with `fade` > 0 it is `crossFade`).
  - `crossFade(name)` (Default Fade) and `crossFade(name, fade)`.
  - `blend(a, b, weight)`, `queue(name)`, `stop()`, `isPlaying()`, `isPlaying(name)`.
- Properties:
  - `time`: seconds in the current clip, writable.
  - `normalizedTime`: 0 to 1, read only.
  - `speed`: writable; the clips keep their phase when it changes.
  - `currentClip`: the clip faded to during a crossfade, `a` in a blend.
- Clips: `getClipNames()` (empty until loaded), `getClipLength(name)` (seconds; 0 for an unknown
  clip).
- Sockets: `getSocketPose(name)` and `getSocketPose(name, i)` return a `VertexAnimatorSocketPose`
  with `position` (Vector3), `rotation` (Quaternion) and `valid`.
- Instances:
  - `playInstance(i, name)`, `playInstance(i, name, fade)`, `crossFadeInstance(i, name, fade)`.
  - `setInstanceSpeed(i, speed)`, `setInstanceTime(i, seconds)`.
  - `getInstanceClip(i)`, `getInstanceNormalizedTime(i)`, `getInstanceCount()`.
  - `playAll(name)` and `playAll(name, fade)` play the GameObject and every instance.
  - `randomizeTimes(seed)` puts every instance at a seeded time of its clip.
- Settings, all writable:
  - `animation` (the `.vat`, relative to the assets folder; setting it loads the file),
    `defaultClip`, `playAutomatically`, `randomStart`, `defaultFadeTime`.
  - `instanceEvents`, `preview` (editor: animate in edit mode), `enabled`.
- Status: `isLoaded()`, and `getLoadError()` (why it doesn't play, such as `mesh_mismatch: ...`;
  `""` when it plays).
- Callbacks: `onEvent(event)`, `onFinish(clip)`, `onInstanceEvent(i, event)`,
  `onInstanceFinish(i, clip)`.
- `getVertexAnimatorComponent()` returns the ECS component.

`GameObject:getVertexAnimator()` returns `nil` when the GameObject has none.
`hasVertexAnimator()` tells whether it has one. `VertexAnimator(scene, entity)` adds the
component when missing. Calls made before the `.vat` is loaded are kept and applied on load. An
unknown clip name or instance index logs one warning and does nothing. The full reference is in
[docs/mcp/guide/lua-api.md](mcp/guide/lua-api.md), section "VertexAnimator".

## Importing Houdini and OpenVAT

Right-click the export in the Project window and choose **Import as Vertex Animation...**, or drop
it on the baker's Source. You can give any part of the export: its folder, the mesh, a texture,
Houdini's `.mat` or data JSON, or OpenVAT's `-remap_info.json`. The importer finds the rest and
lists every missing piece with its fix. The textures are decoded on the CPU and re-packed into the
`.vat` layout, so nothing of Houdini's or Blender's encoding reaches the game. The `.vat` records
the export as its source: Rebake imports it again.

### Houdini (SideFX Labs VAT 3)

Export with the Labs **Vertex Animation Textures** ROP, version 3:

- **Target Engine: Unity**. Unreal-target exports are converted too, from the data JSON's axis
  system. When the target can't be told, the import stops with "re-export with Target Engine =
  Unity, or pass axis".
- Keep the whole export folder (`geo/<asset>_mesh.fbx`, `tex/<asset>_pos.exr`...,
  `unity/<asset>_mat.mat` or `data/<asset>_data.json`). The frame rate comes from that metadata.
  When it has none, the importer asks for it (`fps_unknown`).
- EXR (HDR) and PNG/TGA (LDR) textures both work, and so do **Two Position Textures**.

| Houdini mode | Imported as | Notes |
|---|---|---|
| **Soft-Body Deformation** | Vertices | normals from the rotation texture, or from compressed normals |
| **Rigid-Body Dynamics** | Bones, one bone per piece (Vertices above 65,535 pieces) | fast spins (over 60° between frames) and **Smooth Trajectories** are upsampled to extra frames, and the report gives the new frame count. **Stretch by Velocity** is dropped (`stretch_by_velocity_ignored`), and colours that change over time keep their first frame |
| **Dynamic Remeshing** (fluids) | Vertices, absolute positions, no interpolation | one position per triangle corner per frame, much larger than Houdini's packed form. Use the **Desktop** preset: under Mobile & Web it is refused with `too_large`. Surface UVs in the colour texture are not supported (`fluid_uv_from_color_ignored`) |
| **Particle Sprites** | not supported | refused (`unsupported`) |

A colour texture becomes the mesh's vertex colour in Vertices mode.

### OpenVAT (Blender)

Export with OpenVAT and keep the files together: `<name>_vat.exr` (or `.png`), the optional
`<name>_vnrm.*` normals, `<name>-remap_info.json`, and the mesh (glTF or FBX) with its `VAT_UV`
layer. The animations listed in `remap_info` become clips. Without them, every frame is one clip.
Blender's Z-up becomes Y-up. Only OpenVAT's files are read: none of its Blender add-on (GPL) is
used.

## The files

A Vertex Animation is two files that the Project window keeps together through rename, move,
copy and delete:

- **`<name>.vat`**: JSON, written canonically (fixed key order, 2-space indent, the same bytes
  for the same data) so diffs are readable. It holds the clip table, events, sockets, bounds, the
  layout, and the **recipe** (the sources, the settings and the sources' content hashes) used for
  staleness and Rebake.
- **`<name>.vat.glb`**: the render mesh and the baked texture, written only by the baker and the
  importers. The Project window marks it "generated": don't edit it. It holds one mesh with the
  source's materials and textures. Each vertex has a `_VAT_INDEX` attribute: its row in the
  texture, the same for every LOD. The payload is in the glTF extension
  `JASPER_vertex_animation`, and the mesh bounds cover every frame, so culling is right.

```json
{
  "format": "jasperengine.vat",
  "version": 1,
  "guid": "5a7c0de0b1a5e5c0ffee00112233aabb",
  "mode": "bones",
  "mesh": "hero.vat.glb",
  "layout": {
    "precision": "float",
    "width": 2048,
    "height": 4,
    "samples": 91,
    "vertices": 0,
    "bones": 41,
    "normals": true,
    "colors": false,
    "layout_hash": "c9b9fb72356d50c9"
  },
  "bounds": {"min": [-0.61, 0, -0.55], "max": [0.63, 1.92, 0.71]},
  "clips": [
    {
      "name": "Walk",
      "start": 0,
      "frames": 31,
      "fps": 30,
      "wrap": "loop",
      "speed": 1,
      "interpolate": true,
      "bounds": {"min": [-0.4, 0, -0.5], "max": [0.4, 1.9, 0.6]},
      "events": [
        {"time": 0.25, "function": "onFootstep", "args": ["left"]}
      ]
    },
    {
      "name": "Idle",
      "start": 31,
      "frames": 60,
      "fps": 30,
      "wrap": "loop",
      "speed": 1,
      "interpolate": true,
      "bounds": {"min": [-0.35, 0, -0.3], "max": [0.35, 1.92, 0.3]}
    }
  ],
  "sockets": [
    {"name": "RightHand", "path": "Armature/Hips/Spine/Chest/RightShoulder/RightArm/RightHand"}
  ],
  "recipe": {
    "sources": [
      {"kind": "model", "path": "assets/models/hero.gltf", "clips": ["Walk", "Idle"], "hashes": {"assets/models/hero.gltf": "sha1:…"}}
    ],
    "mode": "auto",
    "fps": 30,
    "preset": "mobile_web",
    "normals": true,
    "precision": "auto",
    "tolerance_mm": 1,
    "sockets": ["Armature/Hips/Spine/Chest/RightShoulder/RightArm/RightHand"]
  },
  "stats": {"bytes": 131072, "max_error_mm": 0.21, "mean_error_mm": 0.04}
}
```

| Key | Who writes it | Meaning |
|---|---|---|
| `format`, `version` | required | `"jasperengine.vat"`, `1` |
| `guid` | the baker | the asset's identity, kept by Rebake |
| `clips[].name`, `wrap`, `speed`, `interpolate`, `events` | **editable** | `wrap`: `loop`, `once`, `ping_pong`, `clamp_forever`. `speed` multiplies the player's speed. `events`: `time`, `function`, `args`, as in an `.anim` |
| removing a whole clip | **editable** | its frames stay in the texture until the next Rebake (the Inspector says so) |
| `sockets[].name` and their order | **editable** | names must be unique |
| `mode`, `mesh`, `layout`, `bounds`, `stats` | generated | `mode`: `vertices` or `bones`. `layout.layout_hash` must match the one in the `.vat.glb` |
| `clips[].start`, `frames`, `fps`, `bounds`, `rest` | generated | where the clip's frames are in the texture. `"rest": "none"` marks absolute positions (imported fluids) |
| `recipe` | the baker or importer | the sources (`kind`: `model`, `anim`, `entity`, `pose`, `houdini_vat3`, `openvat`) and settings for Rebake |

The editable fields are lenient, as in `.anim` files: a bad value is repaired with a warning that
carries its JSON path, and the rest loads. A generated field that was changed, or that no longer
matches the `.vat.glb`, fails to load with "generated field *X* changed: rebake (Inspector >
Rebake, or bake_vertex_animation {rebake})", and the mesh draws at rest. Unknown top-level keys are
kept. The editor reloads a hand-edited `.vat` within a second, and the Vertex Animators playing it
pick up the change.

## Staleness and Rebake

The recipe stores a content hash of every file the bake read. When one of them changes (a model
re-exported, an `.anim` edited), the `.vat` asset view shows "Source changed since baking", names
the file, and offers **Rebake**. Rebake runs the recipe again into the same files, as one undo
step. It keeps:

- the GUID, so every scene keeps working;
- the clip names, events, speeds, wraps and interpolation, and the socket names. Clips are matched
  by name first, then in order.

Clips you removed stay removed. For imports, Rebake reads the export again.

`jasperengine-editor check --project . --vat` reports, for every `.vat` (see
[docs/mcp/checks.md](mcp/checks.md)):

- `vat-source-changed`: a source changed since the bake;
- `vat-source-missing`: a source is gone;
- `vat-mesh-mismatch`: the `.vat.glb` is missing or doesn't hold the payload;
- `vat-layout-mismatch`: the `.vat` and its mesh come from two bakes, or a generated field was
  edited.

From the command line:

```bash
jasperengine-editor bake-vat --project . --source assets/models/hero.gltf --clips Walk,Idle --dry-run
jasperengine-editor bake-vat --project . --source assets/models/hero.gltf --clips Walk,Idle \
    --mode auto --fps 30 --preset mobile_web --output assets/vat/hero.vat
jasperengine-editor bake-vat --project . --rebake assets/vat/hero.vat
```

`--dry-run` prints the report without writing (`--format json` for scripts). The exit code is 0
on success, 1 when the bake is refused or fails, and 2 for bad arguments.

## Troubleshooting

- **The mesh stands still at its rest pose.** The Inspector's Vertex Animator section says why:
  - **`mesh_mismatch`**: the Model is not the `.vat`'s mesh. Click **Use hero.vat.glb**.
  - **A load error**: see the items below.
  - **In edit mode, time stands still**: turn on **Preview**.

  From Lua, `getLoadError()` returns the same text.
- **"Vertex Animation '...' needs a WxH texture, over this device's limit of N: drawing at
  rest".** The payload is taller than this GPU allows: usually a Desktop-preset bake on a phone or
  in a browser. Rebake with the Mobile & Web preset, fewer frames (a lower sample rate) or fewer
  clips.
- **`too_large`.** The bake doesn't fit the preset. Apply the fixes in their order: lower the
  sample rate, turn Normals off, use Bones mode, leave out a clip, or switch to Desktop.
  `memory_high` is the same warning before the limit.
- **`materials_not_vat_ready`.** A material uses a shader forked into the project. The `.vat.glb`
  can't carry it, so the VAT mesh draws with the built-in Lit material. Fork the shader again from
  the library (library shaders are VAT-ready) and assign it to the VAT GameObject's material. A
  project shader forked before this version doesn't include the vertex animation code, so a mesh
  using it **draws at rest**. Fork it again, or switch to the library shader.
- **`material_simplified`.** The `.vat.glb` keeps PBR colours and textures only. A library shader's
  features, Stochastic Tiling and second-UV textures are dropped. Set up the material again on the
  placed GameObject.
- **"generated field ... changed: rebake".** `layout`, `mode`, `mesh` or a clip's
  `start`/`frames`/`fps` was edited, or the `.vat` and `.vat.glb` come from different bakes.
  Rebake (Inspector, or `bake-vat --rebake`).
- **"not a JasperEngine vertex animation: this is a Unity asset (YAML)" (or an EXR, a PNG, a
  Houdini JSON).** A file from another tool was named `.vat`. Import the export with **Import as
  Vertex Animation...** instead.
- **The crowd marches in step.** Turn on **Random Start**, or call `randomizeTimes(seed)` after
  giving instances their clips.
- **An event does nothing.**
  - Events fire in play mode only.
  - On a GameObject with an Instanced Mesh they fire only with **Instance Events** on, and only
    to `onInstanceEvent`, never to script methods.
  - On a single GameObject the handler is a method of a Lua script on that GameObject, named
    exactly like the event's function, or an `onEvent` subscriber. The callback gets one `event`
    value.
- **`large_interpolation_error`.** A clip moves too much between two frames (fast spins, impacts).
  Raise its sample rate (the fix gives the rate that fits), or turn its interpolation off for a
  stepped look. When the warning says the clip "doesn't end where it starts", the error is at the
  loop seam, where a looping clip jumps from its last pose back to its first: no rate fixes that.
  Bake that clip with the wrap `once` or `clamp_forever`, or turn its interpolation off.
- **`precision_switched`.** Half floats would move vertices more than the tolerance, so the bake
  used float (twice the memory). Accept it, or set Precision to Half.
- **`mixed_rest`.** The source has no usable bind pose, so the rest mesh is the first frame of the
  first clip. Only the mesh drawn without the animation changes.
- **`unbound_tracks` / `no_animation`.** The clips' tracks don't reach the source's nodes: clips
  made for another rig, or renamed nodes. Play them on the model they were made for, or fix their
  paths in the Animation window.
- **`bone_limit`.** Over 65,535 bones in Bones mode: bake in Vertices mode. As a warning, Bones
  mode can't reproduce morph targets or lattices: use Vertices or Auto.
- **`vertex_limit`.** More than 16.7 million vertices: bake a LOD of the model.
- **`clip_limit`.** A Vertex Animation holds at most 64 clips. Split it into two assets.
- **`socket_not_found`.** The socket path doesn't name a node under the source. Pick it from the
  list or with **Add Selected**.
- **`scene_playing`.** GameObject sources bake in edit mode. Stop Play first.
- **Houdini imports.**
  - `unknown_target`: re-export with Target Engine = Unity, or give the axis.
  - `fps_unknown`: give the frame rate.
  - `missing_file`: the message names the file and how to get it, such as "export with Normals
    enabled".
  - `metadata_mismatch`: the textures and the mesh come from two exports. Export again.
- **Clicking or physics hits the character where it stands at rest.** Picking and colliders use
  the rest pose. Add colliders that fit the motion, or use an Animation Player for that character.

## Not in this version

- An animator state machine or graph (Lua drives the clips).
- Root motion, IK and ragdolls on baked characters.
- Picking and physics against the animated shape (they use the rest pose).
- Motion vectors (the renderer has none yet).
- GPU-driven culling and indirect draws.
- Houdini Particle Sprites.
- Per-instance `queue` and `blend`.
- Attachments for instances (a weapon per instance as a second instanced mesh).
- A per-LOD update interval (distant characters advancing every Nth frame).
- A crowd brush that paints instances in the Scene view.
- Recording a play-mode physics simulation to a Bones bake.
- A 16-bit normalized precision option.
- A lookup-table layout that keeps Houdini's packed fluids small enough for mobile.
- Vertex colours baked from engine sources: only imports carry a colour texture.
