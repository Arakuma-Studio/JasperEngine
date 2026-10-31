# Engine gaps found while porting JasperDev Arcade (Unity) to JasperEngine

Date: 2026-10-30/31. Source of truth for the `gaps/*` feature specs (see [LEDGER.md](LEDGER.md)).

The Unity game is <https://github.com/JasperRaccoon/Minigames> (Unity 2022.3, Built-in RP, Android/iOS,
landscape). In-scope scenes: Menu, A (Aqua Motor), B (BACK-MAN), C (Jasper Kart), D (Horror Rooms),
E (Event Horizon), G (Grow a Garden), H (hill climb), I (Invaders). Out of scope: TeamWater, J, T, F.
A local clone is at `/tmp/gamesrc/Minigames` (re-clone with `git clone --depth 1` if missing).
Paths below: `S/` = `Assets/Scripts/`, `M/` = `Assets/Scripts/Minigames/`.

Engine paths are relative to the repo root. Everything here was read from code (nothing was run) unless
stated otherwise. Each item lists what the game does, what the engine has, and the gap.

---

## 1. Networking, URL opening, native code bridge (ads/IAP themselves are NOT in scope yet)

Game:
- `UnityWebRequest` wrapper `S/Network/RequestServerInfo.cs` (GET with timeout; accept-all cert handler on Android <= 7).
  Endpoints: `gamedata.arakuma.io/{GameTag}/AdData.csv` (remote config CSV), `.../furdata/ecpm_report.json`,
  `backend.arakuma.io/Now` (server time JSON, Garden restock timer `GShopManager:41`, `PremiumConcurrentCoins:59`),
  geo-IP JSON chain (freeipapi.com, ipwho.is, ipapi.co, api.country.is; `RequestUserCountry:57-135`),
  `HEAD https://www.google.com` (connectivity, `InternetChecker:22`), `POST backend.arakuma.io/Interact`
  (JSON analytics, `ArakumaAnalytics:10,76`), binary downloads of mp4/PNG to `persistentDataPath`
  (`RequestMyAds.cs:104`, `RequestMyIcons.cs:75`, then `Texture2D.LoadImage` + `Sprite.Create`).
- `Application.OpenURL` x8 (store page, house-ad links, manage subscription, `CheckVersion:52-54`).
- Native plugins: Wortise ads, RevenueCat, iOS ATT, PreciseLocale (`GetRegion()`), `AndroidJavaClass`
  versionCode lookup (`Plugins/GetMobileVersionCode`), `SystemInfo.operatingSystem`.

Engine:
- No HTTP client anywhere in `engine/core` or `engine/platform` (no curl, emscripten_fetch, HttpURLConnection,
  NSURLSession outside the AdMob SDK). `docs/mcp/guide/html-ui.md:1533` says "no network at run time".
  Android export can add the INTERNET permission (`editor/Exporter.cpp:302`).
- Lua state is single-threaded and enforced (`engine/core/script/LuaBinding.cpp:131-165`), so async results
  must be delivered on the main thread.
- Runtime texture from file works (`Texture(path)`, `TextureData:loadTextureFromFile`, `data://` paths).
- `Application.OpenURL` missing; HTML UI even warns "System:openURL is not available"
  (`engine/core/htmlui/compat/html/transforms/AttributeTransform.cpp:256`).
- Native: JNI calls hand-wired per feature in `engine/platform/android/JasperEngineAndroid.cpp` (cached method
  IDs); `MainActivity.java:52-59` builds optional wrappers by reflection; AdMob interstitial only
  (`CoreClassesLua.cpp:1754-1758`, `AdMobWrapper.java`, `AdMobAdapter.m`); CrazyGames SDK on Web
  (`CoreClassesLua.cpp:1760-1766`). No Lua -> Java / ObjC / JS bridge, no export hooks for extra
  Gradle/CocoaPods dependencies.

## 2. Platform services and screen

Game: `targetFrameRate` 60 (`S/Misc/SimpleVsync.cs:40`, re-applied on sceneLoaded), orientation forced to
landscape at runtime via `Screen.autorotateTo*` + `Input.deviceOrientation` (`S/Misc/AutoRotate.cs`),
`Application.systemLanguage` (`S/Localization/Localization.cs:169`), `Application.Quit`,
`OnApplicationFocus` (`M/Misc/MinigameSongs.cs:99`), `OnApplicationPause` (app-open ad on resume,
`S/Ads/AdsManagement.cs:739`), `Screen.safeArea` rig with a banner-ad margin (`S/UI/CanvasHelper.cs:111-243`,
`androidRenderOutsideSafeArea: 1`).

Engine:
- Orientation: Android export setting only (`editor/Project.h:545-564`, `editor/Exporter.cpp:3095-3097`);
  iOS plist always allows LandscapeLeft/Right + Portrait (`engine/workspaces/xcode/ios/Info.plist:263-268`), no
  iOS setting (`editor/Project.h:531-543`); no runtime orientation API, no device-orientation query.
- No target frame rate / vsync API (Windows compile-time flag `JasperEngineWin.cpp:28`; iOS only
  `CADisableMinimumFrameDurationOnPhone`, `editor/Exporter.cpp:3434`).
- No system language API. `Engine.platform` exists. `System.quit()` exists (base warns unsupported,
  `engine/core/System.cpp:136-137`).
- `Engine.onPause/onResume` exist (`CoreClassesLua.cpp:763-764`); focus gained/lost on Android only sets a flag
  (`engine/platform/android/NativeEngine.cpp:1382-1389`), no `onFocus` event.
- **Safe area: missing.** Android draws behind the cutout (`MainActivity.java:37-39`, hides cutout `:46-47`) but
  never reads insets; iOS `ViewController.m` never reads `safeAreaInsets`; nothing exposed to Lua;
  HTML UI `env(safe-area-inset-*)` resolves to 0px (`engine/core/htmlui/compat/css/ValueNormalizer.cpp:592-595`);
  native UI anchors have no safe-area concept.
- No accelerometer/gyro (not used by the game; out of scope).

## 3. Scripting runtime

Game: 78 `StartCoroutine` (54 files), 47 `WaitForSeconds`, `WaitForSecondsRealtime` (4 files), `WaitUntil`
(11 files), `WaitWhile`, `WaitForEndOfFrame`, `IEnumerator Start()`, `StopAllCoroutines`. No string `Invoke`
at all. `LeanTween.delayedCall` x8 (scaled time). `OnEnable` x40, `OnDisable` x18, `OnDestroy` x14.
`FindObjectOfType`/`FindObjectsOfType`, ~61 singleton classes, `GameObject.Find`, `FindGameObjectsWithTag`.
UnityEvent in 8 scripts (inspector-wired ~61-74 calls per scene). ScriptableObjects (MilkShake presets, Garden
items). Newtonsoft JSON (5 files). PlayerPrefs int/string with `HasKey`, `DeleteKey`, `Save`.
`Time.unscaledDeltaTime`. `Input.GetAxis/GetAxisRaw("Horizontal"/"Vertical")` summed with the joystick,
`GetKeyDown/GetKeyUp/GetButtonDown`. DontDestroyOnLoad singletons: ads manager, idle-interstitial timer, IAP
listener (with its own canvases), VsyncManager, VolumeManager, analytics, debug console, LeanTween.

Engine (`docs/mcp/guide/lua-api.md` is the Lua reference):
- Lifecycle: `init()` (third pass after all instances exist), `onUpdate`/`onFixedUpdate` via
  `RegisterEngineEvent` (`CoreClassesLua.cpp:760-761`), `onPostUpdate` global (LateUpdate-ish), `onDestroy` on
  destroy but NOT on scene unload (lua-api.md:76). **No OnEnable/OnDisable.** `ScriptComponent`/`ScriptEntry.enabled`
  not bound. **No Lua-side event unregister** (lua-api.md:134-135).
- **Coroutines:** `luaL_openlibs` loads `coroutine` (`LuaBinding.cpp:638`, Lua 5.4.7) but there is no engine
  scheduler; event closures are called via `lua_pcall` from C, so yielding from `onUpdate` fails. Callbacks store
  the `lua_State*` they were registered from (`LuaFunctionBase.cpp:29-37`, `HtmlUIAnimeLua.cpp:54-56`):
  registering from inside a coroutine stores the coroutine state (dangling later). Should normalise to
  `LUA_RIDX_MAINTHREAD`.
- Timers: `anime.createTimer{duration=ms, loop, onComplete, onLoop}` -> `AnimeTimer` with `:cancel()/:pause()`
  (`HtmlUIAnimeLua.cpp:507-519,1232-1267,1320`), ticked by each scene's UIDocumentSystem, UI clock = unscaled
  (`Engine.cpp:1357-1359`); timers survive scene loads until shutdown (`Engine.cpp:1575`). lua-api.md:569 wrongly says
  no timer API. `GameObject.destroy(obj, delay)` exists. Tweens: `onStop` as onComplete.
- Time: `Engine.deltatime` (scaled), `Engine.timeScale` (scales physics, actions, animations, particles; not audio,
  not HTML UI), `Engine.updateTime` = fixed timestep (name trap: `Engine.fixedDeltatime` is something else),
  `Engine.gameTime`, `Engine.frameCount`, `Engine.systemTime`. **No unscaled delta binding.**
- Objects: `GameObject.instantiate/destroy`, `scene:findEntity` (one scene), `findByTag/findAllByTag`,
  `LayerMask.*`. **No getComponent<Script>/FindObjectOfType** (lua-api.md:1495). `FunctionSubscribe` can't be
  constructed from Lua (`CoreClassesLua.cpp:938-942`). No ScriptableObject-like data asset.
- Persistence: `UserSettings.get/set*ForKey`, `removeKey` (`IOClassesLua.cpp:65-95`); **no hasKey/deleteAll/save**.
  `System.getUserDataPath()`, `File`, Lua `io`. **No JSON binding** (nlohmann in `engine/libs/json`).
- Scenes: `SceneManager.loadScene` (deferred), preload/progress/loading scene, `addChildScene/removeChildScene`,
  `Engine.onSceneLoaded`. `loadScene` calls `Engine::removeAllScenes()` (`engine/core/manager/SceneManager.cpp:125`);
  generated `load_X()` deletes non-running scenes (`editor/Generator.cpp:2207-2256`); editor Play uses another factory
  (`editor/Project.cpp:8834`). Sounds stop only in `~Scene` (`Scene.cpp:67`). **No DontDestroyOnLoad.** The Lua
  state is created once (`Engine.cpp:1186`) so `require`d modules survive loads.
- Input: `Input.numTouches/getTouchPosition/findTouchIndex`, `onTouchStart/Move/End/Cancel(pointer,x,y)`
  (`CoreClassesLua.cpp:1712-1722,768-771`); `Touch` (`Input.h:194`) not bound, no phases/deltaPosition;
  `isKeyPressed` only (no down/up edge queries), `onKeyDown/onKeyUp` events; gamepad buttons/axes exist
  (`CoreClassesLua.cpp:1715-1720`); **no virtual axes / input actions**; no on-screen joystick.
- Math: `Vector3::lerp/moveTowards` exist in C++ but unbound; no SmoothDamp/Angle/ProjectOnPlane/FromToRotation.

## 4. Physics

Game: Rigidbody velocity control everywhere (no CharacterController, no WheelCollider, no RCC, no NavMesh).
C kart: arcade raycast controller, gravity off + manual gravity, non-convex static MeshCollider track.
H hill climb: `WheelJoint2D` x2 (suspension frequency 3, damping 0.35, angle 90, motor disabled) in
`Prefabs/H - Hill Climb Racing/PlayerCar.prefab`, wheels driven by `AddTorque`, tire friction 2,
`PolygonCollider2D` terrain made of two leapfrogged FBX meshes (`M/Hill/InfiniteTerrainAccomodator.cs`).
B ragdoll: 11 kinematic interpolated bodies + 10 CharacterJoints (`Prefabs/B- Backrooms/Entity.prefab`), enabled
by `RagdollManager` (disables Animator, `isKinematic=false`). Contacts: OnCollisionEnter/Stay/Exit, OnTriggerEnter/
Exit/Stay, OnCollisionEnter2D, OnTriggerEnter2D. Layer matrix used. Raycast x29 with masks.

Engine (abbrev: `GOL` = `engine/core/script/binding/GameObjectClassesLua.cpp`, `ECSL` = `.../ECSClassesLua.cpp`,
`CCL` = `.../CoreClassesLua.cpp`, `PS` = `engine/core/subsystem/PhysicsSystem.cpp`):
- 3D body: type, velocities, forces/impulses, mass (live only, lost on reload `Rigidbody.h:111-112`), gravityFactor,
  `setAllowedDOFs`, CCD, sleeping, scene-wide interpolation. **Missing: linear/angular damping, set centre of mass,
  safe static->dynamic switch** (bodies created without `mAllowDynamicOrKinematic`, `PS:1513`; `setType` calls
  `SetMotionType` directly `Rigidbody.cpp:397`; Jolt asserts), per-body interpolation, isSleeping.
- 3D colliders: box/sphere/capsule/tapered/cylinder/convex/mesh/heightfield, up to 10 per body. **Sensor flag and
  friction/restitution per body, not per collider; colliders can't be removed/resized from Lua; Lua-built vertex
  lists capped at 256 vertices/768 indices in exports only** (`engine/core/Engine.h:137-145`, sized from authored
  scenes at `editor/Exporter.cpp:2921-2925`). 3D world capacity hard-coded 1024 bodies/pairs/contacts (`PS:235-238`).
- Contacts: **global scene-wide events** (`onContactAdded3D/Persisted3D/Removed3D`, `shouldCollide3D`,
  `ECSL:228-233`; 2D `beginContact2D/endContact2D/beginSensorContact2D/endSensorContact2D/hitContact2D/preSolve2D`,
  `ECSL:218-225`) - every script gets every pair (`docs/mcp/guide/physics.md:145`). **3D Lua handlers are invoked
  on Jolt job threads** (`engine/core/util/JoltPhysicsAux.h:325-338`, pool with cores-1 workers
  `engine/core/physics/JoltShapeBuilder.cpp:300-308`) under a recursive mutex; the Lua thread checker flags it.
  Contact3D holds raw Jolt pointers (callback-only). No impulse/point count; removed event has no contact.
  **2D contact events off by default** (`Collider2D.contactEvents=false`, `Rigidbody2DComponent.h:53`); no 2D stay.
- Queries: `Physics.raycast/raycastAll` (+2D), masks (`CCL:554-571`). No overlap/shape casts (not needed by game).
- Filtering: layer collision matrix per backend (`engine/core/registry/PhysicsLayers.h`). OK.
- Materials: 3D per body, fixed combine (geometric mean friction, max restitution); 2D per collider.
- 2D: Box2D 3.0, body API incl. damping. Colliders box/polygon/circle/capsule/segment/chain. **Chain capped at 16
  points (`MAX_COLLIDER_POINTS_2D`, Engine.h:133, uint8 count), 10 colliders/body, enforced only in exports.**
  Joints distance/revolute/prismatic/mouse/wheel/weld/motor (`GOL:2115-2127`). **Wheel joint uses
  `b2DefaultWheelJointDef` unchanged** (`PS:2061`: 1 Hz spring, 0.7 damping, motor off); no setters for spring,
  motor, limits, break force on any 2D joint; `getBox2DJoint` unbound. Sub-steps hard-coded 4 (`PS:3036`).
- 3D joints: fixed/distance/point/hinge/cone/prismatic/swing-twist/six-DOF/path/gear/rack/pulley (`GOL:2244-2261`).
  **Hinge has no limits or motor (`PS:2320-2324`); cone joint leaves `mHalfConeAngle` 0 (`PS:2370-2385`) = no
  swing; no motors/springs/break force; no Ragdoll** (Jolt `Ragdoll` compiled, unused). Unverified: skinning follows
  physics-moved bone entities.
- No runtime collider debug draw (`PS:3205` empty).

## 5. Animation

Game: Animator controllers: EntityAnim (B, bool), GranjeroAnim (G: **2 layers with an AvatarMask** "Holding",
float Speed, bool Watering), MikoGranjAnim (trigger), Astronaut, JasperTexting. Humanoid avatars with retargeted
clips (Menu, B, G). Legacy `Animation` (16 of 22 clips) on UI/menu objects (`GameOverPanel:54,108`) with curves on
transform, **`m_IsActive` and `m_Color`**, one clip with events.

Engine: `AnimationPlayer` (legacy-Animation-like: play/crossFade/blend/queue/stop, wrap modes, events into Lua,
`docs/mcp/guide/animation.md:221-256`, `GOL:1826-1865`); clips `.anim` and `model.gltf#Clip`; bind by child path.
**No layers / bone masks / state machine / root motion. No retargeting between different rigs** (PoseAnimator
is rig-independent but uses its own pose format). **Clip `Property` channel reserved, never sampled**
(`engine/core/animation/ClipTypes.h:30`): clips can't animate active state, color, material or UI properties.

## 6. Audio

Game: `Audio/Mixer.mixer` groups Master/MUSIC/SFX + Lowpass/Highpass effects; exposed params MusicVolume,
SFXVolume, MASTERVOLUME, lowPassCutoff, highPassCutoff; mute = -80 dB (`VolumeController:43`); menu<->gameplay
filter sweeps (`MinigameSongs:124-147`). `PlayOneShot`, pitch modulation, 3D sources in C and D. 58 MB audio.

Engine: SoLoud. play/pause/stop/loop/volume/pan/seek/callbacks, pitch via `speed`, 3D (`AudioSystem.cpp:259-415`).
**No buses (no SoLoud::Bus), no filters exposed, only `AudioSystem.globalVolume`. No streaming: every file decoded
with `Wav::loadMem` (`engine/core/pool/SoundPool.cpp:156`). One voice handle per source: replaying orphans the old
voice (`AudioSystem.cpp:345-356`) - no PlayOneShot. No fades** (SoLoud `fadeVolume` unexposed). Pause on app pause OK.

## 7. Rendering

Game: TrailRenderer (Invaders `Bullet.prefab`), 2-point LineRenderer lightning (B `BRLightningCannon:37-62`),
custom 4-light vertex lighting via `Shader.SetGlobalColor/Vector` (`S/VertexLighting/VertexLightsUpdater.cs:59-77`,
used in Menu/A/D/G/H/I), Amplify shaders, runtime keyword toggles, `RenderSettings.skybox` swap + rotation (H),
VideoPlayer playing a downloaded **mp4** into a RenderTexture (`S/Ads/FakeAdsManager.cs:44-46`).
18 particle systems: shapes sphere/hemisphere/cone/circle/rectangle, bursts, color/size/rotation/velocity over
lifetime, clamp velocity, force, texture sheet, mesh particles; no sub-emitters/collision/trails used.

Engine: **LineRenderer has no width** (raw `LINES`, `RenderSystem.cpp:3395`); **no TrailRenderer**. **No user
global shader uniforms** (only the engine frame block, `engine/shaderlib/includes/frame.glsl:12-27`).
Keyword toggles OK (`setShaderFeature`, `CCL:1258,1350`). Video: HTML `<video>` WebM (AV1+Opus via dav1d) only,
local files only (`docs/html-ui/phase-6-video.md`). Particles (`engine/core/component/ParticleSystemComponent.h`,
`engine/core/subsystem/ActionSystem.cpp`): shapes OK, bursts OK, **velocity is a min/max box, not speed along the
shape direction** (`ActionSystem.cpp:926-928`); **over-lifetime modifiers are single from/to segments except
color gradient**; no duration field; **no Unity ParticleSystem importer**.

## 8. UI

Game: all canvases Screen Space Overlay, ScaleWithScreenSize 1920x1080 match 1 (CanvasScaleAdjuster changes match
on narrow screens); TMP with outline/underlay presets, auto-size, rich text, **TextMesh Pro Effect "Waves"**
per-vertex animation (Menu titles, paywall); **Coffee UIParticle** (particles rendered inside UI, GameOverMenu);
Floating joystick + `TouchArea` camera look at the same time (B, D, E, G), joystick + fire button (I);
`EventTrigger` PointerDown; drag & drop inventory (G).

Engine: native UI toolkit + HTML/CSS UI (RmlUi). `Engine.scalingMode` FITWIDTH/FITHEIGHT/LETTERBOX/CROP/STRETCH/
NATIVE (`engine/core/Engine.h:175-182`) - **no match-width-or-height blend**. **No pointer id in either UI**: native
`onPointerDown/Up/Move/onDrag*` pass only (x,y), UISystem handles one pointer (`UISystem.h:155-157`), touches
forwarded without id (`Engine.cpp:1905`); HTML UI has no pointer*/touch* events/pointer id (`object/ui/UIEvent.h:40`)
and lifting any finger clears `:active` everywhere (`docs/mcp/guide/html-ui.md:1329-1333`). No per-character text
animation effect; no particles inside UI.

## 9. Debug / camera

Game: IngameDebugConsole (remote-enabled), Graphy FPS monitor, MilkShake `Shaker` + ShakePreset assets
(Menu, A, B, D, E, H, I via `S/Misc/CameraShaker.cs`).
Engine: **no on-device log console** (`Log` has no listener/ring buffer, `engine/core/Log.h:11-18`), stats overlay is
editor-only (`htmlui/HtmlUIRenderStats.h:13`); `Engine.framerate` readable. **No camera shake** component.

## 10. Device-only failures (editor vs export)

Collider caps (2D chain 16 points, 10 colliders/body; 3D Lua-built 256 vertices/768 indices) are enforced only in
exported builds, and the 3D world capacity is fixed at 1024 - things that work in the editor break on device.

## Not needed by this game (not in scope)

Character controller, vehicles, navmesh, overlap/shape casts, procedural mesh/texture pixel API, particle
sub-emitters/collision/trails, lightmaps, Timeline, accelerometer, Animator state-machine graph editor.
