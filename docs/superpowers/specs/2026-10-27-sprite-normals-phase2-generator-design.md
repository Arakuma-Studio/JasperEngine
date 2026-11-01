# Sprite Normals Phase 2: the generator (core, media, recipe, preview, session, apply)

Status: binding for wave 1 implementers 2A and 2B. Conforms to
`2026-10-27-sprite-normals-contract.md` (names, settings, presets, files, recipe, engine conventions). Where this
spec adds detail the contract does not have, this spec rules; where they disagree, the contract rules and the
controller amends this file.

Phase 2 produces everything Phases 3-5 call: a deterministic, engine-free generator (a C++ port of
spritefusion-sprite-normals plus the advanced options of the contract), media decoding (PNG, JPEG/TGA/BMP, GIF,
APNG, sheets), the recipe chunk, a CPU lit preview and contact sheet, metrics and plain-language assessment, the
editor-side session (paths, outputs, undoable writes, staleness, recipes back to specs, a worker), and the one
Scene-undoable command that wires a result onto a Sprite. It adds no window, no MCP tool, no CLI command and no
check (Phases 3 and 4).

## 1. Module layout and ownership

All files live in `editor/spritenormals/`, namespace `jasperengine::editor::spritenormals`. "Core" files depend only
on the C++ standard library, `texsynth/SynthImage.h`, `texsynth/SynthesisRecipe.h` (2B's recipe only), `json.hpp`
and stb; they build into the standalone `jasperengine-spritenormals-tests`. "Editor" files may use the engine,
Project and command history.

| File | Kind | Owner | Purpose |
|---|---|---|---|
| `SpriteNormals.h/.cpp` | core | 2A | Settings, presets, validation, cells and grid slicing, `generate()` |
| `SpriteNormalsField.h/.cpp` | core (internal, `detail` namespace, exposed for tests) | 2A | Field, EDT, Gaussian, masked Gaussian, Sobel, components, Oklab |
| `SpriteMedia.h/.cpp` | core | 2A | Source loading (stills, GIF, APNG), probing, atlas packing, PNG encoding, limits |
| `SpriteNormalsRecipe.h/.cpp` | core | 2B | Recipe JSON, settings <-> JSON, PNG chunk read/write (keyword `jasper.spritenormals`) |
| `SpriteNormalsPreview.h/.cpp` | core | 2B | Lit render, contact sheet, metrics, assessment |
| `SpriteNormalsSession.h/.cpp` | editor | 2B | Spec, paths, frames resolution, planned outputs, prepare, encode, undoable write, staleness, specFromRecipe, scene warnings, worker run |
| `SpriteNormalsApply.h/.cpp` | editor | 2B | `applyToSprite()`: one Scene undo step |
| `LICENSE-spritefusion-sprite-normals.txt` | licence | 2A | Upstream MIT text, verbatim, with a header line naming the commit (`d14b067`) |
| `tests/SpriteNormalsTests.cmake` | build | 2A | The core test target (globs `tests/*Test.cpp`) |
| `tests/SpriteNormalsCoreTest.cpp`, `tests/SpriteNormalsMediaTest.cpp`, `tests/fixtures/**` | test | 2A | Core, parity, media tests and fixtures |
| `tests/SpriteNormalsRecipeTest.cpp`, `tests/SpriteNormalsPreviewTest.cpp` | test | 2B | Recipe and preview tests (picked up by the glob) |
| `editor/tests/SpriteNormalsSessionTest.cpp`, `editor/tests/SpriteNormalsApplyTest.cpp` | editor test | 2B | Session and apply tests (EditorTests.cmake glob) |
| `editor/texsynth/SynthesisRecipe.h/.cpp` | existing | 2B | Keyword parameter on the chunk functions (section 6.1) |
| `NOTICE.md` | existing | 2A | One third-party row (section 9) |
| `CMakeLists.txt`, `tools/build.sh` | existing | 2A | All Phase 2 wiring, including 2B's files (section 10) |

**Task 0 (skeleton), 2A's first commit, made before any other work and pushed to its branch immediately:** every
header of section 2-8 verbatim from this spec; for 2B's four `.cpp` files a stub containing only the license header,
the include of its own header and an empty namespace block; `SpriteNormalsTests.cmake`; the CMake and build.sh
wiring of section 10; the licence file and NOTICE row. Commit message: `Sprite normals: skeleton (headers, build
wiring)`. 2B starts by `git merge`-ing that commit from 2A's branch (`git log --all --grep 'Sprite normals:
skeleton'`). After Task 0, 2A never touches 2B's files and 2B never touches 2A's; a header change either needs goes
through the controller (a ledger ruling), never a silent edit.

## 2. Core header: `SpriteNormals.h` (2A, verbatim)

```cpp
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Sprite normal maps (editor-only, never shipped in games): a C++ port of Hugo Duprez's
// spritefusion-sprite-normals (MIT, NOTICE.md, LICENSE-spritefusion-sprite-normals.txt) plus
// JasperEngine's options (profiles, edge modes, auto radius, outline and colour regions, height map).
//
// Per frame cell, per 8-connected island of pixels above the alpha cutoff: a height field is the
// silhouette's distance to the edge through a profile (volume) plus the Oklab lightness of the
// painted shading, smoothed at two scales (detail); a normalized Sobel gives slopes, scaled by
// strength, encoded OpenGL style (green up the image) unless directx. Pixels at or below the
// alpha cutoff get (128,128,255) with their own alpha; alpha is always the source's byte.
//
// Deterministic: identical inputs and settings give identical bytes whatever the thread count.
// With profile Rounded, edges Transparent, an explicit radius, no outline handling and no
// regions, the output matches the upstream tool within 1 per channel (tests/fixtures).
//
// Usage:
//   spritenormals::Request request;
//   request.color = rgba8Image;                       // texsynth::Image, 4 channels, 8-bit
//   request.settings = spritenormals::presetSettings(spritenormals::Preset::Default);
//   spritenormals::Result result = spritenormals::generate(request);   // blocking
//   if (result.status == spritenormals::Status::Ok) { result.normal ... }

#include "texsynth/SynthImage.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    using Image = texsynth::Image;

    // ---- settings (contract section 3: names, ranges, defaults)

    enum class Preset { Soft, Default, Crisp, Detailed, Chunky };
    enum class Profile { Rounded, Dome, Bevel };
    enum class Edges { Transparent, Wrap, Extend };

    // A colour region: pixels whose Oklab colour is within `tolerance` of one of `colors`
    struct Region {
        std::vector<uint32_t> colors;   // 0xRRGGBB, 1..16
        float tolerance = 0.03f;        // Oklab Euclidean distance, 0..0.5
        float detail = 1.0f;            // 0..8: multiplier of the detail term (0: the colour is ignored by it)
        float lift = 0.0f;              // -16..16: pixels of height added (before strength)
        bool flat = false;              // the pixels' normals are forced to +Z
    };

    struct Settings {
        Preset preset = Preset::Default;      // what the six main values started from (recorded, never re-applied)
        float strength = 1.5f;                // 0..8
        float detail = 3.0f;                  // 0..8
        float smoothing = 1.0f;               // 0..8 px
        float volume = 1.15f;                 // 0..8
        std::optional<float> radius;          // nullopt: auto; else 0.5..512 px
        float softness = 1.15f;               // 0..8 px
        float alphaThreshold = 0.03f;         // 0..0.99
        Profile profile = Profile::Rounded;
        Edges edges = Edges::Transparent;
        bool ignoreOutline = false;
        std::vector<Region> regions;          // at most 16
        bool directx = false;
        bool height = false;                  // also produce Result::height
    };

    // The preset's values (contract section 3 table); softness 1.15, radius auto, alpha 0.03,
    // Rounded, Transparent, no outline handling, no regions, OpenGL, no height map.
    // Resolving a preset with overrides (every surface does exactly this):
    //   Settings s = presetSettings(p); settingsFromJson(overrides, s, error); s.preset = p; validateSettings(s)
    Settings presetSettings(Preset preset);
    // "soft", "default", "crisp", "detailed", "chunky" (and back; false for an unknown name)
    const char* toString(Preset preset);
    const char* toString(Profile profile);    // "rounded", "dome", "bevel"
    const char* toString(Edges edges);        // "transparent", "wrap", "extend"
    bool parsePreset(const std::string& name, Preset& out);
    bool parseProfile(const std::string& name, Profile& out);
    bool parseEdges(const std::string& name, Edges& out);
    // "#rrggbb" or "rrggbb" (case-insensitive) -> 0xRRGGBB; "#rrggbb" back (lower case)
    bool parseColor(const std::string& text, uint32_t& out);
    std::string colorText(uint32_t rgb);

    // ---- problems (shared kinds: contract + section 11)

    struct Problem {
        std::string kind;      // "invalid_settings", "frames_mismatch", "no_alpha", ...
        std::string field;     // "strength", "regions[1].tolerance", "frames.tile_size", "source", "" ...
        std::string message;   // one sentence naming the value and the limit
        std::string fix;       // what to do instead ("" when there is nothing to do)
    };

    // Every problem of the settings (never only the first): non-finite values, values out of
    // range ("strength must be between 0 and 8 (got 9)"), too many regions or colours
    std::vector<Problem> validateSettings(const Settings& settings);

    // ---- cells

    // A pixel rectangle of the image, top-left origin
    struct Cell {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool operator==(const Cell& o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
    };

    // The Texture Slicer / set_sprite_frames grid: tile size, or columns and rows; offset,
    // padding, count. Same math as the MCP sliceSheet (WorldTools.cpp): tile_size given ->
    // columns/rows = floor((avail + padding) / (tile + padding)); columns and rows given ->
    // tile = floor((avail - padding * (n - 1)) / n); both given: the tile size is used and
    // columns/rows > 0 cap the counts (sliceSheet); cells row-major, the last ones clipped to
    // the image; count (0 = all) caps them.
    struct Grid {
        int tileWidth = 0;
        int tileHeight = 0;
        int columns = 0;
        int rows = 0;
        int count = 0;
        int offsetX = 0;
        int offsetY = 0;
        int paddingX = 0;
        int paddingY = 0;
    };
    // False with `error` ("The grid is empty: the image is 30x30 pixels.") for an empty grid,
    // or more than kMaxCells cells
    bool sliceGrid(const Grid& grid, int imageWidth, int imageHeight, std::vector<Cell>& cells, std::string& error);
    inline constexpr int kMaxCells = 4096;

    // ---- generation

    struct Request {
        Image color;                  // RGBA8 (4 channels, 8-bit): an image, a sheet or an atlas
        std::vector<Cell> cells;      // empty: the whole image is one cell. Inside the image, not
                                      // overlapping (identical duplicates are processed once);
                                      // pixels outside every cell stay neutral
        bool sequence = false;        // the cells are frames of one animation: auto radius is one
                                      // value for all of them (section 3.6)
        Settings settings;
        int threads = 0;              // 0: hardware concurrency (at most 16); never changes the output
    };

    struct Progress {
        double fraction = 0.0;        // 0..1 (work items done / total)
        double elapsedSeconds = 0.0;
    };

    struct Control {
        // Called on a generating thread after work items, at most every progressIntervalSeconds
        std::function<void(const Progress&)> onProgress;
        double progressIntervalSeconds = 0.1;
        // Set from any thread: generate() returns Status::Cancelled after the work items running
        const std::atomic<bool>* cancel = nullptr;
    };

    enum class Status { Ok, InvalidRequest, Cancelled, Failed };
    const char* toString(Status status);   // "ok", "invalid_request", "cancelled", "failed"

    struct Result {
        Status status = Status::InvalidRequest;
        std::vector<Problem> errors;         // InvalidRequest / Failed: why
        std::vector<Problem> warnings;       // outline_not_found, region_unmatched (section 3.8)

        Image normal;                        // RGBA8, the colour image's size
        Image height;                        // settings.height: 1-channel 16-bit (section 3.9); else empty
        float heightMin = 0.0f;              // the height range the 16-bit values span (px, after strength)
        float heightMax = 0.0f;

        // The radius used: one value (explicit, or auto for a sequence), or per island (auto on
        // a still image: radiusMin..radiusMax over its islands)
        bool radiusPerIsland = false;
        float radiusMin = 0.0f;
        float radiusMax = 0.0f;

        std::optional<uint32_t> outlineColor;   // ignoreOutline: the colour detected (0xRRGGBB)
        int islands = 0;                     // processed (Transparent edges); cells otherwise
        int threads = 0;                     // used
        double seconds = 0.0;
    };

    // Validates (settings, image format and size, cells), then generates. Never throws:
    // problems come back with Status::InvalidRequest (kinds invalid_settings, invalid_source,
    // frames_mismatch, too_large), Failed when memory ran out. Blocking; any thread.
    Result generate(const Request& request, const Control& control = Control());

    // Limits (contract section 4)
    inline constexpr int64_t kMaxPixels = 16777216;            // 4096 x 4096, per image or atlas
}
```

## 3. The algorithm (2A)

### 3.1 Parity rules (bit-for-bit with upstream on glibc; within 1 per channel elsewhere)

The upstream sources are at `/tmp/sfsn-src/repo/src` (commit `d14b067`); read `lib.rs`, `relief.rs`, `field.rs`,
`distance.rs`, `components.rs`, `color.rs`, `settings.rs` in full before writing code. The port must:

- Use `float` everywhere except the EDT parabola intersection, which is `double`. Every literal is a `float`
  (`0.7f`, `0.3f`, `0.125f`, `1e-8f`, `0.5f`, `255.0f`, the Oklab coefficients, `0.04045f`, `12.92f`, `0.055f`,
  `1.055f`, `2.4f`).
- Compile `SpriteNormals.cpp` and `SpriteNormalsField.cpp` with `-ffp-contract=off` (GCC/Clang; MSVC defaults are
  fine): Rust never contracts `a*b+c`. Use `std::fma` exactly where upstream calls `mul_add`: the EDT output
  (`fmaf(delta, delta, f[site])`) and the normal length (`sqrtf(fmaf(x, x, fmaf(y, y, 1.0f)))`).
- Keep every evaluation order: Oklab dot products left to right; `L = (0.21045426f*l + 0.7936178f*m) -
  0.004072047f*s`; kernel taps generated and summed from `-r` to `r` (sum starts at `0.0f`), each tap divided by the
  sum; separable passes horizontal then vertical, each output accumulating `out += src * w` (multiply, then add,
  separately rounded) in tap order `-r..r`, taps outside the field skipped; Sobel exactly
  `((((( d[u+1] + 2*d[i+1]) + d[dn+1]) - d[u-1]) - 2*d[i-1]) - d[dn-1]) * 0.125f` and
  `((((( d[dn-1] + 2*d[dn]) + d[dn+1]) - d[u-1]) - 2*d[u]) - d[u+1]) * 0.125f`; detail added as
  `c + ds * ((0.7f * fine) + (0.3f * broad))`; shape as `(S * R) * log1pf(inset / R)`.
- Use `cbrtf`, `expf`, `log1pf`, `powf`, `sqrtf` from `<cmath>` (the float overloads). The sRGB->linear table is 256
  floats computed once (`v = i / 255.0f; v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f)`).
- Encode with `(uint8_t)std::roundf((v * 0.5f + 0.5f) * 255.0f)` (half away from zero), clamped to 0..255.
- Mask: `float(a) / 255.0f > alphaThreshold` (strict).

### 3.2 Pipeline (Transparent edges; Wrap/Extend differ as in 3.4)

1. **Validate**: `validateSettings`; image non-empty, 4 channels 8-bit (`invalid_source` otherwise); width x height
   <= `kMaxPixels` (`too_large`, message names both numbers); cells inside the image, width and height >= 1, at most
   `kMaxCells`, pairwise non-overlapping after removing identical duplicates (`frames_mismatch`, naming the two cell
   indices).
2. **Output** starts as `(128,128,255, a)` for every pixel `a` of the source alpha.
3. **Work items**: for each cell (the whole image when there are none), in row-major cell order: build the mask, label
   8-connected components (upstream BFS order: scan pixels in index order, a new component per unvisited mask pixel,
   breadth-first over the 3x3 neighbourhood in `ny` then `nx` order), and make one work item per island, in label
   order. Each item's island image is its bounding box, holding only that island's pixels (other islands inside the
   box are transparent zeros).
4. **Relief per island** (upstream `relief::generate`), on a field of the island box padded by **1 pixel** of zeros
   on every side. Upstream pads by `ceil(3*max(softness, 2*smoothing)) + 3`; every field is exactly 0 outside the
   island and the blurs use zero extension, so a skipped tap and an added `0.0f` tap give identical sums: pad 1 is
   byte-identical (the test of 12.1 proves it on the fixtures).
   - `opacity[i] = a/255`, `lightness[i] = OklabL(rgb)` for mask pixels, 0 elsewhere; `low`/`high` = min/max
     lightness over the island (initialised 1 and 0); every mask pixel `lightness -= (low + high) * 0.5f`.
   - `dist = edt(mask)` (3.3); shape height per field pixel = `profile(max(dist - 0.5f, 0.0f))` (3.5);
     `combined = gaussian(shape, softness)`.
   - `fine = maskedGaussian(lightness, opacity, smoothing)`, `broad = maskedGaussian(lightness, opacity,
     smoothing * 2.0f)`; `combined += detail * (0.7f*fine + 0.3f*broad)` (with regions/outline: 3.8).
   - Lift (3.8), when any region lifts: `combined += gaussian(liftField, softness)`.
   - For each mask pixel: `(dx, dy) = sobel(combined, i)`; normal = `slopesToNormal(dx, dy)`; flat regions (3.8)
     give `(0,0,1)`; encode; write `(r, g, b, srcAlpha)` into the output at the island pixel's image position.
5. **slopesToNormal**: `x = -dx * strength; y = dy * strength; if (directx) y = -y;` then the length and division as
   upstream. (Upstream's `invert_x`/`invert_y` are not ported: `directx` is the only convention switch.)

`gaussian(f, sigma)`: `sigma < 0.05f` returns a copy; `r = (int)ceilf(sigma * 3.0f)`; kernel
`expf(-0.5f * (t * t))` with `t = (float)o / sigma`. `maskedGaussian(v, w, sigma)` = `gaussian(v*w) /
fmaxf(gaussian(w), 1e-8f)` element-wise (the product `v[i] * w[i]` computed first, in its own array).

Note on smoothing below 0.6: upstream refuses `detail_scale < 0.6`; the contract allows 0..8. Values below 0.6 run the
same code (sigma below 0.05 is the identity), so nothing special is needed; they are simply outside the parity set.

### 3.3 Exact Euclidean distance transform

Felzenszwalb-Huttenlocher, separable, rows then columns, exactly as `distance.rs` (sites skipped where the input is
`+INFINITY`; intersection in double `((h + q*q) - f[p] - p*p) / (2.0 * (q - p))` evaluated left to right with `q*q`
and `p*p` as double products; strict `starts[active+1] < q`; a line without sites becomes `+INFINITY`); the column
pass reads the row pass's squared distances; the final value is `sqrtf`. Mask pixels get `+INFINITY` as input,
background `0`.

### 3.4 Edge modes

- **Transparent** (default): as 3.2. The image border and every cell border behave as transparent (upstream).
- **Wrap** (seamless tiles): each cell (or the image) is processed as **one** field, without island isolation, on a
  torus: the EDT runs each 1-D pass on the line repeated three times (positions `-n..2n-1`, sites taken from all three
  copies) and keeps the middle copy, which is the exact toroidal distance; Gaussian taps and Sobel neighbours index
  modulo the cell size; the lightness centering uses the cell's low/high. A cell without any transparent pixel has
  `dist = +INF` everywhere: its shape height is a constant 0 (the profile is not evaluated), so only detail and lift
  shape it.
- **Extend** (tileset tiles, no border bevel): one field per cell without island isolation; outside the cell
  continues each border pixel outward (clamp-to-edge). For the EDT this is exactly "outside is opaque" (a transparent
  border pixel's outward ray is never closer than the pixel itself), so the EDT runs with no padding and no implicit
  background beyond the cell; blurs and Sobel clamp indices to the cell. A cell without transparency: shape height 0
  as in Wrap.
- Wrap and Extend report `islands = number of cells processed`.

### 3.5 Profiles (inset `t = max(d - 0.5, 0)`, `S = volume`, `R = radius`)

| Profile | Height | Notes |
|---|---|---|
| `rounded` | `(S * R) * log1pf(t / R)` | upstream; keeps rising, never flat |
| `dome` | `t >= R ? S * R : S * sqrtf(fmaxf(R*R - (R - t)*(R - t), 0.0f))` | circular cap reaching `S*R` at `t = R`, flat beyond |
| `bevel` | `S * fminf(t, R)` | straight ramp, flat beyond `R` |

`d = +INF` (Wrap/Extend without transparency) gives 0 for every profile (3.4). In Transparent mode `d` is always
finite.

### 3.6 Auto radius (`settings.radius` empty)

- `request.sequence == true` (GIF/APNG atlases and frame grids marked as an animation): for each cell, find its largest
  island (most pixels; ties: lowest label) and take the shorter side of its bounding box; `m` = the median of those
  values over the cells that have an island (even count: the mean of the two middle values); `R = clamp(0.15f * m,
  4.0f, 512.0f)`, one value for every island. No island anywhere: `R = 4`.
- Otherwise each island gets `R = clamp(0.15f * min(boxWidth, boxHeight), 4.0f, 512.0f)`; Wrap/Extend cells use the
  cell's own size. `Result::radiusPerIsland = true`, min/max reported.
- An explicit radius is used as is everywhere.

### 3.7 Threading and determinism

Work items (3.2 step 3) are collected first, in their deterministic order, then run on `threads` worker threads
(`std::thread`, a shared atomic index over the item list). Items write disjoint output pixels (islands of one cell are
disjoint; cells do not overlap), so the bytes do not depend on scheduling. Auto radius for sequences and outline
detection run before the items start. Cancel is checked before each item; progress reported after items (rate
limited). `std::bad_alloc` anywhere -> `Status::Failed` with kind `failed`. The test of 12.1 runs every fixture with
1 and 8 threads and requires identical bytes.

### 3.8 Outline and regions

- **Region match**: for mask pixels only; Oklab (L, a, b) of the pixel (the same L as 3.2, plus
  `a = 1.9779984951f*l - 2.4285922050f*m + 0.4505937099f*s`, `b = 0.0259040371f*l + 0.7827717662f*m -
  0.8086757660f*s`, from the same l, m, s cube roots) within Euclidean `tolerance` of one of the region's colours. A
  pixel takes the **first** matching region in list order.
- **ignore_outline**: before the work items, over the whole image (all cells): rim pixels are mask pixels with at
  least one 4-neighbour outside the mask or outside their cell; count exact RGB colours among rim pixels; the most
  frequent colour (ties: the darker by Oklab L, then the smaller 0xRRGGBB) is the outline when it covers at least 50%
  of the rim pixels and its Oklab L <= 0.35. It becomes an implicit region `{colors: [outline], tolerance: 0.03,
  detail: 0}` placed **before** the user's regions. None found: warning `outline_not_found` ("No outline colour
  covers half of the silhouette rim; nothing was excluded."), `Result::outlineColor` empty.
- **detail multiplier** (only when some region has `detail != 1`, so the parity path is untouched otherwise): for each
  island, `g[i]` = the pixel's region `detail` (1 when unmatched) on mask pixels; the opacity used by both masked
  Gaussians becomes `opacity[i] * (g[i] > 0 ? 1 : 0)` (excluded colours neither feed nor receive their own lightness:
  their height is interpolated from the neighbours); the gain `G = maskedGaussian(g, opacity, smoothing)` (the
  original opacity); the detail term becomes `detail * G[i] * (0.7f*fine + 0.3f*broad)`.
- **lift**: `lift[i]` = the region's lift on matched mask pixels, 0 elsewhere; `combined += gaussian(lift,
  softness)` (zero extension / the edge mode's indexing).
- **flat**: matched pixels encode `(128,128,255)` (with their alpha).
- **region_unmatched** warning per region that matched no pixel ("regions[2] (#c0ffee) matched no pixel; widen its
  tolerance (0.03).").

### 3.9 Height map

When `settings.height`: `h = strength * combined` at mask pixels (per island, so islands are independent heights);
`heightMin/heightMax` = min/max over all mask pixels of the image; 1-channel 16-bit image, `round((h - min) / (max -
min) * 65535)` for mask pixels (32768 when `max == min`), 0 elsewhere. The recipe records the range (section 6).
`directx` does not change the height map.

### 3.10 Performance targets (Release, the build host)

32x32: under 2 ms; 512x512 opaque blob: under 60 ms single-threaded; 4096x4096 with 4,000 islands: under 4 s with 8
threads. A test asserts the 512 case under 250 ms (generous for CI) only in Release builds (`NDEBUG`).

## 4. Field header: `SpriteNormalsField.h` (2A, verbatim; internal, for tests)

```cpp
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The scalar image operations of the generator (internal: SpriteNormals.cpp and the tests).
// Float-exact ports of upstream field.rs / distance.rs / components.rs / color.rs: see the
// parity rules of the Phase 2 spec (no FMA contraction, evaluation order, float literals).

#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::editor::spritenormals::detail {

    // How a field reads outside itself
    enum class Border { Zero, Wrap, Clamp };

    struct Field {
        int width = 0;
        int height = 0;
        std::vector<float> data;    // row-major
        static Field zero(int width, int height);
    };

    // sRGB 8-bit -> linear, 256 entries (upstream color.rs)
    const float* srgbToLinearTable();
    // Oklab L of an sRGB colour (upstream lightness), and the full Oklab (L, a, b)
    float oklabL(uint8_t r, uint8_t g, uint8_t b);
    void oklab(uint8_t r, uint8_t g, uint8_t b, float& L, float& a, float& bb);

    // Exact Euclidean distance (pixels, centre to centre) from each mask pixel to the nearest
    // non-mask pixel; 0 on non-mask pixels; +INF where the field has none. Border::Zero: nothing
    // beyond the field (upstream); Wrap: toroidal; Clamp: outside counts as mask (Extend).
    Field edt(const std::vector<uint8_t>& mask, int width, int height, Border border = Border::Zero);

    // Separable Gaussian (upstream: sigma < 0.05 is the identity, radius ceil(3 sigma))
    Field gaussian(const Field& input, float sigma, Border border = Border::Zero);
    // gaussian(input * weight) / max(gaussian(weight), 1e-8)
    Field maskedGaussian(const Field& input, const Field& weight, float sigma, Border border = Border::Zero);
    // Normalized Sobel correlation at (x, y): +x slopes up to the right, +y downward. Border::Zero
    // requires (x, y) at least one pixel inside the field.
    void sobel(const Field& input, int x, int y, float& dx, float& dy, Border border = Border::Zero);

    // 8-connected components of `mask` (upstream order)
    struct Component {
        std::vector<int> pixels;    // indices y * width + x, BFS order
        int minX = 0, minY = 0, maxX = 0, maxY = 0;
    };
    std::vector<Component> components(const std::vector<uint8_t>& mask, int width, int height);

    // (128,128,255)-encoding of slopes (upstream slopes_to_normal + byte rounding)
    void encodeNormal(float dx, float dy, float strength, bool directx, uint8_t out[3]);
}
```

## 5. Media header: `SpriteMedia.h` (2A, verbatim)

```cpp
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Sources of the sprite normal generator: still images (PNG 8/16-bit, palette, grey; JPEG, TGA,
// BMP through stb_image) become RGBA8; animated GIFs and APNGs are decoded with spec-exact frame
// compositing (GIF disposal 0-3, APNG dispose and blend ops) and packed row-major into one RGBA8
// atlas (columns = ceil(sqrt(frames)), unused cells transparent), with their delays and loop
// count. Limits are checked from headers and before each frame is decoded (bomb-safe).

#include "SpriteNormals.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    enum class SourceKind { Image, Gif, Apng };
    const char* toString(SourceKind kind);    // "image", "gif", "apng"

    // A frame delay as an exact ratio of milliseconds (GIF: centiseconds * 10 / 1; APNG:
    // delay_num * 1000 / delay_den, den 0 meaning 100, reduced by their gcd)
    struct Delay {
        uint32_t numerator = 100;
        uint32_t denominator = 1;
        double ms() const;
        // What playback uses: ms() rounded; below 10 ms (0 included) -> 100, as browsers do
        int playbackMs() const;
    };

    struct Source {
        SourceKind kind = SourceKind::Image;
        Image color;                    // RGBA8: the image, or the atlas of the frames
        // Animation (kind Gif/Apng): frame size, layout and timing; empty cells for an Image
        int frameWidth = 0;
        int frameHeight = 0;
        int columns = 0;
        int rows = 0;
        int frameCount = 0;
        std::vector<Cell> cells;        // frameCount cells, row-major (the frames in the atlas)
        std::vector<Delay> delays;      // one per frame
        int loopCount = 0;              // 0: forever (GIF NETSCAPE 0 / no extension -> 1; APNG num_plays)
        // The file as it was read
        std::string sha1;               // of the file bytes, 40 hex digits ("" from memory without hashing)
        int fileWidth = 0;              // the image (or the animation's canvas)
        int fileHeight = 0;
        int fileChannels = 0;          // 1..4 (stills; 4 for animations)
        int fileBitDepth = 8;           // 8 or 16
    };

    // What a file is without decoding pixels: its kind, canvas size and frame count (GIF: the
    // image descriptors counted by walking the blocks; APNG: acTL num_frames). For planning
    // output names and refusing oversized inputs early.
    struct SourceInfo {
        SourceKind kind = SourceKind::Image;
        int width = 0;
        int height = 0;
        int frameCount = 1;
    };

    inline constexpr int64_t kMaxInputBytes = 64LL * 1024 * 1024;
    inline constexpr int kMaxFrames = 256;
    // kMaxPixels (SpriteNormals.h) bounds the still image, the canvas x frames decoded so far,
    // and the atlas

    // False with problems (kind invalid_source for unreadable / unsupported / malformed files,
    // too_large for the limits, naming the limit and the actual numbers)
    bool probeSource(const std::filesystem::path& path, SourceInfo& info, std::vector<Problem>& problems);
    bool loadSource(const std::filesystem::path& path, Source& out, std::vector<Problem>& problems);
    bool loadSourceFromMemory(const uint8_t* bytes, size_t size, Source& out, std::vector<Problem>& problems,
                              bool hash = true);

    // Packs equal-size RGBA8 frames row-major (columns = ceil(sqrt(n)), rows = ceil(n / columns)),
    // unused cells transparent black; cells receives the frame rects. False when the atlas would
    // exceed kMaxPixels.
    bool packAtlas(const std::vector<Image>& frames, Image& atlas, int& columns, int& rows, std::vector<Cell>& cells,
                   std::string& error);

    // PNG bytes without ancillary chunks: RGBA8 (normal maps, atlases) or 1-channel 16-bit (height
    // maps); never gAMA/sRGB/iCCP
    bool encodePng(const Image& image, std::vector<uint8_t>& png, std::string& error);
}
```

### 5.1 Stills

`texsynth::loadImageFromMemory` (8/16-bit PNG of any colour type, JPEG, TGA, BMP, PSD) then `texsynth::toRgba8`.
HDR and SVG are refused (`invalid_source`: "Use a PNG (or JPEG/TGA/BMP) image, a GIF or an APNG"). Files over
`kMaxInputBytes` are refused before reading (`too_large`). Size checked from the header before decoding
(`probeImage` / stbi_info). Detection is by magic bytes, never by extension: `GIF87a`/`GIF89a` -> GIF; PNG signature
with an `acTL` chunk before the first `IDAT` -> APNG; anything else -> still.

### 5.2 GIF (own decoder, in `SpriteMedia.cpp`)

stb's GIF loader is not used: its disposal 2 restores the pre-frame state instead of clearing, it exposes no loop
count and it decodes every frame before any limit can be applied. The decoder:

- Header and logical screen descriptor; global colour table; blocks until the trailer `0x3B` (a missing trailer
  after at least one frame is accepted, like browsers).
- Extensions: Graphic Control (disposal method, transparency flag and index, delay in centiseconds), Application
  `NETSCAPE2.0`/`ANIMEXTS1.0` sub-block 1 (loop count: 0 = forever; absent extension -> `loopCount = 1`), others
  skipped by sub-blocks.
- Image descriptor: position, size, local colour table, interlace flag; LZW decoding (min code size 2..8, clear/end
  codes, code size growth to 12 bits, the deferred-clear case where the table is full), interlaced row order (passes
  8/8/4/2 starting 0/4/2/1). Pixels outside the canvas are clipped. Indices beyond the colour table are transparent.
- Compositing on an RGBA canvas starting fully transparent: draw each frame's non-transparent pixels; after the frame
  (before the next): disposal 0/1 keep, 2 clears the frame's rectangle to transparent (0,0,0,0), 3 restores the canvas
  as it was before the frame was drawn. Each frame emitted is the canvas after drawing.
- Limits before decoding each frame: `frames < kMaxFrames` and `(frames + 1) * canvas pixels <= kMaxPixels`;
  canvas `width * height <= kMaxPixels` and both > 0 from the screen descriptor.
- Delays: `Delay{cs * 10, 1}`.
- Truncated or malformed data -> `invalid_source` naming the byte offset ("walk.gif: LZW data ends early in frame 3
  (offset 1840)").

### 5.3 APNG (in `SpriteMedia.cpp`)

- Parse chunks with CRC checks (a CRC mismatch -> `invalid_source`). `acTL` gives `num_frames` (`> kMaxFrames` ->
  `too_large` from the header) and `num_plays` (loop count, 0 = forever).
- Frames: an `fcTL` before `IDAT` makes the default image frame 0 (its data is the `IDAT`s); otherwise the `IDAT`
  image is not part of the animation. Each later `fcTL` starts a frame whose data is the following `fdAT` chunks
  (their 4-byte sequence number stripped). Sequence numbers must increase (else `invalid_source`).
- Each frame is decoded by building a standalone PNG in memory: signature, `IHDR` with the frame's width/height (bit
  depth, colour type, interlace copied from the real `IHDR`), the file's `PLTE` and `tRNS` when present, one `IDAT`
  holding the concatenated frame data, `IEND` (CRCs computed), then `texsynth::loadImageFromMemory` + `toRgba8`.
- `fcTL` validation: `x_offset + width <= canvas width`, same for y, width/height > 0, the first frame's offsets 0 and
  size equal to the canvas (APNG spec); else `invalid_source`.
- Compositing on an RGBA canvas starting transparent: before drawing frame i, apply frame i-1's dispose op (0 none, 1
  clear its region to transparent, 2 restore its region to the canvas before frame i-1 was drawn; a dispose op 2 on
  the first frame is treated as 1, per spec). Draw with blend op 0 (source: replace the region's RGBA) or 1 (over: for
  each pixel with source alpha `sa`, destination `da`: `outA = sa + da*(255-sa)/255`; `outC = (sC*sa +
  dC*da*(255-sa)/255) / outA`, computed in float and rounded to nearest; `outA = 0` gives (0,0,0,0)). The emitted frame
  is the canvas after drawing.
- Delays: `Delay{delay_num * 1000, delay_den == 0 ? 100 : delay_den}` reduced by gcd.
- Limits as for GIF, checked before each frame is decoded.

### 5.4 Atlas

`packAtlas` exactly as upstream (`columns = ceil(sqrt(n))` computed as the smallest c with `c*c >= n`, integer math;
`rows = ceil(n / columns)`), frame i at `((i % columns) * w, (i / columns) * h)`. A GIF/APNG is always an animation,
even with one frame (upstream parity). The colour atlas is what gets written as `<stem>_sheet.png` and fed to
`generate()` with `cells = source.cells`, `sequence = true`.

### 5.5 Encoding

`encodePng`: RGBA8 through `texsynth::encodePng` (stb_image_write, which writes no ancillary chunks); 1-channel 16-bit
through the same function (util/PngWriter). Recipes are added by 2B (`SpriteNormalsRecipe`).

## 6. Recipe header: `SpriteNormalsRecipe.h` (2B, verbatim)

```cpp
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The recipe of a generated sprite normal map: every PNG the generator writes (the normal map,
// the colour atlas of an animation, the height map) carries how it was made as canonical JSON in
// an iTXt chunk with the keyword jasper.spritenormals, right after IHDR (contract section 6). It
// lets the editor regenerate the map, tell that its source changed since (staleness) and show
// what made it. Phase 5 adds a second chunk, jasper.spritenormals.paint.

#include "SpriteMedia.h"
#include "SpriteNormals.h"

#include "json.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    inline constexpr const char* kRecipeKeyword = "jasper.spritenormals";
    inline constexpr const char* kRecipeFormat = "jasperengine.spritenormals";
    inline constexpr int kRecipeVersion = 1;

    // Every setting of the contract (resolved: what generate() used), snake_case keys; floats
    // rounded to 6 decimals; radius "auto" or a number; colours "#rrggbb"
    nlohmann::json settingsToJson(const Settings& settings);
    // The reverse; unknown keys ignored, missing keys keep `out`'s values; false with `error`
    // naming the key on a wrong type or an unknown enum value (values are not range-checked
    // here: validateSettings does that)
    bool settingsFromJson(const nlohmann::json& json, Settings& out, std::string& error);

    // How the frames were found (recipe "frames.from")
    enum class FramesFrom { Single, Sprite, Gif, Apng, Given };
    const char* toString(FramesFrom from);   // "single", "sprite", "gif", "apng", "given"
    bool parseFramesFrom(const std::string& text, FramesFrom& out);

    struct RecipeInput {
        std::string role;                    // "normal", "atlas", "height"
        std::string sourcePath;              // project-relative ('/') inside the project, else relative to the
                                             // directory of the PNG carrying the recipe ('/')
        std::string sourceSha1;
        SourceKind sourceKind = SourceKind::Image;
        bool fromSprite = false;             // the source was a Sprite GameObject's texture
        std::string spriteEntity;            // its hierarchy path (fromSprite)
        unsigned spriteSubmesh = 0;          // its submesh (fromSprite; source.submesh when not 0)
        FramesFrom framesFrom = FramesFrom::Single;
        std::vector<Cell> cells;
        int columns = 0;                     // animations (0 otherwise)
        int rows = 0;
        std::vector<Delay> delays;
        int loopCount = 0;
        Settings settings;
        const Result* result = nullptr;      // radius, outline, height range
        std::string normalOutput;            // project-relative paths of the set written together
        std::string atlasOutput;
        std::string heightOutput;
    };

    // The canonical recipe (contract section 6 + section 6.2 of the Phase 2 spec)
    nlohmann::json makeRecipe(const RecipeInput& input);
    // Compact canonical text (sorted keys, UTF-8 checked): false with `error` naming the path
    // that is not valid UTF-8
    bool recipeText(const nlohmann::json& recipe, std::string& text, std::string& error);

    // Every type the readers rely on; false with `error` ("settings.strength is a string")
    bool validateRecipe(const nlohmann::json& recipe, std::string& error);

    // PNG bytes of `image` (SpriteMedia::encodePng) with the recipe chunk spliced in
    bool encodeWithRecipe(const Image& image, const nlohmann::json& recipe, std::vector<uint8_t>& png, std::string& error);
    // The jasper.spritenormals recipe of PNG bytes / a file; nullopt when there is none (error
    // empty) or it is garbled (error says why). readRecipeHead reads only the chunks before IDAT.
    std::optional<nlohmann::json> readRecipe(const uint8_t* bytes, size_t size, std::string* error = nullptr);
    std::optional<nlohmann::json> readRecipe(const std::filesystem::path& png, std::string* error = nullptr);
    std::optional<nlohmann::json> readRecipeHead(const std::filesystem::path& png, std::string* error = nullptr);
}
```

### 6.1 Generalising the texsynth chunk code (2B, no behaviour change for `jasper.synthesis`)

In `editor/texsynth/SynthesisRecipe.h/.cpp`, add a trailing `const char* keyword = kRecipeKeyword` parameter to
`insertRecipeChunk`, `readRecipeTextFromPng`, `readRecipeText` and `readRecipeTextHead` (and use it everywhere the
keyword is compared or written). `insertRecipeChunk` drops only existing chunks with the **same** keyword, so a
normal map can carry `jasper.spritenormals` and later `jasper.spritenormals.paint`. Every existing caller compiles
unchanged; `TexSynthTest.cpp` stays green; a new test in `SpriteNormalsRecipeTest.cpp` puts both keywords in one PNG
and reads each back.

### 6.2 Recipe schema (version 1)

```json
{
  "format": "jasperengine.spritenormals",
  "version": 1,
  "generator": "JasperEngine 0.9.0",
  "role": "normal",
  "source": {"path": "assets/hero.png", "sha1": "…", "kind": "sprite", "entity": "Player/Hero"},
  "frames": {"from": "sprite", "cells": [[0,0,32,32],[32,0,32,32]]},
  "settings": {"preset": "default", "strength": 1.5, "detail": 3, "smoothing": 1, "volume": 1.15,
               "radius": "auto", "softness": 1.15, "alpha_threshold": 0.03, "profile": "rounded",
               "edges": "transparent", "ignore_outline": false, "regions": [], "directx": false, "height": false},
  "radius_resolved": 4.8,
  "outline": "#1a1c2c",
  "height_range_px": [0, 7.25],
  "outputs": {"normal": "assets/hero_normal.png", "atlas": null, "height": null}
}
```

`generator` is `"JasperEngine " JASPERENGINE_EDITOR_VERSION` (EditorVersion.h, as texsynth does). Rules: keys sorted
(nlohmann's default `std::map` ordering), compact dump; `source.kind` is `"sprite"` when `fromSprite`, else
`toString(sourceKind)`; `entity` is present exactly when kind is `"sprite"`; `source.submesh` only when kind is
`"sprite"` and `spriteSubmesh != 0`; `frames.columns/rows/delays_ms/loop` only for animations (`delays_ms` as exact
`[num, den]` ratios); `cells` empty for a single image; `radius_resolved` a number, or `"per-island"` with
`radius_range` `[min, max]`; `outline` only when ignore_outline found one; `height_range_px` only when a height map was
made; `outputs` lists every file of the set (null for those not written). Regions are `{"colors": ["#rrggbb"],
"tolerance": …, "detail": …, "lift": …, "flat": …}`. Paths: project relative with `/` inside the project, else relative
to the directory of the PNG carrying the recipe (with `/`; for the normal map, the normal output's directory); refuse
non-UTF-8 paths (recipeText error).

## 7. Preview header: `SpriteNormalsPreview.h` (2B, verbatim)

```cpp
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What a sprite normal map looks like, without a GPU: the sprite lit by 2D point lights with
// exactly the engine's formula (shaderlib includes/lighting2d.glsl getLight2DIntensity on the
// unlit 2D path of mesh.frag: linear albedo x (ambient + sum of colour x intensity x attenuation
// x N.L), gamma 2.2 back to sRGB), a contact sheet for agents and tests, quality metrics and the
// plain-language assessment shared by the window, the MCP tool and the CLI.

#include "SpriteNormals.h"

#include <string>
#include <vector>

namespace jasperengine::editor::spritenormals {

    // A point light over the image. Image pixel coordinates (x right, y DOWN, origin top-left
    // of the image given to renderLit); height and range in pixels (world units of a 1:1 sprite)
    struct PreviewLight {
        float x = 0.0f;
        float y = 0.0f;
        float height = 64.0f;          // > 0 (the engine default for new Light 2Ds)
        float range = 200.0f;
        float falloff = 1.0f;          // pow(1 - d/range, falloff)
        float red = 1.0f, green = 1.0f, blue = 1.0f;   // linear
        float intensity = 1.0f;
    };

    struct LightSetup {
        std::vector<PreviewLight> lights;
        float ambientRed = 1.0f, ambientGreen = 1.0f, ambientBlue = 1.0f;   // linear
        float ambientIntensity = 0.15f;
        bool directx = false;          // the map's convention (green flipped back before lighting)
        float alphaThreshold = 0.03f;  // pixels at or below are left transparent
    };

    // One light at `angleDegrees` around the image centre (0 = right, 90 = up the image,
    // counter-clockwise), at distance 0.75 * max(w, h), height 0.5 * max(w, h), range 3 * max(w, h)
    LightSetup orbitLight(int width, int height, float angleDegrees, bool directx = false, float alphaThreshold = 0.03f);

    // RGBA8 of `color`'s size: lit colour, the colour's alpha. normal and color the same size.
    Image renderLit(const Image& normal, const Image& color, const LightSetup& setup);

    // The contact sheet (section 7.2): per frame a row "colour | normal | lit 135° | lit 45° |
    // lit 315° | lit 225°" (upper-left, upper-right, lower-right, lower-left), on a checkerboard
    struct SheetOptions {
        int maxWidth = 1024;
        int maxHeight = 1024;
        int maxFrames = 8;
        int maxScale = 8;
    };
    Image contactSheet(const Image& color, const Image& normal, const std::vector<Cell>& cells, bool directx,
                       float alphaThreshold, const SheetOptions& options = SheetOptions());
    // Which frames a sheet shows: all when <= maxFrames, else round(i * (n - 1) / (maxFrames - 1))
    std::vector<int> sheetFrames(int frameCount, int maxFrames);

    // ---- quality

    struct Metrics {
        double coveragePct = 0.0;      // opaque pixels (above the cutoff) / all pixels
        double flatPct = 0.0;          // opaque pixels tilted under 2 degrees from +Z
        double steepPct = 0.0;         // tilted over 60 degrees
        double meanTiltDeg = 0.0;      // mean tilt of opaque pixels
        double leanDeg = 0.0;          // angle between the mean opaque normal and +Z (NormalMapTilt's measure)
        double noisePct = 0.0;         // opaque pixels with >= 3 opaque 8-neighbours whose normal is over
                                       // 35 degrees from those neighbours' mean normal
        double softEdgePct = 0.0;      // opaque pixels with alpha < 250
        int framesTouchingEdges = 0;   // cells whose opaque pixels touch the cell border (cells given)
        bool hasTransparency = false;  // some pixel at or below the cutoff
        int opaquePixels = 0;
    };
    Metrics measure(const Image& normal, const std::vector<Cell>& cells, bool directx, float alphaThreshold);

    struct Assessment {
        std::vector<std::string> lines;   // "Shape: rounded (mean tilt 24°, 12% flat)", ...
        std::string hint;                 // "Looks fine." when nothing applies
        std::vector<Problem> warnings;    // kinds of section 7.3
    };
    Assessment assess(const Settings& settings, const Metrics& metrics, int cellCount);
}
```

### 7.1 renderLit

For each pixel with alpha/255 above the cutoff: `n = rgb/255*2-1` (green negated first when `directx`), normalized
(a zero vector counts as +Z); position `p = (x + 0.5, y + 0.5)`; for each light: `to = (lx - px, -(ly - py))`
(image y down -> world y up, so green-up maps face lights above them), `d = |to|`; skip when `d >= range`; `atten =
pow(clamp(1 - d/range, 0, 1), falloff)`; `ndotl = clamp(dot(n, normalize(to.x, to.y, height)), 0, 1)` (height > 0
always in the preview); `acc += colour * intensity * atten * ndotl`. `lit = srgbToLinear(rgb) * (ambient *
ambientIntensity + acc)` with `srgbToLinear(c) = pow(c, 2.2)` and back with `pow(c, 1/2.2)`, clamped, rounded; alpha =
the colour's alpha. Pixels at or below the cutoff: (0,0,0,0). Exactly the engine's formula (contract 7: normal-mapped
fragments use N.L).

### 7.2 Contact sheet layout

- Frames: `cells` empty -> one frame (the whole image); otherwise `sheetFrames(cells.size(), maxFrames)`.
- Six columns per row: colour, normal (as stored), then `renderLit` of the frame crop with `orbitLight` at 135°, 45°,
  315°, 225°. Each tile is the frame scaled by the integer `s = min(maxScale, floor((maxWidth - 5*4) / (6*w)),
  floor((maxHeight - (rows-1)*4) / (rows*h)))`, at least 1 (nearest), with 4 px gaps.
- Background: an 8x8-pixel (sheet pixels) checkerboard `#2a2a30`/`#222227`; tiles composited over it with alpha
  "over". RGBA8, opaque.
- If even `s = 1` exceeds the limits, frames are scaled down by the integer factor that fits (nearest, every k-th
  pixel) so the sheet never exceeds `maxWidth x maxHeight`.

### 7.3 Assessment

`lines`, in this order (numbers rounded to integers; `%` without a space):
1. `Shape: <profile> (mean tilt N°, N% flat)` 2. `Steep pixels: N%` 3. `Noise: N%` 4. `Lean: N°` 5. `Coverage: N%`
   6. for cells: `Frames: N (N touch their cell's edge)`.

Warnings (`Problem{kind, field "", message, fix}`), each with its exact message pattern:

| kind | when | message / fix |
|---|---|---|
| `no_alpha` | `!hasTransparency` and edges `transparent` | "The image has no transparent pixels: volume only bevels its border." / "For tiles use edges wrap or extend." |
| `mostly_flat` | `flatPct > 60` | "N% of the sprite is flat." / "Raise strength or volume, or detail if it is painted with shading." |
| `too_steep` | `steepPct > 25` | "N% of the pixels are steeper than 60°." / "Lower strength (try N), or raise smoothing." with N = strength * 0.6 rounded to 0.1 |
| `noisy` | `noisePct > 15` | "N% of the pixels disagree with their neighbours (dithering or hard shading bands)." / "Raise smoothing to 1.5-2 or lower detail." |
| `leaning` | `leanDeg > 8` | "The map leans N° on average: the painted light direction became a slope." / "Lower detail, or add a region with detail 0 for the highlight colours." |
| `frames_touch_edges` | `framesTouchingEdges > 0` and edges `transparent` | "N frames touch their cell's edge, which gets a rim there." / "Use edges extend if the sprite continues past the cell." |
| `soft_edges` | `softEdgePct > 10` | "N% of the opaque pixels are semi-transparent; they count as solid." / "Raise alpha_threshold to ignore glows and soft edges." |

`hint`: the fix of the first warning in table order, else `"Looks fine."`.

## 8. Session header: `SpriteNormalsSession.h` (2B, verbatim)

```cpp
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor side of the sprite normal generator, shared by the Sprite Normal Map window
// (Phase 4), the MCP tool generate_sprite_normals and the CLI generate-sprite-normals (Phase 3),
// so all of them resolve sources and frames, name and write outputs, and word feedback the same:
//
// - Spec: what a generation is made of (project-relative paths, settings, frames, outputs);
// - checkSpec(): every problem visible without decoding pixels; prepare(): load + Request;
// - plannedOutputs() / defaultOutputs(): the naming of contract section 5;
// - encodeOutputs() + writeOutputsUndoable(): the PNGs with their recipe (and the frames JSON),
//   ONE step of the project command history; writeOutputsDirect() for the CLI;
// - checkStaleness() / specFromRecipe(): written maps back to their source and settings;
// - framesFromSprite(), lightCheck(), sceneWarnings(): what a scene says about the result;
// - SpriteNormalsRun: generate() on a worker thread.

#include "SpriteMedia.h"
#include "SpriteNormals.h"
#include "SpriteNormalsPreview.h"
#include "SpriteNormalsRecipe.h"

#include "ecs/Entity.h"
#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
}

namespace jasperengine::editor::spritenormals {

    // How the frames are chosen (contract `frames`)
    struct FramesSpec {
        enum class Mode { Auto, None, Grid, Cells };
        Mode mode = Mode::Auto;
        Grid grid;                   // Mode::Grid
        std::vector<Cell> cells;     // Mode::Cells (a Sprite's frames, or given rects)
        FramesFrom from = FramesFrom::Given;   // Mode::Cells: what the cells are (Sprite or Given), recorded
    };

    // Output paths, project-relative ('/'); "" = the default name (contract section 5)
    struct OutputSpec {
        std::string normal;
        std::string atlas;           // animated sources only
        std::string height;          // settings.height only
        bool framesJson = false;
        std::string framesJsonPath;  // "" = <atlas stem>.frames.json (or <normal stem>.frames.json)
    };

    struct Spec {
        std::string source;          // project-relative image path; "" when sprite is set (its texture)
        uint32_t sceneId = 0;        // with entity: a Sprite (or any mesh) GameObject as the source
        Entity entity = NULL_ENTITY;
        unsigned submesh = 0;        // with entity: the submesh whose base colour texture is the source
        Settings settings;
        FramesSpec frames;
        OutputSpec output;
        bool overwrite = false;

        bool hasSprite() const { return entity != NULL_ENTITY; }
        // Everything that changes the result, as text (the window's "stale" test; submesh included)
        std::string fingerprint() const;
    };

    // ---- paths and naming

    // `rel` against the project; refused when absolute, climbing out of it or inside a hidden
    // directory (the texsynth rules)
    bool resolveProjectFile(const std::filesystem::path& projectDir, const std::string& rel, std::filesystem::path& absolute,
                            std::string& error);
    std::string projectRelative(const std::filesystem::path& projectDir, const std::filesystem::path& absolute);

    struct PlannedOutput {
        std::string role;            // "normal", "atlas", "height", "frames_json"
        std::string path;            // project-relative
    };
    // Contract section 5 names for a source: <stem>_normal.png, or <stem>_sheet.png +
    // <stem>_sheet_normal.png for GIF/APNG; <stem>_height.png / <stem>_sheet_height.png;
    // <stem>_sheet.frames.json (animations) or <stem>_normal.frames.json (stills)
    std::vector<PlannedOutput> defaultOutputs(const std::string& sourcePath, SourceKind kind, bool height, bool framesJson);
    // The files `spec` writes (its explicit paths, else the defaults), in the order above
    std::vector<PlannedOutput> plannedOutputs(const std::filesystem::path& projectDir, const Spec& spec, SourceKind kind);

    // ---- frames

    // The Sprite's frames (SpriteComponent framesRect, pixels; only when submesh is 0, else no
    // cells) and the colour texture of `submesh` (its base colour, project-relative); false with
    // `error` (kind not_a_sprite / invalid_source)
    bool framesFromSprite(const Scene* scene, Entity entity, unsigned submesh, std::string& texturePath,
                          std::vector<Cell>& cells, std::string& error);

    // ---- check and prepare

    // Every problem visible without decoding pixels: settings, source missing / outside the
    // project / unsupported (probeSource), frames (a grid or none on a GIF/APNG: frames_mismatch;
    // grid empty or too many cells; overlapping Sprite frames: frames_mismatch), outputs (not
    // .png, outside the project's assets directory, colliding with the source or each other,
    // existing without overwrite: output_exists, not valid UTF-8)
    std::vector<Problem> checkSpec(const std::filesystem::path& projectDir, const Spec& spec, const Scene* scene = nullptr);

    // A spec resolved and loaded: what generate() runs and encodeOutputs() writes
    struct Prepared {
        std::filesystem::path projectDir;
        Spec spec;
        std::string sourcePath;              // project-relative (or absolute outside a project)
        std::filesystem::path sourceAbsolute;
        Source source;                       // decoded
        FramesFrom framesFrom = FramesFrom::Single;
        std::string spriteEntityPath;        // hierarchy path when from a Sprite
        Request request;
        std::vector<PlannedOutput> outputs;
    };
    // checkSpec, then the source decoded and the Request built; false with the problems
    bool prepare(const std::filesystem::path& projectDir, const Spec& spec, const Scene* scene, Prepared& out,
                 std::vector<Problem>& problems);

    // ---- outputs

    struct OutputFile {
        std::string role;
        std::string path;                    // project-relative
        std::filesystem::path absolute;
        std::string bytes;                   // PNG with its recipe, or the frames JSON
    };
    bool encodeOutputs(const Prepared& prepared, const Result& result, std::vector<OutputFile>& files, std::string& error);
    // One step of the project command history (WriteFilesCmd through w4::writeFilesUndoable),
    // then the editor notices them (textures reloaded, thumbnails, Project window). Main thread.
    // The step is named "Generate Normal Map <file name of the first normal output>" ("Generate
    // Normal Maps (N)" for N normal outputs).
    bool writeOutputsUndoable(const std::vector<OutputFile>& files, std::string& error);
    // All or nothing without history (the CLI): texsynth::writeFilesAllOrNothing
    bool writeOutputsDirect(const std::vector<OutputFile>& files, std::string& failedPath, std::string& error);

    // The frames JSON (contract section 5): {"version":1, "source":<recipe path>, "width":W,
    // "height":H, "animation":{"frame_width", "frame_height", "columns", "rows", "frame_count",
    // "delays_ms":[[num,den],...], "frame_ms":[float,...], "loop":<loopCount>},
    // "settings":settingsToJson}; a still with cells has "animation" with frame_count = cells,
    // delays_ms/frame_ms/loop omitted
    nlohmann::json framesJson(const Prepared& prepared);

    // ---- recipes and staleness

    struct Staleness {
        enum class State { Fresh, Changed, Missing, NoRecipe, Invalid };
        State state = State::NoRecipe;
        std::string sourcePath;              // as recorded
        std::filesystem::path sourceAbsolute;
        std::string message;                 // "hero.png changed since this map was made"
    };
    const char* toString(Staleness::State state);   // "fresh", "changed", "missing", "no_recipe", "invalid"
    // projectDir empty: a recipe source.path is resolved against the PNG's own directory
    Staleness checkStaleness(const std::filesystem::path& projectDir, const std::filesystem::path& png);

    // A written map's recipe back into a spec (same settings, frames, outputs = that set,
    // overwrite on); `notes` say what could not be restored (a Sprite no longer in the scene ->
    // the texture path is used). False with `error` when the recipe can't be used.
    bool specFromRecipe(const nlohmann::json& recipe, const std::filesystem::path& png, const std::filesystem::path& projectDir,
                        Spec& spec, std::vector<std::string>& notes, std::string& error);

    // ---- what the scene says

    struct LightCheck {
        int lights = 0;                      // enabled Light 2Ds
        int lightsWithHeight = 0;            // ... with height > 0 (they light normal maps)
        float ambientIntensity = 0.0f;       // scene ambient 2D intensity
    };
    LightCheck lightCheck(const Scene* scene);
    // The scene-aware warnings of section 8.2, the only ones every surface shows: no_light (no
    // enabled Light 2D), no_light_height (lights > 0 but none with height), ambient_high
    // (ambientIntensity >= 0.8), and for an entity: size_mismatch (its base texture size !=
    // normal size), filter_mismatch (a normal texture set, or the result will be applied, and its min/mag
    // filter or wrap differ from the base texture's; apply copies them, so a willApply result is
    // compared after that copy), material_linked (submesh material from a .material file),
    // too_many_frames (delays.size() > 128), directx_in_engine (settings.directx),
    // shader_ignores_normals (a manifest shader for which !Catalog::shaderReadsMaterialNormalMap);
    // and zero_delay (some delay's playbackMs replaced a delay under 10 ms by 100). Scene null:
    // only zero_delay.
    std::vector<Problem> sceneWarnings(const Scene* scene, Entity entity, const Settings& settings, int normalWidth,
                                       int normalHeight, const std::vector<Delay>& delays, bool willApply);

    // ---- worker

    // generate() on a detached worker thread (the texsynth SynthesisRun pattern): the run keeps
    // itself alive until it finishes; no exception escapes (out of memory -> Status::Failed).
    class SpriteNormalsRun : public std::enable_shared_from_this<SpriteNormalsRun> {
    public:
        using Finished = std::function<void(SpriteNormalsRun& run)>;
        // onFinished runs on the worker thread once result() is ready
        static std::shared_ptr<SpriteNormalsRun> start(Prepared prepared, int threads = 0, Finished onFinished = nullptr);
        // Editor shutdown: callbacks stop, every run cancelled and waited for; false on timeout
        static bool cancelAll(double timeoutSeconds = 10.0);
        // A project switch: cancel without waiting
        static void cancelRunning();

        void cancel() { cancelFlag = true; }
        bool finished() const { return done.load(); }
        Progress progress() const;
        // texsynth::projectGeneration() when the run started
        uint64_t generation() const { return startGeneration; }
        const Prepared& prepared() const { return prep; }
        const Result& result() const { return res; }

    private:
        SpriteNormalsRun() = default;
        Prepared prep;
        Result res;
        uint64_t startGeneration = 0;
        std::atomic<bool> cancelFlag{false};
        std::atomic<bool> done{false};
        mutable std::mutex mutex;
        Progress last;
    };
}
```

### 8.1 Session semantics

- **Sources**: `spec.source` (project-relative) or a Sprite (`sceneId` + `entity` + `submesh`): its submesh
  `spec.submesh` base colour texture's path is the source (frames from the Sprite only when submesh is 0); `frames`
  `Auto` then uses the Sprite's `framesRect` (pixel rects) when it has any (`FramesFrom::Sprite`), else the whole
  image. The recipe records `source.submesh` when it is not 0. Outside a project (the CLI without `--project`)
  `projectDir` is empty and paths are used as given (relative to the working directory, absolute allowed); the
  recipe's `source.path` is then relative to the directory of the PNG carrying it (6.2), and `checkStaleness`
  resolves it against the PNG's own directory.
- **Frames**: `Auto` -> GIF/APNG frames, the Sprite's frames, else none. `None` -> whole image; on a GIF/APNG
  refused with `frames_mismatch` ("An animation's frames come from the file; leave frames auto."). `Grid` ->
  `sliceGrid` (refused with `frames_mismatch` on a GIF/APNG: "The frames come from the GIF; leave frames auto.").
  `Cells` -> as given, recorded as `frames.from` = `FramesSpec::from` (`Sprite` or `Given`). Sprite frames that
  overlap: `checkSpec` reports `frames_mismatch` with the fix "Use frames none, or reslice the sprite without
  overlap". `request.sequence` = true for GIF/APNG and for a Grid or Sprite frames with 2 or more cells, and for
  `Cells` with 2 or more cells.
- **Outputs**: `.png` (case-insensitive) for images, `.json` for the frames JSON; inside the project's assets
  directory (`Project::isInsideAssetsPath`, or below `projectDir` when only a path is known) and not in a hidden
  directory; distinct from the source and each other (`output_invalid`); existing files need `overwrite`
  (`output_exists`, fix "Pass overwrite, or another output path."); a non-UTF-8 path is refused with a message saying
  to rename it.
- **Writing**: editor -> `writeOutputsUndoable` (one Project undo step for the whole set); CLI ->
  `writeOutputsDirect`. Every PNG gets its recipe (`role` normal/atlas/height); the atlas is written only for
  animated sources. The colour atlas never overwrites the source.
- **Project switches**: a run records `texsynth::projectGeneration()` at start; whoever writes its result compares it
  with the current one and refuses (`project_changed`: "Another project was opened while generating.") when it
  changed.
- **Staleness**: recipe found -> the source's SHA-1 now vs recorded (`changed`), missing file (`missing`); no recipe
  (`no_recipe`); garbled (`invalid`, message from validateRecipe).

### 8.2 Scene warnings (exact messages)

| kind | message | fix |
|---|---|---|
| `no_light` | "This scene has no Light 2D." | "Create a Light 2D (new lights get Height 64)." |
| `no_light_height` | "No Light 2D in this scene has a height above 0, so normal maps don't change the lighting." | "Set a Light 2D's Height (64 is a good start)." |
| `ambient_high` | "The scene's 2D ambient intensity is N, which washes out normal maps." | "Lower Ambient Light 2D intensity to 0.2-0.4." |
| `size_mismatch` | "The sprite's texture is WxH but the normal map is WxH; they must match pixel for pixel." | "Generate from the sprite's own texture (source: the GameObject)." |
| `filter_mismatch` | "The Normal Texture is filtered differently from the Base Texture (pixels will smear at edges)." | "Apply the map (it copies the filter and wrap), or give the Normal Texture the Base Texture's filter and wrap." |
| `material_linked` | "The sprite's material comes from M; the scene's copy changed, the file did not." | "Save the material from the Inspector to keep the map in M." |
| `too_many_frames` | "The animation has N frames; a Sprite holds at most 128." | "Trim the animation, or use the atlas with your own frames." |
| `directx_in_engine` | "The map uses DirectX green (down); this engine reads OpenGL (green up), so lights look upside down." | "Turn directx off for this engine's sprites." |
| `shader_ignores_normals` | "The sprite's material shader S does not sample a normal map." | "Use the built-in sprite material or library:sprite." |
| `zero_delay` | "N frames have a delay under 10 ms; they play at 100 ms, as browsers do." | "Set the Sprite Animation's frame times if another rate is meant." |

## 9. Apply header: `SpriteNormalsApply.h` (2B, verbatim)

```cpp
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Wiring a generated normal map onto a GameObject as ONE step of its scene's command history
// (the Save & Assign button, MCP apply): every submesh whose base colour texture is the map's
// colour source gets normalTexture = the map, with the colour texture's min/mag filter and wrap
// copied onto it. For an animated source (GIF/APNG atlas) the Sprite also gets the colour atlas
// as its base texture (filter and wrap kept), the frames (frame_0..), its size set to one frame,
// frame 0 shown, and a Sprite Animation child (new, or the existing one targeting it) cycling
// the frames with the file's delays (playbackMs) and loop (loopCount 1 -> no loop), autoplay
// true when created. Refused while the scene plays. Main thread.

#include "SpriteNormals.h"
#include "SpriteMedia.h"

#include "ecs/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::spritenormals {

    struct ApplyRequest {
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        std::string normalPath;              // project-relative, written already
        std::string colorSourcePath;         // the texture the map was made from (matches submeshes by it)
        struct Map {
            std::string normalPath;
            std::string colorSourcePath;
        };
        std::vector<Map> maps;               // stills: every (normal, colour) pair, all applied in one step
                                             // (normalPath/colorSourcePath are maps[0] when maps is empty)
        // Animated sources: the colour atlas becomes the base texture and these the Sprite's frames
        std::string atlasPath;               // "" for stills
        std::vector<Cell> frames;
        std::vector<int> frameMs;            // one per frame (Delay::playbackMs)
        int loopCount = 0;
        std::string animationName = "Sprite Animation";
    };

    struct ApplyResult {
        bool ok = false;
        Problem error;                       // scene_playing, not_a_sprite, size_mismatch, too_many_frames
        std::vector<Problem> warnings;       // material_linked
        std::vector<int> submeshes;          // the submeshes given the map
        Entity animation = NULL_ENTITY;      // the Sprite Animation made or updated
        bool createdAnimation = false;
        std::string undoName;                // "Assign Normal Map" / "Assign Animated Sprite"
    };

    ApplyResult applyToSprite(Project* project, const ApplyRequest& request);
    // One Scene undo step for all (one scene); all or nothing: the first refusal undoes what was
    // added and is the result's error
    ApplyResult applyToSprites(Project* project, const std::vector<ApplyRequest>& requests);
}
```

Apply semantics: entity must have a `MeshComponent` (`not_a_sprite` otherwise); still sources need at least one submesh
whose base colour path equals `colorSourcePath` (normalized, project-relative), else `not_a_sprite` ("Its texture is X,
not Y."); with `maps`, every pair is matched this way (each against its own colour path) and all of them are applied in
the same step; the normal map's size must equal that texture's pixel size (`size_mismatch`). Animated: the entity must
have a `SpriteComponent`; > 128 frames -> `too_many_frames`. Commands: `PropertyCmd`/`MultiPropertyCmd` on Catalog
property names (`submeshes[N].material.normalTexture`, `...baseColorTexture`, `framesRect[i].rect`,
`framesRect[i].name`, `numFramesRect`, `width`, `height`, SpriteAnimation `frames[i]`, `framesTime[i]`, `framesSize`,
`framesTimeSize`, `loop`, `autoplay` (true when created), ActionComponent `target`, `state`),
`CreateEntityCmd(SPRITE_ANIMATION)` parented to the sprite, all added with `setNoMerge()` and grouped with
`CommandHistory::mark()`/`groupSince()` into one step; a failure undoes what was added (the WorldTools `UndoStep`
pattern) and returns the error. The scene is marked modified. `applyToSprites` runs every request inside one
`mark()`/`groupSince()` group of their (single) scene; requests for different scenes are refused (`not_a_sprite` naming
the scene).

## 10. Build wiring (2A, Task 0)

- Root `CMakeLists.txt`, right after the texsynth source set:
  ```cmake
  # The sprite normal generator core (editor/spritenormals: spritefusion-sprite-normals ported, NOTICE.md),
  # also built alone into jasperengine-spritenormals-tests (-DJASPERENGINE_SPRITENORMALS_TESTS=ON)
  set(JASPERENGINE_SPRITENORMALS_SOURCES
      ${EDITOR_DIR}/spritenormals/SpriteNormals.cpp
      ${EDITOR_DIR}/spritenormals/SpriteNormalsField.cpp
      ${EDITOR_DIR}/spritenormals/SpriteMedia.cpp
      ${EDITOR_DIR}/spritenormals/SpriteNormalsRecipe.cpp
      ${EDITOR_DIR}/spritenormals/SpriteNormalsPreview.cpp
  )
  if(NOT MSVC)
      set_source_files_properties(${EDITOR_DIR}/spritenormals/SpriteNormals.cpp ${EDITOR_DIR}/spritenormals/SpriteNormalsField.cpp
          PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
  endif()
  ```
  The editor's source list gets `${JASPERENGINE_SPRITENORMALS_SOURCES}`, `SpriteNormalsSession.cpp` and
  `SpriteNormalsApply.cpp` next to the texsynth editor files. The option block after the texsynth one:
  `option(JASPERENGINE_SPRITENORMALS_TESTS "Build jasperengine-spritenormals-tests (sprite normal generator tests)" OFF)`
  + `include(${EDITOR_DIR}/spritenormals/tests/SpriteNormalsTests.cmake)`.
- `SpriteNormalsTests.cmake` mirrors `TexSynthTests.cmake`: sources `${JASPERENGINE_SPRITENORMALS_SOURCES}`,
  `${JASPERENGINE_TEXSYNTH_SOURCES}`, `util/PngWriter.cpp`, `file(GLOB ... tests/*Test.cpp)`; links gtest_main,
  gtest, stb, Threads; `-Wall -Wextra -Wshadow`; defines `SPRITENORMALS_FIXTURES="${CMAKE_CURRENT_LIST_DIR}/fixtures"`
  and `SPRITENORMALS_UPDATE_GOLDEN` read from the environment at run time (not compile time).
- `tools/build.sh spritenormals-tests` exactly like `texsynth-tests` (usage lines, the case list, the linux image,
  `SPRITENORMALS_TEST_ARGS`, `RUN=0`).
- Editor tests need no wiring (glob).

## 11. Problem kinds (shared with Phases 3-5)

Errors: `invalid_source`, `invalid_settings`, `frames_mismatch`, `too_large`, `output_exists`, `output_invalid`,
`scene_playing`, `not_a_sprite`, `size_mismatch` (apply), `too_many_frames` (apply), `write_failed`,
`project_changed`, `cancelled`, `failed`. Warnings: `no_alpha`, `mostly_flat`, `too_steep`, `noisy`, `leaning`,
`frames_touch_edges`, `soft_edges`, `outline_not_found`, `region_unmatched`, `no_light`, `no_light_height`,
`ambient_high`, `size_mismatch`, `filter_mismatch`, `material_linked`, `too_many_frames`, `directx_in_engine`,
`shader_ignores_normals`, `zero_delay`. Every message names the value and limit; every fix is an action.

## 12. Tests and fixtures

### 12.1 2A: `tests/SpriteNormalsCoreTest.cpp`

- **Golden parity** (the acceptance gate). Fixtures in `tests/fixtures/` (inputs copied from `/tmp/sfsn-eval/in/`,
  expected outputs produced by the upstream binary `/tmp/sfsn-eval/build/target/release/spritefusion-sprite-normals`
  with `--force`, both committed; a `tests/fixtures/README.md` lists each command line so they can be regenerated):

  | Fixture | Input | Upstream command |
  |---|---|---|
  | ball | `ball.png` 32x32 | defaults (`-o ball_n.png`) |
  | highlight | `highlight.png` | defaults |
  | donut | `donut.png` (hole) | defaults |
  | character | `character.png` (outline) | defaults |
  | thin | `thin.png` (1 px antenna, sword) | defaults |
  | flat | `flat.png`, `flat_outline.png`, `flat2.png` | defaults |
  | alpha | `alpha.png` (soft glow) | defaults |
  | sheet | `sheet.png` 128x32 | `--frame-size 32x32` |
  | tiles | `tiles.png` 64x64 opaque | defaults |
  | walk | `walk.gif` 8 frames | defaults (atlas `walk_n.png`, `walk_n.color.png`, `walk_n.frames.json`) |
  | spin | `spin.gif` | defaults |
  | apng | `twoframes.png`: a 12x10 APNG (frame 1 orange 1/12 s, frame 2 a 3x2 blue patch at (4,3), 1/20 s, blend over), written once by a test helper | defaults |
  | variants | `character.png`, `ball.png` | `--normal-strength 2.5 --detail-strength 4 --detail-scale 1 --shape-strength 2 --shape-radius 8 --shape-softness 0.5` and `--normal-format directx` |
  | tiny | 1x1, 1x19, 23x1 opaque grey 80; 20x20 diagonal (alpha 255 where x == y) | defaults |
  | upstream tests | `disk(31)` (RGB 145,99,63, alpha where hypot <= 12), 16x16 gradient alpha, 64x32 ramp `40+3x` (`--shape-strength 0`), the 9x9 sprite + 42x34 sheet | as in upstream tests/generation.rs |

  Each fixture runs our `generate()` with the equivalent settings (`profile rounded`, `edges transparent`, explicit
  `radius` 4 or the variant's, the five numbers, `directx`, cells for the sheet/atlases) and compares every pixel: alpha
  exactly equal; RGB within 1; at least 99.5% of pixels exactly equal (logged). GIF fixtures also compare our
  `loadSource` atlas with `*_n.color.png` exactly and our delays/columns with `frames.json`.
- **Upstream invariants** (ported from `tests/generation.rs`): outward normals and unit length on the disk; zero
  strength / zero volume on flat colour -> exactly neutral; ramp relief; alpha preserved and hidden RGB ignored;
  padding/nearby-island independence; sheet cells isolated; tiny shapes deterministic; GL vs DirectX flips only green
  (`|G_gl + G_dx - 255| <= 1`, R/B/A equal); invalid settings and dimensions rejected with the right kinds.
- **EDT** brute-force test (upstream's LCG, seed 75, sizes 1..12 x 1..16) for `Border::Zero`, plus Wrap and Clamp
  against brute force on the torus / with outside-as-mask.
- **Determinism**: every fixture with `threads` 1 and 8 -> identical bytes.
- **Options**: dome/bevel heights at known insets; bevel/dome on a 64x64 square give flat (exactly 128,128,255) centres
  and rounded uses ramps; Wrap on a seamless tile: a 3x3 tiling of the output has no seam (the normals across the wrap
  edge equal those of the same tile shifted by half its size and regenerated); Extend on an opaque tile: no pixel
  within 2 px of the border tilts more than the interior's maximum; auto radius values for a 32x32 ball (4.8 per
  island), a 200x40 bar (6), a sequence (median rule); ignore_outline finds the outline of `character.png` and
  `flat_outline.png` and none on `ball.png` (warning); regions: detail 0 on the highlight colour of `highlight.png`
  lowers `leanDeg`-style asymmetry (mean |red - 128| over the highlight pixels drops), lift raises (the slope at a lifted
  region's border points outward), flat gives exact (128,128,255); region_unmatched warning; height map range and
  monotonicity on the disk (centre highest); validation messages for every setting's range.
- **Grid**: `sliceGrid` against `set_sprite_frames` semantics (tile size, columns/rows, both given: tile size used and
  columns/rows capping the counts, offset, padding, count, clipping, empty grid error); overlapping cells refused with
  `frames_mismatch`; identical duplicates processed once. - **Performance** (Release only): 512x512 under 250 ms
  single-threaded.

### 12.2 2A: `tests/SpriteNormalsMediaTest.cpp`

GIF built in-test (an encoder helper in the test writing LZW with clear codes): three frames red full / 4x4 green at
(4,4) dispose 3 / 2x2 blue at (10,10) with delays 5, 9, 12 cs -> delays [50,90,120], atlas 32x32, frame 3 shows red
where frame 2 was, empty 4th cell alpha 0 (upstream media test); disposal 2 clears to transparent; interlaced frame;
local colour table; transparency index; NETSCAPE loop 0 and 3 and absent (-> 1); truncated LZW -> invalid_source with
an offset; 65535x65535 screen -> too_large before decoding; 300 frames -> too_large at frame 257 without decoding it.
APNG built in-test (stb_image_write's zlib): default image as frame 0 and as non-frame; dispose 0/1/2; blend 0/1 with
a half-transparent pixel (exact expected values); fractional delays (1/12 s); den 0 -> 1/100; bad CRC and decreasing
sequence numbers -> invalid_source; 16-bit and palette APNGs. Stills: 16-bit PNG, palette PNG with tRNS, grey+alpha,
JPEG (opaque) all to RGBA8; HDR refused; > 64 MiB refused before reading (a sparse file); probeSource frame counts
without decoding (checked by timing-free means: a GIF whose later frames are garbage still probes its count).
`packAtlas` layouts 1, 2, 3, 5, 10 frames.

### 12.3 2B: `tests/SpriteNormalsRecipeTest.cpp`, `tests/SpriteNormalsPreviewTest.cpp`

Recipe: makeRecipe canonical bytes for a fixed input (golden string in the test); `source.kind` `"sprite"` with
`entity` when `fromSprite` (and `submesh` only when not 0), the decoded kind and no `entity` otherwise; `delays_ms`
as `[num, den]` pairs; settings round-trip through JSON
for random settings (all enums, regions, radius auto/number); validateRecipe errors; encode -> readRecipe and
readRecipeHead; both keywords in one PNG (6.1); a texsynth recipe untouched by the new keyword.
Preview: renderLit against hand-computed values for a flat normal and a tilted normal under one light (exact bytes),
directx flips the response, pixels at the cutoff transparent; orbitLight positions; contactSheet size rules (1 frame,
4 frames, 20 frames -> 8 shown with the spread rule, a 2000x10 strip scaled down), checkerboard and opacity; metrics
on synthetic maps (all flat -> flatPct 100; a disk -> meanTilt range; noise injected -> noisePct); assess lines and
every warning's threshold edge (just below / above).

### 12.4 2B: `editor/tests/SpriteNormalsSessionTest.cpp`, `editor/tests/SpriteNormalsApplyTest.cpp`

Session (temporary project directory, generated PNG/GIF files): defaultOutputs for png/gif/apng with and without
height/frames JSON; checkSpec kinds (missing source, outside project, hidden dir, grid on a GIF, none on a GIF,
overlapping Sprite frames, existing output, output outside the assets directory, collision with source, non-.png
output); prepare + generate + encodeOutputs: the normal PNG has the recipe, the atlas too, the frames JSON schema (the
exact keys of `framesJson`'s comment, for an animation and for a still with cells); `Cells` frames with `from = Sprite`
recorded as `frames.from` `"sprite"` and run as a sequence; outside a project the recipe's `source.path` is relative to
the output's directory and `checkStaleness` with an empty `projectDir` resolves it; writeOutputsDirect all-or-nothing (a
read-only target dir); writeOutputsUndoable + undo removes the set (the editor test harness's MCP context, like
TextureSynthesisTest); staleness fresh -> changed -> missing; specFromRecipe round trip (same fingerprint); a Sprite
source with `submesh` 1 (a Tilemap with two tilesets): its texture is the source, no Sprite frames, recipe
`source.submesh` 1, specFromRecipe restores it; project generation refusal (`project_changed`); the undo step name;
SpriteNormalsRun finishes, cancel, cancelAll. Scene: a scene with a Sprite (CreateEntityCmd), framesFromSprite after
setting frames, lightCheck with no light / a light at height 0 / height 64 / ambient 1.0, sceneWarnings each kind
(`no_light` and `no_light_height` separately, `filter_mismatch` not reported with willApply, `zero_delay` with a null
scene).
Apply: still map onto a Sprite (normalTexture path and copied filters; one undo step restores everything), a tilemap
with two tilesets (only the matching submesh; with `maps` holding both pairs, both submeshes in one undo step),
`applyToSprites` on two Sprites (one undo step; a refusal of the second leaves the first unchanged), size_mismatch,
not_a_sprite, scene_playing, animated GIF (atlas base texture, frames, size, animation child with framesTime from
delays, loop, autoplay), an existing Sprite Animation updated instead of a new one, 129 frames -> too_many_frames,
undo/redo of the whole step.

## 13. Acceptance criteria

1. `jasperengine-spritenormals-tests` and the new editor tests pass in Release; `jasperengine-texsynth-tests` and the
   existing editor tests still pass.
2. Golden parity of 12.1 holds on every fixture (alpha exact, RGB within 1, >= 99.5% exact).
3. Every header in this spec exists verbatim (comments may be extended, declarations not changed without a ruling).
4. No warning from `-Wall -Wextra -Wshadow` in the new files; everything compiles as C++17 (`JASPERENGINE_CXX_STANDARD` defaults to 17: no C++20 features).
5. No change in behaviour for `jasper.synthesis` recipes, texsynth outputs or any existing test.
6. 2A and 2B each write a report: what was built, test commands and outputs, any deviation (with a reason).

## 14. Task breakdown

**2A (core + media)** — branch `sn/p2a`, worktree of `sprite-normals`.
1. Task 0 skeleton (section 1), committed first.
2. `SpriteNormalsField` (Oklab, EDT with three borders, Gaussian, masked, Sobel, components, encode) + its tests.
3. `SpriteNormals` (settings, presets, parse/print, validation, sliceGrid, generate: transparent pipeline first,
   parity fixtures, then profiles, edges, auto radius, outline/regions, height map, threads, cancel, progress).
4. `SpriteMedia` (stills, GIF decoder, APNG decoder, probe, atlas, encode) + media tests.
5. Fixtures + README, licence file, NOTICE row (`| spritefusion-sprite-normals | Hugo Duprez | MIT |
   editor/spritenormals/ (C++ port of the normal-map generator) | https://github.com/Hugo-Dz/spritefusion-sprite-normals |`,
   matching the table's existing columns).

**2B (recipe, preview, session, apply)** — branch `sn/p2b`, merges 2A's skeleton commit first; may merge 2A's branch
again later to run its tests against the real generator (until then it tests against images built in the test).
1. texsynth keyword generalisation (6.1) + test.
2. `SpriteNormalsRecipe` + tests.
3. `SpriteNormalsPreview` (renderLit, orbitLight, contactSheet, sheetFrames, measure, assess) + tests.
4. `SpriteNormalsSession` (everything of section 8) + editor tests.
5. `SpriteNormalsApply` + editor tests.

Shared files: only 2A edits `CMakeLists.txt`, `tools/build.sh`, `NOTICE.md`; only 2B edits
`editor/texsynth/SynthesisRecipe.{h,cpp}`.

## Amendments (2026-10-27, consistency review)

Rulings applied from the cross-spec consistency review (`.superpowers/sdd/2026-10-27-sprite-normals-contract/reports/spec-consistency.md`,
items in brackets). The sections above already carry them; this is the checklist.

### Affects 2A (SpriteNormals.h, SpriteNormalsField.h, SpriteMedia.h, core/media .cpp, tests)

- [ ] `SpriteNormals.h`, comment only above `presetSettings` (no declaration change) [1]:
  `// Resolving a preset with overrides (every surface does exactly this):`
  `//   Settings s = presetSettings(p); settingsFromJson(overrides, s, error); s.preset = p; validateSettings(s)`
- [ ] `SpriteNormals.h` `Grid` comment + `sliceGrid` behaviour (`SpriteNormals.cpp`) [27]: when both a tile size and
  columns/rows are given, the tile size is used and columns/rows > 0 cap the counts (as `sliceSheet`). Test: 12.1
  **Grid** gains the "both given" case.
- [ ] `SpriteNormalsField.h`, `SpriteMedia.h`, `kMaxCells` (stays 4096) [7]: no change.
- [ ] Branch name `sn/p2a` (was `sn-2a`) [45].

### Affects 2B (recipe, preview, session, apply)

Recipe (`SpriteNormalsRecipe.h/.cpp`, 6.2):
- [ ] `RecipeInput::sourcePath` comment: `// project-relative ('/') inside the project, else relative to the directory of
  the PNG carrying the recipe ('/')`; recipe paths outside a project follow it (was: absolute) [8].
- [ ] New member after `spriteEntity`: `unsigned spriteSubmesh = 0;          // its submesh (fromSprite; source.submesh when not 0)` [5].
- [ ] Rules: `source.kind` is `"sprite"` when `fromSprite`, else `toString(sourceKind)`; `entity` present exactly when
  kind is `"sprite"`; `source.submesh` only when kind is `"sprite"` and not 0; `delays_ms` are `[num, den]` pairs.
  The schema example is now a Sprite source (`"kind": "sprite"`, `"frames": {"from": "sprite", ...}`) [9, 16, 5].
- [ ] `validateRecipe`/`specFromRecipe` accept and restore `source.submesh` [5].

Session (`SpriteNormalsSession.h/.cpp`, 8, 8.1, 8.2):
- [ ] `FramesSpec` new member: `FramesFrom from = FramesFrom::Given;   // Mode::Cells: what the cells are (Sprite or Given), recorded` [28].
- [ ] `Spec` new member after `entity`: `unsigned submesh = 0;        // with entity: the submesh whose base colour texture is the source`;
  `fingerprint()` includes it [5].
- [ ] `framesFromSprite` signature: `bool framesFromSprite(const Scene* scene, Entity entity, unsigned submesh,
  std::string& texturePath, std::vector<Cell>& cells, std::string& error);` (cells only when submesh is 0) [5].
- [ ] `sceneWarnings` signature: `std::vector<Problem> sceneWarnings(const Scene* scene, Entity entity, const Settings&
  settings, int normalWidth, int normalHeight, const std::vector<Delay>& delays, bool willApply);` (`frameCount`
  removed: `too_many_frames` uses `delays.size()`) [4].
- [ ] `sceneWarnings` kinds (8.2 table, exact messages): new `no_light` (split from `no_light_height`, whose "(or
  This scene has no Light 2D.)" alternative is gone); `filter_linear` renamed `filter_mismatch` (normal texture set,
  or the result will be applied, and min/mag filter or wrap differ from the base texture's; not reported for a
  willApply result, apply copies them); new `directx_in_engine` (`settings.directx` with an entity), new
  `shader_ignores_normals` (`!Catalog::shaderReadsMaterialNormalMap` for a manifest shader, Phase 1), new
  `zero_delay` (a delay whose `playbackMs` replaced a delay under 10 ms by 100; the only kind with a null scene) [4].
- [ ] Comment-only header changes: `checkSpec` (none on a GIF/APNG, overlapping Sprite frames, outputs inside the
  assets directory), `writeOutputsUndoable` (step name), `framesJson` (exact schema), `checkStaleness` (empty
  `projectDir`) [13, 29, 14, 21, 10, 8].
- [ ] Behaviour: the source of a GameObject is submesh `spec.submesh`'s base colour texture; recipe `source.submesh`
  when not 0 [5].
- [ ] Behaviour: `frames` `None` on a GIF/APNG is refused with `frames_mismatch` ("An animation's frames come from the
  file; leave frames auto.") (was: accepted, frames kept) [13].
- [ ] Behaviour: `Cells` records `frames.from` = `FramesSpec::from`; `request.sequence` also true for `Cells` with 2 or
  more cells [28].
- [ ] Behaviour: overlapping Sprite frames -> `checkSpec` `frames_mismatch`, fix "Use frames none, or reslice the
  sprite without overlap" [29].
- [ ] Behaviour: outputs must be inside the project's assets directory (`Project::isInsideAssetsPath`, or below
  `projectDir` when only a path is known) (was: inside the project) [14].
- [ ] Behaviour: the Project undo step is "Generate Normal Map <file name of the first normal output>" ("Generate
  Normal Maps (N)" for N normal outputs) [21].
- [ ] Behaviour: `framesJson` writes `{"version":1, "source", "width", "height", "animation":{"frame_width",
  "frame_height", "columns", "rows", "frame_count", "delays_ms":[[num,den],...], "frame_ms":[float,...], "loop"},
  "settings"}`; a still with cells: `animation` with `frame_count` = cells, no `delays_ms`/`frame_ms`/`loop` [10].
- [ ] Behaviour: `checkStaleness` with an empty `projectDir` resolves `source.path` against the PNG's directory [8].
- [ ] Kind rename: a project switch refuses with `project_changed` (was `write_failed`) [2].

Apply (`SpriteNormalsApply.h/.cpp`, 9):
- [ ] `ApplyRequest` new members after `colorSourcePath`:
  `struct Map { std::string normalPath; std::string colorSourcePath; };`
  `std::vector<Map> maps;               // stills: every (normal, colour) pair, all applied in one step (normalPath/colorSourcePath are maps[0] when maps is empty)` [6].
- [ ] New function: `ApplyResult applyToSprites(Project* project, const std::vector<ApplyRequest>& requests);` (one
  Scene undo step for all, one scene; all or nothing; requests for different scenes refused with `not_a_sprite`) [6].
- [ ] Behaviour: a created Sprite Animation gets `autoplay` true [20].

Shared:
- [ ] Section 11 kinds: errors gain `project_changed`; warnings `filter_linear` -> `filter_mismatch`, new `no_light`,
  `directx_in_engine`, `shader_ignores_normals`, `zero_delay` [2, 4].
- [ ] Tests 12.3/12.4 gain: recipe sprite kind/entity/submesh and `delays_ms` pairs; checkSpec none on GIF,
  overlapping Sprite frames, output outside assets; exact `framesJson` keys; `Cells` from Sprite; recipe path and
  staleness outside a project; submesh 1 source; `project_changed`; undo step name; `no_light`/`no_light_height`,
  `filter_mismatch` with willApply, `zero_delay` with a null scene; apply `maps`, `applyToSprites`, `autoplay`.
- [ ] Branch name `sn/p2b` (was `sn-2b`) [45].

## Amendments (2026-10-28, integration review)

- Parity: bit-exact with the upstream binary on every fixture on Linux/glibc (the reference platform of the
  fixtures); across platforms within 1 per channel (the contract's bound: `cbrt`, `exp`, `pow` of another C library
  may round differently). No compiler contracts `a*b+c` into an FMA in the generator, the media decoding or the paint
  compositing: `-ffp-contract=off` (GCC, Clang), `/fp:precise` (MSVC), `/fp:precise` + `/clang:-ffp-contract=off`
  (clang-cl), set on `SpriteNormals.cpp`, `SpriteNormalsField.cpp`, `SpriteMedia.cpp`, `SpriteNormalsPaint.cpp`.
