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
| `REGISTER_BUTTON_EVENT(onPress, method)` | `UNREGISTER_BUTTON_EVENT(onPress, method)` | Button events |
| `REGISTER_UI_EVENT(event, method)`, `REGISTER_SCROLLBAR_EVENT`, `REGISTER_PANEL_EVENT` | `UNREGISTER_...` | UI component events |
| `REGISTER_COMPONENT_EVENT(Component, event, method)` | `UNREGISTER_COMPONENT_EVENT(...)` | any component's event |

The method name must be the Engine event's name (`onUpdate`, `onFixedUpdate`,
`onPostUpdate`, `onKeyDown`, ...). Events of a disabled script (inactive GameObject or
disabled entry) are skipped. Registering in the constructor or in `init()` both work.

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
- The C++ standard is a project setting (`cxxStandard`: 17, 20 or 23); do not set
  `CMAKE_CXX_STANDARD` yourself.
- Code that must build with a physics backend disabled in the export guards on
  `JASPERENGINE_PHYSICS_2D` / `JASPERENGINE_PHYSICS_3D`.
- Verify: `compile_scripts` builds the scripts without playing and returns the compiler
  errors as `file:line:column: message` (project-relative): fix, compile again, then play and
  read the Console for runtime failures ("Script crash in scene ..." entries). Play builds too
  (incrementally after a compile_scripts), and a compile error then shows in the Console.

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
