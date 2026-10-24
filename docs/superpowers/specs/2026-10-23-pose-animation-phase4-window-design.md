# Pose Animation, phase 4: the Pose Animation window and the edit session

Status: approved design, ready for implementation.
Branch: `pose-animation`. Builds on phases 1-3. Phase 5 (viewport posing) is implemented in
parallel against the `PoseEditSession` contract in section 2: that section is binding for both.

## 1. Goal

A human-first window, in the style of the editor's Unity-like panels (Dear ImGui, `editor/Theme`),
that makes pose-to-pose animation fast: pick a character, duplicate-and-tweak poses, see time to
scale and drag it, pick a feel, preview exactly what bakes, bake in one click. Every action has a
keyboard shortcut and every edit is one undo step.

## 2. `PoseEditSession` (contract, `editor/pose/PoseEditSession.{h,cpp}`)

One session per window (the window owns it; `PoseEditSession::active()` returns the window's
session or null). It is the only code that writes the bound model's bone transforms in edit mode.

```cpp
class PoseEditSession {
public:
    enum class Mode { Idle, Posing, Previewing };
    static PoseEditSession* active();
    // binding
    bool bind(Scene* scene, uint32_t sceneId, Entity model, std::string* error);   // a scene Model entity with a valid humanoid
    void unbind();                                   // restores bones
    Entity model() const;  Scene* scene() const;  uint32_t sceneId() const;
    const std::shared_ptr<pose::PoseRig>& rig() const;
    bool open(const std::string& poseanimPath, std::string* error);   // phase 2 document
    const std::shared_ptr<PoseAnimationDocument>& document() const;
    // selection
    int primary() const;  const std::vector<int>& selection() const;
    void select(int index, bool additive, bool range);
    // modes
    Mode mode() const;
    void startPosing();                              // applies the primary key pose to the model
    void startPreview();  void stopPreview();  void setPreviewTime(float t);  float previewTime() const;
    bool previewPlaying() const;  void setPreviewPlaying(bool);
    // working pose (posing): phase 5 handles edit it
    const pose::HumanPose& working() const;
    void setWorking(const pose::HumanPose&);         // applies to the bones immediately (no undo)
    bool commitWorking(const std::string& undoName, const std::string& mergeKey);  // toBodyPose, limits, grounding, doc commit
    // ghost / onion settings read by phase 5
    struct OnionSettings { bool previous = true; bool next = false; float opacity = 0.5f; };  // 0.5: ruling of the phase 5 fix round (was 0.3)
    OnionSettings& onion();
    // global edit (phase 5 drives it; the window shows its bar)
    bool globalEditActive() const;  void beginGlobalEdit();  bool applyGlobalEdit(const std::vector<int>& targets /* empty = all others */);  void cancelGlobalEdit();
    const pose::HumanPose& globalEditBaseline() const;
    int addListener(std::function<void()>);          // any change: document, selection, mode, time (returns an id)
    void removeListener(int id);
};
```

Rules:
- Bones are saved (local TRS of every skeleton bone) on the first apply and restored on unbind,
  window close, entering play mode, scene close/switch, project close and before the scene is
  saved (then re-applied), so posing never dirties or corrupts the scene file (follow how
  `AnimationWindow` saves/restores preview state; reuse its helpers where possible).
- `commitWorking` keeps the key's name/hold/transition, converts with `toBodyPose`, clamps limits
  when `humanize.jointLimits`, re-grounds when the key is grounded (or stores `lift` from the
  lowest point when not), and commits as one undo step (`mergeKey` merges a whole drag).
- Undo/redo of a pose edit re-applies the primary pose to the bones (the document listener).
- Global Edit: `beginGlobalEdit` snapshots the primary pose as the baseline; `applyGlobalEdit`
  computes per-slot deltas in joint-angle space (`D_b = E_cur_b * E_base_b^-1` on the phase 1
  `E` rotations, slots whose change is > 0.01 degrees) and applies `E'_b = D_b * E_b` to every
  target pose (offset unchanged), clamps, re-grounds, one undo step ("Global edit"); selecting
  another pose, previewing or closing cancels (restores the baseline).

Additions (implementation; the members above are unchanged, these are extra):
- `struct ViewSettings { bool showBalance = true; bool ghostDuringPreview = false; }; ViewSettings& view();`:
  the Pose tab's "Show balance" and "Ghost during preview" toggles that phase 5 reads, beside `onion()`.
- `bound()`, `rigInfo()` (the phase 2 `PoseRigInfo`: map, bone entities, bridges), `modelFile()`,
  `realisation()` (`document()->realise(rig())`), `keyPose(i)` (realised key), `displayed()` (the pose the
  bones show), `previewLength()`, `previewFrame()`, `globalEditChangedSlots()` (0: "Nothing changed yet").
- `commitData(data, undoName, mergeKey, selection*, primary)` (the window's edits), `setSelection`, `goIdle()`
  (window close), `suspend()` (before a save / play: bones restored, applied again by `update(dt)` once the
  scene is stopped), `clearSceneState(sceneId)`, `update(dt)` (advances a playing preview, follows play
  mode, scene switch and deleted models), `closeDocument()`, `setActive()`.
- Global Edit computes `E` from a slot's three DOF values alone (4.5), so it needs no rig and also carries
  slots the bound rig does not map (`poseui::applyGlobalEditDelta`). Cancelling after an edit commits the
  baseline back as its own undo step ("Cancel global edit").
- Fix round 1 (all additive, source-compatible):
  - `int addListener(std::function<void()>)` now returns an id for `removeListener(id)`. A change made
    while the listeners run queues one more round, so it is not dropped. The destructor notifies no one.
  - `editable()`: false while the scene is saved (sync or async) or played, until `update()` reopens the
    bone writes. Bones are never written then, whatever starts in between (select, posing, preview,
    document edits). The window disables its edit controls; phase 5 must disable its handles too.
  - `ownsEntity(sceneId, entity)`: the bound model or one of its bones. The Inspector keeps bone transforms
    read-only while posing.
  - `setViewportHandleEngaged(bool)` / `viewportHandleEngaged()` (also under the first names
    `setViewportHandleActive` / `viewportHandleActive`): phase 5 sets it while a handle is hovered or dragged;
    it is part of the Scene view's key routing condition.
  - Bone restores also put back the skinned meshes' `bonesMatrix`, `skinnedAABB` and `worldAABB`, which
    the scene file stores: a scene saved while posing is byte-identical to one saved without posing.
  - `keyRouting()`: the Scene view's keys and undo go to the window. True only while the window is
    shown, the session poses, and either the scene selection is the model or its bones, or a phase 5
    handle is active. While it is true the window shows a "Pose keys" hint over the Scene view.
  - `static suspendActive()`: called before duplicate, copy/cut, delete, create prefab (drag, variant)
    and apply prefab, so those copy the rest bones.
  - The rig follows the model's `.humanoid` sidecar: a change on disk (Map Bones, its undo/redo, MCP
    `set_humanoid`) rebinds.
  - Global Edit stays active through window edits that keep the poses (angles, offset, hold,
    transition, feel, humanize, mirror, paste, reset/ground/center). Selecting another pose,
    previewing, closing, adding, deleting or reordering cancels it (`poseui::keepsGlobalEdit`).
  - The DOF maths behind `E` lives in one place, `pose::jointRotation` / `pose::jointAngles` in
    `PoseRig.h`, which `PoseRig::localFromAngles` / `anglesFromLocal` use too.
- The window: `PoseAnimationWindow::addTab({name, draw, order})` is the Templates tab's extension point;
  MCP `focus_window {window: "pose_animation", pose: {entity, path, index, play, tab}}` binds, opens,
  selects, previews and switches tabs (`tab: "map_bones"` opens the mapping popup) and returns the window
  state.

## 3. The window (`editor/window/PoseAnimationWindow.{h,cpp}`)

Opened from **Window > Animation > Pose Animation**, docked next to the Animation window by
default and in Reset Layout, persisted in `PanelVisibilitySettings`; also opened by double-clicking
a `.poseanim` in the Project window and by the Pose Animator Inspector **Open** button (phase 2
placeholder made live). Width-responsive (the strip scrolls horizontally).

### 3.1 Layout, top to bottom

1. **Setup row**: Character field (scene Model entity; "Use Selection" button; accepts drag from
   the Hierarchy), Animation field (`.poseanim` asset; **New** creates one next to the model file
   with one neutral pose and the Snappy preset, preview model = the character's model file;
   **Open** browses). Rig status chip: "Humanoid 22/22" or "Missing: chest, neck" in warning
   colour with a **Map Bones...** button opening the humanoid mapping popup (section 3.4).
2. **Transport**: Play/Pause, Stop, scrub slider, time label (`1.20 s · 6 poses`, while previewing
   `0.42 / 1.20 s · frame 13`), Loop toggle, frame rate combo (24/30/60), **Bake** button with the
   target animation name field; a status line after baking ("Baked 37 frames to 'Walk' on Hero").
3. **Pose strip**: cards 100 px wide: thumbnail 88x116 (phase 3 `PoseRender`, three-quarter view,
   shaded, overlay off, cached by (pose body hash, rig), re-rendered lazily, the primary card live
   while posing), index, name, `hold 0.10s`; transition chips between cards (`→ 0.12s`, custom
   transitions tinted with the theme accent, `↺` for the loop back; clicking a chip selects the
   pose that owns the transition and focuses its transition fields); the last card is **+**
   (duplicates the primary pose after it and selects the copy; with no poses it adds a neutral pose).
   Click selects, Shift-click range, Ctrl/Cmd-click toggles. Drag to reorder (6 px threshold,
   insertion marker, multi-selection moves together). Right-click menu: Duplicate, Duplicate at
   End, Duplicate Mirrored, Mirror (L ↔ R), Copy Pose, Paste Pose (keeps the target's name, hold,
   transition), Move Left/Right, Rename, Reset to Neutral, Delete; labels pluralise for multiple
   poses. Mirrored copies swap L/R in the name (`Contact R` -> `Contact L`, `Left`/`Right`, `.L`/`.R`).
4. **Timing bar**: the whole animation to scale: hold blocks (selected highlighted, a 0 s hold
   still shows a 3 px marker), transitions drawn as their ease curve (24 samples, so overshoot
   and wind-up are visible), the playhead while previewing, frame ticks when zoomed in. Hover an
   edge (within 5 px) -> resize cursor; drag a hold end or a transition end: values snap to the
   frame grid (`1/frameRate`, or `1/stepRate` when choppy; hold Alt to disable snapping), the
   pixel scale is frozen during the drag, a tooltip shows the value, the whole drag is one undo
   step; dragging a transition end makes it custom. Holds clamp to the phase 1 ranges. Zero-length
   holds remain draggable (hold edges take priority over transition edges at the same x).
   Buttons: **Auto Timing**, **Reset Custom** (clears every custom transition).
5. **Tabs**: Pose, Feel, Humanize (and Templates in phase 6):
   - **Pose** (selected pose): Name, Hold, Grounded (and Lift when not grounded), Offset
     (left/forward), Custom transition toggle + curve combo + duration/overshoot/anticipation
     sliders with a live curve plot; tool buttons: Mirror, Reset to Neutral, Ground, Center
     (offset 0), Copy, Paste; **Joint angles** collapsible: every DOF grouped by body part
     (Body, Spine, Head, Left arm, Right arm, Left leg, Right leg), a drag-float per DOF in
     degrees with the limit range shown, values outside the limits in the theme's error colour;
     editing a value is an undoable commit (a drag merges). With several poses selected, fields
     show mixed values and edits apply to all.
   - **Feel**: preset chips (Snappy, Cartoon, Rubbery, Stop-motion, Heavy, Linear, Soft; the
     matching one lit), curve, snap time, overshoot, wind-up (greyed for non-spring curves) with a
     curve plot, choppiness (step rate, 0 = off, "on twos" hint at 12), overlap, looseness, body
     weights (spine, head, arms, legs) with one-line captions.
   - **Humanize**: enable toggle, joint limits, moving holds, in-betweens, life, seed (with a
     dice button), head follow, torso follow, springiness, each with a caption.
6. **Empty states** with one primary action: no character ("Select a character in the scene"),
   not humanoid ("This model's bones could not be mapped: Map Bones..."), no animation
   ("New" / "Open"), no poses ("+ Add pose").

### 3.2 Preview

Play/scrub drive `PoseEditSession` previewing: the bound model shows the realised frames
(phase 2 realisation, the same frames as bake and runtime) in the Scene view; one-shot animations
replay after a 0.5 s pause; Stop returns to posing the primary pose. Previewing hides phase 5
handles. Entering play mode stops the preview and restores the bones.

### 3.3 Keyboard shortcuts (window focused, or the Scene view while posing)

| key | action | | key | action |
|---|---|---|---|---|
| Space | play / pause | | Ctrl/Cmd+D | duplicate pose |
| Left / Right | previous / next pose | | Ctrl/Cmd+Shift+D | duplicate mirrored |
| Shift+Left / Right | move pose left / right | | M | mirror pose |
| Home / End | first / last pose | | Delete / Backspace | delete pose(s) |
| , / . | previous / next frame (preview) | | G | ground pose |
| Escape | stop preview / cancel Global Edit | | Ctrl/Cmd+C / V | copy / paste pose |
| F2 | rename pose | | N | add (duplicate after) |

Undo/redo (Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z, Ctrl/Cmd+Y) target the project history when the window
or a posing Scene view has focus, so pose edits undo in order. Shortcuts are listed in tooltips.

### 3.4 Humanoid mapping popup

Two columns: slot (grouped, required ones marked) and a combo of skeleton bones (search filter),
"Auto" resets a slot to auto-detect; status per slot (auto / override / missing); Apply writes the
`.humanoid` sidecar (phase 2 `set_humanoid` path, one undo step) and rebuilds the rig.

### 3.5 Project window and menus

- `.poseanim` gets a file type and icon in the Project window, a thumbnail (the middle pose via
  `PoseRender`), double-click opens the window, and **Create > Pose Animation** (in the Project
  window context menu) creates a new asset as in 3.1.
- `.humanoid` files show as a plain data file type.

## 4. Tests

- Editor logic that needs no ImGui (name mirroring, timing-bar hit testing and snapping, strip
  reorder index maths, preset matching, mixed-value multi-edit) lives in plain functions
  (`editor/pose/PoseUiLogic.{h,cpp}`) tested in `jasperengine-pose-tests` (link the file in).
- `tools/mcp/tests/test_pose_window.py` (fixture project): open the window via `focus_window`
  (`window: "pose_animation"`, extend the tool's window list), bind the hero, open an asset,
  verify through `screenshot view: editor` that the window renders (non-flat region), run
  shortcut-equivalent actions through the MCP `edit_pose_animation` path while the window is open
  and check it refreshes (the window title/status reflects the pose count via
  `editor_status`/window state if exposed), preview play then entering play mode restores the bone
  transforms (compare `get_entity` transforms of a bone before/after), saving the scene while
  posing writes rest transforms (scene file diff).
- Full MCP suite and pose tests pass.
