# C++ scripting

How to write C++ gameplay scripts for a JasperEngine project. Served as
`jasper://docs/cpp-scripting`. The runtime API of a project is the header snapshot in
`<project>/.jasperengine/engine-api/` (copied from the editor at open/build): it is the only
source of truth for class and method names. Never carry over names from Unity C#, Unreal or
Godot.

## Two kinds of script

| Kind | Base class | Use it for |
|---|---|---|
| Script class | `jasperengine::ScriptBase` | general logic: game controllers, spawners, input |
| GameObject subclass | the GameObject's own class: `Mesh`, `Primitive`, `Sprite`, `Model`, `Camera`, `GameObject`, ... | calling that class's methods on `this` (`setColor`, `setPosition`, `getPosition`) |

`ScriptBase` has `getScene()`, `getEntity()`, `isActiveAndEnabled()`, `isEnabled()` /
`setEnabled(bool)` (Unity's `enabled`: calls `onEnable`/`onDisable`), `getScript<T>()` (the first
C++ script of this GameObject that is a `T`, enabled or not, else `nullptr`; Unity's
`GetComponent<T>()`) and the lifecycle virtuals below; it does NOT have mesh or transform
methods. Wrap other GameObjects explicitly (`GameObject(scene, entity)`, `Sprite(scene, entity)`).
Scene-wide finds are Lua-only (`GameObject.findObjectOfType`); in C++ walk
`Engine::getSceneLayers()`.

No reflection or declaration macro is needed (no `GENERATED_BODY`, `GDCLASS`, `Q_OBJECT`,
`D_OBJECT`): inherit the base class and declare a `(jasperengine::Scene*, jasperengine::Entity)`
constructor. Prefer editing the skeleton the script creation tool generated.

## Skeleton (ScriptBase)

```cpp
// Spinner.h
#pragma once

#include "ScriptBase.h"
#include "Engine.h"
#include "ScriptProperty.h"

class Spinner : public jasperengine::ScriptBase {
public:
    DPROPERTY("Speed")
    float speed = 90.0f;

    DPROPERTY("Target")                       // a GameObject reference set in the Inspector
    jasperengine::GameObject* target = nullptr;

    DPROPERTY("Bullet")                       // a Prefab dragged from the Project window
    jasperengine::PrefabAsset bullet;

    Spinner(jasperengine::Scene* scene, jasperengine::Entity entity);
    virtual ~Spinner();

    void onUpdate();
};
```

```cpp
// Spinner.cpp
#include "Spinner.h"
#include "GameObject.h"
#include "Input.h"
#include "Log.h"

using namespace jasperengine;

Spinner::Spinner(Scene* scene, Entity entity): ScriptBase(scene, entity) {
    REGISTER_ENGINE_EVENT(onUpdate);
}

Spinner::~Spinner() {
    UNREGISTER_ENGINE_EVENT(onUpdate);
}

void Spinner::onUpdate() {
    float dt = Engine::getDeltatime();
    if (target) {
        Vector3 p = target->getPosition();
        target->setPosition(p + Vector3(speed * dt, 0, 0));
    }
    if (Input::isKeyPressed(D_KEY_SPACE) && bullet.isValid()) {
        GameObject go = GameObject::instantiate(bullet, Vector3(0, 5, 0), Quaternion());
        Log::print("spawned %s", go.getName().c_str());
    }
}
```

## Lifecycle

A `ScriptBase` script overrides the methods it needs; the engine calls only the overridden
ones, with the rules and order of Lua scripts (`jasper://docs/lua-api`, 1.1):

| Override | When | Unity |
|---|---|---|
| `void init() override` | once, the first time the GameObject is active in hierarchy, enabled or not; **after the DPROPERTY members are bound** | `Awake` |
| `void onEnable() override` | it becomes active and enabled (after `init`) | `OnEnable` |
| `void start() override` | once, at the start of the frame after the first `onEnable` | `Start` |
| `void onDisable() override` | it stops being active and enabled; before `onDestroy` | `OnDisable` |
| `void onDestroy() override` | once: destroy, scene unload, Play stop / quit (only if `init` ran); the destructor runs after it | `OnDestroy` |
| `void onApplicationPause(bool paused) override`, `onApplicationFocus(bool focused)`, `onApplicationQuit()` | app background / focus / quit | `OnApplication*` |
| `void onJointBreak2D(jasperengine::Joint2D& joint) override` | a 2D joint of this GameObject (or whose bodyA is it) broke | `OnJointBreak2D` |
| `void onParticleSystemStopped() override` | a Particle System of this GameObject with stop action Callback finished | `OnParticleSystemStopped` |

Overrides may be public, protected or private; do not call the base method: it only notes the
script does not override the message, which is then not sent to it again. The constructor runs
first, before the properties are bound: read DPROPERTY members in
`init()`, not in the constructor. Events registered with `REGISTER_ENGINE_EVENT` start after
`start()` (a script enabled during a frame gets its first `onUpdate` in the next one).
Migration: a method of an existing script named like one of these is now an override the engine
calls (twice if the script calls it itself too); one with another signature (`bool init()`) no
longer compiles: rename it. Every Script component entry gets its object, enabled or not
(disabled C++ scripts are compiled too). Exceptions thrown from these methods are logged and
the loop goes on. The GameObject-subclass kind (below) is an object wrapper, not a behaviour:
it gets no lifecycle calls.

```cpp
void Spinner::init() { if (!target) Log::warn("Spinner: no target"); }
void Spinner::onDisable() { Log::print("paused"); }
```

Persistent objects (Unity's `DontDestroyOnLoad`, rules in `jasper://docs/lua-api` 2.8.1):

```cpp
void AdsManager::init() {
    if (persistSingleton<AdsManager>() != this) return;   // a duplicate: its GameObject goes
}
void MusicPlayer::init() { dontDestroyOnLoad(); }           // the root GameObject survives loads
```

`isPersistent()`, `SceneManager::dontDestroyOnLoad(scene, root)` and
`SceneManager::moveGameObjectToScene(from, root, to)` do the same from anywhere. The move keeps
the object (its members, pointers to it stay valid); `getScene()` and `getEntity()` change with
it, and the wrappers its entity-reference properties made for the moved GameObjects follow.

A GameObject subclass looks the same with `public jasperengine::Primitive` (or `Mesh`,
`Sprite`...) as base, `#include "Primitive.h"`, and `Primitive(scene, entity)` in the
initializer list; it can then call `setColor(...)`, `getPosition()`, `setPosition(...)` directly.

## Properties: DPROPERTY

`DPROPERTY("Display Name")` before a public member exposes it in the Inspector. Supported
member types: `bool`, `int`, `float`, `std::string`, `Vector2`, `Vector3`, `Vector4`,
`PrefabAsset`, and pointers to engine wrapper classes or other script classes
(`GameObject*`, `Mesh*`, `Sprite*`, `MyOtherScript*`) for GameObject references. Colors:
`DPROPERTY("Tint", Color4) Vector4 tint = Vector4(1,1,1,1);` (or `Color3` with a `Vector3`).

## Events

| Macro (in the constructor) | Pair in the destructor | Calls |
|---|---|---|
| `REGISTER_ENGINE_EVENT(onUpdate)` | `UNREGISTER_ENGINE_EVENT(onUpdate)` | every frame |
| `REGISTER_ENGINE_EVENT(onFixedUpdate)` | `UNREGISTER_ENGINE_EVENT(onFixedUpdate)` | every physics step |
| `REGISTER_BUTTON_EVENT(onPress, method)` | `UNREGISTER_BUTTON_EVENT(onPress, method)` | Button events: `onClick` is the click (released over it, or a Submit while selected); `onPress`, `onRelease` (every release, also off the button) |
| `REGISTER_UI_EVENT(event, method)`, `REGISTER_SCROLLBAR_EVENT`, `REGISTER_PANEL_EVENT` | `UNREGISTER_...` | UI component events |
| `REGISTER_COMPONENT_EVENT(Component, event, method)` | `UNREGISTER_COMPONENT_EVENT(...)` | any component's event |

The method name must be the Engine event's name (`onUpdate`, `onFixedUpdate`,
`onPostUpdate`, `onKeyDown`, ...). Events of a disabled script (inactive GameObject or
disabled entry) are skipped. Registering in the constructor or in `init()` both work.

`onCharInput` passes one whole typed character: declare it `void onCharInput(char32_t character)`.
The older form `void onCharInput(wchar_t)` still compiles but is deprecated: on Windows `wchar_t` is
16 bits, so a character beyond U+FFFF (an emoji) arrives cut to its low 16 bits there.

## Timers and time (Invoke)

C++ scripts have no coroutines (C++20 `co_await` is not used: projects build with C++17 too);
the Lua game's coroutines port to timers or to state in `onUpdate`. A `ScriptBase` script:

```cpp
// in a method (the constructor too): once in 2 s, then every 0.5 s from 1 s
invoke([this]() { spawn(); }, 2.0f);
TaskId blink = invokeRepeating([this]() { toggle(); }, 1.0f, 0.5f);
invoke([this]() { resumeMenu(); }, 0.3f, TimeMode::Unscaled);   // runs at timeScale 0
cancelInvoke(blink);      // false when it is not this script's or already done
cancelAllInvokes();
bool pending = isInvoking();   // or isInvoking(id)
```

The timers belong to the script: they keep running while it is disabled or its GameObject is
inactive and are cancelled when the script is deleted (`~ScriptBase`: GameObject destroyed,
scene unloaded, Play stop). Callbacks run on the main thread after `onUpdate` (with the Lua
timers, in creation order, at most once a frame; a late repeat is rescheduled from now);
exceptions are not caught. Code that is not a script: `#include "Timer.h"`,
`jasperengine::Timer::after(2.0, fn)`, `Timer::every(0.5, fn, TimeMode::Scaled, owner, count)`,
`Timer::cancel(id)` (or `ScriptScheduler::after/every` from `"ScriptScheduler.h"`; an `owner`
pointer makes `ScriptScheduler::cancelOwner(owner)` end them, unowned ones end at Play stop).

Time under Unity's names: `#include "EngineTime.h"`, `jasperengine::Time::getDeltaTime()` (the
fixed step inside `onFixedUpdate`), `getUnscaledDeltaTime()`, `getFixedDeltaTime()`, `getTime()`,
`getUnscaledTime()`, `getRealtimeSinceStartup()`, `getTimeSinceLevelLoad()`, `getTimeScale()`...;
or `Engine::getDeltatime()`, `Engine::getUnscaledDeltatime()`, `Engine::getUpdateTime()` (the
fixed step; `Engine::getFixedDeltatime()` is a fixed frame delta, Unity's captureDeltaTime).

## Saves and JSON

- PlayerPrefs: `jasperengine::UserSettings` (`"io/UserSettings.h"`, flat include
  `"UserSettings.h"`): `getIntegerForKey(k, default)`, `setIntegerForKey(k, v)`, the float,
  double, long, bool, string and `Data` versions, `hasKey`, `deleteKey`, `deleteAll`, `save`.
  The same keys as Lua `PlayerPrefs` / `UserSettings` (jasper://docs/lua-api, 2.4).
- Files: `jasperengine::FileUtils` (`"FileUtils.h"`): `readText(path, out, error)`,
  `writeText(path, text, error)` (atomic), `appendText`, `exists`, `remove`, `makeDirs`, `list`,
  with the Lua `File.*` path rules (`data://` writable; `asset://`, `lua://` read-only).
- JSON: nlohmann json 3.11 is on the include path (`#include "json.hpp"`, `nlohmann::json`);
  `nlohmann::json::parse(text)` and `j.dump()`. To hand JSON to Lua or take it from Lua use
  `jasperengine::LuaJson` (`"LuaJson.h"`: `toJson`, `push`, `encode`, `decode`, the same rules
  as the Lua `Json` module).

## Global shader values

`jasperengine::ShaderGlobals` (`"shader/ShaderGlobals.h"`, flat `"ShaderGlobals.h"`), the Lua
`Shader.setGlobal*` (jasper://docs/shaders#global-values), main thread only:
`int id = ShaderGlobals::propertyToID("FogTint")` once, then `ShaderGlobals::setColor(id,
Vector4(1, 0.5f, 0, 1))` (sRGB in), `setFloat`, `setVector`, `setMatrix`, `setFloatArray(id,
values, count)`, `setVectorArray`, `setTexture(id, Texture("textures/noise.png"))`,
`clearTexture`, `get(id, value)` (a `ShaderGlobalValue`: type, linear values, texture),
`exists`, `getNames()`, `resetToDefaults()`. A renderer's parameter follows a global with
`setShaderGlobalBinding(member, global)` (`Image`, `Text`, `Polygon`, `Points`, `LineRenderer`,
`TrailRenderer`, `Skybox`; a mesh: `Material::shaderGlobalBindings` with
`ShaderGlobals::setBinding`, then `setMaterial`).

The subscriptions belong to the script (`this`): when the script goes away they are removed even
without the `UNREGISTER_*` (and its timers cancelled). Timers: `#include "util/Scheduler.h"`,
`Scheduler::game().after(2.0, [this]() { ... }, TimeDomain::Game, {.ownerKey = ownerKeyOf(this)})`,
or keep the handles in a `TimerGroup` member. Logging with a category and the call's file:line:
`JE_LOG_WARN(logCategoryScript(), "hp %d", hp)` (`"log/Logger.h"`).

## Rules that bite

- Includes are quoted and resolve against the engine API snapshot: `"Engine.h"`,
  `"Input.h"`, `"Log.h"`, `"GameObject.h"`, `"Mesh.h"`, `"component/ScriptComponent.h"`.
  Never `#include <core/...>`.
- Logging: `Log::print("pos %f %f", p.x, p.y)`, `Log::warn`, `Log::error` (printf format).
  Never `printf`, `std::cout` or `Engine::log` (it does not exist).
- Key codes are `D_KEY_*` macros (`D_KEY_SPACE`, `D_KEY_LEFT`); Lua uses `Input.KEY_*`.
- Input as in Lua (jasper://docs/input): `Input::getKeyDown(D_KEY_SPACE)`, `Input::getTouch(0)`,
  and the input actions of Project Settings > Input with `#include "input/InputActions.h"`:
  `InputActions::getAxis("Horizontal")`, `InputActions::getButtonDown("Jump")`. A source the
  script feeds (`InputActions::setSourceValue(reinterpret_cast<uintptr_t>(this), "Horizontal",
  1.0f, InputSourceKind::Script)`) is cleared when the script is deleted.
- Angles are in DEGREES by default (`Engine::setUseDegrees(false)` switches):
  `Quaternion(angle, axis)` takes the angle first; `Quaternion::fromEulerAngles`,
  `Camera::rotateView` all go through `Angle::defaultToRad`. `std::sin/cos` want radians:
  convert with `Angle::defaultToRad`, `Angle::degToDefault`, `Angle::radToDefault`.
- Physics: continuous forces in `onFixedUpdate`, not multiplied by deltatime; teleport with
  `Rigidbody::setPosition/setRotation` or `Rigidbody2D::setPosition/setAngle`, never by
  writing the GameObject's transform from `onFixedUpdate`. See `jasper://docs/physics`.
- `Rigidbody::setMass` only writes the live Jolt body and is lost on reload: author density.

## Where sources go and how they build

- Script files (`.h`/`.cpp`) can live anywhere in the project outside `.jasperengine/`. The
  editor discovers the classes attached to GameObjects and compiles them into the project
  library (editor Play) or the game (export).
- **Script directories** (Project Settings > Directories, `scriptDirs` in project.yaml) are
  compiled whole and are include roots: `<root>/Player/Thing.h` is included as
  `"Player/Thing.h"`. Shared helper classes belong there. A `.cpp` neither under a root nor
  included by an attached script is not compiled at all. Discovery skips hidden directories
  and directories named `build`.
- The editor runs a C++ build only when a C++ script (enabled or not) is attached to some
  GameObject: in a Lua-only project helper sources are first compiled by an export.
- **Never edit the generated `CMakeLists.txt`** (regenerated every build). Custom settings go
  in `ProjectBuild.cmake` at the project root, attached to `${JASPERENGINE_TARGET}` with paths
  from `${JASPERENGINE_SCRIPTS_DIR}`:
  ```cmake
  target_include_directories(${JASPERENGINE_TARGET} PRIVATE ${JASPERENGINE_SCRIPTS_DIR}/Source/Public)
  target_link_libraries(${JASPERENGINE_TARGET} PRIVATE mylib)
  ```
- The C++ standard is a project setting (`cxxStandard`: 20, the default, or 23); do not set
  `CMAKE_CXX_STANDARD` yourself. The engine and its API headers need C++20. The compilers that
  build it: GCC 11, Clang 16, Apple Clang 15, MSVC 19.30 (Visual Studio 2022) or newer; the
  generated `CMakeLists.txt` stops with a message naming the compiler below those. C++23 exports
  to Xcode need Xcode 16 or newer.
- Code that must build with a physics backend disabled in the export guards on
  `JASPERENGINE_PHYSICS_2D` / `JASPERENGINE_PHYSICS_3D`.
- Verify: `compile_scripts` builds the scripts without playing and returns the compiler
  errors as `file:line:column: message` (project-relative): fix, compile again, then play and
  read the Console for runtime failures ("Script crash in scene ..." entries). Play builds too
  (incrementally after a compile_scripts), and a compile error then shows in the Console.

## Moving from C++17

A project saved with `cxxStandard: 17` opens as C++20: the editor logs one Console warning
(`Project C++ standard 17 is no longer supported; the project now builds with C++20 ...`) and the
next Play or `compile_scripts` builds at C++20. Its compile errors show the usual way. Most
C++17 code compiles unchanged; what C++20 changed:

- `u8"..."` literals are `const char8_t[]`: they no longer convert to `const char*` or
  `std::string`. Drop the `u8` prefix (sources are UTF-8) or copy the characters over.
- `std::filesystem::path::u8string()` and `generic_u8string()` return `std::u8string`, and
  `std::filesystem::u8path()` is deprecated. Convert with
  `std::string(s.begin(), s.end())`, and build paths from UTF-8 with
  `std::filesystem::path(std::u8string(text.begin(), text.end()))`.
- `[=]` in a member function capturing `this` implicitly is deprecated: write `[=, this]`
  (or `[this]`, or `[*this]` for a copy).
- Compound assignment and `++`/`--` on a `volatile` variable are deprecated: write
  `x = x + y`.
- A class with a user-declared constructor (even `= default` or `= delete`) is no longer an
  aggregate: `T{1, 2}` needs a matching constructor, or remove the declared ones.
- Arithmetic and bitwise operators between two different enumeration types, or between an
  enumeration and a floating-point value, are deprecated: cast to the underlying type first.
- `char8_t`, `concept`, `requires`, `consteval`, `constinit`, `co_await`, `co_yield` and
  `co_return` are keywords and can no longer name things.
- `a == b` also tries `b == a`: a non-`const` member `operator==` can become ambiguous. Make it
  `const`, or a free function with symmetric parameters.

Every export builds with the oldest standard libraries the engine supports (GCC 11's, and
libc++ for macOS 10.15 and iOS 13), so scripts that ship to every platform stay off `<format>`,
`std::jthread` and stop tokens, `<semaphore>`/`<latch>`/`<barrier>` and `std::atomic` waits,
`<memory_resource>`, floating-point `std::to_chars`/`from_chars`, `<syncstream>`, `<chrono>`
calendars and time zones, coroutines and modules.

## Tools

- `create_script` (kind `cpp_script` = ScriptBase subclass, `cpp_subclass` = subclass of the
  GameObject's class, needs `entity`) writes the editor's template (or your `header_content` /
  `source_content`) and can attach it. `attach_script` attaches an existing `.h`/`.cpp` pair,
  `update_script_entry` repoints / reorders / enables, `detach_script` removes it.
- `set_script_properties` sets DPROPERTY values on a GameObject (`{"speed": 300}`);
  `list_scripts` shows them. A new DPROPERTY appears once the saved header is re-parsed:
  before your next tool call (listed in its `external_changes`).
- Edit sources with your own file tools (clangd reads the generated `.clangd`, which points
  it at `.jasperengine/clangd/compile_commands.json`; engine headers are in
  `.jasperengine/engine-api/`). Then `compile_scripts`, or `jasperengine-editor check` from a
  shell. `write_file` / `read_file` / `search_files` are for clients without file access.
