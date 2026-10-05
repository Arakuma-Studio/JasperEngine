# Slop audit — October 2026

Baseline: `a277a50b`. The checkout was clean and matched `origin/main` at the start. Parallel audits covered first-party engine, editor, shader compiler, platform adapters, build configuration, tools, fixtures, and documentation. Separate reviewers checked the combined changes. Bundled dependency integration was inspected; upstream libraries, examples, licenses, and tests were retained rather than treating external APIs as dead because JasperEngine does not call them.

This is a behavior-preserving cleanup. Public engine APIs, Lua bindings, compatibility decoding, platform fallbacks, useful tests, and ownership/concurrency guards remain. The following groups describe every substantive change; adjacent declaration/call-site updates and obsolete comments belong to their corresponding group.

| Change | Evidence and reason | Risk and verification |
| --- | --- | --- |
| LuaFunction call overloads | Fourteen bodies differed only in argument count; two constrained packs retain original arities and zero-argument overloads. | Overload/stack risk: original and changed real-Lua harnesses passed every supported arity, order, return type, explicit template/member-pointer use, excess-arity rejection, errors, and stack balance. |
| Animation frame setters | Repeated the existing getActionFrame bounds check and error handler. | Same checked accessor, assignment and logging; affected translation unit compiled. |
| GameObject handle construction | File-local handleOf only called the constructor. | Same non-owning handles and C++17 elision; all callers inspected and compiled. |
| FunctionSubscribe private wrappers | addImpl and getPlaceholder merely forwarded existing operations. | Callback overloads tested with ASan/UBSan; original destruction order preserved. |
| XMLUtils lookup | Manual sibling walk duplicated TinyXML's name-filtered lookup. | Duplicate/nested/missing/case-sensitive/empty-key checks with ASan/UBSan; public null-key checks retained. |
| FileData path assembly | Two reversing stacks served only to restore input order; extension lookup ran twice. | 195,312 strings compared against original for both separator conventions, with ASan/UBSan. |
| Vector2/Vector3/Quaternion normalization and Rect predicates | Existing normalization methods contained identical arithmetic; boolean branches repeated their conditions. | Exact comparison over 114,641 inputs including NaN, infinity and signed zero. |
| SceneManager and ResourceProgress lookups | Repeated scene search and duplicate fallback assignments. | Existing lookup/fallback semantics inspected; foundation checks and compilation. |
| ShaderPool parsing/names | Duplicate canonical type table, single-use filename wrapper, identical Fog branches, impossible empty-name check, redundant hexadecimal prefix detection. | 10,069 parser comparisons plus every prefix byte; private wrapper callers checked. |
| SoundPool/TextureDataPool helpers | Single-use filename wrappers, repeated hash calculation, unused capture, duplicate shutdown checks while holding the same mutex. | All shutdown writers use that mutex; worker checks and ownership-count predicate retained; independent review and compilation. |
| ShaderDataSerializer | Stateless helper class repeated declarations; writer templates had one concrete writer. | Identical SDAT bytes, JSON, round trips and 607 error results, including every truncation, invalid magic and wrong key. Corrected stale format/cache comments. |
| Shader compiler optimizer blocks | All checked CMake/Xcode configurations set ENABLE_OPT=0; SPIRV-Tools is not built. | Original and changed preprocessed translation units byte-identical with ENABLE_OPT=0; public argument fields retained. |
| ShaderVariantCompiler storage type | Every mapping branch returned STRUCT. | Constant substitution inspected and compiled. |
| ShaderVariantCollection clear | A copied vector of map keys was immediately consumed to erase the map. | Repeated begin erasure preserves ascending release order; keys passed by value and destructor dependencies reviewed. |
| Buffers and SokolBuffer | Empty-vector checks immediately followed clear; defaults and size assignments repeated; boolean result wrapped in branches. | Constructor defaults and exact descriptor assignments checked; compiled. |
| Texture cleanup and SokolObject | Two callbacks only called free; UniformStageSlot had no references. | Same scheduling and lifetime with std::free; backend compilation. |
| ActionSystem tweens | Mesh/UI color and alpha methods had identical bodies; private time/action parameters unused. | Seven original bodies compared with target substitutions; same interpolation and update order; compilation. |
| UISystem and AudioSystem | Unread eventId, uncalled private hit-test overload, redundant boolean returns. | Global references checked; same audio calls/results; compilation. |
| MeshSystem | Write-only async result fields, unused locals/parameter, assignment-only material wrapper, duplicate lookup/return logic. | Private references and call effects inspected; compilation. |
| RenderSystem | Unused private parameters and unchanged matrix copy. | Editor and runtime configurations compiled; rendering expressions retained. |
| Prefab movement recovery | Recovery map was never populated, undo branch unreachable, move implementation had no callers. | Removed command branch, Project methods/types together; independent usage review and prefab suites. Documentation now describes the live restructuring rules. |
| Prefab import flag and command merge overrides | Setter had no callers; four merge overrides returned the inherited false. | Prefab Mode/placement conditions retained; command-history and prefab checks. |
| CreateEntity/CreateTrackKey commands | Duplicate constructors, unread selection vector, constructor-only cached fields and integer-channel dispatch. | Delegated initialization and directly seeded properties preserve values; command compilation and prefab checks. |
| AI provider adapters | OpenAI and DeepSeek adapters differed only in literal endpoint URL. | Common transport keeps provider-specific payload behavior; 11,345 original/changed request-response cases matched byte-for-byte. |
| Catalog/Factory | Null-return callback and aggregate wrapper added indirection; positive-float fallback always 1.0. | Both callback consumers already check null; formatter equivalence over 10,009 float patterns; compilation. |
| Generator/Exporter | Command passthrough, unused path arguments, duplicate Windows path normalization, pruning flag always true. | Copy harness compared content, mtimes, updates, pruning and errors; exact Windows path logic retained; compilation. |
| Editor private state/helpers | Unread resourcesFocused; unused Prefab bookkeeping/accessors, shader-cache clearer, include-root helper, CLI/include-key forwarding. | Repository-wide references and call arguments checked; affected translation units compiled. |
| Editor rendering | Ten uncalled getters/toggles, eight base-only mouse overrides, ten identical overlay allocators. | Shared allocator keeps system entity pool, overlay registration and ownership; twelve helper/render translation units checked. |
| Window private parameters and PlayInput | Unused arguments, discarded pure prefab lookup, press/release lambdas used once. | All caller expressions checked for side effects; original key press/repeat/release order retained. |
| ExportWindow shader selection | Built and sorted a display list only to convert it back to keys and rebuild it; output path buffer was write-only. | 10,073 original/changed cases under ASan/UBSan preserve normalization, union, ordering, fromScene and selection reset. |
| Dialog helpers | Constructor-only setup wrapper, unused tooltip/scene/buffer state, filename wrappers, unused tree root parameter and redundant branches. | References and side effects checked; affected dialog compilation and shader-selection differential checks. |
| CustomTextEditor | Read-only/line-number controls had no consumers; documentation field never populated; owned suggestions always initialized; search wrap duplicated selection code. | Retained live editing/autocomplete controls; 80,000 original/changed search steps plus editing, undo and completion matched byte-for-byte across four languages. No implicit move can invalidate suggestions. |
| InputTextContextMenu | Looked up input state before immediately looking it up again after the popup opened. | Removed only unused first lookup. |
| Platform adapters | Three desktop sample-count overrides matched the base; Android input-state member was transient; private EGL result ignored. | Linux compilation; exact GameActivity implementation copies the input structure synchronously; recovery/log messages retained. |
| Prefab thumbnail timestamp portability | Apple CI exposed an existing ambiguous to_string call because libc++ uses a 128-bit filesystem clock representation. | Use the same explicit int64_t conversion as Project::getThumbnailPath, retaining native tick units and existing Linux/Windows stamp strings; integer-format regression checks and platform rebuild. |
| macOS keyboard activity range | Apple CI exposed references to private ImGui range macros unavailable through the public header. | Use their exact public enum aliases, matching EditorFrame; compile-time equality checks establish the identical key range, with Apple compilation checked in CI. |
| Xcode shader compiler deployment targets | Apple CI compiled glslang for macOS 10.14 despite its std::filesystem dependency requiring 10.15; the iOS shader compiler similarly lagged the app/engine floor. | Align only shadercompiler Debug/Release with the existing macOS 10.15 and iOS 13.0 app/engine targets. Parsed all 3,275 project objects and resolved 4,691 references; exactly four deployment settings changed. Native compilation is covered by Apple CI. |
| Android Gradle and dependency CMake | Unused legacy test runner/BuildConfig generation, commented alternatives, duplicate Threads lookup. | No tracked consumers; required unconditional lookup retained; platform builds. |
| Prefab fixture/test scaffolding | Duplicate fixture builders/accessors/constants, unused import/argument/snapshot/YAML read and single-element loops. | Five fixture modes generated byte-identical projects/equivalent ops; all 76 instance assertions retained; all three suites retained. |
| Prefab dump comparison | morphWeights mask was a stale workaround; production now zero-initializes the array. | Equal dumps pass and changed morph weights fail, including with ignore-derived. |
| Stale comments/includes | Disabled calls, obsolete agent attribution, narration and duplicate includes added no information. | Active code untouched; corrected workspace/prefab/shader comments checked against implementation. |

Two proposed changes were rejected during review: iterator-based pool cleanup could invalidate traversal through custom deleters, and unified subscriber erasure could change callable destruction ordering. Snapshot cleanup and original erasure ordering were restored. The shared-pointer ownership-count guard and protected subsystem signature were also retained for valid external uses.

Integrated validation passed:

- Linux Release editor, shared runtime and standalone sample builds.
- Android debug APK build for arm64-v8a, armeabi-v7a, x86 and x86_64.
- Web Debug build and Chromium rendering at 960 × 540, including runtime shader compilation; no browser errors.
- All three prefab suites against both the baseline installed editor and the rebuilt editor. The prefab-runtime scene dump also matched byte-for-byte.
- Runtime-shader fixture export, complete standalone build, game launch and editor GUI smoke test. An extended run reached “every variant ready, nothing pending” and “0 not ready, 0 compiling”; its screenshot showed the expected objects and shadows. No fatal log markers.
- Core component/morph and Base64 ASan/UBSan checks, the differential checks listed above, Python/shell syntax checks, and `git diff --check`.

Reproduction from the repository root:

```sh
JOBS=4 tools/build.sh linux
JOBS=2 tools/build.sh web
tools/build.sh android
FIXTURE=/path/to/TappyPlane python3 tools/prefab-instance-edit-test.py
FIXTURE=/path/to/TappyPlane python3 tools/prefab-nested-test.py
FIXTURE=/path/to/TappyPlane python3 tools/prefab-variant-test.py
python3 tools/performance/run_core.py --sanitize
python3 tools/performance/run-base64.py --baseline a277a50b --check
tools/smoke-test.sh tools/fixtures/runtime-shaders
```

The local prefab runs used `build/audit/perf-smoke-source` for sprite assets. Detailed logs/screenshots are under ignored `build/slop-audit/` and `build/smoke/`; auxiliary differential harnesses are temporary validation artifacts rather than permanent test scaffolding. Windows/macOS/iOS require the CI runners; no local execution on those platforms is claimed. Successful builds and the checks above establish the reviewed behavior, not an exhaustive proof of every engine configuration.
