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
