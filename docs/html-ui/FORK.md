# RmlUi fork

| | |
|---|---|
| Upstream | https://github.com/mikke89/RmlUi |
| Base | tag `6.3`, commit `ba95ffe8bfb6370efb2cdcca927eaad4710c5413` (2026-08-22) |
| Location | `engine/libs/rmlui/` |
| Build | static, hidden visibility, `RMLUI_STATIC_LIB` (public), `RMLUI_NO_FILE_INTERFACE_DEFAULT`, `RMLUI_SVG_PLUGIN` (links `lunasvg` privately), no FreeType; `Source/Debugger` only with `JASPERENGINE_HTMLUI_DEBUGGER` (editor, Debug builds, tests) |

The build files are ours: `engine/libs/rmlui/CMakeLists.txt` (desktop, Web, Android, every
export through `engine/CMakeLists.txt`), `engine/libs/rmlui/hidden-symbols.map` (the version
script that keeps every `Rml::`, `tvg::` and `lunasvg::` symbol out of the editor's shared engine, GNU ld/lld) and the
`rmlui-ios` / `rmlui-macos` targets of `engine/workspaces/xcode/JasperEngine.xcodeproj`. The
CMake and Xcode targets compile every `.cpp` of the vendored subset but `Tests/` (CMake: `Source/Debugger`
only with `JASPERENGINE_HTMLUI_DEBUGGER`; Xcode always, and the Debug configurations define it),
so adding or removing upstream files only needs the Xcode file list updated.

## Vendored subset

Copied byte for byte from the upstream tag:

```
engine/libs/rmlui/
  LICENSE.txt                    (MIT)
  Include/RmlUi/Core.h
  Include/RmlUi/Debugger.h
  Include/RmlUi/Config/**
  Include/RmlUi/Core/**          (includes Containers/robin_hood.h, itlib, and the shared Containers/LICENSE.txt)
  Include/RmlUi/Debugger/**
  Source/Core/*.{cpp,h,inl}      (Pool.inl is included by Pool.h)
  Source/Core/Elements/*.{cpp,h}
  Source/Core/Layout/*.{cpp,h}
  Source/Debugger/*.{cpp,h}      (+ Source/Debugger/LICENSE.txt: the embedded Courier Prime font, OFL)
  Include/RmlUi/SVG/ElementSVG.h  (phase 1, p1-vector)
  Source/SVG/*.{cpp,h}           (phase 1, p1-vector: the SVG plugin, unchanged, on LunaSVG)
  Tests/CMakeLists.txt           (phase 3, p3-testkit: RmlUi's own test suites, not part of the engine build)
  Tests/Dependencies/**          (doctest, trompeloeil, nanobench, lodepng; Tests/Dependencies/LICENSE.txt)
  Tests/Source/**                (Common, UnitTests, Benchmarks, VisualTests: the sources and upstream's CMake files;
                                  phase-3 patches add their own unit tests and benchmarks, rows below)
  Tests/Data/UnitTests/**, Tests/Data/style.rcss   (documents the unit tests load)
```

`Tests/` is built and run standalone, never by the engine build (whose source globs are not
recursive): `tools/htmlui/run-rmlui-tests.sh` (phase-3 spec 5.1) assembles the fork with the
upstream 6.3 parts the engine does not vendor (the FreeType font engine
`Source/Core/FontEngineDefault`, `CMake/`, `Samples/shell`, `Samples/assets`,
`Samples/basic/{demo,benchmark}/data`, `Backends/RmlUi_Backend.h`, from a pinned clone of the tag)
and builds it with `tools/htmlui/rmlui-tests/CMakeLists.txt` (globs the fork's sources) and a
window-less backend (`tools/htmlui/rmlui-tests/RmlUi_Backend_Null.cpp`). FreeType comes from the
host (`pkg-config freetype2`) or is built from source (`--docker`, the engine's Linux image has
none); the engine library stays without FreeType. Baseline on the fork: 146 test cases, 8306
assertions, all passing (P3-T2 adapts 6.3's tests to P2-8).

The SVG plugin is compiled into the `rmlui` target with `RMLUI_SVG_PLUGIN` defined, so
`Rml::Initialise` registers it as upstream does (`Source/Core/Core.cpp`); it links
`engine/libs/lunasvg` (LunaSVG 3.5.0 + plutovg, the version the phase-0 benchmark measured). The
engine registers its built-in fonts with LunaSVG (`engine/core/util/LunaSVGRuntime`), which is
built without system-font discovery and without file access (`<image href>` takes `data:` URIs).

## Removed from upstream

| Upstream path | Why it is not vendored |
|---|---|
| `Source/Core/FontEngineDefault/` | FreeType font engine. The engine's own `RmlFontEngine` (STBText stack: stb_truetype, HarfBuzz, SheenBidi) replaces it; `Core.cpp` includes it only under `RMLUI_FONT_ENGINE_FREETYPE`, which is never defined. |
| `Source/Lottie/`, `Include/RmlUi/Lottie/` | Lottie plugin on rlottie (last release 2020, a new texture per frame). The engine's own `<lottie>` element (`engine/core/htmlui/vector/`) runs on ThorVG. |
| `Source/Lua/`, `Include/RmlUi/Lua/`, `Include/RmlUi/Lua.h` | RmlUi's Lua bindings. The engine has its own browser-shaped Lua API (`UIDocument`, `UIElement`, `UIEvent`). |
| `Backends/` | Sample platform and renderer backends (GL, Vulkan, SDL, GLFW ...). The engine implements the interfaces on its own render abstraction (`engine/core/htmlui/`). |
| `Samples/`, `Utilities/` | Demos and tooling, not part of the library (the test build takes the sample shell and fonts from a pinned upstream clone). |
| `Tests/Data/VisualTests/`, `Tests/Data/*.rml`, `Tests/Output/`, `Tests/Tools/`, `Tests/readme.md` | RmlUi's visual tests (a window and stored captures); the browser references of phase 3 replace them. Their sources are vendored only so upstream's CMake files configure unchanged. |
| `CMake/`, every `CMakeLists.txt`, `CMakePresets.json` | Upstream build system; ours is `engine/libs/rmlui/CMakeLists.txt`. |
| `Dependencies/` | Third-party dependency scripts of the upstream build. |
| `Source/Debugger/.clang-format`, `readme.md`, `changelog.md`, `contributing.md`, `.appveyor.yml`, `.clang-format`, `.editorconfig`, `.gitignore`, `.github/` | Not source. |

## Patches

Every change to files under `engine/libs/rmlui` other than our build files above is a separate
commit prefixed `rmlui:` and listed here.

| ID | Commit | Files | Reason | Test |
|---|---|---|---|---|
| — | `ae671dec` | `Include/**`, `Source/Core/**`, `Source/Debugger/**`, `LICENSE.txt` | `rmlui: vendor RmlUi 6.3 (Core, Debugger)`: the phase-0 vendoring commit, byte for byte upstream. | phase-0 suites |
| P1-1 | `bf32a608` `rmlui: Event::PreventDefault` | `Include/RmlUi/Core/Event.h`, `Source/Core/Event.cpp`, `Source/Core/EventDispatcher.{h,cpp}`, `Include/RmlUi/Core/Element.h`, `Source/Core/Element.cpp`, `Include/RmlUi/Core/Context.h`, `Source/Core/Context.cpp`, `Source/Core/Elements/WidgetTextInput.cpp` | DOM `preventDefault()` for the Lua/C++ element API (phase 1 §3.14 patch 1): `Event::PreventDefault`/`IsDefaultPrevented`; the dispatcher skips the elements' default actions of a prevented event and reports it as consumed (`DispatchEvent` returns false; new `Element::DispatchEvent(EventId, params, bool* default_prevented)`); so `Context::ProcessKeyDown/Up/TextInput` and `ProcessMouseWheel` return "consumed" (a prevented wheel does not scroll; a prevented mousedown starts no drag and no double click); the text input produced by a prevented key down is dropped until the next key down or key up; `WidgetTextInput` ignores key downs/text inputs an ancestor's capture listener prevented | `engine/tests/htmlui/TestScriptEvents.cpp` (`PreventDefaultKeepsCheckboxUnchecked`, `PreventDefaultOnKeyDown`, `PreventDefaultOnWheelStopsScrolling`) |
| — | `e580f8d7` | `Source/SVG/**`, `Include/RmlUi/SVG/**` | `rmlui: vendor SVG plugin (6.3)`: a vendoring commit, the files are byte for byte upstream. The planned rasteriser patch (`rmlui: SVG plugin rasterises with ThorVG`, phase 1 §3.10.2) is **not made**: the phase-0 benchmark chose LunaSVG for SVG (`docs/html-ui/svg-benchmark.md`), which is the plugin's upstream rasteriser. | `TestVectorSvg.cpp` (`SvgPluginTest`), goldens `p1/svg_file`, `p1/svg_inline` |
| P2-1 | `9f85fff0` `rmlui: SVG source hook` | `Include/RmlUi/SVG/ElementSVG.h`, `Source/SVG/ElementSVG.cpp`, `Source/SVG/SVGCache.{h,cpp}` | Phase-2 hook H8 (`currentColor` in SVG follows the element's CSS `color`): `Rml::SVG::SourceHook` (`SetSourceHook`/`GetSourceHook`, `ProcessSource(element, path, source, Result{source_id, data, use_colour, colour})`, `IsSourceProperty(changed)`) lets the host rewrite an `<svg>` element's source (inline, or the `src` file it loads) before LunaSVG rasterises it and choose the colour the image is multiplied with; `ElementSVG` takes the new cache handle before releasing the old one (a colour change keeps the texture) and reloads when the hook names a changed property; `SVGCache::GetHandle` takes an optional colour override. The preprocessing itself is compat code (`htmlui::compat::SvgElementHook`, `SvgPreprocess`). Without a hook the plugin is upstream's. | `engine/tests/htmlui/compat/compat_svg_test.cpp` (`CompatSvgElementTest.*`), `TestVectorSvg.cpp` (`SvgPluginTest`) |
| P2-2 | `cc633f88` `rmlui: outer box-shadow blurred before it is clipped` | `Source/Core/GeometryBoxShadow.cpp` | An outer box-shadow is blurred and then clipped to outside the border box (CSS); 6.3 clipped first, so `box-shadow: 0 0 12px` glows drew nothing (p2-ua calibration). | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`); golden `p1/box_shadow` (matches Chromium's rendering of the fixture) |
| P2-3 | `ccb5b45e` `rmlui: failed var() substitution is unset in GetProperty` | `Source/Core/ElementStyle.cpp` | `Element::GetProperty` treats a property whose `var()` substitution fails as unset (inherited: the parent's value; others: the initial value), as `ComputeValues` already does; `font-family: var(--undefined)` gave an empty family (compat's inherit/unset sentinel, e.g. `button { font: inherit }`). | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) |
| P2-4 | `8bf41c2c` `rmlui: bare text in a flex container is an anonymous flex item` | `Source/Core/Layout/BlockFormattingContext.cpp`, `Source/Core/Layout/FlexFormattingContext.cpp` | Text directly inside a flex container is an anonymous flex item (CSS Flexbox §4): formatted as the inline content of its own anonymous block and keeps its line offset; it was a zero-sized item, so `display: flex` buttons and labels with bare text collapsed. | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) |
| P2-5 | `50081553` `rmlui: slider bar centered across an auto-height track` | `Source/Core/Elements/WidgetSlider.{h,cpp}` | A horizontal slider bar is centered across a track with no height of its own (an auto-height range track shrinks to its thumb and is centered: `appearance: none` ranges); the value fill is placed at the track's offset. | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) (`settings`) |
| P2-6 | `e48319bf` `rmlui: border radii scaled against the border box` | `Source/Core/GeometryBackgroundBorder.cpp` | Overlapping radii are scaled against the border box (CSS Backgrounds 3 §5.5), not the padding box: a bordered `border-radius: 50%` circle was a rounded square. | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) |
| P2-7 | `28a465ab` `rmlui: CSS paint order of decorators and borders` | `Source/Core/BoxShadowCache.{h,cpp}`, `Source/Core/Element.cpp`, `Source/Core/ElementBackgroundBorder.{h,cpp}` | An element with decorators (compat's background images) draws its border after them (background color and box shadows, then decorators, then the border; `ElementBackgroundBorder::RenderBorder`); a background image covered the border. Unchanged without decorators. | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) |
| P2-8 | `2b00264e` `rmlui: white-space-only text between inline-level boxes is a space` | `Source/Core/ElementText.cpp`, `Source/Core/Factory.cpp`, `Source/Core/Layout/BlockContainer.h`, `Source/Core/Layout/BlockFormattingContext.cpp`, `Source/Core/Layout/FlexFormattingContext.cpp`, `Source/Core/Layout/LayoutDetails.{h,cpp}`, `Source/Core/Layout/TableFormattingDetails.cpp` | CSS Text 3 §4.1.1: white-space-only text elements are kept (6.3 dropped them at parse time), collapse to one space unless a line starts there, and layout skips them where CSS removes them (line start and end in block containers, between flex items, between table parts); `<button>A</button> <button>B</button>` had no space. Applies to RML documents too (P2-13 keeps the Debugger's look); the HTML stage breaks lines inside tags so its RML adds no white space. | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`); `FullPathGoldenTest.Debugger` |
| P2-9 | `62521242` `rmlui: a select is as wide as its widest option` | `Source/Core/Elements/ElementFormControlSelect.cpp` | Intrinsic width: the widest option's text plus the arrow's and value's horizontal edges (was a fixed 128 dp). The commit is not fork-only: it also changes `engine/core/htmlui/compat/runtime/HtmlDetailsElement.cpp` (p2-runtime: a closed `details` hides its bare text content) and the golden `engine/tests/htmlui/golden/compat/engine/ua-defaults.png`; when re-applying the patch upstream, take only the `ElementFormControlSelect.cpp` hunk. | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) (`ua-defaults`) |
| P2-10 | `e78441ef` `rmlui: select baseline, textarea width` | `Include/RmlUi/Core/Elements/ElementFormControlSelect.h`, `Source/Core/Elements/ElementFormControlSelect.cpp`, `Source/Core/Elements/ElementFormControlTextArea.cpp` | A select's baseline is its value text's baseline (CSS), not its bottom margin edge; a textarea is `cols` x 0.6em plus a 15px scrollbar gutter wide (Chromium), not `cols` x the width of 'm'. | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) (`ua-defaults`) |
| P2-11 | `e0422556` `rmlui: sub/super offsets, scaled radii not rounded, select white space` | `Source/Core/Layout/InlineLevelBox.cpp`, `Source/Core/GeometryBackgroundBorder.cpp`, `Source/Core/Elements/ElementFormControlSelect.cpp` | `vertical-align: sub`/`super` shift by 1/5 and 1/3 of the parent's font size plus 1px (Chromium's amounts); radii scaled down to fit are not rounded (rounding up overlapped the curves: a 7px circle drew as a diamond); white space between a select's options is skipped (it was offered as an option and warned). | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) (`ua-defaults`) |
| P2-12 | `937dad03` `rmlui: type-less selectors do not match widget parts; select height` | `Source/Core/StyleSheetNode.cpp`, `Source/Core/Elements/ElementFormControlSelect.cpp` | A selector without a type (`*`, `:hover`, `[attr]`) does not match the elements widgets build (`selectvalue`, `selectarrow`, `selectbox`, `slidertrack`, `sliderbar`, `sliderprogress`, `sliderarrowdec/inc`, `scrollbar*`, `fill`, `selection`), as CSS selectors do not reach a control's internal parts: an author `* { margin: 0; padding: 0 }` reset stripped the range track and select value of their UA edges. A select is one line plus its value's vertical edges tall (Chromium's internal menulist padding, which author padding adds to). | `engine/tests/htmlui/compat/compat_golden_pages_test.cpp` (`CompatGolden/CompatGoldenPages.*`) (`settings`) |
| P2-13 | `6f7db097` `rmlui: the Debugger menu writes no white space between its buttons` | `Source/Debugger/MenuSource.h` | With P2-8 the white space between the menu's inline-block buttons became a space and the menu wrapped. | `FullPathGoldenTest.Debugger` |
| P2-14 | `683a251a` `rmlui: an element can lower and interrupt its top border` | `Include/RmlUi/Core/Element.h`, `Include/RmlUi/Core/RenderBox.h`, `Source/Core/Element.cpp`, `Source/Core/ElementBackgroundBorder.cpp`, `Source/Core/ElementUtilities.cpp`, `Source/Core/GeometryBackgroundBorder.{h,cpp}`, `Source/Core/MeshUtilities.cpp` | An HTML fieldset draws its top border through the middle of its legend and leaves a gap for it. `Element::GetBorderTopCut(BorderTopCut{inset, gap_begin, gap_end})` (virtual, false by default): `Element::GetRenderBox` (what backgrounds, borders, box shadows and decorators are generated from) starts `inset` px lower and carries the gap (`RenderBox::GetBorderTopGap`), which `GeometryBackgroundBorder::DrawBorder` leaves open in the top edge, within its straight part; `GetRenderBox(area, index, false)` is the box as laid out (the two clipping callers use it); `Element::DirtyBorderTopCut()` regenerates the background, border and decorators when an element's cut changed. The cut itself is compat code (`htmlui::compat::HtmlFieldsetElement`). Without an element that overrides it, nothing changes. | `engine/tests/htmlui/compat/compat_fieldset_test.cpp` (`CompatBorderTopGap.*`, `CompatFieldsetTest.*`); golden page `ua-defaults` |

## Phase 3 patches

Phase 3 (`docs/html-ui/phase-3-fork-patches.md`) patches the fork so standard CSS works natively.
Every row is a commit series prefixed `rmlui:` touching only `engine/libs/rmlui/**`; engine,
compat and test changes follow in separate commits. Upstream status: `upstream PR <url>`,
`upstreamable` or `fork-only` (with the reason). The unit tests are doctest files in
`engine/libs/rmlui/Tests/Source/UnitTests/`, run by `tools/htmlui/run-rmlui-tests.sh` (phase-3 spec §5.1).
P3-T* are p3-testkit's (the vendored tests), P3-N the patches of the other packages.

| ID | Commits | Files | Reason | Upstream status | Tests |
|---|---|---|---|---|---|
| P3-T1 | `260e661e` `rmlui: white-space-only text without a font face logs no missing-font warning` | `Source/Core/ElementText.cpp` | Follow-up to P2-8 (phase 3, p3-testkit): white-space-only text kept between elements made every indented RML document without a `font-family` warn "No font face defined" for its indentation; such text places no glyphs, text with glyphs still warns. Fork-only (with P2-8). | fork-only (with P2-8) | RmlUi unit tests (`tools/htmlui/run-rmlui-tests.sh`): `custom_properties.*`, `Element.ScrollIntoView`, `elementimage.preserve_ratio`, `Localization` count warnings |
| P3-T2 | `ee8db52e` `rmlui: RmlUi 6.3 unit tests follow the kept white-space text (P2-8)` | `Tests/Source/Common/TestsShell.{h,cpp}`, `Tests/Source/UnitTests/{Animation, Core, CustomProperties, DataBinding, Decorator, Element, ElementFormControlSelect, ElementImage, FlexFormatting, Layout, Selectors, XMLParser}.cpp` | 6.3's tests assumed white-space-only text is dropped: they walk element children (`TestsShell::GetElementChild`, `GetNumElementChildren`, `GetNextElementSibling`, the DOM's `children`), compare a document's inner RML without its surrounding white space, and `span:empty` no longer matches a span holding white space (as in browsers). Expectations only; no behaviour change. | fork-only (test expectations for P2-8) | the suite itself: 146 cases, 8306 assertions |
| P3-1 | `6d2e63b4` `rmlui: calc()/min()/max()/clamp() (upstream PR #983, commits 109df5ad, bea7264e, c6744d15)` (author Thomas Edvalson) | `Include/RmlUi/Core/{ComputedValues,DecorationTypes,StyleTypes,Transform,TypeConverter,Types,Unit,Variant}.h`, `Variant.inl`; `Source/Core/Calculation{,Parser,Resolver}.{h,cpp}` (new), `ComputeProperty.{h,cpp}`, `ComputedValues.cpp`, `DecoratorGradient.{h,cpp}`, `DecoratorNinePatch.cpp`, `DecoratorText.cpp`, `DecoratorTiled.cpp`, `Element.cpp`, `ElementAnimation.cpp`, `ElementHandle.cpp`, `ElementStyle.{h,cpp}`, `Filter{Basic,Blur,DropShadow}.*`, `FontEffect{Blur,Glow,Outline,Shadow}.cpp`, `GeometryBoxShadow.cpp`, `Layout/BlockContainer.cpp`, `Layout/TableFormattingContext.cpp`, `Property.cpp`, `PropertyDefinition.cpp`, `PropertyParser{Animation,BoxShadow,ColorStopList,Number,Transform}.*`, `PropertySpecification.cpp`, `StyleSheetContainer.cpp`, `StyleSheetParser.cpp`, `StyleSheetSpecification.cpp`, `Transform.cpp`, `TypeConverter.cpp`, `Variant.cpp`; `CMakeLists.txt` (`RMLUI_MATH_EXPRESSIONS` PUBLIC); `Tests/Source/UnitTests/Calculation{,Types}.cpp`, `Tests/Source/Benchmarks/Calculation.cpp` | CSS math functions: `calc()`, `min()`, `max()`, `clamp()` in every length, percentage, number, angle and time value (sizes, margins, paddings, insets, gaps, origins, flex-basis, line-height, transforms, box shadows, filters, gradients, font effects, transitions and animations), resolved at layout time; percentage-dependent min/max widths are ignored while measuring shrink-to-fit widths; shorthands reject trailing tokens. Applied onto 6.3 + P1-1, P2-1..P2-14 without a conflict; the `RMLUI_MATH_EXPRESSIONS` guards are kept (the engine defines it PUBLIC, and the Xcode targets define it next to `RMLUI_STATIC_LIB`: `tools/htmlui/xcode_sync_rmlui.py`). The CI workflow hunk of `bea7264e` is dropped. Not in the vendored subset: the PR's one-line change of `Tests/Source/UnitTests/CustomProperties.cpp` (a `BoxShadow` aggregate initialisation becomes member assignments; p3-testkit vendors the 6.3 test sources) and the CMake option of its root `CMakeLists.txt`. Compat: `ForkFeature::Calc` present (below). | upstream PR https://github.com/mikke89/RmlUi/pull/983 (open; source commits `109df5ad`, `bea7264e`, `c6744d15`, base `9cbb5440`) | `Tests/Source/UnitTests/Calculation.cpp`, `CalculationTypes.cpp` (36 + 1 cases, WPT-derived); `engine/tests/htmlui/compat/compat_calc_test.cpp`; `CompatCssRmlUiAccepts.EveryGoldenCaseWithThePresentFeatures` |
| P3-2 | `1b249600` `rmlui: widen PropertyId and Unit` | `Include/RmlUi/Core/{ID,Unit,PropertyIdSet,Types,TypeConverter,StyleTypes,ComputedValues}.h`, `Source/Core/{IdNameMap.h,StyleSheetSpecification.cpp,ElementStyle.cpp,Element.cpp}` | Room for phase 3: `PropertyId` is `uint16_t` (`MaxNumIds` 256; 6.3 used 101 of 128, phase 3 adds 86 longhands) and `PropertyIdSet` a bit set of 64-bit words whose iteration skips empty words; `Unit` is `uint64_t` with `VMIN VMAX CH EX TURN GRAD` (bits 31-36, in `LENGTH`/`ANGLE`; parsed and computed by p3-values), `STYLE_VALUE` (37) and `CSS_WIDE_KEYWORD` (38). Every longhand and shorthand of the phase-3 packages (spec Appendix B) is declared in `ID.h` at once, one group per package; an id not registered yet is reserved (`IdNameMap::ReserveUnnamed`: no name, nothing parses into it; custom ids still follow). `will-change` is registered (no computed value). The hot-spot files have one section per package (`StyleSheetSpecification::RegisterDefaultParsers/RegisterDefaultProperties`, `ElementStyle::ComputeValue` with a `case` per new id, `Element::OnPropertyChange`, `ComputedValues`' rare values and accessors, `StyleTypes.h`). | fork-only (capacity and the phase-3 id layout) | the RmlUi suite (unchanged results); `InitialValues.cpp` (every registered property); `engine/tests/htmlui/compat/compat_tables_test.cpp` (`CompatTables.EveryRmlUiPropertyAndShorthandHasARow` skips reserved ids) |
| P3-3 | `c9542f77` `rmlui: root font-size resolution` | `Source/Core/ComputeProperty.cpp`; `Tests/Source/UnitTests/RootFontSize.cpp` | An element without a parent resolves `%` and `em` font sizes (and `calc()` of them) against the initial font size, as CSS does for the root element; 6.3 gave 0 (`rem` on the document element already used the initial size). The document element is a child of the context's root element, so with compat's root mapping `html { font-size: 62.5% } body { font-size: 1.6rem }` gives 16px on the body in stylesheets (compat converts root `%`/`em` to px), inline styles and scripts alike. | upstreamable | `Tests/Source/UnitTests/RootFontSize.cpp`; `engine/tests/htmlui/compat/compat_root_font_size_test.cpp` |
| P3-4 | `57ce3d6c` `rmlui: CSS initial values` | `Include/RmlUi/Core/{ComputedValues,StyleTypes}.h`, `Source/Core/{StyleSheetSpecification.cpp,ElementStyle.cpp}`, `Source/Core/Layout/{LayoutDetails.h,BlockContainer.cpp}`; `Tests/Source/UnitTests/{InitialValues,Calculation}.cpp` | The defaults are the CSS initial values: `font-size` 16px (was 12px), `color` black (was white), `border-*-color` `currentcolor` (computed: a border color an element does not declare, or whose `var()` fails, is its `color` and follows it; registered as `black` until the colour parser knows `currentcolor`, p3-values; 6.3 computed white), `min-width`/`min-height` `auto` (computed `LengthPercentageAuto`; `auto` is 0 until p3-layout's flex automatic minimum), `opacity` not inherited (an element draws with its own opacity times its parent's, so a faded container still fades its children and nested opacities multiply, until p3-paint's groups), `visibility` inherited (showing or hiding a document restyles its elements once). `line-height: normal` (p3-values, `LineHeightNormal`) and the alignment `normal` keywords (P3-10) follow in their patches. The engine's phase-0/1/2 goldens and the Debugger did not change (their documents set colour and size). The 6.3 test `custom_properties.data_model` logs its invalid substitution twice (the document's visibility change restyles it): its expected warning count becomes 2 when the 6.3 tests are vendored. Compat: the property table's `rmluiDefaultDiffers` is false for `color`, `font-size`, `min-width`, `min-height` and the border colours (`initial`/`unset` write the sentinel and the fork computes the initial value). | fork-only (changes CSS initial values) | `Tests/Source/UnitTests/InitialValues.cpp` (registered default ≡ computed default for every property, the values, `currentcolor` borders, opacity, visibility); `Calculation.cpp` (media queries match WPT at 16px); `engine/tests/htmlui/compat/compat_css_golden_test.cpp` (`cascade-keywords`) |

### Fork features made present

`compat::ForkFeature` entries set in `engine/core/htmlui/compat/ForkFeatures.cpp` (`kPresent`), each
with the patch that implements it; `ForkFeatures.TableMatchesProbes` checks them against the fork.

| Feature | Patch | What compat does with it present |
|---|---|---|
| `Calc` | P3-1 | `calc()`, `min()`, `max()`, `clamp()` are type-checked and kept where RmlUi reads the value as written: properties without a rewrite, the `Sizing`, `Gap`, `FontSize`, `LineHeight`, `ZIndex` and `Opacity` rewrites, and custom properties (RmlUi substitutes them); `RcssGrammar::accepts(..., fork)` takes them for one numeric value of the right units. They stay folded statically, as without the feature, in the families a rewrite reads token by token or writes into compat decorator arguments (`border`, `background`, `box-shadow`, `clip-path`, logical properties, ...), in the custom property index the static rewrites read, and when other math functions (`round()`, `sin()`, ...) are inside; a percentage reduced there is reported as `HTMLUI-CSS-0006` (an approximation of that rewrite), not as a fork fallback. p3-values extends the fork's calc to the units and properties phase 3 adds. |

## Updating upstream

1. Copy the new tag over the vendored subset (same include/exclude lists).
2. Re-apply each patch above in order; drop those merged upstream.
3. Update the file list of the `rmlui-ios` / `rmlui-macos` Xcode targets if upstream added
   or removed sources.
4. Run RmlUi's own tests on the fork (`tools/htmlui/run-rmlui-tests.sh`; it pins the upstream
   tag it takes the font engine and sample shell from: update `UPSTREAM_TAG`/`UPSTREAM_COMMIT`).
5. Rebuild, run `jasperengine-htmlui-tests` on glcore and vulkan
   (`tools/build.sh htmlui-tests`, `HTMLUI_BACKEND=vulkan tools/build.sh htmlui-tests`),
   update this file.
