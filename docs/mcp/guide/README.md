# Agent reference docs

Longer references behind the agent guide (`editor/mcp/McpInstructions.cpp`, sent as
`initialize.instructions` and served as `jasper://guide`). They are embedded in the editor at
build time (`CMakeLists.txt` generates `mcp_docs.h`; `editor/mcp/tools/DocsTools.cpp` serves
them), so a new file here becomes `jasper://docs/<file name>` after a rebuild. This README is
not served; `jasper://docs` lists the served documents. The guide refers to them by these URIs:

| File | Resource | Contents |
|---|---|---|
| [lua-api.md](lua-api.md) | `jasper://docs/lua-api` | Lua script structure, property types, events, every bound class used in gameplay, enums, pitfalls (checked against `engine/core/script/binding/*`) |
| [cpp-scripting.md](cpp-scripting.md) | `jasper://docs/cpp-scripting` | C++ script kinds, DPROPERTY, event macros, includes, script dirs, ProjectBuild.cmake |
| [physics.md](physics.md) | `jasper://docs/physics` | bodies and colliders, mass and density, forces, teleporting, layers, ray casts |
| [shaders.md](shaders.md) | `jasper://docs/shaders` | library shaders (list_shaders, material shader values and render state), forking built-in shaders, editing rules, the custom uniform block |
| [prefabs.md](prefabs.md) | `jasper://docs/prefabs` | overrides, Apply/Revert, Prefab Mode, variants, Instantiate/Destroy, the build's prefabs |
| [../../unity-glossary.md](../../unity-glossary.md) | `jasper://docs/unity-glossary` | Unity term to engine name tables |

Keep these in sync with the engine: a name in them must exist in the bindings or headers.
