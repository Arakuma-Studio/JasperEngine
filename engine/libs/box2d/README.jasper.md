# Box2D in JasperEngine

Vendored Box2D 3.1.0 (`src/core.c`), single-threaded worlds (no task system).

## Local patch: surface callbacks

Every hunk is marked `// JASPER PATCH: surface callbacks (upstream 3.1 API)`. It backports the
friction/restitution callbacks of upstream Box2D 3.1 so the engine can apply the per-collider
combine modes of `PhysicsMaterial` (`engine/core/physics/PhysicsMaterial.h`, physics-2d D12):

- `types.h`: `int userMaterialId` in `b2ShapeDef` and `b2ChainDef` (default 0), the
  `b2FrictionCallback` / `b2RestitutionCallback` typedefs.
- `box2d.h`: `b2World_SetFrictionCallback`, `b2World_SetRestitutionCallback`,
  `b2Shape_SetUserMaterial`, `b2Shape_GetUserMaterial`, `b2Chain_SetUserMaterial`.
- `shape.h/.c`: the id stored per shape; chains copy it to their segments.
- `world.h/.c`: the callback pointers (NULL by default: the world mixing rules as before).
- `contact.c`: a new contact mixes friction and restitution through the callbacks when they are set,
  and every contact update mixes them again from its shapes, so a shape's friction, restitution or
  material id changed at run time reaches the contacts it already has (unpatched 3.1 keeps the
  values of when the contact was made; refiltering to remake them sends end / begin events).

With no callback set and no surface changed at run time Box2D behaves exactly as unpatched. When updating Box2D, drop the hunks that
upstream already has (3.1 upstream keeps `userMaterialId` inside `b2SurfaceMaterial`).
