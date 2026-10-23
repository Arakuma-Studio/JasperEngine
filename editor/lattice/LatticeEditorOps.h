// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LATTICEEDITOROPS_H
#define LATTICEEDITOROPS_H

// The single path for lattice edits in the editor (spec §9.3): the Scene view, inspectors,
// context menu, creation command and MCP all call these. Every edit pushes one undoable
// command (or command group) on the scene's CommandHistory, records prefab overrides and
// refreshes the editor. Functions return false and fill `error` (when given) with a message
// that says how to fix the input; nothing is changed on failure.

#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "math/Quaternion.h"
#include "math/Matrix4.h"
#include "component/LatticeComponent.h"
#include "component/LatticeModifierComponent.h"
#include "lattice/LatticePresets.h"
#include <string>
#include <vector>

namespace jasperengine{ class Scene; }

namespace jasperengine::editor{

    class Project;

    enum class LatticeFalloffCurve{
        SMOOTH,
        LINEAR,
        SHARP,
        CONSTANT
    };

    struct LatticeFalloff{
        bool enabled = false;
        float radius = 0.5f; // lattice local units
        LatticeFalloffCurve curve = LatticeFalloffCurve::SMOOTH;
    };

    // Result of fitting a lattice around entities (creation, Fit to Targets).
    struct LatticeFit{
        Vector3 position;
        Quaternion rotation;
        Vector3 scale;
        int resolution[3] = {3, 3, 3};
    };

    class LatticeEditorOps{
    public:
        // --- point selection of the lattice edited in the Scene view (not undoable) ---
        static const std::vector<int>& getSelectedPoints(uint32_t sceneId, Entity lattice);
        static void setSelectedPoints(uint32_t sceneId, Entity lattice, const std::vector<int>& points);
        static void clearSelectedPoints(uint32_t sceneId);
        // the last point added to the selection (pivot mode), -1 when none
        static int getLastSelectedPoint(uint32_t sceneId, Entity lattice);

        // Scene-view falloff settings (shared by the gizmo, inspector and context menu)
        static LatticeFalloff& falloff();

        // --- edits (one undo step each) ---
        static bool setOffsets(Project* project, uint32_t sceneId, Entity lattice,
                               const std::vector<int>& points, const std::vector<Vector3>& offsets,
                               std::string* error = nullptr);
        // applies `localTransform` (lattice local space, about `pivot` in local space) to the
        // current positions of `points`, plus falloff-weighted neighbours
        static bool transformPoints(Project* project, uint32_t sceneId, Entity lattice,
                                    const std::vector<int>& points, const Matrix4& localTransform,
                                    const Vector3& pivot, const LatticeFalloff& falloff,
                                    std::string* error = nullptr);
        static bool resetPoints(Project* project, uint32_t sceneId, Entity lattice,
                                const std::vector<int>& points, std::string* error = nullptr); // empty = all
        static bool applyPreset(Project* project, uint32_t sceneId, Entity lattice,
                                const lattice::PresetParams& params, const std::vector<int>& points,
                                std::string* error = nullptr);
        static bool mirror(Project* project, uint32_t sceneId, Entity lattice, lattice::LatticeAxis axis,
                           bool fromPositive, std::string* error = nullptr);
        static bool setResolution(Project* project, uint32_t sceneId, Entity lattice, int rx, int ry, int rz,
                                  std::string* error = nullptr);

        // bounds of the targets (and their mesh descendants); false when none has a mesh
        static bool computeFit(Scene* scene, const std::vector<Entity>& targets, float padding, LatticeFit& fit,
                               std::string* error = nullptr);
        static bool fitTo(Project* project, uint32_t sceneId, Entity lattice, const std::vector<Entity>& targets,
                          float padding, std::string* error = nullptr);

        // adds the lattice to the target's LatticeModifierComponent (adding the component when missing)
        static bool bind(Project* project, uint32_t sceneId, Entity lattice, Entity target, const LatticeBinding& binding,
                         bool includeChildren, std::string* error = nullptr);
        static bool unbind(Project* project, uint32_t sceneId, Entity lattice, Entity target, std::string* error = nullptr);
        // entities whose LatticeModifierComponent lists this lattice
        static std::vector<Entity> findTargets(Scene* scene, Entity lattice);

        static bool savePose(Project* project, uint32_t sceneId, Entity lattice, const std::string& name,
                             std::string* error = nullptr); // overwrites a pose of the same name
        static bool applyPose(Project* project, uint32_t sceneId, Entity lattice, const std::string& name,
                              float weight, std::string* error = nullptr);
        static bool renamePose(Project* project, uint32_t sceneId, Entity lattice, const std::string& name,
                               const std::string& newName, std::string* error = nullptr);
        static bool deletePose(Project* project, uint32_t sceneId, Entity lattice, const std::string& name,
                               std::string* error = nullptr);

        // creates a lattice entity fitted around `targets` (or a unit lattice at `fallbackPosition`
        // without targets) and binds it to them, one undo step; NULL_ENTITY on failure
        static Entity createLattice(Project* project, uint32_t sceneId, const std::vector<Entity>& targets,
                                    const Vector3& fallbackPosition, const std::string& name = "Lattice",
                                    std::string* error = nullptr);
    };

}

#endif //LATTICEEDITOROPS_H
