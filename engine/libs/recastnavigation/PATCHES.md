# Local patches to recastnavigation

The zlib licence asks that altered sources are plainly marked: each changed line carries a
`JasperEngine patch` comment, and each patch is listed here.

## 1. Avoidance priority (DetourCrowd)

Unity's `NavMeshAgent.avoidancePriority`: 0-99, lower is more important. An agent steers around
neighbours whose priority is less than or equal to its own and ignores those whose priority is
greater, so an important agent keeps its line and the others make way (spec S19 R13, D7).

- `DetourCrowd/Include/DetourCrowd.h`: `dtCrowdAgentParams::avoidancePriority` (`unsigned char`).
  Every caller fills it: the engine's `NavCrowd` sets it from the agent (50 by default), and
  non-carving obstacles get 0 so that every agent avoids them.
- `DetourCrowd/Source/DetourCrowd.cpp`, `dtCrowd::update`: the separation loop and the loop that
  adds neighbours to the obstacle avoidance query as circles skip a neighbour whose priority is
  greater than the agent's. The collision resolution that pushes overlapping agents apart is
  unchanged, so agents never interpenetrate whatever their priorities.

Covered by `NavCrowdTest.PriorityYields` (`engine/tests/core/navigation/NavCrowdTest.cpp`).

## 2. A corridor filled to capacity is valid (DetourCrowd, upstream `9147d09`, #619)

`dtPathCorridor::setCorridor` asserted `npath < m_maxPath`. dtCrowd clips a path-queue result to
the corridor's 256 polygons and sets exactly that many, so every path longer than the corridor
fired the assert (asserts are compiled in where `NDEBUG` is off: the sanitizer builds).

- `DetourCrowd/Source/DetourPathCorridor.cpp`, `dtPathCorridor::setCorridor`: `npath <= m_maxPath`,
  upstream's one-character fix.

Covered by `NavCrowdTest.CorridorAtCapacity` (a 600 m strip: the corridor fills, records no
assert, and dtCrowd's extension takes the agent to its destination).

## 3. Merging more visited polygons than the path holds (DetourCrowd, upstream `599fd0f`, #635)

`dtMergeCorridorStartMoved` computed `size = maxPath - req`, negative when more polygons were
visited past the furthest common one than the path holds, then `memmove`d `size * 4` bytes and
stored all `req` visited polygons past the buffer. dtCrowd itself cannot reach it
(`moveAlongSurface` visits at most 16 polygons, the corridor holds 256); a direct call can.
`dtMergeCorridorStartShortcut` has the same pattern, but its `req` is at most 32 and upstream left
it unchanged, as does this patch.

- `DetourCrowd/Source/DetourPathCorridor.cpp`, `dtMergeCorridorStartMoved`: `if (size > 0)` before
  the `memmove`, and the store loop runs `dtMin(req, maxPath)` times, as upstream does.

Covered by `NavCrowdTest.MergeCorridorStartMovedBounds` (canaries after a 4-polygon path).

## 4. No detail sample against an empty triangle list (Recast, upstream `9f4ce64`, #796)

`buildPolyDetail` measured each sample against `&tris[0]` after `delaunayHull`, which can drop
every dangling face: `rcTempVector::operator[]` then asserts. In Release no triangle is read, so
only builds with asserts failed; upstream's fix skips the sample.

- `Recast/Source/RecastMeshDetail.cpp`, `buildPolyDetail`: `if (tris.size() == 0) continue;`.

Covered by `NavBakeTest.DetailSamplingDegenerate` (slopes 100 km from the origin, no Recast assert).

## 5. The proximity grid's hash wraps in unsigned arithmetic (DetourCrowd, ours)

`hashPos2` multiplied cell coordinates by large primes in `int`, which overflows (undefined
behaviour) for a crowd far enough from the origin: UBSan stops at a crowd 300 m out. The products
and the XOR are now taken in `unsigned int`, whose wrap is defined and gives the same bits the
signed code gave wherever it happened to wrap: the same buckets, the same crowd.

- `DetourCrowd/Source/DetourProximityGrid.cpp`, `hashPos2`.

Covered by `NavCrowdTest.CorridorAtCapacity` and `NavAgentTest.ExtensionKeepsPathWithoutAutoRepath`
under `SANITIZE=address,undefined` (a crowd on a 600 m strip). Upstream (main) still has it.

## Considered and not taken

- `0096f0d`: `dtNavMeshQuery::raycast` stops once its path buffer is full. It changes query and
  crowd results (`optimizePathVisibility` raycasts into 32 slots), and 1.6.0 already keeps
  walking safely.
- `3e94c3b` (`rcFilterLedgeSpans`: `>` became `>=`, early breaks) and `c393777` (a rewritten
  detail-edge detection): both change bake output without fixing a crash, so a rebake would
  differ from the shipped `.navmesh` assets (S19 R40) for no gain.
- The assert-gating series `af6d475`, `63dd4aa`, `77f7e54` and `f9addb2`: they gate asserts on
  `RC_DISABLE_ASSERTS` instead of `NDEBUG`, which turns them on in Release builds.
