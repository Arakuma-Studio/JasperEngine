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
| `GameObject.destroy(object [, delay])`, `GameObject.destroyImmediate(object)` (Unity's `Object.Destroy`, `DestroyImmediate`; scripts get `onDisable`, then `onDestroy`) | `BundleManager.destroyBundle`, then `PrefabManager.destroyPrefab` (removed) |
| MonoBehaviour messages: `Awake` -> `init`, `Start` -> `start`, `OnEnable` -> `onEnable`, `OnDisable` -> `onDisable`, `OnDestroy` -> `onDestroy`, `OnApplicationPause/Focus/Quit` -> `onApplicationPause/Focus/Quit` (Lua methods called by name; C++ `ScriptBase` overrides), Unity's rules and order ([lua-api.md](mcp/guide/lua-api.md) 1.1) | `init` (enabled scripts only, at load) and `onDestroy` (runtime destroy only); the others none (new) |
| `Update` / `FixedUpdate` / `LateUpdate` -> `onUpdate` / `onFixedUpdate` / `onPostUpdate` registered with `RegisterEngineEvent(self, "onUpdate")` (C++ `REGISTER_ENGINE_EVENT`) | unchanged |
| `MonoBehaviour.enabled` -> `self.enabled` (Lua, rw: fires `onEnable`/`onDisable`), `isEnabled()` / `setEnabled(bool)` (C++); `isActiveAndEnabled` -> `self.isActiveAndEnabled` / `isActiveAndEnabled()`; `gameObject` -> `self.gameObject` | none (new) |
| `GetComponent<Enemy>()`, `GetComponents`, `GetComponentInChildren`, `GetComponentsInChildren`, `GetComponentInParent` (for scripts) -> `go:getScript("Enemy")`, `getScripts`, `getScriptInChildren`, `getScriptsInChildren`, `getScriptInParent` (also on `self`); C++ `getScript<T>()` | none (new): a script property typed with the script's class |
| `GameObject.Find`, `GameObject.FindWithTag`, `FindGameObjectsWithTag`, `Object.FindObjectOfType<T>()`, `FindObjectsOfType<T>(includeInactive)` -> `GameObject.find`, `findWithTag`, `findGameObjectsWithTag`, `findObjectOfType("T")`, `findObjectsOfType("T", includeInactive)` (every running scene) | `scene:findEntity`, `scene:findByTag` (one scene) |
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

## Audio

The project has one Audio Mixer asset (`.mixer`, JSON), chosen in Project Settings > Audio and
edited in Window > Audio > Audio Mixer or with MCP `audio_mixer`. Without one every sound plays
through an implicit Master at 0 dB. See [mcp/guide/audio.md](mcp/guide/audio.md).

| Unity term | JasperEngine name |
|---|---|
| `AudioMixer` (asset) | `.mixer` asset (Assets > Create > Audio Mixer); Lua `AudioMixer` (static: the project's mixer) |
| `AudioMixerGroup`, `AudioSource.outputAudioMixerGroup` | mixer group; `AudioSource.outputGroup` (the group's name, "" = Master); Lua `AudioMixer.getGroup(name)` -> `AudioMixerGroup` |
| `AudioMixer.SetFloat` / `GetFloat` / `ClearFloat`, exposed parameters | `AudioMixer.setFloat(name, value [, seconds [, opts]])` / `getFloat(name)` / `clearFloat(name)`; expose in the Audio Mixer window (right-click a fader or a value) or `audio_mixer` op `expose`; "Remember" keeps script values in UserSettings |
| `AudioMixerSnapshot.TransitionTo`, `AudioMixer.TransitionToSnapshots`, `FindSnapshot` | `AudioMixer.transitionTo(name, seconds [, opts])`, `AudioMixer.transitionToSnapshots(names, weights, seconds)`, snapshots by name |
| Audio Mixer update mode Normal / Unscaled Time | mixer `updateMode` "scaled" / "unscaled"; per call `{unscaled = true}` |
| Lowpass / Highpass / Echo / SFX Reverb / ParamEQ / Distortion / Flange / Duck Volume effects | mixer effects `lowpass`, `highpass`, `bandpass`, `echo`, `reverb`, `equalizer`, `distortion`, `flanger`, `lofi`, `bassboost`, `duck`, `limiter` (up to 8 per group) |
| `AudioSource.PlayOneShot(clip, volumeScale)` | `sound:playOneShot(path [, volumeScale])` (up to `maxVoices` overlapping voices) |
| `AudioSource.PlayClipAtPoint(clip, position, volume)` | `AudioSource.playClipAtPoint(scene, path, position [, volume [, group]])` |
| AudioClip Load Type Decompress On Load / Compressed In Memory / Streaming | `AudioSource.loadType` (`AudioLoadType.DecompressOnLoad` / `CompressedInMemory` / `Streaming`, default `Auto`: streams files of 1 MiB and more): per source, no clip import settings |
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

In scene and prefab files the component keys follow the class names, e.g.
`rigidbody2d:`, `audiosource:`, `recttransform:`, `colliders:`, `prefabPath:`.
