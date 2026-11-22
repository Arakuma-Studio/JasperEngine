# Local patches to JoltPhysics

Jolt is vendored at v5.2.0. Each changed line carries a `JasperEngine patch` comment, and each patch
is listed here, so an update of Jolt knows what to carry over (or drop, once upstream has it).

## 1. The depth of a contact replayed from the cache (upstream v5.4.0)

A body pair that barely moved since the last step reuses its cached manifolds
(`ContactConstraintManager::GetContactsFromCache`) and calls `ContactListener::OnContactPersisted`
with a penetration depth estimated from the cached points. The loop over the points read point 0
every time, so the estimate was the first point's depth, not the deepest one's. A body resting on
an edge whose first cached point was a speculative one (not touching yet) read as apart for as long
as it rested: `PlacementWorld`'s decomposed ring over a peg never fell asleep on Windows (its
"falling" rule waits for a touching contact), and `Contact3D::getPenetrationDepth` (also Lua's)
gave that point's depth. Which point comes first follows the platform's floating point: the ring
rested that way on MSVC builds and not on GCC's.

- `Jolt/Physics/Constraints/ContactConstraintManager.cpp`, `GetContactsFromCache`: the depth of
  point `i`, as upstream's v5.4.0 ("Fixed passing underestimate of penetration depth in
  `ContactListener::OnContactPersisted` when the contact comes from the contact cache").

Drop on an update to v5.4.0 or later. Covered by `JoltContactCache.PersistedDepthIsTheDeepestCachedPoint`
(`engine/tests/core/physics/JoltContactCacheTest.cpp`) and `PlacementWorldColliders.RingOverPeg`
(`editor/tests/engine/physics/PlacementWorldTest.cpp`, on Windows).
