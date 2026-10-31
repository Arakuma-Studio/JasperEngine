# UI Canvas Group: group alpha, interactable, blocks raycasts, ignore parent groups (`ui-canvas-group`, ledger #25)

Status: specced 2026-10-31. Branch: `gaps/ui-canvas-group`. Phase 1 (sections 7.1-7.9) is the feature; Phase 2
(7.10, the Flatten render mode) is on the same branch, after Phase 1 passes its tests.
Read first: [AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md) (its conventions are binding),
[anim-properties](2026-10-31-anim-properties-design.md) (registry model, sections 5-7),
[ui-multitouch](2026-10-31-ui-multitouch-design.md) (per-pointer raycast, D2-D6, 7.1, 7.4),
[ui-scaling-text](2026-10-31-ui-scaling-text-design.md) (UI particles 7.5, `docs/mcp/guide/ui.md`),
[script-lifecycle](2026-10-31-script-lifecycle-design.md), [coroutines](2026-10-31-coroutines-design.md) (unscaled time).

## 1. Goals

- A **Canvas Group** component (`CanvasGroupComponent`, Catalog key `canvasgroup`, display "Canvas Group") for the
  native UI with Unity semantics. It affects the GameObject it is on and all its descendants:
  - `alpha`: multiplied into every UI element's draw alpha. Nested groups multiply.
  - `interactable`: the built-in widgets below become non-interactable and show their disabled visual, like a
    Unity Selectable. This covers Button, InputField, Scrollbar, Panel move/resize and the on-screen controls.
  - `blocksRaycasts`: when false, pointers go through the elements below to whatever is behind them.
  - `ignoreParentGroups`: the groups above this one stop counting.
- **No per-frame tree walk when nothing changed.** Changes are found per group and propagated per subtree.
- **Rendering.** The default matches Unity: per-element alpha, so overlapping children show through each other.
  An opt-in `renderMode = FLATTEN` composites the subtree as one layer, as CSS `opacity` does.
- **Every surface.** Lua `CanvasGroup` with fade helpers on the existing tweens, C++ object class, `AlphaTween` on
  groups, property tracks (`canvasgroup.alpha` and the bools), serialization, prefabs, exported builds, the
  Inspector, Add Component, GameObject > UI > Canvas Group, MCP authoring and state, docs and tests.
- **Game ports work 1:1.**
  - `DelayedUI`: alpha, interactable and blocksRaycasts in `onEnable`.
  - `GamePowerups`: `LeanTween.alphaCanvas(cg, 0, 0.25).setOnComplete(SetActive(false))`.
  - Garden `GInventorySlot`: alpha 0.6 and `blocksRaycasts = false` while dragging, restored on drop.
  - `GInventoryNameDisplay` and `THurtFeedback`: scripted alpha lerps.
  - The legacy clips `GameOverMenu`, `go` and `go sped up`: `m_Alpha` curves on the root.
- Old scenes, prefabs and exports load and render unchanged (no group means every factor is 1).

## 2. Non-goals

- **HTML/CSS UI documents.** They have CSS `opacity` (it composites as a group, fork row P3-P4),
  `pointer-events: none` and `disabled`. A `UIDocumentComponent` under a Canvas Group is not affected, and the
  Inspector says so (7.7).
- **Non-UI renderers under a group.** Sprites, meshes and line renderers are not affected, as in Unity, where only
  CanvasRenderers are. UI particles (Points under a RectTransform, ui-scaling-text 7.5) are affected (7.3).
- **Unity's cull quirk.** In Unity a parent group at alpha 0 also hides an `ignoreParentGroups` child. We do not
  copy that: the child shows (decision 4).
- Keyboard and gamepad navigation between Selectables: the native UI has none today.
- A Unity scene importer: there is none in the engine.

## 3. Users and stories

- **Porting human.** "I add Canvas Group to my pause panel and drag Alpha. The panel and its buttons fade in the
  Scene and Game views. I untick Interactable and the buttons grey out and stop clicking, as in Unity."
- **Drag and drop (Garden).** "In `onBeginDrag` I set `group.alpha = 0.6` and `group.blocksRaycasts = false`. The
  drop raycast finds the slot under the finger, not the dragged item. My drag keeps receiving `onDrag`."
- **Script fade (GamePowerups).** `CanvasGroup(self.scene, self.entity):fadeOut(0.25, {deactivate = true})`, or
  `fadeTo(1, 0.25)` in `onEnable`.
- **Animator (GameOverMenu).** "The clip's root track `canvasgroup.alpha` goes from 0 to 1. I record it by dragging
  the Alpha slider with Record on."
- **Agent.** `create_ui` with `canvas_group: {alpha: 0}`, or `add_component canvasgroup` and `set_properties`. It
  checks with `get_runtime_entity` (each element's `canvas_group_state`) and with `screenshot` pixels. It clicks with
  `send_input` and reads the result in `eval_lua`.

## 4. Research summary

| Source | Takeaways | Not taken |
|---|---|---|
| Unity CanvasGroup API <https://docs.unity.com/en-us/engine/current/script-reference/unityengine/canvasgroup> and manual <https://docs.unity3d.com/Packages/com.unity.ugui@1.0/manual/class-CanvasGroup.html> | "The result alpha will be the multiplied result of any nested groups, multiplied with the canvas elements alpha". Not blocking means "graphic raycasting ignores anything in the group". Typical uses: fade a window, grey out controls, click-through. | The `ignoreParentGroups` caveat (a parent at alpha 0 still culls the child) |
| uGUI source <https://github.com/Unity-Technologies/uGUI> (`Selectable.ParentGroupAllowsInteraction`, `Graphic.Raycast`, `ColorBlock.defaultColorBlock`) | One chain rule for every property: walk from the element itself up through the enabled groups, and stop after the first one with `ignoreParentGroups`. Effective interactable = own `interactable` && groups allow. `onClick` checks `IsInteractable()` when the click happens. Alpha 0 still blocks raycasts unless blocksRaycasts is false (so DelayedUI sets both). disabledColor is (200,200,200,128). Non-Selectable handlers (EventTrigger) still get events in a non-interactable group. | Unity's quirk where a disabled group with `ignoreParentGroups` still stops the interactable walk |
| LeanTween `alphaCanvas` (game `Assets/LeanTween/Framework/LeanTween.cs:1180`) | Tween group alpha from its current value; `setOnComplete` | — |
| Godot `CanvasItem.modulate` / `self_modulate` <https://docs.godotengine.org/en/stable/classes/class_canvasitem.html> and `CanvasGroup` node <https://docs.godotengine.org/en/stable/classes/class_canvasgroup.html> | modulate multiplies down the tree per item (Unity-like). The CanvasGroup node draws its children as one object through a backbuffer, so overlaps do not stack, at a fill cost (`fit_margin`, `clear_margin`) | Mipmaps and custom group shaders |
| CSS `opacity` (engine fork rows P3-P4 and the `GroupOpacity` compat row in `docs/html-ui/FORK.md`) | Opacity below 1 composites the stacking context as a layer. A leaf (one draw, no children) skips the layer: its opacity goes into its vertices. Opacity 0 is still hit-testable. `pointer-events: none` can be undone by a child (`auto`) | — |

## 5. Engine today

- Each UI draw uploads `UIComponent::color` (linear, straight alpha) as `u_fs_uiParams.color`
  (`RenderSystem.cpp:10343`). `ui.frag` multiplies it by the texture or the font atlas and the vertex colour. Blend
  modes are set per element (`UIComponent::blendMode`, `SokolPipelineCache.cpp:112-158`).
- UI draws in Transform (Hierarchy) order (`RenderSystem.cpp:15458-15582`). Every component array is sorted by
  Transform, so a subtree is a contiguous range (`EntityRegistry::findBranchLastIndex`, `EntityRegistry.cpp:482`).
  The pass can already be ended and resumed mid-loop (`takeSceneColor`, `RenderSystem.cpp:15330`).
- The hit test picks the last `ImageComponent` in order that contains the point, is not `RectTransform::ignoreEvents`,
  is visible and passes `isUIActive`. It appears three times: down `UISystem.cpp:2890-2910`, up `:2981-2988`,
  move `:3260-3266`. ui-multitouch extracts it into `Entity raycast(Vector2)`.
- `ButtonComponent::disabled` and `InputFieldComponent::disabled` are each widget's own flag. `applyButtonVisual`
  (`UISystem.cpp:613`) picks the disabled texture and colour. Press, release and click check `!button->disabled`
  (`:2715`, `:2992`, `:3030`). `colorDisabled` defaults to white (`ButtonComponent.h:121`), which means no visual
  change.
- `AlphaTween` writes `ui.color.w` or the mesh base colour alpha (`ActionSystem.cpp:793, 2120-2133`). Tweens have
  no unscaled option.
- `Signature` is `std::bitset<64>` (`ecs/Signature.h:12`) and 61 component types are registered
  (`EntityRegistry.cpp:11-75`). See section 12.

## 6. Product decisions

1. **Unity's chain rule, the same for all three properties.** An entity's chain is the enabled
   `CanvasGroupComponent`s from the entity itself up to the root. It stops after (and includes) the first enabled
   group with `ignoreParentGroups`. The effective values over the chain are:
   - `alpha` = the product of the alphas;
   - `interactable` = AND of the `interactable` flags;
   - `blocksRaycasts` = AND of the `blocksRaycasts` flags.

   A disabled group, or a group on an inactive GameObject, does not count. The inactive case is automatic: its
   subtree is inactive too.
2. **Rendering matches Unity by default** (`renderMode = PER_ELEMENT`). The group alpha multiplies each element's
   uniform colour alpha at draw time, at no extra draw cost, and overlaps show through. `FLATTEN` (Phase 2) is the
   CSS and Godot look, for fades of stacked art. Docs show both with a picture (7.9).
3. **Interactable affects the built-in widgets only, as Unity Selectables.**
   - Button: no press and no click, the disabled visual, and `ui.onClick` is gated as for `disabled` today.
   - InputField: cannot focus, and loses focus.
   - Scrollbar: no drag.
   - Panel: no move, resize or bring-to-front.
   - On-screen stick, button and touch area: ignore new pointers and release the ones they hold.

   Legacy `UIComponent` pointer callbacks and the ui-multitouch script callbacks still fire, as EventTrigger
   does in Unity. Scripts can check `rt:isInteractable()`.
4. **`ignoreParentGroups` means what it says.** A child with it under a parent at alpha 0 is drawn at its own
   alpha. This deviates from Unity, and the deviation is documented. The draw skip (7.3) is per element, never per
   subtree, so nothing culls it.
5. **Alpha 0 still blocks raycasts** while `blocksRaycasts` is true (Unity, and CSS `opacity: 0`). The docs and the
   Inspector help text recommend the Unity "Hide" pattern: alpha 0, interactable false, blocksRaycasts false.
6. **Values.** `alpha` is clamped to [0, 1] on every write (Lua, Catalog, MCP, tracks, tweens). NaN is rejected:
   the value is kept and one warning is logged per entity. The group does not change the element's own colour:
   `Image.color` and `alpha` read back unchanged.
7. **Own flags stay own.** `Button.disabled` and `InputField.disabled` read and write only the widget's flag.
   Effective = own && groups allow, which is Unity's `m_Interactable && m_GroupsAllowInteraction`.
8. **Unity-like disabled colour for new Buttons.** The `ButtonComponent::colorDisabled` default becomes sRGB
   (200,200,200,128)/255, stored linear: (0.578, 0.578, 0.578, 0.502). A non-interactable group then greys buttons
   as in Unity. Saved scenes always write `colorDisabled` (`Stream.cpp:6065`), so they are unchanged.
9. **Fades use the existing tweens.**
   - `AlphaTween` gets `alphaTarget`:
     - `AUTO` (the default): the target's enabled Canvas Group if it has one, else today's colour alpha;
     - `COLOR`;
     - `CANVAS_GROUP`.
   - `Tween` gets `useUnscaledTime` (brief: time is explicit; default scaled).
   - Lua `fadeTo`, `fadeIn` and `fadeOut` are built on these.
   - `anime` stays the HTML UI animation library.
10. **Dirty detection compares cached values.** Writers need no `needUpdate` flag. A raw C++ write to the struct is
    seen too. The comparison costs O(number of groups) per check: no tree walk.
11. **The group itself stays visible in tools.** Scene view picking, selection outlines and gizmos ignore group
    alpha. Edit mode shows the alpha and the disabled visuals, as Unity's editor does.

## 7. Design

### 7.1 Runtime data (new `engine/core/component/CanvasGroupComponent.h`)

```cpp
enum class CanvasGroupRenderMode { PER_ELEMENT, FLATTEN };
struct JASPERENGINE_API CanvasGroupComponent {
    bool enabled = true;              // component checkbox (Catalog hasEnabledProperty)
    float alpha = 1.0f;               // [0,1]
    bool interactable = true;
    bool blocksRaycasts = true;
    bool ignoreParentGroups = false;
    CanvasGroupRenderMode renderMode = CanvasGroupRenderMode::PER_ELEMENT;
    // runtime, never serialized or cloned
    struct { bool valid = false; bool enabled, interactable, blocks, ignore; float alpha; } seen;
    float layerAlpha = 1.0f;          // FLATTEN: effective alpha relative to the enclosing flatten layer
    uint32_t subtreeDraws = 0;        // UI draws in the subtree (FLATTEN leaf rule)
};
```

- Runtime fields added to `UIComponent`, none of them serialized:
  - `float groupAlpha = 1` (relative to the nearest enclosing *layered* FLATTEN group, else absolute);
  - `bool groupInteractable = true`;
  - `bool groupBlocksRaycasts = true`;
  - `Entity groupEntity = NULL_ENTITY` (the nearest group in the chain, for the Inspector and MCP).
- `PointsComponent` gets `float uiGroupAlpha = 1`.
- Registration: `registerComponent<CanvasGroupComponent>()` (`EntityRegistry.cpp`, appended), an include in
  `JasperEngine.h`, and `EntityCloner` copyComponent with the runtime block reset.

### 7.2 Propagation (`UISystem`)

- New `UISystem::syncCanvasGroups()`, which is idempotent and O(1) when clean. It is called:
  - at the start of `update`;
  - in `draw()` (`UISystem.cpp:1847`, which runs before `RenderSystem`), so writes from `onPostUpdate` show in
    the same frame;
  - by every raycast and pointer entry point, so a script that changes `blocksRaycasts` in `onBeginDrag` affects
    that same frame's later events.
- **Structural dirty flag.** It is set when any of these happens:
  - `scene->hierarchyRevision()` differs from the cached value;
  - `onComponentAdded` or `onComponentRemoved` reports a `CanvasGroupComponent`, `UIComponent`, `PointsComponent`
    or `Transform`;
  - the scene is loaded.
- **Property dirty set.** Each check scans the CanvasGroup array and compares each group's
  `(enabled, alpha, interactable, blocksRaycasts, ignoreParentGroups, renderMode)` with `seen`. The groups that
  differ are dirty.
- **The full pass runs only when structurally dirty and at least one group exists.** It is one walk of the
  Transform array in order, with a scratch vector of `{alpha, layerAlpha, interactable, blocks, nearestGroup}`
  indexed by array position. Each entry is computed from its parent's entry and its own group by rule 1.
  - When the last group is removed, one pass resets every `UIComponent` and `PointsComponent` to the defaults.
  - With no groups at all, nothing runs.
- **The subtree pass runs on property changes.**
  - The inherited state comes from walking up from the dirty group's parent: O(depth), with groups looked up with
    `findComponent`.
  - Then one walk over `[index, findBranchLastIndex(group)]`.
  - Dirty groups inside another dirty group's range are covered by the outer walk.
  - Writes happen only when a value changes. A change of `uiGroupAlpha` sets the Points render update.
- **When `groupInteractable` flips to false:**
  - the visual updates at once (`button.needUpdateButton`);
  - the entity is queued, and the next `UISystem::update` processes the queue, so callbacks never run inside
    draw or raycast;
  - processing cancels the widget's presses (ui-multitouch D6; before multitouch: `pressed = false`, then
    `onRelease`, no click), drops InputField focus (`onLostFocus`, keyboard hidden), and releases scrollbar, panel
    and on-screen control gestures.
- **Stats for tests.** `UISystem::getCanvasGroupStats()` returns `{fullPasses, subtreePasses, visited}`, counted
  since the last reset.

### 7.3 Rendering

- `RenderSystem::drawUI` (`:10343`) uploads `uiDrawColor(ui.color, ui.groupAlpha, ui.blendMode)`. This is a pure
  function in `engine/core/render/UIDrawColor.h`, unit-tested:
  - ALPHA, ADDITIVE and AUTO: `a *= g`;
  - PREMULTIPLIED: `rgba *= g`;
  - MULTIPLY: `rgb = mix(1, rgb, g)` (exact without a texture);
  - NONE: unchanged.
- **Draw skip.** An element whose `color.a * groupAlpha <= 0` is not drawn. Elements whose stencil writes are active
  are the exception: they still draw, so masks keep working. The scissor setup still runs (`:15490`), so the
  children's clipping is unchanged.
- Custom and library UI shaders get the factor through `uiParams.color`. A shader that ignores it does not fade,
  but it still hides at 0. The shaders guide documents this.
- Points under a group: `updatePoints` (`:11395`) multiplies `renderPoint.color.w` by `points.uiGroupAlpha`.

### 7.4 Raycasts and widgets

- **Hit test.** `raycast` skips elements with `!ui.groupBlocksRaycasts`, and so does each of the three sites until
  ui-multitouch lands. Skipped elements are not hits, not panel hits, and not counted by `isPointerOverUI` or
  `Input.isPointerOverUI` (multitouch D15).
- **Captured gestures continue.** A pointer that already pressed or drags an element keeps delivering
  move/drag/up to it after its group stops blocking (Unity `pointerDrag`). The drop target is the raycast under the
  pointer, which now skips it.
- **Effective interactable.** `bool UISystem::isInteractable(Entity)` returns `ui.groupInteractable && !own
  disabled`. It replaces `!button->disabled` at `:2715` and `:3030`, `!inputfield.disabled` at `:2734` and
  `:3171`, and the guards of scrollbar and panel gestures in `pointerMoveOnUI`. `applyButtonVisual` (`:613`) uses
  it for the Disabled state.
- **Release always runs.** The `button->pressed` release at `:2992` drops its `!disabled` condition, so a button
  pressed before its group turned off cannot stay pressed. The click needs `isInteractable` at release time.
- On-screen controls (multitouch 7.4) call `isInteractable` on down.

### 7.5 Serialization, exports, Catalog

- **Scene and prefab YAML** use the key `canvasgroup`:
  `{enabled, alpha, interactable, blocksRaycasts, ignoreParentGroups, renderMode: PER_ELEMENT|FLATTEN}`.
  - Every field is written (the existing style: `Stream.cpp:6310`).
  - Missing keys decode to the defaults.
  - Encode and decode go next to LayoutGroup (`Stream.cpp:4705`, `:5092`), with `ProjectUtils.cpp` add/remove
    (`:1420`, `:1893`) and the object class (`:2361`).
- Prefab instance overrides work per property through the Catalog, like other components.
- **Exported builds.** `Factory::createCanvasGroupComponent` (`Factory.cpp:1578`, `:2976`) emits the fields.
  `Generator` needs nothing else. Every platform uses the same engine code. Flatten uses the existing sokol
  pass API, which the GL, GLES3, Metal, D3D11 and WebGL2 backends all have.
- **Catalog.**
  - `ComponentType::CanvasGroupComponent` is appended after `VertexAnimatorComponent`, and `LastComponentType`
    moves to it (`Catalog.h:135-153`).
  - The fast properties are `alpha` (Float, range 0..1), `interactable`, `blocksRaycasts` and
    `ignoreParentGroups` (Bool), and `renderMode` (Enum).
  - The enabled ref, name "canvasgroup", display "Canvas Group", icon `ICON_FA_LAYER_GROUP`, the component id and
    copy/paste (`Catalog.cpp:3747, 3834, 4127, 4194, 4339, 4468, 4685, 5594`).
  - `McpValues.cpp:1071`, and the `ComponentAddDialog` text "Fades, disables or makes click-through a UI subtree".

### 7.6 Lua and C++ API (`engine/core/object/ui/CanvasGroup.{h,cpp}`, bound in `GameObjectClassesLua.cpp`)

```
CanvasGroup (EntityHandle)
  CanvasGroup(scene, entity)   wraps; ADDS the component when missing (Unity GetComponent ?? AddComponent)
  CanvasGroup(scene)           new GameObject: RectTransform (stretch to parent) + CanvasGroup
  rw enabled, alpha (clamped), interactable, blocksRaycasts, ignoreParentGroups, renderMode (CanvasGroupRenderMode.*)
  fadeTo(alpha, duration [, opts]) -> AlphaTween      opts {ease = EaseType.*, unscaled = false,
  fadeIn(duration [, opts]) / fadeOut(duration [, opts])   deactivate = false, onComplete = function}
  stopFade([jumpToEnd = false]), isFading(), getGameObject(), getCanvasGroupComponent()
GameObject: getCanvasGroup() -> CanvasGroup|nil, hasCanvasGroup(), addCanvasGroup() -> CanvasGroup, removeCanvasGroup()
RectTransform: getInheritedAlpha() (Unity CanvasRenderer.GetInheritedAlpha), getGroupsAllowInteraction(),
  getGroupsBlockRaycasts(), isInteractable() (groups && own Button/InputField flag)
AlphaTween: rw alphaTarget (AlphaTweenTarget.AUTO | COLOR | CANVAS_GROUP)      Tween: rw useUnscaledTime
```

- The `RectTransform` getters walk up the chain on demand (O(depth)). They are exact even before the next sync.
- **Fade helpers.**
  - A helper creates an `AlphaTween` from the current alpha to the target, with `alphaTarget = CANVAS_GROUP`, the
    target set to the group, `ownedTarget = false` and `entityOwned = false`.
  - A new helper fade on a group stops the previous helper fade of that group (no fighting tweens).
  - The final value is written first, then `onComplete` runs on the main thread, then `deactivate` calls
    `setActive(false)`.
  - The tween entity is destroyed when it completes, on `stopFade`, when the group's entity is destroyed, and
    when the scene unloads (brief rule: cancelled with the owner).
  - `unscaled` sets `Tween.useUnscaledTime`. ActionSystem then advances it with the unscaled delta: the
    coroutines `Engine::unscaledDeltatime`, or before that feature the frame delta without `timeScale`.
- C++ scripts use the same class, with `setAlpha`/`getAlpha` and the other setters and getters.
- The stubs are regenerated, with `-- unresolved:` left empty.

### 7.7 Editor UX

- **Inspector "Canvas Group".**
  - Rows: Alpha (slider 0..1), Interactable, Blocks Raycasts, Ignore Parent Groups and Render Mode (Per Element |
    Flatten), each with a tooltip. Render Mode's tooltip has an overlap diagram explaining the difference.
  - Multi-edit and undo come from `PropertyCmd`.
  - Each row is a record source for property tracks (anim-properties: the `propertyRow` record hook).
  - An info line appears when the subtree holds a UI Document ("HTML documents ignore Canvas Groups: use CSS
    opacity / pointer-events").
- **Canvas Renderer (`UIComponent`) section.** When the element is under a group, one grey line shows, for
  example "Inherited alpha 0.25 · not interactable · click-through (Canvas Group: Panel)". A Select button jumps
  to `groupEntity`.
- **GameObject > UI > Canvas Group** (`HierarchyWindow.cpp:515`) creates a container stretched to its parent with a
  CanvasGroup. Its `EntityCreationType::CANVAS_GROUP` is in `CreateEntityCmd.cpp:315`. The Hierarchy shows the
  group icon (`:1025`).
- Add Component > UI > Canvas Group is allowed on any GameObject with a Transform.

### 7.8 MCP and agent surfaces

- `add_component`, `set_properties`, `set_runtime_property`, `copy_component` and `reset_component` work through
  the Catalog. Names: `canvasgroup` or "Canvas Group".
- `create_ui`: any node accepts `canvas_group: true | {alpha, interactable, blocks_raycasts, ignore_parent_groups,
  render_mode}`. This adds about 110 characters to its description, checked with `tool_budget.py`.
- `get_runtime_entity` and `inspect` of a RectTransform entity under a group add
  `canvas_group_state: {inherited_alpha, interactable, blocks_raycasts, group: "<path>"}`.
- `edit_animation` property tracks take `component: "canvasgroup"` (7.9 entries).
- `docs/mcp/guide/ui.md` (created by ui-scaling-text, or here if absent) gets a "Canvas Group" section of about
  40 lines: the rules, the Hide pattern, drag-and-drop, fades, the Flatten trade-off.
- `McpInstructions.cpp` gets one line: "UI fade/disable/click-through: Canvas Group (canvasgroup; guide ui.md)".
- `TOOLS.md` is regenerated.

### 7.9 Animation tracks and docs

- `PropertyBindings.cpp` (anim-properties section 7) gets these rows:
  - `canvasgroup.alpha`: Float, continuous, clamped on write, Unity `m_Alpha`;
  - `interactable`, `blocksRaycasts`, `ignoreParentGroups`: Bool, discrete, Unity `m_Interactable`,
    `m_BlocksRaycasts`, `m_IgnoreParentGroups`;
  - `enabled` through `hasEnabledProperty`.
- The writes are plain field writes, which the cached-value check (decision 10) picks up.
- The editor test "every registry entry exists in Catalog" covers these rows.
- **Docs.**
  - `lua-api.md`: a section "4.11.1 CanvasGroup", the RectTransform getters, the Tween and AlphaTween fields, and
    the enums `CanvasGroupRenderMode` and `AlphaTweenTarget`.
  - `unity-glossary.md` rows:
    - CanvasGroup (alpha, interactable, blocksRaycasts, ignoreParentGroups);
    - `LeanTween.alphaCanvas` → `fadeTo`;
    - `CanvasRenderer.GetInheritedAlpha` → `getInheritedAlpha`;
    - `Selectable.IsInteractable` → `isInteractable`;
    - `Graphic.raycastTarget` → `RectTransform.ignoreEvents` (inverted);
    - `ColorBlock.disabledColor` → `colorDisabled`;
    - the `m_Alpha` curve → `canvasgroup.alpha`.
  - `docs/animation-clips.md`: the "CanvasGroup recipe" is replaced by the real track.
  - The README UI feature list.
  - `html-ui.md`: a paragraph on native groups compared with CSS `opacity`. CSS composites as a group, and a child
    can undo `pointer-events: none`. A native child can re-enable blocking only with a group that has
    `ignoreParentGroups`.

### 7.10 Phase 2: Flatten render mode (`RenderSystem`)

- **When a layer is made.** A FLATTEN group gets a layer when its `layerAlpha` is in (0, 1) and its subtree has 2 or
  more UI draws (`subtreeDraws`, the CSS leaf rule). Otherwise:
  - at 1, the subtree draws directly and looks the same;
  - at 0, the subtree is skipped whole, stencil writers included (CSS `opacity: 0`).
- **Layer steps.** At the group's index `i` in the draw loop:
  1. `endRenderPass`;
  2. begin a pass on a pooled colour target (`Framebuffer`, the same format and size as the pass viewport,
     single-sample);
  3. clear the scissor rect to transparent. The scissor is the union of the subtree's `worldAABB`s clipped to the
     inherited scissor, rounded out to whole pixels;
  4. draw `i..findBranchLastIndex` normally. Elements use `groupAlpha`, which is relative to this layer;
  5. `resumeRenderPass`, viewport restored as in `takeSceneColor`;
  6. composite one quad with PREMULTIPLIED blending, colour `(g, g, g, g)` where `g = layerAlpha`, under the
     group's inherited scissor;
  7. the loop continues after the branch.
- A straight-alpha draw onto transparent black gives premultiplied content, which is why step 6 composites it as
  PREMULTIPLIED.
- Nested FLATTEN layers stack up to depth 4. A deeper group falls back to PER_ELEMENT, with one warning.
- Pooled targets are freed after 120 unused frames and on resize.
- **Documented trade-offs.**
  - Additive and multiply children blend against the layer, not the background (CSS isolation).
  - `ignoreParentGroups` children inside a layered group are still composited at the layer's alpha.
  - Each active layer costs two pass breaks plus the fill of its rect, and only while the alpha is between 0 and 1.

## 8. Behaviour rules and edge cases

- **R1.** The group on the element itself counts: a CanvasGroup with interactable false on a Button disables it.
- **R2.** A parent with interactable false and a child group with interactable true leaves the child
  non-interactable, unless the child has `ignoreParentGroups`.
- **R3.** Turning interactable back on restores the hover or normal visual from the current state. An own
  `disabled = true` stays disabled.
- **R4.** A press in progress when the group turns non-interactable gets `onRelease` and no click, on the next
  update (7.2). A press that lands on a non-interactable button does nothing, but it is still a hit (it blocks).
- **R5.** A UI element instantiated at runtime under a group has its state before its first draw and its first
  raycast: the lazy sync covers this.
- **R6.** Removing or disabling a group, reparenting an element, or `setActive` on a parent: the next sync
  recomputes. Effective values do not depend on active state, so reactivation needs no work.
- **R7.** Scene unload drops the queued interactability entities and the helper fades (7.6).
- **R8.** Time scale 0: a group's alpha never depends on time. Fades with `unscaled` keep running.
- **R9.** Writes in the same frame: an animation track and then `onPostUpdate` on the same field → the last write
  wins for that frame's draw (anim-properties P3).
- **R10.** A Text, Polygon or Progressbar fill child is faded like any UIComponent. A Button's label is a child, so
  the group fades it, but the label does not take the disabled tint (as in Unity).
- **R11.** `ignoreEvents` (per element) and `blocksRaycasts` (per subtree) combine with AND.
- **R12.** Multiple scenes: a group affects only its scene's hierarchy, and UI layer scenes are separate hierarchies.

## 9. Performance (mobile)

- Clean frame: one comparison per group, so ≤ 2 µs for 50 groups. The draw cost is one multiply per UI draw, with
  no extra draws or state changes.
- Alpha fade of a group with D descendants: O(D) per frame. 100 descendants ≤ 15 µs.
- Structural changes: one O(N) pass per change. N = 5000 transforms ≤ 0.4 ms (Release, host). With no group in
  the scene the cost is 0.
- Flatten: per active layer 2 pass breaks + the scissored fill. Guideline ≤ 2 simultaneous layers on mobile
  (in the docs).

## 10. Tests

**Unit tests** (`editor/tests/CanvasGroupTest.cpp`; the scenes are built in code, with `UISystem` and pointer
entry points):
1. **Propagation.**
   - A group at 0.5 with a child Image at alpha 0.8: `groupAlpha == 0.5` and `uiDrawColor().a == 0.4`.
   - A nested group at 0.5 gives 0.25 on the grandchild.
   - A disabled group counts as 1.
2. **`ignoreParentGroups`.** A child group at 0.8 under a parent at 0.5 gives 0.8. Under a parent at 0 it is still
   0.8, and it is drawn (the draw-skip predicate is false).
3. **Interactable.**
   - Parent false: a press on the Button sets no `pressed` and fires no `onPress` or `onClick`, and `ui.color ==
     colorDisabled`.
   - An Image's legacy `onClick` still fires.
   - An InputField does not focus. A focused field whose group turns false gets `onLostFocus` exactly once.
   - Scrollbar and Panel drags are ignored.
4. **Raycasts.**
   - Two overlapping Images, the front one under `blocksRaycasts = false`: a down hits the back one.
   - At true it hits the front one, and also at alpha 0.
   - An `ignoreParentGroups` child with `blocksRaycasts = true` under a parent at false is hit.
   - `isPointerOverUI` is false over an element that does not block.
5. **Drag continuity (Garden).** `blocksRaycasts` is set to false in `onDragStart`. `onDrag` keeps arriving, and
   the release hit (legacy `currentUIFromPointer`, or multitouch `onDrop`) is the slot behind.
6. **Press then disable.** `onRelease` fires once, `onClick` does not, and `pressed` ends false.
7. **Dirty propagation** (`getCanvasGroupStats`).
   - 10 frames with no change: visited 0.
   - An alpha change of a 3-descendant group: one subtree pass, visited 4.
   - A reparent: one full pass.
   - A Lua `alpha` write in `onPostUpdate`: the same frame's draw sees it.
8. **Blend modes.** The `uiDrawColor` table covers all modes. The stencil-writer exception applies at 0.
9. **Points.** A Points entity under a group at 0.5 gets render points with alpha halved.
10. **Serialization.**
    - Stream round trip is byte-stable.
    - A file missing keys gets the defaults.
    - An old scene with no group is identical after load and save.
    - `GeneratedScenesTest` covers the Factory code for a group.
11. **Tweens.**
    - AlphaTween AUTO on a group entity writes the group, and COLOR writes `ui.color`.
    - A new `fadeTo` stops the old one.
    - The order is final value, then `onComplete`, then deactivate.
    - An `unscaled` fade completes at timeScale 0.
    - `stopFade(true)` jumps to the end.
12. **Lua.** `CanvasGroup(scene, e)` adds the component. `getCanvasGroup()` is nil without one. The getters match
    the cached values after a sync. Alpha 1.5 is read back as 1, and NaN is rejected.
13. **Performance** (Release only, skipped in Debug): the budgets of section 9.
14. **Flatten** (Phase 2, GL under xvfb). Two overlapping opaque white squares in a group at 0.5 over black: the
    overlap pixel is 128 ± 3 with FLATTEN and 191 ± 3 with PER_ELEMENT. At alpha 1 no layer is made (render stats).

**MCP e2e** (`tools/mcp/tests/test_ui_canvas_group.py`, owned UI-scene project):
- `create_ui` builds a panel with `canvas_group: {alpha: 0.5}` holding a white Image and a Button. The `screenshot`
  pixel of the image over black reads ≈128. `set_properties` with alpha 0 gives the background.
- `get_runtime_entity` returns `canvas_group_state.inherited_alpha` 0.5.
- In play:
  - `set_runtime_property canvasgroup.blocksRaycasts false`, then a `send_input` click on the overlap point:
    `eval_lua` counters show that the back Image was clicked.
  - `interactable false`: a click on the Button counts 0, and the screenshot shows the disabled grey.
  - `eval_lua` runs `fadeOut(0.25, {deactivate = true})`. After `wait {game_seconds: 0.4}` the panel is inactive
    and its alpha is 0.
- `add_component "Canvas Group"` resolves, and `list_component_types` lists it.
- The Flatten overlap pixels match (Phase 2).
- An `edit_animation` track on `canvasgroup.alpha` reads 0.5 at t = 0.5 (enabled once anim-properties is on main).
- `test_stubs.py` and the tool budget pass.

## 11. Acceptance criteria

- [ ] Tests 1-13 and the MCP tests pass. Test 14 passes in Phase 2. All baseline tests still pass (LEDGER
  "Baseline"), the build has 0 warnings, and Android and Web compile.
- [ ] Ports of DelayedUI, GamePowerups, Garden drag and GameOverMenu (if anim-properties is merged) behave as in
  Unity in editor Play mode and in an exported Linux build.
- [ ] The Inspector, Add Component, GameObject > UI > Canvas Group, undo and Record all work. Old scenes load and
  render unchanged.
- [ ] The docs of 7.8 and 7.9 are written, `TOOLS.md` is regenerated within budget, and the stubs are clean.

## 12. Integration points

- New files: `engine/core/component/CanvasGroupComponent.h`, `engine/core/object/ui/CanvasGroup.{h,cpp}`,
  `engine/core/render/UIDrawColor.h`.
- Registration: `EntityRegistry.cpp:11-75` (register), `JasperEngine.h:98,170` (includes),
  `EntityCloner.cpp:545,704` (clone).
- Component structs:
  - `UIComponent.h:69` (runtime fields);
  - `ButtonComponent.h:121` (`colorDisabled` default);
  - `PointsComponent.h` (`uiGroupAlpha`);
  - `AlphaTweenComponent.h` (`alphaTarget`);
  - `TweenComponent.h` (`useUnscaledTime`).
- `engine/core/subsystem/UISystem.h:101-105,151-160` and `UISystem.cpp`:
  - `:613` (visual), `:2019` (update and sync);
  - `:2708-2740` (press and focus), `:2866-2953` (down hit);
  - `:2955-3069` (up: release and click), `:3138-3240` (gestures);
  - `:3242-3360` (move and hover, `isPointerOverUI`), `:3376` plus `onComponentAdded` (dirty);
  - `:1847` (draw sync).
- `engine/core/subsystem/RenderSystem.cpp`: `:10343` (uniform), `:11395` (points), `:15458-15582` (skip and
  Flatten), `:15330` (the pass resume pattern), `:12515` (isRenderActive is unchanged).
- `engine/core/subsystem/ActionSystem.cpp:793,2106-2133` (alpha target) and the tween dt (unscaled).
- Lua bindings: `engine/core/script/binding/GameObjectClassesLua.cpp:517,1543` (GameObject and RectTransform
  methods, the CanvasGroup class) and `ActionClassesLua.cpp:112` (AlphaTween and Tween fields, enums).
- Editor:
  - `editor/Catalog.{h,cpp}` (7.5 lines);
  - `editor/Stream.{h,cpp}:4705,5092,6310`, `editor/Factory.{h,cpp}:1578,2976`;
  - `editor/util/ProjectUtils.cpp:1420,1893,2361`;
  - `editor/window/InspectorWindow.cpp:16117` (draw dispatch) and the Canvas Renderer section;
  - `editor/window/HierarchyWindow.cpp:515,1025`, `editor/command/type/CreateEntityCmd.cpp:315`;
  - `editor/window/dialog/ComponentAddDialog.cpp:104`.
- MCP: `editor/mcp/McpValues.cpp:1071`, `editor/mcp/tools/WorldTools.cpp:3636-3790,4841` (`create_ui`), the
  runtime-entity tool (`canvas_group_state`), `editor/mcp/McpInstructions.cpp:184`.
- anim-properties' `engine/core/animation/PropertyBindings.cpp` (7.9 rows).

## 13. Cross-feature dependencies

- **anim-properties (#12).**
  - If it merges first, this branch adds the 7.9 rows. If this branch merges first, anim-properties adds them
    and drops its CanvasGroup non-goal and recipe.
  - Either way the GameOverMenu, `go` and `go sped up` ports must use `canvasgroup.alpha`.
- **ui-multitouch (#5).** Whichever merges second adapts the other:
  - the group filter goes into `raycast()`, and `isInteractable` into the per-pointer widget code and the
    on-screen controls;
  - interactability loss is a D6 cancel for every pointer pressing the widget.
- **ui-scaling-text (#21).** It owns `docs/mcp/guide/ui.md` and the UI particles that 7.3 fades. Text effects'
  per-glyph alpha multiplies with the group.
- **coroutines (#16).** It provides `Engine::unscaledDeltatime` for `Tween.useUnscaledTime`. A fallback is defined
  in 7.6.
- **script-lifecycle (#17).** It provides `onEnable`, which the DelayedUI port needs. That is not a build
  dependency.
- **Risk: component slots.** `Signature` is `std::bitset<64>`, with 61 types used. This feature, lines-trails
  (Trail Renderer), ui-multitouch (3 controls), ui-scaling-text (Text Effect) and camera-shake (2) together pass
  64.
  - The first branch that would pass 64 widens `Signature` to `std::bitset<128>` (`ecs/Signature.h:12`) and fixes
    `editor/physics/PlacementSession.cpp:391` (`to_ullong` throws for high bits) to hash the bitset.
  - The lead should schedule this once, before these branches merge.

## Implementation notes (2026-10-31, branch `gaps/ui-canvas-group`)

Decisions made while implementing, where the spec was silent, ambiguous or in conflict with the code:

- **Two alphas per element instead of one.** `UIComponent::groupAlpha` is the chain's product (what a Per
  Element draw uses, what the Inspector, `getInheritedAlpha` and MCP report) and the new `groupLayerAlpha`
  is the same relative to the nearest enclosing layered FLATTEN group (what the element draws with inside
  that layer). The spec had a single field "relative to the layer, else absolute"; with two, a frame where
  a layer cannot be made (its shader still compiling, an element loaded before the scene had a group and
  reloading with the layer's pipelines, more than 4 nesting levels) falls back to Per Element with the
  right alphas instead of drawing the subtree too opaque. `CanvasGroupComponent` also got `layered` (the
  layer decision of the last sync) and `warnedInvalidAlpha` (the one warning per entity for a NaN).
- **Flatten layers are drawn before the colour pass, not by ending and resuming it.** Each layered group's
  subtree is drawn into its pooled target (cleared transparent; the size of the colour pass target, so
  viewport and scissors are the pass's), innermost groups first so a nested layer is composited into its
  parent's; the colour pass then composites each layer at its group's place in Hierarchy order with one
  premultiplied quad (`g, g, g, g`) scissored to the subtree's projected bounds within the parent's
  scissor, and skips the UI it holds. The pixels are the spec's (the layer's content does not depend on
  what is behind it), but the colour pass is never broken, which also works for swapchain passes (sokol
  cannot resume an MSAA swapchain pass) and costs tilers less: one extra pass per layer and the fill of
  its bounds. The composite uses the HTML UI's passthrough program (`HTMLUI_POST` PASSTHROUGH: a
  premultiplied image times an opacity), so no new shader type; exports always include that variant in every
  build (it is a few KB, and a script can add a group or switch one to FLATTEN in any scene). Layer
  draws use `PIP_RTT`/`PIP_RTT_NODEPTH`: UI elements and UI particles load them as soon as the scene has a
  Canvas Group. The cost is therefore one offscreen pass per layered group per camera pass (plus its
  clear) and one quad, not "two pass breaks" (7.10 and 9 above are superseded by this). Targets are
  pooled by size (the Scene and Game views each keep theirs), groups no camera element draws make no
  layer (culling mask, hidden), and sibling layers whose bounds do not overlap share one target. When
  an outer layer cannot be made, the layers inside it are dropped too, so that branch draws Per
  Element with the right alphas.
- **What a layer holds.** UI elements (UIComponents) of the subtree. UI particles (Points under a
  RectTransform) and non-UI renderers in the subtree draw in the colour pass after the layer, faded per
  element (`uiGroupAlpha`), so `subtreeDraws` counts UIComponents only. A stencil mask above a flattened
  group does not clip inside its layer (the layer has its own stencil): put the mask inside the group or
  use the scissor (in the trade-offs of `ui.md`).
- **Draw skip.** An element is skipped at group alpha 0 in every blend mode (a custom shader that ignores
  the uniform still hides), and an alpha-blended one (AUTO, ALPHA, ADDITIVE) whose faded alpha is 0; never
  an element outside any group (`groupAlpha` 1), so old scenes, including elements at colour alpha 0 with
  depth or custom shaders, draw exactly as before. Stencil writers always draw.
- **Fades complete in the UISystem update.** The AlphaTween's `onStop` marks the fade finished at its end
  (or stopped early: no completion); the next `UISystem::update` (same frame, it runs after ActionSystem)
  destroys the tween, runs `onComplete`, then deactivates. `stopFade(true)` writes the end alpha and
  completes the fade at once (DOTween's `Complete()`; LeanTween's cancel(callOnComplete)).
- **Unscaled tweens before coroutines.** `ActionSystem::unscaledDeltatime(dt)` is `dt / timeScale` while
  the time scale is above 0 (exact for any step a caller makes) and, at time scale 0, the fixed frame delta
  when set, else the last measured frame time (`1 / framerate`, capped by `maxDeltatime`). **When coroutines
  merges**: replace the time scale 0 branch (or the whole function) with `Engine::getUnscaledDeltatime()`
  (marked `TODO(coroutines)` in ActionSystem.cpp).
- **Old tween files stay byte-identical**: `useUnscaledTime` and `alphaTarget` are written to scenes and
  generated code only when not the default. The Canvas Group itself writes every field.
- **Interactability loss** is processed in the next `UISystem::update` (`processInteractabilityLost`):
  a pressed Button gets `onRelease` (no click), a focused InputField `onLostFocus` (keyboard hidden),
  scrollbar and panel gestures end. Scrollbar and panel drags also stop at once in `pointerMoveOnUI`. The
  release of a pressed button no longer requires it to be interactable, and its click requires it at
  release (Unity's `OnPointerClick`). A non-interactable InputField does not take the focus on a press; a
  non-interactable Panel is not brought to the front. The cursor over a non-interactable field or panel is
  the arrow.
- **Hit test extracted** into `UISystem::raycast(Vector2)` (public, syncs first) / `raycastHit`, used by
  the down, up and move entry points; the down handler now clears the other elements' focus after the hit
  is known (before, in the same loop as the hit test).
- **Object class.** A container whose only distinctive component is its group gets the script class
  `CanvasGroup` (a handle, like AudioSource): `getGameObject()` reaches the GameObject.
- **create_ui** takes `canvas_group` on any node (unknown keys are errors); `create_entity` has a
  `canvas_group` type (the menu's container). `get_entity` (edit time) also returns `canvas_group_state`.
- **Catalog writes clamp at once** (`UpdateFlags_CanvasGroup`); UISystem's sync clamps any other raw write
  and rejects NaN (keeping the value it saw last).
- **Pixel test 14** has no GL infrastructure in the editor unit tests: the Flatten and Per Element overlap
  pixels are checked in the MCP e2e test (`test_flatten_overlap_pixels`, Game view under xvfb).
- A render mode change is structural (one full pass): it is what counts the UI elements of the
  subtrees (the leaf rule), which the full pass does only while some group is FLATTEN.
- Typing (char and key events) also requires the field to be interactable (it is dropped before the
  next update takes the focus away). UI particles whose group alpha changes after the update (in
  `onPostUpdate`) are rebuilt by `RenderSystem::draw`, so they fade the same frame.
- Intended behaviour changes in scenes without groups are listed in `ui.md` ("Behaviour changes").
- `docs/animation-clips.md` has no CanvasGroup recipe on main (anim-properties is not merged): nothing to
  replace yet.

What other features plug in (contracts implemented on this side):

- **anim-properties (#12)**: `engine/core/animation/CanvasGroupTracks.h` has the read/write functions of the
  `canvasgroup` rows in the registry's `bool (*)(Scene*, Entity, float*)` shape and a table
  (`canvasGroupTracks`: property, kind, Unity name). anim-properties adds the rows to `PropertyBindings.cpp`
  (`canvasgroup.alpha` Float continuous, `interactable`/`blocksRaycasts`/`ignoreParentGroups`/`enabled` Bool
  discrete), drops its CanvasGroup non-goal and recipe, uses the rows for the GameOverMenu/go ports, wires
  the Inspector's Record hook into the Canvas Group rows (`drawCanvasGroupComponent` uses `propertyRow`),
  and enables the MCP check of an `edit_animation` track on `canvasgroup.alpha` (spec 10, MCP).
  `CanvasGroupTest.TrackRowsMatchTheCatalogAndClamp` checks the rows against the Catalog.
- **ui-multitouch (#5)**: `UISystem::raycast(Vector2)` already applies the group filter (D15's
  `isPointerOverUI` follows); multitouch moves its per-pointer hit test onto it, calls
  `UISystem::isInteractable(entity)` in its per-pointer widget code and on-screen controls' down, and turns
  `processInteractabilityLost` into a D6 cancel of every pointer pressing the widget (it handles the single
  legacy pointer today).
- **coroutines (#16)**: the unscaled delta above.
- **ui-scaling-text (#21)**: owns `docs/mcp/guide/ui.md` (created here with the Canvas Group section);
  merge its sections around this one.
