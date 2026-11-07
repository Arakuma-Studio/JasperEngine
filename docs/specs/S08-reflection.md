# S08 — Reflection tables: one description per component drives serialization, inspector, MCP and migration

Wave 2. Depends on S01, S02, S03, S07; feeds S04, S23, S24, S29, S31 and every wave-2 spec that adds component fields.

## 1. Goal

Every component gets one hand-authored reflection table that lists its fields with name, kind, YAML key,
enum table, ranges, units, label, tooltip, asset kind, flags and rename aliases. That table replaces
the parallel hand-written copies we keep today: the YAML codecs in `editor/Stream.cpp`, the
`Catalog` property descriptors, the MCP enum option tables, the Inspector enum lists and the
exported-code emitters in `Factory.cpp`. All of these consumers then read the table. A component
that needs nothing special gets serialization, a multi-object Inspector, MCP get/set with a JSON
Schema, undo, asset locators for S04 and migration rules without any per-component code. Existing
scenes must re-save byte for byte.

## 2. Scope

**In scope**
- The reflection core in `editor/reflect/`: field kinds, type ops, nested structs, `std::vector`
  arrays, proxy fields, enum tables, path resolution, the YAML/JSON/C++-literal codecs and the registry.
- Moving all 53 `ComponentType`s (`editor/Catalog.h:71-127`) and `PostProcessingSettings` onto it,
  one component at a time, each guarded by parity tests.
- A generic default Inspector with multi-object editing, plus a custom-drawer registry
  (per component, per field path, and per named drawer).
- MCP: component JSON Schemas, richer `get_entity`, range/alias-aware `set_properties`, a
  `jasper://reflection` resource and a read-only `get_inspector_layout` tool.
- Undo through a type-erased value command (S24 interface), prefab-override paths, update flags.
- Migration hooks: rename aliases, per-`formatVersion` YAML migration steps (S03), and
  name+kind value remapping for hot reload (consumed by S23).
- Field locators and asset-reference visiting for the S04 dependency graph.
- Optional: a field metadata dump for Lua docs/stubs and a report of Lua binding gaps.

**Out of scope**
- Their 10k-line C++ header parser / ComponentScanner (do-not-copy list). There is no code
  generator; tables are hand-written C++20.
- Their POD/memcpy ECS core and byte-offset field model (do-not-copy list). Our components stay
  non-POD.
- Changing the scene format to theirs. YAML stays. Format changes go through S03 versioning.
- Generating LuaBridge bindings (S23 owns scripting). Reflecting `Scene`/project settings other
  than `PostProcessingSettings`. Material files as assets (Material is reflected only as a nested
  struct of `MeshComponent`; its file codec stays custom).
- Changing component storage (S07).

## 3. Current state

The same facts are written down five or more times per component:

| Copy | Where | Notes |
|---|---|---|
| YAML codec | `editor/Stream.h:347-534`, `Stream.cpp` (8,627 lines), `StreamPrefab.cpp` | One `encodeX/decodeX(node, const X* old)` pair per component (e.g. Light `Stream.cpp:6299-6355`, Fog `6446-6472`). `encodeComponents` (`Stream.cpp:4192`) is a 50-branch `if (signature.test(...))` chain. |
| Property descriptors | `editor/Catalog.cpp:15-21` `FastPropertyDescriptor{name, PropertyType, updateFlags, def, ref}`, 40 tables / 326 entries (e.g. `kLightProperties` `528-548`), resolver table `3270-3324`, hand-written nested resolvers (Rigidbody 134 lines, Sprite 102 lines), `findEnabledGetter` `3335-3358` | Already a member-pointer reflection, but with no labels, ranges, enums, codecs or YAML keys. |
| Enum tables | `Stream.cpp:1303` `lightTypeToString`, `McpValues.cpp:244` `lightTypeOptions`, `InspectorWindow.cpp:160` `entriesLightType`, `Factory.cpp:570` `formatLightType` | Four copies per enum, and they already drift: Camera YAML writes `camera_perspective` (`Stream.cpp:1444`) while MCP says `perspective` (`McpValues.cpp:304`). `McpValues::enumOptions` (`865-967`) is a per-component switch. |
| Inspector | `editor/window/InspectorWindow.cpp` (14,758 lines). Dispatch is an if/else chain (`14262-14368`) over about 60 `drawXComponent` functions (`5526-13860`). | Each one calls the generic `propertyRow(RowPropertyType, cpType, path, label, entities, RowSettings)` (`2738`), which already handles multi-entity mixed values, prefab override marks and `PropertyCmd<T>`. Only the field list, labels, help text, ranges and conditions are hand-written. |
| Exported code | `editor/Factory.cpp` `createXComponent` (e.g. Fog `1822-1835`) | One `comp.field = literal;` line per field. |
| Structural flags | `isNonStructuralProperty` switch, `editor/command/type/PropertyCmd.h:56-83` | Per-component policy hard-coded. |

Adding `ReflectionProbeComponent` touched 27 source files. Serialization has house rules that
live only in comments: "written only when not the default, so scenes saved before it existed
re-save unchanged" (`Stream.cpp:3324, 3370, 3381`; Light `shading`, `6319`); finite guards
(`decodeFinite` `212`, `encodeFinite` `279`); a hand-written `-1` layer mask (`decodeLayerMask`
`266`); legacy keys (terrain `6092-6114`). Vectors are flow style (`encodeVector3` `1535`). Floats
are written by `YamlNumbers::text` (`editor/util/YamlNumbers.h`) or by `node[k] = float`. Both give
the same text. Scenes are written with `YAML::Dump` (`editor/Project.cpp:5703`), and yaml-cpp
keeps map insertion order. Some property paths are not YAML keys: Light's `shadowCameraNear/Far`
are views onto the YAML key `shadowCameraNearFar` (`Catalog.cpp:543-544`). Prefab overrides are
keyed by property path (`PropertyCmd.h` → `recordPrefabEdit`), and the `ComponentType` ordinal is
a bit position in the prefab override mask (`Catalog.h:122-124`, append-only).

One in-house prototype already works: `PostProcessingSettings::Field`
(`engine/core/SceneSettings.h:167-185`, `SceneSettings.cpp:28+`). Its single table drives YAML
(`Stream.cpp:3389-3440`), the Inspector (`InspectorPostProcessing.cpp:663`), MCP
(`mcp/tools/SceneTools.cpp:650, 680`) and codegen (`Factory.cpp:834`). S08 generalizes it.

## 4. Reference (OpenEngine, MIT, read-only)

All paths are under `external/OpenEngine/`. We take the design and write our own code. We expect
to copy no file; if any snippet is copied, it gets a `NOTICE.md` entry and keeps its header.

- `Engine/Modules/ECS/Include/ECS/Reflection.h`
  - **Take:** an append-only kind taxonomy; `FieldFlags` Hidden/ReadOnly/Transient; per-field
    enum name tables, range, tooltip and asset category.
  - **Do not take:** the offset/size/memcpy model (our fields are non-POD), the 64-argument
    `GE_PP_FOR_EACH` macro, or scanner-emitted enum tables.
- `Engine/Modules/ECS/Include/ECS/ComponentFieldRegistry.h`, `Source/ComponentFieldRegistry.cpp`
  - **Take:** lookup by id and by name (including the unqualified suffix), case-insensitive
    `FindField`, and component marks DoNotSerialize/EditorOnly.
  - **Do not take:** copy-on-write mutation after registration (our tables are complete where
    defined), or module purge (we have no component DLLs).
- `Engine/Source/Scene/ReflectionSceneSchema.cpp`, `Include/Scene/FieldSerializerRegistry.h`
  - **Take:** the generic schema used when no hand-written one exists, with hand-written schemas
    as overrides; a flat per-kind codec table; `EnumerateAssetReferences` (`:584`).
  - **Do not take:** their line-based text format.
- `Engine/Modules/ECS/Include/ECS/ComponentMigration.h` (`BuildFieldRemap`)
  - **Take:** the rule "keep a field only if name and type match, otherwise default". We apply it
    to values, not bytes.
- `Apps/Editor/Source/Inspectors/DefaultComponentInspector.cpp`
  - **Take:** mixed detection per field and per lane (`SpanDiffers` `:60`), live preview while
    dragging with one undo commit (`WireEdit` `:225-268`), quaternion shown as Euler, enum
    dropdowns, asset pickers filtered by kind.
  - **Improve:** compare typed values with epsilon, not bytes.
- `Apps/Editor/Include/InspectorRegistry.h`, `Source/InspectorRegistry.cpp`
  - **Take:** overrides registered per type, and a context that carries the whole selection.
  - **Do not take:** module ownership and their UI toolkit.

## 5. Requirements

### Functional
- **R1** Every `ComponentType` has exactly one `ComponentDesc`: C++ name, YAML/MCP key
  (`"light"`), display name, icon, has-enabled, structural policy, field table. The
  `Catalog::getComponentName/DisplayName/Icon/hasEnabledProperty/getComponentType/findComponents`
  if/else chains are replaced by table lookups that return identical strings.
- **R2** Supported field kinds: Bool, Int, UInt, Float, Double, Vector2/3/4, Quat, Matrix4, Rect,
  String, Enum, LayerMask, Ease, Entity, EntityReference, Texture, Font, Material, AssetPath
  (string with an `AssetKind`), Struct (nested `StructDesc`), Array (`std::vector<T>` of any kind,
  including Struct), and Custom (user-supplied `TypeOps`, e.g. `ShaderUniformValues`, `std::variant`).
  `std::shared_ptr`/Lua refs/GPU handles are either an asset kind with a codec or a `NoSave|Hidden` field.
- **R3** Per-field metadata: path name, YAML key (default = name), flags, update flags, enum,
  hard range (enforced on edit), UI range (slider only), step/format, unit, label (default:
  Unity-style from the name, `shadowBias` → "Shadow Bias"), tooltip, group, named drawer,
  `visibleIf` predicate, UI order, rename aliases.
- **R4** Flags: `Hidden`, `ReadOnly`, `NoSave`, `NoMcpWrite`, `NoCodegen`, `OmitIfDefault`,
  `OmitIfNonFinite`, `Structural`, `ColorLinear`, `ColorSRGB`, `Hdr`, `Proxy` (a view onto another
  field: never saved, prefab override recorded under its own path), `Derived` (saved, not
  editable: Transform world matrices), `NoMultiEdit`.
- **R5** Each enum has one `EnumDesc`: per value a YAML token, an optional MCP token (default =
  YAML token), a label, the C++ name, aliases and a hidden-in-UI flag, plus a fallback value.
  `Stream::xToString/stringToX`, `McpValues::*Options`, the Inspector `entries*` vectors and
  `Factory::formatX` for migrated enums are deleted and replaced by it. MCP accepts both tokens.
- **R6** Path grammar `seg('.'seg | '['N']')*` (today's Catalog syntax, e.g.
  `submeshes[0].material.baseColorFactor`). `resolve(desc, comp, path)` returns
  `{FieldDesc*, void* ref, owner}`. Aliases are accepted in every segment. `Catalog::findProperty`
  / `getProperty` / `getProperties` / `getChangedUpdateFlags` / `copyPropertyValue` work on top of
  it for migrated components, with an unchanged API.
- **R7** Generic YAML codec. Keys are emitted in table order with the existing helpers (flow
  vectors, `YamlNumbers::text`, `encodeTexture`). It honours `OmitIfDefault`/`OmitIfNonFinite`/
  `NoSave`. The decode signature is the legacy `(node, base)` one: absent keys keep the base value.
  Decode never throws. A malformed value keeps the base value and adds a skip record
  `{component, path, line, reason}` to S03's load report. Keys that do not match a field or alias
  are passed to S03's unknown-data store. Whole-component `ComponentCodec` overrides take an
  `EncodeContext` (entity, registry, signature, embed flags), as needed by Mesh and UI
  (`Stream.cpp:4200-4219`).
- **R8** Default Inspector (`editor/window/inspector/DefaultInspector.cpp`). It draws visible
  fields in UI order, grouped (`SeparatorText`), honours `visibleIf` (evaluated on the first
  selected entity, as the hand drawers do today) and draws every row through the existing
  `propertyRow` path. Kinds map to `RowPropertyType`. Named drawers cover `halfCone`, `direction`,
  `color3L`, `po2Slider`, `layerMask`, `quatEuler` and `assetPicker(kind)`. Struct fields become
  collapsible sub-sections. Array fields get a size row plus per-element rows, and add, remove and
  reorder buttons.
- **R9** Multi-object editing:
  - Only components common to the whole selection are shown (the current intersection,
    `InspectorWindow.cpp:14203-14222`).
  - Mixed is detected per field with `TypeOps::equals` (float epsilon 1e-4, as `propertyRow`) and
    per lane for vectors and colours. Enum, string and asset rows show "—".
  - An edit writes the field to every selected entity as one undo step. Lanes that were not
    edited keep each entity's own value.
  - Arrays whose lengths differ are shown as mixed. Their size and order are read-only, and
    "Set all to this" (context menu) copies the first entity's array.
- **R10** Custom drawer registry `InspectorDrawers` with three hooks:
  - `setComponentDrawer(ComponentType, fn)` replaces the default drawer.
  - `setFieldDrawer(ComponentType, path, fn)` overrides one field.
  - `setNamedDrawer(id, fn)` defines a drawer that fields reference by id.
  - Custom drawers can call `drawDefaultFields(ctx, include/exclude list)`.
  - The if/else dispatch at `InspectorWindow.cpp:14262` becomes a registry lookup. Each
    `drawXComponent` is first registered unchanged, then deleted once the default drawer reaches
    layout parity (AC7).
- **R11** Undo: `ReflectedValueCmd` (in `editor/command/type/`) stores type-erased old/new values
  per entity through `TypeOps` (clone/assign/destroy). It applies update flags, records prefab
  overrides under the canonical path, preserves submesh override masks (as `PropertyCmd.h`), and
  merges consecutive edits of the same path during a drag. It implements S24's name: "Set
  <Display Name> <Label>" plus " (N objects)" when N>1. `isNonStructuralProperty` is replaced by
  `Structural` flags. Scalar rows keep `PropertyCmd<T>`; arrays, structs and Custom kinds use
  `ReflectedValueCmd`.
- **R12** Asset locators (for S04). `visitAssetRefs(desc, comp, fn(locator, AssetKind, ref))`
  walks nested structs and arrays. The locator text is `<componentKey>/<path>`, for example
  `mesh/submeshes[2].material.baseColorTexture`. `resolveLocator` reverses it, including through
  aliases.
- **R13** Migration:
  - Rename: aliases on fields, enum tokens and component keys are accepted on read (YAML, MCP,
    prefab override paths, clipboard). Only the canonical name is written.
  - Semantic changes: `ComponentDesc::migrations[] = {{toFormatVersion, fn(YAML::Node&)}}`. S03
    collects these into its ordered step table and runs them on the component node before
    decode. No per-component version key is ever written.
  - Hot reload: `remapByName(oldFields, oldValues, newFields)`. Same name and kind keeps the
    value. Int↔Float, Vector widening and String↔AssetPath are converted. Anything else gets the
    default and a report entry. S23 uses it for Lua/C++ script property sets across reloads.
- **R14** Exported-code emitter. `cppAssignments(desc, comp)` writes `var.field = <literal>;`
  for saved fields without `NoCodegen`, using the enum C++ name. It replaces `createXComponent`
  for migrated flat components.
- **R15** `PostProcessingSettings::Field` moves onto `StructDesc`. `getFields()`/`findField()`
  remain as a thin façade until their four call sites are moved over, then are deleted.

### Platform & backends
- **R16** Reflection is editor-only code (`editor/reflect/`). No engine or exported-player
  sources include it, so the cost in shipped games is zero. It compiles on GCC 13, Clang/Xcode
  and MSVC (all editor toolchains) with no compiler-specific reflection.

### Performance
- **R17** Encode and decode of a synthetic 10,000-entity scene (Transform + Sprite + Light +
  AudioSource + Rigidbody2D mix) each stay within +5% of the legacy codec, median of 5 runs, in
  `tools/performance/reflect_codec.cpp` (same style as the existing perf programs).
- **R18** `resolve` on a flat or indexed path does no heap allocation (string_view parsing,
  per-struct sorted name index built at startup) and runs ≤ 150 ns median for a 20-field component.
- **R19** Default Inspector with 200 selected Light entities: ≤ 3 ms CPU per frame for the
  component body (release build, build server).
- **R20** The tables are static. The name indexes total ≤ 256 KB and are built once at editor
  start in ≤ 5 ms.

### Editor UX (human)
- **R21** The default drawer matches the hand drawer it replaces (labels, order, groups,
  conditional rows, tooltips with units such as "Range (m)"). Tooltip = table tooltip + unit +
  hard range.
- **R22** Hard ranges clamp typed and dragged input. UI ranges set slider bounds only. ReadOnly
  rows are drawn disabled. Hidden rows are not drawn.
- **R23** Copy/Paste Component, Reset (`ComponentValuesCmd`) and prefab Revert/Apply work on
  reflected components unchanged.

### Agent UX (MCP, Lua, CLI)
- **R24** `list_component_types {component}` adds `schema`: a JSON Schema object (type, enum
  tokens, minimum/maximum, `default`, `readOnly`, description with unit, `x-asset-kind`,
  `x-aliases`, nested objects and arrays).
- **R25** `get_entity detail:"full"` adds per property `label, unit, range, ui_range, tooltip,
  read_only, asset_kind, group`. Existing keys and the `values` shape are unchanged.
- **R26** `set_properties`:
  - A value outside a hard range is rejected with an error naming the range and the valid
    alternative.
  - An unknown property is rejected with a did-you-mean suggestion (S03 nearest-name helper).
  - A property given by an alias is applied and the response includes a
    `warnings: ["light.old is now light.new"]` entry.
  - `NoMcpWrite`/ReadOnly properties are rejected.
- **R27** Resource `jasper://reflection`: every component and enum, with schema and format
  version. S31's CLI and S23's single JSON API description consume it.
- **R28** New read-only tool `get_inspector_layout {entities}`: the rows the Inspector drew for
  the selection on the last frame, as
  `[{component, path, label, kind, group, mixed, read_only, visible}]`. It is used by parity tests
  and by agents to check UI state. It is added to the MCP agent guide and `docs/mcp/TOOLS.md`.
- **R29** (optional) `tools/mcp/tests/test_reflection.py::test_lua_field_report` lists reflected
  editable fields with no LuaBridge property of the same name (report only, non-gating).
  `generate_lua_stubs.py` may read `jasper://reflection` for field docs.

### Data & compatibility
- **R30** Re-saving any existing scene, prefab or project gives byte-identical files.
- **R31** House rule from S08 on: fields added to any component are `OmitIfDefault`, so older
  files re-save unchanged and adding a field needs no version bump. A rename (alias) or semantic
  change bumps S03's `formatVersion`, and a semantic change also adds a migration step.
- **R32** Loading never clamps or rewrites values. Ranges apply only to edits.
- **R33** `ComponentType` stays append-only (prefab override mask). Reflection never assumes
  ≤64 types (S07).

## 6. Design constraints & decisions

- **D1 Tables are hand-written C++20 with no generator and live in the editor.** The editor holds
  all consumers, and `Catalog`'s descriptors already live there. Engine component headers are not
  changed.
- **D2 Accessors use member-pointer thunks** (`getMemberRef` pattern, `Catalog.cpp:44-57`), never
  `offsetof`. Non-POD members are handled by `TypeOps`, which are deduced from the member type at
  compile time.
- **D3 Table order = YAML key order.** That order is the persisted contract. Inspector order
  comes from `uiOrder` and `group`.
- **D4 YAML scalar text must come from the same helpers** (`YamlNumbers`, `encodeVector*`,
  `encodeTexture`, `decodeLayerMask`), moved into `TypeOps`. There is no new formatting code.
- **D5 Migration is one component per PR, each PR containing:** table, deleted hand codec, deleted
  Catalog descriptors, deleted MCP/Inspector/Factory enum copies, parity tests and golden updates
  (goldens must not change). The legacy codec moves to the test-only
  `editor/tests/reflect/legacy/` and is compared against until S08 completes, then deleted. The
  goldens stay.
- **D6 Tiers.**
  - P1 pilot: Fog, Mirror, Light2D, Light, Camera, AudioSource.
  - P2 flat: Button, Scrollbar, Progressbar, Panel, InputField, RectTransform, LayoutGroup, Text,
    Image, UIDocument, ReflectionProbe, Skybox, Transform, ShadowCaster2D, Bone, Joint2D/3D,
    Action, Tween, Position/Rotation/Scale/Color/AlphaTween, Animation, SpriteAnimation.
  - P3 nested: Sprite, Tilemap, Polygon, MeshPolygon, Points, LineRenderer, InstancedMesh,
    Keyframe/Translate/Rotate/Scale/MorphTracks, Rigidbody2D/3D, ParticleSystem, Terrain (settings).
  - P4 metadata-only (table drives Inspector, MCP and locators; whole-component `ComponentCodec`
    stays): Mesh (Submesh/Material as StructDesc), UI, Model, Prefab, Script (fields from a
    dynamic `FieldSet` provider over `ScriptProperty`), Terrain maps.
- **D7 Gate for other specs.** P0 and P1 land at the start of wave 2. From then on, wave-2 specs
  that add fields (S10, S11, S12, S13, S20, S21, S22) add them to tables and never to hand codecs.
- **D8** `visibleIf` and drawers are plain function pointers (no `std::function` in tables).
  Custom drawers get `DrawContext{SceneProject*, entities, ComponentType, readOnly, isPrefab}`.

API sketch (`editor/reflect/Reflect.h`):

```cpp
namespace jasperengine::editor::reflect {
enum class Kind : uint8_t { Bool, Int, UInt, Float, Double, Vector2, Vector3, Vector4, Quat, Matrix4, Rect,
    String, Enum, LayerMask, Ease, Entity, EntityReference, Texture, Font, Material, AssetPath,
    Struct, Array, Custom };                                   // append-only
enum class AssetKind : uint8_t { None, Texture, Cubemap, Font, Material, Model, Audio, Script, Prefab, Shader, UIDocument, Scene, Any };
enum class Unit : uint8_t { None, Meters, Pixels, Degrees, Radians, Seconds, Percent, Lux, Lumens, Candela, Kelvin, Hertz, Decibels, MetersPerSecond, Kilograms, Newtons };
struct EnumItem { int value; const char* token; const char* label; const char* cppName;
                  const char* mcpToken = nullptr; std::span<const char* const> aliases = {}; bool hidden = false; };
struct EnumDesc { const char* cppType; std::span<const EnumItem> items; int fallback; };
struct TypeOps;   // construct/destroy/copy/equals(eps)/toYaml/fromYaml/toJson/fromJson/cppLiteral/schema
struct ArrayOps { size_t (*size)(const void*); void (*resize)(void*, size_t); void* (*at)(void*, size_t);
                  void (*insert)(void*, size_t, const void*); void (*erase)(void*, size_t); void (*move)(void*, size_t, size_t); };
struct StructDesc;
struct FieldDesc {
    const char* name; const char* yamlKey; Kind kind; const TypeOps* ops; void* (*ref)(void* owner);
    const EnumDesc* enumDesc; const StructDesc* structDesc; const ArrayOps* arrayOps;
    uint32_t flags; uint64_t updateFlags; AssetKind asset; Unit unit;
    float min, max, uiMin, uiMax, step; const char* format;
    const char* label; const char* tooltip; const char* group; const char* drawer;
    bool (*visibleIf)(const void* owner); std::span<const char* const> aliases; int16_t uiOrder;
};
struct StructDesc { const char* name; std::span<const FieldDesc> fields; };
struct ComponentDesc {
    ComponentType type; const char* cppName; const char* key; const char* displayName; const char* icon;
    StructDesc body; bool hasEnabled; bool structural;
    void* (*find)(EntityRegistry*, Entity);                // S07 storage stays behind this thunk
    const ComponentCodec* codec;                           // null = generic YAML codec
    void (*postDecode)(void* comp, const DecodeContext&);
    std::span<const Migration> migrations;                 // {toFormatVersion, void(*)(YAML::Node&)}
    std::span<const char* const> keyAliases;
};
const ComponentDesc* find(ComponentType); const ComponentDesc* findByKey(std::string_view); // key, cppName, alias; case-insensitive
std::span<const ComponentDesc* const> all();
FieldRef resolve(const ComponentDesc&, void* comp, std::string_view path, PathError* = nullptr);
void visitAssetRefs(const ComponentDesc&, void* comp, AssetVisitor);
YAML::Node encode(const ComponentDesc&, const void* comp, const EncodeContext&);
void decode(const ComponentDesc&, const YAML::Node&, void* comp, const DecodeContext&, DecodeReport&);
Json toJson(const ComponentDesc&, const void*, const JsonContext&); Json schema(const ComponentDesc&);
std::string cppAssignments(const ComponentDesc&, const void*, const CodegenContext&);
RemapResult remapByName(const FieldSet& from, const ValueSet& values, const FieldSet& to);
}
```

Authoring example (`editor/reflect/components/LightingReflect.cpp`; order = today's YAML order):

```cpp
JE_REFLECT_COMPONENT(LightComponent, ComponentType::LightComponent, "light", "Light", ICON_FA_LIGHTBULB,
  JE_FIELD(LightComponent, enabled),
  JE_FIELD(LightComponent, type, .enumDesc = &kLightType, .updateFlags = UpdateFlags_LightShadowMap
           | UpdateFlags_LightShadowCamera | UpdateFlags_Scene_Mesh_Variant),
  JE_FIELD(LightComponent, direction, .updateFlags = UpdateFlags_Transform, .drawer = "direction",
           .visibleIf = [](const void* l){ return ((const LightComponent*)l)->type != LightType::POINT; }),
  JE_FIELD(LightComponent, worldDirection, .flags = Derived | Hidden),
  JE_FIELD(LightComponent, color, .flags = ColorLinear),
  JE_FIELD(LightComponent, range, .unit = Unit::Meters, .min = 0, .max = 1e6f, .updateFlags = UpdateFlags_LightShadowCamera),
  /* ... intensity, innerConeCos (.drawer = "halfCone"), outerConeCos, spotMask, cullingMask,
     shadows, automaticShadowCamera, shadowBias (.group = "Shadow settings"), mapResolution (.drawer = "po2Slider"),
     shadowCameraNearFar (.flags = Hidden), numShadowCascades ... */
  JE_FIELD(LightComponent, shading, .enumDesc = &kLightShading, .flags = OmitIfDefault),
  JE_PROXY(LightComponent, "shadowCameraNear", shadowCameraNearFar.x, .updateFlags = UpdateFlags_LightShadowCamera),
  JE_PROXY(LightComponent, "shadowCameraFar",  shadowCameraNearFar.y, .updateFlags = UpdateFlags_LightShadowCamera));
```

The `JE_FIELD(T, m, ...)` macro expands to `reflect::field<&T::m>(#m, FieldMeta{__VA_ARGS__})`
(designated initializers, S01). Kind, `TypeOps`, `ArrayOps` and `StructDesc` are deduced from
`decltype(T::m)` through a `ReflectTraits<T>` customization point. Nested structs declare
`JE_REFLECT_STRUCT(Submesh, ...)` once and every place that uses them shares it.

MCP surface: `list_component_types` (+`schema`), `get_entity` (+metadata), `set_properties`
(range, alias and did-you-mean behaviour), new `get_inspector_layout`, resource `jasper://reflection`.

## 7. Acceptance criteria

The gtest suites run in the S02 editor unit-test target (`editor/tests/reflect/`). MCP tests are
in `tools/mcp/tests/test_reflection.py`.

- **AC1** `ReflectRegistry.CoversAllComponentTypes`: every `ComponentType` has a desc. Key, C++
  name, display name, icon and has-enabled equal `editor/tests/reflect/catalog_snapshot.json`,
  captured from the pre-S08 Catalog.
- **AC2** `ReflectRegistry.SelfCheck`:
  - field names, YAML keys and aliases are unique per struct;
  - no alias equals a live name;
  - enum tokens are unique and every item round-trips token→value→token;
  - the fallback is a member;
  - every non-`NoSave` field has a codec;
  - every `Proxy` resolves inside its owner.
- **AC3** `ReflectParity.<Component>.EncodeMatchesFrozen` (`EncodeMatchesLegacy` until S73 froze the
  legacy output): for 500 seeded random instances
  (NaN/Inf included), `YAML::Dump(new)` equals `YAML::Dump(legacy)` byte for byte.
- **AC4** `ReflectParity.<Component>.DecodeMatchesFrozen`: decoding legacy text, including random
  key subsets over a random base, gives the same values as the legacy decoder, with floats
  compared bitwise. Malformed scalars produce a skip record and no exception.
- **AC5** `test_reflection.py::test_golden_resave_<project>` (a test per project; see Deviations): the editor opens each of the 18
  `tools/fixtures/*/*.scene`, the prefab fixtures (`tools/prefab-fixtures.py`), and
  `tools/fixtures/reflection-parity/AllComponents.scene` (every component, non-default values),
  saves them, and the bytes equal `tools/fixtures/reflection-parity/golden/`. The goldens are
  captured with the pre-S08 editor in the first S08 PR.
- **AC6** `test_reflection.py::test_golden_resave_<project>`: the generated C++ for the parity fixtures
  equals the golden text captured before migration.
- **AC7** `test_reflection.py::test_inspector_layout_parity`: for each migrated component,
  `get_inspector_layout` matches the JSON captured from the hand drawer (labels, order, groups,
  visibility). For each `visibleIf` field, both states are checked, e.g. Fog `type` linear and
  exponential, Light point and spot.
- **AC8** Multi-edit:
  - `DefaultInspector.MixedDetection` (unit): three Lights with intensity 1, 1, 2 and the same
    colour give intensity mixed and colour not mixed. Vector lanes are mixed separately. Arrays of
    different lengths are mixed and their size is read-only.
  - MCP: `set_properties` on three entities, then `undo` once, restores all three.
- **AC9** `ReflectedValueCmd.*`:
  - array insert, remove and move on two entities, then undo and redo;
  - a prefab override recorded under the canonical path when the edit used an alias;
  - update flags passed to `Catalog::updateEntity` (mock);
  - drag merge produces one entry;
  - the S24 name "Set Light Intensity (2 objects)".
- **AC10** MCP schema (`test_reflection.py::test_schema`):
  - every `list_component_types` schema validates as Draft 2020-12 (python `jsonschema`), and
    `get_entity values` output validates against it;
  - out-of-range, unknown-name (with a suggestion), alias (with a warning) and ReadOnly cases
    behave as R26;
  - the whole existing MCP suite passes unchanged.
- **AC11** `ReflectLocators.*`: `visitAssetRefs` on `AllComponents.scene` entities gives the
  committed expected locator list, and every locator round-trips through `resolveLocator`.
- **AC12** `ReflectMigration.*`, on a test-only component desc:
  - an aliased key and an aliased enum token load into the new field and re-save under the
    canonical name;
  - a migration step at formatVersion N runs only for files below N;
  - a prefab override stored with the old path applies.
- **AC13** `ReflectRemap.*`: same name and kind is kept; int→float is converted; a kind change
  gives the default plus a report entry; added fields get their default.
- **AC14** `tools/performance/reflect_codec.cpp` meets R17. A gtest micro-benchmark meets R18
  (and also asserts no allocation, using a counting allocator). R19 is checked with the Inspector
  frame timer on the build server and logged in the PR.
- **AC15** Outcome after P3 (checked with `wc -l` in the closing PR):
  - `editor/Stream.cpp` ≤ 5,000 lines, `editor/window/InspectorWindow.cpp` ≤ 12,000 lines;
  - `McpValues::enumOptions`'s per-component switch is gone;
  - no `FastPropertyDescriptor` table remains for migrated components;
  - adding a flat component touches ≤ 4 editor files (table, `ComponentType`, registration,
    test), demonstrated by the P2 PR for the last flat component.
- **AC16** `tools/smoke-test.sh` and the full `tools/mcp/run-tests.sh` stay green after every
  migration PR.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01: designated initializers, `std::span`, concepts.
  - S02: gtest target, CI.
  - S03: root `formatVersion`, migration step table, unknown-data store, skip report,
    nearest-name helper.
  - S07: S08 reaches components only through `ComponentDesc::find` and `Catalog::getComponentId`.
    S07 provides `markChanged(entity, componentId)`, which `ReflectedValueCmd` calls, and must
    keep `ComponentType` ordinals stable.
  - S24: the `Command` name / Esc-cancel interface. S08 implements it for `ReflectedValueCmd` and
    forwards names from `PropertyCmd`.
- **Feeds:**
  - S04: locators, `AssetKind`, `visitAssetRefs`. When S04 changes the texture encoding to
    `{path, guid}`, that is a `TypeOps` change behind a formatVersion step.
  - S23: `remapByName`, `jasper://reflection` for the JSON API description, numeric-expression
    input in the shared number widgets.
  - S29: semantic diff labels.
  - S31: CLI flags from the schemas.
  - S10/S11/S12/S13/S20/S21/S22: their new fields go into tables (D7).
- **Merge hotspots:** `editor/Stream.cpp/.h`, `editor/Catalog.cpp/.h`,
  `editor/window/InspectorWindow.cpp/.h`, `editor/mcp/McpValues.cpp`,
  `editor/mcp/tools/ComponentTools.cpp`, `editor/Factory.cpp`, `editor/command/type/PropertyCmd.h`,
  `engine/core/SceneSettings.h/.cpp` (R15). Every wave-2 rendering, particle and animation spec
  edits Light, Camera, Particle, Animation and Mesh codecs and drawers. Migrate those components
  (P1/P3) before or in coordination with S10, S12, S20 and S21, and rebase per component, not in
  bulk.

## 9. Risks & defaults

- **Hidden codec quirks break byte identity** (conditional keys, key order unlike field order,
  legacy branches such as `Stream.cpp:6092`, context-dependent encodes for Mesh and UI).
  *Default:* fuzz parity against the legacy codec plus goldens. A quirk that one flag cannot
  express keeps a whole-component `ComponentCodec` (P4) instead of bending the generic codec.
- **Decode becomes more lenient** (no throws on malformed scalars). *Default:* accepted. This is
  an improvement, reported via S03's skip report and listed in the release notes.
- **Type-erased codec is slower.** *Default:* the R17 budget applies. If it is missed, add
  per-kind fast paths (Float, Vector3, Bool) with inlined `YamlNumbers`.
- **Inspector drift** (spacing, column widths from `getLabelSize`). *Default:* AC7 is
  row-semantic, not pixel-exact. Column width = widest visible label in the group, as today.
  Pixel differences are accepted after a manual screenshot review with `tools/editor-shot.sh` in
  each PR.
- **Enum token drift between YAML and MCP** (Camera). *Default:* keep both tokens (R5); MCP
  accepts both and emits its current token. They are unified only by a future formatVersion
  change.
- **Proxy fields and prefab overrides.** *Default:* overrides are recorded under the proxy's own
  path, as today (`shadowCameraNear`). Proxies are never written to YAML.
- **`visibleIf` on mixed selections** uses the first entity, matching today's hand drawers.
- **Lua generation stays out of scope.** If LuaBridge parity becomes painful, S23 decides on
  binding generation from these tables.

## Deviations

Recorded by the implementation (branch `feat/s08-reflection`).

- **Hand codecs kept (P3 → P4).** 38 of the 65 components use the generic codec. 27 keep a whole-component
  codec called through `ComponentDesc::codec`, with their table driving the Catalog, Inspector,
  MCP, locators and codegen: the P4 set (Mesh, UI, Model, Prefab, Script, Terrain) plus Sprite,
  Tilemap, Polygon, MeshPolygon, Points, LineRenderer, InstancedMesh, MorphTracks,
  Rigidbody2D/3D, Skybox, SpriteAnimation and the lattice, particle, force field and S22 gameplay codecs below. Their YAML has quirks that one flag
  cannot express (count-limited lists, keys read from other components, legacy branches), so
  §9's default applies. The codecs are in `editor/StreamComponents.cpp`. `Stream.cpp` is 4,374
  lines and `StreamComponents.cpp` 2,423 lines, so the codec code that remains is about 6,790
  lines in total. AC15's `Stream.cpp` ≤ 5,000 is met by that split, not by deleting those codecs.
  `ComponentDesc::extraKeys` lists each codec's known keys for `StreamSchema` and
  `tools/check_stream_keys.py`.
- **Components added upstream after the S08 base** (oe-port 05eb8d3b4; 697d661e2 added none): Fluid Simulation and
  Lattice Follower use the generic codec (parity-tested against their upstream codecs); Lattice,
  Lattice Modifier and Lattice Tracks keep their codecs (offsets sized by the resolution,
  flow-style empty lists, clamps on read) in `StreamComponents.cpp`, with tables for the rest.
  Their exported code stays the Factory writers'. The lattice drawers moved to
  `InspectorLattice.cpp`; the fluid keeps its own drawer (`InspectorFluid.cpp`).
- **S20 (oe-port 64d735104).** The new Particle System (an effect asset or an inline `.particles`
  document, exposed values, collision planes) and the Force Field keep S20's codecs in
  `StreamComponents.cpp` (the inline effect is its canonical JSON, the falloff curve key rows);
  their tables (`ParticlesReflect.cpp`) give the properties, MCP schema and locators, the inline
  document compared by its JSON. Their drawers are S20's (`InspectorParticles.cpp`), registered
  in the drawer lookup, with the force field's enum rows from its `EnumDesc`s. S20's scene 2→3
  and prefab 4→5 migrations (`StreamMigrations`) are the only format bumps: S08 adds no
  migration, so the sequence stays S20's. The old particle model's table, drawer and emitter-shape
  rows are gone; its `EnumDesc` stays for the legacy decode the migration uses.
- **S22 (oe-port 4e4ba3964).** The Character Controller 2D and 3D, the Water Volume and the
  Buoyancy keep S22's codecs (every key written) in `StreamComponents.cpp`, with tables in
  `GameplayReflect.cpp` and S22's drawers moved to `InspectorPhysics.cpp` (enum rows from their
  `EnumDesc`s). The controllers' `categoryBits`/`maskBits` are saved but no property (`Derived`),
  as S22's Catalog had them (a `uint16_t` the Catalog's `unsigned int` commands would overrun); the
  schema reads `UIntAs` defaults at their own width. `Catalog::copyPropertyValue` (prefab
  Apply/Revert of one property) copies a Custom value no type branch names through its table's
  `TypeOps::copy`, so Buoyancy `customPoints` and later table fields need no branch of their own.
- **Upstream animation (sync round 2).** The Pose Animator and the Animation Player keep their
  codecs (a missing key is the default, a component decoded over keeps its runtime) in
  `StreamComponents.cpp`, with tables in `ClipPlayersReflect.cpp`; their drawers stay upstream's
  (`InspectorWindow.cpp`, `InspectorWindowClips.cpp`), registered in the drawer lookup. A clip
  entry is a table struct (`clips[2].wrap`; the one-byte `ClipWrapMode` a Custom property, an
  enum to the schema). The Mesh's `morphWeights[N]` and the Model's `animationEntities` are table
  fields; their codecs stay the hand ones. No format bump: the sequence stays S20's.
- **Upstream vertex animation (sync round 3).** The Vertex Animator keeps upstream's codec (the
  authored fields; a component decoded over keeps only its Preview toggle) in
  `StreamComponents.cpp`, with its table in `ClipPlayersReflect.cpp` (attachments are a table
  struct whose `entity` is an entity reference; `attachments`, `instanceClips` and
  `instanceSpeeds` are whole lists); its drawer stays upstream's (`InspectorWindowVat.cpp`),
  registered in the drawer lookup. Its `.vat` is an `AssetRefSchema` row (the `GuidSibling`
  shape). No format bump. No tree has both it and the hand codecs of the others, so its goldens
  were taken on the merged tree, where its codec and drawer are upstream's hand ones: the
  AllComponents fixture keeps its entities and gains the Vertex Animator's as
  `JASPER_REFLECT_WRITE_FIXTURE=1` generated it (a whole regeneration would re-randomize every
  entity after the Mesh's morph weights and empty the Animation Player's clips); the catalog
  snapshot, the locators and the MCP goldens were captured with their tooling from that fixture,
  and differ from round 2's by the Vertex Animator only (and `actionFrameCount`, the accepted
  deviation above, no longer recorded).
  Upstream round 3 changes no scene or prefab format (it has no format versions; the Vertex
  Animator's keys are additive), so S29's scene 3 -> 4 and prefab 5 -> 6 steps (derived transform
  values dropped) stay the last ones; the goldens were taken on S29's formats.
- **Legacy oracle deleted by S73.** `editor/tests/reflect/legacy/` (the deleted hand codecs,
  test-only) stayed so the parity fuzzers could keep running. S73 froze its output for those
  samples in `editor/tests/reflect/parity_golden/` and deleted it, and `ReflectCodecBench` holds
  the codec to the hand codecs' frozen numbers (docs/specs/S73-code-cleanups.md, Deviations).
- **R15.** The engine keeps `PostProcessingSettings::getFields()` as the single field table,
  because the runtime clamp and the Inspector post-processing panel read it. The editor builds
  the `StructDesc` from that table (`editor/reflect/PostProcessingReflect.cpp`), so the scene
  codec and `jasper://reflection` use reflection, and a field added to the engine table is
  reflected without an edit. The façade is not deleted.
- **Default Inspector (R8/R10).** It is in `editor/window/InspectorDefault.cpp`, not
  `inspector/DefaultInspector.cpp`. The drawer registry is a static `componentDrawer(type)`
  lookup, not the three-hook `InspectorDrawers` API. 12 components moved to the default drawer
  (Button, Scrollbar, Progressbar, Panel, Text, RectTransform, Light2D, Mirror,
  LocalVolumetricFog, Tween, Bone, Prefab). The others keep their hand drawers, which were moved
  into `InspectorPhysics.cpp`, `InspectorAnimation.cpp` and `InspectorRenderers.cpp` and use the
  `EnumDesc` entries. These named drawers exist: `label`, `multiline`, `direction`, `halfCone`,
  `po2Slider`, `textureCube` and `modelEntity`. `InspectorWindow.cpp` is 11,410 lines.
- **Extra table vocabulary** beyond R3/R4: flags `NoDefault`, `NonStructural`, `CodegenIfChanged`,
  `CodegenRuntime`, `Transient`, `EmptyIsDefault`, `NullIfEmpty`, `NotListed`, `ListWhole`,
  `ListElements`, `ListEachElement`, `ListTemplate`, `ComponentOnly` and `Clamped`. These keep the Catalog
  property listing and codegen identical to before. Fields `enabledIf`, `enumMask`, `cppPath`,
  `count` (`JE_COUNT`) and `elementOps` were also added. `ComponentDesc` gained
  `remove`/`extraProperty`/`extraProperties` (Script properties, Model maps) and `extraKeys`.
- **Decode differences** (R7/R32, accepted per §9):
  - An unknown ReflectionProbe `mode`/`updateMode` token now keeps the base value and is
    reported. Before, it silently became `static`/`on_load`.
  - LocalVolumetricFog and VolumetricClouds are now also `markChanged` when decoded over an
    existing component, like every other generic component.
  - The Animation `actionFrameCount` pseudo-property is no longer listed. Through MCP it read a
    dangling pointer.
- **MCP.** `set_properties` refuses `Derived` fields as well as `ReadOnly`/`NoMcpWrite` ones. A
  field flagged `Clamped` (UIDocument `textureWidth`/`textureHeight`, Lattice `resolutionX/Y/Z`,
  Lattice Modifier `subdivisions` and binding `weight`) is clamped rather than refused outside
  its range: its load and `Catalog::updateEntity` clamp too, and the existing suite relies on that. MCP
  ease options leave out `custom`, as before. Camera keeps both tokens (R5 default).
- **Benchmarks.** `ReflectCodecBench` (R17) and `ReflectResolveBench` (R18) always log their numbers
  and always check for zero allocations. The time budgets are asserted only with
  `JASPER_TEST_PERF_SLACK`, in the benchmark run on E (docs/testing.md).
- **Goldens.** `test_golden_resave` saves each scene it opens again, 5 frames apart, until two
  saves in a row agree (in the capture and in the check), so values the systems compute after the
  load (mesh vertex counts, UI layout, camera clips, transparency from a texture loaded in the
  background) do not depend on load speed. Values the editor computes from its own view size
  are masked (`<view>`) in the goldens and the re-saves alike: a RectTransform's `scissor` and an
  `autoResize` camera's clips and aspect, in the scenes, prefabs and generated code. They follow
  the editor's window layout, not the code under test (S26's scrollbar fix widened the Scene view
  by 11 px and moved them all). The codegen golden's
  `rigidbody.solidBoxDensity`, an uninitialized member the editor before S08 exported as whatever
  memory held, is 0 (the reflection codec value-initializes components).
- **Done by S73, not here.** `editor/asset/AssetRefSchema` stayed hand-written until S73 generated
  its component rows and walk from these tables.
- **R29** (optional Lua field report) is not implemented.
- **AC5 and AC6 are one test per project** (`test_golden_resave_<project>`, branch `tests/split-slow-mcp`).
  One function re-saving every project, and a second comparing the code it left behind, took
  175-970 s and timed out at 1800 s on loaded servers. Each test now opens one project in its own
  editor and compares its files and its generated code; the file's `TESTS_INDEPENDENT` lets the
  runner put them in separate chunks.
