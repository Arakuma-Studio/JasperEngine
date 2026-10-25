# Engine performance audit

The October 2026 audit reduces repeated CPU work in five engine and editor areas.
The changes preserve public APIs and valid-input results. The comparison runners
compile the production methods from an original git revision and the working tree,
use real dependencies, and check their results before timing them.

## Critical paths

- `EditorFrame::run` drives editor input, `App::engineRender`, scene views and
  presentation. `Engine::runFrame` runs fixed updates, variable updates and draws.
  Scene systems update actions, pose animation, meshes, lattices, legacy UI, HTML
  UI, fluid, rendering, physics and audio. Matrix/vector operations underpin world
  bounds, skeletal bounds, instance bounds, camera projection and culling.
- Hierarchy selection, scoped entity lookup, reparent/delete checks, model
  extraction and MCP filters call `EntityRegistry::isParentOf`.
- Live pose transitions call `PoseAnimatorRuntime::transitionSettleTime` when
  starting `goToPose`; the spring settling scan is an interaction cost, not a
  per-frame cost.
- Editing code calls `CustomTextEditor::tokenizeAll`; it retokenizes the document
  to carry multiline state between lines. Member completion calls
  `inferTypeOfVariable` to find the receiver's declaration or constructor.
- HTML UI layout and text geometry call `TextShaper` through `RmlFontEngine`.
  Legacy UI text uses it through `STBText`. Font fallback, script itemization,
  HarfBuzz shaping and bidi resolution are shared work.

## Top five opportunities

| Priority | Repeated work | Expected effect and implementation |
| --- | --- | --- |
| 1 | Matrix transforms make 12 or 16 out-of-line vector-index calls | Read public vector fields directly, preserving the floating-point accumulation order. Reaches bounds and projection throughout rendering without cache invalidation. |
| 2 | Ancestry queries scan unrelated transforms | Jump to the next ancestor's existing array index. Work scales with the ancestor chain rather than the span of unrelated scene objects; contiguous bone chains retain array traversal. |
| 3 | Pose transitions repeat the same spring settling scan for multiple bones | Reuse exact overshoot/anticipation results within one call. Typical left/right pairs share work; zero looseness shares one scan across all 22 bones. Delay and duration remain per bone. |
| 4 | Code edits rebuild token allocations and autocomplete compiles unused language patterns | Retain surviving line vectors, and compile only the active language's inference regexes. Removes repeated allocations while preserving parsing and match precedence; no persistent regex cache. |
| 5 | Plain ASCII labels allocate and resolve a bidi paragraph | Shape printable ASCII directly when the paragraph direction is LTR or defaults to LTR. Font/script itemization and HarfBuzz remain unchanged; RTL, controls and non-ASCII take the complete bidi path. |

Token vectors retain each surviving line slot's capacity until it shrinks out of
the document or the editor is destroyed. This trades retained reusable memory for
fewer edit-time allocations. No historic document containers or global caches are
added. The spring cache is bounded stack storage and lasts for one call.

## Measured results

Measured on 25 October 2026 against `139d300f2`, with GCC 13.3 on Ubuntu 24.04,
an AMD EPYC Milan virtual machine, and the benchmark process pinned to CPU 0.
Compilation was paused during the sequential measurements. Values are median
process CPU time per operation; these are isolated kernels rather than frame
times. Release fixtures use `-O3` without LTO. Matrix sources compile as separate
translation units, matching the normal engine build.

| Operation and fixture | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| Matrix4 × Vector3 | 21.01 ns | 4.70 ns | 4.47× |
| Matrix4 × Vector4 | 30.83 ns | 4.14 ns | 7.44× |
| Eight-corner AABB transform | 325.35 ns | 144.05 ns | 2.26× |
| Ancestry, shallow child after 1,024 unrelated transforms | 599.43 ns | 29.30 ns | 20.47× |
| Ancestry, shallow child after 10,000 unrelated transforms | 5.630 µs | 0.0377 µs | 149.46× |
| Pose settlement, Snappy preset | 3.806 ms | 1.731 ms | 2.20× |
| Pose settlement, Stop-motion preset | 3.802 ms | 0.173 ms | 22.03× |
| C++ retokenization, 10,000 lines | 5.799 ms | 5.007 ms | 1.16× |
| Lua retokenization, 10,000 lines | 4.509 ms | 3.744 ms | 1.20× |
| CSS retokenization, 10,000 lines | 1.942 ms | 1.381 ms | 1.41× |
| C++ type inference, 100 lines | 386.92 µs | 293.32 µs | 1.32× |
| Lua type inference, 100 lines | 381.00 µs | 91.76 µs | 4.15× |
| ASCII label shaping | 1.339 µs | 1.057 µs | 1.27× |
| ASCII numeric label shaping | 1.724 µs | 1.174 µs | 1.47× |
| ASCII paragraph shaping, 900 characters | 75.977 µs | 64.819 µs | 1.17× |

Retokenization allocations per 10,000-line pass fell from 50,000 to zero for
C++/Lua, and from 80,000 to 40,000 for CSS. Type-inference allocations fell from
14,055 to 10,746 for C++ and to 3,312 for Lua in the 100-line matching case.
Non-ASCII, mixed Arabic and explicit RTL shaping controls stayed within 1% by
median paired ratio. Linear/Smooth pose settlement stayed approximately unchanged.

Ancestry has a small CPU tradeoff on contiguous chains: a 16-link ancestor query
rose from 28.02 to 30.64 ns, and its unrelated query from 28.46 to 33.06 ns. The
10,000-link unrelated query rose from 11.395 to 11.614 µs, approximately 2%.
Direct-parent queries stayed approximately unchanged. The indexed jumps target
wide hierarchies while retaining the contiguous traversal for bone chains.

Ancestry uses nine alternating pairs and independently calibrates every timing
block to at least 10 ms process CPU. Its speedup column uses the median paired
ratio. Pose and editor cases use seven alternating pairs, matrix cases nine
samples per revision with alternating process order, and text cases eleven
alternating pairs. Raw samples and per-comparison environment metadata are in
`build/performance-audit/release` for this run.

## Reproduce

Requirements: Linux, Python 3, GCC or a compatible C++17 compiler/linker, CMake
and Ninja. The fixtures use POSIX timing and GNU-style linker flags. The text runner builds
the bundled HarfBuzz and SheenBidi; other runners compile small standalone
fixtures. Run from the repository root on an otherwise quiet machine:

```bash
python3 tools/performance/run-audit.py --baseline-ref 139d300f2
python3 tools/performance/run-audit.py --baseline-ref 139d300f2 --sanitize
```

The wrapper runs comparisons sequentially and writes compiler/platform/revision
metadata, logs and raw samples under `build/performance-audit/{release,sanitize}`.
`--only ancestry|pose|matrix|tokenizer|inference|text` runs one comparison. Each individual
runner also accepts `--baseline-ref` and `--output`.

The fixtures measure uncached CPU kernels, not total editor latency or scene FPS.
They use alternating before/after order and repeated samples. The tokenizer
fixture uses representative C++, Lua and CSS source lines with the production
language initialization and generated engine type/builtin/enum sets.
Matrix sources compile as separate translation units without LTO, matching the
normal engine build.

## Behavioral verification

- Ancestry: approximately 1.89 million differential queries, including reordered
  arrays, missing transforms/ancestors, malformed order, self-parenting and cycles.
- Pose: all feel presets, randomized settings, non-finite fields and exact
  settlement results. No cache state survives a settings edit or another call.
- Matrix: 20,000 random affine/projective vector cases, 100 special float pairs
  and 13,337 AABB cases. Finite values and signed zero compare bitwise; NaNs compare
  by classification. Covers null/infinite/degenerate boxes and cached corners.
- Tokenizer: six languages, multiline comments/strings, edits, inserted/deleted
  lines, language changes, shrinking, clearing and reloading documents. Checks
  every token and continuation of cached HTML/CSS lexer states.
- Autocomplete inference: six languages, empty/missing names, line bounds,
  pointer/reference/template declarations, constructor precedence, Lua
  `new`/`create`/colon forms and shadowing. Supported inputs return identical types.
- Text: printable ASCII combinations and randomized strings, direction options,
  embedded ranges, reversed fallback fonts, Arabic/Hebrew/combining/emoji/control
  inputs and malformed UTF-8. Compares every shaped glyph field. Overflowing range
  arguments are rejected before iterator arithmetic.

All comparison runners provide ASan/UBSan checks. The full Release editor, editor unit
tests, pose tests and lattice tests provide integration coverage.

```bash
cmake -S . -B build/validation -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DJASPERENGINE_EDITOR_TESTS=ON \
  -DJASPERENGINE_POSE_TESTS=ON -DJASPERENGINE_LATTICE_TESTS=ON
cmake --build build/validation --target jasperengine-editor \
  jasperengine-editor-tests jasperengine-pose-tests jasperengine-lattice-tests -j4
build/validation/jasperengine-editor-tests
build/validation/jasperengine-pose-tests
build/validation/jasperengine-lattice-tests
```

The Release editor/engine build and final incremental build passed. Integration
suites passed 624 tests with zero failures: 357 editor, 230 pose and 37 lattice.
One editor test was skipped because it covers the decomposition-disabled build
configuration, while this build enables decomposition. All six isolated
comparisons also passed their differential checks and ASan/UBSan runs.

The headless scene check relinked the original five audited translation units
with identical Release flags and the same unchanged dependency objects. Three
alternating before/after pairs ran `pose-animation/Main Scene` under Mesa llvmpipe
with two renderer workers, Xvfb and a D-Bus session. Each run used 60 warmup and
180 measured frames, a 752 × 383 game viewport, and disabled mesh LOD/depth prepass.
All six runs rendered identical work: 17 draws, 17 instances and 12,548 triangles.
Median frame time was 52.14 ms before and 52.24 ms after; wall FPS was 19.18 and
19.14. This software-rendered scene check establishes consistent render work;
it does not establish an overall FPS improvement. The CPU reductions are measured
in the isolated comparisons above.

After the Release build, reproduce this audit-specific scene check with Xvfb,
`dbus-run-session` and Mesa installed:

```bash
python3 tools/performance/scene_compare.py baseline --baseline-ref 139d300f2
python3 tools/performance/scene_compare.py compare
```

The helper isolates baseline objects/binaries and resets a copied fixture for
each run under `build/validation`. It rolls back only the five audited `.cpp`
files, sharing the other objects and current headers; use it with the audit's
unchanged dependency/header versions. JSON results are saved under
`build/validation/integration`.

## Other opportunities reviewed

- Repeated bulk reparenting can repeatedly scan and sort branches. Publicly
  mutable parent links and component ordering make persistent branch caches a
  higher-risk change.
- Keyframe lookup scans from the start. Publicly editable, potentially unsorted
  times prevent a behavior-preserving binary search; a different linear scan had
  mixed results on short early samples and was left unchanged.
- Billboard rotation matrices can be shared across an instance batch, and dirty
  skin checks revisit joints. These need representative scene measurements before
  expanding the render changes.
- Project-window offscreen asset work, glyph-atlas width-cache invalidation and
  world-UI triangle picking need interaction/lifecycle measurements before changing
  clipping or caching behavior.
