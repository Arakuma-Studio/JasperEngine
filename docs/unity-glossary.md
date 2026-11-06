# Unity glossary

JasperEngine uses Unity's names for its editor and scripting API. The table maps
each Unity term to the name JasperEngine used before.
The old names are gone: there are no aliases.

Projects made before the rename are migrated once with
`tools/unity_rename.py --project <project dir>`. It rewrites scenes, prefabs,
`project.yaml`, Lua and C++ scripts, and renames `*.bundle` files to `*.prefab`.

## Editor

| Unity term | Old JasperEngine name |
|---|---|
| Hierarchy window | Structure window |
| Inspector window | Properties window |
| Project window | Resources window |
| Console window | Output window |
| Game view (Window > General > Game) | none: play ran inside the scene tab |
| Main toolbar: Hand, Move, Rotate, Scale, Rect tools; Play, Pause, Step | Per-scene toolbar: Pan, Translate, Rotate, Scale, 2D gizmo; Play, Pause, Stop |
| GameObject | Entity / Object |
| Create Empty / Create Empty (No Transform) | Empty object / Empty entity |
| Active, Tag, Layer, component Enabled checkbox | none (new) |
| Prefab | Entity bundle (`.bundle` file) |
| Prefab > Unpack, Unpack Completely; the Inspector's Prefab bar (Select, Unpack) | none (new) |
| Override Prefab Component (component menu) | Make Unique |
| Removed GameObject overrides (deleting a Prefab GameObject in an instance); the Prefab bar's Overrides dropdown (Revert, Apply, Revert All, Apply All) | none (new): deleting removed it from the Prefab |
| Asset GUID (a prefab found by GUID after a rename or move); Missing Prefab (red row, Inspector bar) | none (new): references were paths, a deleted bundle took its instances with it |
| Assets > Prefabs... | Project > Bundles... |
| A Prefab field in the Inspector (a script property of type Prefab: drag a `.prefab` from the Project window onto it) | none (new) |
| Assets > Scenes in Build... | Project > Manage Scenes... |
| File > Build Settings... | File > Export Project... |
| Edit > Preferences... | Edit > Editor Settings... |
| Edit > Project Settings... | Project > Project Settings... |
| Project Settings > Physics / Physics 2D > Layer Collision Matrix | none (new) |
| none: Unity has no editor list of global shader values (Godot: Project Settings > Shader Globals) | none (new): Project Settings > Graphics > Shader Globals (name, type, default; a Live column in Play) |
| Project Settings > Input Manager (virtual axes and buttons; the Input System's actions) | Project Settings > Input (the actions asset and its Input Manager map) and Window > Input Actions (see [Input](#input) below) |
| Window menu | View menu |
| GameObject, Component, Assets menus | Scene and Project menus, the Structure "+" menu |
| Animation Clip (`.anim` asset; Assets > Create > Animation Clip) | none (new): keyframe animations lived only inside a scene or prefab as an Animation entity. A JasperEngine clip is JSON (`{"format": "jasperengine.anim"}`, not Unity's YAML), binds tracks by path relative to the GameObject that plays it, and is referenced by GUID; see [animation-clips.md](animation-clips.md) |
| An Animation Clip's `m_IsActive` curve (GameObject active) | the property track `gameobject.active` (stepped; applied after the other values, parents first) |
| An Animation Clip's `m_Enabled` curve (a component's checkbox) | the property track `<component>.enabled` (`ui`, `light`, `canvasgroup`, `rigidbody`...; `script:<Class>.enabled` for a Lua script) |
| An Animation Clip's `m_Color` curve (Image, Text, SpriteRenderer), TMP `m_fontColor` | `ui.color` on the UI element (keys in sRGB); a sprite's `mesh.submeshes[0].material.baseColorFactor` (Add Property: Sprite Renderer : Color); one channel: `ui.color.a` |
| Material curves (`material._Color`, `_EmissionColor`, `_Metallic`, `_MainTex_ST`) | `mesh.submeshes[N].material.baseColorFactor`, `.emissiveFactor`, `.metallicFactor`, `submeshes[N].textureRect`; shader library parameters `submeshes[N].material.shaderParams.<member>` |
| An Animation Clip's MonoBehaviour field curves | `script:<Class>.<field>` (a declared Lua `properties` field: bool, int, float, vector, color); not C++ script fields |
| `m_AnchoredPosition`, `m_SizeDelta`, `m_Pivot`, `m_AnchorMin` / `m_AnchorMax` curves (RectTransform) | `recttransform.positionOffset`, `width` / `height`, `pivot`, `anchorPointLeft` / `Right` / `Bottom` / `Top`; layout space is y-down, so y values convert: position y -> -y, pivot y -> 1 - y, `m_AnchorMin.y` -> `anchorPointBottom` 1 - y, `m_AnchorMax.y` -> `anchorPointTop` 1 - y |
| A sprite swap (`m_Sprite` PPtr curve) | `sprite.frame` (the frame index, stepped) |
| Animation window key tangents (Clamped Auto, Free Smooth, Flat, Broken, Linear / Constant per side) | the curves view's key menu (Auto, Free Smooth, Flat, Broken, Left / Right / Both Tangents) and its draggable handles |
| Animation window with an Animation Clip (Preview, Record, Dopesheet / Curves, Add Property, events) | Animation window (entity Animations only). It now edits clips when the selected GameObject or an ancestor has an Animation Player, or when a clip is opened from the Project window or the Inspector; entity Animations still open in its older mode |
| A model's imported clips (the Animation tab of a model's Import Settings; sub-assets such as `Take 001`) | none (new): model clips, read-only, named `<model>#<animation>` (`hero.gltf#Walk`; `"Wave 2"` for a second animation named Wave), with **Extract to .anim** for an editable copy and the Model section's **Use Animation Player** |
| Animator Controller (`.controller`), Animator window (Window > Animation > Animator: Layers, Parameters, state machine graph, blend trees), Avatar Mask (`.mask`), Animation Montage (Unreal's montages, `.montage`), Avatar / Configure Avatar | none (new): Assets > Create > Animator Controller / Avatar Mask / Animation Montage; the Avatar is the model's `.humanoid` map |
| none: Unity has no pose-to-pose animation (the closest is keyframing a humanoid clip in the Animation window) | none (new): Pose Animation (Window > Animation > Pose Animation; `.poseanim` assets and templates), played by the Pose Animator or baked to an Animation Clip or an Animation; see [pose-animation.md](pose-animation.md) |
| Avatar (Rig > Configure Avatar) | none (new): the model's humanoid map (22 body slots, fingers, eyes, jaw) detected from bone names, with twist corrections; edited in the Avatar Configuration window (Inspector > Model > Rig > Configure Avatar...) or MCP `set_humanoid`, stored in a `<model>.humanoid` file next to the model; see [pose-animation.md](pose-animation.md#the-avatar-configuration-window) |
| Rig > Animation Type: Humanoid / Generic | Rig > Animation Type (`animation_type` in the `.humanoid` file): a Humanoid model's clips play retargeted on any humanoid character |
| humanoid (muscle) clip, retargeting | humanoid clip: a `.anim` with `humanoid` tracks (per-slot joint rotations from the neutral pose, body position, foot goals; no muscles), from Animation Type Humanoid, an entry's **Retarget**, or Convert to Humanoid Clip...; see [animation-clips.md](animation-clips.md#humanoid-clips-and-retargeting) |
| Animator state Mirror / Foot IK | the Animator state's **Mirror** / **Foot IK** (and **Retarget**; a blend tree child's Mirror), the player entry's **Mirror** / **Foot IK**: one retarget path for both; Lua `state.mirror` / `state.footIK` (player), `AnimatorStateInfo.mirror` / `.footIK` |
| Color picker (the Color window: Hexadecimal, Intensity, the eyedropper, Swatches and `.colors` libraries) | ImGui's colour popup (no eyedropper, swatches or HDR Intensity); see [editor/color-picker.md](editor/color-picker.md) |
| Curve Editor, Gradient Editor, curve presets (`.curves`), Wrap Mode (Clamp, Loop, Ping Pong), tangent modes (Clamped Auto, Auto, Free Smooth, Flat, Broken; Free, Linear, Constant) | the Particle System's key tables; see [editor/curves.md](editor/curves.md) |
| Unity Hub (projects, templates, Add project from disk) | none (new): the Project Hub (File > Project Hub..., Shift at launch, `--hub`), with zip and Git import; the editor used to open a temporary project; see [editor/project-hub.md](editor/project-hub.md) |
| Unity Recorder (Window > General > Recorder: Image Sequence and Movie recorders, Recording Mode, Take, Exit Play Mode) | none (new): movies go through the user's FFmpeg; see [editor/recorder.md](editor/recorder.md) |
| none: Unity has no vertex animation textures built in (they come from Asset Store packages or Houdini's exporter, played by a custom material) | none (new): Vertex Animation (a `.vat` asset and its generated `.vat.glb`; Window > Animation > Vertex Animation Baker, the `.vat` Inspector's Rebake and Create Crowd...), baked from model clips, Animation Clips, Animation Players, pose animations and lattices, or imported from Houdini Labs VAT 3 and OpenVAT exports; see [vertex-animation.md](vertex-animation.md) |
| Animator Controller (`.controller`; Assets > Create > Animator Controller; the Animator window with layers, parameters, states, Any State, transitions, 1D blend trees) | none (new): Animator Controller, a JSON `.controller` (`{"format": "jasperengine.controller"}`, not Unity's YAML, which is refused) played by an **Animator** component and edited in Window > Animation > Animator (`Ctrl+7`) or with MCP `edit_animator_controller`; an Animation Player's Create Animator Controller from Clips makes one; see [animator.md](animator.md) |
| Avatar Mask (`.mask`; Assets > Create > Avatar Mask; Humanoid body parts and Transform paths) | none (new): Avatar Mask, a JSON `.mask` (humanoid body parts without the IK goals, hands and fingers apart, weights 0..1; transform paths with a weight and a children flag; a Default weight), set on an Animator layer or an Animation Player layer; see [animator.md](animator.md#layers-and-masks) and [animation-clips.md](animation-clips.md#avatar-masks) |

## Scripting API (Lua and C++)

| Unity term | Old JasperEngine name |
|---|---|
| `GameObject` | `Object` |
| `Primitive` (box, sphere, plane... meshes) | `Shape` |
| `Rigidbody2D` / `Rigidbody2DComponent` | `Body2D` / `Body2DComponent` |
| `Rigidbody` / `RigidbodyComponent` | `Body3D` / `Body3DComponent` |
| Collider (`Collider2D`, `Collider3D`, `createBoxCollider`, `getNumColliders`, `colliders`) | Shape (`Shape2D`, `Shape3D`, `createBoxShape`, `getNumShapes`, `shapes`) |
| `AudioSource` / `AudioSourceComponent` | `Sound` / `SoundComponent` |
| `Skybox` / `SkyboxComponent` | `SkyBox` / `SkyComponent` |
| `ShadowCaster2D` / `ShadowCaster2DComponent` | `Occluder2D` / `Occluder2DComponent` |
| `LineRenderer` / `LineRendererComponent` | `Lines` / `LinesComponent` |
| `ParticleSystem` / `ParticleSystemComponent` | `Particles` / `ParticlesComponent` |
| `AnimationCurve`, `Keyframe`, `WrapMode`, `TangentMode` (`AnimationCurve.Linear`, `Constant`, `EaseInOut`; `Evaluate`, `AddKey`, `MoveKey`, `RemoveKey`, `SmoothTangents`; `preWrapMode`/`postWrapMode`) | none (new): `AnimationCurve.linear`, `constant`, `easeInOut`; `evaluate`, `addKey`, `moveKey`, `removeKey`, `smoothTangents` with **1-based** key indices in Lua; C++ `Curve` / `CurveKey` (`AnimationCurve` and `Keyframe` are aliases); no weighted keys |
| `Gradient`, `GradientColorKey`, `GradientAlphaKey`, `GradientMode` (`Evaluate`, `SetKeys`) | none (new in Lua): `Gradient:evaluate(t)` (a `Vector4`), `setKeys(colorKeys, alphaKeys)`, `mode` |
| `ParticleSystem.MinMaxCurve`, `MinMaxGradient`, `ParticleSystemCurveMode`, `ParticleSystemGradientMode` | `MinMaxCurve`, `MinMaxGradient`, `MinMaxCurveMode`, `MinMaxGradientMode` (Lua) |
| DOTween's ease with an `AnimationCurve` (`SetEase(curve)`) | the Tween's Ease "Curve" (`EaseType.CURVE`, `TweenComponent.curve`, `tween.curve`) |
| `LineRenderer.positionCount`, `SetPosition(index, position)`, `GetPosition`, `SetPositions`, `loop`, `useWorldSpace` (0-based indices, as Unity) | none (new): the engine had independent segments only (`addLine`, still there as `mode` Segments) |
| `LineRenderer.widthMultiplier`, `widthCurve`, `startWidth` / `endWidth`, `colorGradient`, `startColor` / `endColor`, `numCornerVertices`, `numCapVertices` | none (new): lines were 1 pixel wide (width 0 still is that hairline) |
| `LineAlignment` (`View`, `TransformZ`), `LineTextureMode` (`Stretch`, `Tile`, `DistributePerSegment`, `RepeatPerSegment`), `textureScale` | same names (`LineAlignment.TransformZ`, `LineTextureMode.Tile`; MCP `transform_z`, `tile`) |
| `TrailRenderer` (`time`, `minVertexDistance`, `emitting`, `autodestruct`, `Clear`, `AddPosition`) | `TrailRenderer` / `TrailRendererComponent` (new; `object:getTrailRenderer()`) |
| A line's `material` (Default-Line, a shader with a sprite-sheet flipbook) | the built-in line shader with `texture` (u_baseTexture), or a `lines` library shader such as `library:line_flipbook` |
| `RectTransform` / `RectTransformComponent` | `UILayout` / `UILayoutComponent` |
| `LayoutGroup` / `LayoutGroupComponent` | `Container` / `UIContainerComponent` |
| `InputField` / `InputFieldComponent` | `TextEdit` / `TextEditComponent` |
| `Tween` / `TweenComponent` | `TimedAction` / `TimedActionComponent` |
| `PositionTween`, `RotationTween`, `ScaleTween`, `ColorTween`, `AlphaTween` | `PositionAction`, `RotationAction`, `ScaleAction`, `ColorAction`, `AlphaAction` |
| `GameObject.instantiate` / `GameObject::instantiate` (Unity's `Object.Instantiate`: a prefab or a copy of a GameObject, optional parent and `instantiateInWorldSpace`, or position and rotation) | `BundleManager.createBundle`, then `PrefabManager.createPrefab` (removed) |
| `GameObject.destroy(object [, delay])`, `GameObject.destroyImmediate(object)` (Unity's `Object.Destroy`, `DestroyImmediate`; scripts get `onDisable`, then `onDestroy`) | `BundleManager.destroyBundle`, then `PrefabManager.destroyPrefab` (removed) |
| MonoBehaviour messages: `Awake` -> `init`, `Start` -> `start`, `OnEnable` -> `onEnable`, `OnDisable` -> `onDisable`, `OnDestroy` -> `onDestroy`, `OnApplicationPause/Focus/Quit` -> `onApplicationPause/Focus/Quit` (Lua methods called by name; C++ `ScriptBase` overrides), Unity's rules and order ([lua-api.md](mcp/guide/lua-api.md) 1.1) | `init` (enabled scripts only, at load) and `onDestroy` (runtime destroy only); the others none (new) |
| `Update` / `FixedUpdate` / `LateUpdate` -> `onUpdate` / `onFixedUpdate` / `onPostUpdate` registered with `RegisterEngineEvent(self, "onUpdate")` (C++ `REGISTER_ENGINE_EVENT`) | unchanged |
| `MonoBehaviour.enabled` -> `self.enabled` (Lua, rw: fires `onEnable`/`onDisable`), `isEnabled()` / `setEnabled(bool)` (C++); `isActiveAndEnabled` -> `self.isActiveAndEnabled` / `isActiveAndEnabled()`; `gameObject` -> `self.gameObject` | none (new) |
| `GetComponent<Enemy>()`, `GetComponents`, `GetComponentInChildren`, `GetComponentsInChildren`, `GetComponentInParent` (for scripts) -> `go:getScript("Enemy")`, `getScripts`, `getScriptInChildren`, `getScriptsInChildren`, `getScriptInParent` (also on `self`); C++ `getScript<T>()` | none (new): a script property typed with the script's class |
| `transform.parent`, `SetParent(p, worldPositionStays)`, `childCount`, `GetChild(i)`, `Find("a/b")` -> `go.parent`, `go:setParent(p [, worldPositionStays])`, `go.childCount`, `go:getChild(i)` (0-based), `go:findChild("a/b")`, plus `go:getChildren()`; C++ `getParent()`, `setParent`, `getChildCount()`, `getChild`, `getChildren`, `findChild` | `addChild`, `removeParent` only (no way to read the parent or the children) |
| `Vector3.Lerp` (t clamped), `Vector3.LerpUnclamped`, `Vector3.MoveTowards` (also Vector2, Vector4) -> `a:lerp(b, t)` / `Vector3.lerp(a, b, t)`, `lerpUnclamped`, `moveTowards`; C++ the same | `Vector3::lerp` unclamped, C++ only |
| `Bounds` corners (`GetCorners` of a hand-written helper) -> `AABB:getCorners()` / `OBB:getCorners()` (8 `Vector3`) | one corner |
| `Rigidbody2D.AddForce` (always wakes the body) -> `applyForce(f, p [, wake = true])` and the other force/impulse methods | `wake` read as false when left out |
| Add Component > Scripts > a script, dragging a script onto a GameObject (Inspector or Hierarchy) -> the same: Add Component and the Script component's Add Script list the project's scripts; drops attach | Add Script added an empty C++ entry |
| `GameObject.Find`, `GameObject.FindWithTag`, `FindGameObjectsWithTag`, `Object.FindObjectOfType<T>()`, `FindObjectsOfType<T>(includeInactive)` -> `GameObject.find`, `findWithTag`, `findGameObjectsWithTag`, `findObjectOfType("T")`, `findObjectsOfType("T", includeInactive)` (every running scene) | `scene:findEntity`, `scene:findByTag` (one scene) |
| `Object.DontDestroyOnLoad(gameObject)` -> `DontDestroyOnLoad(self)` (global) or `GameObject.dontDestroyOnLoad(obj)`; C++ `ScriptBase::dontDestroyOnLoad()`; root GameObjects only, the whole hierarchy moves with its running state ([lua-api.md](mcp/guide/lua-api.md) 2.8.1) | none (new) |
| The "DontDestroyOnLoad" scene (Hierarchy in Play) -> the persistent scene `SceneManager.persistentScene`, a scene layer above the loaded scenes (Hierarchy row "DontDestroyOnLoad" in Play; MCP running scene with `persistent: true`) | none (new) |
| `SceneManager.MoveGameObjectToScene(go, scene)` -> `SceneManager.moveGameObjectToScene(obj, scene)` (C++ `SceneManager::moveGameObjectToScene`) | none (new) |
| Singleton pattern `if (instance == null) { instance = this; DontDestroyOnLoad(gameObject); } else if (instance != this) Destroy(gameObject);` -> `if GameObject.persistSingleton(self) ~= self then return end` in `init` (C++ `persistSingleton<T>()`); module tables are per instance, so no `static Instance` | none (new) |
| `UnityEvent` / C# `event` (`AddListener` / `+=`, `RemoveListener` / `-=`, `Invoke`) -> `Event()` (`addListener`, `removeListener`, `invoke`); `-=` of an engine or component event -> `UnregisterEngineEvent` / `UnregisterEvent` | none (new): no unregister from Lua |
| `PrefabAsset` (what a Prefab script property holds; `PrefabAsset.load("path")`) | none (new) |
| `PrefabManager` (the build's prefab registry: `findPrefab`, `getPrefabNames`, `prefabCount`) | `BundleManager` |
| `PrefabComponent` | `BundleComponent` |
| `GameObject:getRigidbody2D()`, `getRigidbody()` | `Object:getBody2D()`, `getBody3D()` |
| `AudioSource:getGameObject()` | `Sound:getObject()` |
| `Collider2DType`, `Collider3DType`, `RigidbodyMotionQuality` | `Shape2DType`, `Shape3DType`, `Body3DMotionQuality` |
| `AudioSourceState`, `AudioSourceAttenuation` | `SoundState`, `SoundAttenuation` |
| `LayoutGroupType`, `ShadowCaster2DShape` | `ContainerType`, `Occluder2DShape` |
| Legacy `Animation` component (`Play`, `CrossFade`, `Blend`, `PlayQueued`, `AnimationState`, Animation Events) | none (new): `AnimationPlayer` / `AnimationPlayerComponent` (Add Component > Animation > Animation Player; `gameObject:getAnimationPlayer()`, nil without one): `play`, `crossFade`, `blend`, `queue`, `getState` (an `AnimationState`), `onEvent` (one `AnimationEvent`), `onFinish` |
| `Animator` (`SetFloat`, `SetInteger`, `SetBool`, `SetTrigger`, `Play`, `CrossFade`, `CrossFadeInFixedTime`, `GetCurrentAnimatorStateInfo`, `IsInTransition`, `SetLayerWeight`, `SetIKPosition`, `GetBoneTransform`, `applyRootMotion`, `deltaPosition`, `OnAnimatorMove`, StateMachineBehaviour enter / exit) | none (new): `Animator` / `AnimatorComponent` (Add Component > Animation > Animator; `gameObject:getAnimator()`): `setFloat`, `setInteger`, `setBool`, `setTrigger`, `play`, `crossFade`, `crossFadeInFixedTime`, `getCurrentState` (an `AnimatorStateInfo`), `isInTransition`, `setLayerWeight`, `setIKTarget`, `getBonePosition` / `getBoneRotation`, `applyRootMotion`, `getDeltaPosition`, `onAnimatorMove`, `onStateEnter` / `onStateExit`; montages: `playMontage`, `onMontageEnded` |
| none: the old JasperEngine name, kept | `Animation` / `AnimationComponent` (Add Component > Animation > Animation) is not Unity's Animation component: it is JasperEngine's entity sequence of actions (keyframe tracks targeting scene GameObjects, tweens, sprite animations, particles, nested animations). Unity's legacy Animation component is the Animation Player |
| none: Unity has no live pose-to-pose player (the closest is an Animator with a humanoid Avatar, its `Animator.speed` and layer weight) | none (new): `PoseAnimator` / `PoseAnimatorComponent` (Add Component > Animation > Pose Animator; `model:getPoseAnimator()`) plays a `.poseanim` pose animation live on a humanoid Model |
| none: Unity has no built-in vertex animation player | none (new): `VertexAnimator` / `VertexAnimatorComponent` (Add Component > Animation > Vertex Animator; `gameObject:getVertexAnimator()`, nil without one) plays a `.vat` on the GameObject and on each instance of its Instanced Mesh: `play`, `crossFade`, `blend`, `queue`, `playInstance`, `crossFadeInstance`, `playAll`, `getSocketPose`, `onEvent`, `onFinish`, `onInstanceEvent` |
| `Input.GetAxis`, `GetAxisRaw`, `GetButton`, `GetButtonDown`, `GetButtonUp` (Input Manager axes) | none (new): `Input.getAxis`, `getAxisRaw`, `getButton`, `getButtonDown`, `getButtonUp`, plus `Input.getVector` (Godot's `get_vector`) and `Engine.onButtonDown` / `onButtonUp` |
| `Input.GetKey`, `GetKeyDown`, `GetKeyUp`, `GetMouseButton`, `GetMouseButtonDown`, `GetMouseButtonUp`, `anyKey`, `anyKeyDown`, `inputString`, `mouseScrollDelta` | `Input.isKeyPressed` (held only; kept); new: `Input.getKey`, `getKeyDown`, `getKeyUp`, `getMouseButton`, `getMouseButtonDown`, `getMouseButtonUp`, `anyKey`, `anyKeyDown`, `inputString`, `mouseScrollDelta`, `mouseDelta` |
| `KeyCode.Space`, `KeyCode.LeftControl`, `KeyCode.Mouse0`, `KeyCode.JoystickButton0` | `Input.KEY_SPACE`, `Input.KEY_LEFT_CONTROL`, `Input.MOUSE_BUTTON_LEFT`, `Input.GAMEPAD_BUTTON_A`; as control names (bindings, `Input.getKeyDown("...")`): `"space"`, `"left_control"`, `"mouse_left"`, `"gamepad_a"` |
| `Touch` (`fingerId`, `phase`, `position`, `deltaPosition`, `deltaTime`, `tapCount`), `TouchPhase`, `Input.touches`, `touchCount`, `GetTouch`, `multiTouchEnabled`, `touchSupported` | `Input.getTouches()` returned `{pointer, position}` tables; now `Touch` (same fields plus `startPosition`; `pointer` kept as the old name of `fingerId`), `TouchPhase`, `Input.touches`, `touchCount`, `getTouch`, `getTouchById`, `multiTouchEnabled`, `touchSupported` |
| `Input.simulateMouseWithTouches` | `Engine.callMouseInTouchEvent` |
| Input System: on-screen controls, `InputAction`, rebinding (`PerformInteractiveRebinding`, `SaveBindingOverridesAsJson`, `LoadBindingOverridesFromJson`) | none (new): virtual sources (`Input.simulateButton`, `simulateAxis`, `addAxisDelta`, `InputActions::setSourceValue` in C++), `Input.getPressedControl`, `Input.setBindings`, `exportBindings`, `importBindings` |
| EventSystem interfaces (`IPointerDownHandler`, `IPointerUpHandler`, `IPointerClickHandler`, `IPointerEnterHandler`, `IPointerExitHandler`, `IInitializePotentialDragHandler`, `IBeginDragHandler`, `IDragHandler`, `IEndDragHandler`, `IDropHandler`, `IScrollHandler`) | none (new): script methods `onPointerDown(e)`, `onPointerUp`, `onPointerClick`, `onPointerEnter`, `onPointerExit`, `onInitializePotentialDrag`, `onBeginDrag`, `onDrag`, `onEndDrag`, `onDrop`, `onScroll` (lua-api 1.7); the legacy `UIComponent.onPointerDown(x, y)` events stay |
| `PointerEventData` (`pointerId`, `position`, `delta`, `pressPosition`, `button`, `clickCount`, `dragging`, `pointerPress`, `pointerDrag`, `useDragThreshold`, `Use()`) | none (new): `PointerEventData` (same names, `use()`) |
| `EventSystem.current.IsPointerOverGameObject([pointerId])` | none (new): `Input.isPointerOverUI([pointerId])` (component UI and HTML content) |
| Input System `OnScreenStick`, `OnScreenButton`; Joystick Pack `FixedJoystick` / `FloatingJoystick` / `VariableJoystick` / `DynamicJoystick` | none (new): On-Screen Stick (`OnScreenStick`, `mode` `FIXED` / `FLOATING` / `DYNAMIC`; Joystick Pack's Variable = Dynamic), On-Screen Button (`OnScreenButton`), Touch Area (`TouchArea`, a drag feeding `Mouse X` / `Mouse Y`); HTML `<onscreen-stick>`, `<onscreen-button>`, `<touch-area>`. They write input actions (a flat Input Manager name or `Map/Action`) instead of control paths |
| `Physics` / `Physics2D` (`ignoreLayerCollision`, `getIgnoreLayerCollision`, `getLayerCollisionMask`, `setLayerCollisionMask`, `raycast`, `raycastAll`, `ALL_LAYERS`, `DEFAULT_RAYCAST_LAYERS`, `IGNORE_RAYCAST_LAYER`) | none (new) |
| `OnCollisionEnter/Stay/Exit(Collision)`, `OnTriggerEnter/Stay/Exit(Collider)`, and the `...2D` ones -> `onCollisionEnter/Stay/Exit`, `onTriggerEnter/Stay/Exit`, `onCollisionEnter2D/Stay2D/Exit2D`, `onTriggerEnter2D/Stay2D/Exit2D` (Lua methods called by name, also on disabled scripts; C++ `ScriptBase` overrides); after the fixed step, on the main thread ([physics.md](mcp/guide/physics.md)) | global events only (`beginContact2D`, `onContactAdded3D` on Jolt's job threads) |
| `Collision` (`gameObject`, `collider`, `rigidbody`, `contactCount`, `GetContact(i)`, `contacts`, `impulse`, `relativeVelocity`), `ContactPoint` (`point`, `normal`, `separation`, `impulse`, `thisCollider`, `otherCollider`), `ContactPoint2D` -> the same names lowerCamelCase (`getContact(i)` 0-based), plus `thisCollider` / `thisRigidbody` | none (new) |
| `Collision2D.otherCollider` / `otherRigidbody` (Unity: **this** side) -> the same (this side), plus the plain `thisCollider` / `thisRigidbody`; `collider` / `rigidbody` / `gameObject` are the other side's | none (new) |
| A child GameObject's collider with its own script -> a collider of the body whose `receiver` (Inspector "Send Callbacks To") is that GameObject | none (new) |
| `Physics.IgnoreCollision(a, b, ignore)` / `Physics2D.IgnoreCollision` (colliders) -> `Physics.ignoreCollision(a, b [, ignore])` / `Physics2D.ignoreCollision` on bodies (GameObjects or Rigidbodies): every collider of the pair | none (new) |
| `Shader.SetGlobalFloat`, `SetGlobalInt`, `SetGlobalVector`, `SetGlobalColor`, `SetGlobalMatrix`, `SetGlobalFloatArray`, `SetGlobalVectorArray`, `SetGlobalTexture`, `GetGlobal*`, `Shader.PropertyToID` | none (new): `Shader.setGlobalFloat`, `setGlobalInt`, ..., `getGlobal*`, `Shader.propertyToID`, plus `hasGlobal`, `getGlobalNames`, `resetGlobals`; shaders read them as members of a `u_vs_globals` / `u_fs_globals` block (or a texture uniform of the name); C++ `ShaderGlobals` ([shaders guide, Global values](mcp/guide/shaders.md#global-values)) |
| A material property a global feeds (Unity: the global applies when the material does not define the property) | none (new): an explicit binding, the Inspector's link button on a shader parameter row (`shaderGlobalBindings`, Lua `material:setShaderGlobalBinding(member, global)`) |
| `Shader.EnableKeyword` (global keywords) | not supported: shader features are per material (`setShaderFeature`) |

Unchanged: `Entity`, `Scene`, `Transform`, `Camera`, `Light`, `Light2D`, `Mesh`,
`Model`, `Sprite`, `Tilemap`, `Terrain`, `Fog`, `Mirror`, `ReflectionProbe`,
`Bone`, `Points`, `Image`, `Text`, `Button`, `Panel`, `Scrollbar`, `Progressbar`,
`Polygon`, `MeshPolygon`, `InstancedMesh`, `Action`, `Animation`,
`SpriteAnimation`, the keyframe tracks, `Joint2D`/`Joint3D`, `Contact2D`/`Contact3D`
and `CollideShapeResult3D`.

Unity's `VideoPlayer` is the **Video Player** component (`VideoPlayerComponent`, Lua
`VideoPlayer`: `gameObject:getVideoPlayer()`, Add Component > Video > Video Player): `source`,
`clip`, `url`, `playOnAwake`, `isLooping`, `renderMode`, `timeUpdateMode`, `audioOutputMode`,
`prepare()`, `play()`, `isPrepared`, `frame`, `frameCount`, the events `prepareCompleted` ->
`onPrepareCompleted` (likewise `started`, `loopPointReached`, `errorReceived`, `seekCompleted`,
`frameReady`). A `VideoClip` asset is the `clip` path of a video file (.mp4, .mov, .webm); a
`RenderTexture` target is the player's own texture, used through the `video|<entity>` texture link
(`player.texture`, `player:getFramebuffer()`); `SetDirectAudioVolume(0, v)` is `directVolume`.
Not offered: `DSPTime` (the audio follows the picture) and `APIOnly` audio. See
[mcp/guide/video.md](mcp/guide/video.md).

New, with no Unity counterpart: `FluidSimulation` / `FluidSimulationComponent`
(GameObject > Effects > Fluid Simulation, Pavel Dobryakov's WebGL fluid). Unity has no
built-in fluid component; there it takes a Custom Render Texture or compute-shader
solver of your own, or a package.

## Platform services (Application, SystemInfo, Screen, Handheld)

New: the engine had none of these. Unity's members became `System`, `Screen`, `Engine` and
`Input` ones (details: [platform-services.md](mcp/guide/platform-services.md)).

| Unity term | JasperEngine |
|---|---|
| `Application.OpenURL(url)` | `System.openURL(url)` (returns whether the system took it) |
| `Application.systemLanguage` | `System.getSystemLanguage()` (`SystemLanguage.*`, same names and values); codes: `System.getLanguageCode()`, `getRegionCode()`, `getLocale()`, `getPreferredLanguages()` |
| `Application.targetFrameRate` | `Engine.targetFrameRate` (Project Settings > Player > Target Frame Rate) |
| `QualitySettings.vSyncCount` | `Engine.vSyncCount` (starts from Quality > VSync) |
| `Screen.orientation`, `Screen.autorotateToPortrait` / `PortraitUpsideDown` / `LandscapeLeft` / `LandscapeRight` | `Screen.orientation`, `Screen.autorotateTo*` (`ScreenOrientation.*`) |
| `Screen.sleepTimeout`, `SleepTimeout.NeverSleep` / `SystemSetting` | `Screen.sleepTimeout`, `SleepTimeout.*` |
| `Screen.dpi`, `Screen.currentResolution.refreshRateRatio` | `Screen.dpi`, `Screen.refreshRate` |
| `Screen.width`, `Screen.height` | `System.getScreenWidth()`, `getScreenHeight()` (pixels), or `Engine.canvasWidth` / `canvasHeight` (canvas units) |
| `Input.deviceOrientation` | `Input.deviceOrientation` (`DeviceOrientation.*`) |
| `OnApplicationFocus(bool)` | the `onApplicationFocus(focused)` script method, or `RegisterEngineEvent(self, "onFocus")` |
| `OnApplicationPause(true / false)` | the `onApplicationPause(paused)` script method, or `onPause` / `onResume` engine events |
| `Application.isFocused` | `Engine.isFocused` |
| `Application.lowMemory` | `Engine.onLowMemory` |
| `Application.version`, `Application.identifier`, `Application.productName` | `System.getAppVersion()`, `getAppIdentifier()`, `getAppName()` |
| Android `versionCode`, iOS `CFBundleVersion` | `System.getAppBuild()` (an integer) |
| `SystemInfo.operatingSystem`, `deviceModel`, `deviceType`, `systemMemorySize`, `processorCount` | `System.getOperatingSystem()` (Unity's format), `getDeviceModel()`, `getDeviceType()`, `getSystemMemorySize()`, `getProcessorCount()`; also `getOperatingSystemVersion()`, `getAndroidApiLevel()` |
| `Application.isEditor` | `System.isEditor()` |
| `Application.internetReachability` | `System.getInternetReachability()` (`NetworkReachability.*`) |
| `Handheld.Vibrate()` | `System.vibrate([ms])`; iOS-style feedback: `System.hapticFeedback(HapticFeedback.*)` |
| Player > Default Orientation, Allowed Orientations for Auto Rotation | Project Settings > Player > Resolution and Presentation (`player.default_orientation`, `player.allowed_orientations`; shared by Android and iOS) |
| Device Simulator (orientation, language, `SystemInfo.deviceModel`, safe area, notch overlay) | the Game view's Device menu (device or Custom..., Rotate, Language), Device Overlay and Safe Area toggles; MCP `configure_game_view {device}` |
| `Screen.safeArea` (pixels, bottom-left origin) | `Screen.safeArea` (canvas units, top-left origin); pixels: `Screen.safeAreaPixels` (top-left); insets: `Screen.safeAreaInsets` (`EdgeInsets`) |
| `Screen.cutouts` | `Screen.cutouts` (canvas units; empty on iOS) |
| Community `SafeArea` script (Crystal SafeArea, a `CanvasHelper` safe panel) | the Safe Area component (GameObject > UI > Safe Area; per-edge Left/Top/Right/Bottom) |
| A banner margin added to the safe panel | `Screen.setExtraInsets(l, t, r, b [, "pixels"])` |
| Player > Android > *Render outside safe area* | the default Android fullscreen (the game draws under the cutout; `Screen.safeArea` keeps the UI out) |
| CSS `env(safe-area-inset-*)` (`viewport-fit=cover`) | the same in HTML UI documents (live); web export `web.viewport_fit_cover` |

## UI scaling, TextMesh Pro, UI particles

New with ui-scaling-text (details: [ui.md](mcp/guide/ui.md), HTML: [html-ui.md](mcp/guide/html-ui.md)).

| Unity term | JasperEngine |
|---|---|
| `CanvasScaler` (UI Scale Mode, Reference Resolution, Screen Match Mode, Match, Scale Factor, Physical Unit, Fallback Screen DPI) | the Canvas Scaler: Project Settings > Graphics > UI Scale (`graphics.ui_scale`), a scene's Canvas Scaler (scene setting `canvas_scaler`, Lua `scene:getCanvasScaler()`: `uiScaleMode`, `referenceResolution`, `screenMatchMode`, `matchWidthOrHeight`, `scaleFactor`, `physicalUnit`, `fallbackScreenDPI`); the extra mode Follow Game Canvas (default) keeps the game canvas |
| `CanvasScaler.ScaleMode.*`, `ScreenMatchMode.*`, `CanvasScaler.Unit.*` | `UIScaleMode.*`, `ScreenMatchMode.*`, `PhysicalUnit.*` (same names) |
| A script switching `matchWidthOrHeight` between 0 and 1 by aspect (`CanvasScaleAdjuster`) | Screen Match Mode Expand |
| `RectTransformUtility.ScreenPointToLocalPointInRectangle` (screen to canvas units) | `UI.canvasToUI(scene, point)` / `UI.uiToCanvas(scene, point)` |
| Game view resolution presets (fixed resolution, custom sizes) | the Game view's resolution menu (Resolutions, Add custom...); MCP `configure_game_view {resolution}` |
| TMP Outline (Thickness, Color) | Text `outlineWidth` (UI units), `outlineColor` |
| TMP Underlay (Offset, Softness, Dilate, Color) | Text `shadowEnabled`, `shadowOffset` (UI units, right-down), `shadowSoftness`, `shadowDilate`, `shadowColor` |
| TMP `maxVisibleCharacters` | Text `visibleCharacters` |
| TextMesh Pro Effect (Waves, Grow, Unfold, Bounce, Sketch; Duration, Amplitude, Character Duration Ratio, Gradient + Mix, Repeat, AutoPlay, ForWords) | Text Effect component (`TextEffect` in Lua): layers Wave (amplitude 0.3 x TMP Amplitude em), Grow, Unfold, Bounce, Shake, Gradient (spread = Character Duration Ratio, mix add/multiply), `loop`, `autoPlay`, `forWords`; HTML: CSS `text-effect` |
| Coffee `UIParticle` (particles in a canvas) | a Particle System under the UI element (GameObject > UI > Particle System, `useUnscaledTime`); HTML: `<particle-system src="....prefab">` |
| `ParticleSystem.main.useUnscaledTime` | `ParticleSystem.useUnscaledTime` |

## Coroutines, timers and time

Rules and the frame order: [lua-api.md](mcp/guide/lua-api.md) 2.13 (Time) and 2.14 (coroutines
and timers); C++ timers: [cpp-scripting.md](mcp/guide/cpp-scripting.md).

| Unity term | JasperEngine name |
|---|---|
| `StartCoroutine(IEnumerator)` / `StartCoroutine("Name")` | `self:startCoroutine(fn, ...)` / `self:startCoroutine("name", ...)` (returns a `Coroutine`; `Coroutine.start(fn)` for code that is not a script) |
| `StopCoroutine(c)` / `StopCoroutine("Name")`, `StopAllCoroutines()` | `self:stopCoroutine(co)` / `self:stopCoroutine("name")` (or `co:stop()`), `self:stopAllCoroutines()` |
| `IEnumerator Start()` | `self:startCoroutine` from `init` (or `start`, which may wait, with the script lifecycle) |
| `yield return null` | `Coroutine.nextFrame()` (or `coroutine.yield()`) |
| `yield return new WaitForSeconds(t)` | `Coroutine.wait(t)` (game time; returns the seconds that passed) |
| `yield return new WaitForSecondsRealtime(t)` | `Coroutine.waitRealtime(t)` (unscaled; unlike Unity it stops while the app or the editor's Play is paused) |
| `yield return new WaitForFixedUpdate()`, `WaitForEndOfFrame()` | `Coroutine.waitForFixedUpdate()`, `Coroutine.waitForEndOfFrame()` |
| `yield return new WaitUntil(f)`, `WaitWhile(f)` | `Coroutine.waitUntil(f [, timeout])`, `Coroutine.waitWhile(f [, timeout])` (a timeout returns false) |
| `yield return StartCoroutine(x)`, `yield return www` / `yield return request.SendWebRequest()` | `Coroutine.waitFor(self:startCoroutine(x))` (true and its results), `Coroutine.waitFor(handle)` for anything with `isDone()`; or call `self:x()` directly (Lua yields through calls) |
| `Invoke("M", t)`, `InvokeRepeating("M", t, r)`, `CancelInvoke(["M"])`, `IsInvoking(["M"])` | `self:invoke("m", t)`, `self:invokeRepeating("m", t, r)`, `self:cancelInvoke(["m" or timer])`, `self:isInvoking([...])` (also with a function; `unscaled` flag); C++ `ScriptBase::invoke(fn, t)`, `invokeRepeating`, `cancelInvoke(id)` |
| `LeanTween.delayedCall(t, fn)`, DOTween `DOVirtual.DelayedCall` | `self:invoke(fn, t)` or `Timer.after(t, fn [, {owner = self, unscaled = true}])`; repeating `Timer.every(r, fn)`; C++ `jasperengine::Timer::after` |
| `Time.deltaTime`, `unscaledDeltaTime`, `fixedDeltaTime`, `fixedUnscaledDeltaTime` | `Time.deltaTime`, `Time.unscaledDeltaTime`, `Time.fixedDeltaTime`, `Time.fixedUnscaledDeltaTime` (also `Engine.deltatime`, `Engine.unscaledDeltatime`, `Engine.updateTime`) |
| `Time.time`, `fixedTime`, `unscaledTime`, `realtimeSinceStartup`, `timeSinceLevelLoad`, `frameCount` | the same names on `Time` (`Engine.gameTime`, `Engine.frameCount`) |
| `Time.timeScale`, `maximumDeltaTime`, `captureDeltaTime`, `smoothDeltaTime`, `inFixedTimeStep` | the same names on `Time` (`Engine.timeScale`, `Engine.maxDeltatime`, `Engine.fixedDeltatime`: a fixed frame delta, not the fixed step) |

## Camera shake and camera follow

Rules, presets and the frame order: [camera.md](mcp/guide/camera.md) (users:
[docs/camera.md](camera.md)); Lua: [lua-api.md](mcp/guide/lua-api.md) 4.9.1.

| Unity term | JasperEngine name |
|---|---|
| MilkShake `Shaker` (component) | Camera Shaker component (`camerashaker`; Lua `CameraShaker`, `go:getCameraShaker()`): on a camera a view offset, the Transform never written |
| MilkShake `Shaker.ShakeAll(preset)`, `GlobalShakers`, `addToGlobalShakers` | `CameraShake.shake(preset)` on every shaker with `global` on (default on) |
| MilkShake `shaker.Shake(preset, seed)`, `ShakeFromPoint(point, maxDistance, preset)` | `shaker:shake(preset, {seed = s})`, `shaker:shakeAt(point, radius, preset)` / `CameraShake.shakeAt` |
| MilkShake `ShakePreset` (asset), `ShakeParameters` | Shake Preset: a `.shake` file (Assets > Create > Shake Preset) or a built-in name (`CameraShake.getPresetNames()`), or a Lua table |
| MilkShake `ShakeInstance`: `Stop(fade, true)`, `Pause(fade)`, `Resume(fade)`, `StrengthScale`, `RoughnessScale`, `State`, `IsFinished` | `ShakeInstance`: `stop([fade])`, `pause([fade])`, `resume([fade])`, `strengthScale`, `roughnessScale`, `state`, `isFinished()` (plus `isValid()`) |
| EZ Camera Shake `CameraShakePresets.Bump / HandheldCamera / Vibration` | built-ins `bump`, `handheld`, `vibration` (strength = magnitude / 2) |
| Trauma shake ("Juicing Your Cameras With Math") | `CameraShake.addTrauma(x)`, `traumaPower`, `traumaDecay`, `traumaMaxOffset`, `traumaMaxAngle` |
| Cinemachine Impulse Source (`GenerateImpulse`) / Impulse Listener (gain) | `CameraShake.shake` / `shakeAt` (distance falloff only, no propagation speed) / Camera Shaker (`positionScale`, `rotationScale`; `mode` "2d") |
| Cinemachine virtual camera follow (Transposer / Framing Transposer + Composer), simple cases | Camera Follow component (`camerafollow`): `target`, `offset` (`offsetSpace`), `damping`, `lookAt`, `deadZone` (2D), bounds; no blending, priorities or collision |
| `Mathf.SmoothDamp` in a hand-written follow script | Camera Follow `damping` (the same formula, per world axis) and `maxSpeed` |
| `transform.localPosition = shake` on a child camera (MilkShake's setup) | not needed: the shake is a view offset, put the Camera Shaker on the camera itself |

## Saves and data

| Unity term | JasperEngine name |
|---|---|
| `PlayerPrefs.GetInt/SetInt/GetFloat/SetFloat/GetString/SetString/HasKey/DeleteKey/DeleteAll/Save` | `PlayerPrefs.getInt/setInt/getFloat/setFloat/getString/setString/hasKey/deleteKey/deleteAll/save` (Lua), over the same store as `UserSettings` (`getIntegerForKey`, `setStringForKey`, ... and `hasKey`, `deleteKey`, `deleteAll`, `save`; C++ `jasperengine::UserSettings`) |
| Edit > Clear All PlayerPrefs | Edit > Clear User Settings (PlayerPrefs)...; MCP `user_settings` (list, get, set, delete, clear) |
| `JsonUtility.ToJson` / `FromJson`, Newtonsoft `JsonConvert.SerializeObject` / `DeserializeObject`, `JObject` | `Json.encode(value [, opts])` / `Json.decode(text [, opts])` (tables, not typed classes; `Json.null`, `Json.array`, `Json.object`); C++ `nlohmann::json`, `jasperengine::LuaJson` |
| `Application.persistentDataPath` | `data://` (`System.getUserDataPath()`) |
| `File.ReadAllText` / `WriteAllText` / `AppendAllText` / `Exists` / `Delete`, `Directory.CreateDirectory` / `GetFileSystemEntries` | `File.readText` / `writeText` (atomic) / `appendText` / `exists` / `remove`, `File.makeDirs` / `File.list` (return `nil, message` on failure) |
| `Resources.Load<TextAsset>(path).text`, `StreamingAssets` files | `File.readText("asset://path")` (read-only, in the APK on Android too); `Json.readFile("asset://...")` |
| ScriptableObject (data assets) | a JSON file (`Json.readFile("lua://data/items.json")`) or a Lua data module (`require("data.items")`): jasper://docs/lua-api 2.4.2 |

## Debugging

| Unity term | JasperEngine name |
|---|---|
| `Debug.Log`, `Debug.LogWarning`, `Debug.LogError` | `Log.print`, `Log.warn`, `Log.error` (Lua; any values, tab-joined like `print`), C++ `Log::print/warn/error` |
| `LogType` (`Log`, `Warning`, `Error`) | `LogLevel` (`INFO`, `VERBOSE`, `DEBUG`, `WARN`, `ERROR`) |
| `Application.logMessageReceived` (`condition, stackTrace, type`) | `Log.onMessageReceived` (`RegisterEvent(self, Log.onMessageReceived, "onLog")`, handler `(message, stackTrace, level)`), C++ `Log::addListener` |
| `Application.logMessageReceivedThreaded` | C++ `Log::addListener(fn, LogDelivery::AnyThread)` |
| `Debug.isDebugBuild`, the Development Build checkbox | `Engine.isDevelopmentBuild`: the editor's Play and exports built with the Debug build type |
| IngameDebugConsole (`DebugLogManager`, `DebugLogConsole.AddCommand`, `ExecuteCommand`, the popup) | `DebugOverlay` (`showConsole`, `toggleConsole`, `addCommand`, `executeCommand`; the popup and the backquote key), Project Settings > Debug |
| Graphy (FPS, RAM, audio monitors) | the debug overlay's stats panel (`DebugOverlay.statsMode`, F3) and `DebugOverlay.getStats()` |

See [mcp/guide/debugging.md](mcp/guide/debugging.md).

## Native plugins

Details: jasper://docs/native-bridge.

| Unity term | JasperEngine name |
|---|---|
| `new AndroidJavaClass("com.me.X").CallStatic<T>("m", args)` | `Native.call("X", "m", ...)` (a public static method of a class in `plugins/native/android`; the full name works too) |
| `AndroidJavaProxy` callbacks, `UnitySendMessage` / `SendMessage` from native code | `NativeCall` (`Native.callAsync`: one result) or `JasperNative.emit(plugin, event, payload)` + `Native.on(plugin, event, fn)` |
| `[DllImport("__Internal")]` functions (iOS) | Objective-C/Swift class methods `+ (id)m:(NSArray*)args` in `plugins/native/ios` or `apple` |
| `.jslib` (`mergeInto(LibraryManager.library, ...)`), `SendMessage` (Web) | `plugins/native/web/*.js` with `JasperNative.register(name, {...})`, `JasperNative.emit` |
| `Assets/Plugins/Android`, `Assets/Plugins/iOS`, `Assets/Plugins/WebGL` | `plugins/native/android`, `plugins/native/ios` (or `apple`), `plugins/native/web` |
| Native C++ plugins (desktop `DllImport`) | `JASPERENGINE_NATIVE_PLUGIN(Name)` in `plugins/native/cpp` |
| External Dependency Manager `*Dependencies.xml` (`androidPackage`, `iosPod`) | `android.gradle_dependencies` + `android.gradle_repositories`, `ios.cocoapods` (Project Settings > Native Plugins) |
| `mainTemplate.gradle`, `gradleTemplate.properties`, custom `AndroidManifest.xml` | `android.gradle_*` settings, `android.manifest_xml` / `manifest_application_xml`, `plugins/native/android/AndroidManifest.xml` (merged) |
| `PostProcessBuild` Info.plist / capabilities edits | `ios.plist_entries`, `ios.plist_xml`, `ios.frameworks`, `ios.entitlements_xml`, `ios.swift_packages` |

## UI (Canvas Group)

| Unity term | JasperEngine name |
|---|---|
| `CanvasGroup` (`alpha`, `interactable`, `blocksRaycasts`, `ignoreParentGroups`) | Canvas Group component (`canvasgroup`); Lua `CanvasGroup(scene, entity)` with the same fields plus `renderMode` (`PER_ELEMENT` = Unity, `FLATTEN` = CSS opacity), `object:getCanvasGroup()`; see [mcp/guide/ui.md](mcp/guide/ui.md) |
| `LeanTween.alphaCanvas(cg, to, time)`, DOTween `cg.DOFade(to, time)` | `group:fadeTo(to, time [, {ease, unscaled, deactivate, onComplete}])`, `fadeIn`, `fadeOut` |
| `CanvasRenderer.GetInheritedAlpha()` | `RectTransform:getInheritedAlpha()` |
| `Selectable.IsInteractable()` | `RectTransform:isInteractable()` |
| `Graphic.raycastTarget` | `RectTransform.ignoreEvents` (inverted: `ignoreEvents = true` is `raycastTarget = false`) |
| `ColorBlock.disabledColor` | `Button.colorDisabled` (default Unity's (200, 200, 200, 128)) |
| An Animation Clip's `m_Alpha` curve on a CanvasGroup | the property track `canvasgroup.alpha` (and `interactable`, `blocksRaycasts`, `ignoreParentGroups`) |
| Unity's quirk: a parent group at alpha 0 also hides an `ignoreParentGroups` child | not copied: the child shows at its own alpha |

## Networking

Details: jasper://docs/networking.

| Unity term | JasperEngine name |
|---|---|
| `UnityWebRequest.Get` / `Post` / `Put` / `Delete` / `Head` + `SendWebRequest()` | `Http.get` / `Http.post` / `Http.put` / `Http.delete` / `Http.head` (a callback on the main thread; also `Http.patch`, `Http.request{...}`); C++ `jasperengine::Http::send` |
| `UnityWebRequest.SetRequestHeader`, `timeout`, `redirectLimit` | options `headers`, `timeout` (seconds), `redirects` |
| `DownloadHandlerBuffer.text` / `.data` | `response.body` (`response:json()`, `responseType = "json"` -> `response.data`) |
| `DownloadHandlerFile` | `Http.download(url, "data://...")` |
| `DownloadHandlerTexture`, `UnityWebRequestTexture.GetTexture` (+ `Texture2D.LoadImage`, `Sprite.Create`) | `Http.getTexture(url, cb)` -> `response.textureData`, `image:setTexture(response.url, response.textureData)` |
| `UnityWebRequest.downloadProgress` | `request.progress`, option `onProgress` |
| `UnityWebRequest.Abort()` | `request:cancel()` (`Http.cancelAll()`) |
| `UnityWebRequest.result` / `error` / `responseCode` | `response.ok` + `response.errorCode`, `response.error`, `response.status` |
| `certificateHandler` accepting every certificate | option `verifyTls = false` (a test server only) |
| `UnityWebRequest.EscapeURL` / `UnEscapeURL` | `Http.encodeUrl` / `Http.decodeUrl` |
| Player Settings > Android > *Internet Access* (Auto / Require) | Project Settings > Android > Internet Access (`android.internet_access`: `auto` / `require`) |
| Player Settings > *Allow downloads over HTTP* | Project Settings > Player > Other Settings > Allow Insecure HTTP (`player.allow_insecure_http`) |

## Input

Unity's Input System is an `.inputactions` asset here (action maps, actions, bindings,
composites, control schemes, rebinding: Window > Input Actions, `InputActions.get("Map/Action")`),
and Unity's Input Manager is that asset's **Input Manager** map: named actions with bindings (Key or
Mouse Button, Gamepad Axis, Mouse Movement) and Unity's knobs (gravity, sensitivity, dead, snap,
invert), read by `Input.getAxis` / `getButton*` by name. A project without an asset has the same
11 defaults (`Horizontal`, `Vertical`, `Fire1`..`Fire3`, `Jump`, `Mouse X`, `Mouse Y`,
`Mouse ScrollWheel`, `Submit`, `Cancel`); Project Settings > Input edits the map (and makes the
asset). Differences: several bindings of an action add up and clamp (Unity takes the largest
same-named entry); the joystick dead zone is rescaled (Unity clips); `getButton` on an axis is
true for both sides; a click or key the UI consumed does not count; an unknown name logs once and
reads 0 (Unity throws). On-screen sticks and buttons feed the same actions, so
`Input.getAxis("Horizontal")` sums keyboard, gamepad and the touch stick. See
[mcp/guide/input.md](mcp/guide/input.md).

## Audio

The project has one Audio Mixer asset (`.mixer`, JSON), chosen in Project Settings > Audio and
edited in Window > Audio > Audio Mixer or with MCP `audio_mixer`. Without one every sound plays
through an implicit Master at 0 dB. See [mcp/guide/audio.md](mcp/guide/audio.md).

| Unity term | JasperEngine name |
|---|---|
| Audio Random Container | Sound Event (`.soundevent`, Assets > Create > Sound Event); `AudioSource.event`, Lua `Audio.play` |
| Timeline Audio Track (audio clips on a timeline), Animation Events that call a method playing a sound | audio cues on the clip's Audio lane (`.anim` `audio`: sound, source or snapshot cues, placed to the sample; MCP `edit_animation {clip, audio}`); on an entity Animation, audio actions and Mixer Snapshot actions (`AudioSnapshotComponent`) |
| A music manager script (crossfades, playlists, beat sync, stingers) | The music system: Lua `Audio.playMusic`, `Audio.playStinger`, `Audio.onMusicBeat`; a Sound Event as a playlist; Project Settings > Audio > Music Group |
| `AudioSource.PlayOneShot` of a Random Container | `AudioSource:playOneShot("x.soundevent")` |
| `AudioMixer` (asset) | `.mixer` asset (Assets > Create > Audio Mixer); Lua `AudioMixer` (static: the project's mixer) |
| `AudioMixerGroup`, `AudioSource.outputAudioMixerGroup` | mixer group; `AudioSource.outputGroup` (the group's name, "" = Master); Lua `AudioMixer.getGroup(name)` -> `AudioMixerGroup` |
| `AudioMixer.SetFloat` / `GetFloat` / `ClearFloat`, exposed parameters | `AudioMixer.setFloat(name, value [, seconds [, opts]])` / `getFloat(name)` / `clearFloat(name)`; expose in the Audio Mixer window (right-click a fader or a value) or `audio_mixer` op `expose`; "Remember" keeps script values in UserSettings |
| `AudioMixerSnapshot.TransitionTo`, `AudioMixer.TransitionToSnapshots`, `FindSnapshot` | `AudioMixer.transitionTo(name, seconds [, opts])`, `AudioMixer.transitionToSnapshots(names, weights, seconds)`, snapshots by name |
| Audio Mixer update mode Normal / Unscaled Time | mixer `updateMode` "scaled" / "unscaled"; per call `{unscaled = true}` |
| Lowpass / Highpass / Echo / SFX Reverb / ParamEQ / Distortion / Flange / Duck Volume effects | mixer effects `lowpass`, `highpass`, `bandpass`, `echo`, `reverb`, `equalizer`, `distortion`, `flanger`, `lofi`, `bassboost`, `duck`, `limiter` (up to 8 per group) |
| `AudioSource.PlayOneShot(clip, volumeScale)` | `sound:playOneShot(path [, volumeScale])` (up to `maxVoices` overlapping voices) |
| `AudioSource.PlayClipAtPoint(clip, position, volume)` | `AudioSource.playClipAtPoint(scene, path, position [, volume [, group]])` |
| AudioClip Load Type Decompress On Load / Compressed In Memory / Streaming | `AudioSource.loadType` (`AudioLoadType.DecompressOnLoad` / `CompressedInMemory` / `Streaming`, default `Auto`: streams files of 1 MiB and more): per source, not a clip import setting |
| `AudioImporter` | `.meta` section `import.audioImporter` (Inspector's Import Settings, MCP `set_audio_import_settings`) |
| AudioClip import settings | `type`, `gain_db`, `trim_head_ms`, `trim_tail_ms`, `fade_in_ms`, `fade_out_ms`, `loop_start_ms`, `loop_end_ms`, `music` (tempo); the engine plays them in Play and in exports (see [mcp/guide/sound.md](mcp/guide/sound.md)) |
| `AudioSource.pitch` | `pitch` (= `speed`) |
| `AudioSource.priority`, Project Settings Real / Virtual Voices | `priority` (0..256, 128); Project Settings > Audio Max Real Voices (32) and Max Voices (256) |
| `AudioSource.ignoreListenerPause`, `AudioListener.pause`, `AudioListener.volume` | the same names (`AudioListener.volume` = `AudioSystem.globalVolume`) |
| volume fades (tweens, `DOFade`) | `sound:fadeIn(seconds)`, `fadeOut(seconds [, {pause = true}])`, `fadeTo(volume, seconds)` |

## Layers and physics

Each physics backend has its Layer Collision Matrix (Project Settings > Physics for
Jolt, Physics 2D for Box2D), saved in `project.yaml` as the layer pairs that do not
collide (`ignoredLayerCollisions: {physics: [[8, 9]], physics2D: [...]}`). Bodies on a
disabled pair pass through each other and report no contact or trigger events. The
matrix gates first: a pair it allows still needs the colliders' category and mask bits
(`setBitsFilter`, and the 2D group index) to match. `Physics.ignoreLayerCollision` and
the `Physics2D` one change it at runtime; in the editor that lasts until Play stops.
Changing a GameObject's layer re-filters its Rigidbody (2D) before the next step.

Ray casts take a layer mask (`LayerMask.getMask(...)`, `Physics.ALL_LAYERS`):
`Physics.raycast(scene, ray [, layerMask])`,
`Physics.raycast(scene, origin, direction, maxDistance [, layerMask])` and `raycastAll`,
and the `Physics2D` ones with `Vector2` origin and direction. Without a mask they use
`DEFAULT_RAYCAST_LAYERS`, every layer but Ignore Raycast; so do the scene queries of
`Ray:intersects`. With a `Ray`, `distance` is the fraction of `ray.direction`; with an
origin, a direction and a distance it is in world units (scene units in 2D).

## 2D physics: Unity names and their JasperEngine counterparts

| Unity | JasperEngine |
|---|---|
| `WheelJoint2D`, `HingeJoint2D`, `SliderJoint2D`, `DistanceJoint2D`, `SpringJoint2D`, `FixedJoint2D`, `RelativeJoint2D`, `FrictionJoint2D`, `TargetJoint2D` | one `Joint2D` component (Hierarchy > Physics > Joint 2D) with `type` `WHEEL`, `REVOLUTE`, `PRISMATIC`, `DISTANCE`, `DISTANCE` + `enableSpring`, `WELD`, `MOTOR`, `MOTOR` + `correctionFactor = 0`, `MOUSE` |
| `connectedBody` | `bodyB` (alias `connectedBody`); the joint's own Rigidbody 2D is `bodyA` |
| `anchor`, `connectedAnchor`, `autoConfigureConnectedAnchor` | `anchorA`, `anchorB` (aliases `anchor`, `connectedAnchor`), `autoAnchors`, with `anchorSpace = LOCAL` |
| `suspension.frequency`, `suspension.dampingRatio`, `suspension.angle` | `frequency`, `dampingRatio`, `axis` (angle 90 = `(0, 1)`) |
| `useMotor`, `motor.motorSpeed`, `motor.maxMotorTorque` | `useMotor`, `motorSpeed` (deg/s), `maxMotorTorque` (`maxMotorForce` for sliders and distance) |
| `useLimits`, `limits.min/max` (hinge), `limits` (slider) | `useLimits`, `lowerAngle` / `upperAngle`, `lowerTranslation` / `upperTranslation` |
| `maxDistanceOnly`, `autoConfigureDistance`, `distance` | `rope`, `autoLength`, `length` |
| `breakForce`, `breakTorque`, `breakAction` (`JointBreakAction2D`) | `breakForce`, `breakTorque`, `breakAction` (`JointBreakAction.IGNORE` / `CALLBACK_ONLY` / `DISABLE` / `DESTROY`) |
| `OnJointBreak2D(Joint2D)` | `onJointBreak2D(self, joint)` (Lua), `onJointBreak2D(Joint2D&)` (C++ `ScriptBase`) |
| `reactionForce`, `reactionTorque`, `jointAngle`, `jointSpeed`, `jointTranslation`, `GetMotorTorque` | `reactionForce`, `reactionTorque`, `jointAngle`, `jointSpeed`, `jointTranslation`, `motorTorque` |
| `PolygonCollider2D` | a `POLYGON` collider (any number of points, concave split in convex pieces; one path per collider) |
| `EdgeCollider2D` | a `CHAIN` collider (2+ points) |
| `PhysicsMaterial2D` (`friction`, `bounciness`) | `PhysicsMaterial` (`.physicsmaterial`, Create > Physics Material 2D), shared with 3D, with `frictionCombine` / `bounceCombine` (Unity 2D's `Mean` is `geometric_mean`; the name `mean` is read as it) |
| `Rigidbody2D.sharedMaterial`, `Collider2D.sharedMaterial` | the body's `material`, the collider's `material` |
| `useAutoMass`, `mass`, `centerOfMass`, `inertia` | `useAutoMass`, `mass`, `centerOfMass` (+ `autoCenterOfMass`), `inertia` (+ `autoInertia`) |
| `drag`, `angularDrag`, `gravityScale`, `freezeRotation` | `linearDamping`, `angularDamping`, `gravityScale`, `fixedRotation` |
| `collisionDetectionMode` (`CollisionDetectionMode2D`), `sleepMode` (`RigidbodySleepMode2D`) | `collisionDetection`, `sleepingMode` (same enum names) |
| `isTrigger` (a collider) | the collider's `sensor` (`setColliderTrigger(i, b)`) |
| Physics 2D settings: Velocity / Position Iterations | the scene's `physics2DSubSteps` (Box2D 3 sub-steps) |

## 3D physics: Unity names and their JasperEngine counterparts

| Unity | JasperEngine |
|---|---|
| `drag` / `linearDamping`, `angularDrag` / `angularDamping` | `linearDamping`, `angularDamping` (0.05 by default, Jolt's, where Unity has 0) |
| `isKinematic` | `isKinematic` (Lua), the `type` `KINEMATIC`; switching in Play keeps the body |
| `mass`, `automaticCenterOfMass` / `centerOfMass`, `automaticInertiaTensor` / `inertiaTensor` / `inertiaTensorRotation` | the same names, plus `automaticMass` (off when a mass is set) |
| `maxLinearVelocity`, `maxAngularVelocity`, `interpolation` (`RigidbodyInterpolation`) | the same (`RigidbodyInterpolation.SCENE` follows the scene's setting) |
| `IsSleeping()`, `Sleep()`, `WakeUp()` | `isSleeping()`, `sleep()`, `wakeUp()` |
| `Collider.isTrigger` | `setColliderTrigger(i, b)` (per collider of the Rigidbody) |
| `PhysicMaterial` / `PhysicsMaterial` (`dynamicFriction`, `bounciness`, `frictionCombine`, `bounceCombine`) | `PhysicsMaterial` (`.physicsmaterial`, Create > Physics Material), one `friction`; Unity's `Average` friction combine of 3D is `average` |
| `collider.material` / `sharedMaterial` | `setColliderFriction` (an own copy) / `setColliderMaterial` |
| `HingeJoint`, `ConfigurableJoint`, `CharacterJoint`, `SpringJoint`, `FixedJoint` | one `Joint3D` component with `type` `HINGE`, `SIXDOF` (or `PRISMATIC` for a slider), `SWINGTWIST`, `DISTANCE` + `useSpring`, `FIXED` |
| `connectedBody`, `anchor`, `connectedAnchor`, `autoConfigureConnectedAnchor`, `axis`, `secondaryAxis`, `swingAxis` | `bodyB` (alias `connectedBody`), `anchor`, `anchorB` (alias `connectedAnchor`), `autoAnchors`, `axis`, `normal`, `planeAxis` (Lua aliases of the Unity names), with `anchorSpace = LOCAL` |
| `HingeJoint.limits / spring / motor / useLimits / useSpring / useMotor`, `angle`, `velocity` | `limitsMin/Max`, `springStiffness / springDamper / springTargetPosition`, `motorTargetVelocity / motorMaxForce`, `useLimits / useSpring / useMotor`, `angle`, `velocity` |
| `CharacterJoint.lowTwistLimit / highTwistLimit / swing1Limit / swing2Limit` | `twistMinAngle / twistMaxAngle / normalHalfConeAngle / planeHalfConeAngle` (Lua aliases of the Unity names) |
| `ConfigurableJoint.xMotion ... angularZMotion` (`ConfigurableJointMotion`), drives, `targetPosition` / `targetRotation` | the same names (`JointMotion.LOCKED` / `LIMITED` / `FREE`), `useDrive` + stiffness / damping |
| `enableCollision`, `breakForce`, `breakTorque`, `OnJointBreak(float)` | the same, `breakAction`, `onJointBreak(self, force, joint)` (C++ `onJointBreak(float, Joint3D&)`) |
| Ragdoll Wizard (GameObject > 3D Object > Ragdoll...) | the same menu, `create_ragdoll` (MCP), the `Ragdoll` component (`simulated`) |
| Physics Debugger, `Physics.autoSyncTransforms` | the Game view's Physics toggle, `Physics.debugDraw`; transforms always sync |

In scene and prefab files the component keys follow the class names, e.g.
`rigidbody2d:`, `audiosource:`, `recttransform:`, `colliders:`, `prefabPath:`.
