# MeshRaycast: raycasts against what is drawn

`MeshRaycast` casts a ray against the triangles a scene draws: meshes (sprites and tilemaps are
meshes), instances of instanced meshes, skinned and morphed meshes at their current pose,
terrains and UI rects. Points, lines, particle systems and 2D occluders answer with their bounds.
No collider or physics body is involved. It is the Scene view's picking, available to games and
editor scripts (docs/specs/S26-scene-view.md).

## Lua

```lua
-- the closest hit; with a Ray, distance is the fraction of ray.direction (as Physics.raycast)
local hit = MeshRaycast.raycast(scene, Ray(origin, direction * 50))
-- with origin, a direction and a maximum distance, distance is in world units
local hit = MeshRaycast.raycast(scene, origin, direction, 50, LayerMask.getMask("Default"))
if hit.hit then
    print(scene:getEntityName(hit.entity), hit.point, hit.normal, hit.distance, hit.triangle)
end
-- every hit, nearest first
for _, h in ipairs(MeshRaycast.raycastAll(scene, origin, direction, 50)) do end
-- one GameObject only
local h = MeshRaycast.raycastEntity(scene, door, ray)
-- build the trees of these meshes (all when none) in the background now
MeshRaycast.prepare(scene)
```

Options, the last argument: `{hitBackfaces = false, includeInactive = false, ignore = {e1, e2},
kinds = MeshRaycast.MESH | MeshRaycast.INSTANCE | MeshRaycast.TERRAIN | MeshRaycast.UI |
MeshRaycast.BOUNDS}`.

A hit has `hit`, `distance`, `point`, `normal` (interpolated from the mesh's normals when it has
them, else the face normal; it faces the ray), `faceNormal`, `barycentric`, `entity`, `submesh`,
`triangle`, `instance` (-1 when not an instance), `kind` (one of the bits above) and `pending`.

## C++

```cpp
#include "picking/MeshRaycast.h"
MeshRaycastHit hit = MeshRaycast::raycast(&scene, Ray(origin, direction * 50.0f));
MeshRaycastOptions options;
options.hitBackfaces = true;
MeshRaycastHit far = MeshRaycast::raycast(&scene, origin, direction, 50.0f, LAYER_MASK_DEFAULT_RAYCAST, options);
RayReturn plain = far.toRayReturn();
```

## MeshRaycast or Physics.raycast

| | Physics.raycast | MeshRaycast |
|---|---|---|
| hits | colliders of bodies | the triangles drawn (and UI rects) |
| needs | a physics body per object | nothing: any mesh, sprite, tilemap, terrain |
| skinned meshes | their colliders | the posed triangles |
| cost | the physics world's broadphase | a scene BVH of bounds, then each mesh's own BVH |
| use it for | gameplay physics (bullets against walls with bodies, ground checks) | aiming at what the player sees, placement, line of sight on visible geometry, editor tools |

Faces: a triangle is hit from the side the renderer draws (double-sided materials from both);
`hitBackfaces` hits both sides of everything. LOD: always LOD0's triangles.

## Cost

Nothing until the first query: a game that never calls MeshRaycast builds nothing and does no
per-frame work. A mesh's triangle tree is built the first time a ray reaches its bounds, inline
(or ahead of time with `prepare`); meshes with the same geometry share one tree. Meshes under 256
triangles are tested directly. Above `Engine::setMeshRaycastMaxTriangles` (default 2,000,000) a
mesh answers with its bounds. Trees of meshes no GameObject uses are freed 30 s later.

Skinned and morphed meshes are posed on the CPU (the shaders' math) and their tree refitted when
the pose changes; above 200,000 vertices they use the bind pose inside their posed bounds.

Alpha-cutout materials pick as solid (their triangles); lock such layers against picking in the
Scene view's Layers menu if they get in the way.
