# Pose animation

Pose animation is pose-to-pose character animation for humanoid models: you set a few **key
poses** (a contact pose, a passing pose, a jump's apex...) and the engine snaps between them with
springy transitions. A **feel** decides how the snaps look (snappy, cartoon, rubbery, heavy,
stop-motion...) and **humanize** adds the small things that make it look alive: moving holds,
follow-through, idle sway, joint limits.

The result is a `.poseanim` asset. It plays live on a model with the **Pose Animator** component
(and from Lua), or bakes to a shared Animation Clip (`.anim`, see
[animation-clips.md](animation-clips.md)) or to a keyframe Animation in the scene. Poses are stored as joint angles that do
not depend on the model's skeleton, so the same asset plays on every humanoid model: a T-pose
or an A-pose rig, Mixamo, Blender, Unreal or VRM bone names. There is no retargeting step.

AI agents edit the same assets through the MCP tools `get_pose_animation`, `edit_pose_animation`
and `render_pose_animation` (see [docs/mcp/guide/pose-animation.md](mcp/guide/pose-animation.md)).

## Workflow

1. **Pick a character.** Open **Window > Animation > Pose Animation**. Select a humanoid model in
   the Scene (or the Hierarchy) and press **Use Selection** in the Character field, or drag the
   GameObject onto it. The chip next to it says `Humanoid 22/22` when every body part was found;
   otherwise it lists what is missing with a **Map Bones...** button (see
   [Bone mapping](#bone-mapping)).
2. **Create the animation.** **New** creates a `.poseanim` next to the model file with one neutral
   pose and the Snappy feel, and uses the character's model as the preview model. **Open** picks
   an existing asset (double-clicking a `.poseanim` in the Project window, **Create > Pose
   Animation** in the Project window and the Pose Animator's **Open** button in the Inspector work
   too). Or start from a [template](#templates).
3. **Pose with the handles.** While the window has a pose selected, the Scene view shows handles
   on the character: drag a **hand** or a **foot** sphere and the arm or leg bends to follow (IK),
   drag the yellow **hips** cube to lower or shift the body while the feet stay planted, click a
   joint dot to get **rotation rings** for it. Hold Shift to keep a hand or foot on a horizontal
   plane, Ctrl/Cmd to rotate rings in 5 degree steps. The character stays on the floor by itself
   while a pose is **Grounded**. Every drag is one undo step.
   You can also type angles: the Pose tab's **Joint angles** section has every joint in degrees,
   with its limits.
4. **Add poses.** **+** at the end of the pose strip (or `N`) duplicates the selected pose after it;
   change the copy. For a walk or a run, pose the right-side half once and use **Duplicate
   Mirrored** (`Ctrl/Cmd+Shift+D`): the copy is the exact mirror image and its name swaps L and R
   (`Contact R` -> `Contact L`). Drag cards to reorder them.
5. **Time it.** The timing bar under the strip shows every hold and every transition to scale,
   with the transition's curve (overshoot and wind-up are visible). Drag a hold end or a
   transition end to change it (values snap to frames; hold Alt to drag freely). **Auto Timing**
   gives bigger moves longer snaps while keeping the total length.
6. **Choose a feel** in the Feel tab: a preset chip (Snappy, Cartoon, Rubbery, Stop-motion, Heavy,
   Linear, Soft) and the fine controls under it: curve, snap time, overshoot, wind-up,
   choppiness (poses per second like stop motion, 12 = "on twos"), overlap (how much later hands,
   feet and head arrive), looseness (how much further they swing) and the body weights that scale
   overlap and looseness per body part. A single pose can have its own transition (Pose tab,
   **Custom transition**).
7. **Humanize** in the Humanize tab: moving holds (a held pose keeps drifting toward the next one),
   in-betweens, life (a little idle sway), head and torso follow-through, springiness, and joint
   limits (on by default: poses and frames stay within natural ranges).
8. **Preview.** Press Play (`Space`) or scrub: the character in the Scene view plays exactly the
   frames the game and the bake will play. One-shot animations replay after a short pause. Stop
   (`Escape`) returns to posing.
9. **Use it in the game**, one of two ways:
   - **Pose Animator** (live): add **Add Component > Animation > Pose Animator** to the model's
     GameObject and set its Animation, Autoplay, Speed and Weight. It plays in play mode and in the
     built game, and Lua controls it (below). Its **Open** button opens the asset in the window,
     **Bake** bakes it into the scene (the entity Animation below).
   - **Bake**: the window's **Bake** button opens a menu. The name comes from the field next to
     it (by default the file name). Either way the bake is one scene undo step, and baking again
     updates the same result.
     - **Bake to Animation Clip** writes `<name>.anim` next to the pose animation: the mapped
       bones' rotations and the hips' position. It lists the clip in the
       character's Animation Player (adding one when needed), so
       `AnimationPlayer:play(name)`, `Model:playAnimation(name)` and every other character with
       the same bone names can play it. Its keys can be edited in the Animation window.
     - **Bake into Scene** writes the frames to a keyframe Animation of the character in this
       scene, which `Model:playAnimation(name)` plays like any imported clip.

### Onion skin, balance and Global Edit

- The Pose tab can show the **previous** pose as a blue ghost and the **next** one as a green ghost
  (onion skin, with an opacity), and keep a ghost while previewing ("Ghost during preview").
- While posing, the Scene view draws a floor disc under the character and a ring under each foot
  that touches the floor. **Show balance** (Pose tab) adds a centre-of-mass diamond on the floor:
  green when the pose is balanced, orange when the character would fall.
- **Global Edit** (`E`, or the button on the pose strip header) spreads one change to many poses:
  change the selected pose (lean the torso forward, raise the arms...), then **Apply to All** or
  **Apply to Selected**. The pose before the edit shows as an orange ghost. **Cancel** or `Escape`
  undoes the change.

### Keyboard shortcuts

With the Pose Animation window focused, or the Scene view while posing:

| key | action | key | action |
|---|---|---|---|
| Space | play / pause | Ctrl/Cmd+D | duplicate pose |
| Left / Right | previous / next pose | Ctrl/Cmd+Shift+D | duplicate mirrored |
| Shift+Left / Right | move the pose left / right | M | mirror the pose |
| Home / End | first / last pose | Delete / Backspace | delete the selected pose(s) |
| , / . | previous / next frame while previewing | G | ground the pose |
| Escape | stop the preview / cancel Global Edit | Ctrl/Cmd+C / V | copy / paste a pose |
| F2 | rename the pose | N | add a pose (duplicate after) |
| E | Global Edit | Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z / Y | undo, redo |

Undo and redo follow the order of your pose edits while the window or a posing Scene view has
focus. The strip's right-click menu has the rest: Duplicate at End, Mirror (L <-> R), Copy and
Paste Pose (paste keeps the target's name, hold and transition), Reset to Neutral, Rename.

## Templates

Templates are ready-made animations that work on any humanoid. The Templates tab of the Pose
Animation window shows them as cards (a picture of the middle pose on your character, the name and
`N poses · loop` or `once`; hover for the description), with a category filter and a search field.

- **Click** a card to create a new animation from it: the dialog proposes
  `<model folder>/<template name>.poseanim` (numbered when that file exists); the new animation
  previews on your character and opens on the Pose tab. One undo step.
- **Right-click** a card: **Replace Poses in Current Animation** (the template's poses and feel,
  one undo step; the asset keeps its preview model), **Append Poses** (added after the current
  poses, the feel stays), and for your own templates (marked "user") **Show in Project** and
  **Delete** (asks first; undo brings it back).
- Without a character the cards have no pictures ("Bind a character to preview templates"). The
  window lists the templates also while no character or no animation is picked.
- **Save Current as Template...** stores the current animation as a project template (name,
  description, category). Project templates live in `<project>/pose-templates/`, so they are
  shared with everyone working on the project; one with the same name as a built-in template
  replaces it in the list. Saving and deleting can be undone.

The built-in templates:

| name | category | poses | plays | feel |
|---|---|---|---|---|
| Idle | Basics | inhale, exhale | loop | Soft with slow 1 s breaths, humanized (sway, moving holds) |
| Walk | Locomotion | contact, passing, mirrored (4), in place | loop | Snappy with a small overshoot |
| Run | Locomotion | contact, flight, mirrored (4); the flight poses leave the floor | loop | Snappy, short snaps |
| Sneak | Locomotion | crouched reach and step, mirrored (4) | loop | Soft, choppy (8 steps a second) |
| Cartoon Walk | Locomotion | contact, down, passing, up, mirrored (8) | loop | Cartoon |
| Jump | Actions | stand, crouch, launch, apex, land, recover | once | Rubbery on the way up; the moves into floor contacts have their own low-overshoot transitions |
| Wave | Gestures | hand left, hand right | loop | Cartoon, loose |
| Sit Down | Actions | stand, lower, sit, settle (sits on a seat behind the character) | once | Heavy |
| Punch | Actions | guard, wind-up, extend, recover | once | Snappy with a wind-up |
| Bow | Gestures | stand, bow, return | once | Soft, slower (0.65 s) |

The locomotion templates walk and run in place: move the GameObject in your script (or bake and
use root motion of your own) to travel.

## How poses are stored: joint angles

Every pose is a list of joint angles in degrees. Zero everywhere is the **neutral standing pose**
(standing straight, arms hanging at the sides, feet forward), not a T-pose. Each body part has up
to three angles: two that swing it and one that twists it, for example `left_upper_arm.raise`
(out to the side; 90 = horizontal), `left_upper_arm.forward` (swing forward) and
`left_upper_arm.twist`. Left and right parts use the same names and the same meaning: equal
values on both sides are mirror images. The full table with the signs and the limits is in the
[agent guide](mcp/guide/pose-animation.md#angles-degrees-from-a-neutral-standing-pose).

Because the angles are measured from each model's own neutral pose, they mean the same thing on
every humanoid. Fingers, eyes and the jaw are not part of a pose (they keep their rest pose).

A pose also has: a **hold** (seconds it is held before the transition out), **Grounded** (the
lowest point of the body touches the floor, automatically; turn it off for airborne poses and set
**Lift**, the height above the floor), and an **offset** that shifts the hips horizontally.

## Lua

The model's Pose Animator (`model:getPoseAnimator()` adds one if the model has none):

```lua
---@class Hero : ScriptInstance
local Hero = { properties = {} }

function Hero:init()
    self.anim = Model(self.scene, self.entity):getPoseAnimator()
    self.anim:setAnimation("anim/walk.poseanim")   -- relative to the assets folder
    self.anim:play()
    RegisterEngineEvent(self, "onKeyDown")
end

function Hero:onKeyDown(key)
    if key == Input.KEY_SPACE then
        -- a live spring transition to a pose of the animation, then hold it
        self.anim:goToPose("Contact R", 0.15)
    elseif key == Input.KEY_P then
        self.anim:play()
    end
end

return Hero
```

- `play()`, `pause()`, `stop()` (back to time 0), `isPlaying()`, `setTime(t)` / `getTime()` /
  `getLength()`, `setSpeed(s)` (negative plays backwards), `getPoseCount()`, `getPoseName(i)`,
  `findPose(name)`, `isLoaded()` (models load in the background; calls made before are kept).
- `goToPose(indexOrName [, duration [, overshoot [, anticipation]]])` springs from whatever pose is
  showing to that pose and holds it; playback stops.
- `onFinish` is called when a one-shot animation ends:
  `self.anim.onFinish = function() self.anim:goToPose("Stand", 0.2) end`.
- **Crossfading with `weight`**: `setWeight(w)` (0..1) blends the pose animation over what the model
  shows without it (its keyframe clip, or the rest pose). Fade it in or out over time:

```lua
function Hero:onUpdate()
    -- fade the pose animation in while the run key is held, out when released
    local target = Input.isKeyPressed(Input.KEY_LEFT_SHIFT) and 1 or 0
    local w = self.anim:getWeight()
    w = w + (target - w) * math.min(1, Engine.deltatime * 8)
    self.anim:setWeight(w)
end
```

  (register `onUpdate` with `RegisterEngineEvent(self, "onUpdate")` in `init`.) The base can be a
  clip the model plays (`Model:playAnimation`), including a baked pose animation; to switch to
  another pose animation, call `setAnimation` with the other file.

The complete reference is in [docs/mcp/guide/lua-api.md](mcp/guide/lua-api.md) (section
"PoseAnimator").

## Troubleshooting

- **"Missing: ..." / the model is not humanoid.** Bones are mapped to body parts from their names
  (Mixamo, Blender/Rigify, Unreal, VRM and Unity names). When a name is unusual, press **Map
  Bones...** and pick the bone for each missing part; "Auto" goes back to automatic detection. The
  fix is saved next to the model (`hero.gltf.humanoid`) and ships with the game. Hips, spine, head,
  upper and lower arms and legs, hands and feet are required; chest, upper chest, neck, shoulders
  and toes are optional.
- **The character floats or sinks.** A grounded pose always puts its lowest point on the floor; do
  not raise the hips to lift a pose, turn **Grounded** off and set **Lift**. A pose that leans so
  far that it would fall shows an orange balance marker (with Show balance on): move the hips over the supporting foot
  (drag the hips handle, or bring the standing leg under the body). The in-between frames are
  grounded too: between two grounded poses the feet stay on the floor, and a jump's frames are only
  kept from going below it.
- **A joint bends the wrong way or stops.** Joint limits are on by default: values outside the
  natural range are clamped (the angle fields show them in red). Turn joint limits off in the
  Humanize tab for stylised poses. A swing longer than 180 degrees is the same as a shorter swing
  the other way: keep it under 180 and use the twist angle instead.
- **The mirrored pose is not a mirror.** Use Duplicate Mirrored or Mirror (`M`): typing the other
  side by hand misses the sign of the centre twists and the hips offset.
- **Changing the feel does nothing.** Poses with their own transition (Custom transition, or after
  Auto Timing) ignore the animation's feel; **Reset Custom** under the timing bar clears them.
- **The bake does not play.** Both bakes use the name in the field (default: the file name).
  **Bake to Animation Clip** adds the state `walk` to the character's Animation Player: play it with
  `Model:playAnimation("walk")` or the player's `play("walk")`, or set it as the Default Clip.
  **Bake into Scene** writes an Animation named `walk` on the model; play it with
  `Model:playAnimation("walk")`. Baking again replaces the keys.
