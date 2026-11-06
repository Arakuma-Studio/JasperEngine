# Mesh memory: components sized by their content, no capacities (design)

Date 2026-11-04 · Branch `dc/mesh-memory` · Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md` (#11)

Every size in this spec is an estimate unless it says otherwise: field arithmetic from the headers
(`/var/lib/orquester/workspaces/jaspersito/dc-work/analysis/verdicts/big-claims.md` #1, 64-bit libstdc++), not
`sizeof` output. Nothing here was built or run. The tests of section 10 settle the numbers.

## 1. Goals

1. A `MeshComponent` costs what it holds, not what it could hold: at least 10x less per mesh on every ABI
   (estimated 204,840 B today; target under 10 KiB for a one-submesh Sprite, component plus its arrays), without
   lowering what a mesh can hold.
2. No capacities. The six compile-time capacities (`MAX_SUBMESHES` 16, `MAX_EXTERNAL_BUFFERS` 30, `MAX_BONES` 128,
   `MAX_SPRITE_FRAMES` 128, `MAX_TILEMAP_TILES` 2048, `MAX_TILEMAP_TILESRECT` 200) and their machinery (scene
   `maxValues`, export `-D` definitions, player capacities, the Compile reason) go. The editor and every game use the
   same containers, so what the editor shows is what the game holds, and the prebuilt player takes any project.
3. Pipeline descriptions only while needed: a working description during a load, then one shared immutable base
   description per distinct (shader, vertex layout, index type, primitive type); GL warm-draw tables only on objects
   that warm.
4. A bone palette only on skinned meshes, sized to the skin (no palette on a box, a sprite or a tilemap).
5. The editor and the serializers never cap content at a default (Doriax `52179c98a`): scene decode, cooked
   scenes, the Inspector's Add buttons, Duplicate Tile, Scene view tile drops, MCP tools, property paths.
6. Heap freed by a scene switch or a Play Stop goes back to the operating system where the allocator can do it.
7. Regressions are caught (sizeof ceilings in the unit tests) and visible (component memory per type in the Systems
   window and `get_ecs_stats`, the description pool in `get_memory_stats`).

## 2. Non-goals

- Shrinking `Material` (2,664 B, five inline `Texture`s of 456 B) or `Texture`: authored data shared by many
  components and the material system; a later change. After this spec it is the largest part of a `Submesh`.
- Allocating a submesh's depth and G-buffer `ObjectRender`s lazily (about 700 B each after this spec).
- Sprite batching, shared sprite buffers and the 8,192-sprite buffer ceiling (`sprite-batching`).
- Skipping redundant pipeline/binding applies (`draw-perf`, Doriax `d103cae0c`) and gating bone bounds on skinning
  (`draw-perf`, `859ab7d9d`). This spec makes a rigid mesh's palette empty, which removes that scan's cost anyway.
- Doriax's `SkinComponent` (`22cb5f9d4`): rejected in D5.
- Lowering the capacity defaults (Doriax `94cfcfd62`): rejected in D1.
- Shrinking WebAssembly memory (linear memory cannot shrink) and a Windows heap trim (D9).
- The cooked scene format (`.jscn`, `.jpfb`): unchanged (section 9).
- 32-bit tilemap indices: the existing 16,383 drawn-tile limit (`MAX_TILEMAP_RENDERED_TILES`,
  `engine/core/component/TilemapComponent.h:9`) stays and becomes the visible tile limit (D7).

## 3. Users and stories

- A mobile developer with 3,000 sprites: today about 3,000 x 211 KB = 630 MB of CPU memory before textures
  (estimate; on wasm the linear memory must grow by about as much). After: about 30 MB. The game no longer dies on
  a 2 GB phone or a browser tab.
- An artist imports a car with 24 materials into a prebuilt-player project. Today the build silently switches to
  Compile ("MAX_SUBMESHES 24 (the player holds 16)") or, off the main thread, a preview drops 8 primitives. After:
  it draws whole, Prebuilt stays available.
- A level designer paints a 6,000-tile map in the Inspector, saves and reopens: today the reload keeps 2,048 tiles
  (`editor/StreamComponents.cpp:460`). After: 6,000 tiles; Add Tile is disabled at 16,383 with a tooltip saying why.
- A user edits the material of submesh 19 of an imported model, saves, reopens: today the edit is gone
  (decode stops at 16, `editor/StreamComponents.cpp:157`). After: kept.
- An agent sets 200 sprite frames with `set_sprite_frames`: today refused ("A sprite holds at most 128 frames",
  `editor/mcp/tools/WorldTools.cpp:2855`). After: accepted.
- An agent checking memory calls `get_ecs_stats` and reads `MeshComponent: element_bytes 1,6xx, heap_bytes ...`;
  `get_memory_stats {prefix: "Render.PipelineDescs"}` shows 6 shared descriptions for 5,000 meshes.
- A Linux user leaves Play after a heavy scene: the editor's resident memory drops back instead of staying at the
  Play peak.

## 4. Findings

Static reading of `main` (`b4f7816ea`); line numbers are hints.

F1. **Every mesh holds 16 submeshes inline.** `HybridArray<T, N>` is `T values[N]{}` outside the editor
`engine/core/util/HybridArray.h:48` and a `std::vector<T>(N)` in the editor (`:19`), so editor and game both pay
for N elements. `MeshComponent::submeshes` is `HybridArray<Submesh, MAX_SUBMESHES>` (`MeshComponent.h:404`,
`Engine.h:89`): 16 x 11,776 B = 188,416 B per mesh. A Sprite uses one (`MeshSystem.cpp:566`). Estimated
`sizeof(MeshComponent)` 204,840 B (libstdc++), 197,032 B (libc++), 173,712 B (wasm32); Doriax 19,528 B.
`PagedStorage` pages hold at least 64 elements (`engine/core/ecs/PagedStorage.h:32-39`), so the first mesh of a
scene reserves about 13 MB.

F2. **Two pipeline descriptions per render object for life.** `SokolObject` holds `sg_pipeline_desc pipeline_desc`
and `baseDesc` by value (`engine/renders/sokol/SokolObject.h:81,85`), 808 B each on 64-bit; three `ObjectRender`s
per `Submesh` (`MeshComponent.h:192-194`): 77,568 B per mesh. After a load only the base part is read
(`SokolObject.cpp:696` `makePipelineDesc` copies `pipeline_desc`; `:1040,1055` at draw for the shader check and the
mirrored/no-cull variants; `:621-654` `prewarmWith`; `:968-977` `endLoadDynamic` copies it to `baseDesc`, read at
`:986,997`). The base is exactly what `setShader`, `setIndex`, `addAttribute` and `beginLoad` write
(`:409-527`): shader, vertex layout, index type, primitive type. Doriax `edef8ba6e` frees its description after
load, which Jasper cannot copy because of those draw-time reads. Also per object: `warmedPipeline[16]` and
`warmKey[16]` (192 B, `SokolObject.h:109-110`), used only while GL warm draws run
(`SokolAsyncCreate::warmDrawsActive()`, `SokolObject.cpp:101-111`).

F3. **Every mesh holds a 128-bone palette.** `HybridArray<Matrix4, MAX_BONES> bonesMatrix` (`MeshComponent.h:423`),
8,192 B on a box or a sprite. The skinning shader reads the palette from a texture or storage buffer of any row
count (`engine/shaderlib/includes/skinning.glsl:6-30`; `RenderSystem.cpp:128-145` sizes it from
`bonesMatrix.size()`), so 128 is a capacity of the container, not of the GPU. A skin over 128 joints fails the
model load in a game (`MeshSystem.cpp:5541-5549`).

F4. **Sprites, tilemaps, points and sprite animations hold their capacities inline too.** `SpriteComponent::framesRect`
128 x 48 B = 6,144 B (`SpriteComponent.h:38`), `PointsComponent::framesRect` the same (`PointsComponent.h:51`),
`SpriteAnimationComponent::frames`/`framesTime` 2 x 512 B (`SpriteAnimationComponent.h:16,19`),
`TilemapComponent::tiles` 2,048 x 56 B and `tilesRect` 200 x 56 B, about 126 KB per tilemap
(`TilemapComponent.h:65,68`; my arithmetic, `std::string` 32 B).

F5. **The editor caps content at the defaults (Doriax `52179c98a`).** The editor's arrays start at N and grow on
`operator[]`, but these paths stop at `size()`, which is N until something grew it:
- decode: submeshes `editor/StreamComponents.cpp:157` (then `:160` sets `numSubmeshes` to the full count), external
  buffers `:148`, sprite frames `:383`, tile rects `:446`, tiles `:460`, points frames `:2228`;
- add: Duplicate Tile `editor/util/ProjectUtils.cpp:1714`, tile drop `editor/window/SceneWindow.cpp:1118`, Inspector
  Add Frame `editor/window/InspectorWindow.cpp:8622`, Add Tile Rect `:9430`, tile from rect `:9530`, Add Tile `:9658`;
- property paths: `editor/reflect/Resolve.cpp:135` refuses an index at or past `size()`.

Consequences (verified by the skeptic pass by reading, not run): a model of 17+ primitives loses saved submesh
edits past slot 16 on reload, because `MeshSystem::collectSubmeshOverrides` (`MeshSystem.cpp:449-462`) reads the
default slots; and encoding a `const MeshComponent` whose `numSubmeshes` is over the vector's size reads past the
vector (`StreamComponents.cpp:32,51` with `HybridArray.h:43-45`), undefined behaviour.

F6. **Capacities shape exports.** `Project::calculateSceneMaxValues` (`editor/Project.cpp:3579-3700`, called at save
`:7199-7207`) stores per-scene maxima in the scene file (`editor/Stream.cpp:3830-3843`, read `:4100-4115`);
`Exporter::sceneMaxValuesOf` (`editor/Exporter.cpp:3982-4012`) floors them at the defaults and feeds the Compile
`-D` definitions (`:4014-4022`, marker `engine/CMakeLists.txt:204-208`), the Xcode definitions (`:5581-5584`), the
compile-cache key (`:1337-1339`) and the Prebuilt/Compile choice (`editor/ExporterPrebuilt.cpp:161-180`). Players
publish their capacities (`tools/players/package_player.py:51,73-80,240`; `editor/players/PlayerRuntimes.h:35`).
`countGltfExternalBuffers` (`Project.cpp:3464`) exists only for this. Prefabs only a script spawns are not counted
(Doriax `b1df76163`, owned by `export-pipeline`).

F7. **Capacity checks and refusals are spread across engine, editor and MCP.** Engine: `MeshSystem.cpp:2096`
(external buffers, `capacityReached`), `:2694` (merge refusal), `:5336-5364` and `:5517-5522` (`reportOverCapacity`),
`:5541-5551` (bones), `:6547-6551` (OBJ), `Tilemap.cpp:83-91,207`, `Sprite.cpp:181,196`, `Points.cpp:292,311`,
`VideoPlayerSystem.cpp:222,621`, `PropertyBindings.cpp:288,297,1373`, `ComponentHooks.cpp:92,107,121`. Editor/MCP:
`WorldTools.cpp:2476,2521-2523,2680,2716-2717,2833,2855-2857,2974-2975`, the Sprite Normals tools and window
(`SpriteNormalsTools.cpp:437,720,1219`, `SpriteNormalsApply.cpp:162,255`, `SpriteNormalsSession.cpp:218,1178`,
`SpriteNormalsWindow.cpp:127,491,495,612,698`, `SpriteNormalsEntry.h:123`, `SpriteNormalsCli.cpp:921`),
`InspectorAnimation.cpp:786,804,809`. 39 `submeshes.size()` uses in 19 files; most mean "capacity".

F8. **No heap trim.** `SceneManager::updateLoading` ends at `cancelPreload` (`engine/core/manager/SceneManager.cpp:763`)
with no `malloc_trim`; the only `malloc_trim` in the tree is a test (`MarkdownParserTest.cpp:323`). Doriax
`9d397c6ca` trims after loads on glibc Linux only.

## 5. Product decisions

D1. **Remove the capacities instead of lowering them.** Doriax lowered the defaults (`94cfcfd62`); in Jasper that
would push ordinary content (5+ submeshes, 33+ frames) off the prebuilt player and shrink what the Inspector, MCP
and scripts accept. Unity has no such limits. With storage sized by content, a capacity has no purpose: the six
macros, `maxValues`, the `-D` definitions and the Compile reason go. Reason: smaller and simpler, and every
project can use the prebuilt player.

D2. **Two containers, one behaviour in the editor and in games.**
- `StableArray<T>` for `Submesh` and `ExternalBuffer`: elements on the heap one by one, never moved by growth, so
  the pointers the render code keeps for a frame (`RenderSystem.h:537` `const Material*`, the local
  `name -> &eBuffers[i]` maps at `RenderSystem.cpp:6152`, `MeshSystem.cpp:3199` ...) stay valid while it grows.
- `GrowArray<T>` for bones, frames, tiles, tile rects: contiguous (`data()` feeds the bone upload), a vector that
  grows on write, exactly what the editor's `HybridArray` already is. Engine code compiled into the editor already
  runs under these semantics (inferred: the editor tree defines `JASPERENGINE_EDITOR`, `CMakeLists.txt:354`).
- Both: `operator[]` (non-const) grows to the index; `const operator[]` past the end returns a shared
  default-constructed element without growing (today's inline arrays return their default slots; the editor's
  vector read past its end, F5); nothing shrinks implicitly. Reason: the 10,574 `submeshes[...]` uses and the
  generated scene code (`editor/Factory.cpp:1200-1252`) keep their meaning.

D3. **One index limit as a sanity check, not a capacity.** `ARRAY_INDEX_LIMIT = 65,536` elements per array:
`validIndex(i)` is `0 <= i < 65,536` for both containers. API entry points that take an index from a user, a file
or an agent (Lua object methods, decoders, property paths, MCP tools) check it and say so; nothing legitimate comes
near it. Reason: an index of 2,000,000,000 from a typo must be an error, not a 100 GB allocation.

D4. **Pipeline descriptions: a working copy during the load, then an interned shared base.** Doriax's drop
(`edef8ba6e`) does not fit (F2). `beginLoad` allocates the working description; `endLoad`/`endLoadDynamic` intern
its base (shader, layout, index type, primitive type) in a pool keyed by `SokolPipelineCache::descHash`/`descEqual`
(which exist, `SokolPipelineCache.cpp:325-397`) and free the working copy. Equal objects share one base: 5,000
equal sprites hold one description, not 10,000. GL warm tables move behind a pointer allocated only when a warm
draw is first needed. Reason: about 1.8 KB less per `ObjectRender` (5.3 KB per submesh) with no change at draw
time other than one pointer read on a variant's first use.

D5. **The bone palette stays in `MeshComponent`, sized by the skin.** Doriax moved it to a `SkinComponent`
(`22cb5f9d4`, about 19 files, a new component in the format). A `GrowArray<Matrix4>` that is empty on rigid
meshes and sized to the skin's joints at model load gives the same memory gain with no format change, and lifts the
128-joint limit (F3).

D6. **The editor and serializer cap bug is fixed by construction and by audit.** The containers have no `size()`:
they have `stored()` (elements held). Every one of the existing `size()` uses on these members fails to compile and
is rewritten to the count field (`numSubmeshes`, `numTiles` ...), to `stored()`, or deleted when it was a capacity
check. Decoders size the array to the count they set. Property paths may append (index equal to `stored()`).
Reason: the compiler lists every site, so none is missed.

D7. **Visible limits are real limits only.** Tiles: the existing drawn-tile limit `MAX_TILEMAP_RENDERED_TILES`
(16,383, 16-bit indices) is what the Inspector, the Scene view drop, Duplicate Tile and `edit_tilemap` stop at,
with that reason ("A Tilemap draws at most 16,383 tiles"). Frames, tile rects, submeshes, external buffers, bones:
only `ARRAY_INDEX_LIMIT`. Reason: today a user may meet 2,048 in one place and nothing in another.

D8. **Scene `maxValues` is kept as read, never computed.** Precedent: the legacy collider caps
(`Stream.cpp:3837-3842`). An old scene's `maxValues` node is written back unchanged (no churn in users' files); a
new scene has none. Nothing reads it. Reason: no format version bump, no diff on first save.

D9. **Heap trim where it returns memory, off the main thread.** `memory::releaseFreeHeap()` runs as one coalesced
Background-lane job after a `SceneManager` load completes, after Play Stop (`Project::finalizeStop`) and after a
scene is closed in the editor. glibc Linux: `malloc_trim(0)`. macOS/iOS: `malloc_zone_pressure_relief(nullptr, 0)`.
Android: `mallopt(M_PURGE, 0)` on API 28+, looked up with `dlsym` (the player builds with minSdk 21; the pattern of
`engine/platform/android/MediaCodecBackend.cpp:11`). Web: nothing (wasm memory cannot shrink). Windows: nothing
(the process heap decommits free pages itself; `_heapmin` has no documented effect on the UCRT heap; assumed, not
measured). Reason: mobile first; Doriax's glibc-only call misses the platforms with the tightest budgets.

D10. **Players that declare capacities are refused for Prebuilt.** A player packaged before this change (only
reachable with Build Settings' allow-mismatch option) still truncates. Its `player-info.json` has a non-empty
`capacities`; `chooseBuildMethod` then gives the reason "player <id> holds fixed array capacities (built before
<version>): use this editor's player or Compile". New players omit `capacities`.

D11. **No new Lua API.** Nothing here is a gameplay behaviour: limits disappear and memory shrinks. The heap trim is
automatic at the points where memory is freed; a script has no better moment to ask for it. The existing Lua
methods that take indices (`Sprite:addFrame(id, ...)`, `Points:addFrame`, `Tilemap:addRect`, `Tilemap:addTile`)
keep their signatures; their errors name the index limit instead of a capacity.

## 6. Design

### 6.1 Containers (`engine/core/util/GrowArray.h`, `engine/core/util/StableArray.h`)

Header-only, `namespace jasperengine`, replacing `engine/core/util/HybridArray.h` (deleted).

```cpp
// GrowArray.h
constexpr size_t ARRAY_INDEX_LIMIT = 65536;   // a sanity limit for indices from users, files and agents

// A contiguous array that grows when an index past its end is written: what a component holds is what it
// uses. Growth moves the elements (a vector): keep indices, not references, across writes past the end.
template<typename T>
class GrowArray {
public:
    size_t stored() const;                     // elements held: one past the highest index written or resized to
    T* data(); const T* data() const;          // stored() contiguous elements
    bool validIndex(int index) const;          // 0 <= index < ARRAY_INDEX_LIMIT
    T& operator[](size_t index);               // grows to index + 1 (new elements value-initialised)
    const T& operator[](size_t index) const;   // past stored(): defaultElement(), no growth
    void resize(size_t count);                 // the decoders' and the model load's
    void clear();                              // stored() 0, storage freed (shrink_to_fit)
    void insert(size_t index, const T& value); // the reflection's array ops
    void erase(size_t index);
    void move(size_t from, size_t to);
    size_t storageBytes() const;               // capacity() * sizeof(T): component memory reports
    static const T& defaultElement();          // one per T, made on first use, never destroyed
};
```

```cpp
// StableArray.h
// Elements allocated one by one: growth never moves them, so a pointer to one stays valid until it is
// erased, cleared or its array is destroyed. Copies are deep; a move takes the elements.
template<typename T>
class StableArray {
public:
    size_t stored() const;
    bool validIndex(int index) const;          // as GrowArray
    T& operator[](size_t index);               // grows: allocates the missing elements
    const T& operator[](size_t index) const;   // past stored(): defaultElement()
    void resize(size_t count);                 // frees the elements past count
    void clear();
    void reverse(size_t first, size_t count);  // swaps the element pointers (MeshSystem's winding reversal)
    void insert(size_t index, const T& value); void erase(size_t index); void move(size_t from, size_t to);
    size_t storageBytes() const;               // stored() * sizeof(T) + the pointer vector
    static const T& defaultElement();
private:
    std::vector<std::unique_ptr<T>> elements;
};
```

Rules: `defaultElement()` is `static const T* const e = new T();` (thread-safe initialisation; leaked on purpose:
a `Submesh` holds `ObjectRender`s and must never be destroyed after sokol shuts down). Copy constructor and copy
assignment deep-copy; move constructor and move assignment are `noexcept` and steal. Equality for the reflection
(6.6) compares index by index up to the larger `stored()`, a missing element counting as `defaultElement()`, so two
arrays that differ only in trailing default elements are equal.

### 6.2 Components

- `MeshComponent` (`engine/core/component/MeshComponent.h`): `StableArray<ExternalBuffer> eBuffers`,
  `StableArray<Submesh> submeshes`, `GrowArray<Matrix4> bonesMatrix`; a runtime field
  `uint32_t bonesUploadRows = 0` (rows of the GPU palette, not saved, not reflected). Counts stay
  (`numSubmeshes`, `numExternalBuffers`): the 782 `numSubmeshes` uses keep their meaning. Invariant: after any load,
  decode or factory, `submeshes.stored() >= numSubmeshes` and `eBuffers.stored() >= numExternalBuffers` (writes
  grow; decoders `resize` to the count).
- `SpriteComponent::framesRect`, `PointsComponent::framesRect`: `GrowArray<SpriteFrameData>`.
- `SpriteAnimationComponent::frames`, `framesTime`: `GrowArray<int>`.
- `TilemapComponent::tilesRect`: `GrowArray<TileRectData>`; `tiles`: `GrowArray<TileData>`.
- Editor-only copies of a palette (`editor/animation/AnimationClipController.cpp:70`,
  `editor/pose/PoseEditSession.h:203`): `GrowArray<Matrix4>`.
- `engine/core/Engine.h:88-130`: remove `MAX_SUBMESHES`, `MAX_TILEMAP_TILESRECT`, `MAX_TILEMAP_TILES`,
  `MAX_SPRITE_FRAMES`, `MAX_BONES`, `MAX_EXTERNAL_BUFFERS`. Keep `MAX_MORPHTARGETS`, `MAX_MESH_LODS`,
  `MAX_TEXT_FONTS` and the rest (fixed by shaders or tiny).

### 6.3 Call sites (the audit D6 forces)

- Every `x.size()` on these members: a loop bound or comparison becomes the count field (`numSubmeshes`,
  `numExternalBuffers`, `numFramesRect`, `numTilesRect`, `numTiles`, `framesSize`, `framesTimeSize`); a "how many
  elements exist" becomes `stored()`; a capacity check is deleted. Known sites: section 4 F5 and F7, plus
  `Catalog.cpp:375`, `ClipValidation.cpp:619`, `LatticeBake.cpp:374,400,600,631,654`, `PoseViewport.cpp:130,866`,
  `EntityCloner.cpp:155`, `VatAssetWriter.cpp:328`, `VatBakeCompute.cpp:545`, `VatContactSheet.cpp:194`,
  `LatticeRender.cpp:219` (max over the palette: `stored()`), `MeshPose.cpp:327-336`, `CpuSkin.cpp:658`,
  `RenderSystem.cpp:130,8457,14942,14951`, `Project.cpp:3618`. (Some of these iterate other types' `submeshes`,
  such as `std::vector` layouts in CpuSkin and VAT; the compiler tells them apart.)
- Capacity branches deleted (dead once storage grows): `MeshSystem.cpp:5336-5364` (`reportOverCapacity` and its
  callers `:5361-5364`, `:5519-5522`), `:2096-2099` and `capacityReached` through `loadGLTFBuffer`'s callers,
  `:2694-2696`, `:5541-5550` (replaced by 6.4), `:6547-6551`; `Tilemap.cpp:88-91`.
- `validIndex` checks that guard an index from outside stay and now mean D3: `Sprite.cpp:181,196`,
  `Points.cpp:292,311`, `Tilemap.cpp:83,207`, `SpriteAnimation.cpp:26,36`, `ActionSystem.cpp:410`,
  `ComponentHooks.cpp:92,107,121`, `StreamComponents.cpp:168,1559,1568`. Their messages say
  "index N is past the limit of 65,536" instead of "max ... is 127".
- Bounds that meant "a submesh of this mesh": `VideoPlayerSystem.cpp:222,621`, `PropertyBindings.cpp:288,297,1373`,
  `SpriteNormals*` sites (F7): compare with `numSubmeshes` / `numFramesRect`.
- `std::reverse(mesh.submeshes.data(), ...)` (`MeshSystem.cpp:6053,6405,6753`): `mesh.submeshes.reverse(0, n)`.
- `mesh.bonesMatrix = HybridArray<Matrix4, MAX_BONES>()` (`MeshSystem.cpp:5102,5513`,
  `StreamComponents.cpp:166`): `mesh.bonesMatrix.clear()`.
- `RenderSystem::destroyMesh` (`RenderSystem.cpp:10717`): destroys the render objects of every stored submesh, not
  only the first `numSubmeshes` (a submesh left past the count by a smaller reload keeps no pipelines). A model load
  that lowers `numSubmeshes` after `destroyMesh` calls `submeshes.resize(numSubmeshes)` so the storage follows.

### 6.4 Bone palette

- `MeshSystem::loadGLTF`: where it resets the palette (`:5102`, `:5513`) it `clear()`s it; for a node with a skin
  it `resize(joints.size())` (identity matrices) instead of `bonesMatrix[lastBone].identity()` and the capacity
  error (`:5541-5551`). A rigid mesh keeps an empty palette.
- Writers never grow a palette: `AnimatorSystem.cpp:500` (`j < count && j < stored()`), `RenderSystem.cpp:14411`
  (joint loop bound `stored()`), `RenderSystem.cpp:15135` (a BoneComponent whose index is past the palette is
  skipped and logged once per entity: an authoring error, not new content). So the palette size changes only with
  a model load, which reloads the mesh.
- `bindSkinningResource` (`RenderSystem.cpp:128-145`): rows = `max(stored(), 1)`; a palette resource whose rows
  differ from `bonesUploadRows` is destroyed and created again with the new rows; sets `bonesUploadRows`.
- `updateMeshBuffers` (`:8452-8463`): uploads `bonesUploadRows` rows; when `stored()` differs from it (a decode or
  cooked read changed a loaded mesh) it sets `needReload` and uploads nothing this frame.
- Skins over the device's maximum texture height on the texture path (2,048 rows at least on GLES3/WebGL2) fail
  that mesh's load with an error naming the joint count and the device limit (`loadFailed`, as a full pool does).
- Effect on `draw-perf`'s finding `859ab7d9d`: the fallback scan over `bonesMatrix` (`RenderSystem.cpp:14950-14957`)
  loops zero times on a rigid mesh.

### 6.5 `SokolObject` (`engine/renders/sokol/SokolObject.{h,cpp}`, `SokolPipelineCache.{h,cpp}`)

Fields removed: `sg_pipeline_desc pipeline_desc`, `sg_pipeline_desc baseDesc`, `uint32_t warmedPipeline[16]`,
`uint64_t warmKey[16]`. Fields added:

```cpp
// from beginLoad to endLoad / endLoadDynamic only: what setShader, setIndex and addAttribute write
std::unique_ptr<sg_pipeline_desc> loadDesc;
// after a load: the base description (shader, layout, index type, primitive type), shared by every object
// loaded with an equal one (SokolPipelineCache::internBase); null before a load and after destroy
std::shared_ptr<const sg_pipeline_desc> base;
// base->shader, read by every draw without following the pointer
sg_shader shader;
// GL warm draws (spec S32): per pipeline type, the pipeline confirmed warm and the warm draw asked for;
// allocated on the first isWarm that needs one (never on backends without warm draws)
struct WarmTable { uint32_t pipeline[PIPELINE_TYPES]; uint64_t key[PIPELINE_TYPES]; };
std::unique_ptr<WarmTable> warm;
```

- `beginLoad`: `loadDesc = std::make_unique<sg_pipeline_desc>()` (zeroed), `base.reset()`, primitive type set.
  `setShader`/`setIndex`/`addAttribute`/`addInterleavedAttribute`: write `*loadDesc` (created if absent, as
  Doriax's `loadingDesc()`).
- `endLoad(types, state, samples)`: `checkVertexInputs()` on `*loadDesc`; `base = SokolPipelineCache::internBase(*loadDesc)`;
  `shader = base->shader`; the pipelines from `makePipelineDesc(type)`, which now starts from `*base`;
  `loadDesc.reset()`.
- `endLoadDynamic`: copy of `*loadDesc` with primitive and index type set, interned as `base`;
  `dynamicBaseHash = baseHash(*base)`; `loadDesc.reset()`. `beginDraw(const PipelineState&)` passes `*base`.
- `beginDraw(PipelineType)`: the shader check (`:1040`) reads `shader`; a variant's first use (`:1055`) builds from
  `*base`. `isWarm`/`isDynamicWarm` read `shader`. `prewarmWith`: nothing to do without `base`.
- `isWarm(pipeline, index)`: returns true at once when `immediatePipelines || !warmDrawsActive()`; otherwise
  allocates `warm` if absent and uses `warm->pipeline[index]`, `warm->key[index]`. `resetWarm` frees it.
- `destroy`: `loadDesc.reset(); base.reset(); shader = {}; warm.reset();` with the rest as today.
- Copy constructor and assignment become explicit: deep-copy `loadDesc` and `warm`, share `base`, the rest as the
  defaulted copy did (`SokolVariantRefs` still takes its own references).
- Labels: interned bases carry `label = nullptr` (`descEqual` ignores labels); the pipeline-making code sets its
  label where it creates a pipeline (`SokolPipelineCache::get` sets "dynamic-pipeline", today taken from
  `baseDesc.label`).
- Diagnostics: `static int SokolObject::getLoadingDescCount()` (live `loadDesc`s, as `getLivePipelineCount`) and
  `ObjectRender::getLoadingDescCount()` forwarding it.

`SokolPipelineCache::internBase(const sg_pipeline_desc&) -> std::shared_ptr<const sg_pipeline_desc>` and
`internedBaseCount()`:
- A pool `unordered_map<uint64_t, std::vector<std::weak_ptr<const sg_pipeline_desc>>>` keyed by `descHash`, under its
  own mutex (loads also run on the editor's thumbnail thread and async-create workers); function-static and leaked
  like the cache's other maps (`SokolPipelineCache.cpp:57-68`).
- A hit is a `lock()`ed entry with `descEqual`; a miss stores a copy (label null) whose `shared_ptr` deleter erases
  its expired entry under the mutex.
- Deadlock rule: a `lock()`ed pointer that does not match may be the last reference when it goes out of scope, and
  its deleter takes the mutex. Such pointers are moved to a local vector destroyed after the lock is released.
- A `memory::TrackedBytes` record `Render.PipelineDescs` (Cpu): `sizeof(sg_pipeline_desc)` and one item per
  interned base, added on insert, subtracted in the deleter.

A base keeps the shader id it was made with. Equal ids from a later render context (sokol reuses ids) may meet an
old base: the content is equal, so sharing it is correct; objects of a dead context are reloaded anyway.

### 6.6 Serialization and reflection

- YAML (`editor/StreamComponents.cpp`): every decode loop runs over the node's elements up to `ARRAY_INDEX_LIMIT`
  (an error past it), writes through `operator[]` and sets the count; `resize(count)` keeps the invariant when
  null entries are skipped (`:383-392`, `:446-470`, `:2228`). `bonesMatrix` decode (`:163-171`): `clear()` then the
  listed matrices. The encode of the palette (`:56-71`) loses its `count <= MAX_BONES` branch: trailing identities
  are trimmed always (a model-backed mesh is not encoded with bones; others write what they posed). Encode reads
  only below the counts, through `const operator[]` (defined past `stored()`, F5).
- Cooked scenes (`engine/core/serialization/CookCodecs.h:303-320`, `:352-355`): `Codec<GrowArray<T>>` and
  `Codec<StableArray<T>>` write `stored()` elements and read every element up to `ARRAY_INDEX_LIMIT` (past it they
  are read into a dropped element, as today's non-editor branch does). `TextureVisit` for both. The record layout
  (varint count, elements) is the same as `HybridArray`'s, so `RuntimeFields.gen.inc` is unchanged; regenerate it
  with `jasperengine-editor reflect --emit-runtime-fields` to check.
- Reflection (`editor/reflect/TypeOps.h:556-582,633-640`): `ops::Grow<T>`/`GrowArrayOps<T>` and
  `ops::Stable<T>`/`StableArrayOps<T>` replace `ops::Hybrid`; `ReflectTraits` for both. `ArrayOps` gets
  `bool appendsByIndex` (true for both): `editor/reflect/Resolve.cpp:135` accepts `index == size` on such arrays
  (the write grows it) and refuses `index > size` or past `ARRAY_INDEX_LIMIT` (`PathError::IndexOutOfRange`, whose
  message names the array's element count). `Resolve.cpp:187` and `Properties.cpp:76,129` read below `size`.
  Equality as in 6.1.
- Scene `maxValues` (D8): `SceneProject::maxValues` becomes `std::optional<YAML::Node> legacyMaxValues`, read
  verbatim (`Stream.cpp:4100-4115`) and written back verbatim (`:3830-3843`); `SceneMaxValues`,
  `calculateSceneMaxValues`, `countGltfExternalBuffers` and the save-time copy (`Project.cpp:7199-7207`) are
  deleted. `StreamSchema.cpp:60` keeps the `maxValues` key. Templates (`editor/resources/templates/*/Main.scene`)
  drop the node.

### 6.7 Export and players

- Delete `Exporter::getSceneMaxValues`, `sceneMaxValuesOf`, `buildSceneMaxValuesDefinitions` and their uses
  (`Exporter.cpp:1337-1339` cache key, `:3640-3644` marker, `:5581-5584` Xcode definitions), the marker and its
  comment (`engine/CMakeLists.txt:204-208`).
- `chooseBuildMethod` (`ExporterPrebuilt.cpp:161-180`): the capacity block becomes the D10 refusal (a player whose
  `capacities` is non-empty). `PlayerInfo::capacities` stays for reading old players.
- `tools/players/package_player.py`: delete `CAPACITIES`, `capacities()` and the `capacities` key (`:51-52,73-80,
  240`); its docstring (`:11`) updated.
- The compile-cache key changes once (one rebuild of each compile cache): say so in `docs/shipping.md`.

### 6.8 Heap trim (`engine/core/memory/HeapTrim.{h,cpp}`)

```cpp
namespace jasperengine::memory {
    // Gives the allocator's free pages back to the operating system where it can (glibc Linux, Apple,
    // Android 9+); false where it does nothing (web, Windows, other allocators). Thread-safe; slow (it walks
    // the heap): call requestHeapTrim instead from the main thread.
    JASPERENGINE_API bool releaseFreeHeap();
    // One Background-lane job runs releaseFreeHeap; a request while one is queued or running is merged into it
    JASPERENGINE_API void requestHeapTrim();
    JASPERENGINE_API uint64_t heapTrimCount();   // completed trims (tests, diagnostics)
}
```

- glibc (`__linux__ && __GLIBC__`, not Android): `malloc_trim(0)`.
- `__APPLE__`: `malloc_zone_pressure_relief(nullptr, 0)` from `<malloc/malloc.h>`.
- `__ANDROID__`: `dlsym(RTLD_DEFAULT, "mallopt")` once; called with `M_PURGE` from `<malloc.h>` when the device API
  level (`android_get_device_api_level()`) is 28 or more; no-op otherwise. `M_PURGE`'s availability in the NDK of the
  build image is to be checked by the implementer (from knowledge, not read here); without the macro the Android
  branch is a no-op.
- Emscripten, Windows, other: return false.
- Each trim is a profiler zone `Memory.HeapTrim`; its duration is logged at debug level.
- Callers: end of `SceneManager::updateLoading` after `cancelPreload` (`SceneManager.cpp:763`), the end of
  `editor::Project::finalizeStop` (`Project.cpp:6147`), the end of `editor::Project::closeScene`
  (`Project.cpp:4704`) once the scene is deleted.
- Without job workers (single-thread mode, web) `requestHeapTrim` runs nothing on the web and runs the trim inline
  elsewhere (the job system runs it from `pump()`).
- New source in `engine/CMakeLists.txt`'s list (`core/memory/...`) and in the Xcode project.

### 6.9 Editor UI (humans)

- Inspector: Add Frame (Sprite, Points), Add Tile Rect, Add Tile and the tile-from-rect button work past the old
  capacities. Add Tile, the Scene view tile drop and Duplicate Tile are disabled at `MAX_TILEMAP_RENDERED_TILES`
  with the tooltip "A Tilemap draws at most 16,383 tiles". The other Add buttons are disabled only at
  `ARRAY_INDEX_LIMIT` ("At most 65,536 frames"). All undoable as today (`MultiPropertyCmd`).
- Systems window footer (`editor/window/SystemsWindow.cpp:192-215`): "ECS memory: X (+ Y in component arrays)"; the
  "Components per type" list shows "MeshComponent: 120 (1.6 KiB each, 780 KiB in arrays)".
- Window > Analysis > Jobs & Memory lists `Render.PipelineDescs` (automatic, a memory record).
- Build Settings' Build Method row loses capacity reasons and gains the D10 reason when it applies.
- Sprite Normals window and tools: the "a Sprite holds at most 128 frames" refusals go (frame lists up to the limit).

### 6.10 MCP (agents)

- `get_ecs_stats` (`editor/mcp/tools/EcsTools.cpp:118-162`): each type gains `element_bytes` (`componentSize()`) and
  `heap_bytes`; `bytes` gains `heap`; the text summary adds the heap. `heap_bytes` comes from a new virtual
  `ComponentArrayBase::arrayHeapBytes()` (default 0) that sums a per-type trait `ComponentHeapBytes<T>`
  (`engine/core/component/ComponentHeapBytes.h`, specialised for `MeshComponent`, `SpriteComponent`,
  `PointsComponent`, `SpriteAnimationComponent`, `TilemapComponent`: the `storageBytes()` of their arrays and, for
  meshes, `stored() * sizeof(Submesh)`). Computed when called (one pass over the type's components), nothing at
  runtime. Description stays within `tools/mcp/budget.json` (`tool_budget.py --check`).
- `get_memory_stats`: the `Render.PipelineDescs` record (no tool change).
- `edit_tilemap` (`WorldTools.cpp:2431`): the tile refusal uses `MAX_TILEMAP_RENDERED_TILES` ("The tilemap would hold
  N tiles; a Tilemap draws at most 16,383."); the area guard (`:2680`) uses 4 x that; the tile-rect refusal and
  slice limit (`:2476,2521`) use `ARRAY_INDEX_LIMIT`.
- `set_sprite_frames`, `set_sprite_animation` (`:2833,2855,2974`): refusals at `ARRAY_INDEX_LIMIT` only.
- Property paths (`set_component_property` and the like): appending at `index == element count` works; past it the
  error says "tiles has 12 elements: write index 12 to add one".
- Regenerate `docs/mcp/TOOLS.md` (`tools/mcp/gen_tool_docs.py`). Guide: `docs/mcp/guide/ecs.md` (the heap fields,
  how to read a mesh's cost), `docs/mcp/guide/shipping.md:14` (no capacity reason; the D10 one).

### 6.11 Lua

No new API (D11). `tools/lua_api` descriptions of `Sprite:addFrame`, `Points:addFrame`, `Tilemap:addRect`,
`Tilemap:addTile` that mention a maximum are updated to the index limit (the build regenerates the API data).

### 6.12 Docs

- `docs/shipping.md:19`: drop the capacity bullet (and the nonexistent "Project Settings > Scene Limits"); add D10
  and the one-time compile-cache rebuild.
- `docs/jobs-memory.md` Memory section: `Render.PipelineDescs`, the heap trim (where, per platform).
- `docs/upgrading-from-main.md`: C++ scripts that used `MAX_SUBMESHES` and the other five macros no longer compile;
  use the count fields. Scene files keep their `maxValues` block unread.
- `docs/scripting.md` or the Lua reference wherever an index limit of these arrays is stated (grep at
  implementation).

### 6.13 Platforms

All changes except 6.8 are portable C++20 inside `tools/check-cxx-subset.py`'s subset and touch no backend code
beyond `SokolObject`/`SokolPipelineCache`, which are backend-neutral sokol calls: GL 3.3, GLES3/WebGL2, Metal,
D3D11 and Vulkan behave as before. Windows, macOS and iOS cannot be built here: the implementer keeps them
compiling by reading (MSVC: no designated-initialiser or `<bit>` surprises in the containers; the Xcode project
`engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj` gets `GrowArray.h`, `StableArray.h`,
`ComponentHeapBytes.h`, `HeapTrim.{h,cpp}` and loses `HybridArray.h`). Android's `mallopt` lookup follows
`MediaCodecBackend.cpp`'s `dlsym` pattern.

## 7. Behaviour rules and edge cases

1. A write to `submeshes[i]` for `i >= stored()` allocates the missing submeshes (default); a const read returns the
   shared default and allocates nothing. `numSubmeshes` is never changed by the container.
2. A pointer or reference to a `Submesh` or `ExternalBuffer` stays valid while its array grows; one into a
   `GrowArray` does not (as in the editor today). Engine code that keeps a reference across a write past the end
   of a `GrowArray` is a bug; the audit (6.3) checks the palette loops (`AnimatorSystem`, `RenderSystem:14411`),
   which never write past `stored()` after 6.4.
3. Storage never shrinks by itself: removing the last frame keeps the slot (reset to default, as `Sprite::removeFrame`
   does, `Sprite.cpp:210-219`); `MeshSystem` shrinks submesh storage only after `destroyMesh` (6.3).
4. Two components that differ only in trailing default elements compare equal (prefab overrides do not appear from
   high-water storage).
5. A decoded or cooked count over `ARRAY_INDEX_LIMIT`: the elements past it are dropped with one error naming the
   component, the array and the limit.
6. A skinned mesh with an empty palette (its model not loaded yet) is not drawn skinned until the model load gives
   it one; `bindSkinningResource` with 0 rows makes 1 identity row, so nothing reads outside the resource.
7. A model reload whose skin changes joint count: the palette is resized by the load, the mesh reloads, the GPU
   palette is made again with the new rows (6.4).
8. Objects of the same base description share it across scenes and the editor's thumbnail renderer; a base lives as
   long as one object holds it.
9. An object whose load starts but never ends keeps its `loadDesc` until its next `beginLoad` or `destroy`.
10. The heap trim never runs on the main thread, never twice at once, and does nothing when nothing was freed.
11. An old scene with `maxValues` saves it back unchanged; a scene created now has no `maxValues`.
12. A mismatched old player (D10) is never chosen for Prebuilt.

## 8. Performance and cost

- Memory per mesh (estimates, libstdc++, to be replaced by the measured values of `MeshMemory.SizeofCeilings`):

| | Today | After |
|---|---|---|
| `SokolObject` | 2,528 B | about 760 B |
| `Submesh` | 11,776 B | about 6,500 B |
| `MeshComponent` inline | 204,840 B | about 1,600 B |
| one-submesh Sprite (component + arrays) | 204,840 B | about 8,100 B |
| 3-submesh rigid model mesh | 204,840 B | about 21,000 B |
| `SpriteComponent` | about 6,300 B | about 200 B + 48 B per frame |
| `TilemapComponent` | about 126 KB | about 300 B + 56 B per tile and per rect |
| pipeline descriptions, 5,000 equal sprites | 5,000 x 3 x 1,616 B = 24 MB | a few x 808 B |

- First-page cost of the paged component storage drops with `sizeof(MeshComponent)` (64 x 1.6 KB instead of
  64 x 205 KB).
- CPU per frame: unchanged within noise. A non-const `submeshes[i]` read gains one predictable branch and one
  pointer load (the editor already pays the branch); a variant's first use reads `*base`. GL warm tables cost one
  allocation per warming object, on GL with a worker context only.
- CPU per load: one 808 B allocation per `beginLoad`, a hash and a mutex per `endLoad`. The implementer checks
  (a counter of `beginLoad` calls per frame in a 5,000-sprite scene and the fixture's 3D scene, read with
  `get_render_stats` frames apart) that no object loads every frame; if one does, that object keeps its `loadDesc`
  buffer for reuse instead of freeing it, recorded in a Deviations section of this spec.
- Instantiating and copying meshes (prefab instantiate, undo snapshots) copies 1 to N submeshes instead of 16.
- Disabled or unused: a rigid mesh has no palette, no palette texture, no warm table. The heap trim runs only after
  a load, a Stop or a scene close, on a background worker, and costs nothing on the web and Windows.

## 9. Compatibility

- Scene and prefab YAML: unchanged (submesh lists, frames, tiles as today). Old scenes decode every element (the
  fix). `maxValues` kept as read (D8). No version bump.
- Cooked scenes: same record layout; an old cook (16 submeshes written) reads into 16 stored submeshes, a new cook
  writes what is stored. `contentFormatVersion` unchanged.
- Generated scene code (Compile builds): unchanged statements; it no longer receives `-DMAX_*`.
- C++ scripts that used the six macros or `HybridArray` stop compiling (documented in `docs/upgrading-from-main.md`).
- Prebuilt players of older versions are refused (D10); players of this version accept any content.
- Compile caches rebuild once (key change).

## 10. Tests

Red controls are runs of the new test against `main` before the change; each test says what fails there.

Core (`jasperengine-core-tests`, non-editor containers):
- `engine/tests/core/component/MeshMemoryTest.cpp`
  - `MeshMemory.SizeofCeilings`: `sizeof(SokolObject) <= 1024`, `sizeof(ObjectRender) <= 1088`,
    `sizeof(Submesh) <= 7168`, `sizeof(MeshComponent) <= 2048`, `sizeof(SpriteComponent) <= 512`,
    `sizeof(TilemapComponent) <= 512`; the implementer tightens each to the measured Linux size rounded up to
    the next 128 B plus 10 %, never above these. **Red:** `sizeof(MeshComponent)` about 204,840.
  - `MeshMemory.SpriteCostsATwentiethOfBefore`: a mesh made by `MeshSystem::prepareSpriteMesh` has
    `sizeof(MeshComponent) + ComponentHeapBytes<MeshComponent>::of(mesh) <= 10240`, `bonesMatrix.stored() == 0`,
    `submeshes.stored() == 1`. **Red:** the inline size alone.
- `engine/tests/core/util/GrowArrayTest.cpp`, `StableArrayTest.cpp`: grow on write; const read past the end returns
  the default and does not grow; `validIndex` at 65,535 and 65,536; deep copy, stealing move; `StableArray` pointers
  survive growth from 1 to 1,000; `reverse` swaps; equality with trailing defaults; `resize` frees.
- `engine/tests/core/render/ModelCapacityGpuTest.cpp` reworked (file kept, header comment rewritten):
  `ModelCapacityGpu.FlattenedBakeKeepsEveryPrimitive`: the off-main-thread flatten of a model with 40 primitives
  and more than 30 buffers draws every primitive (pixel coverage equal to the main-thread child-mesh path within
  the file's tolerance); glcore and Vulkan. **Red:** primitives past 16 are left out.
- `engine/tests/core/render/PipelineDescPoolGpuTest.cpp`:
  - `PipelineDescPoolGpu.EqualMeshesShareOneBase`: 200 equal boxes loaded and drawn raise `internedBaseCount()` by at
    most the number of distinct passes (color, depth, G-buffer: 3), and destroying them returns it.
  - `PipelineDescPoolGpu.NoLoadDescriptionAfterDraw`: after a scene of meshes, sprites, a tilemap and particles has
    drawn one frame, `ObjectRender::getLoadingDescCount() == 0`.
  Existing guards that must stay green: `SharedMeshBuffersTest.MirroredCopiesSharingBuffersDrawWithTheReversedWindingVariant`
  (`engine/tests/htmlui/TestSharedMeshBuffers.cpp:866`, variants from the shared base), `AsyncCreateTest`
  (`engine/tests/core/render/AsyncCreateTest.cpp:660-683`, warm draws in worker mode).
- `engine/tests/core/render/SkinPaletteGpuTest.cpp`:
  - `SkinPaletteGpu.PaletteSizedToTheSkin`: a skinned glTF the test writes with 70 joints gets
    `bonesMatrix.stored() == 70` and draws posed like `MeshPose.MatchesGpuSkinning` expects; a box has an empty
    palette and no palette texture or buffer.
  - `SkinPaletteGpu.SkinOverTheOldLimitLoads`: the same with 200 joints loads and draws. **Red:** "more bones than the
    maximum allowed (128)", load fails.
- `engine/tests/core/memory/HeapTrimTest.cpp`:
  - `HeapTrim.ReturnsFreedPagesOnGlibc` (Linux glibc only, else expects `releaseFreeHeap() == false` where 6.8 says
    no-op): allocate 64 MiB in 4 KiB blocks, free them, `releaseFreeHeap()`, resident size from
    `/proc/self/statm` falls by at least 32 MiB. **Red:** without the trim (main has no call) it does not fall.
  - `HeapTrim.RequestsMerge`: ten `requestHeapTrim()` calls in a row complete as one or two trims (`heapTrimCount`).
- `editor/tests/engine/manager/SceneManagerHeapTrimTest.cpp` (scene loads are driven through editor scenes, as in
  `editor/tests/PlayStartupScenesTest.cpp`): `SceneManagerHeapTrim.LoadEndRequestsATrim`: a completed load raises
  `heapTrimCount()` by one once the jobs finish. **Red:** no trim on main (the counter stays).

Editor (`jasperengine-editor-tests`):
- `editor/tests/stream/StreamNoCapacityTest.cpp`
  - `StreamNoCapacity.MeshKeepsEverySubmesh`: a mesh node of 20 submeshes, submesh 19 with `overrideFields` and a
    `baseColorFactor` edit, decodes to 20 stored submeshes with the edit, and encodes back equal; clean under
    `SANITIZE=address,undefined`. **Red:** submesh 19 is default after decode (and ASan reports the encode's read
    past the vector).
  - `StreamNoCapacity.SpriteTilemapPointsKeepEveryElement`: 130 sprite frames, 201 tile rects, 2,049 tiles, 130
    points frames, 130 sprite-animation frames round-trip. **Red:** 128/200/2,048/128 kept.
  - `StreamNoCapacity.IndexLimitDropsAndLogs`: 65,537 frames keep 65,536 and log one error.
- `editor/tests/util/ProjectUtilsTileTest.cpp` (new; no test calls `buildDuplicateTileCmd` today):
  `ProjectUtilsTile.DuplicateTilePastTheOldCapacity`: a tilemap with 2,048 tiles gets a non-null duplicate command that
  executes and undoes. `...RefusedAtTheDrawLimit`: at 16,383 tiles it returns null. **Red:** null at 2,048.
- `editor/tests/reflect/ResolveAppendTest.cpp`: `ReflectResolve.IndexAtTheCountAppendsToAGrowArray` (a sprite with
  128 frames: `framesRect[128].name` resolves and grows), `...PastTheCountIsOutOfRange` (`framesRect[130]`).
  **Red:** `framesRect[128]` is out of range on main.
- `editor/tests/export/BuildMethodTest.cpp`: `CapacitiesAboveThePlayersCompile` is replaced by
  `BuildMethod.ContentSizeNeverForcesCompile` (a scene with a 200-joint mesh and a 24-submesh mesh: Prebuilt) and
  `BuildMethod.PlayerWithCapacitiesIsRefused` (D10 reason). **Red:** the first gets "MAX_BONES 200 (the player holds
  128)" on main.
- `editor/tests/stream/SceneMaxValuesTest.cpp`: `SceneMaxValues.OldBlockSavedUnchanged` (a fixture with `maxValues`
  saves byte-identical), `SceneMaxValues.NewSceneWritesNone`.
- `editor/tests/engine/subsystem/GltfMeshOffsetTest.cpp:153-154,173,200`: the `countGltfExternalBuffers`
  expectations go with the function.

MCP (`tools/mcp/tests/`):
- `test_world.py::test_tilemap_past_old_capacity`: `edit_tilemap` with 2,100 tiles succeeds and `get_tilemap`
  returns 2,100; 16,384 is refused with the draw-limit message. **Red:** "the maximum is 2048".
- `test_world.py::test_sprite_frames_past_old_capacity`: 200 frames via `set_sprite_frames`. **Red:** refused at 128.
- `test_ecs.py::test_mesh_component_memory`: a scene with 200 sprites: `MeshComponent` `bytes / count <= 4096`,
  `element_bytes <= 2048`, `heap_bytes / count <= 8192`. **Red:** `bytes / count` about 260,000 (pages of 64).
- `test_jobs_memory.py::test_pipeline_desc_record`: after a scene of 200 equal sprites draws,
  `get_memory_stats {prefix: "Render.PipelineDescs"}` has a record with `count <= 10`.
- `test_build.py:118`: expects none of the six `MAX_*=` definitions in the Xcode settings. **Red:** present.
- `test_reflection.py:144-157`: `ICON_MAX_VALUES`/`max_values_as_now` replaced by dropping `maxValues` from the
  goldens' comparison of newly created scenes.
- Each changed tool's group stays hidden until `tool_groups` enables it (existing tests in `test_tool_groups.py`).

Gates: `tools/check-changed` per commit; the sanitizer run (`SANITIZE=address,undefined tools/build.sh unit-tests`)
once on the branch because the containers and copies change ownership; `GRAPHIC_BACKEND=vulkan UNIT_SUITE=core`
for the GPU suites; `tools/smoke-test.sh` (export, build, run) once, since exports change.

## 11. Acceptance criteria

1. `engine/core/util/HybridArray.h` and the six `MAX_*` capacity macros are gone (`git grep` finds none outside
   docs history and `docs/superpowers`); `GrowArray`/`StableArray` are the only array containers of components.
2. `MeshMemory.SizeofCeilings` and `MeshMemory.SpriteCostsATwentiethOfBefore` pass on Linux, and the CPU suites pass
   on Windows and macOS CI; the measured sizes are recorded in the branch's report next to today's.
3. Every red control of section 10 was run on `main` and failed for the reason given; each passes on the branch.
4. A scene with a 20-submesh model whose submesh 19 was edited, a 6,000-tile tilemap and a 200-frame sprite saves,
   reopens and plays (in the editor and in a Prebuilt Linux build) with all content: checked by the MCP tests and
   one smoke build.
5. A 200-joint skinned model loads and animates in Play and in a Prebuilt build.
6. `get_ecs_stats` reports `element_bytes` and `heap_bytes`; `get_memory_stats` lists `Render.PipelineDescs`;
   `docs/mcp/TOOLS.md` regenerated, `tool_budget.py --check` passes.
7. The Systems window shows the per-type and total array memory; Add Tile is disabled at 16,383 with its tooltip.
8. `chooseBuildMethod` never gives a capacity reason; a player with `capacities` gets the D10 reason.
9. An old scene with `maxValues` saves its block unchanged; a new scene has none.
10. After Play Stop on Linux the editor's RSS falls (recorded once in the report with the numbers; the unit test
    `HeapTrim.ReturnsFreedPagesOnGlibc` is the check).
11. No per-frame cost: the frame time of a 5,000-sprite scene and of the fixture's 3D scene (`get_render_stats`)
    stays within 3 % of `main` on the same server, recorded in the report.
12. ASan/UBSan unit-test run clean for the touched suites; Vulkan GPU suites green.

## 12. Files likely touched

Engine: `engine/core/util/{GrowArray.h,StableArray.h}` (new), `engine/core/util/HybridArray.h` (deleted),
`engine/core/Engine.h`, `engine/core/component/{MeshComponent.h,SpriteComponent.h,PointsComponent.h,
SpriteAnimationComponent.h,TilemapComponent.h,ComponentHeapBytes.h (new)}`, `engine/core/ecs/ComponentArray.h`,
`engine/renders/sokol/{SokolObject.h,SokolObject.cpp,SokolPipelineCache.h,SokolPipelineCache.cpp}`,
`engine/core/render/{ObjectRender.h,ObjectRender.cpp,CpuSkin.cpp}`, `engine/core/subsystem/{MeshSystem.cpp,
RenderSystem.cpp,AnimatorSystem.cpp,ActionSystem.cpp,VideoPlayerSystem.cpp}`, `engine/core/object/{Sprite.cpp,
Points.cpp,Tilemap.cpp}`, `engine/core/action/SpriteAnimation.cpp`, `engine/core/animation/PropertyBindings.cpp`,
`engine/core/lattice/LatticeRender.cpp`, `engine/core/picking/{MeshPose.cpp,PickGeometry.h}`,
`engine/core/registry/EntityCloner.cpp`, `engine/core/serialization/{CookCodecs.h,codecs/ComponentHooks.cpp,
RuntimeFields.gen.inc (regenerated)}`, `engine/core/memory/HeapTrim.{h,cpp}` (new),
`engine/core/manager/SceneManager.cpp`, `engine/CMakeLists.txt`, the Xcode project.

Editor: `editor/StreamComponents.cpp`, `editor/Stream.cpp`, `editor/StreamSchema.cpp` (unchanged key, check),
`editor/Project.{h,cpp}`, `editor/Exporter.{h,cpp}`, `editor/ExporterPrebuilt.cpp`, `editor/players/PlayerRuntimes.*`,
`editor/reflect/{TypeOps.h,Resolve.cpp,Properties.cpp}`, `editor/util/ProjectUtils.cpp`,
`editor/window/{InspectorWindow.cpp,InspectorAnimation.cpp,SceneWindow.cpp,SystemsWindow.cpp,SpriteNormalsWindow.cpp,
SpriteNormalsEntry.h}`, `editor/mcp/tools/{WorldTools.cpp,EcsTools.cpp,SpriteNormalsTools.cpp}`,
`editor/spritenormals/{SpriteNormalsApply.cpp,SpriteNormalsSession.cpp,SpriteNormalsCli.cpp,SpriteNormalsCheck.cpp}`,
`editor/animation/{AnimationClipController.cpp,ClipValidation.cpp}`, `editor/pose/{PoseEditSession.h,PoseViewport.cpp}`,
`editor/lattice/LatticeBake.cpp`, `editor/vat/{VatAssetWriter.cpp,VatBakeCompute.cpp,VatContactSheet.cpp}`,
`editor/Catalog.cpp`, `editor/resources/templates/*/Main.scene`.

Tools, tests, docs: `tools/players/package_player.py`, the tests of section 10, `docs/shipping.md`,
`docs/jobs-memory.md`, `docs/upgrading-from-main.md`, `docs/mcp/guide/{ecs.md,shipping.md}`, `docs/mcp/TOOLS.md`,
`tools/lua_api` descriptions.

Hot files for merge planning: `SokolObject.{h,cpp}`, `RenderSystem.cpp`, `MeshSystem.cpp`, `MeshComponent.h`,
`StreamComponents.cpp`, `Stream.cpp`, `Project.cpp`, `Exporter.cpp`, `ExporterPrebuilt.cpp`, `WorldTools.cpp`,
`InspectorWindow.cpp`.

## 13. Cross-feature dependencies

- `export-pipeline`: its item "capacity counting for script-only prefabs" (Doriax `b1df76163`) becomes moot: there
  is nothing to count once this lands. If `export-pipeline` lands first and implements it, this branch deletes it
  with `sceneMaxValuesOf`. Both touch `Exporter.cpp` and `ExporterPrebuilt.cpp::chooseBuildMethod`.
- `mobile-shipping`: removes the AdMob reason (d) from the same `chooseBuildMethod` and edits
  `package_player.py` and the Xcode project. Trivial textual merges; whoever lands second rebases.
- `draw-perf`: `d103cae0c` (skip redundant applies) edits `SokolObject::beginDraw`/`draw` — the same functions as
  6.5; land one, rebase the other, keep the S32 checks. `859ab7d9d` (bone bounds only on skinned meshes) edits
  `RenderSystem.cpp:14936-14957`; after this spec the scan is empty on rigid meshes, its gate is still worth having.
- `sprite-batching`: edits `SokolObject.h` (`BufferInfo`), `SpriteComponent.h` and sprite paths in `MeshSystem.cpp`.
  A batch key can use the interned `base` pointer (equal pointer, equal base description): offered, not required.
- `render-correctness`: edits `GltfMeshOffsetTest.cpp` (`:192`, stretched bounds) where this spec removes the
  `countGltfExternalBuffers` lines, and `MeshSystem::loadGLTF`/`RenderSystem::loadMesh` (normals, bounds).
- `scene-integrity`: edits scene save/load in `Project.cpp` and `Stream.cpp` near the `maxValues` code (6.6).
- `runtime-core`: edits `EntityCloner.cpp` (`:136`, hidden children) next to `:155` here.
- `scripting`: its Model Lua API over child mesh parts reads `submeshes`; use the counts, never `stored()`.
- `agent-authoring`: may edit `WorldTools.cpp`; the tilemap and sprite refusals here are small hunks.
- No dependency on the water, UI, platform, unicode, build-toolchain, project-window, ssr-probes or code-editor-git
  features.

## Deviations

Recorded during the implementation (branch `dc/mesh-memory`).

1. **Palette writers may grow it (6.4).** `AnimatorSystem`, the skin bindings and the legacy BoneComponent path
   keep their `validIndex` (the 65,536 limit) instead of stopping at `stored()`: a write past the palette grows it,
   and `updateMeshBuffers` then reloads the mesh, which makes its GPU palette again with the new rows. This heals a
   palette that was shorter than its skin (a scene or cooked read) instead of dropping the joint. The model load
   sizes the palette on both of its paths (the flatten path takes its largest skin's), so in practice writers
   stay inside it.
2. **Palette length is kept by every writer.** The YAML encoder writes the whole palette (it no longer trims
   trailing identities: with no 128 fixed entries nothing needs trimming, and the length is how many joints the
   skinned vertices may name); the cooked writer and the generated scene code always write the last entry. A
   mesh without a skin writes `bonesMatrix: []` as before, and old files' trimmed lists read back as they are.
3. **No automatic shrink after a smaller reload (6.3).** `RenderSystem::destroyMesh` releases the GPU objects of
   every stored submesh and buffer (also past the counts), but `MeshSystem` does not `resize` the storage after a
   reload with fewer primitives: a Tilemap writes a submesh's material before it raises `numSubmeshes`
   (`Tilemap::addRect`), so trimming at a reload could drop authored data. The few kilobytes stay until the
   component goes. Decoders do size frames, tiles and tile rects to their counts.
4. **`arrayHeapBytes()` members instead of a `ComponentHeapBytes<T>` trait header (6.10).** `ComponentArray<T>`
   detects a `size_t arrayHeapBytes() const` member with a `requires` expression; Mesh, Sprite, Points, Sprite
   Animation and Tilemap components have one. Same report, no extra header.
5. **Lean MCP rules (AGENT-BRIEF).** `get_ecs_stats` keeps its description; the new numbers are fields of its
   structured result (`element_bytes`, `heap_bytes`, `bytes.heap`) and one clause of its text. The property-path
   error keeps its text (there is no per-error text to extend; the append rule itself is in the resolver).
6. **Tests placed by fixture.** `GrowArray` and `StableArray` share `engine/tests/core/util/GrowArrayTest.cpp`; the
   scene-load trim is `PersistenceTest.ASceneLoadRequestsAHeapTrim` (its SceneManager fixture);
   `HeapTrim.RequestsMerge` replaces the coalescing part of the spec's list.
7. **`test_save_determinism.py`** no longer checks `maxValues` (there is none to count) nor the scene-dump CLI's
   count of a model's buffers (the purpose of that step, sizing `MAX_EXTERNAL_BUFFERS` without a model load, is
   gone); the byte-for-byte checks stay. `test_reflection.py` compares a golden's `maxValues` with what the resave
   wrote (the block as read, or none).
8. **Inspector limits.** The Add buttons are disabled with a tooltip at the limits (D7), the tile-from-rect button
   included.
9. **Pixel-art S45** plans `std::vector` for `SpriteComponent::framesRect` and keeps `MAX_SPRITE_FRAMES` for
   Sprite Animation and Points. With this branch both are already growable (`GrowArray` has `resize`, which S45's
   generated code calls) and the macro is gone: S45 can keep `GrowArray` as it is.
10. **MCP test homes.** `test_ecs.py` runs on the default fixture project, which CI has no copy of, so
    `test_mesh_component_memory` lives in `test_jobs_memory.py` (a created project) beside
    `test_pipeline_desc_record`, which compares 64 equal sprites with one instead of a fixed count (the editor's
    own objects hold descriptions too).
11. **Both path walkers append.** `reflect::resolve` and the property lookup the commands use
    (`editor/reflect/Properties.cpp`) both accept the index one past the end of a growing array.
12. **Templates** were regenerated with `tools/templates/make_project_templates.py` (generated files), so their
    scenes have no `maxValues`; the generator gives them new scene ids and GUIDs, as every run does.
13. **Measured sizes** (Linux, libstdc++, `MeshMemory.SizeofCeilings`): `SokolObject` 752 B (2,528 before),
    `ObjectRender` 768 (2,544), `Submesh` 6,448 (11,776), `MeshComponent` 1,672 (204,928), `SpriteComponent` 72
    (6,192), `TilemapComponent` 152 (125,992); a sprite's mesh with its arrays 8,128 B, 25x less.
14. **AC10 and AC11 not measured on a running editor.** The host's two build slots were the bottleneck and the
    lead stopped further long runs on branches; the frame-time comparison of a 5,000-sprite scene (AC11) and the
    editor's resident size after Play (AC10) were not taken. `HeapTrim.ReturnsFreedPagesOnGlibc` checks the trim
    itself; no per-frame code path gained work beyond a predictable branch (6.1) and the full MCP suite showed no
    timing regression of its own.
15. **After main's S55 and S71.** S55 (automatic GPU instancing) gives every Submesh two more render objects,
    `batchRender` and `batchDepthRender`, inline: a Submesh is 8,528 B and a sprite's mesh with its arrays 10,280 B
    (about 20x less than 204,928). They are loaded only while GPU Instancing is on; allocating them on demand, as
    this spec does for the warm tables, would save 1.6 KB per submesh when it is off (a follow-up for S55's
    owner). S71 counted a submesh's Custom properties as changed on every decode because the editor's array
    always held 16 slots; arrays that grow by index now add their elements' always-flags whatever they hold, so
    a decode over a mesh of no submeshes yet still reloads it. S53's model cooker keeps its 30-buffer
    interleaving rule (fewer buffers bind faster); only its comment no longer names a player capacity.
