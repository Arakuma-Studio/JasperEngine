# The Scene view for agents: picking, camera, bookmarks, layouts, measures, filters

What a user does with the Scene view toolbar and clicks, through MCP (group `editor_ux`: enable
it with `tool_groups` to list these tools; they are callable either way). All of it is per user
(the project's `.jasperengine/user/workspace.yaml`), never in a scene file or a build.

## Picking what is under a point

`pick_at {scene, x, y}` (pixels of the full-size `screenshot view:"scene"` image) or `{u, v}`
(0..1) returns:

- `hits`: what the point hits, nearest first in 3D (2D and UI: the last drawn first), one per
  GameObject, icons (lights, cameras, audio, probes) before geometry. Each has `entity`, `path`,
  `scene_id` (a layer scene's own), `kind` (mesh, instance, terrain, ui, bounds, icon),
  `submesh`, `triangle`, `instance`, `point`, `normal`, `distance` (world units), `pending`.
- `candidates`: what clicks at that spot cycle through: the pick root (outermost prefab
  instance, model or collapsed Hierarchy row), the prefab roots inside it, the hit, then what
  is behind it.
- `would_select` and `action`: what a click there does now, given the selection.

`apply: "select"` selects as a first click would; `apply: "click"` is the whole click: the first
selects the root, the next at the same spot (within 4 px) the next candidate, wrapping; a click
elsewhere on an instance whose root is selected selects the GameObject under the point. `shift`
adds, `ctrl` toggles (the exact hit when it is selected). A miss clears. Each applied click is one
undo step. `exact: true` (the toolbar's Exact Pick) leaves the roots out. `pane` picks in another
pane of Split or Quad (it becomes the active one).

Picking is triangle-exact: a room around the camera does not take the click (its walls face
away), gaps between tiles and sprite cutouts by mesh do not hit. A large mesh whose tree is still
building is waited for, never picked by its bounds.

## Camera

`set_scene_camera` reads and sets the editor camera (instant for an agent; Camera Easing
animates the user's moves):

- `projection` perspective / orthographic (Persp / Iso: the framing is kept, ortho half height
  = distance x tan(fov/2)), `ortho_size`;
- `view` top / bottom / front / back / left / right: an axis view, orthographic while it stays
  aligned (rotating out of it returns to perspective unless Iso was chosen);
- `frame_all` (Alt+F: everything but terrains and infinite bounds);
- `camera_settings {fov, near, far, speed, acceleration, easing, dynamic_clipping}`, the Scene
  Camera popup's settings (any of them; the reply lists them all).

## Frame checks (screenshot findings)

Every `screenshot` of the Scene or Game view returns `findings` (at most 5, the most severe first,
each `{kind, severity, message, fix, entities?}`) and, when there is one, `next` with the first
fix. A finding is a warning, never an error: the image is still returned. When `next` is present,
make its fix (or tell the user why not) before judging the image. The kinds:

- `exposure_clipped`: 20 % or more of the image at luminance 0.98 or above; names the brightest
  light on what is in view and halves it.
- `exposure_dark`: mean luminance under 0.04 with 80 % of the pixels under 0.02 (not when nothing
  is drawn on a dark clear colour).
- `empty_frame`: no renderer's bounds meet the camera's frustum.
- `subject_out_of_frame`: the selection (with none, the visible objects that could fit the
  frame) more than half outside it; `subject_too_small`: the selection under 1 % of the image.
  The Scene view's fix is `frame_entity` (`set_scene_camera` for a subject too small for it); the
  Game view's moves and aims its main camera.
- `texture_missing`: a visible renderer's material references a missing asset
  (`list_missing_assets`, `retarget_asset_references`).
- `z_fighting`: two renderers with coplanar, same-facing faces overlapping within 1 mm.
- `checks_skipped` (info): the checks passed their 30 ms budget; the rest were not run.

`findings_ms` is their cost. The Scene view's Gizmos menu has **Frame Checks** (the display flag
`frame_checks` of `set_scene_camera`): the same findings listed at the view's corner for the user.

## While the game plays

After a game load replaced the played scene, the played scene's Scene view draws (and picks) the
scene the game loaded, from the editor camera.

## Bookmarks, layouts, measures, filters

- `edit_scene_view_bookmarks {action: list | store | recall | delete | rename, slot}`: slot 0 is
  Home, 1..9 numbered; a store saves a 160x90 preview (`preview_path`).
- `set_scene_view_layout {layout: single | split | quad}`: Split is Persp | Top, Quad Top, Front,
  Side, Persp. `pane_settings [{pane, axis, shading}]`, `active_pane`, `maximized_pane`,
  `capture: true` returns one image per pane. Inactive panes redraw only when something changes.
- `measure_scene_view {action: add, from, to}` (world) or `from_screen`/`to_screen` with `snap`
  surface / vertex / grid: returns `length` and `delta`; `list`, `delete {id}`, `clear`.
- `set_scene_view_filter`: `visible_layers` / `pickable_layers` masks (the Layers dropdown),
  `hide` / `show` / `lock` / `unlock` GameObjects (children too unless `children: false`),
  `show_all`. Hidden is the Scene view only: the Game view, play and builds draw them.

`jasper://scene-view/{scene}` returns all of it plus the BVH cache stats (built, pending, bytes) and `pending_click`, the status text of a user click still waiting for a mesh's BVH (null when none).

## MeshRaycast (runtime and editor)

`raycast_meshes {scene, origin, direction, max_distance, layer_mask, all, backfaces}` (group
`scripting`) is Lua's `MeshRaycast` (see `jasper://docs/lua-api-reference`, class MeshRaycast):

```lua
local function look(scene, origin, direction, ray, door)
    -- the closest hit; with a Ray its distance is the fraction of ray.direction
    local hit = MeshRaycast.raycast(scene, ray)
    -- origin, direction, maxDistance (then a layer mask and options): world units
    local far = MeshRaycast.raycast(scene, origin, direction, 50, LayerMask.getMask("Default"),
                                    {hitBackfaces = false, ignore = {door}, kinds = MeshRaycast.MESH})
    -- every hit, nearest first
    local all = MeshRaycast.raycastAll(scene, origin, direction, 50)
    -- one GameObject only
    local one = MeshRaycast.raycastEntity(scene, door, ray)
    -- build the trees ahead, so no later raycast builds one inline
    MeshRaycast.prepare(scene)
    return hit.hit and hit.distance, far.point, #all, one.triangle
end
```

`Physics.raycast` hits colliders and needs bodies; `MeshRaycast` hits what is drawn (triangles of
meshes, instances, skinned and morphed meshes as posed, terrains, UI rects; points, lines and
particles by their bounds). The default layer mask is every layer but Ignore Raycast.
`MeshRaycast.overlapSphere(scene, center, radius [, mask [, options]])` / `overlapBox(scene, center,
halfExtents [, rotation, ...])` (+ `NonAlloc`) list what is drawn by its bounds, no colliders
needed (frame snapshot unless `options.current`); `query_physics {dimension: "drawn"}` from MCP.
