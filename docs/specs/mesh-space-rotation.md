# Mesh Space Rotation for animation layers

Wave: follow-ups to S21. Owner stream: animation. Status: implemented.

Source of the idea: Kevin Iglesias' "Spine Proxy" for Unity
(<https://www.keviniglesias.com/spine-proxy.html>, `SpineProxy.cs`, and the Godot port `spine_proxy.gd` by
hlarsen). Neither file states a licence, so neither is copied: this spec takes the idea only. Unreal's Layered Blend
Per Bone has the same idea as its "Mesh Space Rotation Blend" option. Line numbers below are of `main` at `97d450661`; the animation files are unchanged at `c4e3b58ad`.

## 1. Goal

When an upper-body layer (an attack, a reload, a wave) plays over a locomotion layer, the torso must point where the
upper-body clip points, not where the run's hips happen to twist. Today a masked Override layer blends each bone in
its parent's space, so the spine inherits the base layer's hip rotation and a sword swing lands off to the side.

1. A layer gets one checkbox, **Mesh Space Rotation**. With it on, the bones where the mask starts or stops keep the
   rotation they have relative to the character, not relative to their parent. The torso faces where the
   upper-body clip faces, and the legs keep facing where the run faces.
2. It works for every clip and rig: no extra bone, no specially exported animation, no hierarchy to rebuild, no
   script. Rigs from Kevin Iglesias' packs (`B-` bone names) are detected as humanoids.
3. People can find it (a warning with a Fix button where the problem shows), and see it (a gizmo and a compare
   toggle). Agents can set it, and measure the error before and after as a number.
4. It is off by default everywhere. Existing projects play and save exactly as before.

## 2. Scope

### In scope

| Step | Content |
|---|---|
| 1 | Humanoid detection of `B-` rigs; proxy bones never mapped; a note when a rig has a spine proxy bone |
| 2 | Animator runtime: the Mesh Space Rotation pass, the controller field, `CompiledLayer`, Lua |
| 3 | Editor: the layer field in the Animator window, the warning and Fix, the Torso Direction gizmo, Compare in Preview, docs |
| 4 | Agents: `edit_animator_controller` fields, `sample_animation_pose` model space and solo layer, `check_animation` `layer_blend`, `check --anims` warning, guide |
| 5 | Animation Player: the same field, runtime pass, Inspector column, Lua and `set_properties` path |

### Out of scope

- **Follow Hips** (a 0 to 1 amount of hip yaw and tilt the torso still takes). It is the answer to hip spins past
  180 degrees and to wanting some locomotion bob in the torso. It is a follow-up and needs its own acceptance bar.
  The data model here leaves room for it (D7).
- Mesh space for translation or scale. Translation stays local, as in Unreal, so an attack's crouch or lunge from
  its hips is not carried over (§9).
- Additive layers and layer 0. The option has no meaning there (D5).
- Reading the `B-spineProxy` track at runtime, or a compatibility mode that copies it onto the spine. The native pass
  gives the same result from the clip itself (D6).
- Copying any code, names or comments from `SpineProxy.cs` or `spine_proxy.gd`.
- Per-bone partial blending in mesh space for bones inside the mask (Unreal's Blend Depth). Bones inside the mask
  keep local blending (D2).

## 3. Current state

Verified by reading the code at `97d450661`; nothing below was run.

- **The blend is local.** The layer combine copies the base layer's output, then for each layer above it calls
  `poseBlend(finalPose, finalPose, run.output, run.weight, mask, channelMask)`
  (`engine/core/animation/AnimatorInstance.cpp:1736-1750`). `poseBlend` lerps each bone's local TRS
  (`engine/core/animation/AnimatorPose.h:93-114`). S21 chose this (`docs/specs/S21-animation.md:259-262`). A bone
  whose mask weight is 1 under a parent whose weight is 0 keeps the layer's local rotation under the base layer's
  parent, so it inherits the base layer's hip rotation.
- **The layer clip still has its own hips.** `sampleSlot` samples the whole clip into `run.output` whatever the mask
  (`AnimatorInstance.cpp:1566-1573`). So the layer clip's rotation relative to the character can be computed for
  every bone, with no proxy bone.
- **Masks are per-bone floats, baked at bind.** `run.mask[b]` comes from `maskWeightOf`
  (`AnimatorInstance.cpp:345-383`) under the one rule in `AvatarMaskWeights.h:4-21`: path entry, then humanoid body
  part, then `default`. The humanoid **Body** part covers hips, spine, chest and upper chest together
  (`AvatarMaskWeights.h:11-13`). A body-part mask with Body on therefore takes the hips from the layer, and the run's
  legs inherit the attack's hip twist: the same problem in the other direction.
- **Rig space exists, after the combine only.** `poseToRigSpace` and its variants (`AnimatorPose.h:152-157`) and
  the instance's `spaceRotation` buffers are filled in `runIK` (`AnimatorInstance.cpp:1793-1795`), after every layer
  has been blended. The pose before a layer's blend is gone by then, so the fix belongs inside the combine loop.
- **Skinning reads the instance, not Transforms.** `AnimatorSystem::writePose` builds skin matrices from
  `instance.rigWorld()` (`engine/core/subsystem/AnimatorSystem.cpp:561-583`). A Lua copy of the spine rotation after
  the update (Spine Proxy's `LateUpdate` approach) would move the bone's Transform and not the mesh. A script cannot
  fix this; the engine must.
- **Layer data.** `LayerDef` (`engine/core/animation/ControllerDef.h:171-179`) has name, weight, blending, mask,
  slot. `CompiledLayer` copies them (`engine/core/animation/CompiledController.h:123-133`). The reader is at
  `AnimatorAssetIO.cpp:1292-1305` and keeps unknown keys. The writer (`:1372-1384`) writes every known key every
  time. `kLayerMembers` is at `:1104`. `CONTROLLER_VERSION = 2` (`AnimatorAssetIO.h:47`).
- **Animation Player.** A separate composer works per entity and channel in local space
  (`engine/core/subsystem/ActionSystemLayers.cpp:543`, masks prepared at `:312`). Masked-out tracks are neither
  sampled nor accumulated, so the layer's own hips are not available there today. `AnimationPlayerLayer`
  (`engine/core/component/AnimationPlayerComponent.h:77-88`) has name, weight, blend, mask, maskGuid. The old layer
  design note left mesh space out (`docs/superpowers/specs/2026-10-31-anim-layers-design.md:62`). That directory is
  not current rules (AGENTS.md); this spec supersedes the note for this one feature.
- **Editor.** The Animator window's layer Inspector draws Weight, Blending, Mask and Montage Slot
  (`editor/window/AnimatorWindow.cpp:1818-1873`) and commits one project undo step through `stage`. The Animation
  Player's Layers table draws Weight, Blending and Mask through `layerFieldCommand`
  (`editor/window/InspectorWindowAnimLayers.cpp:270-518`).
- **Agents.**
  - `edit_animator_controller` checks layer op fields in `checkOpFields` (`editor/mcp/tools/AnimatorTools.cpp:810-823`)
    and applies them in `set_layer` (`:933-967`).
  - `sample_animation_pose` returns world transforms of the final pose only (`:1612-1689`).
  - `check_animation`'s checks are the table `kChecks` (`editor/mcp/tools/AnimationQualityTools.cpp:241`). It
    never runs the Animator's layer stack.
  - The `animation` tool group is at its limit of 13 tools (`tools/mcp/budget.json:75-79`).
  - No tool can measure how far the blended torso is from the layer clip's torso.
- **Lua.** Animator: `getLayerIndex`, `setLayerWeight`, `getLayerWeight`
  (`engine/core/script/binding/GameObjectClassesLua.cpp:4436-4450`). Animation Player: also `setLayerMask` and
  `setLayerBlendMode` (`:4179-4221`).
- **Kevin Iglesias' rigs are not detected as humanoids.** The prefix list is
  `{"mixamorig", "bip001", "bip01", "def-", "def_", "j_bip_", "b_", "bone_"}` (`engine/core/pose/HumanoidMap.cpp:253`).
  `B-hips` keeps its prefix and becomes the key `bhips`, which matches nothing, so his rigs fail detection and
  body-part masks do not apply to them. Once `b-` is stripped, `spineProxy` must not be taken for the spine: the
  excluded tokens are at `:68`.
- **The proxy bone survives import.** It is not a skin joint, so it imports as a plain node with its animation track
  (`editor/util/GlbBuilder.cpp:847-860,1104-1173`, `engine/core/animation/ModelSubClips.cpp:546-623`). It is
  harmless and unused.

## 4. Reference

Ideas only. Nothing is vendored or copied.

| Source | Take | Do not take |
|---|---|---|
| Spine Proxy article, `SpineProxy.cs`, `spine_proxy.gd` | The problem statement, the test case (run plus vertical sword swing), the limits it lists (hip translation, spins past 180 degrees, conflicts with torso aim) | Any code, comments or names. The proxy bone and the rebuilt `Rig/B-root/B-spineProxy` hierarchy. The offset read from the scene pose in `Awake` |
| Unreal Layered Blend Per Bone, "Mesh Space Rotation Blend" ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-blueprint-blend-nodes-in-unreal-engine)) | The option's name and meaning: rotation blends in mesh space, translation stays local | Blend Depth, per-bone mesh-space blending of every bone inside the mask |
| V Rising's layering write-up ([80.lv](https://80.lv/articles/v-rising-s-animation-layering-in-unity/)) | Prior art for the same fix through a root-parented proxy | |

`NOTICE.md` gets a line under "Ideas re-implemented, no code copied" (§5, R40).

## 5. Requirements

### Functional: step 1, rigs

- **R1.** Humanoid auto-detection strips a leading `b-` like `b_`. A rig named `B-hips`, `B-spine`, `B-chest`,
  `B-neck`, `B-head`, `B-shoulder.L`, `B-upperArm.L`, `B-forearm.L`, `B-hand.L`, `B-thigh.L`, `B-shin.L`, `B-foot.L`
  (and the right side) maps every required slot.
- **R2.** A name containing the token `proxy` is never mapped to a humanoid slot, with or without separators
  (`B-spineProxy`, `spine_proxy`, `SpineProxy`).
- **R3.** When a model has an animated node whose name contains both `spine` and `proxy` outside the hips' subtree,
  the humanoid notes say once, in the Avatar window's Notes tab and in the pose tools' notes: "B-spineProxy is a spine
  proxy bone for Unity's Spine Proxy. JasperEngine does not need it: turn on Mesh Space Rotation on the layer
  instead." The node and its track are kept unchanged.

### Functional: step 2, Animator runtime

- **R4.** `LayerDef` and `CompiledLayer` gain `bool meshSpaceRotation = false`.
- **R5.** **Boundary bones.** At bind, for every layer above 0 with Override blending and the option on, the
  instance lists the layer's boundary bones. A boundary bone is a rig bone `b` with a parent `p` where the mask
  starts or increases (`run.mask[b] > run.mask[p]`), or where it stops right under where it starts
  (`run.mask[b] < run.mask[p]` and the mask starts at `p`). An unmasked layer has none, so the option changes
  nothing on it.
- **R6.** **The pass.** For such a layer, at its place in the combine (`AnimatorInstance.cpp:1740-1750`):
  1. Before `poseBlend`, take the rig-space rotation of each boundary bone's parent chain in the pose so far
     (`below`) and in `run.output` (`layer`). Rotations only, parents first.
  2. Run the existing `poseBlend` unchanged. Translation, scale and float channels are its result.
  3. For each boundary bone `b`, parents first:
     `target = nlerp(belowRig[b], layerRig[b], run.weight * run.mask[b])` (shortest arc), then
     `local[b].rotation = normalize(conj(finalRig[parent(b)]) * target)`, where `finalRig` is the rig-space rotation
     in the blended pose with every earlier boundary already corrected.
- **R7.** **Results.** With layer weight 1 and mask weights of 0 or 1:
  - every bone whose mask weight is 1 has the layer clip's rig-space rotation, within 1e-5 per quaternion component
    (sign-aligned);
  - every bone whose mask weight is 0 has the rig-space rotation of the pose below the layer, within the same bound;
  - every bone's local translation and scale equal `poseBlend`'s result bit for bit.
- **R8.** **Unchanged where it should be.** With the option off, `finalPose` is bit-identical to `main` for every
  existing test and the Crowd200 scene. With the option on, a layer with weight 0 changes nothing, bit for bit.
- **R9.** **Order.** The pass runs per layer, inside the combine, before the next layer blends, so a later layer
  sees the corrected pose. IK, look-at and Foot IK (`runIK`) run after every layer, on the corrected pose. Root
  motion is unchanged.
- **R10.** **Frame of reference.** Rig space is the Animator GameObject's local space, the same space `runIK` uses.
  When the Animator applies root motion, both poses have it removed already (`sampleSlot`), so the reference is the
  character, not the clip's root bone path.
- **R11.** **Montages and transitions.** The pass works on the layer's `run.output` after its transitions and
  montages, so an upper-body montage on a flagged slot layer is corrected with no further work. A transition inside
  the layer blends locally first, then the pass applies once (D3).
- **R12.** **Retargeted clips.** Humanoid-retargeted clips are realised into the rig's local pose before the
  combine (`AnimatorInstance.cpp:1553-1564`). The pass needs nothing extra for them and gives R7's result on the
  target rig.
- **R13.** **Layer weight fades.** `setLayerWeight` with a fade drives `run.weight`, so the pass blends in rig space
  through the fade with no pop at the start or the end.
- **R14.** **Lua (Animator).** `Animator:setLayerMeshSpaceRotation(layer, enabled)` and
  `Animator:getLayerMeshSpaceRotation(layer)`, where `layer` is an index or a name like `setLayerWeight`. It is a
  runtime override for this Animator only. It does not write the controller asset, and resets on rebind like the
  other runtime layer values. An unknown layer is warned once and ignored (the getter returns false), as on the
  Animation Player.

### Functional: step 5, Animation Player

- **R15.** `AnimationPlayerLayer` gains `bool meshSpaceRotation = false`, compared in `operator==`.
- **R16.** The Player's composer applies the same rule as R5 to R7 for a flagged Override layer above 0. It samples
  the ancestor tracks of each boundary bone for that layer even where the mask is 0, into scratch that is allocated
  at prepare time only when some layer has the option on.
- **R17.** **Lua (Player).** `AnimationPlayer:setLayerMeshSpaceRotation(layer, enabled)` and
  `getLayerMeshSpaceRotation(layer)`. Unlike the Animator's, it sets the authored field, as `setLayerMask` and
  `setLayerBlendMode` do today.
- **R18.** The Animation Player and the Animator give the same pose, within 1e-5, for the same rig, clips, mask and
  weight with the option on.

### Platform and backends

- **R19.** CPU only, portable C++20 within `tools/check-cxx-subset.py`. No GPU change, so it works on every graphics
  backend and every export target. It builds for linux, web and android here.
- **R20.** The pass runs on the Animator's job workers like the rest of `update`: pure, no locks, no Lua, no
  allocation after bind.

### Performance

- **R21.** **Off costs nothing.** No layer with the option: no scratch is allocated, and the combine pays at most
  one predictable branch per layer.
- **R22.** **On is cheap.** Scratch is two `Quaternion` arrays of the rig's bone count, allocated at bind (about 2 KB
  for 64 bones). Per flagged layer per frame, the work is one rig-space rotation pass over the bones up to the
  deepest boundary bone's parent for each of the two poses, plus a few quaternion operations per boundary bone. No
  matrices.
- **R23.** Crowd200 with its existing upper-body masked layer, the option off: within S21's budgets (p95 at most
  2.5 ms, serial at most 8.8 ms, 0 allocations), and no slower than a same-machine `main` baseline beyond noise.
- **R24.** A new benchmark row `Crowd200MeshSpace`: Crowd200 with the option on for its masked layer. It stays
  within the same S21 budgets and 0 allocations in steady state. Results go in `docs/perf/animator.md` with the
  machine named. ARM stays unmeasured, and the doc says so.

### Editor UX (human)

- **R25.** **The field.** In the Animator window's layer Inspector, directly under Mask: a checkbox **Mesh Space
  Rotation**. Tooltip: "Bones where the mask starts or stops keep this layer's direction relative to the character,
  instead of following their parent. Use it for upper-body layers over locomotion, so the torso faces where this
  layer's clip faces. Unreal: Mesh Space Rotation Blend."
- **R26.** The checkbox is disabled on layer 0 and on Additive layers, with a tooltip saying why. A file that sets it
  on such a layer keeps the value and the runtime ignores it (D5).
- **R27.** Under the checkbox, when it is on and the Animator window has a preview model or a bound Animator, a
  read-only line lists the boundary bones by their GameObject names, for example "Applies at: B-spine" for a path
  mask from the spine, or "Applies at: B-thigh.L, B-thigh.R" for a body-part mask with Body on.
  With no model it says "Applies where the mask starts or stops".
- **R28.** Editing it is one project undo step named "Edit Layer <name>", through the window's existing `stage`
  path, like Weight and Blending.
- **R29.** **The warning.** When a bound Animator has an Override layer above 0 with the option off, weight above 0,
  and a mask that starts at a torso bone under a torso bone it leaves out (by humanoid slot: Hips, Spine, Chest,
  Upper Chest; with no humanoid, by name through the humanoid auto-detection), then it shows a warning, naming the
  first such start in rig order:
  "Layer 'Upper Body': the mask starts at Spine, so the torso follows the base layer's hips. Turn on Mesh Space
  Rotation to keep this layer's direction."
  - It appears in the Animator component Inspector and the Animator window's Problems strip. Clicking it in the
    Problems strip selects the layer.
  - It has a **Fix** button that turns the option on as one undo step.
  - It never changes anything by itself. Turning the option on (in the controller, or for that Animator from Lua),
    or setting the layer to Additive, clears it.
- **R30.** **Torso Direction gizmo.** A Gizmos > Animation > **Torso Direction** item. With it on, for a selected
  Animator with a flagged layer, the Scene view draws each boundary bone's forward axis:
  - the corrected axis in the overlay colour for "applied";
  - a ghost of the axis without the pass, in the overlay colour for "rejected".
  It draws nothing and keeps no snapshot when off. Colours come from `overlaycolor`.
- **R31.** **Compare.** In Animator Preview, a toolbar toggle **Compare** shows the pose without the pass while held
  on. It is a preview-only runtime flag: never saved, no undo, and it turns off when Preview stops.
- **R32.** **Animation Player.** The Layers table gets the same checkbox under Mask, with the same disabling,
  tooltip and boundary line. Undo goes through `layerFieldCommand`. The layer's summary row says "mesh space" when
  it is on, for example "override, mask UpperBody, mesh space".
- **R33.** **Docs.**
  - `docs/animator.md` and `docs/animation-clips.md`: what the option does, when to use it, a before and after pair
    of screenshots, and its limits (§9).
  - `docs/unity-glossary.md`: a row saying Unity has no equivalent, pointing to Unreal's term and to Spine Proxy.
  - `docs/pose-animation.md`: the `B-` naming and the proxy note.

### Agent UX (MCP, Lua, CLI)

- **R34.** **`edit_animator_controller`**: `add_layer` and `set_layer` accept `mesh_space_rotation` (bool). The
  result's `controller` echoes it for each layer. Setting it true on layer 0 or an Additive layer is not an error,
  but the result's `warnings` say it has no effect and why.
- **R35.** **`sample_animation_pose`** gains:
  - `space`: `"world"` (default, today's output) or `"model"` (rotations and positions in the Animator's rig space);
  - `solo_layer`: a layer name. The pose is then that layer's own output alone, unmasked, as the reference to
    compare against.
  - Both work in edit mode through the existing evaluation path. The result echoes them.
- **R36.** **`check_animation`** gains the check `layer_blend` in `kChecks`, with
  `layer_blend: {layer, base_state, state, times, bones}`. All are optional except `layer`; defaults are the layer's
  default state, the base layer's default state, 5 evenly spaced times over one cycle of the layer's state as it plays
  (its length over its speed and the Animator's), and the layer's boundary bones plus their children.
  - It evaluates the bound Animator's layer stack at each time and compares each listed bone's rig-space rotation
    with the solo layer's.
  - It returns `layer_blend: {layer, mesh_space_rotation, boundary_bones, max_angle_deg, worst_bone,
    facing_delta_deg, samples[]}`. `facing_delta_deg` is the angle between the two chest forward axes projected on
    the ground plane.
  - Above `limits.layer_twist_deg` (default 5) it adds the finding `anim-layer-twist`, whose `next` is the exact
    `edit_animator_controller` call that turns the option on.
  - It runs on the main thread in slices, as a job when it outlasts the usual limit, like its existing checks.
- **R37.** **Static check.** `check --anims` warns `anim-mask-root-inherits` for an Override layer above 0, option
  off, whose mask is a `paths` mask with a weighted entry whose parent path weighs 0, both of them torso names as the
  humanoid auto-detection reads them (R29's rule by name). It cannot know the rig, so the message says the warning
  is about the mask's shape. `docs/mcp/checks.md` lists the code.
- **R38.** **Coverage.** `get_animation {entity}`'s `mask_coverage` lists each layer's `boundary_bones` and its
  `mesh_space_rotation`, so an agent can see where the pass applies without running a check.
- **R39.** **Player.** Agents set the Player's field through
  `set_properties {animationplayer: {"layers[1].meshSpaceRotation": true}}`, the existing path for Player layer
  fields. `get_animation` echoes it.
- **R40.** **Budget and text.**
  - No new tool. The `animation` group's tool count stays 13. Its byte budget may grow by what R34 to R36 add, with
    the reason in `tools/mcp/budget.json`.
  - `docs/mcp/guide/animator.md` gets one line on when to use the option.
  - `tools/mcp/eval/cases.py` gets a case that fails without the new text: an agent asked to fix a twisted torso on
    an upper-body layer reaches `mesh_space_rotation: true` through `check_animation`'s `next`.
  - `docs/mcp/TOOLS.md` is regenerated.
- **R41.** **Credit.** `NOTICE.md`, under "Ideas re-implemented, no code copied": Kevin Iglesias' Spine Proxy (no
  licence stated) and Unreal's Mesh Space Rotation Blend, read for the idea only. The entry names the files that
  implement it and states that none of their code is here.

### Data and compatibility

- **R42.** **Controller files.** The layer key is `"mesh_space_rotation": true`, written only when true. A
  controller without it reads as false. Old controllers re-save byte for byte. No version bump and no migration
  (AGENTS.md: an optional new field gets a default). It joins `kLayerMembers` and the reader's keep list.
- **R43.** **Older editors.** An older editor keeps the key through its unknown-key path and writes it back.
- **R44.** **Scenes.** The Player's layer key is `meshSpaceRotation`, written only when true, so a scene's
  `defaultLayers` test and existing scenes are unchanged. Reflection and `RuntimeFields.gen.inc` are regenerated.
  `tools/check_stream_keys.py` passes.
- **R45.** **Unity import.** If S94's Unity `.controller` import lands first, it leaves the field false: Unity has
  no equivalent.

## 6. Design decisions

- **D1. Name.** The option is **Mesh Space Rotation** in the UI, `mesh_space_rotation` in controller JSON and MCP,
  `meshSpaceRotation` in C++ fields and the Player's scene key, `setLayerMeshSpaceRotation` in Lua. Unity has no
  term for it, so the glossary's no-alias rule is not broken. The industry term is the one people search for.
- **D2. Boundary bones only.** Mesh space applies only where the mask starts, and where it stops right under its
  start. With 0 or 1 weights the bones inside the mask get what full per-bone mesh-space blending gives, at a fraction
  of the work:
  - a child with the same weight as its parent inherits the corrected rotation through the hierarchy;
  - fingers left out of an arm mask keep Unity's behaviour (they follow the hand), as does a head left out of a torso
    mask: the mask stops there, but not right under where it starts;
  - both directions are covered where the layer's direction enters: the spine under the run's hips (a mask from the
    spine, or a lower-body mask that leaves the spine out), and the run's legs under the attack's hips when a
    body-part mask includes Body.
  With fractional weights inside the mask, the result is local blending inside and mesh space at the edges. That is
  documented, not a defect.
- **D3. After the layer's own blending.** Transitions and montages inside a layer blend locally into `run.output`,
  and the pass applies once at the mask edges. Correcting each clip before a transition would cost more and differ
  only during the transition.
- **D4. Default off, a warning and Fix to turn it on.** The owner chose this: no file or scene changes meaning, and
  the editor points at the option where the problem is visible. The editor never turns it on by itself, including
  for new layers.
- **D5. Override layers above 0 only.** Layer 0 has nothing below it. Additive layers add deltas, so a
  rotation relative to the character has no meaning there. The field is kept when set on them, and ignored.
- **D6. No proxy compatibility mode.** The layer clip's own hips give the spine's rotation relative to the
  character exactly, so the proxy track adds nothing. Reading it would tie the feature to one vendor's naming and
  break on rigs where the track was baked against a different bind pose. The proxy node stays in the model (S53
  forbids changing a model's nodes) and the notes (R3) say why it is unused.
- **D7. Room for Follow Hips.** The pass computes `target` from `belowRig` and `layerRig`. A later Follow Hips amount
  would decompose the hips' difference into twist about the up axis and swing, and fold a part of it into `target`.
  That needs no change to R5's boundary list or to the stored key, only a new optional field.
- **D8. API sketch.**

```cpp
// ControllerDef.h, LayerDef and CompiledLayer
bool meshSpaceRotation = false;   // Override layers above 0: boundary bones keep rig-space rotation

// AnimatorInstance: per layer, from bind
std::vector<uint16_t> boundaryBones;   // parents first; empty when the option is off
// shared scratch, sized at bind only when some layer has boundaryBones
std::vector<Quaternion> belowRig, layerRig;

// AnimatorPose.h, pure, no allocation
void poseMeshSpaceRotation(Pose& final, const Pose& below, const Pose& layer, const int* parents,
                           const uint16_t* boundary, size_t boundaryCount, const float* mask, float weight,
                           Quaternion* belowRig, Quaternion* layerRig);
```

```lua
animator:setLayerMeshSpaceRotation("Upper Body", true)
local on = animator:getLayerMeshSpaceRotation(1)
player:setLayerMeshSpaceRotation("Upper Body", true)
```

```json
{"op": "set_layer", "layer": "Upper Body", "mesh_space_rotation": true}
{"checks": ["layer_blend"], "entity": "Hero", "layer_blend": {"layer": "Upper Body", "base_state": "Run", "state": "Attack"}}
```

## 7. Acceptance criteria

| # | Check | Verification |
|---|---|---|
| AC1 | R1, R2 | `HumanoidMapTest`: a `B-` rig maps every required slot; `B-spineProxy`, `spine_proxy` and `SpineProxy` map nothing. Red on `main` for the `B-` rig |
| AC2 | R3 | Unit test on a generated rig with a proxy node: the note appears once in the humanoid notes; the node and its track are unchanged. MCP: the pose tools' notes contain it |
| AC3 | R5 | `AnimationLayersTest.MeshSpaceBoundaryBones`: a path mask from Spine gives exactly {Spine}; a Body-on body-part mask gives the legs' first bones; an unmasked layer gives none |
| AC4 | R6, R7 | `AnimationLayersTest.MeshSpaceRotationKeepsTheLayerDirection`: base hips yawed 90 degrees, layer hips yawed -20 degrees, Spine path mask. With the option, Spine's rig rotation equals the layer clip's within 1e-5 and the legs equal the base's. Without it, the Spine error is 110 degrees: the red control |
| AC5 | R7 | Same test with a Body-on mask: the legs keep the base's rig rotation, red without the option. Translation and scale equal `poseBlend`'s bit for bit |
| AC6 | R8 | Existing `AnimationLayersTest` and `AnimatorLayers*` tests stay green unedited. A weight-0 flagged layer leaves `finalPose` bit-identical |
| AC7 | R9, R11 | Test with two flagged layers: the second sees the first's corrected pose. A montage on a flagged slot layer is corrected. A Look-At IK target still reached (S21's IK test stays green) |
| AC8 | R10 | Test with root motion on and the layer clip turning its root: the reference is the Animator, within 1e-5 of the expected rig rotation |
| AC9 | R12 | `HumanoidModelTest`: a retargeted upper-body clip on the robot rig meets AC4's bound on the target rig |
| AC10 | R13 | Fade test: over a 0.3 s weight fade, the Spine's rig rotation is continuous (no step above the per-frame motion of either pose) |
| AC11 | R20, R21, R22 | `AnimatorHotPathTest`: 0 allocations after bind with the option on. Option off: no scratch allocated (scratch size 0) |
| AC12 | R23, R24 | `je-bench-anim`: Crowd200 within budget against a same-machine `main` baseline; `Crowd200MeshSpace` row within the S21 budgets; `docs/perf/animator.md` updated |
| AC13 | R14 | Lua test: set and get by index and by name; an unknown layer is warned once and changes nothing; the value resets on rebind; `tools/lua_api` output has both functions |
| AC14 | R15, R16, R18 | `AnimationPlayerTest`: the AC4 case on the Player meets the same bound, red without the option; Player and Animator agree within 1e-5 |
| AC15 | R17 | Lua test: the Player's setter changes the authored field |
| AC16 | R42, R43 | `AnimatorAssetIOTest`: an old controller re-saves byte for byte; a true value round-trips; a file written with the key reads in a build without the field (unknown-key path) and writes it back |
| AC17 | R44 | Scene round-trip test for the Player key; `defaultLayers` unchanged; `check_stream_keys.py` passes |
| AC18 | R25 to R28, R32 | `AnimatorWindowTest` and `AnimationLayersUiTest`: the checkbox under Mask, disabled on layer 0 and Additive, one undo step that restores the file bytes, boundary line text. Screenshots from an editor on a scratch project |
| AC19 | R29 | UI test: the warning shows for the AC4 setup, Fix turns the option on as one undo step and the warning clears; it never shows for an Additive layer or with the option on |
| AC20 | R30, R31 | Overlay test: gizmo off draws nothing (draw counter 0); on draws two axes per boundary bone. Compare toggles the pass off in Preview and resets when Preview stops. Screenshots |
| AC21 | R34 | `test_animator_controller.py`: `set_layer` sets the key and the result echoes it; layer 0 and Additive give a warning, not an error |
| AC22 | R35 | `test_animator.py`: `space: "model"` and `solo_layer` on the AC4 setup; the solo pose's Spine equals the blended Spine within 1e-4 degrees with the option on, and differs by 110 degrees off |
| AC23 | R36 | `test_animation_quality.py`: `layer_blend` reports about 110 degrees and `anim-layer-twist` with a `next` call; applying the `next` call and rerunning reports under 0.01 degrees and no finding |
| AC24 | R37, R38 | CLI test: the static warning fires for a path mask from Spine and not for a mask that includes Hips. `get_animation` lists `boundary_bones` |
| AC25 | R39 | MCP: `set_properties` path sets the Player field; undo restores it |
| AC26 | R40 | `tool_budget.py --check tools/mcp/budget.json` passes; `gen_tool_docs.py` output committed; the eval case fails without the new text and passes with it |
| AC27 | R19, R41 | `WERROR=1 tools/build.sh linux`, `unit-tests`, `web`, `android`; `check-cxx-subset`; `tools/ci/check-notices.py` |
| AC28 | Whole | Before landing: `tools/check-changed` on each commit; `MCP_SUITE=all tools/build.sh mcp-tests -k "animat"`; the docs' before and after screenshots taken from an editor on a scratch project |

## 8. Dependencies and conflicts

- **Depends on** S21 (layers, masks, combine, IK) and S77 (`check_animation`, `kChecks`, its job slicing). Both are
  on `main`.
- **Touches the same files as** S94 (`origin/feature/s94-…`: Avatar Masks, Unity `.controller` and `.mask` import):
  `AnimatorInstance.cpp` (mask bake), `AnimatorAssetIO.cpp` (layer keys), `AnimatorTools.cpp`. Merge whichever
  lands second onto the first, and keep R45.
- **Merge hotspots:** `AnimatorInstance.cpp` combine loop, `AnimatorAssetIO.cpp` `kLayerMembers`,
  `editor/window/AnimatorWindow.cpp` `drawLayerInspector`, `InspectorWindowAnimLayers.cpp`,
  `ActionSystemLayers.cpp`, `tools/mcp/budget.json`, `docs/specs/README.md` index.
- **Steps.** Step 1 ships alone. Step 2 blocks 3, 4 and 5. Steps 3 and 4 can run in parallel. Step 5 needs step 2's
  `poseMeshSpaceRotation` kernel.

## 9. Risks and defaults

| Risk | Default |
|---|---|
| An attack's crouch or lunge from its hips translation is lost, because translation stays local | Documented as a limit (R33). Unreal does the same. A later spec can carry a filtered part of the hips' vertical translation |
| Hip spins past 180 degrees in the upper-body clip twist the torso a full turn over still legs | Documented. `check_animation layer_blend` shows it as a large angle. Follow Hips (out of scope) is the fix |
| Torso aim or look-at constraints fight the pass, as they do with Spine Proxy | They do not: IK runs after the combine on the corrected pose (R9). AC7 checks it |
| The Player's composer needs ancestor tracks that the mask skips, and the change grows | Step 5 may record a deviation and ship after steps 1 to 4. The Animator alone meets the goal for controllers |
| Fractional mask weights inside the mask give local results inside and mesh space at the edges | D2: documented, not a defect |
| Non-uniform scale on the chain above a boundary bone | The pass composes rotations only, as `poseToRigSpace` does for rotation. Documented as assuming uniform scale on that chain |
| The warning's rule (R29) fires on layers where local blending is intended | It only proposes. The option stays off until Fix or the checkbox. A layer can silence it by being Additive or by having the option on; no other silencing in this spec |
| `layer_blend` sampling is slow on large controllers | Default 5 times and the boundary bones only; runs as a job past the usual limit (R36) |

## Deviations

- **R1, R2: only for new imports and reimports (the lead's decision when landing).** A model imported before
  these rules keeps the humanoid status it had. The rules are numbered (`pose::HUMANOID_NAME_RULES`: 1 the
  original, 2 strips `b-` and never maps `proxy`), and a model's `.humanoid` sidecar records the rules of its import
  as `name_rules` (optional, absent is 1, no sidecar version change). A model arriving in the open project (a new
  import) or reimported (the Project window's Reimport, Reimport All Models, the Model Importer's and the Mesh Audit
  window's Reimport, `reimport_models`) gets `name_rules: 2` written into its sidecar when rules 2 map its bones
  otherwise than the rules it has; elsewhere nothing is written. A model that was in the project when it opened
  keeps rules 1 until then, and its humanoid notes say "Reimport the model to apply the current naming". A model
  put back after a delete counts as an import. The Mesh Space Rotation warnings (R29, R37), the Fit To Skeleton
  of capsule shadows and other readings of a rig without a sidecar use the current rules.
- **R14: an unknown layer is warned, not an error.** The Animator's setter and getter warn once per layer and
  ignore an unknown one (the getter returns false), as the Animation Player's and the Animator's other name
  lookups (`setFloat`, `play`) do, so the same script mistake does the same thing on either component.

- **R5, D8: boundary bones of every masked layer.** The instance lists the boundary bones of every masked layer
  above 0, whatever its blending and option, not only of flagged Override layers. The editor's warning (R29) and
  `get_animation`'s coverage (R38) need them while the option is off. The list is bind-time data, not scratch: the
  two scratch arrays (R21, R22) are still sized only while a layer runs the pass.
- **R5, D2: where the mask stops.** The first rule listed every bone whose weight differs from its parent's, in
  both directions, so fingers left out of an arm mask (hand 1, fingers 0) took the base layer's rotation relative to
  the character and no longer followed the hand, against D2. The owner's rule replaces it: a bone is a boundary bone
  where the mask starts or increases, or where it stops right under where it starts (the legs under the hips of a
  Body-on mask, the spine under the hips of a lower-body mask). Two bones count as where a mask starts without a
  parent's weight to compare with: a bone right under the root when it weighs above 0 (rig space has nothing to
  inherit), and, for a mask with body parts on a humanoid rig, the hips when they weigh above 0. The bones above the
  hips are the mask's root part (`root_hierarchy`), so a body-part mask that is on from the root (everything but
  the legs) starts its body at the hips, as Unity's humanoid masks do; without that its legs would follow the
  layer's hips. A path mask on from the root of a rig with a node above the hips (`Armature`) starts at that node,
  so its legs follow the hips: give the mask the hips and below to keep the run's legs. The rule stays
  `anim::maskBoundaryBones`, shared by the Animator's bind, the Animation Player's composer and the editor's line.
- **R29, R37: one torso rule.** The warning first fired for any boundary bone under a bone the mask leaves out on a
  rig without a humanoid, and the static check for any weighted entry under an unweighted parent, so a mask that
  takes the hips under `Armature` or `B-root` (`Armature/Hips`, `Rig/B-root/B-hips`: the whole body) warned that
  "the mask starts at Hips". Both now share one rule: the mask starts at a torso bone under a torso bone it leaves
  out, by humanoid slot on a bound humanoid, else by name (`pose::isTorsoBoneName`, the auto-detection's reading:
  `B-hips`, `mixamorig:Spine1`). The start must be a torso bone too, so a mask from an arm (the shoulder under the
  chest) does not warn that the torso follows the base layer's hips, on a humanoid as before and now on a generic
  rig. The message names the first such start in rig order (the static check: the shallowest entry).
- **R14: "rebind" is a load of the controller.** The Lua override resets when the controller loads again (another
  controller, or the same file changed), where the layers' runtime state is rebuilt. It survives a rig rebind
  after a hierarchy change, like `setLayerWeight`, so a script's call before the first bind is not lost.
- **R30: what the gizmo's ghost is.** The runtime records, per boundary bone, the rig-space rotation right after
  the layer's own blend and before the pass (`AnimatorInstance::layerMeshSpaceUnpassed`), over the pose the
  layers below left, corrections included.
- **R34: where the warning comes from.** The "no effect" warning for the option on layer 0 or an Additive layer is
  the MCP validation's (`edit_animator_controller`, `get_animator` / `get_animation {controller}`, `write_file` of a
  `.controller`), not `anim::validateController`'s, so `check --anims` and the Animator window do not repeat it.
- **R36: what a bone is compared with, and when.** Each listed bone is compared with where the blend should put it:
  the solo layer's rig rotation where the mask takes it, the layers below where it does not
  (`poseBlendRotation(below, solo, weight * mask)`, the pass's target), so a Body-on mask's legs are measured against
  the run, not the attack. The layers below are the same evaluation with this layer and those above it at weight 0.
  The check plays both states from their start in edit mode, one time per main-thread slice with the scene put back
  after each; in play mode it is refused. `layer_blend` is listed as a free-form object (the handler checks its
  members) and `check_animation` has a budget exception, so the `animation` group keeps 13 tools.
- **R38: where the Animator's coverage is.** `get_animation {entity}`'s `mask_coverage` was the Animation Player's
  (`players[0].mask_coverage`). The Animator's masked layers are a new member, `animator: {entity, controller,
  mask_coverage: [{layer, covered, total, warnings, mesh_space_rotation, boundary_bones}]}`, the bones as paths from
  the Animator's GameObject; `get_animation {entity}` now answers for a GameObject that only an Animator plays.
- **R38, R39: where the Player's value is.** `get_animation` echoes a Player layer's option as `mesh_space_rotation` in
  `players[0].layers[]`, beside its weight, blend and mask. The Player's `mask_coverage` entries carry
  `mesh_space_rotation` and `boundary_bones`, the paths of the Inspector's Applies at line (the rule over the Player's
  rig, `animlayersui::meshSpaceBoundary`: the GameObjects its clips animate and their ancestors, through
  `anim::playerRig`, the runtime's rig of R16, so a GameObject no clip animates is never listed).
- **R16: the Player's rig, weights and sources.** The Player's rig is every GameObject its transform slots animate
  and their ancestors below the Player's GameObject, rig space that GameObject's local space. Boundary bones come
  from the layer's mask through `anim::maskBoundaryBones` (one rule with the Animator's bind and the editor's line);
  a state's mixing transforms add none, they weigh its contribution at the nodes as everywhere else. A boundary
  bone's weight is the Player's own blend weight there, `λ·min(1, S)` with `S` the states' weights times their mask
  weight (R6's `run.weight * run.mask[b]` for one state at weight 1). The table (the boundary bones and their
  ancestors, parents first) is built at prepare while a layer runs the pass, never per frame. When the scene's
  hierarchy revision changes, each bone of the table's rig is checked against the Transform parent it was built
  with, as the bindings and the humanoid rig are checked; a moved one (a humanoid's bone the retargeted clips bind
  by entity, which no path check sees) binds the masks' paths again and rebuilds the table. Its nodes are never
  occluded by a layer above. A bone no layer below wrote this frame is at its rest, and the layer's own rotation at
  a bone its clips do not rotate is the bone's rest (the Animator's `run.output`); a boundary bone no clip rotates
  gets a Rotation slot of its own. The layered edit-mode preview (the Animation window's Preview over) applies the
  option too (`PlayerPreviewLayer::meshSpaceRotation`).
- **R18: where the bound holds.** The Player and the Animator agree within 1e-5 where the layers' clips rotate the
  bones they mask in. A bone a masked Override layer's clips have no track for already blended differently before
  this spec (the Animator toward the bone's rest, the Player not at all); the pass does not change that.
