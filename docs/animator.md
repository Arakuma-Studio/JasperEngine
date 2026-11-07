# Animator

The Animator is Unity's Mecanim for JasperEngine: a component that plays an **Animator
Controller** (a `.controller` asset) on a GameObject's hierarchy. It is built on the engine's
Animation Clips (`.anim` files and model clips such as `models/hero.gltf#Walk`, see
`docs/animation-clips.md`) and on the humanoid maps of the Pose Animation system (`.humanoid`,
see `docs/pose-animation.md`).

| | Animation Player | Animator |
|---|---|---|
| What it plays | a list of clips, by name | a state machine of clips and blend trees |
| Control | `play`, `crossFade`, `blend`, `queue` | parameters (float, int, bool, trigger) that drive transitions |
| Layers, masks, additive | yes (clip layers with weights, Avatar Masks, override or additive) | yes (per state machine layer) |
| Montages, IK, root motion | no | yes |
| Retargeting | by bone names | humanoid clips play on any humanoid |

The Animation Player stays the simple path. A GameObject with both works: the Animator runs after
the player and owns the bones its clips animate. **Create Animator Controller from Animation
Player** (the Animator's **New from Player** button, the Animation Player's **Create Animator
Controller from Clips**, or `edit_animator_controller {from_player}`) makes a controller with a
state per clip of a player, its layers as controller layers.

A **disabled** Animation Player beside an Animator hands over to it: its `play`, `crossFade`,
`stop`, `setFloat` / `setBool` / `setTrigger`..., state reads and `onStateEnter` / `onStateExit`
go to the Animator (the Inspector says so on the player). Create Animator Controller from Clips
sets that up: it adds the Animator playing the new controller and disables the player in one undo
step. Scenes and prefabs made before the Animator joined the engine, whose Animation Player named
a controller, load the same way (the player's controller moved to an Animator).

## Getting started

1. Assets > Create > Animator Controller (or the Project window's Create menu). Double-click it to
   open it in Window > Animation > Animator.
2. Select the character and Add Component > Animation > Animator; drop the controller on its
   Controller field (or press **New Controller** in the component).
3. In the Animator window, drag clips (`.anim` files, or a model to pick one of its clips) from
   the Project window onto the graph: each becomes a state. The first state is the layer's
   default (orange). Right-click a state > Make Transition, then click the target state.
4. Add parameters in the Parameters tab, select a transition and give it conditions in the
   Inspector (Speed Greater 0.1, a trigger Jump...).
5. Press **Preview** in the Animator window to run the controller in edit mode (the scene comes
   back when it stops), or press Play. While playing, the window shows the active state's
   progress, the transition's on its arrow, and the parameters: edit them there to test.

## The Animator window

- **Layers** tab: add, rename, reorder (right-click), delete; select a layer to set its weight,
  blending (Override, Additive), Avatar Mask and montage slot in the Inspector.
- **Parameters** tab: add (Float, Int, Bool, Trigger), rename (Enter; conditions and blend trees
  follow), default values; in Play and Preview the live values (triggers as buttons).
- **Graph**: pan with the middle mouse (or Alt + drag), zoom with the wheel, box select, drag to
  move (grid snap). Entry is green, Any State cyan, Exit red, the default state orange,
  sub-state machines blue (double-click to enter, the breadcrumb or (Up) to go back).
  Right-click: Create State (Empty, From Clip, From New Blend Tree), Create Sub-State Machine,
  Make Transition, Make Transition from Any State, Set as Layer Default State, Delete (Del).
- **Inspector**: a selected state (motion, speed and multiplier, cycle offset, motion time, loop,
  tag, additive reference), transition (exit time, fixed duration, duration, offset, interruption
  source, ordered interruption, can transition to self, mute, conditions; in Preview a Blend
  slider shows the transition at any point), layer, or blend tree.
- Every edit is one undo step (Edit > Undo), named after what it did.

## Blend trees

A state's motion can be a blend tree: **1D** (one parameter, children at thresholds), **2D Simple
Directional** (one child per direction, optionally one at the centre), **2D Freeform
Directional** and **2D Freeform Cartesian** (any positions, gradient band interpolation), or
**Direct** (each child weighted by its own parameter). The Inspector shows each child's weight
for the current parameters, a weight graph (1D) or the sample diagram (2D) with the parameter
point (drag it while previewing), and **Compute Thresholds / Positions** from each clip's root
motion speed and direction. Children may be nested trees; a tree has at most 32 children.

## Layers and masks

The first layer always weighs 1. Further layers override (blend over the layers below by their
weight) or add (an additive layer adds each clip's difference from its reference pose: the first
frame, the rest pose, or a time). An **Avatar Mask** (`.mask`, Assets > Create > Avatar Mask)
limits a layer: each GameObject under the masked one gets a weight from 0 to 1, the first rule
that applies: its transform path entry (or the nearest parent's entry that includes children),
else its humanoid body part (click the parts on the figure: head, body, arms, hands, fingers, legs,
root; a part can weigh a fraction), else the mask's **Default**. **Root Hierarchy** also gives the
root part the GameObject and what lies between it and the hips. Animation Player layers use the
same masks. The **Preview Model** builds the transform tree and figure from a model or prefab.

## Montages

An **Animation Montage** (`.montage`) is a one-shot clip played on a layer whose **slot** it names
(Default: the base layer): attacks, emotes, reactions. It blends in and out with eases, is cut
into **sections** (each plays on into its next one, or ends the montage; a section that names
itself loops) and has its own events. Play it from Lua (`animator:playMontage("anim/wave.montage")`)
or MCP (`control_animator {montage}`); a second montage on the same slot interrupts the first
(`onMontageEnded(path, true)`).

## Events

Clip events (the Animation window's events lane) fire for the clip with the highest weight of each
layer (**Event Mode** Leader Only, the default) or for every playing clip (All), once per crossing.
They call the Animator's `onEvent` subscribers and, with **Send Events To Scripts**, the method of
the same name on the GameObject's scripts. State changes call `onStateEnter` / `onStateExit`.

## Root motion

With **Apply Root Motion** the clips' movement of their root bone (the clip's `root_motion.bone`,
else the Animator's **Root Motion Bone**, the humanoid hips, or the shallowest moving bone) moves
the GameObject instead of the bone: through its Character Controller when it has one, else its
Transform. The pose keeps the vertical motion (a jump stays in the pose) unless the clip bakes it.
A script setting `onAnimatorMove` takes the motion over.

## IK

The IK list runs after the layers and montages: **Two Bone** (upper, middle and end bone reach a
target, the middle bends toward a pole), **FABRIK** (a chain of 2 to 16 bones), **Look At** (up
to 4 bones turn toward a target within a clamp angle), **Foot IK** (a raycast below each foot:
the pelvis drops, the ankle meets the ground and turns to its slope) and **Foot Lock** (a planted
foot stays put). Bones are paths or humanoid slots (`left_hand`); targets are GameObjects, or set
from scripts (`setIKTarget`). Each constraint has a weight and an optional weight parameter.

- **Soften** (Two Bone, Foot IK, Foot Lock; 0 to 1, default 0): the part of the reach over which the
  end eases toward full extension. At 0 the leg has a hard limit at 0.999 of its reach: just before
  it the knee turns 2 to 3 degrees per mm of target, then it stops dead. Softened, the knee keeps
  moving, and the end stops short of the target from that part of the reach on. Try 0.03. It is off
  by default because it changes where feet land.
- **Hinge Axis** (the same; default zero, auto): the middle bone's local axis it only bends about.
  A positive turn straightens the joint, so the sign picks the side. A straight limb has no bend
  plane of its own, and the axis gives it one; the bone's twist survives. Auto bends toward the
  pole, else the current bend. An axis along a bone counts as auto. Under non-uniform scale on the
  chain only the side is kept, and like auto the solve can stop short of the target (it is then
  not reached).
- **Twist** (the same; degrees): turns the chain about the line from its first bone to the target.
- **Aim Offset** (Look At; default none): the eyes or the muzzle, from the last bone in its local
  space: their forward ray is what ends on the target.

**Look At** walks its bones from the first: each measures the turn again from where the ones before
it left the head and takes its share of it (its weight over what the remaining bones weigh), and
the last with a weight takes the rest. With the clamp out of the way and weight 1 the aim ends
within 0.1 degree of the target, also on a chain that leans. The clamp applies to the last bone's
turn from the animated pose.

**Foot IK** puts the centre of the sole on the ground: on a slope the ankle stands higher than **Foot
Height** (Foot Height / cos(slope)) because the foot turns to the slope, up to **Max Foot Angle**;
on a steeper slope the foot stops turning and the centre of the sole stays on the plane. The foot
keeps its animated rotation turned to the ground, whatever the knee's bend does to the shin. The
height stops growing past the steeper of 60 degrees and Max Foot Angle. **Foot Lock** holds the
ankle the same way: the foot keeps the rotation it came with, the animation's or Foot IK's.

A script reads the outcome of the last solve with `isIKReached(name)`: the end within 1 mm of its
target (a Look At: its aim within 0.1 degree, a Foot IK: on ground it found), measured on the final
pose in world space, so a partial weight, a soft limit, a clamp or a target out of reach is not
reached. It is false while a constraint does not solve: weight 0, disabled, culled.

## Humanoids and retargeting

A model whose bones map to the 22 humanoid slots is a humanoid (the Animator's Avatar section
shows it; **Configure Avatar...** opens the Avatar Configuration window: the body, finger, eye and
jaw mapping, twist corrections and a retargeted test clip, saved in the model's `.humanoid` file;
the Model Inspector's **Rig** foldout opens it too and sets the model's **Animation Type**).
A clip made on another humanoid model plays **retargeted**: each slot's rotation is moved from the
source rig's frames to the target's (a slot the target lacks folds into the next one it has), the
fingers, eyes and jaw follow, and the hips' movement is scaled by the rigs' sizes, so a Mixamo
walk plays on a Rigify robot. Set **Retarget** to Off to bind by bone path only. **Humanoid clips**
(`.anim` version 2, from **Convert to Humanoid Clip...**, **Bake to Humanoid Clip** or a model
whose Animation Type is Humanoid) play on any humanoid Animator.

A state's **Mirror** swaps left and right, **Foot IK** makes the feet reach the clip's foot goals
(no sliding when the legs are longer or shorter), and **Retarget** retargets its clips even when
the Animator's Retarget is Off; a blend tree child's Mirror flips its state's. They play the
state's clips through the humanoid path, so they need a humanoid avatar. The Animation Player's
Retarget, Mirror and Foot IK entries go through the very same path: one clip with the same options
gives the same pose on both. (The **Foot IK** IK constraint below is another thing: it grounds the
feet on colliders after every layer.)

## Performance

The Animator evaluates its characters in parallel (the job system), keeps poses in SIMD-friendly
arrays and writes skin matrices directly. **Culling Mode** Cull Update Transforms skips writing
the pose of characters no camera sees (time, states, events and root motion go on); Cull
Completely stops them. **Write Bones: Exposed** writes only the bones listed (and bones with
attachments) to their GameObjects: the skins still follow every bone. The budget: 200 humanoids
per frame (see `docs/perf/animator.md`).

What it costs right now: Window > Analysis > Profiler shows the Animator under the **Animation**
band of CPU Usage, and its monitors `Animators`, `Animators sampled`, `Animators culled`,
`Anim Prepare ms`, `Anim Wave ms`, `Anim Finalize ms` and `Clip memory` (`get_frame_times {monitors}`
for agents). Prepare and Finalize are main thread time; Wave is the wall time of the parallel update.
`Animation.Clips` in Jobs & Memory counts the clips loaded now.

## Files

`.controller` and `.mask` are JSON version 2, `.montage` version 1 (`"format":
"jasperengine.controller"`, `"version"`, a `guid`), written in a canonical order with unknown keys
kept. They are assets with GUIDs: references follow renames and moves, and renaming a clip
rewrites the controllers and montages that use it.

Version 1 files load converted: the Animator's earlier masks (true / false body parts, exact
paths) and the controllers and masks of the Animation Player's former controller support (flat
states and transitions, `humanoid` and `transforms` masks), which share the version 1 header and
are told apart by their members. The Inspector notes a converted file; the next edit saves it as
version 2, keeping every member (what the engine does not read is written back as it was).
`jasperengine-editor check --anims` reports them too.
