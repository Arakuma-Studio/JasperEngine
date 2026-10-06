# Phase 3: RmlUi fork patches

Binding inputs: `docs/html-ui/DESIGN.md` (architecture, research facts) and `docs/html-ui/BRIEF.md`
(rules for implementers). This file details phase 3. Every `file:line` below refers to
**RmlUi 6.3** (tag `6.3`, commit `ba95ffe8`, pristine copy at `/tmp/htmlui-src/rmlui-6.3`) unless
it says otherwise. In the fork the same files live under `engine/libs/rmlui/`; line numbers drift
once patches land, so search for the quoted symbol when a number no longer matches.

Phase 3 starts when phases 0–2 are merged into `html-ui`: RmlUi 6.3 vendored at
`engine/libs/rmlui` (bookkeeping in `docs/html-ui/FORK.md`), the sokol render backend with
layers/filters/clip masks, the STBText font engine, `UIDocument` integration, the Lua API, ThorVG,
and the compatibility layer `engine/core/htmlui/compat/` with its per-property feature flags,
diagnostics and machine-readable property tables.

## 1. Outcome

When phase 3 is merged:

1. The CSS that coding agents and web developers actually write is handled **natively by the
   RmlUi fork**: `calc()/min()/max()/clamp()`, `border: 1px solid red` and every border style
   (dashed and dotted included, with rounded corners), `%` and elliptical radii, `::before`,
   `::after` (and list markers), `:root`, `:is()`, `:where()`, `:has()`, correct
   `:focus`/`:focus-within`/`:focus-visible`, `!important`, `inherit`/`initial`/`unset`/`revert`/`all`,
   `position: fixed` relative to the viewport, `position: sticky`, `opacity` as a group,
   `cubic-bezier()`/`steps()`/`linear()` and CSS easing keywords, `ms`, transitions on any value
   change (inline styles and scripts included), animation longhands with real fill modes and
   pausing, CSS Grid (a C++ port of Taffy's algorithm), and the list of commonly used gaps in §2.3.
2. For every feature that became native, the compat layer's rewrite is **switched off** through
   its feature flag, its property table says `native`, and its diagnostics no longer report a
   rewrite. The rewrite code stays (flag off) so a regression can be bisected by flipping it.
3. Rendering and layout of the fixtures in `engine/tests/htmlui/fixtures/p3/` match **headless
   Chrome** reference renders (pixel comparison with an anti-aliasing tolerant metric, and
   element-box comparison within 1 px), and Taffy's browser-generated layout fixtures for grid,
   flex, block and float pass (§5.3).
4. RmlUi's own unit test suite (146 cases at 6.3, plus the cases added by phase 3) passes, built
   standalone from the fork (§5.1).
5. Each patch is an isolated `rmlui:` commit series in upstream style (clang-format of the fork,
   RmlUi naming, doctest unit tests in `Tests/Source/UnitTests/`), recorded in `FORK.md`.

## 2. Scope

### 2.1 In scope (implemented in the fork)

| # | Area | Section |
|---|---|---|
| 0 | Fork groundwork: HTML root model (`<html>` is the document element), CSS initial values, `PropertyId`/`Unit` capacity, shorthand machinery (`AnyOrder`, `Custom`), style-value variant, layout-children iteration, intrinsic sizing API, containing-block override, alignment keywords, `order`, `display`/`position` enum widening | §3.1 |
| 1 | `calc()`, `min()`, `max()`, `clamp()` — upstream PR #983 cherry-picked, then extended to every new length/percentage property | §3.2 |
| 2 | Values: new units, CSS colors (alpha 0–1, space syntax, all named colors, `currentcolor`, `hwb()`, `color-mix()`), `font-family` lists, `font` shorthand, `line-height: normal`, font-size/weight keywords, logical properties | §3.3 |
| 3 | Cascade: `!important` (stylesheets and inline), `inherit`/`initial`/`unset`/`revert`/`revert-layer`, `all`, inline style replacement semantics | §3.4 |
| 4 | Selectors: `:root`, `:is()`, `:where()`, `:has()`, `:focus`/`:focus-within`/`:focus-visible`, `:enabled`/`:disabled`/`:checked` parity, `:link`/`:any-link`, attribute selector `i`/`s` flags, `:nth-child(An+B of S)` | §3.5 |
| 5 | `::before`, `::after`, `::marker`; `content` (strings, `attr()`, `url()`, `counter()`, `counters()`, quotes keywords); counters; `display: list-item`, `list-style-*` | §3.6 |
| 6 | Borders: per-side `border-style` (all ten values), `border`/`border-*` shorthands in any order, dashed/dotted/double/groove/ridge/inset/outset geometry incl. rounded corners, `%` and elliptical radii with CSS overlap scaling, `outline`, `outline-offset` | §3.7 |
| 7 | Group opacity, stacking-context parity (`opacity`, `position: fixed/sticky`, `isolation`, `z-index` only where CSS applies it), inherited `visibility` with `collapse`, canvas background propagation | §3.8 |
| 8 | Native `background-*` (image layers: `url()`, linear/radial/conic/repeating gradients; size, position, repeat, origin, clip, attachment), CSS-syntax `mask-image`, `object-fit`, `object-position` | §3.9 |
| 9 | Text: float `letter-spacing`, `word-spacing`, Unicode `text-transform` incl. `capitalize`, `white-space` (`break-spaces`) and `white-space-collapse`/`text-wrap-mode`, `overflow-wrap`, `word-break: keep-all`, `text-overflow` parity, `text-decoration-*` longhands, native `text-shadow`, `text-align: start/end`, `user-select`, `cursor`, `pointer-events`, rounded font sizes | §3.10 |
| 10 | Positioning: `position: fixed` relative to the viewport, `position: sticky`, `overflow: clip`, overflow propagation from `<body>` to the viewport, `overflow-x/y` value fix-up | §3.11 |
| 11 | Box/flex parity: flex `order`, `flex-basis: content`, `min-width/min-height: auto` (flex automatic minimum size), `aspect-ratio`, `width/height/min-*/max-*: min-content | max-content | fit-content | fit-content(<length-percentage>)`, parent/child and empty-block margin collapsing, `display: contents`, `display: flow-root` parity, alignment keywords `start/end/left/right/normal/stretch/self-start/self-end` for flex | §3.12 |
| 12 | Transitions/animations: `cubic-bezier()`, `steps()`, `linear()`, all CSS easing keywords, `s`/`ms`, `0s`, transition and animation longhands, transitions on any computed-value change, `transition-behavior: allow-discrete`, `animation-direction/fill-mode/iteration-count/play-state` parity, per-keyframe timing functions, interpolation of `box-shadow`/`text-shadow`/`background-position`, individual transform properties `translate`/`rotate`/`scale` | §3.13 |
| 13 | CSS Grid: `display: grid | inline-grid`, all template/placement/auto properties and shorthands, `place-*`, `justify-items/self`, gaps, intrinsic sizing, implicit tracks, dense auto-placement, absolutely positioned grid children with grid-area containing blocks, scrolling | §3.14 |

### 2.2 Out of scope (diagnosed, not implemented) — with justification

Each item is reported by the compat diagnostics (phase 2 machinery) with `file:line:column` and the
listed replacement. Phase 3 packages add or update the diagnostic entries for the items they own.

| Feature | Why not implemented | Diagnostic replacement hint |
|---|---|---|
| `writing-mode`, vertical text, `direction: rtl` **layout mirroring** (grid/flex/block axis reversal) | RmlUi lays out block and flex boxes LTR only; supporting mirrored layout for grid alone would be inconsistent, and a full RTL layout pass touches every formatting context. Bidi *text* already works (SheenBidi, `-rmlui-direction`). Taffy fixtures with `rtl`/`writing-mode` are excluded from §5.3. | "Mirrored layout is not supported; mirror the layout explicitly (e.g. `flex-direction: row-reverse`)." |
| `subgrid`, `masonry` | Not implemented by Taffy either; no stable browser-reference semantics across engines (masonry). Tier 3 or below. | "Use nested grids with explicit track sizes." |
| `@container` queries, `container-type` | Requires a layout→style→layout fix-point loop RmlUi's update order cannot express without a second full style pass per frame; Tier 3, low game-UI use. | "Use media queries (canvas size) or classes set from Lua." |
| `::first-line`, `::first-letter`, `::selection` (non-input), `::placeholder`, `::-webkit-scrollbar*` | `::first-line/letter` need fragment-level styling inside the inline formatter; the other three are mapped by the compat layer to RmlUi's own widget elements (phase 2) and stay there. | first-line/letter: "Wrap the text in a `<span>`." |
| `-webkit-line-clamp` / `line-clamp` | Needs multi-line ellipsis in `InlineContainer` and the legacy `display: -webkit-box` model; rare in game UI. | "Use `max-height` + `overflow: hidden`, or single-line `text-overflow: ellipsis`." |
| Static text selection (`user-select: text/all` on non-input content) | RmlUi has selection only inside text inputs; implementing selection for arbitrary inline content is a separate feature, not a CSS parity fix. `user-select: none/auto` are native (they match current behavior). | "Text outside inputs is not selectable." |
| `scroll-snap-*`, `scroll-behavior: smooth` (as CSS), `overscroll-behavior` values beyond `auto/contain` | Needs snap points in `ScrollController`; game menus navigate with focus/gamepad (`nav-*`). Smooth scrolling is available from Lua (`scrollIntoView{behavior="smooth"}`). | "Use focus navigation or Lua scrolling." |
| Multi-column layout (`columns`, `column-count`) | Tier 3, new fragmentation machinery. | "Use CSS Grid or flex-wrap." |
| `@layer` cascade layers, `revert-layer` with layers | No cascade layers in the parser; `revert-layer` without layers behaves as `revert` (CSS-correct when no layers exist), `@layer` blocks are diagnosed by compat. | "Remove `@layer`; order rules by specificity." |
| CSS counters in `counter()` styles other than the list in §3.6; `@counter-style` | Long tail. | "Use one of: decimal, decimal-leading-zero, lower/upper-alpha/latin, lower/upper-roman, lower-greek, disc, circle, square, none." |
| `text-decoration-style: wavy`, `text-emphasis`, `text-indent: hanging/each-line`, `hyphens: auto` | Long tail; `wavy` renders as `solid`. | listed per property |
| `mix-blend-mode`, `background-blend-mode` | Needs blend modes beyond `Blend/Replace` in `RenderInterface::CompositeLayers` (RenderInterface.h:98-122) on all sokol backends; phase 5 (effects) owns new compositing modes. | "Not supported; use opacity or a pre-blended image." |
| `background-clip: text` | Needs text as a mask layer per text run; phase 5 effects library provides a shader decorator for gradient text. | "Use the gradient-text effect (phase 5)." |
| `display: ruby*`, table `border-collapse: collapse` parity, `caption-side` | Tables are rare in game UI; RmlUi's table model is kept as is. | per property |
| `font-variant*`, `font-stretch`, `font-feature-settings`, `font-variation-settings` | Requires OpenType feature plumbing through `FontEngineInterface`; the STBText engine (phase 0) shapes with default features. | "Use a font with the desired glyphs as the default." |
| `cursor: url(...)` custom images | Platform cursor images are not exposed by `System` on all platforms; keyword fallbacks in the list are honoured. | "Custom cursor images are not supported; the fallback keyword is used." |

### 2.3 Decisions on the "other gaps" list (item 11 of the brief)

| Gap | Decision | Where |
|---|---|---|
| flex `order` | Fork (registration and layout-order iteration in groundwork, flex use in p3-layout, grid use in p3-grid) | §3.1.6, §3.12 |
| `flex-basis: content` | Fork | §3.12 |
| `line-height: normal` | Fork; also becomes the initial value | §3.3 |
| `rem` relative to `<html>` | Fork, by the HTML root model (the document element is `<html>`) | §3.1.1 |
| `aspect-ratio` | Fork (block, flex, grid, absolute, replaced) | §3.12 |
| `outline`, `outline-offset` | Fork | §3.7 |
| `text-shadow` | Fork, rendered by `ElementText` (independent of font-engine effects) | §3.10 |
| `background-image: url()` + `background-size/position/repeat` | Fork | §3.9 |
| `object-fit`, `object-position` | Fork (`ElementImage`, SVG/Lottie elements of phase 1 use the same helper) | §3.9 |
| `fit-content`/`min-content`/`max-content` sizing keywords | Fork | §3.12 |
| `inset` shorthands | Already native in 6.3 (`StyleSheetSpecification.cpp:313`); logical `inset-inline/block` added as shorthands | §3.3 |
| `rgba()` alpha 0–1 | Fork; compat rewrite removed | §3.3 |
| `font-family` lists | Fork normalizes to a canonical comma list; the engine font engine resolves the fallback chain (one-line contract with phase 0/1, §3.3.4) | §3.3 |
| `text-transform` | Fork (Unicode simple case mapping + `capitalize`) | §3.10 |
| `letter-spacing`/`word-spacing` | Fork (float px; `word_spacing` added to `TextShapingContext`, applied by the engine font engine) | §3.10 |
| `white-space` values | Fork (`break-spaces`, longhands) | §3.10 |
| `overflow-wrap`/`word-break` | Fork | §3.10 |
| `text-overflow` | Already native (`clip`, `ellipsis`, string); parity fix: applies to block containers with `overflow` other than `visible`, as CSS | §3.10 |
| `user-select` | Registered natively; `none`/`auto` native, `text`/`all` diagnosed (§2.2) | §3.10 |
| `pointer-events` | Native in 6.3 (`none`, `auto`); SVG-only values diagnosed | §3.10 |
| `cursor` | Fork parses CSS cursor lists and passes the first supported keyword to `SystemInterface::SetMouseCursor` | §3.10 |
| `visibility` | Fork: inherited, `collapse`, children can re-show | §3.8 |
| `z-index` stacking parity | Fork | §3.8 |
| `transform-origin` | Native; fork fixes keyword order (`top left`), 3-value form, `calc()` | §3.3 |
| `will-change` | Ignored: registered as a no-op property so it neither fails nor rewrites; no diagnostic (it has no visual effect in CSS either) | §3.1.3 |
| `box-sizing` default | `content-box` stays the initial value (CSS-correct); nothing to patch | — |
| margin collapsing | Fork: parent/first-child, parent/last-child, empty-block collapse-through (CSS 2.1 §8.3.1) | §3.12 |
| `vertical-align` | Native; parity fix for `%`/length on inline-blocks and `middle` with x-height from font metrics | §3.10 |
| `filter`/`backdrop-filter` | Native in 6.3; fork parity fixes: argument defaults (`blur()` = 0, `drop-shadow` color default `currentcolor`), `%` amounts, space-separated lists with `url()` diagnosed, filter does not clip overflowing descendants (§3.8.4) | §3.8 |

## 3. Detailed design

### 3.0 Conventions for every patch

- **Commits.** Each patch is a series of commits prefixed `rmlui:` touching only
  `engine/libs/rmlui/**`, followed by separate non-`rmlui:` commits for engine/compat/test changes.
  The `rmlui:` commits build and pass RmlUi's unit tests on their own (bisectable).
- **Style.** Format fork files with the fork's `.clang-format` (RmlUi's). RmlUi naming
  (`PascalCase` functions, `snake_case` members), no engine headers, no exceptions, `Rml::` types
  (`String`, `Vector`, `SharedPtr`, `UniquePtr`).
- **FORK.md.** Every patch adds a row: `id | commits | files | reason | upstream status | tests`.
  Upstream status is one of `upstream PR <url>` (for #983), `upstreamable` or `fork-only`
  (with a reason, e.g. "changes CSS initial values").
- **Tests.** Every patch adds doctest cases to `engine/libs/rmlui/Tests/Source/UnitTests/` (parse
  → specified value → computed value → used value/layout/geometry), and golden fixtures to
  `engine/tests/htmlui/fixtures/p3/<area>/` with browser references (§5.2).
- **Compat flag flip.** For each feature listed in Appendix A: set the phase-2 feature flag to
  "native" (rewrite off), change the property-table row to `native`, update the compat unit tests
  whose expected RML/RCSS output changes, and add a consistency test (§5.4) that parses the
  table's sample declaration with `StyleSheetSpecification::ParsePropertyDeclaration` and checks the
  computed value. Flag names in Appendix A are the expected phase-2 keys; when phase 2 used
  different names, use phase 2's names (phase 2 owns them).
- **Hot spots.** `ID.h`, `StyleSheetSpecification.cpp`, `ComputedValues.h`, `StyleTypes.h`,
  `ElementStyle.cpp` (`ComputeValue`), `Element.cpp` (`OnPropertyChange`), `Unit.h`,
  `PropertySpecification.cpp` are pre-structured by groundwork (§3.1.2) into per-package
  sections. Wave-1 packages append **inside their own section only**.

---

### 3.1 Groundwork (package `p3-base`)

#### 3.1.1 HTML root model

**Problem.** RmlUi instantiates the document element from the `<body>` tag
(`Factory.cpp:180` registers `"body"` → `ElementDocument`; `XMLNodeHandlerBody::ElementStart`
applies `<body>` attributes to the document). There is no `<html>` element, so `html {…}` and
`body {…}` rules land on one element, `rem` is relative to `body`'s font-size
(`ComputeProperty.cpp:87-118`, `document_values` = owner document), and `:root` has no sane
meaning. `html{font-size:62.5%} body{font-size:1.6rem}` (very common) yields a self-referential
font size.

**Decision.** The document element is `<html>`; `<body>` is an ordinary child element.

- RmlUi already supports a configurable document tag: `Context::SetDocumentsBaseTag`
  (`Context.h:279`, `Context.cpp:1616`), used by `Context::LoadDocument` (`Context.cpp:287`),
  `ElementDocument` reload (`ElementDocument.cpp:300`) and `Factory::InstanceElementText`
  (`Factory.cpp:373`).
- Fork changes:
  - `Factory.cpp:180`: also register `"html"` with the generic `ElementDocument` instancer.
  - New `XMLNodeHandlerHtml` (copy of the attribute-application half of
    `XMLNodeHandlerBody::ElementStart`): registered for `"html"`; when the parse frame's element is
    the owner document, apply the `<html>` attributes (`lang`, `class`, `id`, `style`, `data-*`) to
    the document and return it.
  - `XMLNodeHandlerBody::ElementStart`: when the owner document's tag is `"html"` and the parse
    frame's element is the document, instance a real element with
    `Factory::InstanceElement(document, "*", "body", attributes)`, append it, apply the `template`
    logic to it, `PushDefaultHandler()`, and return it. With the default base tag (`"body"`) the
    existing behavior is unchanged (upstream compatible).
  - `ComputeFontsize` (`ComputeProperty.cpp:96-112`): for the root (`parent_values == nullptr`),
    `%` and `em` resolve against the initial font size instead of returning 0; `rem` on the root
    uses the initial value (already the case at `:105-108`).
- Engine changes (additive, phase-1 file): `HtmlUIContext` (or wherever phase 1 creates the
  `Rml::Context`) calls `context->SetDocumentsBaseTag("html")`.
- Compat contract (phase-2 emitter, changed here): the RML produced from an HTML file is
  `<html …attrs><head>…</head><body …attrs>…</body></html>` (XML). `html` and `body` type
  selectors are passed through unchanged; any phase-2 rewrite that merged `html`/`body`/`:root`
  selectors is switched off (Appendix A, `root-merge`). The user-agent stylesheet gains
  `html { display:block; position:absolute; left:0; top:0; right:0; }` (the document is
  `position:absolute` in RmlUi, `ElementDocument.cpp:139`, so these insets make it span the
  viewport width like the CSS root) and `body { display:block; margin:8px; }` as in browsers.
- `rem` now refers to `<html>` automatically (RmlUi resolves `rem` against the owner document's
  font size, `ElementStyle.cpp:942`), and `DirtyPropertiesWithUnitsRecursive(Unit::REM)` on
  document font-size change (`ElementDocument.cpp:562-564`) already invalidates correctly.

#### 3.1.2 CSS initial values

RmlUi's defaults differ from CSS initial values. Fork-only change (documented in FORK.md), with
affected RmlUi unit-test expectations updated in the same commit:

| Property | RmlUi 6.3 default (`StyleSheetSpecification.cpp`) | New initial value |
|---|---|---|
| `font-size` | `12px` (`:356`, `ComputedValues.h:98`) | `16px` (`medium`) |
| `color` | `white` (`:346`) | `black` (CSS `canvastext` in a light scheme) |
| `border-*-color` | `black` registered, white computed (`:288-291`, `ComputedValues.h:79`) | `currentcolor` (§3.3.3) |
| `line-height` | `1.2` (`:330`) | `normal` (§3.3.6) |
| `min-width`/`min-height` | `0px` (`:323`,`:327`) | `auto` (§3.12.3) |
| `justify-content`, `align-content` | `flex-start`/`stretch` (`:416`,`:426`) | `normal` (§3.1.7) |
| `align-items`, `justify-items` | `stretch` | `normal` |
| `opacity` | inherited (`:351`) | not inherited (§3.8) |
| `visibility` | not inherited (`:339`) | inherited (§3.8) |

`DefaultComputedValues()` (`ComputeProperty.cpp:14`) and the `CommonValues`/`InheritedValues`/
`RareValues` constructors (`ComputedValues.h:19-184`) are changed to match. A unit test asserts
that for every registered property the computed value of the registered default string equals
the default-constructed computed value (catches future drift).

#### 3.1.3 Capacity and schema

- `PropertyId` (`ID.h:43`) becomes `enum class PropertyId : uint16_t` with `MaxNumIds = 256`
  (6.3 uses 101 of 128; phase 3 adds ~70 longhands, Appendix B). `PropertyIdSet`
  (`PropertyIdSet.h:21`) follows automatically (`std::bitset<256>`). Check every
  `static_cast<uint8_t>`/`uint8_t` storage of a `PropertyId` (grep `uint8_t` near `PropertyId`).
- `ShorthandId` stays `uint8_t` (max `0xff`; ~30 used + ~30 added).
- `Unit` (`Unit.h:8`) becomes `enum class Unit : uint64_t`. PR #983 takes bit 30
  (`CALCULATION`); groundwork adds `VMIN`, `VMAX`, `CH`, `EX`, `TURN`, `GRAD` (bits 31–36),
  `STYLE_VALUE` (bit 37) and `CSS_WIDE_KEYWORD` (bit 38), and extends the masks: `LENGTH` +=
  `VMIN|VMAX|CH|EX`, `ANGLE` += `TURN|GRAD`. Parsing/computing of the new units is p3-values'
  job (§3.3.1); groundwork only reserves them so wave-1 packages compile against the final enum.
- All new `PropertyId`/`ShorthandId` entries of Appendix B are added to `ID.h` in one commit,
  grouped by package with a comment header per group, so the enum never conflicts.
- Hot-spot sections: groundwork inserts per-package section headers (ordinary grouping comments
  such as `// Grid layout`) at the end of: the `PropertyId` enum, the `ShorthandId` enum,
  `StyleSheetSpecification::RegisterDefaultProperties` (`StyleSheetSpecification.cpp:250-436`),
  `RegisterDefaultParsers` (`:225-246`), `RareValues` fields and accessors
  (`ComputedValues.h:130-376`), the `ElementStyle::ComputeValue` switch (`ElementStyle.cpp:1050`;
  each new id gets a `case` with `break;` in its package section so `-Wswitch` stays clean), and
  `Element::OnPropertyChange` (`Element.cpp:1880-1990`).
- `will-change` is registered (`keyword`/`string`, no computed value, not forcing layout).

#### 3.1.4 Shorthand machinery

`ParseShorthandDeclaration` (`PropertySpecification.cpp:284-471`) only knows
`FallThrough/Replicate/Box/RecursiveRepeat/RecursiveCommaSeparated/Flex`
(`PropertySpecification.h:17-31`). CSS shorthands such as `border`, `outline`, `background`,
`transition`, `animation`, `font`, `grid-area`, `list-style`, `text-decoration` need any-order
matching, reset of omitted longhands to their initial values, and `/` separators. Groundwork adds:

- `ShorthandType::AnyOrder`: whitespace-split; each token is tried against every not-yet-assigned
  item in declaration order; a token that matches no item fails the declaration; omitted items are
  set to their initial value (the registered default parsed through the definition). Also used
  to fix `border-top` etc. (FallThrough today does not reset omitted longhands and requires order).
- `ShorthandType::Custom` + `StyleSheetSpecification::RegisterShorthandParser(ShorthandId,
  ShorthandParserFunction)` where `using ShorthandParserFunction = bool (*)(PropertyDictionary& out,
  const ShorthandDefinition&, const String& value, const PropertySpecification&)`. Custom
  shorthands receive the raw value (after `var()` detection, `PropertySpecification.cpp:297-301`)
  and are responsible for setting every longhand (omitted → initial). Each wave-1 package
  implements its custom shorthands in its own source file.
- Comma-list longhands (layered `background-*`, `transition-*`, `animation-*`) use a shared helper
  `SplitCommaList(const String&, StringList&)` that is parenthesis- and quote-aware
  (`StringUtilities::ExpandString(..., ',', '(', ')')` plus quote handling).
- Every shorthand rejects unconsumed trailing tokens (PR #983 adds this for the existing types).

#### 3.1.5 Style-value variant

New complex value types (grid track lists, shadows, background layers, content lists, counter
lists, easing lists) would each need a `Variant` type, `Set/Get` overloads and a `TypeConverter`
(`Variant.h:21-48`, `Variant.cpp:54-385`, `TypeConverter.h:97-114`) — a conflict magnet.
Groundwork adds one generic slot:

```cpp
// Include/RmlUi/Core/StyleValue.h
class RMLUICORE_API StyleValue {
public:
    virtual ~StyleValue();
    virtual String ToString() const = 0;                               // CSS serialization
    virtual bool Equals(const StyleValue& other) const = 0;            // same dynamic type assumed
    virtual SharedPtr<const StyleValue> Interpolate(const StyleValue& to, float alpha,
        Element& element) const;                                       // nullptr = discrete
    virtual Units GetUnitDependencies() const;                         // for DirtyPropertiesWithUnits
};
using StyleValuePtr = SharedPtr<const StyleValue>;
```

`Variant::STYLEVALUEPTR`, `Unit::STYLE_VALUE`, `Property::Get<StyleValuePtr>()`,
`TypeConverter<StyleValuePtr, String>` (calls `ToString`), `ElementAnimation` support
(`InterpolateProperties`, `ElementAnimation.cpp:199`: calls `Interpolate`; valid-unit gate
`:640-648` adds `STYLE_VALUE`), and `ElementStyle::DirtyPropertiesWithUnits`
(`ElementStyle.cpp:809-820`) consults `GetUnitDependencies()`. Packages subclass it in their own
files and expose typed accessors (`GetLocalProperty(id, StyleValuePtr())` + `static_cast`).

#### 3.1.6 Layout children, `order`, `display`/`position` widening

- `Style::Display` (`StyleTypes.h:31-46`, 14 values, stored in 4 bits at `ComputedValues.h:38`)
  gains `Grid`, `InlineGrid`, `ListItem`, `Contents` and the bitfield becomes 5 bits; the keyword
  list at `StyleSheetSpecification.cpp:306-307` is extended in enum order. `GetOuterDisplayType`
  (`BlockFormattingContext.cpp:62-85`) maps `Grid`/`ListItem` → block-level, `InlineGrid` →
  inline-level. The formatting-context dispatch for grid is p3-grid's.
- `Style::Position` (`StyleTypes.h:47`, 2 bits at `ComputedValues.h:39`) gains `Sticky`
  (3 bits); until p3-layout lands, `Sticky` is laid out as `Relative` (every `Position::Relative`
  check gets `|| Sticky`: `BlockContainer.cpp:136,157,179,253`, `FlexFormattingContext.cpp:270`,
  `LayoutDetails.cpp:176`, `Element.cpp:720`).
- `order` (integer, initial 0, not inherited, forces layout) is registered with computed accessor
  `order()`.
- New `LayoutDetails::GetLayoutChildren(Element* parent, LayoutChildList& out, bool apply_order)`
  returns the boxes to lay out in order: `::marker` (if any, §3.6), `::before`, DOM children
  (`GetNumChildren()` range), `::after`; `display: contents` children are replaced by their own
  layout children recursively; when `apply_order` is true (flex and grid containers) the list is
  stable-sorted by `order`. `BlockFormattingContext.cpp:119,152,174`,
  `FlexFormattingContext.cpp:251-257` and `TableFormattingDetails.cpp:16-200` switch to it. The
  pseudo-element pointers are read through `Element::GetPseudoElement(PseudoElementType)`, which
  groundwork declares (returns `nullptr` until p3-cascade implements generation).
- `Element::AddChildrenToStackingContext` (`Element.cpp:2384-2392`) iterates in the same paint
  order and does not mark pseudo-elements as non-DOM atomic units (`Element.cpp:2475`).

#### 3.1.7 Alignment keywords

`StyleTypes.h:127-133` enums are extended (appending to keep `AlignSelf == AlignItems + 1`,
static_assert at `FlexFormattingContext.cpp:297-303`):

- `AlignItems`/`AlignSelf`/`JustifyItems`/`JustifySelf`: `Normal, Start, End, SelfStart, SelfEnd,
  Left, Right` (justify only), `FirstBaseline, LastBaseline` (`baseline` = `FirstBaseline`),
  `Legacy` (justify-items only, treated as `Normal`). `safe`/`unsafe` prefixes are parsed and
  stored as a flag.
- `AlignContent`/`JustifyContent`: `Normal, Start, End, Left, Right, Stretch`.
- New properties `justify-items` (initial `legacy`, which behaves as `normal`) and
  `justify-self` (initial `auto`), shorthands `place-items`, `place-self`, `place-content`
  (`Custom`: one value sets both, two values set align then justify).
- Multi-word values (`safe center`, `last baseline`) cannot be expressed with the keyword parser's
  value aliases (`PropertyDefinition.cpp:37-48` maps single tokens), so all alignment properties
  (`align-*`, `justify-*`) use a small dedicated parser `alignment` registered by groundwork; it
  stores the keyword plus the `safe`/`unsafe` flag.
- Flex mapping of the new keywords is p3-layout's (§3.12.7); grid's is p3-grid's.

#### 3.1.8 Intrinsic sizing API

RmlUi 6.3 has only a max-content measurement (`LayoutDetails::GetShrinkToFitWidth`,
`LayoutDetails.cpp:217-262`, which formats at `containing_block.x + 10000` and clamps the result to
the available width at `:255-260`) and no min-content measurement anywhere. Groundwork adds:

- `virtual float LayoutBox::GetMinContentWidth() const` next to `GetShrinkToFitWidth`
  (`LayoutBox.h`), implemented by `BlockContainer` (max over children of their min-content widths
  and floats), `InlineContainer` (widest unbreakable segment: formatting at available width 0 makes
  each line hold one unbreakable fragment, `LineBox.cpp:78`; the widest line is the result),
  `FlexContainer` (`FlexFormattingContext::GetMinContentSize`, §3.12) and `TableWrapper`.
- `LayoutDetails::GetMinContentWidth(Element*, Vector2f containing_block)` and
  `LayoutDetails::GetMaxContentWidth(Element*, Vector2f containing_block)` (the latter = shrink-to-fit
  without the clamp), both returning **border-box** widths, plus
  `LayoutDetails::GetContentHeightForWidth(Element*, float border_box_width, Vector2f containing_block)`
  (formats with a definite width, returns border-box height — the pattern used by
  `FlexFormattingContext.cpp:336-342`).
- A per-`LayoutEngine::FormatElement` memo (`LayoutMeasureCache`, keyed by element + mode + width
  bits) so grid/flex measuring does not reformat the same element repeatedly; cleared at the start
  of each `FormatElement` (`LayoutEngine.cpp:10-31`). RmlUi has no layout cache today.

#### 3.1.9 Containing-block override

`Element::UpdateOffset` (`Element.cpp:2245-2289`) and `Element::GetContainingBlock`
(`Element.cpp:710-735`) re-resolve insets against the offset parent's padding box, so a grid area
or the viewport cannot be a containing block. Groundwork adds:

- `struct AbsoluteElement` (`ContainerBox.h:65`) gets `Optional<Rectanglef> containing_block`
  (relative to the offset parent's border box); `ContainerBox::AddAbsoluteElement` takes it as an
  optional argument; `ClosePositionedElements` (`ContainerBox.cpp:46-92`) builds the box with
  `LayoutDetails::BuildBox(box, rect.size, element)` and offsets by `rect.origin`.
- `ElementMeta` stores the override; `Element::UpdateOffset` and `GetContainingBlock` use it when set.
- Consumers: p3-layout (fixed → viewport, §3.11) and p3-grid (grid-area containing blocks, §3.14).

#### 3.1.10 Animation layers

`ElementStyle` gets two dictionaries, `transition_properties` and `animated_properties`, with
setters/removers used by `Element`'s animation code (§3.13.5). `GetLocalProperty`, the property
iterators and `ComputeValues` consult them in CSS cascade order: transitions > important
declarations > animations > normal declarations (inline included). Until p3-cascade adds
importance, groundwork implements "transitions > animations > declarations".

#### 3.1.11 Order of application

PR #983 (§3.2) is applied by `p3-base` as its first commit series, because it rewrites the
hottest files (`ComputedValues.h`, `StyleTypes.h`, `ElementStyle.cpp`, `ElementAnimation.cpp`,
`PropertyParserAnimation.cpp`, `PropertyParserTransform.cpp`); everything else in groundwork is
applied on top of it.

---

### 3.2 `calc()`, `min()`, `max()`, `clamp()` (PR #983; `p3-base` applies, `p3-values` extends)

**Evaluation of PR #983** ("Add CSS math expressions (`calc()`, `min()`, `max()`, `clamp()`)",
author Thomas Edvalson (`Cruel`), open, created 2026-08-08; maintainer comment 2026-08-10: needs a
thorough review, agrees the CMake flag must go before merge; no code-review comments yet):

- Base `9cbb5440` (6.2-84), 7 upstream commits before tag 6.3. `git merge-tree 6.3 pr983` is
  clean; cherry-picking its three commits onto 6.3 is clean. Built on 6.3 with
  `-DRMLUI_MATH_EXPRESSIONS=ON`: **182/182 test cases, 10611 assertions pass**, incl. 36
  `Calculation.*` cases (2305 assertions, WPT-derived).
- Design: `Unit::CALCULATION = 1<<30`; `Variant::CALCULATIONPTR` holding
  `SharedPtr<const Calculation>`; immutable typed expression tree (Value, Sum, Product, Negate,
  Invert, Min, Max, Clamp) with dimension types (length, angle, time, resolution, percent);
  parser `CalculationParser.cpp` (nesting depth 32, units px/dp/vw/vh/em/rem/in/cm/mm/pt/pc/deg/rad/
  s/ms/x, typed arithmetic, WPT-style rejection) hooked into `PropertyParserNumber::ParseValue`
  only after `strtof` fails (zero cost for plain numbers); compute-time folding
  (`CalculationResolver.cpp`) converts em/rem/vw/vh/dp/inch units to px and classifies the result
  as constant, linear `px + pct` (`LinearLengthPercentage`) or a residual tree over {px, %};
  `LengthPercentage`/`LengthPercentageAuto` gain `Type::Calculation` (bitfields widened to 2 bits)
  with residuals in a sparse per-element `ComputedCalculationStore` (25 slots: sizes, margins,
  paddings, insets, min/max, origins, flex-basis, gaps); used-value resolution in the single funnel
  `ResolveValueOr` (`ComputedValues.cpp:59-76`) — so `width: min(100%, 600px)` works at layout
  time; transforms keep a side table of calc arguments resolved against the border box;
  interpolation resolves both endpoints to px when the property has a relative target, else builds
  a mix tree; fixes the 6.3 interpolation bug at `ElementAnimation.cpp:185` (uses `v0` twice);
  invalidation through a dependency mask in `DirtyPropertiesWithUnits`, including units hidden
  behind `var()` (6.3 bug: `ElementStyle.cpp:809-820` tests the *specified* unit).
- Behavior changes: `BlockContainer::GetShrinkToFitWidth` ignores percentage min/max-width while
  measuring (a fix: 6.3 resolved them against 0); shorthands reject trailing tokens.

**Decision: reuse, do not rewrite.** The architecture is what an independent design produces
(linear form for the common case, residual tree only where `min()/max()/clamp()` contain `%`),
it merges cleanly, and it brings a WPT-grade test corpus. Application:

1. `git fetch https://github.com/mikke89/RmlUi.git pull/983/head:pr983`; cherry-pick `109df5ad`
   and the code parts of `bea7264e` (drop its `.github/workflows/build.yml` hunk) and `c6744d15`
   onto the fork; commit as `rmlui: calc()/min()/max()/clamp() (upstream PR #983, …)`, keeping
   the original author as commit author. Record the PR URL and source commits in FORK.md.
2. Keep the `#ifdef RMLUI_MATH_EXPRESSIONS` guards (eases rebasing onto upstream when the PR
   lands); `engine/libs/rmlui/CMakeLists.txt` forces `RMLUI_MATH_EXPRESSIONS ON` (PUBLIC compile
   definition, so every TU agrees on the `ComputedValues` layout). The standalone test build (§5.1)
   uses `-DRMLUI_MATH_EXPRESSIONS=ON`.
3. Add our own tests for the shrink-to-fit change (unit test: `max-width: 50%` inside a
   shrink-to-fit parent).

**Extensions (p3-values and the owning packages):**

- New units in calc (`vmin`, `vmax`, `ch`, `ex`, `turn`, `grad`; `svw/svh/lvw/lvh/dvw/dvh` alias
  `vw/vh`): `CalculationParser` unit table + `CalculationResolver` (ch = advance of "0" from font
  metrics if available, else `0.5em`; ex = `FontMetrics::x_height`, `FontMetrics.h:14`).
- Every **new** length/percentage property uses the PR's machinery:
  - scalar longhands (radii x/y, `outline-offset`, `letter-spacing`, `word-spacing`,
    `text-underline-offset`, `text-decoration-thickness`, `translate` components, `inset` logical
    shorthands) register the standard `length`/`length_percent` parsers and get a
    `ComputedCalculationStore` slot when they accept `%`;
  - list/value types stored as `StyleValue` (background positions/sizes, shadows, grid track
    sizes) store `NumericValue` or `CalculationPtr` per component and resolve with a groundwork
    helper `float ResolveLengthPercentage(const LengthPercentageValue&, const Element&, float
    percent_basis)` that wraps `ResolveUsedCalculation`.
- Tests: extend `Calculation.cpp` with the new units and with one case per new consumer property.

---

### 3.3 Values (package `p3-values`)

#### 3.3.1 Units
- `PropertyParserNumber` (`PropertyParserNumber.cpp:7-24` unit table, `:43-97`): add `vmin`,
  `vmax`, `ch`, `ex`, `turn`, `grad`, aliases `svw svh lvw lvh dvw dvh` → `VW/VH`, `Q` is not added.
- `ComputeLength` (`ComputeProperty.cpp:52-70`): `VMIN/VMAX` from `vp_dimensions`; `CH/EX` need the
  font face → `ComputeLength` gets the element's `FontMetrics*` (nullable) through a new
  `LengthContext` struct `{font_size, root_font_size, dp_ratio, vp_dimensions, metrics}` that
  replaces the five scalar parameters at all call sites (PR #983 already threads the same values
  into `CalculationResolver`). `CH`/`EX` properties are re-dirtied when the font face changes
  (`ElementStyle.cpp:996-1001`) like `EM`.
- `ComputeAngle` (`:72-85`): `TURN`, `GRAD`.
- Viewport units are dirtied on context resize (`ElementDocument.cpp:555`): extend the mask with
  `VMIN|VMAX`.

#### 3.3.2 Colors (`PropertyParserColour.cpp`)
6.3: hex 3/4/6/8 OK (`:211-249`); `rgb/rgba` comma-only, alpha parsed with `atoi` so `0.5` → 0
(`:274-287`); `hsl/hsla` comma-only (`:290-330`); 18 CSS2 names + `transparent` (`:116-136`);
no `currentcolor`.
- Rewrite `rgb()/rgba()/hsl()/hsla()` per CSS Color 4: both comma and space syntax, `/ alpha`,
  alpha as number 0–1 or `%`, components as numbers or `%` (rgb) and `deg/rad/turn/grad` or number
  (hue), `none` keyword = 0, out-of-range clamping, `rgb()` accepting 4 comma args.
- `hwb()`; `lab/lch/oklab/oklch` exist — verify alpha and `none` handling.
- `color-mix(in <space>, c1 [p1], c2 [p2])` for spaces `srgb`, `srgb-linear`, `oklab`, `oklch`,
  `lab`, `lch`, `hsl`, `hwb`, `xyz`; constant-folded at parse time when both operands are literal.
- Full CSS named-color table (148 names + `rebeccapurple`), case-insensitive.
- `currentcolor`: parsed to `Property{Unit::KEYWORD, CurrentColor}` accepted by every `color`
  parser use. Resolution: `color` is computed first (like `font-size`, `ElementStyle.cpp:917`);
  `ComputeValue` for every color property maps `currentcolor` to the element's computed `color`;
  inside `color` itself it means the parent's color. Properties holding colors inside values
  (`box-shadow`, `text-shadow`, `filter: drop-shadow()`, gradient color stops, `outline-color`,
  `text-decoration-color`, `caret-color`, `border-*-color`) resolve at compute or decorator
  instancing time (`DecoratorGradient` instancing has the element). A change of `color` dirties
  every property whose specified value uses `currentcolor` (tracked per element as a small id set
  `uses_currentcolor`).
- System colors (`Canvas`, `ButtonFace`, …): diagnosed by compat.

#### 3.3.3 `transform-origin`, `perspective-origin`, `<position>` grammar
6.3 registers `transform-origin` as `FallThrough` over x/y/z (`StyleSheetSpecification.cpp:394-397`)
so `top left` fails. A shared `<position>` parser (groundwork provides
`ParsePosition(const StringList& tokens, NumericValue& x, NumericValue& y)` implementing the 1–4
value CSS grammar with keyword order freedom and edge offsets `right 10px bottom 20%`) is used by
`transform-origin` (+ optional z length), `perspective-origin`, `background-position` (p3-paint),
`object-position` (p3-paint). p3-values converts the two origin shorthands to `Custom`.

#### 3.3.4 Fonts
- `font-family`: new parser `font_family` registered for `font-family`
  (`StyleSheetSpecification.cpp:353`): comma-split (quote-aware), strip quotes, collapse
  whitespace, lowercase, join with `","` → canonical string `inter,segoe ui,sans-serif`.
  `ComputeFontFamily` (`ComputeProperty.cpp:120-123`) keeps it. Contract with the engine font
  engine (phase 0/1, `engine/core/htmlui/RmlFontEngine.cpp`): `GetFontFaceHandle(family, …)`
  (`FontEngineInterface.h:68`) receives the canonical list and resolves families left to right,
  generic families (`serif`, `sans-serif`, `monospace`, `system-ui`, `ui-*`, `cursive`, `fantasy`,
  `emoji`, `math`) mapped to built-in fonts; if phase 0/1 already split lists produced by the
  compat rewrite, only the separator contract is checked (unit test in
  `engine/tests/htmlui/FontFamilyListTest.cpp`).
- `font-size` keywords `xx-small … xxx-large` (CSS table relative to 16px), `smaller`/`larger`
  (×1/1.2 of parent), `math` → `medium`.
- `font-weight`: `bolder`/`lighter` (CSS relative-weight table, computed against parent), numbers
  1–1000.
- `font` shorthand (`StyleSheetSpecification.cpp:359`, FallThrough) → `Custom`:
  `[style || variant(normal only) || weight || stretch(normal only)]? size[/line-height]? family`
  and system font keywords (`caption`, `menu`, … → diagnosed). Omitted longhands reset.
- Font size passed to the font engine is rounded, not truncated (`ElementStyle.cpp:1000`
  `(int)values.font_size()` → `Math::RoundToInteger`); `FontEngineInterface` keeps `int size`.

#### 3.3.5 Numbers
- `opacity`, `flex-grow`, `flex-shrink` accept `%` where CSS does (opacity), clamp negative
  values per spec.
- `z-index` accepts integers only (non-integers rejected as in CSS).
- `aspect-ratio` parser (`<number> [/ <number>]? || auto`) is registered here (computed in §3.12).

#### 3.3.6 `line-height: normal`
- Parser keyword `normal`; `Style::LineHeight::InheritType` gains `Normal` (`ComputedValues.h:117`
  area, `StyleTypes.h`). `normal` inherits as the keyword. The used value is the font's
  `FontMetrics::line_spacing` (`FontMetrics.h:12`) from the element's font face; because
  line-height is computed before the font face handle is fetched (`ElementStyle.cpp:944-970` vs
  `:996-1001`), `normal` is resolved after the handle is known and `VerticalAlign` is re-dirtied.
- Engine contract: the STBText engine reports `line_spacing` as `(hhea.ascender − hhea.descender +
  hhea.lineGap) × scale` (or OS/2 typo metrics when `USE_TYPO_METRICS` is set), which is what
  Chrome uses for `normal`. Verified by a golden fixture with three fonts.

#### 3.3.7 Logical properties
Shorthands mapping to physical longhands (horizontal-tb, ltr): `margin-inline(-start|-end)`,
`margin-block(-start|-end)`, `padding-inline/block(*)`, `inset-inline/block(*)`,
`border-inline/block(-start|-end)(-width|-style|-color)`, `inline-size`, `block-size`,
`min-/max-inline-size`, `min-/max-block-size`. A `direction: rtl` document triggers a compat
diagnostic for logical properties (layout mirroring is out of scope, §2.2).

#### 3.3.8 Cursor
`cursor` (`StyleSheetSpecification.cpp:371`, plain string) gets a parser accepting
`[url(...) [x y]?,]* <keyword>`; the computed value is the keyword (url entries dropped with a
compat diagnostic). Keyword set: CSS UI 4 list. The engine maps them in phase 1's
`SystemInterface::SetMouseCursor`.

**Tests (p3-values):** `Tests/Source/UnitTests/Values.cpp` (units, keyword fonts, font shorthand
round-trips, line-height normal), `Colors.cpp` (one CHECK per syntax form, alpha cases `0.5`,
`50%`, `.5`, `none`, named table spot checks, `currentcolor` in `color` and in `border-color`,
`color-mix` vectors from WPT `color-mix-*.html`), `Position.cpp` (`<position>` grammar matrix).
Goldens: `p3/values/{units,colors,currentcolor,line-height-normal,font-family-fallback,
transform-origin-keywords,calc-layout,calc-min-max-clamp}.html`.

---

### 3.4 Cascade (package `p3-cascade`)

**Current behavior.** A property's specificity is selector specificity
(`StyleSheetNode.cpp:369-390`; constants `StyleSheetSelector.h:13-21`: tag 10 000, class/attribute/
pseudo-class 100 000, id 1 000 000) + rule index (`StyleSheetParser.cpp:742`,
`StyleSheetNode.cpp:142-145`) + per-sheet offset (`StyleSheet.cpp:43-44`).
`PropertyDictionary::SetProperty` keeps the higher specificity (`PropertyDictionary.cpp:134-142`),
`Import` overwrites specificity when the rule's is > 0 (`:93-103`). Inline properties always win:
`ElementStyle::GetLocalProperty` checks `inline_properties` first (`ElementStyle.cpp:48-60`), as does
`PlainPropertiesIterator` (`PropertiesIterator.h:18-80`). The word "important" appears in no
parser: `color: red !important` fails the color parser and the declaration is dropped
(`StyleSheetParser.cpp:1024`). There is no `inherit`/`initial`/`unset`.
Bug: changing the `style` attribute only calls `SetProperty` for the new declarations
(`Element.cpp:1756-1772`), properties removed from the attribute stay.

#### 3.4.1 `!important`
- `PropertySpecification::ParsePropertyDeclaration(dict, name, value)`
  (`PropertySpecification.cpp:219`) — the single entry for stylesheets
  (`StyleSheetParser.cpp:142`), the `style` attribute and `Element::SetProperty(String, String)`
  (`Element.cpp:593-616`) — strips a trailing `!\s*important` (ASCII case-insensitive, also after
  `var()` values), parses into a temporary dictionary, and marks every resulting property
  (longhands of shorthands, custom properties, var shorthands) `important`.
- `Property` (`Property.h:50-57`) gets `bool important` (packed next to `specificity`).
- Effective specificity: `PropertyDictionary::Import/Merge` add `ImportantOffset = 1 << 29` when
  `important` (stylesheet specificities stay far below: ids are 1e6 per id, rule counts small).
- Inline vs stylesheet: inline properties get `InlineSpecificity = 1 << 28` (+ `ImportantOffset`
  when important); `ElementStyle::GetLocalProperty`, `GetLocalCustomProperty`, `GetLocalShorthand`
  (`ElementStyle.cpp:48-85`) and `PlainPropertiesIterator::ProceedToNextValid` compare the inline
  and definition values by effective specificity instead of "inline first". Result: author
  `!important` beats non-important inline; inline `!important` beats everything; animations
  (§3.13.5) sit between normal and important declarations as in CSS (they are a separate layer
  consulted after normal declarations, overridden by important ones).
- `!important` inside `@keyframes` is ignored (CSS), diagnosed by compat.
- Serialization: `Property::ToString` appends ` !important`.

#### 3.4.2 CSS-wide keywords
- Recognized in `ParsePropertyDeclaration` (by id `:253` and in `ParseShorthandDeclaration :284`
  for every longhand) **before** the definition's parsers: `inherit`, `initial`, `unset`, `revert`,
  `revert-layer` (= `revert`, no layers). Stored as `Property{int keyword,
  Unit::CSS_WIDE_KEYWORD}` (groundwork unit bit). Valid for every property, custom properties
  included (`--x: initial` = guaranteed-invalid initial value).
- `all` shorthand (`ShorthandType::Custom`): only CSS-wide keywords; sets every registered longhand
  except `direction` and `-rmlui-*` properties.
- Resolution:
  - Specified level (`ElementStyle::GetSpecifiedProperty`, `ElementStyle.cpp:87-113`, and
    `GetProperty :653-661`): `initial` → `PropertyDefinition::GetDefaultValue()`; `inherit` → the
    parent's specified/computed property; `unset` → `inherit` for inherited properties, else
    `initial`; `revert` → the value from the user-agent origin (below), else `unset`.
  - Computed level (`ComputeValues :873-1027`): `inherit` copies the parent's **computed** field
    (new `ComputedValues::CopyFrom(PropertyId, const ComputedValues&)` generated per id), so `em`
    and `%` are not re-resolved; `initial` computes the default property. The font-size and
    line-height pre-passes (`:917-970`) handle the keywords too.
  - Inherited-dirty propagation: an element whose property is `inherit` on a non-inherited property
    must be dirtied when the parent's value changes: `ComputeValues` propagates dirty ids for
    which any child has an explicit `inherit` (child style keeps a small `explicit_inherit`
    `PropertyIdSet`).
- `ElementAnimation` treats CSS-wide keywords as the resolved computed value at animation start.

#### 3.4.3 `revert` and the user-agent origin
The compat layer (phase 2) loads the user-agent stylesheet first. Fork addition:
`StyleSheetContainer::SetOrigin(StyleOrigin::UserAgent)` (or a flag on `StyleSheet`); when sheets
are merged (`StyleSheet.cpp:43-44`), UA properties are tagged `origin = UserAgent` in `Property`
(2 bits, next to `important`). `ElementDefinition` keeps, in addition to the merged dictionary, a
`PropertyDictionary ua_properties` filled only from UA-origin nodes (small: the UA sheet matches
few rules per element). `revert` returns the UA value for that id, else behaves as `unset`. The
engine's compat code calls the new API when it loads the UA sheet (additive one-line change in
phase-2 code).

#### 3.4.4 Inline style replacement
`Element::OnAttributeChange` for `style` (`Element.cpp:1756-1772`) parses the whole attribute into
a new dictionary, removes inline properties not present anymore (`ElementStyle::RemoveProperty`),
sets the new ones. Data-binding `data-style-*` and `Element::SetProperty` keep per-property
semantics.

**Tests:** `Tests/Source/UnitTests/Important.cpp` (stylesheet vs stylesheet, inline vs important,
inline important, shorthand importance, var() + important, keyframes ignore),
`CascadeKeywords.cpp` (matrix: every keyword × inherited/non-inherited × parent with
`em`/`%` values; `all: unset` on a button; `revert` to a UA rule; `--x: initial`),
`InlineStyle.cpp` (attribute shrink removes properties). Goldens:
`p3/cascade/{important,keywords,all-unset-button,revert-ua}.html`.

---

### 3.5 Selectors (package `p3-cascade`)

**Current behavior** (selector parser `StyleSheetParser::ImportProperties`,
`StyleSheetParser.cpp:1073-1195`): pseudo tokens go to `StyleSheetFactory::GetSelector`
(`:1146-1154`); unknown pseudo-classes are kept as literal names and never match (no error);
`::x` is folded into a pseudo-class named `":x"` (`:1121-1123`); a leading combinator (`> a`) is not
parsed; attribute operators `= ~= |= ^= $= *=` are supported (`:94-115`, matching in
`StyleSheetNode.cpp:192-265`) but the `i`/`s` flags are not. Structural selectors registered at
`StyleSheetFactory.cpp:14-28` (nth-*, first/last/only-*, empty, not, scope). `:not` builds a
`SelectorTree` (`StyleSheetFactory.cpp:114-124`). `:focus` is set on the **whole ancestor chain**:
`Context::OnFocusChange` (`Context.cpp:1190-1237`) dispatches Focus to every element in
`new_chain \ old_chain` (`:1221-1235`), and the Target-phase default action sets `:focus` and
`:focus-visible` (`Element.cpp:2034-2042`). `:disabled` is set from the `disabled` attribute by
`ElementFormControl::OnAttributeChange` (`Elements/ElementFormControl.cpp:43-60`); `:checked` by
checkbox/radio/select (`InputTypeCheckbox.cpp:27`, `InputTypeRadio.cpp:33`,
`WidgetDropDown.cpp:302-307`). No `:enabled`, `:focus-within`, `:root`, `:is`, `:where`, `:has`.
Invalidation: attribute/class/pseudo-class changes dirty self and siblings
(`Element.cpp:2509-2520`), a dirty element re-matches its whole subtree (`:2522-2542`); nothing
dirties ancestors.

Unknown or invalid selectors must not silently create never-matching rules: the parser logs
(`Log::LT_WARNING` with source line) and drops the selector; compat diagnostics turn these into
`file:line:column` reports.

#### 3.5.1 `:root`
Structural selector type `Root`: `element == element->GetOwnerDocument()` (a document is its own
owner, `Element.cpp:2137-2143`). With the HTML root model (§3.1.1) this is `<html>`. Specificity:
pseudo-class.

#### 3.5.2 `:is()`, `:where()`
`StructuralSelectorType::Is`, `Where` (`StyleSheetSelector.h:69`), registered in the factory map
and `requires_parameter` (`StyleSheetFactory.cpp:92`). Construction generalizes the `Not` branch
(`:114-124`); matching returns true if any leaf `IsApplicable(element, scope)`. Specificity: `:is`
= max of the arguments' specificities (computed by `StyleSheetNode::CalculateAndSetSpecificity`
for the argument trees), `:where` = 0 (so `StructuralSelector::specificity` becomes per instance;
`StyleSheetSelector.h:98`). Forgiving selector lists: invalid arguments are dropped, not the whole
selector (Selectors 4). `:matches()`/`:-webkit-any()` are accepted as aliases of `:is`.
`:not()` already accepts complex selector lists (it builds a selector tree) and already uses the
max of its arguments as specificity (`StyleSheetFactory.cpp:114-124`), as Selectors 4 requires;
it only gains the forgiving-list behavior.

#### 3.5.3 `:has()`
- Parsing: each comma-separated relative selector gets `:scope` prepended (`> x` → `:scope > x`,
  `x` → `:scope x`, `+ x` → `:scope + x`, `~ x` → `:scope ~ x`), then `ConstructNodes`. Nested
  `:has()` is rejected (CSS).
- Matching: for `>`/descendant forms walk the subject's descendants (DOM children only, pseudo-
  elements excluded), for `+`/`~` the following siblings, calling `leaf->IsApplicable(candidate,
  /*scope=*/subject)` (`:scope` already compares `element == scope`, `StyleSheetSelector.cpp:352`).
- Invalidation: `StyleSheet::BuildIndex` records `uses_has` and the set of tag/class/id/attribute
  names appearing inside `:has()` arguments. When a sheet uses `:has`, `Element::DirtyDefinition`,
  `AppendChild`, `InsertBefore`, `RemoveChild` (`Element.cpp:1344-1498`), class/attribute and
  pseudo-class changes of an element whose names intersect that set walk the ancestors (and
  preceding siblings for `+`/`~`) setting a new `dirty_definition_self` flag: re-match only that
  element, and its subtree only if its definition changed **and** `:has` appears in a non-subject
  compound. This keeps hover changes from re-matching documents.
- Performance test: a 2 000-element document with `li:has(> input:checked)` toggling one checkbox
  re-matches fewer than 50 elements (counter in `RMLUI_TESTS_ENABLED` builds).

#### 3.5.4 `:focus`, `:focus-within`, `:focus-visible`
- `Context::OnFocusChange` (`Context.cpp:1190-1237`) sets pseudo-classes itself after dispatching
  events: `focus` only on the new focus element (removed from the old), `focus-within` on
  `new_chain` (removed from `old_chain \ new_chain`), `focus-visible` only on the new focus element
  when the focus was keyboard/gamepad-initiated (the existing `focus_visible` parameter).
- Remove the Target-phase handling at `Element.cpp:2034-2042`. The Focus/Blur events are still
  dispatched to the whole chain (RmlUi API behavior kept for listeners).
- Widgets that set `:focus` on sub-parts manually (`WidgetDropDown.cpp:520-531`,
  `WidgetSlider.cpp:471-477`, `WidgetTextInput.cpp:665`) keep working; review each to set
  `focus-within` where they emulate ancestor focus.
- Compat: the phase-2 UA stylesheet that emulated `:focus-within` (if any) is switched off.

#### 3.5.5 Form state parity
- `:enabled`: structural type, matches `ElementFormControl` (and `<button>`, `<option>`,
  `<optgroup>`, `<fieldset>` elements the compat layer maps) without the `disabled` attribute.
- `:disabled` also inherited from a disabled `<fieldset>` ancestor (CSS/HTML).
- `ElementFormControl::OnAttributeChange` (`Elements/ElementFormControl.cpp:43-60`) stops writing
  inline `focus:none` (clobbers user inline styles); disabled controls are made unfocusable in
  `ElementDocument` focus traversal instead.
- `:checked` on `<option selected>` inside `<select>` (`WidgetDropDown.cpp:606-625` sets it on the
  dropdown's option element — verify both select and listbox modes).
- `:read-only`/`:read-write`, `:required`/`:optional`, `:placeholder-shown` (exists,
  `WidgetTextInput.cpp:1026`), `:indeterminate` (checkbox attribute), `:default` → structural
  types driven by attributes.
- `:link`, `:any-link` = `a[href]`; `:visited` never matches; `:target` matches the element whose
  id equals the document URL fragment (set by `ElementDocument` on navigation; static otherwise).

#### 3.5.6 Attribute selectors and `:nth-*`
- `[a="v" i]`, `[a="v" s]` flags (`ParseAttributeSelector`, `StyleSheetParser.cpp:94-115`; match
  in `StyleSheetNode.cpp:192-265` with ASCII case-folding).
- `:nth-child(An+B of S)`, `:nth-last-child(An+B of S)`; An+B parsing replaced by a proper CSS
  An+B tokenizer (`StyleSheetFactory.cpp:127-176` uses `atoi`, accepts garbage).
- `:empty` follows Chrome (the reference renderer): any element or text child, whitespace-only
  text included, makes the element non-empty; comments never count. 6.3 counts DOM children
  (`StyleSheetSelector.cpp:327`), which matches as long as the RML parser keeps whitespace-only
  text nodes where the HTML parser does; the golden `p3/selectors/empty.html` (whitespace,
  comment, nested empty span cases) decides and the parser is adjusted if they differ.

**Tests:** extend `Tests/Source/UnitTests/Selectors.cpp` (matrix per selector, specificity
assertions via `Element::GetProperty(...)->specificity`), new `SelectorsHas.cpp` (forms,
invalidation counters), `FocusPseudoClasses.cpp` (mouse vs keyboard focus, chain, widgets).
Goldens: `p3/selectors/{root,is-where-specificity,has-checked,focus-within,form-states,
attr-flags}.html` (states driven by fixture steps, §5.2).

---

### 3.6 Pseudo-elements, `content`, counters, list markers (package `p3-cascade`)

**Storage decision.** Pseudo-elements are real `Element` objects owned by the originating element
as **non-DOM children** (`AppendChild(…, dom_element=false)`, `Element.cpp:1344-1371`), pointers in
`ElementMeta` (`pseudo_before`, `pseudo_after`, `pseudo_marker`). Consequences:
- DOM APIs (`GetChild`, `GetNumChildren()`, siblings, `QuerySelector*`, `GetElementById`,
  structural selectors, `GetInnerRML`, data-binding `data-for`, the Lua element API) already skip
  non-DOM children — no change needed and no risk of `:nth-child` counting them.
- Update, style propagation (`GetNumChildren(true)`, `ElementStyle.cpp:1012-1018`), rendering and
  destruction already include non-DOM children.
- Layout order and paint order come from `LayoutDetails::GetLayoutChildren` and the stacking-context
  iteration (groundwork §3.1.6), which place `::marker`, `::before` first and `::after` last.
- `SetInnerRML` / `RemoveChild` do not touch them; `Element::~Element` destroys them.

(Rejected alternative: inserting them as DOM children and filtering every DOM API — touches ~15
call sites, Lua proxies and data binding.)

#### 3.6.1 Selector support
- `CompoundSelector` (`StyleSheetSelector.h:112-120`) gets `PseudoElementType pseudo_element`
  (`None, Before, After, Marker`); the parser (`StyleSheetParser.cpp:1121-1154`) recognizes
  `::before`, `::after`, `::marker` and legacy single-colon `:before`/`:after`. A pseudo-element must
  be the last simple selector; it may be followed only by user-action pseudo-classes
  (`::before:hover` is invalid CSS; `:hover::before` is the normal form). Unknown pseudo-elements
  drop the selector with a warning.
- Specificity: a pseudo-element counts as a type selector (tag constant).
- Index: `StyleSheetNode::BuildIndex` (`StyleSheetNode.cpp:99-135`) puts nodes ending in a
  pseudo-element into a separate `StyleSheetIndex::pseudo_element_nodes[type]` (and keeps them out
  of the normal indexes, so `*`, `:hover`, `[x]` rules in `other` never apply to pseudo-elements).
- Matching: `StyleSheet::GetElementDefinition(const Element* originating, PseudoElementType)` runs
  the normal match against the **originating** element for the compound chain and requires the
  node's pseudo-element type; the element-definition cache (`StyleSheet.cpp:229`) keys include the
  type. Ordinary nodes reject pseudo-elements (`Element::GetPseudoElementType() != None`).

#### 3.6.2 Generation
- After `Element::UpdateDefinition` (`Element.cpp:2522`) of an element E, for each type T, ask
  the sheet whether any T node matches E. If yes, create/update the pseudo-element P (tag
  `"::before"` etc., `Element::GetPseudoElementType()` = T, owner document = E's), set P's
  definition from `GetElementDefinition(E, T)`; P's `ElementStyle` uses E's computed values as
  parent (inheritance from the originating element).
- `content` (new property, not inherited, initial `normal`) is resolved on P's computed values:
  `normal`/`none` → P is destroyed (for `::marker`, `normal` means "generated from
  `list-style-type`"); otherwise the content list is materialized as P's children: string and
  `attr(name)` segments concatenated into one `ElementText`; `url(...)` segments become an `img`
  child element (ElementImage, `object-fit: fill` semantics as a replaced element);
  `counter(name[, style])`/`counters(name, sep[, style])` segments text; `open-quote`/`close-quote`
  use `quotes` (registered, initial `auto` → `“ ” ‘ ’`), `no-open-quote`/`no-close-quote` affect
  depth only. Escapes (`"\201C"`, `"\A"` with `white-space: pre*`) are decoded by the parser.
- Content value type: `ContentValue : StyleValue` (list of segments). `attr()` dependency: E's
  `OnAttributeChange` re-evaluates P's text when the attribute is referenced.
- `display` on P: default `inline`; `display:none` on P removes it from layout as for elements.
- Events and hover: `Context::GetElementAtPoint` (`Context.cpp:1382-1460`) maps a hit on P (or a
  descendant of P) to the nearest non-pseudo ancestor, so events target the originating element and
  `:hover` lands on E (and thus `E:hover::after` rules apply). Do not use `pointer-events` for this
  (it is inherited, `StyleSheetSpecification.cpp:386`). `pointer-events: none` on P makes it
  transparent to hit testing as usual.
- Transitions/animations work on P because it is a normal element with `ElementStyle`; P's
  definition changes when E's pseudo-classes change, so `:hover::after` transitions run (covered by
  §3.13.4's "transition on any computed change").
- Debugger plugin shows pseudo-elements under their originating element with their tag.

#### 3.6.3 Counters
- Properties: `counter-reset`, `counter-increment`, `counter-set` (`CounterListValue :
  StyleValue`, `none` | (`<name> <integer>?`)+), not inherited.
- `CounterResolver` in `ElementDocument`: when a counter property, `display`, the DOM order or a
  pseudo-element's existence changes, the document is marked `counters_dirty`; before layout
  (`ElementDocument::UpdateLayout`) a pre-order walk over the layout tree (elements +
  pseudo-elements, `display:none` excluded) applies reset/set/increment with CSS scoping
  (counters are inherited by descendants and following siblings' subtrees) and updates the text of
  every pseudo-element whose content uses `counter()`/`counters()`. Implicit `list-item` counter:
  incremented by every `display: list-item` element, reset by `ol`/`ul`/`menu` via the UA
  stylesheet (`ol { counter-reset: list-item }`), `<ol start>`/`reversed` and `<li value>` handled
  by UA attribute rules + the resolver.
- Styles: `decimal`, `decimal-leading-zero`, `lower-alpha/latin`, `upper-alpha/latin`,
  `lower-roman`, `upper-roman`, `lower-greek`, `disc`, `circle`, `square`, `none`, `<string>`
  (others diagnosed, §2.2).

#### 3.6.4 `display: list-item` and `::marker`
- Properties: `list-style-type` (inherited, initial `disc`), `list-style-position` (inherited,
  `outside` | `inside`), `list-style-image` (registered; `none` only, `url()` diagnosed),
  `list-style` (`AnyOrder` shorthand).
- A `display: list-item` element gets a `::marker` pseudo-element when `list-style-type` is not
  `none` or a `::marker` rule sets `content`. Default content: counter text + `". "` for ordinal
  styles; the bullet glyph (U+2022 disc, U+25E6 circle, U+25AA square) + space for symbol styles.
  `::marker` accepts only the CSS-allowed properties (color, font-*, content, white-space,
  animation/transition, direction); others are ignored at definition time.
- Layout: `inside` → the marker is the first inline box of the element's first line. `outside` →
  the marker is laid out as an inline-block (shrink-to-fit) positioned so that its end edge touches
  the list item's border-box start edge and its baseline aligns with the first line box's baseline;
  it does not affect the list item's size and is painted with the list item. Implemented in
  `BlockContainer`/`InlineContainer` when the first line box closes (`LineBox` gives the baseline).
- UA stylesheet (compat): `li { display: list-item }`, `ul { list-style-type: disc;
  padding-left: 40px; margin: 1em 0 }`, nested `ul ul { list-style-type: circle }`, `ol { list-style-type:
  decimal }`. The phase-2 rule that forced `list-style: none` (if any) is switched off.

**Tests:** `Tests/Source/UnitTests/PseudoElements.cpp` (generation/removal on content changes,
non-DOM invisibility to `GetChild`/`QuerySelectorAll`/`:nth-child`, `attr()` updates, hit testing
returns originating element, inheritance from originating element, transitions on
`:hover::after`, destruction with the parent, data-for siblings), `Counters.cpp`, `ListMarkers.cpp`.
Goldens: `p3/pseudo/{before-after-basic,content-attr,content-url,hover-transition,
counters-nested,list-markers-outside,list-markers-inside,ordered-start-reversed}.html`.

---

### 3.7 Borders, radii, outline (package `p3-paint`)

**Current behavior.** No border-style property; `border-top` = FallThrough of width + color
(`StyleSheetSpecification.cpp:294-297`), `border` = RecursiveRepeat (`:298`), so
`border: 1px solid red` fails at `PropertySpecification.cpp:438-440`. Radii: four `length`
longhands (`StyleSheetSpecification.cpp:300-303`), Box shorthand (`:304`), stored as `int16_t`
(`ComputedValues.h:182`, truncated at `:370`), no `%`, no `/`. Geometry:
`MeshUtilities::GenerateBackgroundBorder` (`MeshUtilities.cpp:57-116`) → `ComputeBorderMetrics`
(`GeometryBackgroundBorder.cpp:11-73`) scales overlapping radii against the **padding** box
(`:46-51`, CSS uses the border box), rounds radii (`:54`), outer corners are circles; borders are
per-side quads with sharp-corner mitres and colour *interpolated* along rounded corners
(`RoundedLerp :247`); tessellation `clamp(3 + round(R/6), 2, 100)` (`:325-328`); no anti-aliasing.
`Element::GetRenderBox` (`Element.cpp:483-524`) passes radii through unresolved. Clip masks use
`ElementBackgroundBorder::GetClipGeometry` (`ElementBackgroundBorder.cpp:55-76`) →
`MeshUtilities::GenerateBackground` with the same `RenderBox`, so clip and background stay
consistent if radii are resolved in one place. Box-shadow takes raw `computed.border_radius()`
(`BoxShadowCache.cpp:92-93`), spread adjustment per corner at `GeometryBoxShadow.cpp:168-178`.

#### 3.7.1 Properties
- `border-{top,right,bottom,left}-style`: keywords `none hidden dotted dashed solid double groove
  ridge inset outset`, initial `none`, not inherited, forces layout (width becomes 0 when
  `none`/`hidden`). `Style::BorderStyle` in `StyleTypes.h`; 4×4 bits in `CommonValues`.
- `border-style` (Box), `border-top/right/bottom/left` (`AnyOrder` of width, style, color),
  `border` (`AnyOrder`, all four sides, also resets `border-image` — registered as no-op
  `none`-only property with a compat diagnostic for other values), `border-width` keywords
  `thin`=1px, `medium`=3px, `thick`=5px (initial `medium`, CSS), border widths stored as `float`
  (6.3 `uint16_t`, `ComputedValues.h:78`) so `0.5px` hairlines survive (rendered with coverage
  alpha when < 1 device px).
- Computed border width = 0 when style is `none`/`hidden` (CSS) — in `ElementStyle::ComputeValues`
  after the property loop; layout (`LayoutDetails::BuildBox`) needs no change.
  **RCSS compatibility note**: RCSS that sets only `border-width` stops drawing borders; the compat
  layer always emits styles, and the fork's own Debugger/sample RCSS is updated with
  `border-style: solid` where it relies on borders.
- Radii: each corner becomes a shorthand over `-x`/`-y` longhands (e.g.
  `border-top-left-radius-x`, internal names prefixed `-rmlui-` so they do not pollute CSS
  serialization), `length_percent` parsers with calc support; `border-top-left-radius: 10px 20px`
  sets x/y; `border-radius` is `Custom`: `<1-4 values> [ / <1-4 values> ]`.
  Computed: `LengthPercentage` per axis per corner (`RareValues`), calc residual slots.
- `outline-width` (keywords as border), `outline-style` (`auto` + border styles except `hidden`),
  `outline-color` (initial `currentcolor`; `invert` → `currentcolor`), `outline-offset` (length),
  `outline` (`AnyOrder`). Not inherited, never force layout.

#### 3.7.2 Radius resolution and overlap scaling (single place)
`Element::GetRenderBox` resolves radii: x against the border-box width, y against the border-box
height, then applies CSS Backgrounds 3 §5.5 scaling `f = min(L_side / (r_a + r_b))` over the 8
side/axis pairs using border-box lengths, before computing inner/padding/content radii. The scaling
in `ComputeBorderMetrics` (`GeometryBackgroundBorder.cpp:46-51`) is removed; the rounding at `:54` is
removed (sub-pixel radii). `RenderBox` (`Include/RmlUi/Core/RenderBox.h:10-48`) changes its radius
type to `CornerSizes2` (Vector2f per corner; public API change recorded in FORK.md). `DrawArcArc`,
`DrawArcPoint`, `DrawBackgroundCorner` take elliptical outer radii; `GetNumPoints` uses `max(rx, ry)`
and is raised for small radii so 50% circles of 24px stay round. `BoxShadowCache.cpp:92` takes
radii from `GetRenderBox(BoxArea::Border)`; the spread adjustment
(`GeometryBoxShadow.cpp:168-178`) becomes per axis. Hit testing stays rectangular
(`Context.cpp:1450-1455`) — CSS hit-testing honours radii: add an ellipse test against the
border-box radii in `Context::GetElementAtPoint` for elements with non-zero radii.

#### 3.7.3 Border geometry
Keep the current solid code as the fast path when all visible sides are `solid`. Otherwise a new
generator (`Source/Core/GeometryBorderStyles.cpp`) builds per side:
- **Corner split** at the CSS angle `atan2(w_side, w_adjacent)` on the outer/inner ellipses
  (sampled at the same parametric angle as `DrawArcArc`), not by colour interpolation; this also
  fixes the colour blend at rounded corners for solid borders with different side colours (applied
  to the fast path too).
- **Centre path** per side: second half of the start corner arc, straight edge, first half of the
  end corner arc; arc length measured numerically from the tessellated points.
- **Dashed** (measured on Chrome 150 while writing this spec, 300 px straight edges): `w < 3` →
  dash `3w`, gap `2w` (1 px: 3/2, 2 px: 6/4); `w ≥ 3` → dash `2w`, gap `w` (4 px: 8/4, 8 px: 16/8).
  The pattern starts and ends with a full dash at the corners; the gaps absorb the remainder
  (`n = max(1, round((L + gap) / (dash + gap)))` dashes, gap stretched to fit, observed as single
  1 px wider gaps). Each dash is a quad strip between the inner and outer offset curves over
  `[s0, s1]` along the centre path (curved inside corners). Fixture
  `p3/borders/dashed-widths.html` (w = 1, 2, 3, 4, 8; lengths 250, 300, 301) locks this.
- **Dotted** (measured): round dots of diameter `w`, period `2w` (2 px: 2/2, 4 px and 8 px: period
  8 and 16), a dot centred on each corner split point, spacing stretched to fit; `w = 1` renders
  1 px squares with period 2. Dots are triangle fans with `GetNumPoints(w/2)`.
- **Double** (measured, w = 6: 2/2/2): outer and inner solid bands of `round(w/3)` each, middle
  empty, radii offset accordingly; `w < 3` renders solid (CSS).
- **Groove/ridge/inset/outset** (measured, colour rgb(200,100,50), w = 6): the "dark" colour is
  Chrome's `Color::Dark()`: `v = max(r,g,b)/255`, factor `max(0, (v − 0.33)/v)` → (116,58,29);
  the "light" colour is the specified colour itself. groove: top/left outer half dark, inner half
  light; bottom/right outer half light, inner half dark (bands `w/2`). ridge: inverted. inset:
  top/left fully dark, bottom/right fully specified colour. outset: inverted. Black and very dark
  colours: Chrome substitutes a fixed grey — measured by `p3/borders/3d-styles.html`
  (black/white/red cases) and encoded by the implementer.
- **Anti-aliasing**: border and background outer edges get a 1-device-px feather ring (outer vertex
  ring with alpha 0) when the document renders without MSAA. Controlled by a new global
  `Rml::SetGeometryFeathering(bool)` that the engine sets from its render-target MSAA state
  (phase 1 renders documents into an MSAA target when available; then feathering stays off).

#### 3.7.4 Outline
CSS paints outlines in a final pass of the stacking context (Appendix E step 10), so an outline
lies above the element's content and above later in-flow siblings. RmlUi renders per element, so
outlines are collected per stacking context: while `Element::Render` (`Element.cpp:180-214`) walks
a stacking context, elements with a visible outline are appended to a list owned by the stacking
context root, and their outline geometry (built and cached by `ElementBackgroundBorder` as a
separate mesh) is drawn after the root's last stacking child (`Element.cpp:209-210`), with each
element's transform and clip applied. Geometry: the border generator applied to the border box
expanded by `outline-offset + outline-width`, outer radii = border radii + offset (Chrome follows
radii), clamped ≥ 0. `outline-style: auto` draws Chrome's focus ring: a 2 px solid ring in
`outline-color` when it is not `currentcolor`/`auto`, else the two-tone ring (`#101010` 2 px
inside a 1 px `#ffffff` halo) — `p3/borders/outline-auto.html` fixes the exact geometry. Outlines
never affect layout or clipping of other content and are included in the ink overflow used for
layer bounds (§3.8.1).

**Tests:** `Tests/Source/UnitTests/BorderStyle.cpp` (shorthand matrix in any order, resets,
computed width 0 for none/hidden, `thin/medium/thick`), `BorderRadius.cpp` (`%`, `/`, overlap
scaling numbers, calc), extend `ElementBackgroundBorder.cpp` (vertex counts for dashed/dotted,
clip geometry equals background geometry). Goldens: `p3/borders/{shorthand-any-order,
styles-all-10,dashed-widths,dotted-rounded,double-rounded,3d-styles,radius-percent-circle,
radius-elliptical,radius-overlap-scaling,radius-clip-children,box-shadow-radius-percent,
outline-offset,outline-auto,hairline}.html`.

---

### 3.8 Opacity groups, stacking contexts, visibility, canvas background (package `p3-paint`)

**Current behavior.** `opacity` is registered **inherited** (`StyleSheetSpecification.cpp:351`),
assigned without multiplication (`ElementStyle.cpp:1211-1212`) and applied **per draw** by
premultiplying vertex colours (`ElementBackgroundBorder.cpp:122-128`, `BoxShadowCache.cpp:57,93`,
`DecoratorGradient.cpp:140,265,434,631`, `DecoratorShader.cpp:42`, `DecoratorTiled.cpp:80`,
`DecoratorNinePatch.cpp:40`, `DecoratorText.cpp:80-85`, `ElementText.cpp:371-385,497,528`,
`ElementImage.cpp:180`, `ElementProgress.cpp:224`): a child with `opacity:1` inside a `0.5`
parent draws fully opaque, overlapping children show through each other. Opacity does not create a
stacking context (`Element.cpp:1888-1894`: z-index, filter, backdrop-filter, mask, transform,
perspective, forced). Layer API: `RenderInterface::PushLayer/CompositeLayers/PopLayer/
SaveLayerAsTexture/SaveLayerAsMaskImage/CompileFilter` (`RenderInterface.h:98-122`, wrappers
`RenderManager.cpp:274-330`); `ElementEffects::RenderEffects` (`ElementEffects.cpp:185-316`) pushes
a layer for filters/masks on Enter (`:251-254`) and composites on Exit (`:283-314`) with the scissor
narrowed to the element's ink box (`:214-235`). An `opacity` filter exists
(`Factory.cpp:221`, `FilterBasic.cpp:17-20`).

#### 3.8.1 Semantics
- `opacity` becomes **non-inherited** (CSS) and moves to `RareValues`. It creates a stacking
  context when < 1 (add to the condition at `Element.cpp:1892-1894` and the trigger list at `:1888`).
- Rendering of an element E with opacity α < 1 is a **group**: E's whole stacking context (its
  background, border, decorators, content, descendants) is rendered into a layer and composited
  with the `opacity` filter at α. Implementation in `ElementEffects`: a `CompiledFilter
  group_opacity` compiled in `ReloadEffectsData` (`opacity` changes already dirty effects data,
  `Element.cpp:1951-1958`), the `PushLayer` condition extended (`:251`), the handle appended after
  filter/mask handles (`:293-307`). When E already has a filter or mask, opacity folds into the same
  layer (one composite).
- Per-draw opacity sites switch from `computed.opacity()` to `Element::GetDrawOpacity()`:
  1 for elements inside a group; for a **leaf fast path** element (below) its own α multiplied into
  its vertices (and its text children's).
- **Leaf fast path** (no layer; vertex alpha): α < 1 and E has no element children other than text
  nodes, no pseudo-elements, and paints at most one of {background/border/box-shadow/decorator,
  text}. This is exact (no overlap within E) and covers the common "fade a label/icon" case.
- α == 0: skip rendering E's stacking context entirely; E stays hit-testable (CSS).
- Layer scissor: the union of the subtree's ink overflow (E's border box + shadows/outlines +
  visible overflow of descendants, `LayoutBox::visible_overflow_size`) intersected with the current
  scissor — not E's border box (6.3 filters clip overflowing descendants, `ElementEffects.cpp:222`;
  fix it for filters too: CSS filters apply to the whole ink overflow).
- Opacity animations: only the composite filter changes per frame (filters are recompiled when α
  changes; release the previous handle) — no geometry regeneration, which is cheaper than 6.3.
- Engine requirement: the sokol renderer (phase 1) implements `CompileFilter("opacity")`. If
  phase 1 did not, p3-paint adds it in `engine/core/htmlui/RmlRenderer.cpp` (one filter case: multiply
  the layer by α while compositing, premultiplied).

#### 3.8.2 Stacking-context parity
- Create a stacking context for: `opacity < 1`, `position: fixed`, `position: sticky`,
  `isolation: isolate` (new property), `z-index` ≠ auto **only** when positioned or a flex/grid item
  (CSS: `z-index` on static non-items is ignored; 6.3 applies it to any element), filter,
  backdrop-filter, mask, transform, perspective (already), `will-change: transform|opacity`
  (CSS creates stacking contexts for these).
- Paint order inside a stacking context follows CSS Appendix E as already implemented by
  `RenderOrder` (`Element.cpp:2325`); `::before`/`::after` in tree order (groundwork §3.1.6).

#### 3.8.3 `visibility`
`visibility` becomes inherited with `collapse` (= `hidden` outside tables). A hidden element is
laid out, not painted, not hit-testable, but its descendants with `visibility: visible` paint and
receive events. 6.3 skips the whole subtree (`AddToStackingContext`, `Element.cpp:2395-2399`):
change to "skip own painting" (background/border/decoration/`OnRender`/text) and keep adding
children. Interpolation (`ElementAnimation.cpp:215-228`) keeps its CSS special case.

#### 3.8.4 Canvas background
CSS: the root element's background (or `<body>`'s when the root has none) paints the whole
viewport. In `ElementDocument` rendering, if `<html>` has a transparent background and no
decorator/background-image, use `<body>`'s background layers for the canvas and do not paint them
on `<body>`; the canvas area is the context's dimensions (the document's containing block). A game
that wants the scene visible simply leaves both transparent (the default).

#### 3.8.5 Filter parity
- `filter`/`backdrop-filter` function defaults: `blur()` = 0, `drop-shadow(x y)` colour =
  `currentcolor`, `brightness()` etc. default 1, `%` amounts; `hue-rotate` angle units (turn/grad).
- `drop-shadow` uses the element's ink outline, unchanged.

**Tests:** `Tests/Source/UnitTests/GroupOpacity.cpp` using the trompeloeil mock render interface
(`Tests/Source/Common/Mocks.h`) to assert `PushLayer`/`CompositeLayers` sequences for: group,
leaf fast path, α = 0, filter+opacity folding; `TestsRenderInterface` counters gain
`push_layer`, `composite_layers`, `pop_layer` (additive test-infra change). `StackingContext.cpp`
(z-index on static vs positioned vs flex item; opacity/fixed create contexts), `Visibility.cpp`.
Goldens: `p3/paint/{opacity-group-overlap,opacity-nested,opacity-fade-transition,opacity-zero-hit,
z-index-static-ignored,z-index-flex-item,isolation,visibility-child-visible,canvas-background-body,
filter-overflow}.html`.

---

### 3.9 Backgrounds, masks, `object-fit` (package `p3-paint`)

**Current behavior.** `background` is a FallThrough of `background-color` only
(`StyleSheetSpecification.cpp:343-344`); images and gradients exist only via the RmlUi `decorator`
property (`:403`) and `mask-image` uses decorator syntax (`:404`). `ElementImage` stretches the
texture over the content box (`ElementImage.cpp:180-183`).

#### 3.9.1 Properties
Comma-list longhands (`StyleValue` types), not inherited:
`background-image` (`none | url() | linear-gradient() | radial-gradient() | conic-gradient() |
repeating-*()`; `image()`/`image-set()` → first url; `cross-fade()`/`element()` diagnosed),
`background-position-x`, `background-position-y` (and `background-position` `Custom` shorthand via
`<position>`), `background-size` (`auto | cover | contain | <lp> <lp>?`), `background-repeat`
(`repeat-x | repeat-y | [repeat|space|round|no-repeat]{1,2}`), `background-origin`,
`background-clip` (`border-box | padding-box | content-box`; `text` diagnosed),
`background-attachment` (`scroll`, `local`; `fixed` → treated as viewport-anchored image position
at render time, cheap given the canvas rect), and `background` (`Custom`, full CSS grammar with
`/ size`, last layer may carry the color). Calc/`%` lengths via §3.2's helper.

#### 3.9.2 Rendering
- `ElementBackgroundLayers` (new, `Source/Core/ElementBackgroundLayers.{h,cpp}`) owned by
  `ElementBackgroundBorder`, generated between background colour and border (CSS order:
  colour, layers bottom-to-top, border). For each layer: origin box from `background-origin`, tile
  size from `background-size` (intrinsic size from the texture for `url()`, the origin box for
  gradients), tile position from `background-position`, repetition from `background-repeat`
  (space/round per CSS), clipped to the `background-clip` box.
- `url()` tiles: one textured quad per visible tile (as `DecoratorTiledImage`), clipped to the clip
  box; when the clip box has radii, the layer draws inside a clip mask built from
  `GenerateBackground(GetRenderBox(clip_area))` (same path as overflow clipping,
  `ElementUtilities.cpp:157-169`), so rounded corners match exactly.
- Gradients: reuse `DecoratorGradient`'s instancers (`Decorator*Gradient` / shader-based ones,
  `DecoratorGradient.cpp`) with the tile rect as paint area, repeated per tile; currentcolor stops
  resolved per element (§3.3.2).
- Images load through the existing texture path (`RenderManager::LoadTexture`), so project
  assets and the phase-1 SVG/Lottie support apply (`url(icon.svg)` backgrounds via the SVG plugin
  texture source).
- `mask-image` accepts the same layer syntax (`url()`, gradients) natively; `mask-size`,
  `mask-position`, `mask-repeat`, `mask` shorthand (alpha masks) map onto the same layer generator
  rendering into the mask layer (`ElementEffects.cpp:296-309`).
- The RmlUi `decorator` property keeps working (fork-internal and used by phase-5 effects);
  `decorator` paints above `background-image` layers.

#### 3.9.3 `object-fit`, `object-position`
Registered for all elements, used by replaced elements: `ElementImage::GenerateGeometry`
(`ElementImage.cpp:180-183`) and the phase-1 SVG/Lottie elements (same helper
`ComputeObjectFitRect(content_box, intrinsic_size, fit, position) -> {dest_rect, uv_rect}`):
`fill` (6.3 behavior), `contain`, `cover` (UV-cropped, no overdraw), `none`, `scale-down`;
`object-position` via `<position>`.

**Tests:** `Tests/Source/UnitTests/Backgrounds.cpp` (shorthand round trips incl. multiple layers,
`/` size, defaults reset; tile counts for repeat/space/round), `ObjectFit.cpp` (dest/uv rects for
all five values). Goldens: `p3/backgrounds/{shorthand-gradient,multiple-layers,url-cover,
url-contain,repeat-space-round,position-keywords,clip-padding-radius,origin-content,
attachment-fixed,mask-image-gradient,mask-image-url}.html`, `p3/replaced/{object-fit-5,
object-position}.html`.

---

### 3.10 Text (package `p3-text`)

| Feature | 6.3 state | Patch |
|---|---|---|
| `letter-spacing` | `normal` or length (`StyleSheetSpecification.cpp:358`), lazily resolved (`ComputedValues.cpp:45-50`); default font engine casts to int (`FontFaceHandleDefault.cpp:80,239`) | Float px, `%`/`em`/calc; `TextShapingContext::letter_spacing` (float, `TextShapingContext.h`) already float. |
| `word-spacing` | missing | New property (inherited, `normal | <length-percentage>`), new `TextShapingContext::word_spacing` (float px) passed to `GetStringWidth`/`GenerateString`; the engine font engine adds it to U+0020/U+00A0/U+1361 advances (one-line contract; additive change in `engine/core/htmlui/RmlFontEngine.cpp`). Justified text distributes extra space on top. |
| `text-transform` | ASCII only, `capitalize` no-op (`ElementText.cpp:679-690`) | Unicode simple case mapping (table generated from UnicodeData.txt by `Utilities/gen_case_tables.py` into `Source/Core/UnicodeCaseTables.inl`, BMP + supplementary simple mappings), `capitalize` = first letter of each word (word boundary: after whitespace/punctuation per CSS Text), `full-width`, `full-size-kana` diagnosed. German ß → "SS" (special casing for `uppercase`). |
| `white-space` | `normal pre nowrap pre-wrap pre-line` (`:364`) | Add `break-spaces`; register `white-space-collapse` (`collapse preserve preserve-breaks preserve-spaces break-spaces`) and `text-wrap-mode` (`wrap nowrap`) as the CSS Text 4 longhands with `white-space` as shorthand; `text-wrap` shorthand accepts `wrap nowrap balance pretty stable` (`balance`/`pretty`/`stable` → `wrap` + compat info-level diagnostic). |
| `overflow-wrap` (`word-wrap` alias) | missing; `word-break: break-word` exists (`:365`) | `normal anywhere break-word`; line breaker in `ElementText::GenerateLine` breaks inside words when a word does not fit; `anywhere` also affects min-content (soft wrap opportunities count for `GetMinContentWidth`), `break-word` does not (CSS). |
| `word-break` | `normal break-all break-word` | add `keep-all` (no breaks between CJK letters). |
| `text-overflow` | `clip ellipsis <string>` (`:340`) | Parity: applies to a block container with `overflow` ≠ `visible` and the text overflowing its line box (6.3 rules checked against Chrome on `p3/text/ellipsis-*`), two-value syntax diagnosed. |
| `text-decoration` | single keyword property (`:362`) | `text-decoration-line` (multiple values: `underline overline line-through`), `-color` (initial `currentcolor`), `-style` (`solid double dotted dashed`; `wavy` → solid + diagnostic), `-thickness` (`auto | from-font | <lp>`), `text-underline-offset`, `text-decoration` `AnyOrder` shorthand. Geometry from `FontMetrics::underline_position/thickness` (`FontMetrics.h:16-17`). Decorations propagate to inline descendants (CSS) — RmlUi draws per text element; keep, but descendants inherit the drawing via the existing inherited `text-decoration` value. |
| `text-shadow` | missing (only `font-effect: shadow`) | New inherited property (`TextShadowListValue : StyleValue`: list of `offset-x offset-y blur? color?`, colour default `currentcolor`, interpolable). Rendering in `ElementText`: for each shadow (painted back to front, behind the text) the text geometry is generated again with the shadow colour and translated by the offset; blur > 0 renders those meshes into a layer and composites with the `blur` filter (σ = blur/2, CSS) — scissor = text ink box + 3σ. No dependency on font-engine effects. |
| `text-align` | `left right center justify` (`:361`) | add `start`, `end` (resolve via direction), `match-parent`; `-webkit-center` → center. |
| `vertical-align` | keywords + length (`:331-333`) | parity check fixtures: `middle` uses `x_height/2` from metrics; `text-top/bottom` use font ascent/descent; `%` of line-height. |
| `user-select` | missing | registered (`auto text none contain all`), inherited; `none`/`auto` no-op, `text/all` diagnosed (§2.2); text inputs honour `none` (no selection). |
| `pointer-events` | `none auto` (`:386`) | parity verified (inherited); SVG values diagnosed. |
| `caret-color`, `accent-color` | caret exists | `accent-color` registered; used by checkbox/radio/range default UA styling (phase-2 UA sheet uses it). |
| Font size precision | `(int)` truncation (`ElementStyle.cpp:1000`) | rounding (§3.3.4). |

**Tests:** `Tests/Source/UnitTests/TextProperties.cpp` (parsing of every property above,
computed values), `TextTransform.cpp` (Unicode table vectors incl. ß, Turkish not special),
`LineBreaking.cpp` (overflow-wrap/word-break/break-spaces line splits with a test font engine of
fixed advances — `TestsInterface` font engine). Goldens: `p3/text/{letter-word-spacing,
text-transform-unicode,white-space-all,overflow-wrap,ellipsis-single-line,ellipsis-flex-child,
decoration-longhands,text-shadow-hard,text-shadow-blur-glow,text-shadow-multiple,align-start-end,
vertical-align-middle}.html`.

---

### 3.11 Positioning: fixed, sticky, overflow (package `p3-layout`)

**Current behavior.** `fixed` deliberately follows the absolute rules: `LayoutDetails::GetContainingBlock`
(`LayoutDetails.cpp:155-165`, comment "We deviate from the CSS specs…") walks to the nearest
ancestor flagged `is_absolute_positioning_containing_block` (`ContainerBox.cpp:112-113`).
`Element::UpdateAbsoluteOffsetAndRenderBoxData` (`Element.cpp:367-390`) adds the offset parent's
absolute offset and, for fixed elements, skips only the offset parent's own scroll — a fixed
element moves when an outer ancestor scrolls. Documents are `position: absolute`
(`ElementDocument.cpp:139`), so "fixed" means "fixed to the nearest positioned ancestor or the
document". Clipping walks the offset-parent chain (`ElementUtilities::GetClippingRegion`,
`ElementUtilities.cpp:119-211`), so `overflow: hidden` on the containing block and above clips a
fixed element; hit testing does the same (`Context.cpp:1455`).

#### 3.11.1 `position: fixed`
- Containing block: the **viewport** (context dimensions, origin at the document's border-box
  origin, which is the canvas origin for our documents), unless an ancestor has `transform`,
  `perspective`, `filter`, `backdrop-filter`, `mask-image`, `contain: paint/layout` (not
  registered), or `will-change: transform/filter`; then that ancestor's padding box (CSS).
  New flag `is_fixed_positioning_containing_block` next to `ContainerBox.cpp:112`.
- Layout: `GetContainingBlock` for `Fixed` walks to the root container unless the new flag is set
  on an ancestor; the root case uses the containing-block override (§3.1.9) with the viewport rect
  so `left/right/top/bottom/%` resolve against the viewport, and `UpdateOffset`/`GetContainingBlock`
  (`Element.cpp:2245-2289`, `710-735`) use the override.
- Offset: for a fixed element whose containing block is the viewport, the offset parent is the
  document; `UpdateAbsoluteOffsetAndRenderBoxData` excludes every ancestor scroll offset (document
  included). The element does not move when any ancestor (or the document) scrolls.
- Clipping and hit testing: `GetClippingRegion` for a viewport-fixed element starts at the document
  and ignores intermediate `overflow` clips (CSS: only its containing block chain clips, which for
  the viewport is nothing but the viewport). Same rule in `Context::GetElementAtPoint`.
- Stacking: `position: fixed` creates a stacking context (§3.8.2).
- Scroll containers' scrollable overflow ignores viewport-fixed descendants
  (`ContainerBox::SubmitBox` overflow accumulation, `ContainerBox.cpp:178-239`).
- RmlUi's widgets that relied on the old behavior (`ElementHandle` drag handles,
  `WidgetDropDown` selection box which is absolutely positioned) are checked; RmlUi's own uses of
  `position: fixed` in Debugger RCSS are changed to `absolute` if they depended on the deviation.

#### 3.11.2 `position: sticky`
- Layout as `relative` (groundwork already routes `Sticky` like `Relative`).
- Offset: in `Element::UpdateOffset`/`UpdateAbsoluteOffsetAndRenderBoxData`, for `Sticky`, find the
  nearest ancestor scroll container (`overflow` ≠ `visible`/`clip`, or the document), compute the
  sticky view rectangle (its scrollport inset by the element's `top/right/bottom/left` where not
  `auto`), and shift the element so its border box stays inside that rectangle, constrained by its
  containing block's content box (CSS Positioned Layout 3 §3.4). The shift is added to the relative
  offset.
- Invalidation: `SetScrollLeft/Top` already call `DirtyAbsoluteOffset()` recursively
  (`Element.cpp:1009,1027`, `:2225-2242`); sticky elements recompute lazily on the next offset
  query. No scroll listener needed. Hit testing uses the same offsets.
- Creates a stacking context (CSS).

#### 3.11.3 Overflow
- `overflow: clip` keyword (and `overflow-clip-margin` registered, length only): clips like
  `hidden` but is not a scroll container (no scrollbars, not scrollable by script/wheel).
- Value fix-up (CSS): if one axis is `visible`/`clip` and the other is `hidden/scroll/auto`,
  `visible` computes to `auto` and `clip` to `hidden`.
- Viewport propagation: if `<html>`'s `overflow` is `visible`, `<body>`'s `overflow` is used for the
  document (viewport) and `<body>` computes as `visible` (CSS Overflow 3 §3.3). Implemented in
  `ElementStyle::ComputeValues` for the document and its `body` child (tag check against the HTML
  root model).
- Scrollbar gutter: `scrollbar-gutter: stable` registered (reserves the scrollbar width);
  `scrollbar-width: none|thin|auto` and `scrollbar-color` are mapped by compat to RmlUi scrollbar
  element styles (phase 2) — unchanged.

**Tests:** `Tests/Source/UnitTests/PositionFixed.cpp` (containing block = viewport; with transformed
ancestor; no movement on ancestor/document scroll; not clipped by overflow ancestors; hit test),
`PositionSticky.cpp` (shift values at scroll positions, containing-block constraint), extend
`Layout.cpp` (overflow clip/fix-up/propagation). Goldens: `p3/position/{fixed-viewport-scrolled,
fixed-in-transform,fixed-not-clipped,sticky-header-list,sticky-bottom,sticky-in-flex,
overflow-clip,body-overflow-propagation}.html` (scroll positions set by fixture steps).

---

### 3.12 Box, flex and sizing parity (package `p3-layout`)

#### 3.12.1 `order`
Flex containers call `GetLayoutChildren(…, apply_order = true)` (groundwork); paint order follows
`order` too (CSS: flex items paint in order-modified document order) — the stacking-context
iteration uses the same sorted list for flex/grid containers.

#### 3.12.2 `flex-basis: content`, `flex` keywords
`flex-basis` (`StyleSheetSpecification.cpp:420`) accepts `content`, `min-content`, `max-content`,
`fit-content`; `GetItemSizing` (`FlexFormattingContext.cpp:~150-190`) treats `content` as
`auto` with an `auto` main size. `flex: none|auto|initial|<n>` keyword forms checked against CSS
(the `Flex` shorthand type, `PropertySpecification.cpp:310`).

#### 3.12.3 `min-width/min-height: auto` (automatic minimum size)
- Initial value becomes `auto` (§3.1.2). For non-flex/grid items `auto` = 0 (CSS).
- Flex items: automatic minimum size in the main axis = min(specified size suggestion, content
  size suggestion = min-content size, clamped by max size; aspect-ratio transferred size) unless
  the item is a scroll container (then 0) — CSS Flexbox §4.5. Uses the groundwork min-content
  API. This is the single largest flex parity fix (text in flex rows no longer collapses).
- Grid items: handled by p3-grid's minimum contribution (Taffy `minimum_contribution`).

#### 3.12.4 Intrinsic sizing keywords
`width`, `height`, `min-*`, `max-*`, `flex-basis` accept `min-content`, `max-content`,
`fit-content`, `fit-content(<length-percentage>)` (width/height only), `stretch` (and
`-webkit-fill-available` alias). `LengthPercentageAuto` gets a `Type::Keyword` with the keyword in
the value bits (PR #983 already widened the type field). Resolution in `LayoutDetails::BuildBox`
and `BuildComputedHorizontalSize/VerticalSize` (`LayoutDetails.h:16-23`) via the groundwork
intrinsic API: inline axis exact; block axis `min-content`/`max-content`/`fit-content` = `auto`
(CSS for block-axis heights in horizontal writing mode). `FlexFormattingContext::GetMinContentSize`
(new, mirrors `GetMaxContentSize`, `FlexFormattingContext.cpp:95-114`).

#### 3.12.5 `aspect-ratio`
Computed: `auto`, ratio, or `auto && ratio`. Applied in `LayoutDetails::BuildBox` (block-level and
absolutely positioned boxes: when one axis is auto and the other definite, the auto axis comes from
the ratio, respecting `box-sizing` and min/max transfer), flex items (`GetItemSizing`, transferred
sizes), grid items (p3-grid via Taffy's `aspect_ratio`), replaced elements (`auto` uses natural
ratio; `ReplacedFormattingContext`). `auto && ratio` on replaced elements prefers natural ratio.

#### 3.12.6 Margin collapsing
6.3 collapses adjacent sibling margins only (`BlockContainer.cpp:279-316`). Add CSS 2.1 §8.3.1:
- parent ↔ first in-flow child top margins when the parent has no top border/padding, is not a BFC
  root (`overflow` ≠ visible, flow-root, flex/grid item, float, abs, inline-block establish one —
  `FormattingContext.cpp:34-39` already decides "independent" formatting), and no clearance;
- parent ↔ last in-flow child bottom margins when, additionally, the parent's height is `auto`
  and `min-height` is 0/auto;
- collapse-through of empty blocks (no height, min-height, padding, border, line boxes);
- the collapsed margin of the first child moves the parent's position (the parent's border box
  starts after the collapsed margin), implemented by deferring the parent's top margin in
  `BlockContainer` until its first child is placed (`BlockContainer::NextBoxPosition`,
  `:279-283`, and `CloseChildBox`).
- `<body>`'s margin collapses with its first child as in browsers (UA `body{margin:8px}` + `h1`).

#### 3.12.7 Flex alignment keywords
Map the groundwork enum additions in `FlexFormattingContext`: `normal` = `stretch` for
`align-items`/`align-content` and `flex-start` for `justify-content`; `flex-start`/`flex-end`
follow the flex direction (they swap for `row-reverse`/`column-reverse` and `wrap-reverse`), while
`start`/`end` follow the container's writing mode and do **not** swap with `*-reverse` (CSS Box
Alignment 3); `left`/`right` (justify-content on a horizontal main axis; behave as `start`
otherwise), `self-start`/`self-end` (the item's own writing mode, equal to `start`/`end` here),
`first baseline`/`last baseline`, `safe` overflow fallback (`safe center` → `start` when the item
or content overflows). `justify-items`/`justify-self` are ignored in flex (CSS).

#### 3.12.8 `display: contents`, `flow-root`, inline-flex parity
- `display: contents`: the element generates no box (groundwork's layout-children expansion); it
  does not render its own background/border; its children render in its place; it keeps events
  and style inheritance.
- `FormattingContext.cpp:36` checks only `Display::Flex` for "parent is a flex container";
  add `InlineFlex` (and `Grid`/`InlineGrid` for p3-grid).
- Flex items with `display: inline*` blockify (CSS) — verify `ElementStyle` display computation.

#### 3.12.9 Fixture-driven flex/block conformance
The Taffy fixture suites `flex/`, `block/`, `blockflex/`, `leaf/`, `float/` (§5.3) are the oracle
for RmlUi's block and flex formatting; every failure in a fixture that only uses features in scope
is a p3-layout bug to fix. If the share of failing flex fixtures after the targeted fixes above is
large enough that per-case fixes would contort `FlexFormattingContext`, replace its algorithm with
a port of Taffy's flexbox (`/tmp/htmlui-src/taffy/src/compute/flexbox.rs`, MIT) behind the same
`FlexFormattingContext::Format` interface, reusing the grid measuring adapter (§3.14.3) — this is
the package's call, recorded in FORK.md with the before/after pass counts.

**Tests:** extend `Tests/Source/UnitTests/FlexFormatting.cpp` (order, min-size auto, basis
content, alignment keywords), new `IntrinsicSizing.cpp`, `AspectRatio.cpp`, `MarginCollapse.cpp`,
`DisplayContents.cpp`. Goldens: `p3/layout/{flex-order,flex-min-auto-text,flex-basis-content,
fit-content,min-max-content,aspect-ratio-card,aspect-ratio-img,margin-collapse-parent-child,
margin-collapse-empty,display-contents,justify-start-end-safe}.html` plus the Taffy suites.

---

### 3.13 Transitions and animations (package `p3-animation`)

**Current behavior.** Transitions start only in `ElementStyle::UpdateDefinition`
(`ElementStyle.cpp:449-510`), i.e. when the matched rule set changes, and only if old and new
definitions are non-null (`:485`, `:385`); `TransitionPropertyChanges` (`:378-447`) computes the
target with an **empty inline dictionary** (`:406,413`) and `all` uses `transitions[0]`
(`:424-435`). Inline changes (`SetProperty`, style attribute, data binding, Lua) never transition.
`StartTransition` (`Element.cpp:2670-2704`) writes the start value as an **inline** property
(`:2699`); `AdvanceAnimations` (`:2847-2889`) writes animated values inline (`:2857`) and removes
them on completion (`:2879-2880`) — clobbering user inline styles, effectively `fill-mode: none`.
`paused` means "never started" (`:2789`). `ElementAnimation` uses the *to* key's tween for a segment
(`ElementAnimation.cpp:735`, CSS uses the segment start's), alternate flips per iteration (`:765`),
delay has no backwards fill (`:748`), `dt` is clamped to 0.1 s (`:751`, time drift on slow frames).
`Tween` (`Include/RmlUi/Core/Tween.h:10-36`): named Penner curves only; `Callback` is a bare
function pointer. `PropertyParserAnimation` (`PropertyParserAnimation.cpp`): keywords at `:27-77`,
comma split not paren-aware (`:97`), durations via `sscanf("%fs")` (`:164,267`) so `200ms` parses as
a number (rejected), durations ≤ 0 rejected (`:204,323`), transition keywords case-sensitive
(`:240`). Only `transition` and `animation` exist (`StyleSheetSpecification.cpp:399-400`).
PR #983 (applied in groundwork) already extends this parser for calc times — p3-animation builds on
the PR's version.

#### 3.13.1 Timing functions
- `Tween` gains a kind: `Legacy` (existing named curves, kept for RCSS and the Lua
  `anime` API), `CubicBezier{x1,y1,x2,y2}` (Newton–Raphson with bisection fallback, as WebKit's
  `UnitBezier`, precision 1e-6/duration-scaled), `Steps{n, position}` (`jump-start|jump-end|
  jump-none|jump-both|start|end`, with the CSS "before flag" for jump-start at t=0 during delay),
  `Linear{stops}` (`linear()` with optional input percentages, CSS Easing 2). `operator==`,
  `ToString` (CSS serialization) updated. Keywords: `linear`, `ease` (.25,.1,.25,1), `ease-in`
  (.42,0,1,1), `ease-out` (0,0,.58,1), `ease-in-out` (.42,0,.58,1), `step-start`, `step-end`.
  RmlUi's names (`cubic-in-out`, …) stay accepted.
- Output values outside [0,1] (overshoot) are passed to interpolation (CSS allows extrapolation
  for numeric values; colours clamp).

#### 3.13.2 Parsing
Rewrite `PropertyParserAnimation` on the groundwork comma splitter (paren/quote aware) with a CSS
tokenizer per item:
- time: `<number>(s|ms)`, `0` without unit only where CSS allows (it does not for times — reject),
  negative delays allowed, negative durations rejected; calc times (PR).
- `transition`: `[ none | <property> ] || <time> || <easing> || <time> || <transition-behavior>`.
  First time = duration, second = delay. Property names validated against the specification
  (unknown names kept and ignored, CSS).
- `animation`: `<time> || <easing> || <time> || <iteration-count> || <direction> || <fill-mode> ||
  <play-state> || [ none | <keyframes-name> ]` with the CSS rule that the name is the token not
  matching any keyword (and quoted names).
- Keyword matching case-insensitive.

#### 3.13.3 Longhands
- `transition-property`, `transition-duration`, `transition-timing-function`, `transition-delay`,
  `transition-behavior` (`normal | allow-discrete`); `animation-name`, `animation-duration`,
  `animation-timing-function`, `animation-delay`, `animation-iteration-count` (`infinite | <number>`,
  fractional allowed), `animation-direction` (`normal reverse alternate alternate-reverse`),
  `animation-fill-mode` (`none forwards backwards both`), `animation-play-state`
  (`running paused`), `animation-composition` (registered; `replace` only, others diagnosed).
- All are comma-list `StyleValue`s; `transition` and `animation` become `Custom` shorthands that
  set the longhands. The computed `TransitionList`/`AnimationList` (`Include/RmlUi/Core/Animation.h`)
  are built from the longhands at compute time with CSS list-length rules (repeat shorter lists,
  truncate by `*-name`/`transition-property` length). `ComputedValues::transition()/animation()`
  (`ComputedValues.h`) return the built lists. The old `PropertyId::Transition`/`Animation` ids are
  replaced by the shorthand ids (RCSS `transition: …` keeps working through the shorthand).

#### 3.13.4 Transitions on any computed-value change
- Per element, keep a snapshot of the last **computed-time resolved** value (after `var()` and
  CSS-wide keyword resolution) for each property listed in its transition list (`all` = every
  animatable property).
- In `ElementStyle::ComputeValues`, after resolving each dirty id (main loop `:974-993` and the
  font-size/line-height pre-passes), if the id is transitionable and the new value differs from the
  snapshot and the change was not produced by the animation layer, start a transition from the
  **current animated value** (if a transition is running, CSS reversing-shortening rule with
  `reverse_adjustment_factor`) to the new value, then update the snapshot. Inherited-value changes,
  `var()` changes, inline/style attribute changes, data-binding and Lua changes, and
  definition null↔non-null changes all transition now. The `UpdateDefinition` hook
  (`ElementStyle.cpp:500-501`) is removed.
- Elements that are created or whose `display` was `none` in the previous style computation do not
  transition (CSS "before-change style" does not exist), unless `@starting-style` is supported —
  it is: register `@starting-style` blocks in the stylesheet parser as an alternate definition used
  as the before-change style on first style resolution (common in modern LLM CSS for entry
  animations).
- `transition-behavior: allow-discrete`: discrete properties (keywords, non-interpolable pairs)
  transition only with it, flipping at 50% (and `display`/`visibility`/`content-visibility`
  special rules at `ElementAnimation.cpp:209-231`); without it the change is immediate.
- `transitionrun`, `transitionstart`, `transitionend`, `transitioncancel` events (6.3 has
  `transitionend` only, `EventSpecification.cpp`): add the others with `propertyName`,
  `elapsedTime` parameters.

#### 3.13.5 Animation layer
- The layer dictionaries `animated_properties` and `transition_properties` provided by p3-base
  (§3.1.10) are consulted in CSS cascade order: transitions > `!important` declarations >
  animations > normal declarations (p3-cascade adds the importance step).
- `StartTransition`/`AdvanceAnimations`/`HandleTransitionProperty`/`HandleAnimationProperty`
  (`Element.cpp:2670-2889`) write into these dictionaries instead of `SetProperty`/
  `RemoveProperty`: user inline styles are never touched.
- Fill modes: `forwards` keeps the last keyframe value in `animated_properties` after the end;
  `backwards` applies the first keyframe (direction-aware) during the delay; `both` = both.
- Direction: `reverse`, `alternate-reverse` start reversed; iteration counts fractional (end value
  interpolated at the fractional position).
- `animation-play-state: paused` freezes the animation's local time (no restart, no jump on
  resume); changing only the play state does not restart the animation (6.3 restarts every
  animation whenever `dirty_animation` is set, `Element.cpp:1966-1976`, `2752-2845`: diff the
  lists and restart only changed entries, matched by name and index as CSS).
- Timing: per-keyframe `animation-timing-function` inside `@keyframes` (parsed in the keyframe
  block, stored per key); a segment uses the **start** key's easing; keyframes with multiple
  selectors (`0%, 100% {}`), duplicate offsets merged in order, missing `from`/`to` synthesized from
  the underlying value (6.3 does this), `!important` in keyframes ignored.
- Time: remove the 0.1 s `dt` clamp (`ElementAnimation.cpp:751`); animations use the context's
  clock so a stalled frame does not slow animations (the engine already supplies wall time through
  `SystemInterface::GetElapsedTime`).
- Events: `animationstart`, `animationiteration`, `animationend`, `animationcancel` with
  `animationName`, `elapsedTime`.

#### 3.13.6 Interpolation coverage
- `box-shadow` (6.3: `BOXSHADOWLIST` not in the valid list, `ElementAnimation.cpp:640-648`) —
  pairwise, padding the shorter list with transparent zero shadows, `inset` mismatch discrete.
- `StyleValue` types (groundwork): text-shadow, background-position/size, grid tracks (discrete
  unless same structure — CSS Grid 2 interpolates track lists of equal length), counters/content
  discrete.
- Colours interpolate in premultiplied sRGB (CSS default for legacy colours); `color-mix` results
  are plain colours.
- `transform` lists: 6.3 behavior plus individual properties below.

#### 3.13.7 Individual transform properties
`translate` (`<lp> <lp>? <length>?`), `rotate` (`<angle> | <axis> <angle>`), `scale`
(`<number|%>{1,3}`), not inherited, `none` initial. Composed in `Element::UpdateTransformState`
(around `Element.cpp:2924-2992`) as `translate · rotate · scale · transform` around
`transform-origin`; each creates a stacking context and contributes to `has_local_transform`.
Interpolated independently (common LLM pattern: `transition: scale .2s` with `:hover{scale:1.05}`).

**Tests:** `Tests/Source/UnitTests/Easing.cpp` (bezier sample tables against reference values,
steps positions incl. before-flag, linear() stops), extend `Animation.cpp` (parsing matrix of
`transition`/`animation` shorthands and longhands, ms/s/0s, list-length rules; inline-change
transitions; fill modes; play-state pause/resume times; reverse/alternate-reverse; per-keyframe
easing; events order), `TransitionsAnyChange.cpp` (style attribute, `SetProperty`, `var()` change,
inherited colour change, class toggle, `display:none` → no transition, `@starting-style`),
`IndividualTransforms.cpp`. Goldens (multi-capture at given times, §5.2):
`p3/animation/{transition-hover-ease,transition-inline-style,transition-var,
transition-allow-discrete-display,steps-sprite,cubic-bezier-overshoot,keyframes-fill-forwards,
keyframes-backwards-delay,alternate-reverse,play-state-paused,per-keyframe-easing,
starting-style-entry,box-shadow-transition,scale-rotate-translate}.html`.

---

### 3.14 CSS Grid (package `p3-grid`)

Source: Taffy master `8409f3f8c6de1244cb4cf7fc6c485be2481876cd` (2026-10-06, "Grid: treat
indefinite percentage min tracks as auto (#1261)"), MIT (`/tmp/htmlui-src/taffy/LICENSE`,
"Copyright (c) 2018 Visly Inc., (c) 2026 Taffy Authors"). Ported code carries the MIT notice in each
new file header and `engine/libs/rmlui/LICENSE-taffy.txt`; FORK.md records the Taffy commit.

#### 3.14.1 Properties and values
Longhands (not inherited, force layout): `grid-template-columns`, `grid-template-rows`,
`grid-template-areas`, `grid-auto-columns`, `grid-auto-rows`, `grid-auto-flow`,
`grid-row-start`, `grid-row-end`, `grid-column-start`, `grid-column-end` (+ `justify-items`,
`justify-self` from groundwork). Shorthands: `grid-row`, `grid-column` (`<line> [/ <line>]?`),
`grid-area` (1–4 values with CSS name-replication rules), `grid-template`
(`none | <rows> / <columns> | [ <line-names>? <string> <track-size>? <line-names>? ]+ [ / <explicit-track-list> ]?`),
`grid` (`<grid-template> | <rows> / [ auto-flow && dense? ] <auto-columns>? |
[ auto-flow && dense? ] <auto-rows>? / <columns>`), all `Custom` (groundwork §3.1.4).
`gap`/`row-gap`/`column-gap` exist (`StyleSheetSpecification.cpp:367-369`); fix `column-gap`'s
relative target (`ContainingBlockHeight` → width; 6.3 bug) and accept `normal` (= 0 for grid/flex).
Legacy `grid-gap`, `grid-row-gap`, `grid-column-gap` aliases registered.

Value types (`Source/Core/GridStyleValues.{h,cpp}`, `StyleValue` subclasses):
```
struct GridTrackBreadth { enum Kind {Length, Percent, Calc, Auto, MinContent, MaxContent, Fr};
                          Kind kind; float value; Unit unit /*px,em,rem,vw,..*/; CalculationPtr calc; };
struct GridTrackSize   { GridTrackBreadth min, max; bool fit_content; GridTrackBreadth fit_content_limit; };
struct GridLineNames   { StringList names; };
struct GridTrackRepeat { enum Count {Fixed, AutoFill, AutoFit}; Count count; int n;
                          Vector<GridLineNames> line_names; Vector<GridTrackSize> tracks; };
struct GridTemplateComponent { variant<GridTrackSize, GridTrackRepeat>; };
class GridTrackListValue : StyleValue { Vector<GridTemplateComponent> components;
                          Vector<GridLineNames> line_names /* size components+1 */; bool none; };
class GridAutoTracksValue : StyleValue { Vector<GridTrackSize> tracks; };
class GridTemplateAreasValue : StyleValue { int rows, columns;
                          Vector<GridArea{name,row_start,row_end,column_start,column_end}> areas; };
class GridLineValue : StyleValue { enum Kind {Auto, Line, NamedLine, Span, NamedSpan};
                          Kind kind; int16_t index; String name; };
```
Units stay unresolved in the value; layout resolves lengths with the element's `LengthContext`
(em/rem/vw/vh/dp) and `%`/calc against the grid content box (indefinite → treated as `auto` for
min tracks per the Taffy commit above, as `auto`/content for max tracks per CSS). Parsers validate
CSS rules (one auto-repeat per list, auto-repeat only with fixed sizes, `fr` not allowed as min,
`minmax()`, `fit-content()`, `repeat()` with line names, area strings rectangular). `grid-auto-flow`
via keyword aliases (`row`, `column`, `dense`, `row dense`, `dense row`, …).

#### 3.14.2 Module mapping (Taffy → C++ in `Source/Core/Layout/`)

| C++ file | Ports (Taffy, `src/compute/grid` unless noted) | Contents |
|---|---|---|
| `GridTypes.h` | `types/coordinates.rs`, `types/grid_track_counts.rs`, `types/grid_track.rs`, `types/grid_item.rs` (struct only) | `GridLine` (1-based, negative from end, int16), `OriginZeroLine` (`IntoTrackVecIndex`, implied implicit tracks), `TrackCounts {negative_implicit, explicit, positive_implicit}` with `OzLineToNextTrack`/`TrackToPrevOzLine`; track vector layout `[gutter, track, gutter, …, gutter]`; `GridTrack {kind (Track/Gutter), is_collapsed, min_fn, max_fn, offset, base_size, growth_limit, content_alignment_adjustment, item_incurred_increase, base_size_planned_increase, growth_limit_planned_increase, infinitely_growable, fit_content_limit}`; `GridItem {element, source_order, row/column OriginZero lines, row/column track indexes, cached style (size, min/max, margins, padding, border, aspect ratio, align/justify self, overflow, box-sizing, is_compressible_replaced), baseline, baseline_shim, crosses_{flexible,intrinsic}_{row,column}, contribution caches (min-content, minimum, max-content per axis), known-dimensions cache, y_position, height}`; `AvailableSpace {Definite(float), MinContent, MaxContent}`; `AbsoluteAxis`; `MAX_GRID_TRACKS = 10000`. |
| `GridOccupancy.{h,cpp}` | `types/cell_occupancy.rs` | Sparse `CellOccupancyMatrix` (per-track interval lists both orientations, `CellOccupancyState {Unoccupied, DefinitelyPlaced, AutoPlaced}`), `ExpandToFitRange`, `MarkAreaAs`, `LineAreaIsUnoccupied`, `LineAreaCollisionJump`, `OccupiedTrackJump`, `RowIsOccupied`, `ColumnIsOccupied`, `LastOfType`. |
| `GridNamedLines.{h,cpp}` | `types/named.rs` | `NamedLineResolver`: name → line lists per axis from template line names, `repeat()` expansions and areas (`<area>-start/-end` implicit names), `name N`, `span name`, `-start/-end` fallbacks, missing names → first implicit line; explicit count = max(template, areas). |
| `GridExplicit.{h,cpp}` | `explicit_grid.rs`, `implicit_grid.rs` | `ComputeExplicitGridSizeInAxis` (auto-repeat counts: `MaxRepetitionsThatDoNotOverflow` with definite size/max-size, `MinRepetitionsThatDoOverflow` with min-size), `InitializeGridTracks` (auto-fit collapses empty tracks and adjacent gutters), `ComputeGridSizeEstimate`. |
| `GridPlacement.{h,cpp}` | `placement.rs` | Auto-placement (§8.5): definite items, items locked to a row/column, auto items with cursor, `dense` resets the cursor; order-modified document order (groundwork `GetLayoutChildren(…, true)` — Taffy places in document order; the `order` sort is ours). |
| `GridTrackSizing.{h,cpp}` | `track_sizing.rs` | `TrackSizingAlgorithm` §11.4–11.8: `InitializeTrackSizes`, `ResolveItemBaselines`, `ComputeAlignmentGutterAdjustment`, `ResolveIntrinsicTrackSizes` with `ItemBatcher` (span batches; flex-crossing items last), span-1 fast path, `DistributeItemSpaceToBaseSize`, `DistributeItemSpaceToGrowthLimit`, `DistributeSpaceUpToLimits`, `FlushPlannedGrowthLimitIncreases`, `MaximiseTracks`, `ExpandFlexibleTracks` + `FindSizeOfFr`, `StretchAutoTracks`. |
| `GridItemMeasure.{h,cpp}` | `types/grid_item.rs` (contribution functions) | `KnownDimensions`, `MinContentContribution`, `MaxContentContribution`, `MinimumContribution` (automatic minimum, scroll containers 0, clamped by fixed max track functions), `GridAreaSize`, plus the RmlUi measuring adapter (§3.14.3). |
| `GridAlignment.{h,cpp}` | `alignment.rs`, `compute/common/alignment.rs` | `AlignTracks` (gutters as tracks, `space-*`, `stretch`, `safe` fallback), `AlignAndPositionItem` (`normal` = stretch unless preferred size/aspect ratio; auto margins; insets for relatively positioned items), `ApplyAlignmentFallback`, `ComputeAlignmentOffset`. |
| `GridFormattingContext.{h,cpp}` | `mod.rs` (`compute_grid_layout`) | Static `Format(…)` with the scrollbar retry loop (pattern `FlexFormattingContext.cpp:15-93`), private `FormatGrid(state, mode)` running the steps below, static `GetMaxContentSize`/`GetMinContentSize`. |
| `ContainerBox.{h,cpp}` | — | `class GridContainer final : public ContainerBox` (copy of `FlexContainer`, `ContainerBox.h:103`) with `Close()`, `GetShrinkToFitWidth()`, `GetMinContentWidth()`; `LayoutBox::Type::GridContainer` (`LayoutBox.h:13`); `LayoutPools.cpp:24` chunk size includes it. |

Steps of `FormatGrid` (mod.rs line refs): (1) available grid space incl. padding/border, box-sizing,
min/max/preferred, aspect ratio, scrollbar gutter for `overflow: scroll` (:59-156); (2) size-only
short-circuit (:158); (3) explicit grid per axis + named lines (:180); (4) implicit estimate (:234);
(5) placement (:240); (6) initialize tracks (:267); (7) column sizing then row sizing with the
estimate from definite max functions (:295-344); (8) container size (:364); (9) re-run columns/rows
when percentage tracks met an indefinite size or min-content contributions changed after row
sizing (:396-551); (10) track alignment (:558); (11) item layout and positioning (:584-640);
(12) absolutely positioned children (:643, §3.14.5); (13) baselines: first item in the first row
participating in baseline alignment, else first item of the first row (:789); (14) output: border
box size, scrollable overflow, baseline.

#### 3.14.3 Measuring children (adapter)
Taffy works in border-box sizes with `known_dimensions` + `available_space`; RmlUi in content
boxes. The adapter (`GridItemMeasure.cpp`):
- inline axis: known width → return it; `MaxContent` → `LayoutDetails::GetMaxContentWidth`;
  `MinContent` → `LayoutDetails::GetMinContentWidth` (groundwork §3.1.8); `Definite(w)` →
  `min(max-content, max(min-content, w))` (fit-content).
- block axis: width = known width, else the stretched grid-area width, else fit-content width;
  height = `LayoutDetails::GetContentHeightForWidth` (formats with a definite width; pattern
  `FlexFormattingContext.cpp:336-342`).
- results converted between border box and content box with the item's `Box` edges
  (`LayoutDetails::BuildBox(box, grid_area_size, element, BuildBoxMode::UnalignedBlock)`);
  memoized per `FormatGrid` call (and the groundwork per-layout cache) because RmlUi has no layout
  cache and every measurement is a full format with side effects.
- final layout per item: `box.SetContent(resolved_content_size)` + margins from
  `AlignAndPositionItem`, `FormattingContext::FormatIndependent(grid_box, element, &box,
  FormattingContextType::Block)`, `element->SetOffset(position_relative_to_container_border_box,
  element_grid)`; visible/scrollable overflow accumulated as `FlexFormattingContext.cpp:968-976`.

#### 3.14.4 Dispatch and intrinsic size of the grid container
- `FormattingContextType::Grid` (`FormattingContext.h:11-16`); `FormatIndependent`
  (`FormattingContext.cpp:26`) selects it for `Grid`/`InlineGrid`; a parent of `Grid`/`InlineGrid`
  (and `InlineFlex`, 6.3 misses it at `:36`) forces independent formatting of the child.
- `GetOuterDisplayType` (`BlockFormattingContext.cpp:62-85`, groundwork) already maps the display
  values; `InlineGrid` joins the shrink-to-fit set (`LayoutDetails.cpp:428-430`);
  `Element.cpp:2386,2449,2456` treat grid items like flex items for paint (atomic, `z-index` applies).
- `GridContainer::GetShrinkToFitWidth()` = definite width or `GridFormattingContext::
  GetMaxContentSize(element).x` (steps 1–7 with available width `MaxContent`, early-out after column
  sizing — Taffy :332-339); `GetMinContentWidth()` the same with `MinContent`. Grid containers as
  flex items, floats, inline-grids and abs-pos shrink-to-fit therefore size correctly.
- Overflow/scrolling: a grid with `overflow: auto/scroll` reserves scrollbar space per the retry loop
  (scrollbar sizes from `ElementScroll`, `FlexFormattingContext.cpp:51-57` pattern), the scrollable
  overflow is the union of item margin boxes plus end padding (Taffy's `scrollable_overflow`),
  `ContainerBox::SubmitBox`/`CatchOverflow` unchanged.

#### 3.14.5 Absolutely positioned grid children
- Children with `position: absolute` (and `fixed` whose containing block is the grid because of a
  transform) whose containing block is the grid container (the grid is positioned) get their
  containing block from their grid placement: lines resolved with the named-line resolver, lines
  outside the grid or `span`/`auto` → the padding edge, start line = start of the following track,
  end line = end of the preceding track (Taffy `resolve_absolute_grid_area`, mod.rs:1040 and
  `resolve_absolute_grid_axis` :960). The rect is passed through the groundwork containing-block
  override to `AddAbsoluteElement`; the static position is the grid area's start corner
  (`resolve_static_position_grid_area`, mod.rs:842).
- When the containing block is an ancestor of the grid, the static position comes from the grid
  content box (CSS).

#### 3.14.6 Out of scope inside grid
`subgrid`, `masonry`, RTL axis mirroring (§2.2). `grid-template-*: subgrid` and `masonry` values
are parse errors (diagnosed by compat).

**Tests:** RmlUi unit tests `Tests/Source/UnitTests/GridParsing.cpp` (every grammar form,
invalid inputs, shorthand expansions and serialization round-trips), `GridLayout.cpp` (placement
incl. named lines/areas/negative lines/spans/dense, track sizing numbers for fr/minmax/fit-content/
auto-fill/auto-fit, intrinsic contributions, abs-pos areas, scroll overflow), plus the Taffy fixture
suites `grid/`, `blockgrid/`, `gridflex/` through the browser-box harness (§5.3) — all must pass
except the excluded list (rtl, writing-mode, `x*` fixtures). Goldens: `p3/grid/{dashboard-areas,
holy-grail,auto-fill-cards,auto-fit-collapse,minmax-fr,fit-content,dense-gallery,span-negative-
lines,named-lines,align-justify-matrix,place-shorthands,abs-pos-area,scroll-grid,inline-grid,
grid-in-flex,flex-in-grid,percent-gap}.html`.

## 4. Work packages

Waves: **wave 0** (2 packages in parallel) lands first; **wave 1** (7 packages in parallel) branches
from `html-ui` after both wave-0 packages merged; **wave 2** (1 package) after all wave-1 packages
merged. Worktrees and branches follow BRIEF.md (`…-wt/<slug>` on `html-ui/<slug>`).

Ownership rules on top of BRIEF.md: in the hot-spot files (§3.0) a wave-1 package edits only its
own groundwork section and the functions listed as owned below. Logic goes into the package's own
new source files; the hot-spot hunk is a call into it. A package that needs a change in another
package's owned function makes it additive (new overload, new hook call) and lists it in its report.

### Wave 0

#### `p3-base` — fork groundwork and calc
- **Goal:** §3.1 and the PR #983 application of §3.2; the fork compiles and all existing tests pass
  with the new schema, so wave-1 packages only add.
- **Owned (fork):** `Include/RmlUi/Core/{ID.h, Unit.h, PropertyIdSet.h, StyleValue.h (new),
  Variant.h, Variant.inl, TypeConverter.h, PropertySpecification.h, StyleSheetSpecification.h}`,
  `Source/Core/{Variant.cpp, TypeConverter.cpp, PropertySpecification.cpp, XMLNodeHandlerBody.cpp,
  XMLNodeHandlerHtml.{h,cpp} (new), StyleValue.cpp (new), ElementMeta.h}`, all files of PR #983,
  `Source/Core/Layout/{LayoutDetails.{h,cpp} (GetLayoutChildren, intrinsic API),
  LayoutBox.h, LayoutEngine.cpp (measure cache)}`, `ContainerBox.{h,cpp}` (`AbsoluteElement`
  override), the section structure of the hot-spot files, `StyleTypes.h` enum widening
  (`Display`, `Position`, alignment enums), `ElementStyle` animation/transition layer dictionaries
  (`animated_properties`, `transition_properties` and their lookup, ignoring importance until
  p3-cascade), `engine/libs/rmlui/CMakeLists.txt`.
- **Additive touches:** `Factory.cpp:180` (`"html"` instancer, XML handler registration
  `Factory.cpp:260-268`), `ComputeProperty.cpp` root `%`/`em`, `BlockContainer`/`InlineContainer`/
  `FlexFormattingContext`/`TableFormattingDetails` child iteration switched to
  `GetLayoutChildren`, `GetMinContentWidth` overrides, `Element.cpp` (`GetPseudoElement`
  declaration returning null, stacking iteration order, `UpdateOffset`/`GetContainingBlock`
  override use), engine `HtmlUIContext` (`SetDocumentsBaseTag("html")`, `"html"` instancer if the
  engine registers custom instancers), compat emitter (`<html>` root) and UA sheet (`html`, `body`
  rules), compat flag `root-merge` off.
- **Deps:** phases 0–2 merged.
- **Deliverables:** commits `rmlui: calc()/min()/max()/clamp() (upstream PR #983 …)`,
  `rmlui: widen PropertyId and Unit`, `rmlui: HTML root element`, `rmlui: CSS initial values`,
  `rmlui: AnyOrder and Custom shorthand types`, `rmlui: StyleValue variant`,
  `rmlui: layout children iteration and order`, `rmlui: intrinsic sizing API`,
  `rmlui: containing-block override`, `rmlui: alignment keywords`, `rmlui: animation layers`;
  FORK.md "Phase 3" table with these rows; the section headers in hot-spot files.
- **Tests:** PR #983's `Calculation.cpp`, `CalculationTypes.cpp`; new `HtmlRoot.cpp` (html/body
  structure, attributes on both, rem relative to html, `html{font-size:62.5%} body{font-size:1.6rem}`
  = 16px, default base tag unchanged), `InitialValues.cpp` (registered default ≡ default computed
  value for every property), `ShorthandTypes.cpp` (AnyOrder resets/any order/trailing-token
  rejection; Custom dispatch), `StyleValue.cpp`, `LayoutChildren.cpp`, `IntrinsicSizing.cpp`
  (min/max-content of block/inline/flex: longest word, nowrap, floats), `ContainingBlockOverride.cpp`;
  RmlUi suite green; engine golden harness green (no visual change except documented initial-value
  changes, whose phase-0/1/2 goldens are regenerated and inspected).

#### `p3-testkit` — reference renders and conformance harness
- **Goal:** §5.1–§5.4 infrastructure that every wave-1 package uses.
- **Owned:** `tools/htmlui/run-rmlui-tests.sh`, `tools/htmlui/reference/**` (Node project,
  `render.mjs`, `regen.sh`, `README.md` with the commands), `tools/htmlui/import-taffy-fixtures.py`,
  `engine/tests/htmlui/{BrowserParityTest.cpp, BoxParityTest.cpp, FixtureRunner.{h,cpp},
  ImageCompare.{h,cpp} (if phase 0 has no tolerant comparison), fonts/**,
  fixtures/p3/manifest.json, fixtures/taffy/**, golden/browser/**}`; the vendored RmlUi
  `Tests/` infrastructure if phase 0 did not vendor it (`Tests/CMakeLists.txt`,
  `Tests/Source/Common/**`, `Tests/Dependencies/**`, `Tests/Source/UnitTests/{CMakeLists.txt,
  main.cpp}` + the 6.3 test sources).
- **Additive touches:** `engine/tests/htmlui/CMakeLists.txt` (new test sources),
  `.gitignore` (`tools/htmlui/reference/node_modules`).
- **Deps:** phases 0–2. Independent of `p3-base` (fixtures are HTML; failing parity tests are
  allowed in wave 0 and tracked in `known-failures.txt`).
- **Deliverables:** working scripts with documented commands; imported Taffy fixtures (grid,
  flex, block, blockflex, blockgrid, gridflex, leaf, float) with Chrome references;
  `engine/tests/htmlui/fixtures/taffy/known-failures.txt` from a baseline run against the
  pre-phase-3 fork, one line per failing fixture with `owner=<package>` and a reason class
  (`grid-unsupported`, `flex-min-auto`, `margin-collapse`, `rtl-excluded`, …); the excluded list
  (rtl, writing-mode, `x*` fixtures) separately in `excluded.txt` with reasons.
- **Tests:** self-tests of the harness: a fixture that must match (identical simple boxes), a
  fixture that must fail (1 px offset) — the comparison reports it; `render.mjs` reproducibility
  (two runs byte-identical PNGs).

### Wave 1

| Slug | Goal (sections) | Owned (fork unless noted) | Additive touches | Compat flags flipped (Appendix A) |
|---|---|---|---|---|
| `p3-values` | §3.2 extensions, §3.3 | `PropertyParserNumber.*` (unit table), `PropertyParserColour.*`, `ComputeProperty.*`, `CalculationParser/Resolver` unit additions, new `PropertyParserFontFamily.*`, `ShorthandFont.cpp`, `ShorthandOrigin.cpp`, `LogicalProperties.cpp`, `PropertyParserCursor.*`, `ColorSpaces.cpp` (color-mix) | hot-spot "Values" sections; `ElementStyle::ComputeValues` hooks (currentcolor pre-pass, line-height normal post-pass); `DecoratorGradient` stop colour resolution; engine `RmlFontEngine` family-list test only | `calc`, `rgba-alpha`, `color-space-syntax`, `named-colors`, `currentcolor`, `font-family-list`, `font-shorthand`, `units-vmin-vmax`, `line-height-normal`, `transform-origin-keywords`, `logical-properties`, `cursor-list` |
| `p3-cascade` | §3.4, §3.5, §3.6 | `StyleSheetParser.*` (except `@keyframes` body parsing), `StyleSheetNode.*`, `StyleSheetSelector.*`, `StyleSheetFactory.*`, `StyleSheet.cpp`, `StyleSheetContainer.cpp`, `ElementDefinition.*`, `PropertyDictionary.*`, `Property.*`, `PropertiesIterator.h`, `ElementStyle.cpp` cascade functions (`GetLocalProperty*`, `GetSpecifiedProperty*`, `GetProperty`, keyword resolution), `Context::OnFocusChange`, `Elements/ElementFormControl.cpp`, new `PseudoElements.{h,cpp}`, `ContentValue.*`, `CounterResolver.*`, `ListMarker.*` | `Element.cpp` (`UpdateDefinition` pseudo generation, `OnAttributeChange` style/`attr()`, `GetPseudoElement`, focus default action removal), `Context::GetElementAtPoint` (pseudo → originating), `ElementDocument` (counter pass), `BlockContainer`/`InlineContainer` marker hooks, widgets' focus code, compat UA sheet (lists, `:focus-visible`), compat `revert` origin call | `important`, `css-wide-keywords`, `all-shorthand`, `root-selector`, `is-where`, `has`, `focus-within`, `focus-visible`, `enabled`, `attr-flags`, `nth-of`, `pseudo-before-after`, `content`, `counters`, `list-markers` |
| `p3-paint` | §3.7, §3.8, §3.9 | `GeometryBackgroundBorder.*`, new `GeometryBorderStyles.cpp`, `MeshUtilities.cpp`, `ElementBackgroundBorder.*`, new `ElementBackgroundLayers.*`, `BoxShadowCache.*`, `GeometryBoxShadow.*`, `Include/RmlUi/Core/RenderBox.h`, `ElementEffects.*`, `Element.cpp` render/stacking functions (`Render`, `GetRenderBox`, `OnPropertyChange` stacking block, `AddToStackingContext`, `IsVisible` semantics), `ElementImage.*`, decorator opacity sites, new `ShorthandBorder.cpp`, `ShorthandBackground.cpp`, `ObjectFit.cpp`, filter parsers | `ElementText` draw-opacity line, `Context::GetElementAtPoint` radius hit test, `ElementUtilities::GetClippingRegion` (layer bounds), `Tests/Source/Common/TestsInterface.*` layer counters, engine `RmlRenderer.cpp` opacity filter if missing, compat UA sheet (`:focus-visible` outline) | `border-shorthand`, `border-style`, `border-radius-percent`, `border-radius-elliptical`, `outline`, `opacity-group`, `background-shorthand`, `background-image-url`, `background-gradient`, `mask-image-css`, `object-fit`, `visibility-inherit`, `z-index-static` |
| `p3-text` | §3.10 | `ElementText.*` (except draw-opacity line), `TextShapingContext.h`, `Layout/{InlineContainer, InlineBox, InlineLevelBox, LineBox}.*`, new `UnicodeCaseTables.inl`, `Utilities/gen_case_tables.py`, `TextShadow.*`, `ShorthandTextDecoration.cpp`, `ShorthandWhiteSpace.cpp` | engine `RmlFontEngine.cpp` (`word_spacing`), `FontEngineInterface.h` doc comment, `ElementFormControl` text inputs (`user-select`) | `text-shadow`, `letter-spacing-float`, `word-spacing`, `text-transform-unicode`, `white-space-break-spaces`, `overflow-wrap`, `text-decoration-longhands`, `text-align-start-end`, `user-select` |
| `p3-layout` | §3.11, §3.12 | `Layout/{BlockContainer, BlockFormattingContext, FlexFormattingContext, LayoutDetails (beyond groundwork), ReplacedFormattingContext, FormattingContext.cpp (non-grid branches)}`, `ContainerBox.*` (fixed flag, overflow), `ElementUtilities::GetClippingRegion`, `Element.cpp` offset functions (`UpdateOffset`, `UpdateAbsoluteOffsetAndRenderBoxData`, `GetContainingBlock`, `SetOffset`), `ElementDocument.cpp` (overflow propagation), `ElementScroll.*` | `Context::GetElementAtPoint` (fixed clip rule), Debugger RCSS if it used `fixed` | `position-fixed`, `position-sticky`, `overflow-clip`, `flex-order`, `flex-basis-content`, `min-size-auto`, `sizing-keywords`, `aspect-ratio`, `margin-collapse`, `display-contents`, `alignment-keywords` |
| `p3-animation` | §3.13 | `Tween.*`, `PropertyParserAnimation.*`, `Animation.h`, `ElementAnimation.*`, `Element.cpp` animation functions (`StartTransition`, `HandleTransitionProperty`, `HandleAnimationProperty`, `AdvanceAnimations`, `UpdateTransformState`), `ElementStyle::TransitionPropertyChanges` replacement and snapshot code, new `Easing.*`, `ShorthandAnimation.cpp`, `IndividualTransforms.cpp`, `EventSpecification.cpp` (events) | `StyleSheetParser` keyframe block parsing (per-keyframe easing) and `@starting-style` (new at-rule branch), `ElementStyle::ComputeValues` hook (transition start) | `easing-functions`, `time-ms`, `transition-longhands`, `animation-longhands`, `transition-any-change`, `fill-mode`, `play-state`, `transition-behavior`, `individual-transforms`, `starting-style` |
| `p3-grid` | §3.14 | new `Layout/Grid*.{h,cpp}`, `GridStyleValues.*`, grid parsers and `ShorthandGrid.cpp`, `LICENSE-taffy.txt` | `FormattingContext.{h,cpp}` grid branch, `ContainerBox.{h,cpp}` (`GridContainer` class appended), `LayoutBox.h` type, `LayoutPools.cpp`, `LayoutDetails.cpp:428-430` shrink-to-fit set, `Element.cpp:2386,2449,2456` grid items paint, `column-gap` relative target | `display-grid`, `grid-template`, `grid-placement`, `grid-areas`, `place-shorthands`, `justify-items-self` |

Deliverables of every wave-1 package: its `rmlui:` commits, FORK.md rows, RmlUi unit tests and
golden fixtures listed in its §3 section, removal of its entries from `known-failures.txt`, compat
flag flips with updated compat tests and consistency tests (§5.4), diagnostics for its §2.2 items,
and the user-facing support notes in `docs/mcp/guide/html-ui.md` for its features.

### Wave 2

#### `p3-integration` — cross-feature verification and closing
- **Goal:** prove the patches work together, close the phase.
- **Owned:** `engine/tests/htmlui/fixtures/p3/integration/**`, FORK.md consolidation, the phase-3
  part of `docs/mcp/guide/html-ui.md`, `docs/html-ui/FORK.md` upstream-sync notes.
- **Work:** cross-feature fixtures (`::after` with transition on `:hover` inside a grid item with
  group opacity; sticky header in a scrolling grid; `calc()` grid tracks with `var()`; `!important`
  vs animation; fixed modal with backdrop-filter over a scrolled document; LLM-style sample pages:
  game main menu, settings screen with form controls, inventory grid, HUD with fixed corners,
  dialog with `::before` overlay — written in plain HTML/CSS as an agent would write them);
  audit that every compat property-table row is `native` or `diagnosed` with no stale `rewritten`
  rows for phase-3 features; empty `known-failures.txt` (only `excluded.txt` remains); upstream
  rebase rehearsal: rebase the fork's `rmlui:` commits onto current upstream `master` in a scratch
  clone and record conflicts per patch in FORK.md; run the full acceptance list (§6).
- **Deps:** all wave-1 packages merged.

## 5. Test plan

### 5.1 RmlUi's own unit tests (standalone, host)
The fork's `Tests/` must keep building and passing. They need RmlUi's sample shell (FreeType for
`Shell::LoadFonts`, a backend for `rmlui_shell`), which the engine build does not have, so they are
built standalone on the host (verified on this host: Ubuntu, GCC 13, FreeType 2.13 via
`pkg-config freetype2` = 26.1.20, no GLFW/SDL packages, X11 headers without `xf86vmode.h`):

```
# tools/htmlui/run-rmlui-tests.sh  (what it does; verified manually during spec writing)
# 1. GLFW 3.4 from source, cached in build/rmlui-tests/glfw-install:
git clone --depth 1 -b 3.4 https://github.com/glfw/glfw.git build/rmlui-tests/glfw
cmake -S build/rmlui-tests/glfw -B build/rmlui-tests/glfw/build -DGLFW_BUILD_WAYLAND=OFF \
      -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF \
      -DCMAKE_INSTALL_PREFIX=build/rmlui-tests/glfw-install
cmake --build build/rmlui-tests/glfw/build -j3 && cmake --install build/rmlui-tests/glfw/build
# 2. Source tree = fork (engine/libs/rmlui) + upstream 6.3 parts the engine does not vendor
#    (Samples/shell, Samples/assets fonts, Backends, CMake/), assembled in build/rmlui-tests/src
# 3. Configure and build:
cmake -S build/rmlui-tests/src -B build/rmlui-tests/build -DBUILD_TESTING=ON \
      -DBUILD_SHARED_LIBS=OFF -DRMLUI_BACKEND=GLFW_GL3 -DRMLUI_PRECOMPILED_HEADERS=OFF \
      -DRMLUI_MATH_EXPRESSIONS=ON -DCMAKE_BUILD_TYPE=Release \
      -Dglfw3_ROOT=$PWD/build/rmlui-tests/glfw-install
cmake --build build/rmlui-tests/build -j3 --target rmlui_unit_tests
# 4. Run (no display needed: the tests use a dummy render interface unless RMLUI_TESTS_USE_SHELL=1):
build/rmlui-tests/build/rmlui_unit_tests
```
Baseline at 6.3: `146 test cases, 8306 assertions, all passed, 0.8 s`. With PR #983: 182 cases,
10611 assertions. The script takes the upstream parts from a pinned clone of tag `6.3` (cached;
`git clone --depth 1 -b 6.3 https://github.com/mikke89/RmlUi.git`) unless phase 0 vendored them.
It also builds and runs `rmlui_benchmarks` with `--benchmark` subsets for style computation and
layout when `RUN_BENCHMARKS=1` (used by p3-base, p3-cascade and p3-grid to report before/after
numbers in FORK.md; a regression > 10 % on existing benchmarks needs a justification). RmlUi's
`VisualTests` are not run (they need a window and compare against stored captures); our browser
goldens replace them.

### 5.2 Browser reference goldens

**Tooling** (verified on this host): Node 20.20.2; `playwright-core` 1.63.0 from npm (registry
reachable); Chrome for Testing headless shell 153.0.8010.12 already cached at
`~/.cache/ms-playwright/chromium_headless_shell-1243/chrome-headless-shell-linux64/` and
`/usr/bin/google-chrome` 150.0.7871.181. A manual check rendered a grid + dashed elliptical-radius
fixture with a local TTF loaded by `@font-face` correctly with:
`google-chrome --headless=new --disable-gpu --hide-scrollbars --force-color-profile=srgb
--font-render-hinting=none --force-device-scale-factor=1 --allow-file-access-from-files
--window-size=320,240 --screenshot=out.png file:///…/t.html`.
The agent-browser MCP tool is not used for goldens (shared browser, non-deterministic size/flags).

- `tools/htmlui/reference/package.json` pins `playwright-core@1.63.0`; `render.mjs` launches the
  headless shell given by `HTMLUI_CHROME` (default: the playwright-managed headless shell; `regen.sh`
  installs it with `npx playwright-core install chromium-headless-shell` if missing) with
  `--force-color-profile=srgb --font-render-hinting=none --disable-gpu --disable-lcd-text
  --disable-font-subpixel-positioning`, `deviceScaleFactor: 1`, viewport from the sidecar.
- Fixture = `name.html` (+ optional `name.css`, assets) + `name.json` sidecar:
  `{ "viewport": [w, h], "steps": [ {"hover": "#id"} | {"focus": "#id", "keyboard": true} |
  {"addClass": ["#id","c"]} | {"setStyle": ["#id","width","50px"]} | {"setAttr": [...]} |
  {"scroll": ["#id", x, y]} | {"advance": seconds} | {"capture": "suffix"} ], "boxes": true,
  "tolerance": {...optional overrides} }`. The same steps are executed by the engine-side
  `FixtureRunner` through the UIDocument/Lua element API (hover/focus through injected input
  events), and `advance` drives the document clock deterministically (the test SystemInterface's
  `GetElapsedTime`).
- Browser side for `advance`: all animations are paused at load (`document.getAnimations()` →
  `pause()`, then `currentTime` = accumulated advance for each capture), CSS transitions are
  started by the step and seeked the same way, so captures are frame-exact.
- Outputs (committed): `engine/tests/htmlui/golden/browser/<area>/<name>[.<capture>].png`,
  `<name>.boxes.json` (border-box rect of every element in DOM order that has an `id`;
  browsers expose no DOM geometry for `::before`/`::after`, so pseudo-element boxes are checked by
  pixels, and by unit tests on the engine side), `golden/browser/VERSION` (Chrome + playwright
  versions). References are regenerated only by `regen.sh <area|fixture>`; changed images are
  inspected (BRIEF.md).
- Fonts: every fixture uses `@font-face` with files in `engine/tests/htmlui/fonts/`:
  `Roboto-Regular-subset.ttf` extracted from `engine/core/util/DefaultFont.h` (same bytes the
  engine uses) and `Ahem.ttf` (CC0, Kozea/Ahem; also converted from the woff2 embedded in Taffy's
  `test_base_style.css`). Generic families are avoided in fixtures.
- Comparison (`ImageCompare`): images are compared in premultiplied sRGB; a pixel is bad when its
  max channel delta exceeds 24 **and** no pixel in the 3×3 neighbourhood of the other image is
  within 24 (anti-aliasing and rasterizer differences); a fixture passes when bad pixels ≤ 0.1 % of
  the image and no 4-connected bad cluster exceeds 16 pixels. Text-heavy fixtures additionally
  carry `"boxes": true` (the box comparison below is the primary oracle; pixels use the same
  metric). Per-fixture overrides are allowed only with a comment in the sidecar explaining the
  rasterization difference.
- Box comparison: for each id'ed element, engine border box (`Element::GetAbsoluteOffset(Border)`
  + `GetBox().GetSize(Border)`) vs Chrome `getBoundingClientRect()` (untransformed elements),
  |Δ| ≤ 1 px on x, y, width, height.
- Engine side runs under Xvfb in the docker build image (as phase 0), GL core backend; p3-paint
  also runs the paint fixtures with `GRAPHIC_BACKEND=vulkan` (BRIEF.md).

### 5.3 Taffy layout fixtures
`tools/htmlui/import-taffy-fixtures.py` copies (MIT, attribution file) the fixture HTML of Taffy
`8409f3f8` suites `grid` (565), `flex` (686), `block` (264), `blockflex` (12), `blockgrid` (14),
`gridflex` (7), `leaf` (15), `float` (32) into `engine/tests/htmlui/fixtures/taffy/`, replacing the
`<script>`/`<link>` with an inline copy of `test_base_style.css` adapted to local `Ahem.ttf` and a
15 px scrollbar rule (Chrome `::-webkit-scrollbar` in the reference, RmlUi scrollbar element width
in the engine UA addition used only by these fixtures). `render.mjs --boxes-only` dumps the
border-box rect of `#test-root` and every descendant element (DOM order) relative to `#test-root`;
the engine `BoxParityTest` (googletest, parameterized over the manifest) loads each fixture through
the compat layer at a 1024×768 viewport and compares rects within 1 px.
Excluded (`excluded.txt`, with reason): fixtures whose name starts with `x` (Taffy's known-invalid),
fixtures using `direction: rtl` (24 grid, 17 flex, 10 block) or `writing-mode` (2 grid) — §2.2.
Every other fixture must pass at the end of phase 3.

### 5.4 Compat consistency tests
`engine/tests/htmlui/compat/NativeFeatureConsistencyTest.cpp` (p3-testkit creates, every package
adds rows): for each property-table row marked `native`, parse the row's sample declaration with
the fork (`StyleSheetSpecification::ParsePropertyDeclaration`) and check (a) success, (b) the
compat output for the sample passes the declaration through unchanged with the flag off, (c) no
diagnostic is emitted. For each flipped flag, a test that turning the flag back on still produces
the old rewrite (the code is kept, §1).

### 5.5 Other suites
- Engine golden regression (phase-0 harness) and the existing `jasperengine-htmlui-tests`.
- MCP: `tools/mcp/run-tests.sh tools/mcp/tests/test_protocol.py`, `test_ui_anchors.py` and the
  phase-1/2 `test_html_ui*.py` files (BRIEF.md).
- Builds: Linux (GL core + Vulkan for p3-paint/p3-base), Web (`tools/build.sh web`; WebGL2 layers
  for group opacity), Android build (`tools/build.sh android`) for every package touching the fork.

## 6. Acceptance criteria

Phase 3 is accepted when all boxes are checked on the merged `html-ui` branch:

**Fork hygiene**
- [ ] Every phase-3 fork change is in `rmlui:` commits; FORK.md lists each patch (commits, files,
      reason, upstream status, tests), PR #983 provenance and the Taffy commit + license.
- [ ] `tools/htmlui/run-rmlui-tests.sh` passes: all 6.3 cases (expectations changed only where FORK.md
      documents an initial-value change), all PR #983 cases, all phase-3 cases.
- [ ] Benchmarks recorded before/after for style computation, `:has`-free documents and layout;
      no unexplained regression > 10 %.
- [ ] Zero new compiler warnings in the fork and engine on Linux GCC; Web and Android builds pass.

**Features (each with unit tests and browser goldens passing)**
- [ ] `calc()/min()/max()/clamp()` with px/%/em/rem/vw/vh/vmin/vmax/dp/ch/ex, nested, in every
      length/percentage property including radii, grid tracks, background positions, shadows, transforms.
- [ ] `border: 1px solid red` and all shorthands in any order; all ten border styles render like
      Chrome incl. rounded corners; `%` and elliptical radii with overlap scaling; clip masks and
      box-shadow follow the same radii; `outline` + `outline-offset`.
- [ ] `::before`/`::after` with string/`attr()`/`url()`/counter content, styled, laid out, hoverable
      through the originating element, transitioned; `::marker` and list styles.
- [ ] `:root` (= `<html>`), `:is()`, `:where()` (zero specificity), `:has()` (child, descendant,
      sibling forms, invalidation bounded), `:focus` only on the focused element, `:focus-within`,
      `:focus-visible` keyboard-only, `:enabled/:disabled/:checked` parity, attribute flags.
- [ ] `!important` in stylesheets and inline styles with CSS precedence vs animations/transitions;
      `inherit/initial/unset/revert/revert-layer` for every property; `all`.
- [ ] `position: fixed` relative to the viewport, unaffected by ancestor scroll/overflow, contained by
      transformed ancestors; `position: sticky`.
- [ ] Group opacity: overlapping children of a 0.5-opacity parent composite like Chrome; leaf fast
      path; `opacity: 0` hit-testable; stacking contexts per CSS.
- [ ] Easing keywords, `cubic-bezier()`, `steps()`, `linear()`, `ms`/`s`/`0s`, longhands, transitions
      on inline-style/script/var()/inherited changes, `allow-discrete`, fill modes, direction,
      fractional iteration counts, real pause, per-keyframe easing, `@starting-style`, individual
      transform properties.
- [ ] CSS Grid: every property/shorthand of §3.14.1; all non-excluded Taffy `grid`, `blockgrid`,
      `gridflex` fixtures pass; abs-pos grid areas; grid as flex item and inline-grid shrink-to-fit.
- [ ] §2.3 gaps: each "Fork" row implemented and covered by a golden; `flex`, `block`, `blockflex`,
      `leaf`, `float` Taffy suites pass except `excluded.txt`.
- [ ] Values: `rgba(…, 0.5)` and space syntax native, `currentcolor`, all named colors,
      `color-mix()`, font-family lists resolve fallbacks, `line-height: normal`, `rem` relative to
      `<html>`, `html{font-size:62.5%}` pattern correct.

**Compat layer**
- [ ] Every flag in Appendix A is off by default; the property table marks those features `native`;
      §5.4 consistency tests pass; turning a flag on restores the rewrite (tested).
- [ ] Every §2.2 item produces a diagnostic with `file:line:column` and the listed hint.
- [ ] `known-failures.txt` is empty; `excluded.txt` contains only §2.2 reasons.

**Integration**
- [ ] The five LLM-style sample pages of `p3-integration` match Chrome within the §5.2 metric.
- [ ] MCP suites of §5.5 pass; screenshots inspected.
- [ ] `docs/mcp/guide/html-ui.md` describes the newly native features and the §2.2 limits.

---

## Appendix A — compat flags switched off in phase 3

Expected phase-2 flag keys (use phase 2's actual names where they differ). "Rewrite before" is the
phase-2 behavior being replaced.

| Flag (expected key) | Rewrite before (phase 2) | After | Package |
|---|---|---|---|
| `root-merge` | `html`/`:root` selectors merged onto `body` | pass-through; `<html>` emitted | p3-base |
| `calc` | constant-folding of calc / diagnose | native | p3-values (p3-base lands PR) |
| `rgba-alpha` | alpha 0–1 scaled to 0–255 | native | p3-values |
| `color-space-syntax` | `rgb(r g b / a)` → comma form | native | p3-values |
| `named-colors` | CSS names → hex | native | p3-values |
| `currentcolor` | substituted with resolved `color` | native | p3-values |
| `font-family-list` | list → first family / engine-specific form | native canonical list | p3-values |
| `font-shorthand` | `font` expanded | native | p3-values |
| `units-vmin-vmax`, `units-ch-ex` | converted/diagnosed | native | p3-values |
| `line-height-normal` | `normal` → `1.2` | native | p3-values |
| `transform-origin-keywords` | keyword reordering | native | p3-values |
| `logical-properties` | mapped to physical | native shorthands | p3-values |
| `cursor-list` | first keyword picked | native | p3-values |
| `important` | `!important` stripped (diagnosed) | native | p3-cascade |
| `css-wide-keywords` | dropped / resolved statically | native | p3-cascade |
| `all-shorthand` | expanded | native | p3-cascade |
| `root-selector` | `:root` → `body` | native | p3-cascade |
| `is-where` | `:is()`/`:where()` expanded into lists (specificity lost) | native | p3-cascade |
| `has` | diagnosed, rule dropped | native | p3-cascade |
| `focus-within`, `focus-visible` | emulation / diagnosed | native | p3-cascade |
| `enabled`, `attr-flags`, `nth-of` | `:not([disabled])` rewrite / diagnosed | native | p3-cascade |
| `pseudo-before-after`, `content`, `counters`, `list-markers` | diagnosed / dropped | native | p3-cascade |
| `border-shorthand`, `border-style` | split into width + color, style dropped | native | p3-paint |
| `border-radius-percent`, `border-radius-elliptical` | `50%` → large px, `/` dropped | native | p3-paint |
| `outline` | diagnosed or mapped to box-shadow | native | p3-paint |
| `opacity-group` | none (diagnosed) | native | p3-paint |
| `background-shorthand`, `background-gradient`, `background-image-url` | → `decorator:` | native | p3-paint |
| `mask-image-css` | → decorator syntax | native | p3-paint |
| `object-fit` | diagnosed | native | p3-paint |
| `visibility-inherit`, `z-index-static` | diagnosed | native | p3-paint |
| `text-shadow` | → `font-effect: shadow()` | native | p3-text |
| `letter-spacing-float`, `word-spacing` | rounded / diagnosed | native | p3-text |
| `text-transform-unicode`, `white-space-break-spaces`, `overflow-wrap`, `text-decoration-longhands`, `text-align-start-end`, `user-select` | mapped / diagnosed | native | p3-text |
| `position-fixed`, `position-sticky`, `overflow-clip` | diagnosed (fixed acted as absolute) | native | p3-layout |
| `flex-order`, `flex-basis-content`, `min-size-auto`, `sizing-keywords`, `aspect-ratio`, `margin-collapse`, `display-contents`, `alignment-keywords` | diagnosed / approximated | native | p3-layout |
| `easing-functions`, `time-ms` | `ease` → `cubic-in-out`, `ms` → `s` | native | p3-animation |
| `transition-longhands`, `animation-longhands` | combined into shorthands | native | p3-animation |
| `transition-any-change`, `fill-mode`, `play-state`, `transition-behavior`, `individual-transforms`, `starting-style` | diagnosed | native | p3-animation |
| `display-grid`, `grid-template`, `grid-placement`, `grid-areas`, `place-shorthands`, `justify-items-self` | diagnosed (grid unsupported) | native | p3-grid |

## Appendix B — new properties (registered in `ID.h` by p3-base, implemented by the package)

| Package | Longhands | Shorthands |
|---|---|---|
| p3-base | `order`, `justify-items`, `justify-self`, `will-change` | `place-items`, `place-self`, `place-content` |
| p3-values | — (new units/values only) | `margin-inline/-block(-start/-end)`, `padding-inline/-block(*)`, `inset-inline/-block(*)`, `border-inline/-block(*)`, `inline-size`, `block-size`, `min-/max-inline-size`, `min-/max-block-size`; `font` (now Custom), `transform-origin`/`perspective-origin` (Custom) |
| p3-cascade | `content`, `quotes`, `counter-reset`, `counter-increment`, `counter-set`, `list-style-type`, `list-style-position`, `list-style-image` | `all`, `list-style` |
| p3-paint | `border-{top,right,bottom,left}-style`, `-rmlui-border-{tl,tr,br,bl}-radius-{x,y}` (8, replacing the 4 radius ids), `outline-width`, `outline-style`, `outline-color`, `outline-offset`, `isolation`, `background-image`, `background-position-x`, `background-position-y`, `background-size`, `background-repeat`, `background-origin`, `background-clip`, `background-attachment`, `mask-size`, `mask-position-x`, `mask-position-y`, `mask-repeat`, `object-fit`, `object-position-x`, `object-position-y`, `border-image` (none only) | `border-style`, `border-*` (AnyOrder), `border` (AnyOrder), `border-*-radius` (4), `border-radius` (Custom), `outline`, `background` (Custom), `background-position`, `mask`, `mask-position`, `object-position` |
| p3-text | `word-spacing`, `overflow-wrap`, `white-space-collapse`, `text-wrap-mode`, `text-decoration-color`, `text-decoration-style`, `text-decoration-thickness`, `text-underline-offset`, `text-shadow`, `user-select`, `accent-color` (`text-decoration` becomes `text-decoration-line`) | `white-space` (Custom), `text-wrap`, `text-decoration` (AnyOrder), `word-wrap` (alias) |
| p3-layout | `overflow-clip-margin`, `aspect-ratio`, `scrollbar-gutter` | — |
| p3-animation | `transition-property`, `transition-duration`, `transition-timing-function`, `transition-delay`, `transition-behavior`, `animation-name`, `animation-duration`, `animation-timing-function`, `animation-delay`, `animation-iteration-count`, `animation-direction`, `animation-fill-mode`, `animation-play-state`, `animation-composition`, `translate`, `rotate`, `scale` | `transition`, `animation` (both Custom, replacing the 6.3 list properties) |
| p3-grid | `grid-template-columns`, `grid-template-rows`, `grid-template-areas`, `grid-auto-columns`, `grid-auto-rows`, `grid-auto-flow`, `grid-row-start`, `grid-row-end`, `grid-column-start`, `grid-column-end` | `grid-row`, `grid-column`, `grid-area`, `grid-template`, `grid`, `grid-gap`/`grid-row-gap`/`grid-column-gap` (aliases) |

Total new longhands ≈ 75 (6.3 has 101 of 128 ids), hence `PropertyId : uint16_t`, `MaxNumIds = 256`.
