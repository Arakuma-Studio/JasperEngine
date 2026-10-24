# Pose Animation, phase 5: viewport posing (IK handles, rings, floor, onion skin, Global Edit)

Status: approved design, ready for implementation.
Branch: `pose-animation`. Builds on phases 1-3; implemented in parallel with phase 4 against the
binding `PoseEditSession` contract (phase 4 spec, section 2). If phase 4 has not landed yet,
develop against the header as specified and a minimal test session; never change the contract
without recording it in the phase 4 spec.

## 1. Goal

Direct manipulation in the Scene view: drag a hand or a foot and the limb bends (IK), the
character stays on the floor, the hips handle keeps feet planted, any joint can be picked and
rotated with rings, the previous/next poses show as ghosts, and Global Edit spreads a change to
many poses. All of it removes chores instead of adding controls.

## 2. Viewport takeover

While `PoseEditSession::mode() == Posing` and the bound model is in the Scene view's scene, the
Scene view enters a pose-editing state (precedent: `TerrainEditWindow` + `isTerrainEditing()` in
`editor/window/SceneWindow.cpp` and its brush drawing in `editor/render/SceneRender.cpp`):

- The regular transform gizmo is hidden for the model and its bones; clicking empty space or
  another object in the Hierarchy leaves posing (the session goes Idle, bones restored) and
  selects normally; clicking the model keeps posing.
- Handles are drawn in the tools layer (`editor/render/ToolsLayer.h`), screen-size constant,
  hover-highlighted, depth-tested off (always visible), and hit-tested with the point-handle
  pattern (`hitTestLinePoint` style: handle AABB + ray), nearest-to-camera wins.
- Every drag is one undo step: `setWorking` during the drag (live), `commitWorking(name,
  mergeKey)` on release (or merged per drag frame with the same key).

## 3. Handles

| handle | look | behaviour |
|---|---|---|
| Hands | sphere, coral `#FF7A45` (left) / cyan `#36B3FF` (right), white when active | screen-plane drag of the target; phase 1 `solveLimbIK` (arm pole = -forward); the hand keeps its local rotation |
| Feet | sphere, same side colours | IK with the leg pole = forward, the foot keeps its world rotation; on a grounded pose the target height snaps to the floor + sole when within `0.02 * humanScale` |
| Hips | cube, yellow `#FFD54F` | grounded pose: `moveHipsKeepFeet`; ungrounded: moves the whole body (feet follow) |
| Joints | small dots on spine, chest, upper chest, neck, head, shoulders, upper/lower arms and legs, toes; side-coloured | click to make the joint active (shows rings) |
| Rotation rings | three discs on the active joint's canonical axes mapped to model space (red = L/pitch, green = U/twist, blue = F/roll), front half only, plus a free-rotate outer ring | drag rotates the joint's world rotation about the ring axis; converts back to the local rotation; with joint limits on, the rotation is clamped **during** the drag (the ring stops at the limit, a short red tick marks the limit) instead of snapping back afterwards |

After every handle change: limits (if on), then grounding if the pose is grounded (the lowest
point stays on the floor), then `setWorking`.

Shift while dragging a hand/foot: constrain to the horizontal plane; Ctrl/Cmd: snap ring
rotations to 5 degree steps. Shortcuts are documented in the window's tooltip and guide.

## 4. Floor and contacts

- A floor disc at the rig floor height under the character: radius `1.5 * humanScale`, 5% alpha
  fill, a 0.25 m grid clipped to the disc (lines from the editor's line renderer).
- Contact rings: a side-coloured ring under each foot whose height is within `0.01 * humanScale`
  (phase 2 metrics `contact`).
- A centre-of-mass marker (small diamond projected on the floor) that is green when balanced and
  orange when not (phase 2 metrics). Toggle in the window's Pose tab ("Show balance").

## 5. Ghosts (onion skin and Global Edit baseline)

- Ghost meshes are clones of the model's skinned meshes (`registry/EntityCloner.h`) flagged as
  editor overlays (`SceneRender::overlay`, `RenderSystem::setEditorOverlay`), with a dedicated
  ghost material: unlit-ish rim shading, alpha-blended, depth prepass so only the front surface
  shows, drawn after the model so they read on top. Bones of each ghost are driven directly
  (their own pose), never the scene's bones.
- Onion skin: previous pose in blue `#6FB3FF` at `onion().opacity`, next pose in green
  `#7CE38B` when `onion().next`; the loop wraps (pose 0's previous is the last pose when looping).
  Toggles live in the window's Pose tab.
- During preview the ghost can stay on ("Ghost during preview") showing the key pose the
  playhead is leaving.
- Global Edit baseline: an orange `#FF9F43` ghost of the pose before the edit, replacing the onion
  skin while Global Edit is active.
- Ghosts are never saved (no scene dirtying, no Hierarchy entries visible to the user, cleaned up
  on every exit path: mode change, unbind, scene close, play mode, editor shutdown).

## 6. Global Edit UI

A tool button on the pose strip header (phase 4 window) and the `E` shortcut start Global Edit on
the primary pose: the button and an orange bar ("Global Edit: change this pose, then apply the same
change to other poses") appear with **Apply to All**, **Apply to Selected** (the other selected
poses) and **Cancel**. Applying uses `PoseEditSession::applyGlobalEdit` (one undo step); when
nothing changed the bar says "Nothing changed yet". Selecting another pose, previewing or Escape
cancels.

## 7. Tests

- Logic without the GPU (handle hit-testing maths, ring-axis mapping, limit-during-drag clamping,
  screen-plane drag projection, floor snapping) in `editor/pose/PoseViewportLogic.{h,cpp}`, tested
  in `jasperengine-pose-tests`.
- `tools/mcp/tests/test_pose_viewport.py`: bind + posing on the fixture hero; the Scene view shows
  handles (screenshot `view: scene` differs from Idle in the handle colours); posing does not mark
  the scene modified (`list_scenes` `modified == false`); entering play mode, closing the window
  and saving the scene each restore rest bone transforms and remove ghosts (`get_scene_tree` has no
  extra entities); Global Edit apply via the session through a debug-only test hook if needed
  (or via the UI path exposed to MCP through `edit_pose_animation` equivalents); undo after a
  committed drag restores the previous pose.
- Full MCP suite and pose tests pass.

## 8. Implementation notes

(Implemented: `editor/pose/PoseViewportLogic.{h,cpp}` (maths, tested), `editor/pose/PoseViewport.{h,cpp}`
(one per 3D `SceneRender`), hooks in `SceneRender`, `SceneRender3D`, `SceneWindow`, `App`.)

- Handles live in the tools layer scene; the floor disc, grid, contact rings and centre-of-mass
  diamond are System-pool overlays of the edited scene; ghosts are `EntityCloner` clones (meshes,
  bones and glTF nodes only: scripts, sounds, animations, physics and the Pose Animator are left
  out) in a ghost layer scene of the Scene view drawn between the scene and the tools layer, so
  they read on top of the model and under the handles and are never in the scene's Hierarchy or
  file. Ghost material: library `hologram` (alpha blend keyword, no scanlines/flicker, tint alpha
  = 2 x onion opacity, fill 0.5: surfaces facing the view at the opacity, the rim up to twice it)
  with a `stencil_mask` extra pass (order Before, depth write on, no colour) as the depth pre-pass.
- Ring axes: the joint's canonical L/U/F in its effective parent's frame as seen in the neutral
  pose (`W_p * N_p^-1`; the character frame for the hips), so a ring turn changes the DOFs about
  that axis. The outer ring turns about the view direction. With joint limits on, a ring drag is
  clamped to the contiguous angle range around the start in which the joint gets no further
  outside its limits than it starts (the ring stops there); the range ends are the red ticks.
- Foot snap: "floor + sole" is the foot bone height of the foot standing on the floor as it is
  turned at the drag start (feet keep their world rotation), not the rest-pose sole offset
  (which differs once the neutral pose turns the foot).
- Contact rings use the section 4 threshold (height <= 0.01 humanScale), stricter than the
  metrics' `contact` (0.02).
- Clicking a hand, foot or the hips also makes it the active joint (its rings show). A selection
  change to anything outside the model and its bones (or to nothing) leaves posing; the scene's
  transform gizmo is not placed on the model or its bones while posing. In a posing Scene view the
  toolbar's Q/W/E/R keys are off (E is Global Edit there).
- MCP: `focus_window` pose_animation's state has `viewport` {posing, previewing, handles (Scene view
  px), active_joint, ghosts, floor, contacts, balanced, global_edit, view}; `pose.viewport` is an
  undocumented test hook driving the same SceneRender mouse calls as SceneWindow (send_input only
  reaches a running game).
