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
CMake and Xcode targets compile every `.cpp` of the vendored subset (CMake: `Source/Debugger`
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
  Tests/Source/UnitTests/*.cpp   (phase 3: the fork's own unit tests, P3 rows below; RmlUi's
  Tests/Source/Benchmarks/*.cpp   test infrastructure and 6.3 test sources are p3-testkit's)
```

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
| `Samples/`, `Tests/`, `Utilities/` | Demos, upstream test suites and tooling, not part of the library. |
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
`engine/libs/rmlui/Tests/Source/UnitTests/`, run standalone on the host (phase-3 spec §5.1).

| ID | Commits | Files | Reason | Upstream status | Tests |
|---|---|---|---|---|---|
| P3-1 | `6d2e63b4` `rmlui: calc()/min()/max()/clamp() (upstream PR #983, commits 109df5ad, bea7264e, c6744d15)` (author Thomas Edvalson) | `Include/RmlUi/Core/{ComputedValues,DecorationTypes,StyleTypes,Transform,TypeConverter,Types,Unit,Variant}.h`, `Variant.inl`; `Source/Core/Calculation{,Parser,Resolver}.{h,cpp}` (new), `ComputeProperty.{h,cpp}`, `ComputedValues.cpp`, `DecoratorGradient.{h,cpp}`, `DecoratorNinePatch.cpp`, `DecoratorText.cpp`, `DecoratorTiled.cpp`, `Element.cpp`, `ElementAnimation.cpp`, `ElementHandle.cpp`, `ElementStyle.{h,cpp}`, `Filter{Basic,Blur,DropShadow}.*`, `FontEffect{Blur,Glow,Outline,Shadow}.cpp`, `GeometryBoxShadow.cpp`, `Layout/BlockContainer.cpp`, `Layout/TableFormattingContext.cpp`, `Property.cpp`, `PropertyDefinition.cpp`, `PropertyParser{Animation,BoxShadow,ColorStopList,Number,Transform}.*`, `PropertySpecification.cpp`, `StyleSheetContainer.cpp`, `StyleSheetParser.cpp`, `StyleSheetSpecification.cpp`, `Transform.cpp`, `TypeConverter.cpp`, `Variant.cpp`; `CMakeLists.txt` (`RMLUI_MATH_EXPRESSIONS` PUBLIC); `Tests/Source/UnitTests/Calculation{,Types}.cpp`, `Tests/Source/Benchmarks/Calculation.cpp` | CSS math functions: `calc()`, `min()`, `max()`, `clamp()` in every length, percentage, number, angle and time value (sizes, margins, paddings, insets, gaps, origins, flex-basis, line-height, transforms, box shadows, filters, gradients, font effects, transitions and animations), resolved at layout time; percentage-dependent min/max widths are ignored while measuring shrink-to-fit widths; shorthands reject trailing tokens. Applied onto 6.3 + P1-1, P2-1..P2-14 without a conflict; the `RMLUI_MATH_EXPRESSIONS` guards are kept (the engine defines it PUBLIC, and the Xcode targets define it next to `RMLUI_STATIC_LIB`: `tools/htmlui/xcode_sync_rmlui.py`). The CI workflow hunk of `bea7264e` is dropped. Not in the vendored subset: the PR's one-line change of `Tests/Source/UnitTests/CustomProperties.cpp` (a `BoxShadow` aggregate initialisation becomes member assignments; p3-testkit vendors the 6.3 test sources) and the CMake option of its root `CMakeLists.txt`. Compat: `ForkFeature::Calc` present (below). | upstream PR https://github.com/mikke89/RmlUi/pull/983 (open; source commits `109df5ad`, `bea7264e`, `c6744d15`, base `9cbb5440`) | `Tests/Source/UnitTests/Calculation.cpp`, `CalculationTypes.cpp` (36 + 1 cases, WPT-derived); `engine/tests/htmlui/compat/compat_calc_test.cpp`; `CompatCssRmlUiAccepts.EveryGoldenCaseWithThePresentFeatures` |

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
4. Rebuild, run `jasperengine-htmlui-tests` on glcore and vulkan
   (`tools/build.sh htmlui-tests`, `HTMLUI_BACKEND=vulkan tools/build.sh htmlui-tests`),
   update this file.
