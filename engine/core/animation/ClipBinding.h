// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 2.2, 9.R9/R39)
// - Purpose: the engine's resolver of clip binding paths, relative to the animated root. One
//   grammar with editor/util/EntityPath (which gains relative()/findRelative() over these):
//   names from the root joined by '/', a segment "Name#N" selecting the N-th later sibling of
//   that name (Transform child order; "#0" is never written), "" = the root itself.
// - Thread-safety: reads the registry: the thread that owns the scene (main thread for scenes in
//   the editor and the game).
// - Invariants:
//   * formatPath(parsePath(p)) == p for every canonical path; parsePath rejects empty segments
//     and malformed "#N" (non-digits, N < 0).
//   * resolution walks Transform children in order; a missing segment gives NULL_ENTITY (the
//     runtime skips it silently, the editor reports it: ClipValidation).
//   * an empty name, a name containing '/' or ending in '#', "#<digits>" or "#-<digits>" can't be
//     addressed (isAddressableName, the one rule: ModelSubClips uses it too): relativePath fails
//     for entities under such a name.

#ifndef CLIP_BINDING_H
#define CLIP_BINDING_H

#include "Export.h"
#include "animation/ClipTypes.h"
#include "ecs/Entity.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{
    class EntityRegistry;
}

namespace jasperengine::anim{

    struct PathSegment{
        std::string name;
        int index = 0;      // "#N": the N-th later same-named sibling; 0 = the first
    };

    // "" gives no segments (the root); false on an empty segment or a malformed "#N"
    JASPERENGINE_API bool parsePath(const std::string& path, std::vector<PathSegment>& out);
    JASPERENGINE_API std::string formatPath(const std::vector<PathSegment>& segments);
    // "Name" or "Name#N" (N > 0)
    JASPERENGINE_API std::string formatSegment(const std::string& name, int index);
    // false for empty names, names with '/' and names ending in '#', "#<digits>" or "#-<digits>"
    // (they would read back as a malformed or real "#N"; spec 9.R39)
    JASPERENGINE_API bool isAddressableName(const std::string& name);

    // the Transform children of `parent`, in order
    JASPERENGINE_API std::vector<Entity> getChildren(EntityRegistry* registry, Entity parent);

    struct PathResolution{
        Entity entity = NULL_ENTITY;
        // segments that matched before the walk stopped (== segment count when resolved)
        size_t resolvedSegments = 0;
        // a segment without "#N" matched a name that more siblings have (edit-time warning)
        bool duplicateSibling = false;
        // the entity the walk stopped at (the deepest match: the root when nothing matched)
        Entity lastMatched = NULL_ENTITY;
    };

    JASPERENGINE_API Entity resolvePath(EntityRegistry* registry, Entity root, const std::string& path,
                                        PathResolution* info = nullptr);
    JASPERENGINE_API Entity resolvePath(EntityRegistry* registry, Entity root, const std::vector<PathSegment>& segments,
                                        PathResolution* info = nullptr);

    // The path of `entity` relative to `root` ("" for the root itself); false when it is not the
    // root or under it, or a name on the way can't be addressed
    JASPERENGINE_API bool relativePath(EntityRegistry* registry, Entity root, Entity entity, std::string& out);

    // Every addressable descendant of `root` (root included, as "") with its path, depth first in
    // child order: nearest-name suggestions, the Add Property tree, binding reports
    JASPERENGINE_API std::vector<std::pair<std::string, Entity>> descendantPaths(EntityRegistry* registry, Entity root);

    // ---- a bound channel's current value, in ClipKey::value layout ----
    // The one reader/writer of channel values for Record, keying ("current value"), MCP
    // "current" lattice keys and euler-from-current, Bake, preview snapshots and restores.
    // Layout: position/scale x y z; rotation x y z w; morph one weight per morph target of the
    // MeshComponent (MAX_MORPHTARGETS); lattice 3 per entry of `points` (flat cage indices; empty
    // `points` = every point of the cage, in index order).

    // false when the entity lacks the component (Transform / MeshComponent / LatticeComponent)
    // or a lattice point is outside the cage
    JASPERENGINE_API bool readChannelValue(EntityRegistry* registry, Entity entity, ClipChannel channel,
                                           const std::vector<int>& points, std::vector<float>& out);
    // writes it with the same update flags the ActionSystem flush sets (Transform needUpdate,
    // LatticeComponent revision when an offset changed); no command, no scene modified flag. The
    // flush's anchored-UI layout offset correction is not applied (clips animate 3D/2D objects).
    JASPERENGINE_API bool writeChannelValue(EntityRegistry* registry, Entity entity, ClipChannel channel,
                                            const std::vector<int>& points, const std::vector<float>& value);

}

#endif //CLIP_BINDING_H
