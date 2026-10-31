# Native UI (`jasper://docs/ui`)

The component UI (Image, Text, Button, Panel, Scrollbar, Progressbar, InputField, Layout Group:
Unity's uGUI) built with `create_ui` or GameObject > UI. Layout rules: the `create_ui` and
`set_rect_layout` tool descriptions. The HTML/CSS UI is a separate system: `jasper://docs/html-ui`.

## Canvas Group

Unity's CanvasGroup (component key `canvasgroup`, "Canvas Group"): it affects the GameObject it
is on and every descendant.

| Field | Effect |
|---|---|
| `alpha` (0-1) | multiplied into every UI element's draw alpha; nested groups multiply. The elements' own colours are unchanged (`Image.color` reads back as set) |
| `interactable` | false: Buttons (no press, no click, the disabled look), Input Fields (no focus; a focused one loses it), Scrollbars (no drag) and Panels (no move, resize or bring-to-front) stop reacting. Plain pointer callbacks (`ui.onClick` of an Image) still fire, as Unity's EventTrigger |
| `blocksRaycasts` | false: pointers go through the elements below to whatever is behind |
| `ignoreParentGroups` | the groups above stop counting |
| `renderMode` | `PER_ELEMENT` (Unity) or `FLATTEN` (CSS opacity), below |

**The chain rule.** An element's chain is the enabled groups from the element itself up to the
root, stopping after (and including) the first one with `ignoreParentGroups`. Alpha is the
product over the chain, interactable and blocksRaycasts the AND. A disabled group (or one on an
inactive GameObject) does not count. Unlike Unity, a child with `ignoreParentGroups` under a
parent at alpha 0 still shows at its own alpha.

**Hiding a panel like Unity.** Alpha 0 still blocks clicks (Unity, CSS `opacity: 0`): set
`alpha = 0, interactable = false, blocksRaycasts = false` (the game's DelayedUI does it in
`onEnable`). Effective interactability: `rt:isInteractable()` (groups and the widget's own
`disabled`); `rt:getInheritedAlpha()`, `rt:getGroupsAllowInteraction()`,
`rt:getGroupsBlockRaycasts()` walk the chain on demand.

**Drag and drop (Garden inventory).** In `onDragStart` set `group.alpha = 0.6` and
`group.blocksRaycasts = false`: the dragged item keeps receiving `onDrag`/`onDragEnd` (a captured
press continues), and the element under the pointer at the release is the slot behind it.
Restore both on drop.

**Fades.** `CanvasGroup(self.scene, self.entity)` wraps the GameObject's group (adding one when
missing); `self.object:getCanvasGroup()` is nil without one.
```lua
local group = CanvasGroup(self.scene, self.entity)
group:fadeOut(0.25, {deactivate = true})              -- LeanTween.alphaCanvas(cg, 0, .25) + SetActive(false)
group:fadeTo(1, 0.3, {ease = EaseType.QUAD_OUT, unscaled = true, onComplete = function() print("shown") end})
group:stopFade(true)                                  -- jump to the end (completes it)
```
A fade is an AlphaTween on the group (`alphaTarget = CANVAS_GROUP`) from the current alpha; a new
fade stops the group's previous one. On completion the final alpha is written, then `onComplete`
runs (main thread), then the GameObject is deactivated when asked. `unscaled` keeps it running
at time scale 0 (pause menus). The tween goes with the group and with the scene. A plain
`AlphaTween` whose target has an enabled Canvas Group fades the group (`alphaTarget = AUTO`;
`COLOR` forces the colour alpha).

**Animation clips.** Property tracks `component: "canvasgroup"`, `property: "alpha"` (Unity
`m_Alpha`), and the bools `interactable`, `blocksRaycasts`, `ignoreParentGroups`, `enabled`
(available with the clip property channel, feature anim-properties).

**Render modes.** `PER_ELEMENT` multiplies each element's alpha at no extra draw cost: overlapping
children show through each other (darker overlaps). `FLATTEN` draws the subtree into a layer and
composites it once at the group's alpha, so overlaps look like the front element (CSS `opacity`,
Godot CanvasGroup). Its cost, only while its alpha is strictly between 0 and 1 and the subtree has
two or more UI elements: per camera that draws it, one offscreen pass before the camera's own (a
target the size of the screen, cleared; flattened siblings elsewhere on screen share one) and one
textured quad over the subtree's bounds; the colour pass is not broken. Keep at most two flattened
groups fading at once on mobile. Trade-offs: additive and multiply children blend
against the layer, not the background; `ignoreParentGroups` children inside a flattened group are
still composited at its alpha; UI particles and sprites in the subtree are faded per element and drawn
over the layer; a stencil mask above a flattened group does not clip inside it (put the mask inside
the group, or use a Panel's scissor); at alpha 0 the whole subtree is skipped (masks included); more
than 4 nested layers fall back to Per Element.

**What it does not affect.** HTML UI documents (use CSS `opacity` / `pointer-events`), sprites,
meshes and line renderers (as in Unity, only UI elements). UI particles (Points under a
RectTransform) are faded. A custom UI shader receives the factor in `uiParams.color`; one that
ignores it does not fade, but still hides at alpha 0.

**MCP.** `create_ui` nodes take `canvas_group: true | {alpha, interactable, blocks_raycasts,
ignore_parent_groups, render_mode}`; `add_component canvasgroup` + `set_properties` /
`set_runtime_property canvasgroup.alpha`; `get_entity` and `get_runtime_entity` of an element
under a group return `canvas_group_state {inherited_alpha, interactable, blocks_raycasts,
group}`. Check the look with `screenshot` pixels and clicks with `send_input` + `eval_lua`.

**Behaviour changes in scenes without Canvas Groups** (intended, Unity-like): an InputField whose own
`disabled` is set no longer takes the focus on a press (and typing does not reach it); a Button that
is pressed when it becomes disabled is released (`onRelease`, no click) instead of staying pressed;
new Buttons' `colorDisabled` defaults to Unity's grey (sRGB 200, 200, 200, 128): saved scenes keep
the colour they stored, buttons made from Lua or MCP get the grey (set `colorDisabled` for another
look).
