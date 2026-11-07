# The Scene view

How the Scene view picks, frames and lays out what you edit (docs/specs/S26-scene-view.md). Agents
drive the same behaviour through MCP: `jasper://docs/scene-view` (docs/mcp/guide/scene-view.md).

## Selecting

A click selects what is drawn under the pointer, triangle-exact: through a gap between tiles, a
window of a mesh or the walls of a room around the camera (their faces point away from it) you
reach what is behind. Icons (lights, cameras, audio sources, probes) are drawn over everything and
are picked first.

A camera shows a compact frustum gizmo: 2 units deep in perspective, as deep as half its view's
larger side in orthographic. Select it and the gizmo becomes its whole frustum, near to far, as in
Unity. A perspective camera always renders with 0 < Near < Far: a Near of 0 or less draws with
0.001 (the component keeps what was typed).

- The first click on a prefab instance or a model selects its root (the outermost one for nested
  prefabs). A GameObject below a collapsed Hierarchy row selects that row. In Prefab Mode the
  prefab being edited is never the root.
- Once the root (or something inside the instance) is selected, a click selects the GameObject
  under the pointer.
- Clicking the same spot again (within 4 px) selects the next object along the ray, wrapping
  around: root, its prefab roots, the object, then what is behind it. A pill shows its name and
  `n/m`.
- Ctrl+wheel (Cmd+wheel on macOS) steps through those objects without clicking; the next click
  there keeps the choice. The plain wheel still zooms.
- Shift+click adds to the selection, Ctrl+click (Cmd) toggles; when the exact object is already
  selected the modifier acts on it rather than on its root. A click on nothing clears the
  selection. Every click is one undo step.
- Exact Pick (toolbar) turns roots off: a click always selects the object under the pointer.
- A large mesh still preparing its picking data is waited for (busy cursor; after 100 ms the
  status bar says "Preparing picking for ..."): nothing is selected by its bounds.

## Toolbar

After the scene kind and Parent: the camera bookmarks (Home, the stored slots 1-9, `+`), Layout,
Persp/Iso, Layers, Measure, Rulers, Exact Pick, Align to Surface, the Scene Camera, post FX and
Gizmos. A narrow view gathers the middle items in a `...` menu. The shortcuts below are the
defaults: every one is a Scene View command of **Edit > Shortcuts...** (see
[keyboard shortcuts](keyboard-shortcuts.md)), and works while the view has focus or the pointer.

| Item | Does | Shortcut |
|---|---|---|
| Bookmarks | click: go there; Ctrl+click: store the current view; right-click: Rename, Overwrite, Delete (Reset for Home); hover: the 160x90 preview | Shift+1..9 recall, Ctrl+Shift+1..9 store |
| Layout | Single, Split (Persp \| Top), Quad (Top, Front, Side, Persp); 3D only | |
| Persp / Iso | perspective or orthographic, framing kept | the view gizmo's cube |
| Layers | per layer an eye (shown in the Scene view) and a lock (pickable); Everything, Nothing | |
| Measure | drag from A to B: the length (m in 3D), Shift adds the deltas; points snap to surfaces, to the hit triangle's corners with V held, to the grid with snap-to-grid; click a measure to select it, Delete removes it, Esc ends the tool | M |
| Rulers | top and left rulers in 2D, UI and orthographic axis views | |
| Exact Pick | no prefab or model roots | |
| Align to Surface | drops turn their up axis onto the surface | Alt while dragging |
| Scene Camera | Field of View, Dynamic Clipping, Near, Far, Camera Speed with its Min/Max, Shift multiplier, Camera Acceleration, Camera Easing, Reset | |

## Camera

- Right mouse + WASD/QE flies at Camera Speed (the wheel changes it while flying); Shift
  multiplies it; Camera Acceleration ramps it from 25 % to full over 0.6 s.
- Middle mouse orbits (Shift pans, Ctrl zooms), Alt+left orbits, the wheel zooms (in Iso it
  scales the view).
- F frames the selection, Alt+F (Home in 3D) frames everything but terrains and the sky.
- Numpad 1/3/7 (Ctrl: the opposite side) and the view gizmo's arrows look along an axis; an axis
  view is orthographic while it stays aligned and goes back to perspective once rotated (unless
  you chose Iso). The cube between the arrows toggles Persp/Iso.
- With Camera Easing on, framing, axis views and bookmarks move over 0.25 s.

## Layouts

Each pane has its own camera. The pane under the pointer takes the input and shows the gizmos;
every pane draws the selection outlines and its grid in the plane it looks at. Axis panes are
orthographic and pan and zoom without orbiting; their header menu changes the axis and the
shading (Shaded, Shaded + Shadows). Inactive panes redraw only when something changes, without
shadows unless their shading asks for them, and without post FX. Double-click a pane's header to
maximize it.

## Hiding and locking

- The Hierarchy's left gutter: an eye hides a GameObject in the Scene view, a hand locks it
  against picking. A click applies to the GameObject and its children, Alt+click to it alone.
- H hides the selection (shows it again when all of it is hidden), Alt+H shows everything, L
  toggles the pick lock. A banner "N hidden" shows everything again on click.
- Hidden is the Scene view's only: the Game view, Play and builds draw everything.

## Drops

A model or prefab dropped on the view lands on the surface under the pointer (a triangle, a
terrain or a UI rect on a pickable layer; else the ground plane; else 10 units ahead), its bottom
on the surface. With Align to Surface (or Alt held) its up axis turns onto the surface normal. A
drop onto a mesh still preparing its picking data is refused ("Preparing surface...").

## While the game plays

The Scene view keeps drawing from the editor camera, with its tools and picking, while the Game
view shows the game.

- When the game loads another scene (`SceneManager.loadScene`), the Scene view shows that scene
  from the editor camera; clicks select its GameObjects. The played scene's Hierarchy row says
  "Not running: the game loaded ...".
- A game that captures the cursor (mouse-look) gives it back on **Escape** in the Game view, as in
  Unity: the game still receives the Escape key, and the status bar says how to give the cursor
  back (click the Game view). The cursor is also the editor's whenever the Game view does not have
  the focus. **Release Game Cursor** (Search All, or a key you bind in Edit > Shortcuts...) does
  the same as Escape.
- A stopped scene shown beside the game can show its child scenes' contents (Hierarchy > Show
  Contents) while the game runs.

## Where it is kept

Everything above is yours: the project's `.jasperengine/user/workspace.yaml` per scene (camera
settings, layout and panes, bookmarks, measures, the Layers filter, hidden and locked GameObjects)
and `.jasperengine/user/bookmarks/<scene>/<slot>.png`. Exact Pick, Align to Surface and Rulers are
editor preferences (also in **Preferences > Scene View**). Scene files never change. Large meshes keep their picking data in
`.jasperengine/cache/meshbvh/` (rebuilt when stale; at most 512 MB).
