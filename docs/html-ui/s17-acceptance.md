# S17 acceptance (HTML UI widgets)

The record of `docs/specs/S17-htmlui-widgets.md` §7 on `feat/s17-htmlui-widgets`, merged with
`oe-port` (`c62ecba1c`). Every build and test ran on the remote build servers through `rb.sh`: the
suites on B (shared), the timing gates on E (the benchmark host, nothing else running), the web
builds on C, the editor and the MCP tests on D, the Android build and emulator on A. The deviations
from the spec are in its "Deviations" section.

## Criteria

| AC | Evidence | Result |
|---|---|---|
| AC1 | `VirtualWindowCore.*` (21, ported OE cases), `HeightIndex.*` (7, 100k random ops against a naive prefix array) | pass |
| AC2 | `HtmlUIWidgets.ListPoolBound` (pool ≤ 13 + 8 + 8, no element made scrolling to every 97th index, first/last = `HeightIndex::indexAt`); MCP `test_html_ui_widgets.py::test_inspect_widget` | pass |
| AC3 | `HtmlUIWidgets.AutoHeightAnchor` (first visible row within 0.5 px) | pass |
| AC4 | `HtmlUIWidgets.ModelBinding` (one row updated, count and sizer follow, no element made with P7-D), `ModelBindingWithoutRowAliases` | pass |
| AC5 | `HtmlUIWidgets.Keyboard`, `HtmlUIWidgets.GamepadNavigation` (d-pad, A fires `activate`, Up at the first row leaves the view) | pass |
| AC6 | `HtmlUIWidgets.Tree`, `.Table` (sort event and classes, a header-edge drag resizes header and every pooled cell), `.Grid` (4 → 3 lanes, no stale cell); MCP `test_column_resize_by_drag` | pass |
| AC7 | `WidgetsGolden.*` (`golden/s17/`): the four views at rest and scrolled (sticky table header), a `title` tooltip at each corner (flipped, clamped), a rich tooltip with arrow, the drag ghost with allowed and denied classes, Markdown of every block kind; glcore and Vulkan from one set of images | pass (no Chromium references: deviation) |
| AC8 | `PopupPlacement.*` (10), `TooltipManager.*` (6, fixed `HtmlUIClock`) | pass |
| AC9 | `TestScriptEvents.DataTransfer*` (9: order, Lua acceptance, not accepted, Escape, prevented `dragstart`, `UIDrag`, list-view bands, edge auto-scroll, touch long-press) | pass |
| AC10 | `MarkdownParser.*` (18), `MarkdownHtml.*` (30-snippet corpus, `<script>`/`onerror` as text, autolinks), `HtmlUIWidgets.MarkdownElements` (inline `<https://a.b>`, HTMLUI-HTML-0032, `src`, `data-markdown`, `setMarkdown`, `markdownToHtml`) | pass |
| AC11 | `TreeReconcile.*` (12) | pass |
| AC12 | `TestDocumentReload.Patch*` (4: handles, id-less listeners, anime, Lua node, input value, focus, scroll, scripts once, `onReload` not `onLoad`; data-for subtree; broken edit; rebuild mode), `CompatHotReloadTest.*`, `CompatLuaTest.DocumentScriptsRunInPlay` | pass |
| AC13 | On E: `HtmlUIWidgetsPerf.ListScroll10k` p95 0.33 ms (≤ 1.0), `.ListScroll10kAuto` p95 0.33 ms (≤ 1.25), `TestDocumentReload.PatchReload2kPerf` p95 4.1 ms (≤ 15), `.ModelSetAndTreeFlatten` 8.9 ms (≤ 30) and 0.47 ms (≤ 2), `MarkdownHtml.Perf20KB` 0.27 ms (≤ 2), 24 B/item (`HeightIndex.MemoryPerItem`, the view's `memoryBytes`) | pass (the mobile device check: not run, no reference device) |
| AC14 | MCP `test_html_ui_widgets.py` (inspect_ui widget/tooltip/drag/reload, assert_ui virtual_window/tooltip/selected, edit_ui_view actions and refusals, the html_ui group, the inventory slot 0 → 5 swap read with `inspect_ui models`, a patch keeping Lua state); `test_html_ui_examples.py` (the new examples at every viewport) | see the run record |
| AC15 | `ExamplesCompat.*` (14 examples and blank, no warning or error); `check --ui` widget codes (`test_cli_ui_widget_codes`: HTMLUI-HTML-0030; 0029/0031/0032 are runtime or notes: deviation); support matrix (`test_ui_support_json_and_generated_doc`: `title` emulated, `draggable` mapped, the views and `markdown` engine elements) | pass |
| AC16 | `tools/test-projects/htmlui-widgets` (server_browser and inventory with their Lua): web gate `GATE=htmlui-widgets` PASS (WebGL2: 10,000 rows, 24 slots, no error), sample-project gate PASS; Android (`ANDROID_PROJECT=htmlui-widgets`, API 34 emulator on A): both `android-expect.txt` lines (10,000 rows shown 1..16, 24 slots, no problem) PASS | pass |

## Run record

The final runs, on the branch merged with `oe-port` `c62ecba1c`:

- glcore, the whole `jasperengine-htmlui-tests` (B, `JASPER_HTMLUI_PERF_SLACK=3`): 3,356 passed, 74
  skipped (the usual), 1 failed: `EffectPerfTest.GridSteadyStateAndBudgets` (564 s on the shared
  host; it passes on E, and it is not S17's).
- Vulkan (`HTMLUI_BACKEND=vulkan`, C): the S17 suites (widgets, goldens, tooltips, drag and drop, reconcile, reload, Markdown, placement, views, texture mode, examples, compat Lua and hot reload; timing excluded) 142 ran, 0 failed. An earlier whole-suite Vulkan run: 3,353
  passed, with the same three failures the glcore run had then (fixed below).
- Perf (E, nothing else running): the eight S17 timing tests and `EffectPerfTest` pass; numbers in AC13.
- Editor (D): `htmlui-editor-tests` 72 ran, 0 failed. MCP (D): `gen_tool_docs.py` (193 tools,
  `docs/mcp/TOOLS.md` regenerated), `tool_budget.py --check` within `tools/mcp/budget.json`
  (`html_ui` 1 tool, 1,100 chars; guide 15,708 of 16,384 bytes), then the HTML UI, budget,
  conventions, groups, coverage, generators, Lua API and protocol tests: 134 passed, 0 failed, 2
  skipped. `test_html_ui_examples.py` (every example at every viewport) passed after the S23 merge.
- RmlUi's own tests (`tools/htmlui/run-rmlui-tests.sh`, C): 463 of 463 passed.
- Web (C): `WEB_PROJECT=htmlui-widgets` + `GATE=htmlui-widgets` PASS, `tools/build.sh web` +
  `web-gate` (sample-project) PASS.
- Android (A): `ANDROID_PROJECT=htmlui-widgets` on the API 34 emulator: both `android-expect.txt`
  lines PASS. Smoke (`tools/fixtures/post-processing`) PASS.

Failures found on the way and fixed:

- `CompatLuaTest.DocumentScriptsRunInPlay`: a patch re-added a text a script had replaced; the
  3-way rule now keeps nodes a script removed unless the author changed them.
- `ForkFeatures.PresentFeatures` and `NativeFeatureConsistency.EveryFeatureHasASample`: S17's two
  runtime-only fork features.
- The first htmlui-widgets web gate failed on a debug-build assertion of the fork ("Missing
  specification for one or more Property IDs", P5-1's id counted too early), fixed in
  `StyleSheetSpecification.cpp` (FORK.md "P5-1 fix").
- `MarkdownParser.HostileInputsParseInLinearTime` failed on E at first ('[' x 500,000 then links:
  76.7 ms against 3.6 ms at a quarter): glibc page faults, not the parser. The nodes and brackets
  are now chunked and the test keeps the heap: 11.1 ms against 2.1 ms.
- UBSan: a misaligned read in the fork's `SVGCache` hash (FORK.md "SVG hash fix"). A UBSan report
  in `ShaderData` (a `bool` load) predates S17 and stops `SANITIZE=address,undefined` runs early;
  `SANITIZE=address` ran the S17 tests clean.
