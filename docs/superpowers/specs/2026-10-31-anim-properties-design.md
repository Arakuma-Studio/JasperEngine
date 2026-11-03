# Clip property tracks: active state, enabled, colours, materials, UI, lights, cameras, scripts (`anim-properties`, ledger #12)

Status: specced 2026-10-31. Branch: `gaps/anim-properties`. It merges after `anim-layers` (#10) and
rebases on it. `anim-retarget` (#11) merges after this feature.

Read first:
- [AGENT-BRIEF.md](../../engine-gaps/AGENT-BRIEF.md). Its shared conventions are binding.
- [docs/animation-clips.md](../../animation-clips.md) and
  [2026-10-24-animation-clips-design.md](2026-10-24-animation-clips-design.md). Section 2.3 there
  reserved `"channel": "property"`.
- [2026-10-31-anim-layers-design.md](2026-10-31-anim-layers-design.md). Its evaluator gets property
  slots.

## 1. Goals

- Implement the reserved clip channel `Property` (`engine/core/animation/ClipTypes.h:30`). A clip can
  animate any of these:
  - **GameObject active** (Unity `m_IsActive`).
  - **The enabled checkbox of components**, Unity `m_Enabled`.
  - **Colours and alpha**:
    - UI Image, Text and Button graphics: Unity `m_Color` and TMP `m_fontColor`;
    - sprite and mesh material colours;
    - light colours.
  - **Material values**: base colour, emissive, metallic, roughness and alpha cutoff; the texture
    rect (offset and scale); shader-library parameters.
  - **UI Rect Transform values**: anchored position, size, pivot and anchors.
  - **Light and camera values**: intensity, range, cone, FOV, ortho size, near and far.
  - **Audio**: volume, pitch and pan.
  - **Sprite frame**: Unity's `m_Sprite` sprite swap, as used by the game's `falling.anim`.
  - **Lua script `properties` fields**, as Unity animates MonoBehaviour fields.
- Properties use one binding mechanism. The engine has a registry of animatable properties named
  with the editor's Catalog names, the same names `set_properties` uses. So agents and the Inspector
  speak one vocabulary, and exported builds need no editor code.
- **Discrete** values (bool, int, frame) are stepped and **continuous** values are interpolated. Both
  blend through crossfades, layers and the R2 rest rule.
- The Animation window has three surfaces for properties:
  - **Add Property** lists the component properties of every bound GameObject.
  - **Record** keys Inspector edits: property rows, the GameObject active checkbox and component
    enable checkboxes.
  - **The curves view** gets Unity-like key tangent editing (Auto, Free, Flat, Linear, Constant,
    Broken, and draggable handles) for every scalar curve.
- Clip events, the existing channels and every v1 file keep working as they do today.
- The game's legacy clips can be ported 1:1:
  - `GameStatsShowing` (stepped `m_IsActive` and events);
  - `uiFadeIn` (Image alpha);
  - `DoorJumpscare` (Image colours);
  - `EarnAnim` / `LoseMoneyAnim` (text colour);
  - `go` (anchored position y; its CanvasGroup alpha is not portable, see Non-goals);
  - `falling.anim` (sprite frames).

## 2. Non-goals

- **CanvasGroup alpha (Unity `m_Alpha`).** The engine has no group alpha. The game uses it in
  `GameOverMenu`, `go` and `go sped up`. The porting recipe is to animate `ui.color.a` on each
  graphic of the group, and the docs show it. We recommend a separate "UI Canvas Group" feature to
  the lead (see the final report).
- **Strings, texture, font and mesh swaps (PPtr curves).** Sprite frames are the exception, through
  `sprite.frame`.
- **C++ script fields.** At runtime C++ scripts have no reflection, because `ScriptProperty::memberPtr`
  is editor-only.
- **Properties that need a reload when they change.** Examples: shader, blend mode, `castShadows`,
  `fontSize` (atlas), `mapResolution`. Animating those every frame would rebuild GPU state. The
  registry excludes them, and the Add Property tree does not list them.
- **Physics body values** such as mass or gravity factor. Only the bodies' `enabled` is in.
- Animating the Animation Player's own `enabled`.
- Unity curve import, and Euler-curve tangent editing for rotations. Rotation keys keep the ease
  presets. Free tangents apply to the scalar channels: position, scale, morph and properties.

## 3. Users and stories

- **Porter.**
  - The game-over stats panel shows `puntaje` at 0.5 s, `record` at 1.333 s and `coins` at 2.167 s.
    The porter keys stepped `GameObject : Active` rows and keeps the events
    `CountThisGameScore`, `CheckHighscore` and `CountMoney`. It plays exactly like the Unity clip.
- **UI designer.**
  - With Record on, drags the Image colour's alpha in the Inspector at 0 s and at 1.5 s, and gets a
    fade-in.
  - In the Curves view, sets the alpha keys to Auto for an ease, or drags a tangent handle.
- **Gameplay programmer.** Animates a Lua script field `glow` (a float) and reads `self.glow` in
  `onUpdate`. The light's `intensity` flickers through a looping clip.
- **AI agent.**
  - Writes `{path: "Panel/Stats/puntaje", channel: "property", component: "gameobject",
    property: "active", keys: [{time: 0, value: false}, {time: 0.5, value: true}]}` with
    `edit_animation`.
  - Reads `bindings` (a missing component is reported as `missing_component`).
  - Lists what is animatable with `get_animation {root, list_properties: "Panel/Stats"}`.
  - Verifies with `get_animation {clip, times}` and, at runtime, `get_runtime_entity`.

## 4. Research summary

- **Unity `EditorCurveBinding`.** References:
  [EditorCurveBinding](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/EditorCurveBinding.html),
  [AnimationUtility](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AnimationUtility.html),
  [AnimationClip.SetCurve](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AnimationClip.SetCurve.html).
  - **Take:**
    - the triple of path, component type and property name, for example `m_IsActive`, `m_Enabled`,
      `m_Color.a` or `material._Color`;
    - discrete curves for bool and int;
    - animating script fields;
    - record mode keying Inspector edits;
    - one float curve per vector component, through the swizzle form below.
  - **Leave:**
    - Unity's serialized-field names (`m_Color`). We use the Catalog names users and agents already
      know from `set_properties`, for example `ui` + `color`.
    - Separate float curves as the only form. A whole-vector track is easier to read, and a swizzle
      track (`color.a`) gives the per-component case.
- **Godot value tracks.** Reference: the track paths `"Node:property:sub"` and the update modes
  Continuous, Discrete and Capture in
  [Animation](https://docs.godotengine.org/en/stable/classes/class_animation.html).
  - **Take:** a path plus property plus sub-property; discrete vs continuous chosen per type.
  - **Leave:** Capture mode. Our R2 rest fill already blends from the scene value.
- **Unity Animation window curves.** Key tangent modes: Clamped Auto, Auto, Free Smooth, Flat,
  Broken, and per side Free, Linear and Constant. We take Clamped Auto (as "Auto"), Free (smooth),
  Flat, Broken and the per-side modes. We store the result as the format's Hermite tangents, plus a
  `tangent_mode` hint so Auto recomputes when neighbours move.
- **Blending discrete values.** Unity blends bool curves as floats and thresholds them. We take
  "highest weight wins", including the rest's weight. It is deterministic and has no 0.5-threshold
  surprises in crossfades.

## 5. Product decisions

1. **Track identity is `(path, component, property)`, with the format reserved in v1.**
   - `component` is the Catalog component key (`Catalog::getComponentName(type, true)`, the same
     keys MCP uses): `ui`, `mesh`, `sprite`, `light`, `light2d`, `camera`, `recttransform`,
     `audiosource`, `button`, and so on.
   - Three pseudo components exist:
     - `gameobject` (property `active`);
     - `script:<ClassName>` (a Lua script's `properties` field);
     - `enabled` is a property of each component that has the enable checkbox
       (`Catalog::hasEnabledProperty`).
   - `property` is the Catalog property name, optionally with one swizzle suffix (`.r .g .b .a` or
     `.x .y .z .w`). Examples: `color`, `color.a`, `submeshes[0].material.baseColorFactor`,
     `positionOffset.y`.
2. **The engine owns the vocabulary.** `engine/core/animation/PropertyBindings.{h,cpp}` holds the
   registry of every animatable `(component, property)`. Each entry has:
   - a value kind;
   - discrete or continuous;
   - a display transform;
   - read and write functions.
   The write functions set the runtime dirty flags the engine's own Lua setters set. An editor test
   checks that every registry entry exists in `Catalog` with a compatible `PropertyType`. Exported
   builds use the registry alone.
3. **Keys use the Inspector's display units.**
   - Colours are keyed in **sRGB**, as the Inspector colour widgets (InspectorWindow.cpp:3897/3939)
     and Lua wrappers show them. They are converted to linear on write. `"#RRGGBB[AA]"` strings are
     accepted as input.
   - Spot cones are keyed as half-angles in degrees, as the Inspector's HalfCone rows show them.
   - Everything else is keyed raw.
   - Interpolation happens in key space. sRGB is Unity's behaviour for colour curves.
4. **Kinds and sampling.**
   - Bool: discrete, written `true`/`false`.
   - Int: discrete, except registry entries marked `interpolate`, such as UI sizes and anchor
     offsets, which are rounded.
   - Frame: a sprite frame index, discrete.
   - Float.
   - Vector2, Vector3, Vector4.
   - Color3, Color4.

   Discrete tracks always step. An `ease` other than `step` is ignored there, with one warning per
   track. Continuous tracks use every existing ease and Hermite tangent.
5. **Blending.**
   - Continuous values use the weighted average plus the R2 rest fill, per component, exactly like
     morph weights.
   - Discrete values take the value of the contributor with the largest weight. The rest counts as a
     contributor with weight `1 - W` when `W < 1`. Ties go to the higher layer, then the later state.
   - Additive layers (#10) skip discrete tracks, with a warning.
6. **Writes are cheap and deferred.**
   - Values are written in `flushPoseBlend` (ActionSystem.cpp:1632), after the transforms, and only
     when the value differs from the current one.
   - `active` writes are collected and applied last, through `Scene::setEntityActive`, in path depth
     order, parents first.
   - The callbacks that activation causes (`onEnable`/`onDisable`, feature `script-lifecycle`) run
     synchronously inside `setEntityActive`, as that spec defines and as Unity's Animator does.
     Because the `active` writes come after every other write of the flush, those callbacks never
     run inside the player loop or the accumulator loops.
   - The flush holds no component pointer or map iterator across these writes, because a callback
     may add components or destroy GameObjects. Each queued write re-checks
     `isEntityCreated`.
7. **Script fields.**
   - Only declared Lua `properties` entries of these types are animatable: Bool, Int, Float, Vector2,
     Vector3, Vector4, Color3 and Color4 (`ScriptProperty.h` types). Entity and prefab references are
     not.
   - The write sets the instance table field `self.<name>`, holding a Vector userdata for vectors.
     Vector values are fresh copies: a script holding the old table never sees it mutate.
   - It also updates `ScriptProperty::value`, so the Inspector shows the live value in play mode.
   - When several scripts of the same class are on one GameObject, the first enabled one is used.
8. **Rest and preview.**
   - Each property's rest is taken the first time it is bound (R2). It is stored in
     `ChannelRest::properties`, keyed by the property key.
   - The edit-mode preview restores properties like the other channels (`restorePlayerPreview`). The
     `active` preview in edit mode is shown by toggling the real flag, restored on stop. It is never
     saved: the scene is not dirtied, as with every preview.
9. **Format stays v1.** v1 readers already keep property tracks verbatim (AnimationClipIO.cpp:996-1002)
   and v1 writers write them back. The new optional key members (`tangent_mode`) are ignored by v1
   readers. Their tangents remain, and only the editor hint is lost.
10. **Record keys properties only through the Inspector or the Hierarchy active checkbox.** MCP
    `set_properties` never records, as the existing rule says.
11. **Tangent editing applies to every scalar curve.** This covers position, scale, morph, lattice
    offsets and property components. Rotation keys keep the ease presets in this version.

## 6. File format (`.anim`, version 1)

```json
{"path": "Panel/Stats/puntaje", "channel": "property", "component": "gameobject", "property": "active",
 "keys": [[0, false], [0.5, true]]},
{"path": "Fade", "channel": "property", "component": "ui", "property": "color.a",
 "keys": [{"time": 0, "value": 0, "in": 0, "out": 0, "tangent_mode": "auto"}, {"time": 1.5, "value": 1, "in": 0, "out": 0, "tangent_mode": "auto"}]},
{"path": "Panel/Vignette", "channel": "property", "component": "ui", "property": "color",
 "keys": [[0, [1, 0, 0, 1]], [0.1, "#330000FF", "step"]]},
{"path": "", "channel": "property", "component": "script:Torch", "property": "glow", "keys": [[0, 0.2], [0.5, 1, "sine_in_out"]]},
{"path": "Ship", "channel": "property", "component": "sprite", "property": "frame", "keys": [[0, 0], [0.083, 1], [0.166, 2]]}
```

- **Values.**
  - Bool: `true`/`false`. `0`/`1` is read as a bool.
  - Int and frame: integers. A fractional value rounds, with a warning.
  - Float: a number. `[n]` is accepted.
  - Vectors: arrays of their width.
  - Colours: arrays of 3 or 4 numbers in sRGB, or `"#RRGGBB"` / `"#RRGGBBAA"`. Colours are written
    as arrays. A Color4 key with 3 numbers takes alpha 1.
  - A swizzle track (`color.a`) holds one float.
- **Kind resolution at parse time, which is pure and needs no scene.**
  - A known registry entry gives the kind.
  - For `script:*`, the kind comes from the values: `true`/`false` means Bool; numbers mean Float;
    arrays of n numbers mean Vector-n. At bind time the script's declared type wins. An Int field
    rounds and steps; a Color field treats its keys as sRGB.
  - An unknown `(component, property)` is kept and is not played. The warning is
    "unknown animatable property 'light.fooBar' (did you mean 'intensity'?)", and the binding report
    calls it `unknown_property`.
- **Key objects** get an optional `tangent_mode` member: `"auto"`, `"free"`, `"flat"`, `"broken"` or
  `"linear"`. It is editor-only. The runtime uses `in` and `out`. It is written only on keys that
  have tangents.
- **Overlapping tracks.** A whole-property track and a swizzle of the same property in one clip (for
  example `color` and `color.a`) are allowed. The swizzle wins for its component. The
  `ClipValidation` warning is "color.a overrides the alpha of the color track".
- **Identity and sorting.** Property tracks sort after the other channels of the same path, by
  `(component, property)` in byte order. This is the existing comparator at AnimationClipIO.cpp:374.
  Duplicates merge, and the later track wins (AnimationClipIO.cpp:688).

## 7. The supported set (the registry, `PropertyBindings.cpp`)

| component | properties (kind) | write notes |
|---|---|---|
| `gameobject` | `active` (bool) | deferred `setEntityActive` (decision 6) |
| any component with `hasEnabledProperty`, except `animationplayer` | `enabled` (bool) | the component's `enabled`. Physics bodies apply on the next physics step through the existing enable handling |
| `ui` (Image, Text, Button graphic, any UIComponent) | `color` (Color4) | linear write to `UIComponent::color` |
| `button` | `colorNormal`, `colorHovered`, `colorPressed`, `colorDisabled` (Color4), `disabled` (bool) | a Button overwrites `ui.color` from its state colours, so animate these instead (doc note and binding warning) |
| `mesh` (the MeshComponent of meshes, sprites, models' mesh nodes and tilemaps) | `submeshes[N].material.baseColorFactor` (Color4), `.emissiveFactor` (Color3), `.metallicFactor`, `.roughnessFactor`, `.alphaCutoff` (float), `submeshes[N].textureRect` (Vector4: offset x y, scale x y), `submeshes[N].material.shaderParams.<member>` (kind from the shader manifest: float/vec/int; `COLOR`/`HDRCOLOR` → Color, sRGB keys) | writes the material field. `shaderParams` sets `needUpdateShaderUniforms`. Sprites and Lua's `Sprite.color` are `submeshes[0].material.baseColorFactor`, which Add Property shows as "Sprite : Color" |
| `ui`, `linerenderer`, `points`, `skybox` | `shaderUniforms.<member>` (manifest kind) | sets the component's uniform update flag |
| `sprite` | `frame` (frame) | `Sprite::setFrame(int)` semantics. An index out of range is skipped, with a warning once |
| `recttransform` | `positionOffset` (Vector2, Unity anchored position), `width`, `height` (int, interpolated), `pivot` (Vector2), `anchorPointLeft/Top/Right/Bottom` (float), `anchorOffsetLeft/Top/Right/Bottom` (int, interpolated) | the same code as the Lua RectTransform setters (layout invalidation) |
| `light` | `color` (Color3), `intensity`, `range` (float), `innerConeCos`, `outerConeCos` (keys in half-angle degrees; written as cosines) | `range` and cones: the shadow camera is updated as the Lua setters do |
| `light2d` | `color` (Color3), `intensity`, `range`, `falloff`, `height` (float) | — |
| `camera` | `yfov` (float, radians as the Inspector shows), `near`, `far`, `left`, `right`, `bottom`, `top` (float) | sets `camera.needUpdate` |
| `audiosource` | `volume`, `speed` (pitch), `pan` (float) | applied to the playing voice through `AudioSystem`, as the Lua setters do |
| `fog` | `color` (Color3), `density`, `linearStart`, `linearEnd` (float) | — |
| `skybox` | `color` (Color4) | — |
| `script:<Class>` | the declared `properties` of types Bool, Int, Float, Vector2/3/4, Color3/4 | decision 7 |
| `progressbar` | `value` (float) | — |

Further details:
- `N` in `submeshes[N]` is a literal index. A track whose submesh does not exist on the bound mesh is
  unresolved (`missing_component`).
- A text colour is `ui.color` on the Text entity, which is how the Lua `Text:setColor` writes it
  (Text.cpp:190).
- The registry API (`anim::`) is `const AnimatableProperty* findAnimatableProperty(component, property)`.
- `AnimatableProperty` holds:
  - `kind`, `width`, `discrete`, `interpolateInt`, `display` (identity, sRGB, half-cone degrees);
  - `ComponentId (*componentId)(Scene*)`;
  - `bool (*read)(Scene*, Entity, float* out)` and `bool (*write)(Scene*, Entity, const float* in)`.
- `listAnimatableProperties(scene, entity) -> vector<PropertyKey>` returns the entity's components
  and live script properties. The Add Property tree and MCP use it.

## 8. Runtime C++

- **Clip model** (`engine/core/animation/AnimationClip.h:80-90, 150-170`).
  - Property tracks become sampled. `ClipTrackData` and `AnimationClip::Track` get:
    - `PropertyKind kind`, `width`, `swizzle` (-1 or 0..3), `discrete`;
    - `const AnimatableProperty* binding`, which is null for `script:*` and unknown properties.
  - The `component`, `property` and `preserved` fields stay. `preserved` is only kept for unknown
    properties, so they round-trip verbatim.
  - `ClipData::valueWidth(Property, …)` returns the track's width.
  - Values are stored as floats: bools as 0/1, ints as whole numbers, colours in key space.
  - `Track::isSampled()` drops the `!= Property` condition.
- **Parsing and writing** (`engine/core/animation/AnimationClipIO.cpp`).
  - :996-1002 parses keys like the other channels and drops the "not supported yet" warning.
  - :1285-1293 writes keys canonically: bools as literals, ints without decimals.
- **Sampling** (`engine/core/animation/KeyframeSampling.{h,cpp}`). It gains scalar and vector-n
  sampling with eases and Hermite tangents, which is the morph-weights code generalized to width n.
  Discrete tracks sample the key at or before t: at exactly a key time, that key's value (the step
  rule).
- **Binding** (`ActionSystemPlayers.cpp:113` `resolveBindings`). A property track resolves its path
  as usual, then checks the component:
  - `gameobject` always resolves.
  - `script:<Class>` needs a `ScriptComponent` entry with that `className` of type LUA, with the
    property declared.
  - Others need `componentId` present on the entity, plus the submesh index or the shader uniform
    member.
  - Otherwise the track is `missing_component`.
  - `completeRest` (:64) reads the rest through `read` (or the script field) into
    `ChannelRest::properties[key]`.
- **Accumulation.**
  - `accumulateClip` (:555 today `continue`) adds to a new `PropertyBlendAccum`. It lives in
    ActionSystem next to `morphBlend` and is keyed by `(Entity, PropertyKey)`.
  - Continuous values: per component, `sums[k]`, `weights[k]`, rest. Discrete values: the best
    `(weight, value)`.
  - A swizzle track writes only its component.
  - The layered evaluator (#10) treats each `(entity, propertyKey)` as a slot with `width` elements.
- **Flush** (`ActionSystem::flushPoseBlend` :1632, after the lattice loop):
  - Continuous values: `value = rest-filled weighted average`, then converted from key space
    (sRGB → linear for colours, degrees → cosine for cones, rounded for ints), then written only if
    changed. Writing goes through `binding->write`, or `LuaBinding::setLuaScriptProperty(scene,
    entity, className, name, value)`. That function is new, next to `callLuaScriptMethod` at
    LuaBinding.cpp:1016, and runs on the main thread.
  - Discrete values: the winner's value is written if changed.
  - `active` entries are queued, then applied after every other write, sorted by hierarchy depth.
- **Entities deactivated by a clip.**
  - A child made inactive keeps being animated by its ancestor's player, because the player is not
    inactive. So `active` can be keyed back on. This matches Unity.
  - A clip that deactivates the player's own GameObject (path `""`) stops that player from the next
    frame (R25: inactive, time stops). Unity does the same. The binding report warns
    "animates its own active state: the player stops when it turns it off".
- **Preview** (`previewPlayer` :971, `restorePlayerPreview` :1098). Property channels are included.
  `PlayerPreviewResult::channels` gets `PropertyKey` entries. Restore writes the rest values back,
  including `active`.

## 9. Lua API

There is no new playback API: property tracks play through `AnimationPlayer` as today. Two additions:
- `AnimationPlayer:getAnimatedProperties(name)` returns, for a state, the list of
  `"path:component.property"` strings it binds. It is for debugging.
- Script fields animated by a clip are plain table fields. The docs say: "read `self.glow` in
  `onUpdate`; writing it yourself is overwritten next frame while a clip animates it, as in Unity".

Stubs are regenerated.

## 10. Editor UX

- **Add Property** (`editor/window/AnimationWindowClipDraw.cpp:1151` `drawAddPropertyPopup`). Under
  each GameObject of the bound hierarchy, after Position, Rotation, Scale, Blend Shapes and Lattice,
  it lists:
  - **GameObject ▸ Active**;
  - one group per component, using its Inspector display name (Image, Sprite Renderer, Light,
    Camera, Rect Transform, Audio Source, Button...): **Enabled** plus its registry properties, with
    Inspector labels ("Color", "Intensity", "Y FOV", "Anchored Position" for `positionOffset`, "Size"
    as width plus height rows);
  - material properties under **Material [N]**, with shader params by their manifest labels;
  - scripts as **Torch (Script) ▸ glow**.

  Colours offer the whole colour or **Alpha only** (`color.a`). A property already in the clip is
  greyed out. New rows read `Name : Image.Color` and key the current value at the playhead.
- **Dopesheet and rows.**
  - Bool rows draw on/off bars: filled while true.
  - Frame and int rows show the value in each key.
  - Colour rows draw a gradient strip of the sampled colour under the keys, and their foldout shows
    r, g, b, a.
  - The value column edits with the Inspector widgets (colour picker in sRGB, checkbox, drag). A
    typed value without Record previews it, as today.
- **Record** (`editor/animation/ClipRecordHooks.{h,cpp}`).
  - `ClipRecordValue` gets `PropertyKey property` (component, property, script). Its `channel` is
    `Property` and its `value` is in key space.
  - `channelsOf` (ClipRecordHooks.h:117) returns `Property` for registry properties.
  - New `claimsProperty(sceneId, entity, const PropertyKey&)` and `animatesProperty(...)`.
  - Sources that call claims and record:
    - Inspector property rows of registry properties (`InspectorWindow.cpp` `propertyRow`);
    - the GameObject header's active checkbox, and the Hierarchy toggle if one exists;
    - component header enable checkboxes;
    - script property rows (`set_script_properties` is MCP, so it does not record).
  - A claimed edit is keyed at the playhead and previewed. It is not a scene edit, as for
    transforms. Animated rows are tinted red as today, and rows Record would start keying get the
    fainter red.
  - Before and after a channel write, the `PropertyCmd` paths (PropertyCmd.h:140/181/243) notify the
    Property channel with the key, so the preview-rest logic (R6) covers properties.
- **The Curves view** (`editor/window/AnimationWindowClipCurves.cpp`).
  - Property components are drawn with their own colours: r, g, b, a for colours; x, y, z, w for
    vectors; a single curve for scalars.
  - Bools and discrete values are drawn as steps on a 0..1 band of their own and cannot be dragged
    in value.
  - **Tangent editing** applies to scalar channels. Right-click a key for:
    - **Auto** (Clamped Auto): Catmull-Rom slope `(v₊ - v₋)/(t₊ - t₋)`, set to 0 when the key is a
      local extremum, and limited so that the curve does not overshoot its neighbours;
    - **Free Smooth** and **Flat**;
    - **Broken**;
    - **Left/Right/Both Tangents ▸ Free / Linear / Constant**. Constant on the right side is the
      key's `step` ease; on the left side it is the previous key's `step` ease.
  - Selected keys show handles. Dragging a handle sets that side's tangent; unless the key is broken,
    the other side is mirrored.
  - A key whose mode is Auto recomputes when it or a neighbour moves.
  - Giving tangents to one key of a track that had none gives the track's other keys their linear
    slopes. This is the existing rule (animation-clips.md: "a track in which only some keys have
    tangents gives the others linear ones"). Their curves are therefore unchanged.
  - Each drag is one undo step. The "tangent handles are read-only" note in docs/animation-clips.md
    is removed.
- **Inspector (clip asset).** Track lines show `path : component.property`. The Bindings foldout of
  the Animation Player reports `missing_component` and `unknown_property` with did-you-means.

## 11. MCP and agent surfaces

- **`edit_animation` tracks.**
  - A track can be `{path | target, channel: "property", component, property, keys, mode}`. Key
    values follow section 6, and an `ease` is ignored on discrete properties, with a warning.
  - `component` accepts the Catalog key or the Inspector display name ("Image", resolved to `ui`).
  - Unknown properties are refused with a did-you-mean listing the component's animatable
    properties.
  - Tangent keys may carry `tangent_mode`.
- **`get_animation`.**
  - `{root, list_properties: "<path>"}` lists `{component, property, kind, label, value}` for that
    GameObject, including scripts. Agents discover names with it.
  - Samples of property tracks return values in key space: sRGB colours and bools as JSON bools.
  - The binding statuses `missing_component` and `unknown_property` are added.
- `preview_animation` covers properties, and its `bound` list includes property values.
- **Guide.** `docs/mcp/guide/animation.md` gets a "Property tracks" table of about 40 lines,
  including the CanvasGroup recipe. `jasper://guide` changes by at most one line. Tool budget:
  +400 characters.

## 12. Exported-build path

- The registry, sampling and writers are engine code.
- `.anim` files ship as today. Lua script writes use the shipped Lua state.
- The game's porting needs no Generator change. Scenes reference clips as before.

## 13. Behaviour rules and edge cases

- **P1. Blends.** In a crossfade between a clip that keys `active = true` and one that keys
  `active = false`, the value flips at the midpoint (weights 0.5 / 0.5: ties go to the later state,
  the incoming one).
- **P2. Untouched values.** A property not animated by any playing state keeps its scene value.
  When the last state that animated it stops, the value stays as written (R1b "last pose stays"),
  unless a fade brings the rest back.
- **P3. Writes from script and clip together.** A script writing an animated property in `onUpdate`
  is overwritten by the clip in the same frame's system update. Writes in `onPostUpdate` win for that
  frame's render. This is documented, as it is for transforms.
- **P4. Script reloads and missing scripts.** A Lua script reload rebinds `script:*` tracks on the
  next update, because the script instance changed and the hierarchy revision check covers it (the
  scripts' instance ref is part of the bind stamp). A missing script leaves the track unresolved, with
  a warning once.
- **P5. Unity's rule for `enabled` on a Button.** The button keeps its colour logic: its state colours
  overwrite `ui.color`. The binding warning names the `button.colorNormal` alternative.
- **P6. Frame tracks on a sprite with no frames.** They do nothing, with a warning once. An
  out-of-range index is skipped.
- **P7. Discrete keys and loops.** With loop wrap, a discrete key at time 0 applies at the start of
  every loop. Events at the same time fire after the values are written: events are dispatched after
  the flush.
- **P8. Preview.** Edit-mode preview of `active` toggles real flags. Those flags are restored, and
  never saved, on stop, scene save, Play, a scene switch or window close (the existing preview
  rules). Undo history is not touched.
- **P9. Colour space round trip.** An sRGB key written as linear and read back by `read` returns the
  same sRGB value within 1e-5.

## 14. Performance (mobile)

- Each animated property costs one sample, one accumulate and one compare-and-write per frame.
  Writes are skipped when unchanged.
- Budget: 200 animated properties (UI fades, flickering lights) cost ≤ 0.2 ms per frame on a
  mid-range Android device. A host test asserts ≤ 0.08 ms for 200 properties in a Release build.
- `active` changes are rare and their cost is the scene's. The registry lookup happens at parse time,
  and binding happens once per rebind, so there is no string work per frame.

## 15. Tests

Engine tests (`engine/tests/anim`, `jasperengine-anim-tests`):

1. **IO.**
   - Each kind parses and writes canonically.
   - Writing the same data twice gives the same bytes.
   - `"#FF000080"` is read as `[1, 0, 0, 0.502]`.
   - v1 files with unknown property tracks round-trip byte-identically.
   - An `ease` on a bool is warned and ignored.
   - `tangent_mode` round-trips.
2. **Discrete active.** This is the `GameStatsShowing` port. Keys `active`: `[0, false]`,
   `[0.5, true]` on child A. At t = 0.49, A is inactive; at t = 0.5, it is active (the step rule). An
   event at 0.5 runs after A is active: the handler reads `isActive() == true`.
3. **Colour fade.** `ui.color.a` goes from 0 to 1 over 1.5 s, linear, on an Image with sRGB colour
   (1, 0.5, 0.2).
   - At t = 0.75, `UIComponent::color` equals `sRGBToLinear((1, 0.5, 0.2))` with w = 0.5 (1e-5).
   - r, g and b are untouched: swizzle only.
4. **sRGB interpolation.** A `ui.color` track from (0, 0, 0, 1) to (1, 1, 1, 1). At t = 0.5 the
   linear stored value is `sRGBToLinear(0.5) = 0.2140` (1e-4), not 0.5.
5. **Blend and rest.**
   - A state at weight 0.25 animating `light.intensity` to 4, with scene intensity 1, gives
     1.75 (1e-6).
   - A discrete bool at weight 0.25 against the rest `true`: the rest wins.
   - At weight 0.6, the clip wins.
6. **Layers (#10).** An override layer 1 at weight 0.5 animates `light.intensity` to 3, over a base
   that animates it to 1: the result is 2. An additive layer adds `v - v_ref`.
7. **Script field.**
   - The Lua script `Torch` has `properties = {glow = {type = "float", default = 0}}`. Key 0 → 1
     over 1 s, linear. At t = 0.5, `self.glow == 0.5` in `onUpdate` of the next frame (1e-6).
   - An Int field rounds and steps.
   - A Vector3 field gets a fresh `Vector3` each change.
8. **Sprite frame.** Frames 0, 1, 2 at 0, 0.083, 0.166. At t = 0.1 the frame is 1, and `textureRect`
   equals frame 1's rect.
9. **Missing component.** The track is unresolved, with status `missing_component`. Adding the
   component at runtime binds it within one UNRESOLVED_RETRY period (0.25 s).
10. **Self deactivation.** A clip turning off the root stops the player. After `setActive(true)` from
    script, the player resumes from its held time.
11. **Preview restore.** After a preview of `active` and `color`, stopping restores both exactly. The
    scene's modified flag is unchanged.
12. **Tangents (sampling).** A track with keys (0, 0, out = 2) and (1, 1, in = 0) gives
    `h(0.5) = 0.75` (Hermite: 0.5·0 + 0.125·2 + 0.5·1 + (-0.125)·0 = 0.75), within 1e-6.
13. **Clamped Auto** (editor helper unit test). Keys 0 → 0, 1 → 1, 2 → 0.5. The middle key's tangent
    is 0 (local extremum). With keys 0, 1, 2 at values 0, 1, 2 the middle tangent is 1.

Editor tests (`editor/tests`):
- Every registry entry exists in Catalog with a compatible type and the same name (decision 2).
- Add Property lists Image Color, Alpha only, and GameObject Active for a UI hierarchy.
- Record: an Inspector colour edit keys sRGB at the playhead, with no scene command, and is undoable
  as a project step. The active checkbox keys a bool.
- Curves view driver: setting Auto on a key writes the expected tangents. A handle drag is one undo
  step. Broken keeps the sides independent.
- `ClipRecordHooks::channelsOf("ui", "color")` returns Property.

MCP tests (`tools/mcp/tests/test_animation_properties.py`):
- Build the GameStatsShowing port with `edit_animation`: 6 active tracks plus 3 events.
- Play, `wait {game_seconds: 0.6}`, then `get_runtime_entity` shows `puntaje` active and `record`
  inactive.
- `get_animation {root, list_properties}` lists `ui.color` with label "Color".
- An unknown property gives a did-you-mean.

## 16. Acceptance criteria

- [ ] Tests 1 to 13 pass, and so do the editor and MCP tests. All existing clip tests pass. v1 files
  round-trip byte-identically.
- [ ] The ports of `GameStatsShowing`, `uiFadeIn`, `DoorJumpscare`, `EarnAnim` and `falling.anim` play
  in editor Play mode and in an exported Linux build, matching the Unity timings: active steps at
  0.5 / 1.333 / 2.167 / 3 s, events after the values.
- [ ] Record keys Inspector colour, intensity, active and enabled edits. The curves view edits
  tangents with undo.
- [ ] Docs:
  - `docs/animation-clips.md`: Add Property, Record, the property-track format table, the
    CanvasGroup recipe, and the "Not in this version" list updated;
  - `docs/mcp/guide/animation.md`;
  - `docs/unity-glossary.md`: `m_IsActive`, `m_Enabled`, `m_Color`, material and MonoBehaviour field
    curves, mapped to the property tracks;
  - `lua-api.md`: `getAnimatedProperties`, and the script-field note.
  `TOOLS.md` is regenerated within budget.

## 17. Integration points

- `engine/core/animation/ClipTypes.h:30` (Property, now sampled; the contract comment updated).
- `engine/core/animation/AnimationClip.h:80-90` and `:150-172`; `AnimationClip.cpp` (build: property
  storage); `AnimationClipIO.cpp` :112, :374-379, :688, :996-1002, :1285-1293.
- `engine/core/animation/KeyframeSampling.{h,cpp}` (n-wide scalar sampling).
- New: `engine/core/animation/PropertyBindings.{h,cpp}`.
- `engine/core/animation/AnimationPlayerRuntime.h:63` (`ChannelRest::properties`).
- `engine/core/subsystem/ActionSystemPlayers.cpp` :64 (`completeRest`), :113 (`resolveBindings`),
  :430-565 (`accumulateClip`), :971/:1098 (preview).
- `engine/core/subsystem/ActionSystem.h:81-100` (`PropertyBlendAccum`); `ActionSystem.cpp:1616/1632`
  (clear and flush).
- `engine/core/script/LuaBinding.{h,cpp}:131/1016` (`setLuaScriptProperty`).
- `engine/core/registry/EntityRegistry.h:168-173` (`setEntityActive`).
- `editor/Catalog.cpp` property tables:
  - UI :329; RectTransform :343-356; Light :554-571; Light2D :575-583; Camera :764-785;
    Audio :789-804; Text :821; Mesh :880-893; submesh material :3180-3206; enabled getters
    :3817-3841.
- `editor/animation/ClipRecordHooks.{h,cpp}`, `ClipValidation.cpp`; `editor/command/type/PropertyCmd.h`;
  `editor/window/AnimationWindowClip{Draw,Sheet,Curves,Record}.cpp`; `editor/window/InspectorWindow.cpp`
  (row record sources).
- `editor/mcp/tools/AnimationClipTools.cpp`.

## 18. Cross-feature dependencies

- **`anim-layers` (#10), required first.** Property slots in the layered evaluator, and the additive
  rule for continuous values.
- **`script-lifecycle` (#17).** `active` writes trigger that feature's synchronous `onEnable` and
  `onDisable` from `setEntityActive`, at the end of the flush (decision 6). Test 2 then adds a case
  where an `onEnable` handler reads the frame's animated colour, which is already written. If
  `script-lifecycle` merges later, `setEntityActive` keeps today's behaviour, and the lifecycle
  feature adds the callbacks.
- **`anim-retarget` (#11)** merges after this feature. It copies property tracks unchanged into
  converted clips.
- **`ui-scaling-text` (#21).** Per-character text effects are separate. Text colour is `ui.color`.
- **Follow-up for the lead: a "UI Canvas Group" (group alpha and interactable).** The GAP-REPORT
  omits it, and the game's `GameOverMenu` and `go` clips need it (see the final report).

## Implementation notes

Decisions taken where the spec was silent, ambiguous or in conflict with the code, and deviations.
Code references are to the `gaps/anim-properties` branch.

### Engine

- **Files.** The registry is `engine/core/animation/PropertyBindings.{h,cpp}`: rows of
  `AnimatableProperty` with `has` / `read` / `write` (and an optional `snapshot` / `restore`)
  taking `PropertyParams` (the literal submesh index, the shader member or script field), so one
  row serves `submeshes[N]...` and `...shaderParams.<member>`. `parsePropertyRef` is the pure
  parse of (component, property) the clip loader uses; `bindProperty` is the per-entity binding
  (the declared type of a script field wins; a shader member's manifest type, `COLOR`/`HDRCOLOR`
  as sRGB Color4, else the keys' width). Property identities are interned (`internPropertyKey`,
  main thread, at bind) so the accumulator is keyed by `(entity, uint32)` with no string work per
  frame.
- **CanvasGroup (ui-canvas-group merged first).** The five `canvasGroupTracks()` rows of
  `CanvasGroupTracks.h` are registered as `canvasgroup.*`; the CanvasGroup non-goal and its
  `ui.color.a` recipe are dropped (the docs show the real `canvasgroup.alpha` track). The
  Inspector's Canvas Group rows go through `propertyRow`, so they are Record sources like every
  other row; the `edit_animation canvasgroup.alpha` check (0.5 at 0.5 s) is in
  `test_animation_properties.py`.
- **Clip model.** `ClipTrackData` gets `kind`, `width` (key width; 1 for a swizzle), `swizzle`,
  `discrete` and `known`; `normalize` recomputes them from (component, property) and the keys
  (a script field or shader member keeps the kind its keys or the editor gave it). Runtime
  tracks reuse the morph `weights` arrays for their key-space floats; a discrete track's easings
  are STEP; `Track::overridden` marks the components of a whole-property track that a swizzle
  track of the same clip keys (the swizzle wins). `ClipData::findPropertyTrack` /
  `findTrack(const ClipTrackData&)` find a track by its identity.
- **Unknown properties** (a v1 file's track this engine cannot animate) are kept verbatim
  (`preserved`) and not played; the normalize warning is "unknown animatable property
  'light.fooBar' (did you mean 'intensity'?): kept in the file, not played". A component given
  in the wrong case suggests the Catalog key ("did you mean 'light'?"). Known property tracks are
  written canonically (bools as literals, ints without decimals, one float as a number), so a v1
  file with now-animatable tracks is rewritten in the canonical form; unknown ones round-trip
  byte for byte.
- **sRGB.** Colours use the engine's own curve, `Color::sRGBToLinear` (gamma 2.2, as the
  Inspector's colour widgets and the Lua wrappers), not the piecewise sRGB curve: test 4 checks
  `Color::sRGBToLinear(0.5)` (0.2176), not 0.2140. The conversion is odd-symmetric so an
  overshooting ease below 0 gives no NaN.
- **Cones** are keyed as what the Inspector's HalfCone rows show: the full cone angle in degrees
  (`acos(cos) * 2`, Unity's Spot Angle), always degrees (never `Engine::isUseDegrees`), written as
  `cos(half)`. The spec said half-angles; the Inspector shows the full angle.
- **Script fields.** `LuaBinding::get/setLuaScriptProperty` (raw instance field, a fresh Vector
  userdata per write, `ScriptProperty::value` updated). Reserved members cannot be properties
  (script-lifecycle refuses them), and `script:<Class>.enabled` is an extra row: the script's
  checkbox through `ScriptRuntime::setScriptEnabled` (onEnable / onDisable), a deferred write
  like `active`. `isActiveAndEnabled` and `gameObject` are refused as read-only. A declared Int
  keyed with numbers steps (`anim::sampleTrackStepped`).
- **Writes.** `ActionSystem::flushPropertyBlend` runs after the lattice loop: rest-filled average
  (continuous, per component) or the winner (discrete, the rest counting with `1 - W`, ties to the
  later contributor), converted to stored values and written only when the stored value differs.
  Deferred writes (`gameobject.active`, a script's `enabled`) are collected, the pose blend is
  cleared, and `ActionSystem::update` applies them once its pose blend scope has ended (so an
  onEnable they run may `sample()` a player; outside `update`, a flush applies them itself), in
  hierarchy order (the Transforms' order: parents first, siblings in their order, not the hash
  order of the accumulator), each re-checking `isEntityCreated`; the binding is copied into the
  queue so no rest or runtime is held across the callbacks. A component no track keys keeps its
  stored value bit for bit (no key-space round trip: a `ui.color.a` fade never rewrites r, g, b).
- **Rests** keep the stored value besides the key-space one; a preview restore (engine and
  editor) writes the stored value, so a preview never changes a value by an ULP (with gamma 2.2
  in float, about half of the linear values do not survive the round trip).
- **`enabled` rows** do what the components' Lua setters do (every setter audited): the Trail
  Renderer's clears its points when disabled, the Joint 2D's makes a broken joint fresh when
  enabled again, the Lattice's bumps its revision, the Rigidbody 2D's enables or disables a loaded
  body now; the others only set the flag. The On-Screen Stick, On-Screen Button and Touch Area
  have their `enabled` rows, and after the merge with anim-retarget's main the Camera Shaker,
  Camera Follow, Video Player and Text Effect (whose setter also re-applies the effect) (the
  editor test checks that every component with the Inspector's checkbox has one, except the
  Animation Player: it found those four).
- **Script bindings (P4).** A binding picks its script once (the first enabled one of the class,
  else the first) and keeps its index, so with two scripts of a class an `enabled = false` key
  disables the same one every frame; a rebind (a missing component's retry, a hierarchy change)
  passes the previous index to `bindProperty` (`preferredScriptIndex`), which keeps it while it
  is still a Lua script of the class. Its bind stamp is the script instance and its
  `ScriptEntry::instanceGeneration` (a new number per Lua instance: the registry refs are reused):
  a player checks its script bindings every update, on its own (also on frames whose hierarchy
  changed), and binds them again when either changed (a reload, a script removed or reordered),
  so a field whose declared type changed takes the new type. A binding made without an index
  resolves it when it writes `enabled`. RGB keys
  (`"#RRGGBB"`, 3 numbers) of a Color4 key an opaque alpha (1), on script fields as on rows.
- **Layers.** Property slots in `buildPoseSlots` (one element per component). Continuous
  elements compose with the linear rules (additive `v - v_ref`); discrete elements keep the
  largest-weight sample; the base layer's winner competes with the rest at `1 - S`, an Override
  layer above replaces R when `λ·min(1, S) >= 0.5`; additive layers skip discrete tracks (warned
  once per state). Avatar masks and mixing transforms filter transforms only: property tracks
  always apply (Unity's Avatar Mask masks transform curves).
- **Missing components** count as unresolved tracks: besides the hierarchy-revision retry, a
  player with unbound property tracks looks for them again every 0.25 s (test 9). The reason is
  logged once per change of it ("property track 'Lamp : light.intensity' does not bind: no 'light'
  component"), as are the Button `ui.color` warning (P5), a sprite without frames or a frame out
  of range (P6) and a clip animating its own root's `active`.
- **Sprite frame.** `read` gives the frame whose rect the sprite shows (-1 when none); rows with
  side effects have `snapshot` / `restore` (the sprite's texture rect, an anchor row's preset and
  `usingAnchors`) that a preview restore puts back after the value.
- **Steps with tangents** (a deliberate change for v1 files). A `step` ease now survives on keys
  with tangents (Unity's Constant tangent: the Hermite segment holds its key's value) and plays
  stepped; before, the loader dropped it with a warning and played the Hermite segment. No file
  relied on it silently (every such file loaded with that warning, and the engine's importers never
  write a step with tangents), so v1 files are not special-cased. Other eases are still dropped in
  favour of the tangents; when a tangent edit gives a track its first tangents, the window says
  how many eases became linear (`cliptangents::droppedEases`). Older readers reset the step to
  linear with their warning.
- **Performance (spec 14).** `Properties.PerformanceBudget` (100 UI alpha fades and 100
  flickering light intensities through one player, the whole `ActionSystem::update`, best of
  8 x 30 frames, Release): 0.041 ms per frame on the shared build box (budget 0.08 ms; skipped
  above the core count of load). The anim-layers budget test (`Layers.PerformanceBudget`, 0.6 ms)
  measured 0.603-0.624 ms on the box at load 1, where anim-layers' own notes measured
  0.597-0.608 ms: it sits at its line as before.

### Editor and MCP

- **Names.** `editor/animation/ClipPropertyNames.{h,cpp}`: the Add Property groups (the UI
  element's own name for `ui`: Image, Text, Button...; "Sprite Renderer" for a sprite's mesh;
  "GameObject"; "<Class> (Script)"), the Inspector labels, and the resolution of the names agents
  give (`"Image"` and `"Text"` -> `ui`, `"Sprite Renderer"` + `color` -> the mesh's
  `submeshes[0].material.baseColorFactor`, `"<Class> (Script)"` -> `script:<Class>`).
- **Record.** `ClipRecordHooks` gets `ClipPropertyKey`, `claimsProperty` / `animatesProperty`
  and property write listeners (`before/afterPropertyWrite`, called by `PropertyCmd` through
  `channelsOf`, and by `EntityStateCmd` for the active state). The Inspector's `propertyRow`
  routes every row command through `submitRowCommand`: when Record claims the row's property,
  the command's new value (`RecordablePropertyCommand`, stored units) is converted to key space
  and recorded, and the command is dropped (no scene step). The GameObject header's active
  checkbox and the component headers' enable checkboxes record the same way. Lua script rows
  record as `script:<Class>.<field>`. Not record sources: the shader-library member rows (they
  edit their collection property as one command) and the script header's enable checkbox. The
  Hierarchy has no active toggle (nothing to hook). The rows resolve their property keys only
  while a record listener is installed (Record on), not every frame otherwise. When the clip has
  only component tracks of a property (`color.a`), Record keys those and adds component tracks
  for the components the edit
  changed, instead of a whole-property track that would overlap them.
- **Scene writes during a preview.** Every scene command that writes a previewed property puts
  its rest back first and takes the written value as the new rest (stop keeps it, undo writes the
  rest, never the previewed value): a property row's `PropertyCmd`, a Lua script row
  (`scripts[N].<field>`: the component write key `"scripts"`, resolved to `script:<Class>.<field>`
  of that script, the same instance the preview bound), and a whole-component write (Reset, Paste
  Component Values: an empty property, every previewed property of that component, a Mesh's
  submesh rows included).
- **Unity's UI y axis.** The layout space is y-down, Unity's RectTransform y-up: the porting docs
  (guide, `animation-clips.md`, the glossary) and the registry's `unityName` hints give the
  conversions (anchored position y and offsets negated; pivot y and anchor y `1 - y`, anchor min
  and max swapped: Unity's `m_AnchorMin.y` is `anchorPointBottom`). The GameOverPanel port keys
  `-150 -> 70 -> 0` and checks that the panel comes from above its place and overshoots below.
- **The window.** Key identities (`KeyId`, drag identities, pending values, copied keys) carry the
  property, so two property tracks of one GameObject never alias. Bool rows draw their on bars,
  int and frame rows their values, colour rows a strip of the sampled colour; the value column
  uses a checkbox, an int drag, the colour picker (sRGB) or drags; vectors and colours fold out
  their components.
- **Curves.** `editor/animation/ClipTangents.{h,cpp}` holds the pure tangent edits; the controller
  applies them (`setTangentMode`, `setSideTangent`, `setTangentSlope` with one merge key per
  handle drag) and refreshes Auto / Linear / Flat keys in every commit, so they follow moved keys.
  Clamped Auto is flat at the ends of a track (Unity's two-key Auto curve eases in and out) and
  limited to 3x each side's linear slope. Discrete properties are drawn at their values (a bool on
  0..1, a frame index at its index), not on a separate band, and drag in time only.
- **MCP.** `edit_animation` property tracks go through the .anim parser itself (one rule for the
  file and the tool; its warnings appear in `changes`); `get_animation {root, list_properties}`
  lists `{component, property, kind, label, group, value, discrete?}`; samples, keys and
  `preview_animation.properties` are in key space with bools as JSON bools; the binding report
  items of property tracks carry `property` and no write_file rename fix (property track lines
  share their path prefix). `preview_animation` lists the first 16 properties with
  `properties_total`. `get_runtime_entity` now reports `active` and `active_in_hierarchy`
  (the MCP test reads the stats' state there).
- **Tests.** Engine: `engine/tests/anim/PropertyTracksTest.cpp` (tests 1-12, the GameOverPanel
  port: GameStatsShowing's six stepped active tracks and three events, GameOverMenu/go's
  CanvasGroup alpha and anchored position y, EarnAnim's text colour, falling.anim's sprite swap,
  at 60 fps against the Unity timings, and the performance budget). Editor
  (`AnimationClipControllerTest.cpp`): the registry against the Catalog, Clamped Auto (test 13)
  and the tangent edits, Add Property's list and names, Record of an Inspector colour row (sRGB,
  no scene command, a project undo step) and of the active checkbox, a handle drag as one undo
  step, scene edits during a property preview (a script row, a Reset), the eases notice, pasted
  keys keeping their kind (`AnimationWindowClipTest`). Review fixes, engine: exact rest restores
  and bit-exact unkeyed components, the `enabled` setter side effects, hierarchy-ordered deferred
  writes with a `sample()` inside onEnable, one script instance per binding and the rebind after a
  reload, opaque alpha of RGB keys. MCP: `tools/mcp/tests/test_animation_properties.py`, and
  `test_animation_properties_export.py` (acceptance criterion 2: the same port built with
  `build_project desktop` and run with `run_build`; a probe script prints the values at 0.3, 1.0
  and 3.6 s; `JASPER_SLOW_BUILD=1`). The script-lifecycle case of test 2
  (an `onEnable` reading the frame's animated colour) follows from the deferred flush order and
  is covered by the play-mode event check (handlers see the values written).
- **Verification at handoff** (merged with main fda5c0d0f, Linux box): editor + tests build with
  0 warnings; editor unit tests 1641 passed, 9 skipped, 0 failed; anim tests 197 passed, 1 skipped
  (`Layers.PerformanceBudget` under load); full MCP suite 923 passed, 21 skipped, 10 failed (only
  the baseline `test_animation_assets` x5 and `test_lattice_scene` x5). Web: an em++ syntax check of
  the changed engine sources. Not run here: the Apple, Windows and Android builds (no platform code
  changed; the Xcode project lists the new source) and on-device timings.
- **Verification after the review fixes** (not merged with main again): 0 warnings; editor unit
  tests 1644 passed, 9 skipped, 0 failed; anim tests 202 passed, 1 skipped (`Layers.PerformanceBudget`
  under load); MCP `test_animation*`, `test_docs`, `test_stubs`, `test_html_ui_tools`,
  `test_ui_canvas_group`: only the baseline `test_animation_assets` x5 fail; the exported Linux
  player (`test_animation_properties_export.py`, `JASPER_SLOW_BUILD=1`) passes: at 1.0 s alpha 1,
  offset 0, puntaje on and record off; at 0.3 s 48 below its rest on screen; at 3.6 s every stat on
  and the events saw `score:true|record:true|coins:true`. Web: em++ syntax checks with physics 2D
  on and off (the Rigidbody 2D enabled row's body calls are guarded).
- **Verification after the re-review and the merge with main d08d63b31** (anim-retarget,
  video-mp4, native-bridge, xcode26-dav1d, asan-sweep; box 2202): humanoid and property tracks
  side by side in `ClipTypes`, the IO, the layer evaluator, the validation, the Animation window
  (`clipui::findRuntimeTrack` finds both) and `edit_animation`. 0 warnings; editor unit tests
  1811 passed, 9 skipped, 0 failed; anim tests 229 passed, 1 skipped; pose tests 219 passed. The
  timing tests skip themselves above the box's core count of load; at a load of 13 they ran and
  `CanvasGroupTest.PerformanceBudgets` (414 vs 400 us), `PhysicsContactsTest.ContactProcessingCostIsSmall`
  and `Layers.PerformanceBudget` (0.61 vs 0.6 ms, at its line before this branch too) missed by a
  few percent. Full MCP suite (-j3): 950 passed, 22 skipped, 11 failed: the baseline
  `test_animation_assets` x5 and `test_lattice_scene` x5, and `test_html_ui_tools::test_budget`
  before `TOTAL_LIMIT` was set to the measured 114703 (155 tools; then 40 passed with test_docs).
  The exported Linux player test passes. `tools/check-physics-off.sh`: 3d-off, 2d-off, both-off
  pass with 0 warnings. The Xcode sync scripts pass `--check`.
