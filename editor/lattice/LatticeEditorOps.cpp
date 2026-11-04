// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeEditorOps.h"
#include "component/Transform.h"
#include "component/MeshComponent.h"

#include "AppSettings.h"
#include "Project.h"
#include "animation/ClipRecordHooks.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/LatticeBindingsCmd.h"
#include "command/type/LatticePointsCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ObjectTransformCmd.h"
#include "lattice/LatticeSolver.h"
#include "pose/PoseEditSession.h"
#include "render/SceneRender.h"
#include "subsystem/LatticeSystem.h"
#include "component/LatticeFollowerComponent.h"
#include "Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    // The point selection of the lattice edited in each scene's Scene view
    struct PointSelection{
        Entity lattice = NULL_ENTITY;
        std::vector<int> points; // ascending, unique
        int last = -1;
    };

    std::map<uint32_t, PointSelection> selections;

    bool fail(std::string* error, const std::string& message){
        if (error){
            *error = message;
        }
        return false;
    }

    bool isFinite(const Vector3& v){
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    bool isFinite(const Matrix4& m){
        for (int c = 0; c < 4; c++){
            for (int r = 0; r < 4; r++){
                if (!std::isfinite(m.get(c, r))){
                    return false;
                }
            }
        }
        return true;
    }

    int pointCountOf(const LatticeComponent& lattice){
        return lattice.resolutionX * lattice.resolutionY * lattice.resolutionZ;
    }

    bool validResolution(int r){
        return r >= LATTICE_MIN_RESOLUTION && r <= LATTICE_MAX_RESOLUTION;
    }

    std::string entityLabel(Scene* scene, Entity entity){
        std::string name = scene->isEntityCreated(entity) ? scene->getEntityName(entity) : std::string();
        return name.empty() ? "entity " + std::to_string(entity) : "'" + name + "' (" + std::to_string(entity) + ")";
    }

    SceneProject* sceneOf(Project* project, uint32_t sceneId, std::string* error){
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        if (!sceneProject || !sceneProject->scene){
            fail(error, "scene " + std::to_string(sceneId) + " is not open");
            return nullptr;
        }
        return sceneProject;
    }

    LatticeComponent* latticeOf(Scene* scene, Entity entity, std::string* error){
        if (entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
            fail(error, "entity " + std::to_string(entity) + " does not exist in this scene");
            return nullptr;
        }
        LatticeComponent* lattice = scene->findComponent<LatticeComponent>(entity);
        if (!lattice){
            fail(error, entityLabel(scene, entity) + " is not a lattice (it has no Lattice component)");
            return nullptr;
        }
        if (!validResolution(lattice->resolutionX) || !validResolution(lattice->resolutionY) || !validResolution(lattice->resolutionZ) ||
            (int)lattice->offsets.size() != pointCountOf(*lattice)){
            fail(error, entityLabel(scene, entity) + " has a broken layout (resolution " + std::to_string(lattice->resolutionX) + "x" +
                 std::to_string(lattice->resolutionY) + "x" + std::to_string(lattice->resolutionZ) + ", " +
                 std::to_string(lattice->offsets.size()) + " offsets): set its resolution again");
            return nullptr;
        }
        return lattice;
    }

    bool checkPoints(const std::vector<int>& points, int count, std::string* error){
        for (int index : points){
            if (index < 0 || index >= count){
                return fail(error, "point index " + std::to_string(index) + " is out of range: the lattice has " +
                            std::to_string(count) + " points (0.." + std::to_string(count - 1) + ")");
            }
        }
        return true;
    }

    std::vector<int> allPoints(int count){
        std::vector<int> points((size_t)count);
        for (int i = 0; i < count; i++){
            points[(size_t)i] = i;
        }
        return points;
    }

    // a discrete edit: its own undo step, nothing merges into it
    bool push(uint32_t sceneId, Command* command){
        command->setNoMerge();
        return CommandHandle::get(sceneId)->addCommand(command);
    }

    // Record (animation clips 9.R5): a claimed lattice's edit asked to record by its source (the
    // Scene view's point drag: a continuous edit, one gesture until endContinuousEdit; the
    // Inspector's Offset row: one gesture per edit) writes the live offsets and keys the changed
    // points at the playhead, with no command. Edits not asked to record (MCP edit_lattice, menus,
    // presets) always stay scene steps, whatever gesture another source has open (T3c review I4).
    bool recordOffsets(uint32_t sceneId, Entity entity, const LatticeComponent& lattice,
                       const std::vector<Vector3>& offsets, bool continuous, bool record){
        if (!record || !ClipRecordHooks::claims(sceneId, entity, ClipChannel::Lattice)) return false;
        ClipRecordValue value;
        value.channel = ClipChannel::Lattice;
        for (size_t i = 0; i < offsets.size(); i++){
            const Vector3 before = i < lattice.offsets.size() ? lattice.offsets[i] : Vector3(0, 0, 0);
            if (offsets[i] == before) continue;
            value.points.push_back((int)i);
            value.value.insert(value.value.end(), {offsets[i].x, offsets[i].y, offsets[i].z});
        }
        if (value.points.empty()) return true;
        const RecordSource source = continuous ? RecordSource::LatticeDrag : RecordSource::Inspector;
        bool opened = false;
        const std::string mergeKey = ClipRecordHooks::gestureMergeKey(source, &opened);
        ClipRecordHooks::record(sceneId, entity, value, mergeKey);
        if (opened && !continuous) ClipRecordHooks::endGesture(source);
        return true;
    }

    // `explicitPoints`: the points the edit sets by index (setOffsets), the new rest of a clip
    // preview even when equal to what it shows (a write of them while a preview shows the
    // lattice is a step even when the offsets shown do not change)
    bool pushOffsets(Project* project, uint32_t sceneId, Entity entity, const LatticeComponent& lattice,
                     const std::vector<Vector3>& offsets, bool continuous, bool record, std::string* error,
                     const std::vector<int>& explicitPoints = {}){
        for (const Vector3& offset : offsets){
            if (!isFinite(offset)){
                return fail(error, "the edit gives a point a non-finite offset");
            }
        }
        if (offsets == lattice.offsets && (explicitPoints.empty() || !ClipRecordHooks::previews(sceneId, entity, ClipChannel::Lattice))){
            return true; // nothing changes: no undo step
        }
        if (recordOffsets(sceneId, entity, lattice, offsets, continuous, record)){
            return true;
        }
        auto* points = new LatticePointsCmd(project, sceneId, entity, offsets, continuous);
        points->setExplicitPoints(explicitPoints);
        Command* command = points;
        bool added = continuous ? CommandHandle::get(sceneId)->addCommand(command) : push(sceneId, command);
        return added ? true : fail(error, "the lattice could not be edited");
    }

    bool pushState(Project* project, uint32_t sceneId, Entity entity, const LatticePointsState& state, std::string* error){
        return push(sceneId, new LatticePointsCmd(project, sceneId, entity, state)) ? true : fail(error, "the lattice could not be edited");
    }

    LatticePointsState stateOf(const LatticeComponent& lattice){
        LatticePointsState state;
        state.resolutionX = lattice.resolutionX;
        state.resolutionY = lattice.resolutionY;
        state.resolutionZ = lattice.resolutionZ;
        state.offsets = lattice.offsets;
        state.poses = lattice.poses;
        return state;
    }

    bool hasMesh(Scene* scene, Entity entity){
        if (MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            AABB aabb = SceneRender::getMeshLocalAABB(*mesh);
            return !aabb.isNull() && !aabb.isInfinite();
        }
        return false;
    }

    bool isDescendantOf(Scene* scene, Entity entity, Entity ancestor){
        Transform* transform = scene->findComponent<Transform>(entity);
        for (int hops = 0; transform && transform->parent != NULL_ENTITY && hops < 4096; hops++){
            if (transform->parent == ancestor){
                return true;
            }
            transform = scene->findComponent<Transform>(transform->parent);
        }
        return false;
    }

    // the target and its descendants that have a mesh with bounds
    std::vector<Entity> meshesOf(Scene* scene, Entity target){
        std::vector<Entity> meshes;
        if (hasMesh(scene, target)){
            meshes.push_back(target);
        }
        auto transforms = scene->getComponentArray<Transform>();
        for (size_t i = 0; i < transforms->size(); i++){
            Entity entity = transforms->getEntity(i);
            if (entity != target && hasMesh(scene, entity) && isDescendantOf(scene, entity, target)){
                meshes.push_back(entity);
            }
        }
        return meshes;
    }

    // bounds of `meshes` in the space `toSpace` maps world into
    AABB boundsIn(Scene* scene, const std::vector<Entity>& meshes, const Matrix4& toSpace){
        AABB bounds;
        for (Entity entity : meshes){
            Transform* transform = scene->findComponent<Transform>(entity);
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!transform || !mesh){
                continue;
            }
            AABB local = SceneRender::getMeshLocalAABB(*mesh);
            const Vector3* corners = local.getCorners();
            Matrix4 toTarget = toSpace * transform->modelMatrix;
            for (int c = 0; c < 8; c++){
                Vector3 p = toTarget * corners[c];
                if (isFinite(p)){
                    bounds.merge(p);
                }
            }
        }
        return bounds;
    }

    // falloff-weighted copy of an affine local transform: translation, rotation and scale
    // scaled by the weight when it decomposes into them, else the moved position blended
    struct WeightedTransform{
        Matrix4 full;
        bool decomposed = false;
        Vector3 translation;
        Quaternion rotation;
        Vector3 scale;

        explicit WeightedTransform(const Matrix4& transform): full(transform){
            transform.decomposeStandard(translation, scale, rotation);
            Matrix4 rebuilt = Matrix4::translateMatrix(translation) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
            float tolerance = 1e-4f;
            for (int c = 0; c < 4; c++){
                for (int r = 0; r < 4; r++){
                    tolerance = std::max(tolerance, 1e-4f * std::fabs(transform.get(c, r)));
                }
            }
            decomposed = true;
            for (int c = 0; c < 4 && decomposed; c++){
                for (int r = 0; r < 4; r++){
                    if (!(std::fabs(rebuilt.get(c, r) - transform.get(c, r)) <= tolerance)){
                        decomposed = false;
                        break;
                    }
                }
            }
        }

        // relative to the pivot
        Vector3 apply(const Vector3& relative, float weight) const{
            if (weight >= 1.0f){
                return full * relative;
            }
            if (!decomposed){
                Vector3 moved = full * relative;
                return relative + (moved - relative) * weight;
            }
            Quaternion partial = Quaternion::slerp(weight, Quaternion(), rotation, true);
            Vector3 partialScale = Vector3(1, 1, 1) + (scale - Vector3(1, 1, 1)) * weight;
            Matrix4 m = Matrix4::translateMatrix(translation * weight) * partial.getRotationMatrix() * Matrix4::scaleMatrix(partialScale);
            return m * relative;
        }
    };

    bool validBinding(const LatticeBinding& binding, std::string* error){
        if (!std::isfinite(binding.weight) || binding.weight < 0.0f || binding.weight > 2.0f){
            return fail(error, "binding weight must be between 0 and 2");
        }
        if (binding.submesh < -1){
            return fail(error, "binding submesh must be -1 (all) or a submesh index");
        }
        if ((int)binding.stage < (int)LatticeStage::BEFORE_SKINNING || (int)binding.stage > (int)LatticeStage::AFTER_SKINNING){
            return fail(error, "binding stage must be before_skinning or after_skinning");
        }
        if ((int)binding.mask < (int)LatticeMaskChannel::NONE || (int)binding.mask > (int)LatticeMaskChannel::INVERTED_ALPHA){
            return fail(error, "binding mask must be none, red, green, blue, alpha or inverted_red..inverted_alpha");
        }
        return true;
    }

    // the bindings `target` has after binding `lattice` with `binding` (replacing its settings
    // when the lattice is already listed)
    std::vector<LatticeBinding> boundList(Scene* scene, Entity target, Entity lattice, const LatticeBinding& binding){
        std::vector<LatticeBinding> bindings;
        if (LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(target)){
            bindings = modifier->bindings;
        }
        LatticeBinding entry = binding;
        entry.lattice = lattice;
        for (LatticeBinding& existing : bindings){
            if (existing.lattice == lattice){
                existing = entry;
                return bindings;
            }
        }
        bindings.push_back(entry);
        return bindings;
    }

    bool sameBindings(const std::vector<LatticeBinding>& a, const std::vector<LatticeBinding>& b){
        if (a.size() != b.size()){
            return false;
        }
        for (size_t i = 0; i < a.size(); i++){
            if (a[i].lattice != b[i].lattice || a[i].weight != b[i].weight || a[i].stage != b[i].stage ||
                a[i].submesh != b[i].submesh || a[i].mask != b[i].mask){
                return false;
            }
        }
        return true;
    }

    // world transform of the lattice placed by `fit`, as its Transform (parent space)
    Matrix4 fitLocalMatrix(Scene* scene, Entity lattice, const LatticeFit& fit){
        Matrix4 world = Matrix4::translateMatrix(fit.position) * fit.rotation.getRotationMatrix() * Matrix4::scaleMatrix(fit.scale);
        if (Transform* transform = scene->findComponent<Transform>(lattice)){
            if (Transform* parent = scene->findComponent<Transform>(transform->parent)){
                return parent->modelMatrix.inverse() * world;
            }
        }
        return world;
    }

    LatticePose* findPose(LatticeComponent& lattice, const std::string& name){
        for (LatticePose& pose : lattice.poses){
            if (pose.name == name){
                return &pose;
            }
        }
        return nullptr;
    }

}

// --- point selection ---

const std::vector<int>& LatticeEditorOps::getSelectedPoints(uint32_t sceneId, Entity lattice){
    static const std::vector<int> none;
    auto it = selections.find(sceneId);
    if (it == selections.end() || it->second.lattice != lattice || lattice == NULL_ENTITY){
        return none;
    }
    return it->second.points;
}

void LatticeEditorOps::setSelectedPoints(uint32_t sceneId, Entity lattice, const std::vector<int>& points){
    if (lattice == NULL_ENTITY || points.empty()){
        selections.erase(sceneId);
        return;
    }
    PointSelection& selection = selections[sceneId];
    const bool sameLattice = selection.lattice == lattice;
    std::vector<int> previous = sameLattice ? selection.points : std::vector<int>();

    selection.lattice = lattice;
    selection.points.clear();
    for (int index : points){
        if (index >= 0){
            selection.points.push_back(index);
        }
    }
    std::sort(selection.points.begin(), selection.points.end());
    selection.points.erase(std::unique(selection.points.begin(), selection.points.end()), selection.points.end());

    // the last point added: the last of `points` that was not selected before, else the
    // previous last when it is still there, else the last given
    int last = -1;
    for (auto it = points.rbegin(); it != points.rend(); ++it){
        if (*it >= 0 && !std::binary_search(previous.begin(), previous.end(), *it)){
            last = *it;
            break;
        }
    }
    if (last < 0 && sameLattice && std::binary_search(selection.points.begin(), selection.points.end(), selection.last)){
        last = selection.last;
    }
    if (last < 0 && !selection.points.empty()){
        last = points.back() >= 0 ? points.back() : selection.points.back();
    }
    selection.last = last;
    if (selection.points.empty()){
        selections.erase(sceneId);
    }
}

void LatticeEditorOps::clearSelectedPoints(uint32_t sceneId){
    selections.erase(sceneId);
}

int LatticeEditorOps::getLastSelectedPoint(uint32_t sceneId, Entity lattice){
    auto it = selections.find(sceneId);
    if (it == selections.end() || it->second.lattice != lattice || lattice == NULL_ENTITY){
        return -1;
    }
    return it->second.last;
}

void LatticeEditorOps::validateSelection(uint32_t sceneId, Entity lattice, int pointCount){
    auto it = selections.find(sceneId);
    if (it == selections.end()){
        return;
    }
    PointSelection& selection = it->second;
    if (lattice == NULL_ENTITY || selection.lattice != lattice){
        selections.erase(it);
        return;
    }
    selection.points.erase(std::remove_if(selection.points.begin(), selection.points.end(), [pointCount](int index){
        return index >= pointCount;
    }), selection.points.end());
    if (selection.points.empty()){
        selections.erase(it);
        return;
    }
    if (selection.last >= pointCount){
        selection.last = selection.points.back();
    }
}

std::vector<int> LatticeEditorOps::getSelectedOrAllPoints(uint32_t sceneId, Entity lattice, const LatticeComponent& component){
    const std::vector<int>& selected = getSelectedPoints(sceneId, lattice);
    if (!selected.empty()){
        return selected;
    }
    return allPoints(pointCountOf(component));
}

Entity LatticeEditorOps::getFollowerDrivingRest(Scene* scene, Entity entity, bool position, bool rotation, bool scale){
    if (!scene || entity == NULL_ENTITY || !scene->isEntityCreated(entity)){
        return NULL_ENTITY;
    }
    const LatticeFollowerComponent* follower = scene->findComponent<LatticeFollowerComponent>(entity);
    if (!follower || !follower->enabled || !scene->isEntityActiveInHierarchy(entity) || !scene->findComponent<Transform>(entity)){
        return NULL_ENTITY;
    }
    if (!((position && follower->applyPosition) || (rotation && follower->applyRotation) || (scale && follower->applyScale))){
        return NULL_ENTITY;
    }
    // the conditions LatticeSystem::updateFollowers moves it under
    const Entity lattice = follower->lattice;
    const Entity rest = follower->rest;
    if (lattice == NULL_ENTITY || rest == NULL_ENTITY || !scene->isEntityCreated(lattice) || !scene->isEntityCreated(rest) ||
        !scene->findComponent<LatticeComponent>(lattice) || !scene->findComponent<Transform>(lattice) ||
        !scene->findComponent<Transform>(rest)){
        return NULL_ENTITY;
    }
    LatticeSystem* system = scene->getSystem<LatticeSystem>().get();
    if (!system || !system->isValidFollower(entity, lattice, rest)){
        return NULL_ENTITY;
    }
    return rest;
}

Entity LatticeEditorOps::getEditedLattice(Project* project, uint32_t sceneId){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene || sceneProject->selectedEntities.size() != 1){
        return NULL_ENTITY;
    }
    Entity entity = sceneProject->selectedEntities[0];
    Scene* scene = sceneProject->scene;
    if (!scene->isEntityCreated(entity) || !scene->findComponent<LatticeComponent>(entity) || !scene->findComponent<Transform>(entity)){
        return NULL_ENTITY;
    }
    return entity;
}

LatticeFalloff& LatticeEditorOps::falloff(){
    static LatticeFalloff settings = [](){
        LatticeFalloff initial;
        const LatticeViewSettings view = AppSettings::getLatticeViewSettings();
        initial.radius = view.falloffRadius;
        initial.curve = (LatticeFalloffCurve)std::clamp(view.falloffCurve, 0, 3);
        return initial;
    }();
    return settings;
}

float LatticeEditorOps::falloffWeight(const LatticeFalloff& falloff, float distance){
    if (!falloff.enabled || !(falloff.radius > 0.0f) || !(distance < falloff.radius)){
        return 0.0f;
    }
    float t = 1.0f - std::max(distance, 0.0f) / falloff.radius;
    switch (falloff.curve){
        case LatticeFalloffCurve::LINEAR:   return t;
        case LatticeFalloffCurve::SHARP:    return t * t;
        case LatticeFalloffCurve::CONSTANT: return 1.0f;
        case LatticeFalloffCurve::SMOOTH:
        default:                            return t * t * (3.0f - 2.0f * t);
    }
}

bool LatticeEditorOps::getPointLocalPosition(const LatticeComponent& lattice, int index, Vector3& position){
    const int count = pointCountOf(lattice);
    if (index < 0 || index >= count || (int)lattice.offsets.size() != count || lattice.resolutionX <= 0 || lattice.resolutionY <= 0){
        return false;
    }
    const int i = index % lattice.resolutionX;
    const int j = (index / lattice.resolutionX) % lattice.resolutionY;
    const int k = index / (lattice.resolutionX * lattice.resolutionY);
    position = lattice::restPosition(lattice.resolutionX, lattice.resolutionY, lattice.resolutionZ, i, j, k) + lattice.offsets[(size_t)index];
    return true;
}

std::vector<Vector3> LatticeEditorOps::transformedOffsets(int rx, int ry, int rz, const std::vector<Vector3>& base,
                                                          const std::vector<int>& points, const Matrix4& localTransform,
                                                          const Vector3& pivot, const LatticeFalloff& falloff){
    if (!validResolution(rx) || !validResolution(ry) || !validResolution(rz) || (int)base.size() != rx * ry * rz){
        return base;
    }
    const int count = rx * ry * rz;

    std::vector<Vector3> current((size_t)count);
    std::vector<Vector3> rest((size_t)count);
    for (int k = 0; k < rz; k++){
        for (int j = 0; j < ry; j++){
            for (int i = 0; i < rx; i++){
                int index = i + rx * (j + ry * k);
                rest[(size_t)index] = lattice::restPosition(rx, ry, rz, i, j, k);
                current[(size_t)index] = rest[(size_t)index] + base[(size_t)index];
            }
        }
    }

    std::vector<int> moved = points.empty() ? allPoints(count) : points;
    std::vector<char> isMoved((size_t)count, 0);
    for (int index : moved){
        if (index >= 0 && index < count){
            isMoved[(size_t)index] = 1;
        }
    }

    // falloff: the moved points' box grown by the radius rejects most far points before the
    // nearest-point search (squared distances, stops at the first point at distance 0)
    const bool useFalloff = falloff.enabled && falloff.radius > 0.0f;
    std::vector<Vector3> movedPositions;
    Vector3 reachMin(std::numeric_limits<float>::max());
    Vector3 reachMax(-std::numeric_limits<float>::max());
    if (useFalloff){
        movedPositions.reserve(moved.size());
        for (int other : moved){
            if (other >= 0 && other < count){
                const Vector3& p = current[(size_t)other];
                movedPositions.push_back(p);
                reachMin = Vector3(std::min(reachMin.x, p.x), std::min(reachMin.y, p.y), std::min(reachMin.z, p.z));
                reachMax = Vector3(std::max(reachMax.x, p.x), std::max(reachMax.y, p.y), std::max(reachMax.z, p.z));
            }
        }
        reachMin = reachMin - Vector3(falloff.radius);
        reachMax = reachMax + Vector3(falloff.radius);
    }

    const WeightedTransform transform(localTransform);
    std::vector<Vector3> result = base;
    for (int index = 0; index < count; index++){
        float weight = 0.0f;
        if (isMoved[(size_t)index]){
            weight = 1.0f;
        }else if (useFalloff && !movedPositions.empty()){
            const Vector3& p = current[(size_t)index];
            if (p.x < reachMin.x || p.y < reachMin.y || p.z < reachMin.z ||
                    p.x > reachMax.x || p.y > reachMax.y || p.z > reachMax.z){
                continue;
            }
            float nearestSq = std::numeric_limits<float>::max();
            for (const Vector3& q : movedPositions){
                const Vector3 d = p - q;
                nearestSq = std::min(nearestSq, d.dotProduct(d));
                if (nearestSq <= 0.0f){
                    break;
                }
            }
            weight = falloffWeight(falloff, std::sqrt(nearestSq));
        }
        if (weight <= 0.0f){
            continue;
        }
        Vector3 position = pivot + transform.apply(current[(size_t)index] - pivot, weight);
        result[(size_t)index] = position - rest[(size_t)index];
    }
    return result;
}

// --- edits ---

bool LatticeEditorOps::setOffsets(Project* project, uint32_t sceneId, Entity entity,
                                  const std::vector<int>& points, const std::vector<Vector3>& offsets, std::string* error, bool record){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (points.empty()){
        return fail(error, "no points given");
    }
    if (points.size() != offsets.size()){
        return fail(error, std::to_string(points.size()) + " points but " + std::to_string(offsets.size()) +
                    " offsets: give one offset per point");
    }
    if (!checkPoints(points, pointCountOf(*lattice), error)) return false;

    std::vector<Vector3> result = lattice->offsets;
    for (size_t n = 0; n < points.size(); n++){
        result[(size_t)points[n]] = offsets[n];
    }
    return pushOffsets(project, sceneId, entity, *lattice, result, false, record, error, points);
}

bool LatticeEditorOps::setOffsetsContinuous(Project* project, uint32_t sceneId, Entity entity,
                                            const std::vector<int>& points, const std::vector<Vector3>& offsets, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (points.size() != offsets.size()){
        return fail(error, std::to_string(points.size()) + " points but " + std::to_string(offsets.size()) +
                    " offsets: give one offset per point");
    }
    if (!checkPoints(points, pointCountOf(*lattice), error)) return false;

    std::vector<Vector3> result = lattice->offsets;
    for (size_t n = 0; n < points.size(); n++){
        result[(size_t)points[n]] = offsets[n];
    }
    return pushOffsets(project, sceneId, entity, *lattice, result, true, true, error);
}

void LatticeEditorOps::endContinuousEdit(uint32_t sceneId){
    CommandHandle::get(sceneId)->mark();
    // a recorded drag: one key edit per drag
    ClipRecordHooks::endGesture(RecordSource::LatticeDrag);
}

bool LatticeEditorOps::transformPoints(Project* project, uint32_t sceneId, Entity entity,
                                       const std::vector<int>& points, const Matrix4& localTransform,
                                       const Vector3& pivot, const LatticeFalloff& falloff, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (!checkPoints(points, pointCountOf(*lattice), error)) return false;
    if (!isFinite(localTransform) || !isFinite(pivot)){
        return fail(error, "the transform and the pivot must be finite");
    }
    if (falloff.enabled && !(falloff.radius >= 0.0f && std::isfinite(falloff.radius))){
        return fail(error, "the falloff radius must be a finite number >= 0 (lattice local units)");
    }

    std::vector<Vector3> result = transformedOffsets(lattice->resolutionX, lattice->resolutionY, lattice->resolutionZ,
                                                     lattice->offsets, points, localTransform, pivot, falloff);
    return pushOffsets(project, sceneId, entity, *lattice, result, false, false, error);
}

bool LatticeEditorOps::resetPoints(Project* project, uint32_t sceneId, Entity entity, const std::vector<int>& points, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (!checkPoints(points, pointCountOf(*lattice), error)) return false;

    std::vector<Vector3> result = lattice->offsets;
    if (points.empty()){
        std::fill(result.begin(), result.end(), Vector3(0, 0, 0));
    }else{
        for (int index : points){
            result[(size_t)index] = Vector3(0, 0, 0);
        }
    }
    return pushOffsets(project, sceneId, entity, *lattice, result, false, false, error);
}

bool LatticeEditorOps::applyPreset(Project* project, uint32_t sceneId, Entity entity,
                                   const lattice::PresetParams& params, const std::vector<int>& points, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (!checkPoints(points, pointCountOf(*lattice), error)) return false;
    if ((int)params.preset < (int)lattice::LatticePreset::SQUASH || (int)params.preset > (int)lattice::LatticePreset::NOISE){
        return fail(error, "unknown preset: use squash, stretch, taper, twist, bend, bulge, shear or noise");
    }
    if ((int)params.axis < 0 || (int)params.axis > 2 || (int)params.toward < 0 || (int)params.toward > 2){
        return fail(error, "axis and toward must be x, y or z");
    }
    if (!std::isfinite(params.amount)){
        return fail(error, "the preset amount must be a finite number");
    }
    if ((params.preset == lattice::LatticePreset::SQUASH) && (params.amount < 0.0f || params.amount > 0.95f)){
        return fail(error, "squash amount must be between 0 and 0.95");
    }
    if ((params.preset == lattice::LatticePreset::BEND || params.preset == lattice::LatticePreset::SHEAR) && params.toward == params.axis){
        return fail(error, std::string(lattice::presetName(params.preset)) + " needs a toward axis other than its axis");
    }

    // bend and twist in the cage's proportions (spec §12 Agent UX pass): true angles in the world
    lattice::PresetParams proportional = params;
    if (const Transform* transform = sceneProject->scene->findComponent<Transform>(entity)){
        proportional.proportions = lattice::presetProportions(transform->modelMatrix);
    }
    std::vector<Vector3> result = lattice->offsets;
    lattice::applyPreset(lattice->resolutionX, lattice->resolutionY, lattice->resolutionZ, result, proportional, points);
    return pushOffsets(project, sceneId, entity, *lattice, result, false, false, error);
}

bool LatticeEditorOps::mirror(Project* project, uint32_t sceneId, Entity entity, lattice::LatticeAxis axis, bool fromPositive, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if ((int)axis < 0 || (int)axis > 2){
        return fail(error, "mirror axis must be x, y or z");
    }

    std::vector<Vector3> result = lattice->offsets;
    lattice::mirror(lattice->resolutionX, lattice->resolutionY, lattice->resolutionZ, result, axis, fromPositive);
    return pushOffsets(project, sceneId, entity, *lattice, result, false, false, error);
}

bool LatticeEditorOps::setResolution(Project* project, uint32_t sceneId, Entity entity, int rx, int ry, int rz, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    // not latticeOf: setting the resolution is how a broken layout (offsets of another size,
    // e.g. a prefab instance overriding one resolution axis while the prefab changed another)
    // gets repaired, as the other ops' error message says
    LatticeComponent* lattice = nullptr;
    if (entity == NULL_ENTITY || !sceneProject->scene->isEntityCreated(entity)){
        return fail(error, "entity " + std::to_string(entity) + " does not exist in this scene");
    }
    lattice = sceneProject->scene->findComponent<LatticeComponent>(entity);
    if (!lattice){
        return fail(error, entityLabel(sceneProject->scene, entity) + " is not a lattice (it has no Lattice component)");
    }
    if (!validResolution(rx) || !validResolution(ry) || !validResolution(rz)){
        return fail(error, "resolution must be " + std::to_string(LATTICE_MIN_RESOLUTION) + ".." + std::to_string(LATTICE_MAX_RESOLUTION) +
                    " points per axis, not " + std::to_string(rx) + "x" + std::to_string(ry) + "x" + std::to_string(rz));
    }
    const int oldX = std::clamp(lattice->resolutionX, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
    const int oldY = std::clamp(lattice->resolutionY, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
    const int oldZ = std::clamp(lattice->resolutionZ, LATTICE_MIN_RESOLUTION, LATTICE_MAX_RESOLUTION);
    const bool broken = oldX != lattice->resolutionX || oldY != lattice->resolutionY || oldZ != lattice->resolutionZ ||
                        (int)lattice->offsets.size() != oldX * oldY * oldZ;
    if (!broken && rx == lattice->resolutionX && ry == lattice->resolutionY && rz == lattice->resolutionZ){
        return true;
    }

    // the rest offsets are resampled, not a preview's (a layout change takes no shown-value
    // merge in LatticePointsCmd): the previews of the lattice restore first (clips 9.R7), after
    // the checks (a refused or unchanged resolution leaves a running preview and Record alone)
    PoseEditSession::suspendActiveFor(sceneId, {entity});
    lattice = sceneProject->scene->findComponent<LatticeComponent>(entity);
    if (!lattice){
        return fail(error, entityLabel(sceneProject->scene, entity) + " is not a lattice (it has no Lattice component)");
    }

    // the runtime's resampling (Lattice:setResolution in Lua gives the same offsets and poses);
    // a broken layout counts its missing points as rest and drops the extra ones, as the solver does
    LatticeComponent resampled;
    resampled.resolutionX = oldX;
    resampled.resolutionY = oldY;
    resampled.resolutionZ = oldZ;
    resampled.offsets = lattice->offsets;
    resampled.offsets.resize((size_t)(oldX * oldY * oldZ), Vector3(0, 0, 0));
    resampled.poses = lattice->poses;
    if (rx != oldX || ry != oldY || rz != oldZ){
        LatticeSystem::setResolution(resampled, rx, ry, rz);
    }
    LatticePointsState state = stateOf(resampled);
    if ((int)state.offsets.size() != rx * ry * rz){
        return fail(error, "resampling the offsets failed");
    }

    if (!pushState(project, sceneId, entity, state, error)){
        return false;
    }
    // the indices name other points now
    if (getSelectedPoints(sceneId, entity).size() > 0){
        clearSelectedPoints(sceneId);
    }
    return true;
}

bool LatticeEditorOps::computeFit(Scene* scene, const std::vector<Entity>& targets, float padding, LatticeFit& fit, std::string* error){
    if (!scene){
        return fail(error, "no scene");
    }
    if (targets.empty()){
        return fail(error, "no targets to fit to");
    }
    if (!std::isfinite(padding) || padding < 0.0f){
        return fail(error, "padding must be a finite fraction >= 0 (0.02 = 2%)");
    }

    std::vector<Entity> meshes;
    for (Entity target : targets){
        if (target == NULL_ENTITY || !scene->isEntityCreated(target)){
            return fail(error, "target entity " + std::to_string(target) + " does not exist in this scene");
        }
        for (Entity mesh : meshesOf(scene, target)){
            if (std::find(meshes.begin(), meshes.end(), mesh) == meshes.end()){
                meshes.push_back(mesh);
            }
        }
    }
    if (meshes.empty()){
        return fail(error, targets.size() == 1
            ? entityLabel(scene, targets[0]) + " has no mesh to fit to (nor do its children)"
            : "none of the targets has a mesh to fit to (nor do their children)");
    }

    // one target: the cage takes its world rotation, around its own bounds
    Matrix4 frame;
    Quaternion rotation;
    Vector3 frameScale(1, 1, 1);
    if (targets.size() == 1){
        if (Transform* transform = scene->findComponent<Transform>(targets[0])){
            rotation = transform->worldRotation;
            frameScale = Vector3(std::fabs(transform->worldScale.x), std::fabs(transform->worldScale.y), std::fabs(transform->worldScale.z));
            if (frameScale.x > 0.0f && frameScale.y > 0.0f && frameScale.z > 0.0f && isFinite(frameScale)){
                frame = Matrix4::translateMatrix(transform->worldPosition) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(frameScale);
            }else{
                // zero scale: no frame to measure in, fit in the world instead
                rotation = Quaternion();
                frameScale = Vector3(1, 1, 1);
            }
        }
    }

    AABB bounds = boundsIn(scene, meshes, frame.inverse());
    if (bounds.isNull() || bounds.isInfinite()){
        return fail(error, "the targets' bounds are empty");
    }

    Vector3 size = bounds.getSize() * frameScale;
    Vector3 center = frame * bounds.getCenter();
    if (!isFinite(size) || !isFinite(center)){
        return fail(error, "the targets' bounds are not finite");
    }

    const float largest = std::max(size.x, std::max(size.y, size.z));
    const float flatLimit = std::max(largest * 1e-3f, 1e-6f);
    float extents[3] = {size.x, size.y, size.z};
    for (int axis = 0; axis < 3; axis++){
        if (extents[axis] <= flatLimit){
            // flat (a sprite, a plane): a thin cage with points on both sides only
            extents[axis] = std::max(largest * 0.1f, 0.01f);
            fit.resolution[axis] = 2;
        }else{
            extents[axis] = extents[axis] + std::max(extents[axis] * padding, 0.01f);
            fit.resolution[axis] = 3;
        }
    }

    fit.position = center;
    fit.rotation = rotation;
    fit.scale = Vector3(extents[0], extents[1], extents[2]);
    return true;
}

bool LatticeEditorOps::fitTo(Project* project, uint32_t sceneId, Entity entity, const std::vector<Entity>& targets, float padding, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    Scene* scene = sceneProject->scene;
    if (!latticeOf(scene, entity, error)) return false;
    if (!scene->findComponent<Transform>(entity)){
        return fail(error, entityLabel(scene, entity) + " has no Transform");
    }
    if (std::find(targets.begin(), targets.end(), entity) != targets.end()){
        return fail(error, "a lattice cannot be fitted to itself");
    }

    LatticeFit fit;
    if (!computeFit(scene, targets, padding, fit, error)){
        return false;
    }

    Matrix4 local = fitLocalMatrix(scene, entity, fit);
    if (!isFinite(local)){
        return fail(error, "the lattice's parent transform cannot hold the fit (zero scale?)");
    }
    return push(sceneId, new ObjectTransformCmd(project, sceneId, entity, local)) ? true : fail(error, "the lattice could not be moved");
}

bool LatticeEditorOps::bind(Project* project, uint32_t sceneId, Entity entity, Entity target, const LatticeBinding& binding,
                            bool includeChildren, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    Scene* scene = sceneProject->scene;
    if (!latticeOf(scene, entity, error)) return false;

    if (target == NULL_ENTITY || !scene->isEntityCreated(target)){
        return fail(error, "target entity " + std::to_string(target) + " does not exist in this scene");
    }
    if (target == entity){
        return fail(error, "a lattice cannot deform itself: bind it to an entity with a mesh");
    }
    if (!scene->findComponent<Transform>(target)){
        return fail(error, entityLabel(scene, target) + " has no Transform, so nothing of it can be deformed");
    }
    if (!validBinding(binding, error)) return false;

    std::vector<LatticeBinding> bindings = boundList(scene, target, entity, binding);
    LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(target);
    if (modifier && modifier->includeChildren == includeChildren && sameBindings(modifier->bindings, bindings)){
        return true;
    }
    return push(sceneId, new LatticeBindingsCmd(project, sceneId, target, bindings, includeChildren))
        ? true : fail(error, "the binding could not be added");
}

bool LatticeEditorOps::unbind(Project* project, uint32_t sceneId, Entity entity, Entity target, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    Scene* scene = sceneProject->scene;

    if (target == NULL_ENTITY || !scene->isEntityCreated(target)){
        return fail(error, "target entity " + std::to_string(target) + " does not exist in this scene");
    }
    LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(target);
    if (!modifier){
        return fail(error, entityLabel(scene, target) + " has no Lattice Modifier: it is not bound to any lattice");
    }
    std::vector<LatticeBinding> bindings = modifier->bindings;
    bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [entity](const LatticeBinding& b){
        return b.lattice == entity;
    }), bindings.end());
    if (bindings.size() == modifier->bindings.size()){
        return fail(error, entityLabel(scene, target) + " is not bound to " + entityLabel(scene, entity));
    }
    return push(sceneId, new LatticeBindingsCmd(project, sceneId, target, bindings, modifier->includeChildren))
        ? true : fail(error, "the binding could not be removed");
}

bool LatticeEditorOps::follow(Project* project, uint32_t sceneId, Entity entity, Entity follower, Entity rest,
                              Entity* restOut, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    Scene* scene = sceneProject->scene;
    if (!latticeOf(scene, entity, error)) return false;

    if (follower == NULL_ENTITY || !scene->isEntityCreated(follower)){
        return fail(error, "follower entity " + std::to_string(follower) + " does not exist in this scene");
    }
    const Transform* transform = scene->findComponent<Transform>(follower);
    if (!transform){
        return fail(error, entityLabel(scene, follower) + " has no Transform: nothing to place");
    }
    if (follower == entity || isDescendantOf(scene, entity, follower)){
        return fail(error, "the lattice is " + entityLabel(scene, follower) + " or below it: the follower would move its own cage. "
                    "Make the follower a GameObject outside the lattice's parents");
    }
    // a target is deformed by the lattice; moving it with the cage too would deform it twice
    for (Entity target : findTargets(scene, entity)){
        const LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(target);
        if (target == follower || (modifier && modifier->includeChildren && isDescendantOf(scene, follower, target))){
            return fail(error, entityLabel(scene, follower) + " is deformed by this lattice (bound" +
                        std::string(target == follower ? "" : " through " + entityLabel(scene, target)) +
                        "): a follower is moved by the cage, a target is bent by it, not both. Follow with a GameObject that is not bound");
        }
    }
    const LatticeFollowerComponent* existing = scene->findComponent<LatticeFollowerComponent>(follower);
    if (rest == NULL_ENTITY && existing && existing->rest != NULL_ENTITY && scene->isEntityCreated(existing->rest) &&
            existing->rest != follower && !isDescendantOf(scene, existing->rest, follower)){
        rest = existing->rest;
    }
    if (rest != NULL_ENTITY){
        if (!scene->isEntityCreated(rest) || !scene->findComponent<Transform>(rest)){
            return fail(error, "rest entity " + std::to_string(rest) + " does not exist in this scene (or has no Transform)");
        }
        if (rest == follower || isDescendantOf(scene, rest, follower)){
            return fail(error, "the rest is " + entityLabel(scene, follower) + " or below it (the follower would move it): "
                        "omit rest to get an empty sibling where it stands");
        }
    }

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    if (!existing && !push(sceneId, new AddComponentCmd(project, sceneId, follower, ComponentType::LatticeFollowerComponent))){
        return fail(error, "the Lattice Follower could not be added");
    }
    auto* multi = new MultiPropertyCmd();
    if (rest == NULL_ENTITY){
        // the inspector's Create Rest Here: an empty sibling where the follower stands
        auto* create = new CreateEntityCmd(project, sceneId, scene->getEntityName(follower) + " Rest", EntityCreationType::GAMEOBJECT, transform->parent);
        create->setQuiet(true);
        if (!push(sceneId, create)){
            delete multi;
            history->groupSince(mark);
            history->undo();
            return fail(error, "the rest GameObject could not be created");
        }
        rest = create->getEntity();
        const Transform* now = scene->findComponent<Transform>(follower);
        multi->addPropertyCmd<Vector3>(project, sceneId, rest, ComponentType::Transform, "position", now->position);
        multi->addPropertyCmd<Quaternion>(project, sceneId, rest, ComponentType::Transform, "rotation", now->rotation);
        multi->addPropertyCmd<Vector3>(project, sceneId, rest, ComponentType::Transform, "scale", now->scale);
    }
    multi->addPropertyCmd<Entity>(project, sceneId, follower, ComponentType::LatticeFollowerComponent, "lattice", entity);
    multi->addPropertyCmd<Entity>(project, sceneId, follower, ComponentType::LatticeFollowerComponent, "rest", rest);
    push(sceneId, multi);
    history->groupSince(mark);
    if (restOut) *restOut = rest;
    return true;
}

std::vector<Entity> LatticeEditorOps::findTargets(Scene* scene, Entity lattice){
    std::vector<Entity> targets;
    if (!scene || lattice == NULL_ENTITY){
        return targets;
    }
    auto modifiers = scene->getComponentArray<LatticeModifierComponent>();
    for (size_t i = 0; i < modifiers->size(); i++){
        const LatticeModifierComponent& modifier = modifiers->getComponentFromIndex(i);
        for (const LatticeBinding& binding : modifier.bindings){
            if (binding.lattice == lattice){
                targets.push_back(modifiers->getEntity(i));
                break;
            }
        }
    }
    return targets;
}

// --- poses ---

bool LatticeEditorOps::savePose(Project* project, uint32_t sceneId, Entity entity, const std::string& name, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (name.empty()){
        return fail(error, "a pose needs a name");
    }
    LatticePointsState state = stateOf(*lattice);
    bool found = false;
    for (LatticePose& pose : state.poses){
        if (pose.name == name){
            if (pose.offsets == lattice->offsets){
                return true;
            }
            pose.offsets = lattice->offsets;
            found = true;
            break;
        }
    }
    if (!found){
        state.poses.push_back({name, lattice->offsets});
    }
    return pushState(project, sceneId, entity, state, error);
}

bool LatticeEditorOps::applyPose(Project* project, uint32_t sceneId, Entity entity, const std::string& name, float weight, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (!std::isfinite(weight)){
        return fail(error, "the pose weight must be a finite number");
    }
    LatticePose* pose = findPose(*lattice, name);
    if (!pose){
        std::string names;
        for (const LatticePose& p : lattice->poses){
            names += (names.empty() ? "" : ", ") + p.name;
        }
        return fail(error, "no pose named '" + name + "'" + (names.empty() ? std::string(": the lattice has no poses") : ": poses are " + names));
    }
    // a pose of another size is resampled as the runtime does (LatticeSystem::applyPose)
    std::vector<Vector3> result = LatticeSystem::getPoseOffsets(*lattice, *pose);
    for (Vector3& offset : result){
        offset = offset * weight;
    }
    return pushOffsets(project, sceneId, entity, *lattice, result, false, false, error);
}

bool LatticeEditorOps::renamePose(Project* project, uint32_t sceneId, Entity entity, const std::string& name, const std::string& newName, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (!findPose(*lattice, name)){
        return fail(error, "no pose named '" + name + "'");
    }
    if (newName.empty()){
        return fail(error, "a pose needs a name");
    }
    if (newName == name){
        return true;
    }
    if (findPose(*lattice, newName)){
        return fail(error, "a pose named '" + newName + "' already exists");
    }
    LatticePointsState state = stateOf(*lattice);
    for (LatticePose& pose : state.poses){
        if (pose.name == name){
            pose.name = newName;
        }
    }
    return pushState(project, sceneId, entity, state, error);
}

bool LatticeEditorOps::deletePose(Project* project, uint32_t sceneId, Entity entity, const std::string& name, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return false;
    LatticeComponent* lattice = latticeOf(sceneProject->scene, entity, error);
    if (!lattice) return false;

    if (!findPose(*lattice, name)){
        return fail(error, "no pose named '" + name + "'");
    }
    LatticePointsState state = stateOf(*lattice);
    state.poses.erase(std::remove_if(state.poses.begin(), state.poses.end(), [&name](const LatticePose& pose){
        return pose.name == name;
    }), state.poses.end());
    return pushState(project, sceneId, entity, state, error);
}

// --- creation ---

Entity LatticeEditorOps::createLattice(Project* project, uint32_t sceneId, const std::vector<Entity>& targets,
                                       const Vector3& fallbackPosition, const std::string& name, std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId, error);
    if (!sceneProject) return NULL_ENTITY;
    Scene* scene = sceneProject->scene;

    for (Entity target : targets){
        if (target == NULL_ENTITY || !scene->isEntityCreated(target)){
            fail(error, "target entity " + std::to_string(target) + " does not exist in this scene");
            return NULL_ENTITY;
        }
    }
    if (!isFinite(fallbackPosition)){
        fail(error, "the lattice position must be finite");
        return NULL_ENTITY;
    }

    // what gets bound: the targets with a mesh of their own or below them
    std::vector<Entity> bound;
    for (Entity target : targets){
        if (!meshesOf(scene, target).empty() && std::find(bound.begin(), bound.end(), target) == bound.end()){
            bound.push_back(target);
        }
    }

    LatticeFit fit;
    if (bound.empty() || !computeFit(scene, bound, 0.02f, fit, nullptr)){
        // nothing to fit to: a unit cage at the given place (2D and UI scenes measure in
        // pixels, where one unit would not show: a 100 unit cage there)
        bound.clear();
        fit = LatticeFit();
        fit.position = fallbackPosition;
        fit.scale = sceneProject->sceneType == SceneType::SCENE_3D ? Vector3(1, 1, 1) : Vector3(100, 100, 100);
    }

    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();

    CreateEntityCmd* create = new CreateEntityCmd(project, sceneId, name.empty() ? std::string("Lattice") : name, EntityCreationType::GAMEOBJECT);
    create->addProperty<Vector3>(ComponentType::Transform, "position", fit.position);
    create->addProperty<Quaternion>(ComponentType::Transform, "rotation", fit.rotation);
    create->addProperty<Vector3>(ComponentType::Transform, "scale", fit.scale);
    if (!push(sceneId, create)){
        fail(error, "the lattice entity could not be created");
        return NULL_ENTITY;
    }
    Entity entity = create->getEntity();

    push(sceneId, new AddComponentCmd(project, sceneId, entity, ComponentType::LatticeComponent));
    if (!scene->findComponent<LatticeComponent>(entity)){
        history->groupSince(mark);
        history->undo();
        fail(error, "the Lattice component could not be added");
        return NULL_ENTITY;
    }

    if (fit.resolution[0] != 3 || fit.resolution[1] != 3 || fit.resolution[2] != 3){
        LatticePointsState state;
        state.resolutionX = fit.resolution[0];
        state.resolutionY = fit.resolution[1];
        state.resolutionZ = fit.resolution[2];
        state.offsets.assign((size_t)(state.resolutionX * state.resolutionY * state.resolutionZ), Vector3(0, 0, 0));
        push(sceneId, new LatticePointsCmd(project, sceneId, entity, state));
    }

    // a group without a mesh of its own is deformed through its children
    auto includesChildren = [scene](Entity target){
        LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(target);
        return (modifier && modifier->includeChildren) || !hasMesh(scene, target);
    };
    for (Entity target : bound){
        // a target below another one that includes its children already inherits this
        // lattice: binding it too would deform it twice (own + inherited, spec §2.1)
        bool inherited = false;
        for (Entity other : bound){
            if (other != target && includesChildren(other) && isDescendantOf(scene, target, other)){
                inherited = true;
                break;
            }
        }
        if (inherited){
            continue;
        }
        bool includeChildren = includesChildren(target);
        LatticeBinding binding;
        binding.lattice = entity;
        std::vector<LatticeBinding> bindings = boundList(scene, target, entity, binding);
        push(sceneId, new LatticeBindingsCmd(project, sceneId, target, bindings, includeChildren));
    }

    history->groupSince(mark);

    clearSelectedPoints(sceneId);
    project->setSelectedEntity(sceneId, entity);
    return entity;
}
