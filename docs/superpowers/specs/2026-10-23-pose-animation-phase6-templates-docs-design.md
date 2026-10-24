# Pose Animation, phase 6: templates, retargeting checks, documentation

Status: approved design, ready for implementation.
Branch: `pose-animation`. Builds on phases 1-5.

## 1. Goal

Starter animations that work on any humanoid out of the box, user templates shared through the
project, and the documentation that makes the whole feature discoverable for humans and agents.
Poses are stored as rig-independent joint angles (phase 1, 4.5), so a template or any
`.poseanim` plays on every humanoid rig without a conversion step ("retargeting" is free); this
phase proves it with tests on both fixture rigs.

## 2. Template format and locations

- A template is a `.poseanim` file (phase 2 format) with two extra top-level keys after `version`:
  `"template": {"name": "Walk", "description": "A relaxed walk cycle, 4 poses.", "category": "Locomotion"}`.
  `preview_model` is empty in templates.
- Built-in templates: `editor/resources/pose-templates/*.poseanim`, embedded in the editor binary
  the same way other editor resources are (check how `editor/resources` files are compiled in),
  read-only.
- User templates: `<project>/pose-templates/*.poseanim` (created on first save).
- `PoseTemplates` (`editor/pose/PoseTemplates.{h,cpp}`): list (built-in first, then user,
  alphabetical within category), load by name (user templates shadow built-ins with the same
  name), save the current animation as a user template (name, description, category),
  delete a user template (undoable file delete through the project history).
- (Implemented: `editor/pose/PoseTemplates.{h,cpp}`, the embedded files in
  `PoseTemplatesBuiltin.cpp` from the generated `pose_templates.h`. Names are case-insensitive; the
  list leaves shadowed built-ins out; save is a `WriteFilesCmd`, delete a new `RemoveFilesCmd`;
  `apply` Replace takes the template's settings and poses and keeps the preview model, Append adds
  the poses and keeps the settings.)

## 3. Built-in templates (authored for this engine, not copied from anywhere)

Authored as joint angles with the phase 2 tools (create, edit, render, check metrics) on the
fixture hero and verified on the fixture robot. Each must: be balanced or intentionally airborne
in every pose (metrics), have contacts where feet should be planted, show no limit hits, read
clearly in `render_pose_animation` front and side sheets, loop seamlessly where it loops, and use
an appropriate feel preset/humanize. Mirrored halves use `mirror_pose`.

| name | category | poses | loop | feel |
|---|---|---|---|---|
| Idle | Basics | 2 (inhale, exhale; tiny chest/shoulder/head changes) | yes | Soft, humanize on (life 1.0, moving holds 0.2) |
| Walk | Locomotion | 4 (contact R, passing R, contact L, passing L; in place) | yes | Snappy with smaller overshoot, legs weight 0 |
| Run | Locomotion | 4 (contact, flight; mirrored), flight poses lifted | yes | Snappy, short snaps |
| Sneak | Locomotion | 4, crouched, exaggerated reach | yes | Soft with stepRate 8 |
| Cartoon Walk | Locomotion | 8 (contact, down, passing, up; mirrored) | yes | Cartoon |
| Jump | Actions | 6 (stand, crouch, launch, apex (lifted), land, recover) | no | Rubbery |
| Wave | Gestures | 2 (arm up hand left, hand right) | yes | Cartoon, looseness 0.6 |
| Sit Down | Actions | 4 (stand, lower, sit (lifted to seat height), settle) | no | Heavy |
| Punch | Actions | 4 (guard, wind-up, extend, recover) | no | Snappy, anticipation 0.2 |
| Bow | Gestures | 3 (stand, bow, return) | no | Soft |

## 4. UI (phase 4 window, Templates tab)

- A grid of cards: thumbnail = the template's middle pose rendered on the bound character
  (phase 3 `PoseRender`), name, `"N poses · loop"` / `"N poses · once"`, the description as tooltip,
  a "user" badge for project templates; a category filter combo and a search field.
- Click: creates a new animation from the template (save dialog defaulting to
  `<model dir>/<template name>.poseanim`, preview model = the character's model), opens it, and
  switches to the Pose tab.
- Right-click: **Replace Poses in Current Animation** (one undo step; keeps the asset's preview
  model), **Append Poses**, and for user templates **Show in Project** and **Delete** (confirm dialog).
- **Save Current as Template...**: a small popup with name, description, category.
- (Implemented: `editor/window/PoseAnimationWindowTemplates.cpp`, registered with
  `PoseAnimationWindow::addTab`; the list comes from `PoseTemplates` and is re-read when
  `<project>/pose-templates/` changes; thumbnails share the strip's `PoseThumbnails` cache. The
  save dialog is an in-window modal whose default is numbered (" 1", " 2") when the file exists.
  Without a character the cards have no thumbnails and the hint "Bind a character to preview
  templates"; the no-character, no-animation and no-poses empty states show the grid too. Delete
  asks for confirmation. MCP: `focus_window` `pose.tab: "templates"`; the state lists the cards;
  the tab's actions are reachable for tests through the `pose.templates` hook
  (`JASPER_MCP_TEST_HOOKS=1`); filter, labels, default path and the replace/append edit are
  `poseui` functions with unit tests.)

## 5. MCP

- `edit_pose_animation`: `create: {template: name}` creates from a template (feel, humanize, poses);
  new op `apply_template` with `name` and `mode: replace | append`; new op `save_template` with
  `name`, `description`, `category` (writes a user template; one project undo step).
- `get_pose_animation`: with `templates: true` (and `path` omitted) returns the template list
  `[{name, category, description, poses, loop, user}]`.
- Stay within the tool-definition budget (trim descriptions if needed).
- (Implemented: `mode` defaults to `replace`; `save_template` saves the animation as the earlier
  ops of the call left it (joint limits applied to the poses they set), `category` defaults to
  "User", and the template file is an extra file of the document commit, so the call stays one undo
  step. Budget: get 873 + edit 2735 chars, total 97,367.)

## 6. Documentation

- `docs/pose-animation.md` (user guide): workflow (character -> new -> pose with handles -> add
  poses -> feel -> humanize -> preview -> bake or Pose Animator), the joint-angle model, shortcuts,
  templates, runtime Lua examples (`PoseAnimator` play, `goToPose`, crossfading with `weight`),
  troubleshooting (bone mapping, grounding, limits).
- `docs/mcp/guide/pose-animation.md`: final pass (DOF table with signs, limits, presets, ops,
  recommended agent loop: `get` -> `edit` (one call, several ops) -> `render` (`poses` front+side,
  then `motion`) -> read metrics -> fix; common mistakes: hand-mirroring instead of `mirror_pose`,
  setting vertical height instead of `grounded`/`lift`, forgetting the arms weight).
- `README.md`: a feature bullet under Editor and Engine; `docs/unity-glossary.md`: rows for Pose
  Animation, Pose Animator, Humanoid map (Unity: Avatar).
- Lua reference (`docs/lua-api.md` or wherever the reference lives) for `PoseAnimator`.

## 7. Tests

- `jasperengine-pose-tests`: every built-in template file loads without warnings; each realised
  on both fixture rigs has no limit hits; loops are seamless; mirrored halves are exact mirrors.
  (Ship a copy of the fixture skeletons as C++ test rigs, or load the fixture glTF skeletons with
  the engine's tinygltf in the test.)
- `tools/mcp/tests/test_pose_templates.py`: list templates; create from each built-in on the hero and
  on the robot; metrics: every pose of Walk has at least one contact, Idle is balanced, Jump's apex
  has no contact; render a sheet per template (non-flat); save_template then list shows it as user;
  apply_template replace/append and undo.
- Full MCP suite and pose tests pass; docs size limits (guide <= 16 KB) hold.
