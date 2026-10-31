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
| Project Settings > Input Manager (virtual axes and buttons; the Input System's actions) | none (new): Project Settings > Input, the input actions (see [Input](#input) below) |
| Window menu | View menu |
| GameObject, Component, Assets menus | Scene and Project menus, the Structure "+" menu |
| Animation Clip (`.anim` asset; Assets > Create > Animation Clip) | none (new): keyframe animations lived only inside a scene or prefab as an Animation entity. A JasperEngine clip is JSON (`{"format": "jasperengine.anim"}`, not Unity's YAML), binds tracks by path relative to the GameObject that plays it, and is referenced by GUID; see [animation-clips.md](animation-clips.md) |
| Animation window with an Animation Clip (Preview, Record, Dopesheet / Curves, Add Property, events) | Animation window (entity Animations only). It now edits clips when the selected GameObject or an ancestor has an Animation Player, or when a clip is opened from the Project window or the Inspector; entity Animations still open in its older mode |
| A model's imported clips (the Animation tab of a model's Import Settings; sub-assets such as `Take 001`) | none (new): model clips, read-only, named `<model>#<animation>` (`hero.gltf#Walk`; `"Wave 2"` for a second animation named Wave), with **Extract to .anim** for an editable copy and the Model section's **Use Animation Player** |
| none: Unity has no pose-to-pose animation (the closest is keyframing a humanoid clip in the Animation window) | none (new): Pose Animation (Window > Animation > Pose Animation; `.poseanim` assets and templates), played by the Pose Animator or baked to an Animation Clip or an Animation; see [pose-animation.md](pose-animation.md) |
| Avatar (Rig > Animation Type Humanoid, Configure Avatar) | none (new): the humanoid map, bones mapped to body parts from their names; fixes in the Pose Animation window's Map Bones... popup, stored in a `<model>.humanoid` file next to the model |
| none: Unity has no vertex animation textures built in (they come from Asset Store packages or Houdini's exporter, played by a custom material) | none (new): Vertex Animation (a `.vat` asset and its generated `.vat.glb`; Window > Animation > Vertex Animation Baker, the `.vat` Inspector's Rebake and Create Crowd...), baked from model clips, Animation Clips, Animation Players, pose animations and lattices, or imported from Houdini Labs VAT 3 and OpenVAT exports; see [vertex-animation.md](vertex-animation.md) |

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
| `RectTransform` / `RectTransformComponent` | `UILayout` / `UILayoutComponent` |
| `LayoutGroup` / `LayoutGroupComponent` | `Container` / `UIContainerComponent` |
| `InputField` / `InputFieldComponent` | `TextEdit` / `TextEditComponent` |
| `Tween` / `TweenComponent` | `TimedAction` / `TimedActionComponent` |
| `PositionTween`, `RotationTween`, `ScaleTween`, `ColorTween`, `AlphaTween` | `PositionAction`, `RotationAction`, `ScaleAction`, `ColorAction`, `AlphaAction` |
| `GameObject.instantiate` / `GameObject::instantiate` (Unity's `Object.Instantiate`: a prefab or a copy of a GameObject, optional parent and `instantiateInWorldSpace`, or position and rotation) | `BundleManager.createBundle`, then `PrefabManager.createPrefab` (removed) |
| `GameObject.destroy(object [, delay])`, `GameObject.destroyImmediate(object)` (Unity's `Object.Destroy`, `DestroyImmediate`; Lua scripts get `onDestroy`) | `BundleManager.destroyBundle`, then `PrefabManager.destroyPrefab` (removed) |
| `PrefabAsset` (what a Prefab script property holds; `PrefabAsset.load("path")`) | none (new) |
| `PrefabManager` (the build's prefab registry: `findPrefab`, `getPrefabNames`, `prefabCount`) | `BundleManager` |
| `PrefabComponent` | `BundleComponent` |
| `GameObject:getRigidbody2D()`, `getRigidbody()` | `Object:getBody2D()`, `getBody3D()` |
| `AudioSource:getGameObject()` | `Sound:getObject()` |
| `Collider2DType`, `Collider3DType`, `RigidbodyMotionQuality` | `Shape2DType`, `Shape3DType`, `Body3DMotionQuality` |
| `AudioSourceState`, `AudioSourceAttenuation` | `SoundState`, `SoundAttenuation` |
| `LayoutGroupType`, `ShadowCaster2DShape` | `ContainerType`, `Occluder2DShape` |
| Legacy `Animation` component (`Play`, `CrossFade`, `Blend`, `PlayQueued`, `AnimationState`, Animation Events) | none (new): `AnimationPlayer` / `AnimationPlayerComponent` (Add Component > Animation > Animation Player; `gameObject:getAnimationPlayer()`, nil without one): `play`, `crossFade`, `blend`, `queue`, `getState` (an `AnimationState`), `onEvent` (one `AnimationEvent`), `onFinish` |
| none: the old JasperEngine name, kept | `Animation` / `AnimationComponent` (Add Component > Animation > Animation) is not Unity's Animation component: it is JasperEngine's entity sequence of actions (keyframe tracks targeting scene GameObjects, tweens, sprite animations, particles, nested animations). Unity's legacy Animation component is the Animation Player |
| none: Unity has no live pose-to-pose player (the closest is an Animator with a humanoid Avatar, its `Animator.speed` and layer weight) | none (new): `PoseAnimator` / `PoseAnimatorComponent` (Add Component > Animation > Pose Animator; `model:getPoseAnimator()`) plays a `.poseanim` pose animation live on a humanoid Model |
| none: Unity has no built-in vertex animation player | none (new): `VertexAnimator` / `VertexAnimatorComponent` (Add Component > Animation > Vertex Animator; `gameObject:getVertexAnimator()`, nil without one) plays a `.vat` on the GameObject and on each instance of its Instanced Mesh: `play`, `crossFade`, `blend`, `queue`, `playInstance`, `crossFadeInstance`, `playAll`, `getSocketPose`, `onEvent`, `onFinish`, `onInstanceEvent` |
| `Input.GetAxis`, `GetAxisRaw`, `GetButton`, `GetButtonDown`, `GetButtonUp` (Input Manager axes) | none (new): `Input.getAxis`, `getAxisRaw`, `getButton`, `getButtonDown`, `getButtonUp`, plus `Input.getVector` (Godot's `get_vector`) and `Engine.onButtonDown` / `onButtonUp` |
| `Input.GetKey`, `GetKeyDown`, `GetKeyUp`, `GetMouseButton`, `GetMouseButtonDown`, `GetMouseButtonUp`, `anyKey`, `anyKeyDown`, `inputString`, `mouseScrollDelta` | `Input.isKeyPressed` (held only; kept); new: `Input.getKey`, `getKeyDown`, `getKeyUp`, `getMouseButton`, `getMouseButtonDown`, `getMouseButtonUp`, `anyKey`, `anyKeyDown`, `inputString`, `mouseScrollDelta`, `mouseDelta` |
| `KeyCode.Space`, `KeyCode.LeftControl`, `KeyCode.Mouse0`, `KeyCode.JoystickButton0` | `Input.KEY_SPACE`, `Input.KEY_LEFT_CONTROL`, `Input.MOUSE_BUTTON_LEFT`, `Input.GAMEPAD_BUTTON_A`; as control names (bindings, `Input.getKeyDown("...")`): `"space"`, `"left_control"`, `"mouse_left"`, `"gamepad_a"` |
| `Touch` (`fingerId`, `phase`, `position`, `deltaPosition`, `deltaTime`, `tapCount`), `TouchPhase`, `Input.touches`, `touchCount`, `GetTouch`, `multiTouchEnabled`, `touchSupported` | `Input.getTouches()` returned `{pointer, position}` tables; now `Touch` (same fields plus `startPosition`; `pointer` kept as the old name of `fingerId`), `TouchPhase`, `Input.touches`, `touchCount`, `getTouch`, `getTouchById`, `multiTouchEnabled`, `touchSupported` |
| `Input.simulateMouseWithTouches` | `Engine.callMouseInTouchEvent` |
| Input System: on-screen controls, `InputAction`, rebinding (`PerformInteractiveRebinding`, `SaveBindingOverridesAsJson`, `LoadBindingOverridesFromJson`) | none (new): virtual sources (`Input.simulateButton`, `simulateAxis`, `addAxisDelta`, `InputActions::setSourceValue` in C++), `Input.getPressedControl`, `Input.setBindings`, `exportBindings`, `importBindings` |
| `Physics` / `Physics2D` (`ignoreLayerCollision`, `getIgnoreLayerCollision`, `getLayerCollisionMask`, `setLayerCollisionMask`, `raycast`, `raycastAll`, `ALL_LAYERS`, `DEFAULT_RAYCAST_LAYERS`, `IGNORE_RAYCAST_LAYER`) | none (new) |

Unchanged: `Entity`, `Scene`, `Transform`, `Camera`, `Light`, `Light2D`, `Mesh`,
`Model`, `Sprite`, `Tilemap`, `Terrain`, `Fog`, `Mirror`, `ReflectionProbe`,
`Bone`, `Points`, `Image`, `Text`, `Button`, `Panel`, `Scrollbar`, `Progressbar`,
`Polygon`, `MeshPolygon`, `InstancedMesh`, `Action`, `Animation`,
`SpriteAnimation`, the keyframe tracks, `Joint2D`/`Joint3D`, `Contact2D`/`Contact3D`
and `CollideShapeResult3D`.

New, with no Unity counterpart: `FluidSimulation` / `FluidSimulationComponent`
(GameObject > Effects > Fluid Simulation, Pavel Dobryakov's WebGL fluid). Unity has no
built-in fluid component; there it takes a Custom Render Texture or compute-shader
solver of your own, or a package.

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

## Input

Project Settings > Input is Unity's Input Manager and the Input System's action list in one:
named actions with bindings (Key or Mouse Button, Gamepad Axis, Mouse Movement) and Unity's
knobs (gravity, sensitivity, dead, snap, invert), the same 11 defaults (`Horizontal`,
`Vertical`, `Fire1`..`Fire3`, `Jump`, `Mouse X`, `Mouse Y`, `Mouse ScrollWheel`, `Submit`,
`Cancel`), saved in `project.yaml` under `input` once edited. Differences: several bindings of an
action add up and clamp (Unity takes the largest same-named entry); the joystick dead zone is
rescaled (Unity clips); `getButton` on an axis is true for both sides; an unknown name logs once
and reads 0 (Unity throws). On-screen sticks and buttons feed the same actions, so
`Input.getAxis("Horizontal")` sums keyboard, gamepad and the touch stick. See
[mcp/guide/input.md](mcp/guide/input.md).

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

In scene and prefab files the component keys follow the class names, e.g.
`rigidbody2d:`, `audiosource:`, `recttransform:`, `colliders:`, `prefabPath:`.
