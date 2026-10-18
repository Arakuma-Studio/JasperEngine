# HTML UI tools

Host-side tools of the HTML/CSS UI (`docs/html-ui/`). The engine's own tests run in Docker
(`tools/build.sh htmlui-tests`); the tools here run on the developer host.

| Tool | What |
|---|---|
| `render-reference.mjs` (+ `package.json`, `npm ci`) | Chromium references: phase-2 golden pages (`--all`, `--page`), phase-3 fixtures (`--fixtures`, `--boxes-only`, `--self-test`) |
| `run-rmlui-tests.sh` (+ `rmlui-tests/`) | RmlUi's own unit tests and benchmarks, built standalone on the fork |
| `import-taffy-fixtures.py` | Taffy's layout fixtures, their exclusions and the known-failures baseline |
| `fixture_server.py` | the importers' test server (phase-2 spec 5.4) |
| `build_lucide_pack.py`, `gen_lucide_replace_fixtures.mjs` | the built-in Lucide icon pack |
| `xcode_sync_compat.py`, `xcode_add_lexbor.py` | Xcode project upkeep |

## Phase 3: fork patches against the browser

Phase 3 (`docs/html-ui/phase-3-fork-patches.md` §5) proves each fork patch three ways: RmlUi's
own unit tests, browser-parity fixtures against Chromium, and Taffy's layout fixtures.

### RmlUi's unit tests (spec §5.1)

```
tools/htmlui/run-rmlui-tests.sh                     # host: FreeType from pkg-config (libfreetype-dev)
tools/htmlui/run-rmlui-tests.sh --docker            # the engine's Linux image; FreeType built from source
tools/htmlui/run-rmlui-tests.sh -- --test-case='*Calc*'                  # doctest arguments after --
RUN_BENCHMARKS=1 BENCHMARK_ARGS='--test-case=*Selector*' tools/htmlui/run-rmlui-tests.sh
JOBS=4 RMLUI_FORK_DIR=build/scratch/rmlui tools/htmlui/run-rmlui-tests.sh   # another tree (bisecting)
```

The script assembles `build/rmlui-tests/<host|docker>/src` from the fork (`engine/libs/rmlui`:
`Include`, `Source/Core`, `Source/Debugger`, `Tests`) and the upstream 6.3 parts the engine does not
vendor (FreeType font engine, `CMake/`, sample shell and fonts; from a pinned clone in
`build/rmlui-tests/upstream-6.3`, or `RMLUI_UPSTREAM_DIR=<checkout of tag 6.3>`), builds it with
`rmlui-tests/CMakeLists.txt` (it globs the fork's sources: new fork files need no list) and a
window-less backend, and runs the tests from the source tree. The first run clones (network); later
runs build incrementally. Baseline on the fork: 146 test cases, 8306 assertions, all passing. New
tests go into `engine/libs/rmlui/Tests/Source/UnitTests/` and its `CMakeLists.txt` (upstream's
list), in `rmlui:` commits. `Tests/Source/Common/TestsShell.h` has `GetElementChild`,
`GetNumElementChildren`, `GetNextElementSibling`: the fork keeps white-space text (FORK.md P2-8).

### Browser-parity fixtures (spec §5.2)

A fixture is `engine/tests/htmlui/fixtures/p3/<area>/<name>.html` with a `<name>.json` sidecar; the
areas and their owners are in `fixtures/p3/manifest.json` (`base values cascade paint text layout
animation grid integration`, and `testkit`, the harness's self-tests). Fonts come from
`engine/tests/htmlui/fonts/` with `@font-face` (`url("../../../fonts/Roboto-Regular-subset-post.ttf")`,
the engine's built-in face with the `post` table Chromium requires, or `Ahem.ttf`); avoid generic
families. The reference fails when a face does not load (also one a later step uses), and
`BrowserFixtures.FontFacesLoadInChromium` checks every local font a fixture names. JavaScript does
not run.

```json
{
 "comment": "what the fixture shows (required with a tolerance override: why)",
 "viewport": [400, 300],
 "steps": [
  {"hover": "#id"},
  {"focus": "#id", "keyboard": true},     // Tab presses until #id has the focus (:focus-visible); without keyboard: a click
  {"press": "#id"}, {"release": true},    // pointer down at the centre / up (or {"release": "#id"}: move there first)
  {"addClass": ["#id", "c"]}, {"removeClass": ["#id", "c"]},
  {"setStyle": ["#id", "width", "50px"]},  // "" removes the property
  {"setAttr": ["#id", "data-x", "1"]},     // null removes the attribute
  {"scroll": ["#id", 0, 40]},
  {"advance": 0.25},                      // seconds of the UI clock: animations and transitions are seeked
  {"seekVideo": ["#v", 1.4]},             // a <video>'s currentTime: the reference waits until the seek is done and the frame is there (the video area runs with scripting on: without it Chromium shows native controls)
  {"capture": "hover"}                    // golden/browser/<area>/<name>.hover.png; without a capture step, one at the end (<name>.png)
 ],
 "boxes": true,                           // text-heavy: the boxes are the oracle, the full image is reported only
 "noText": true,                          // required with "boxes": every capture is also taken without text and gated
                                          // strictly against <name>[.<capture>].notext.png (see below)
 "tolerance": {"channel": 24, "badPercent": 0.1, "cluster": 16, "box": 1},
 "samples": 1                             // the engine renders without MSAA (default 4x); needs a comment. For content scrolled
                                          // past a clip edge: Mesa's llvmpipe/lavapipe leak half the samples there (MsaaScissor.*)
}
```

(Comments are not JSON: the block above annotates the keys.) Targets are `#id`. Every capture
compares the image with the strict metric (`BrowserCompare::compareStrict`: a pixel is bad when it
is off by more than 24 and no pixel of its 3x3 neighbourhood in the other image is within 24; at most
0.1 % bad pixels and no 4-connected bad cluster above 16 pixels) and the border box of every element
with an id within 1 px.

`"noText": true` (required with `"boxes": true`, `BrowserFixtures.ManifestAndReferencesAreConsistent`)
keeps a text-heavy fixture as strict as the phase-2 golden pages' no-text gate: the reference runs the
steps a second time in a fresh browser with page mode's `NO_TEXT_CSS` (transparent text fill, no text
shadows, decorations or caret) into `<name>[.<capture>].notext.png`; the engine takes every capture a
second time with the same frame drawn without glyphs, text decorations or symbol markers
(`RmlFontEngine::setRenderTextForTesting(false)`, as `BrowserPage::render(image, false)`), and
`BrowserParityTest` gates that image at the sidecar's tolerances whatever `"boxes"` says. Only the
glyphs stay report-only. Both sides hide the text caret in every capture (the reference's screenshot
option; the engine sets `caret-color: transparent` on the focused field while it reads the images).

```
cd tools/htmlui && npm ci                                          # once (playwright-core 1.63.0)
node render-reference.mjs --fixtures paint                         # every fixture of an area
node render-reference.mjs --fixtures paint/border-dashed --verify  # one fixture, rendered twice and compared
node render-reference.mjs --fixtures p3 --check                    # all p3 fixtures against the committed references
node render-reference.mjs --self-test                              # reproducibility: testkit twice, byte-identical
HTMLUI_TEST_ARGS='--gtest_filter=P3/BrowserParity.*paint*' JOBS=3 tools/build.sh htmlui-tests
HTMLUI_BACKEND=vulkan HTMLUI_TEST_ARGS='--gtest_filter=P3/BrowserParity.*' JOBS=3 tools/build.sh htmlui-tests
```

References land in `engine/tests/htmlui/golden/browser/<area>/` (`<name>[.<capture>].png`,
`<name>.boxes.json`, and `VERSION`); look at every image you commit. The engine's images, heat maps
(`.diff.png`), metrics and the compat output (`.rml`, `.rcss`, `.diag.txt`) of a run are in
`build/linux/htmlui-tests-<backend>/tests/htmlui/htmlui-test-output/browser/<area>/`.

### Taffy layout fixtures (spec §5.3)

`engine/tests/htmlui/fixtures/taffy/<suite>/` holds Taffy's fixtures (commit 8409f3f8, suites grid,
flex, block, blockflex, blockgrid, gridflex, leaf, float); `BoxParityTest` loads each at 1024x768
and compares the boxes of `#test-root` and every element under it with Chromium's within 1 px.

```
tools/htmlui/import-taffy-fixtures.py import --taffy /tmp/htmlui-src/taffy     # (re)import; writes excluded.txt
node tools/htmlui/render-reference.mjs --fixtures taffy --boxes-only --verify  # golden/browser/taffy/<suite>.boxes.json
node tools/htmlui/render-reference.mjs --fixtures taffy/flex/flex_basis_content_row --boxes-only   # one fixture's entry
HTMLUI_TEST_ARGS='--gtest_filter=Taffy/BoxParity.*:BoxParity*' JOBS=3 tools/build.sh htmlui-tests
HTMLUI_TEST_ARGS='--gtest_filter=Taffy/BoxParity.*flex_*' JOBS=3 tools/build.sh htmlui-tests
tools/htmlui/import-taffy-fixtures.py classify flex/flex_basis_content_row            # owner and reason class of a fixture
tools/htmlui/import-taffy-fixtures.py known-failures \
    build/linux/htmlui-tests-glcore/tests/htmlui/htmlui-test-output/taffy/results.txt --merge
```

### Known failures and exclusions

`fixtures/taffy/known-failures.txt` and `fixtures/p3/known-failures.txt` list, one per line, the
fixtures the fork does not pass yet: `<id> owner=<package> reason=<class> # <first difference>`. A
listed fixture is expected to fail (the run prints it as `[ KNOWN    ]`); when it passes the test
fails until its line is removed. So a wave-1 package:

1. runs the fixtures of its features (`--gtest_filter`, the run's `results.txt` lists pass/fail);
2. removes the lines of the fixtures it fixed (`grep -v` its passing ids; never edit others' lines
   except as in 3);
3. when a fixture of its own still fails for a cause another package owns, changes that line's
   `owner=` and `reason=` to the other package (the next cause), and says so in its report;
4. never adds a line for a fixture of its own area that fails for its own feature: that is a bug
   to fix. It adds a line (owner = the other package) only for a fixture that needs another
   package's feature.

The baseline (`known-failures` command) classifies each failure by the features of the element its
first difference names (`classify`): a guide, not a verdict. `excluded.txt` holds the fixtures that
are not compared (Taffy's invalid `x*` ones, `direction: rtl` and `writing-mode` per spec §2.2, and
two that build their styles with JavaScript); it changes only with the spec.
