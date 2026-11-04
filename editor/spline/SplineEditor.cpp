// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The stroke finalisation and the extend/close rules follow OpenEngine's SplineTool.cpp (MIT, see
// the header and NOTICE.md).

#include "SplineEditor.h"
#include "component/Transform.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "picking/MeshRaycast.h"
#include "spline/SplineEvaluator.h"
#include "spline/SplineStroke.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/SplineSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    SceneProject* sceneOf(Project* project, uint32_t sceneId){
        return project ? project->getScene(sceneId) : nullptr;
    }

    SplineComponent* splineOf(Project* project, uint32_t sceneId, Entity entity){
        SceneProject* sceneProject = sceneOf(project, sceneId);
        return sceneProject && sceneProject->scene->isEntityCreated(entity) ? sceneProject->scene->findComponent<SplineComponent>(entity) : nullptr;
    }

    Matrix4 worldOf(Project* project, uint32_t sceneId, Entity entity){
        SceneProject* sceneProject = sceneOf(project, sceneId);
        const Transform* transform = sceneProject ? sceneProject->scene->findComponent<Transform>(entity) : nullptr;
        return transform ? transform->modelMatrix : Matrix4();
    }

    // the linear part of an affine matrix applied to a direction
    Vector3 direction(const Matrix4& m, const Vector3& v){
        return m * v - m * Vector3(0.0f, 0.0f, 0.0f);
    }

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    float largestAxisScale(const Matrix4& m){
        const float sx = direction(m, Vector3(1.0f, 0.0f, 0.0f)).length();
        const float sy = direction(m, Vector3(0.0f, 1.0f, 0.0f)).length();
        const float sz = direction(m, Vector3(0.0f, 0.0f, 1.0f)).length();
        return std::max(sx, std::max(sy, sz));
    }

}

editor::SplineEditor& editor::SplineEditor::get(){
    static SplineEditor instance;
    return instance;
}

void editor::SplineEditor::beginEdit(Project* project, uint32_t sceneId, Entity entity){
    if (editing && (this->sceneId != sceneId || this->entity != entity)){
        endEdit(project);
    }
    this->sceneId = sceneId;
    this->entity = entity;
    editing = true;
    selection.clear();
    status.clear();
    if (liveTerrain){
        std::string error;
        if (!liveSession.begin(project, sceneId, entity, error)){
            status = error;
        }
    }
}

void editor::SplineEditor::endEdit(Project* project){
    if (liveSession.isActive()){
        liveSession.end();
    }
    if (editing){
        endContinuousEdit(sceneId);
    }
    editing = false;
    selection.clear();
    if (SceneProject* sceneProject = sceneOf(project, sceneId)){
        if (std::shared_ptr<SplineSystem> system = sceneProject->scene->findSystem<SplineSystem>()){
            system->setInteractiveEdit(false);
        }
    }
}

void editor::SplineEditor::setSelection(std::vector<uint32_t> knots){
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
    selection = std::move(knots);
}

void editor::SplineEditor::toggleSelected(uint32_t knot){
    auto it = std::lower_bound(selection.begin(), selection.end(), knot);
    if (it != selection.end() && *it == knot){
        selection.erase(it);
    }else{
        selection.insert(it, knot);
    }
}

bool editor::SplineEditor::isSelected(uint32_t knot) const{
    return std::binary_search(selection.begin(), selection.end(), knot);
}

void editor::SplineEditor::validateSelection(size_t knotCount){
    selection.erase(std::remove_if(selection.begin(), selection.end(), [knotCount](uint32_t k){ return k >= knotCount; }), selection.end());
}

bool editor::SplineEditor::setState(Project* project, uint32_t sceneId, Entity entity, SplineEditState state, SplineEditKind kind,
                                    bool continuous, const std::vector<uint32_t>& selection){
    if (!splineOf(project, sceneId, entity)){
        return false;
    }
    SplineEditCmd* cmd = new SplineEditCmd(project, sceneId, entity, std::move(state), kind, continuous, selection);
    CommandHistory* history = CommandHandle::get(sceneId);
    return continuous ? history->addCommand(cmd) : history->addCommandNoMerge(cmd);
}

void editor::SplineEditor::endContinuousEdit(uint32_t sceneId){
    CommandHandle::get(sceneId)->mark();
}

std::vector<Vector3> editor::SplineEditor::worldKnots(Project* project) const{
    std::vector<Vector3> points;
    const SplineComponent* spline = splineOf(project, sceneId, entity);
    if (!spline){
        return points;
    }
    const Matrix4 world = worldOf(project, sceneId, entity);
    for (const spline::SplineKnot& knot : spline->spline.knots()){
        points.push_back(world * knot.position);
    }
    return points;
}

Vector3 editor::SplineEditor::selectionCentroid(Project* project) const{
    const std::vector<Vector3> points = worldKnots(project);
    Vector3 sum(0.0f, 0.0f, 0.0f);
    size_t count = 0;
    for (uint32_t k : selection){
        if (k < points.size()){
            sum += points[k];
            count++;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : sum;
}

AABB editor::SplineEditor::selectionBounds(Project* project) const{
    AABB bounds;
    const std::vector<Vector3> points = worldKnots(project);
    for (size_t i = 0; i < points.size(); i++){
        if (selection.empty() || isSelected(static_cast<uint32_t>(i))){
            bounds.merge(points[i]);
        }
    }
    return bounds;
}

bool editor::SplineEditor::transformSelected(Project* project, const Matrix4& worldTransform, const Vector3& pivot, bool scaleRadius,
                                             bool continuous, std::string* error){
    SplineComponent* spline = splineOf(project, sceneId, entity);
    if (!spline){
        return fail(error, "No spline is being edited.");
    }
    validateSelection(spline->spline.knotCount());
    if (selection.empty()){
        return fail(error, "No knot is selected.");
    }
    const Matrix4 world = worldOf(project, sceneId, entity);
    const Matrix4 local = world.inverse();
    SplineEditState state = SplineEditCmd::capture(*spline);
    const bool one = selection.size() == 1;
    for (uint32_t k : selection){
        spline::SplineKnot& knot = state.knots[k];
        const Vector3 position = world * knot.position;
        if (one && scaleRadius){
            // Scale on a single knot scales its radius, not its place
            knot.radius = std::max(0.0f, knot.radius * largestAxisScale(worldTransform));
            continue;
        }
        knot.position = local * (worldTransform * (position - pivot) + pivot);
        // handles turn (and scale) with the knots: through world space and back
        knot.tangentIn = direction(local, direction(worldTransform, direction(world, knot.tangentIn)));
        knot.tangentOut = direction(local, direction(worldTransform, direction(world, knot.tangentOut)));
    }
    return setState(project, sceneId, entity, std::move(state), SplineEditKind::MoveKnots, continuous, selection);
}

bool editor::SplineEditor::setTangent(Project* project, uint32_t knot, bool out, const Vector3& worldTip, bool continuous){
    SplineComponent* spline = splineOf(project, sceneId, entity);
    if (!spline || knot >= spline->spline.knotCount()){
        return false;
    }
    const Matrix4 world = worldOf(project, sceneId, entity);
    SplineEditState state = SplineEditCmd::capture(*spline);
    spline::SplineKnot& k = state.knots[knot];
    const Vector3 handle = world.inverse() * worldTip - k.position;
    if (out){
        k.tangentOut = handle;
        spline::applyTangentMode(k, spline::SplineHandle::Out);
    }else{
        k.tangentIn = handle;
        spline::applyTangentMode(k, spline::SplineHandle::In);
    }
    return setState(project, sceneId, entity, std::move(state), SplineEditKind::EditTangent, continuous, {knot});
}

bool editor::SplineEditor::insertOnCurve(Project* project, const Vector3& worldPoint, std::string* error){
    SplineComponent* spline = splineOf(project, sceneId, entity);
    if (!spline || !spline->spline.isValid()){
        return fail(error, "The spline needs two knots before one can be inserted.");
    }
    const Matrix4 world = worldOf(project, sceneId, entity);
    const Vector3 local = world.inverse() * worldPoint;
    const spline::SplineHit hit = spline::closestPoint(spline->spline, local);
    uint32_t segment = 0;
    float u = 0.0f;
    spline::tToSegment(spline->spline, hit.t, segment, u);
    const spline::SplineFrame frame = spline::evaluateSegment(spline->spline, segment, u);

    SplineEditState state = SplineEditCmd::capture(*spline);
    const size_t count = state.knots.size();
    const size_t a = segment % count;
    const size_t b = (segment + 1) % count;
    spline::SplineKnot knot = state.knots[a];
    knot.position = frame.position;
    knot.radius = frame.radius;
    knot.rollDeg = frame.rollDeg;
    knot.tangentIn = Vector3(0.0f, 0.0f, 0.0f);
    knot.tangentOut = Vector3(0.0f, 0.0f, 0.0f);
    if (state.type == spline::SplineType::CubicBezier){
        // de Casteljau at u: the two halves draw the same curve as the segment did
        const Vector3 p0 = state.knots[a].position;
        const Vector3 p1 = p0 + state.knots[a].tangentOut;
        const Vector3 p3 = state.knots[b].position;
        const Vector3 p2 = p3 + state.knots[b].tangentIn;
        const Vector3 q0 = p0 + (p1 - p0) * u;
        const Vector3 q1 = p1 + (p2 - p1) * u;
        const Vector3 q2 = p2 + (p3 - p2) * u;
        const Vector3 r0 = q0 + (q1 - q0) * u;
        const Vector3 r1 = q1 + (q2 - q1) * u;
        const Vector3 s = r0 + (r1 - r0) * u;
        knot.position = s;
        knot.tangentIn = r0 - s;
        knot.tangentOut = r1 - s;
        knot.mode = spline::TangentMode::Aligned;
        state.knots[a].tangentOut = q0 - p0;
        state.knots[b].tangentIn = q2 - p3;
        if (state.knots[a].mode == spline::TangentMode::Mirrored) state.knots[a].mode = spline::TangentMode::Aligned;
        if (state.knots[b].mode == spline::TangentMode::Mirrored) state.knots[b].mode = spline::TangentMode::Aligned;
    }
    const size_t index = a + 1;
    state.knots.insert(state.knots.begin() + static_cast<std::ptrdiff_t>(index), knot);
    selection = {static_cast<uint32_t>(index)};
    return setState(project, sceneId, entity, std::move(state), SplineEditKind::InsertKnot);
}

bool editor::SplineEditor::extendTo(Project* project, const Vector3& worldPoint, std::string* error){
    SplineComponent* spline = splineOf(project, sceneId, entity);
    if (!spline){
        return fail(error, "No spline is being edited.");
    }
    const Matrix4 world = worldOf(project, sceneId, entity);
    const Vector3 local = world.inverse() * worldPoint;
    SplineEditState state = SplineEditCmd::capture(*spline);
    spline::SplineKnot knot;
    bool atStart = false;
    if (!state.knots.empty()){
        const Vector3 first = state.knots.front().position;
        const Vector3 last = state.knots.back().position;
        atStart = !state.closed && (local - first).squaredLength() < (local - last).squaredLength();
        knot = atStart ? state.knots.front() : state.knots.back();
        knot.tangentIn = Vector3(0.0f, 0.0f, 0.0f);
        knot.tangentOut = Vector3(0.0f, 0.0f, 0.0f);
    }
    knot.position = local;
    if (state.type == spline::SplineType::CubicBezier && !state.knots.empty()){
        // handles a third of the way to the neighbour, the Unity default for a new knot
        const Vector3 neighbour = atStart ? state.knots.front().position : state.knots.back().position;
        const Vector3 along = (local - neighbour) * (1.0f / 3.0f);
        knot.tangentOut = atStart ? along * -1.0f : along;
        knot.tangentIn = knot.tangentOut * -1.0f;
        knot.mode = spline::TangentMode::Mirrored;
    }
    if (atStart){
        state.knots.insert(state.knots.begin(), knot);
        selection = {0};
    }else{
        state.knots.push_back(knot);
        selection = {static_cast<uint32_t>(state.knots.size() - 1)};
    }
    return setState(project, sceneId, entity, std::move(state), SplineEditKind::InsertKnot);
}

bool editor::SplineEditor::deleteSelected(Project* project, std::string* error){
    SplineComponent* spline = splineOf(project, sceneId, entity);
    if (!spline){
        return fail(error, "No spline is being edited.");
    }
    validateSelection(spline->spline.knotCount());
    if (selection.empty()){
        return fail(error, "No knot is selected.");
    }
    if (spline->spline.knotCount() - selection.size() < spline::SplineData::MIN_KNOTS){
        status = "A spline keeps two knots at least: the knots were not deleted.";
        return fail(error, status);
    }
    SplineEditState state = SplineEditCmd::capture(*spline);
    for (auto it = selection.rbegin(); it != selection.rend(); ++it){
        state.knots.erase(state.knots.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    selection.clear();
    status.clear();
    return setState(project, sceneId, entity, std::move(state), SplineEditKind::DeleteKnots);
}

// ---- Draw Spline

void editor::SplineEditor::beginDraw(uint32_t sceneId, Entity entity){
    drawing = true;
    drawSceneId = sceneId;
    drawEntity = entity;
    stroke.clear();
    clicked.clear();
}

void editor::SplineEditor::cancelDraw(){
    drawing = false;
    stroke.clear();
    clicked.clear();
}

void editor::SplineEditor::addStrokePoint(const Vector3& world){
    if (!stroke.empty() && (stroke.back() - world).squaredLength() < 1.0e-8f){
        return;
    }
    stroke.push_back(world);
}

void editor::SplineEditor::addClickKnot(const Vector3& world, const Vector3& tangentOut){
    spline::SplineKnot knot;
    knot.position = world;
    knot.tangentOut = tangentOut;
    knot.tangentIn = tangentOut * -1.0f;
    clicked.push_back(knot);
}

void editor::SplineEditor::setLastClickTangent(const Vector3& tangentOut){
    if (!clicked.empty()){
        clicked.back().tangentOut = tangentOut;
        clicked.back().tangentIn = tangentOut * -1.0f;
    }
}

bool editor::SplineEditor::removeLastClickKnot(){
    if (clicked.empty()){
        return false;
    }
    clicked.pop_back();
    return true;
}

spline::SplineSurfaceProbe editor::SplineEditor::makeProbe(SceneProject* sceneProject, SplineSnap snap, Entity ignore){
    if (!sceneProject || snap == SplineSnap::None){
        return spline::SplineSurfaceProbe();
    }
    Scene* scene = sceneProject->scene;
    return [scene, snap, ignore](const Vector3& point) -> spline::SplineSurfaceSample{
        spline::SplineSurfaceSample best;
        // the terrain under the point
        auto terrains = scene->getComponentArray<TerrainComponent>();
        for (size_t i = 0; i < terrains->size(); i++){
            const Entity entity = terrains->getEntity(i);
            TerrainComponent& terrain = terrains->getComponentFromIndex(i);
            const Transform* transform = scene->findComponent<Transform>(entity);
            if (!transform || !scene->isEntityActiveInHierarchy(entity) || !terrain.heightMap.hasData()){
                continue;
            }
            const Vector3 local = transform->modelMatrix.inverse() * point;
            const float half = terrain.terrainSize * 0.5f;
            if (std::fabs(local.x) > half || std::fabs(local.z) > half){
                continue;
            }
            float height = 0.0f;
            Vector3 normal;
            MeshSystem::sampleTerrainSurface(terrain, local.x, local.z, height, normal);
            const Vector3 surface = transform->modelMatrix * Vector3(local.x, height, local.z);
            if (!best.hit || surface.y > best.height){
                best.hit = true;
                best.height = surface.y;
                best.normal = direction(transform->modelMatrix, normal).normalized();
            }
        }
        if (snap != SplineSnap::Surface){
            return best;
        }
        // anything drawn (S26 triangle picking): straight down onto the triangles of the meshes,
        // instances and terrains over the point, the spline's own geometry excepted
        std::shared_ptr<SplineSystem> splines = scene->findSystem<SplineSystem>();
        MeshRaycastOptions options;
        options.kinds = MeshRaycast::MESH | MeshRaycast::INSTANCE | MeshRaycast::TERRAIN;
        if (ignore != NULL_ENTITY){
            options.ignore.push_back(ignore);
            if (splines){
                for (Entity generated : splines->getGeneratedEntities(ignore)){
                    options.ignore.push_back(generated);
                }
            }
        }
        constexpr float REACH = 10000.0f;
        const MeshRaycastHit hit = MeshRaycast::raycast(scene, Vector3(point.x, point.y + REACH, point.z), Vector3(0.0f, -1.0f, 0.0f),
                                                        2.0f * REACH, LAYER_MASK_DEFAULT_RAYCAST, options);
        if (hit.hit && (!best.hit || hit.point.y > best.height)){
            best.hit = true;
            best.height = hit.point.y;
            best.normal = hit.normal.y < 0.0f ? -hit.normal : hit.normal;
        }
        return best;
    };
}

bool editor::SplineEditor::applyStroke(Project* project, uint32_t sceneId, Entity entity, const std::vector<Vector3>& worldStroke,
                                       float tolerance, unsigned int smooth, SplineSnap snap, bool extend, float pickRadius,
                                       std::string* error){
    SceneProject* sceneProject = sceneOf(project, sceneId);
    SplineComponent* spline = splineOf(project, sceneId, entity);
    if (!spline){
        return fail(error, "The GameObject has no Spline.");
    }
    const Matrix4 world = worldOf(project, sceneId, entity);
    const Matrix4 toLocal = world.inverse();

    spline::SplineStrokeOptions options;
    options.simplifyTolerance = std::max(tolerance, 1.0e-4f);
    options.smoothIterations = smooth;
    options.upAxis = spline->spline.upAxis();
    options.closeRadius = std::max(pickRadius, options.simplifyTolerance);
    options.knotRadius = spline->spline.knotCount() > 0 ? spline->spline.knot(0).radius : spline::SplineKnot().radius;
    // the snap reads world space; the stroke is shaped in the spline's local space
    const bool is2D = spline->spline.upAxis().z != 0.0f;
    std::vector<Vector3> shapedWorld;
    {
        spline::SplineStrokeShape shape;
        const spline::SplineSurfaceProbe probe = is2D ? spline::SplineSurfaceProbe() : makeProbe(sceneProject, snap, entity);
        if (!spline::shapeStroke(worldStroke, options, probe ? &probe : nullptr, shape)){
            return fail(error, "The stroke is too short: draw at least two points apart.");
        }
        shapedWorld = shape.points;
        SplineEditState state = SplineEditCmd::capture(*spline);
        std::vector<Vector3> local;
        local.reserve(shapedWorld.size());
        for (const Vector3& p : shapedWorld){
            local.push_back(toLocal * p);
        }

        if (extend && !state.knots.empty() && !state.closed){
            spline::SplineData extended = spline->spline;
            // an extension that ends on the spline's other end closes it
            bool closes = false;
            if (local.size() >= 2 && extended.knotCount() >= 2){
                const Vector3& start = local.front();
                const Vector3& first = extended.knots().front().position;
                const Vector3& last = extended.knots().back().position;
                const Vector3& end = local.back();
                const Vector3& otherEnd = (start - last).squaredLength() <= (start - first).squaredLength() ? first : last;
                closes = (end - otherEnd).length() <= pickRadius;
                if (closes){
                    local.pop_back();
                }
            }
            const spline::SplineStrokeExtension extension = spline::extendWithStroke(extended, local, pickRadius);
            if (extension != spline::SplineStrokeExtension::None){
                state.knots = extended.knots();
                if (closes && state.knots.size() >= spline::SplineData::MIN_CLOSED_KNOTS){
                    state.closed = true;
                }
                return setState(project, sceneId, entity, std::move(state), SplineEditKind::Draw);
            }
        }
        // a new curve: CatmullRom through the shaped points
        std::vector<spline::SplineKnot> knots;
        for (const Vector3& p : local){
            spline::SplineKnot knot;
            knot.position = p;
            knot.radius = options.knotRadius;
            knots.push_back(knot);
        }
        state.type = spline::SplineType::CatmullRom;
        state.closed = shape.closed;
        state.knots = std::move(knots);
        return setState(project, sceneId, entity, std::move(state), SplineEditKind::Draw);
    }
}

bool editor::SplineEditor::finishDraw(Project* project, float worldPerPixel, std::string* error){
    if (!drawing){
        return fail(error, "Not drawing.");
    }
    const uint32_t sceneId = drawSceneId;
    const Entity entity = drawEntity;
    SplineComponent* spline = splineOf(project, sceneId, entity);
    if (!spline){
        cancelDraw();
        return fail(error, "The GameObject has no Spline.");
    }
    const bool is2D = spline->spline.upAxis().z != 0.0f;
    bool ok = false;
    if (drawSettings.mode == SplineDrawMode::Freehand){
        const float tolerance = is2D ? drawSettings.tolerance2DPixels * worldPerPixel : drawSettings.tolerance3D;
        const float pick = drawSettings.pickRadiusPixels * worldPerPixel;
        ok = applyStroke(project, sceneId, entity, stroke, tolerance, drawSettings.smoothIterations, drawSettings.snap, true, pick, error);
    }else{
        // Click: the knots as placed; dragged ones carry Bezier handles
        if (clicked.size() < spline::SplineData::MIN_KNOTS){
            cancelDraw();
            return fail(error, "Place two knots at least.");
        }
        const Matrix4 toLocal = worldOf(project, sceneId, entity).inverse();
        SplineEditState state = SplineEditCmd::capture(*spline);
        bool handles = false;
        state.knots.clear();
        for (const spline::SplineKnot& placed : clicked){
            spline::SplineKnot knot = placed;
            knot.position = toLocal * placed.position;
            knot.tangentOut = direction(toLocal, placed.tangentOut);
            knot.tangentIn = direction(toLocal, placed.tangentIn);
            handles = handles || knot.tangentOut.squaredLength() > 0.0f;
            knot.radius = spline->spline.knotCount() > 0 ? spline->spline.knot(0).radius : knot.radius;
            state.knots.push_back(knot);
        }
        state.type = handles ? spline::SplineType::CubicBezier : spline::SplineType::Linear;
        if (handles){
            // knots placed without a drag get handles a third of the way to their neighbours
            for (size_t i = 0; i < state.knots.size(); i++){
                spline::SplineKnot& k = state.knots[i];
                if (k.tangentOut.squaredLength() > 0.0f){
                    continue;
                }
                const Vector3 prev = state.knots[i > 0 ? i - 1 : i].position;
                const Vector3 next = state.knots[i + 1 < state.knots.size() ? i + 1 : i].position;
                k.tangentOut = (next - prev) * (1.0f / 6.0f);
                k.tangentIn = k.tangentOut * -1.0f;
            }
        }
        // the last click on the first knot closes the spline
        const float pick = drawSettings.pickRadiusPixels * worldPerPixel;
        if (state.knots.size() >= spline::SplineData::MIN_CLOSED_KNOTS + 1 &&
            (state.knots.back().position - state.knots.front().position).length() <= pick){
            state.knots.pop_back();
            state.closed = true;
        }
        ok = setState(project, sceneId, entity, std::move(state), SplineEditKind::Draw);
    }
    cancelDraw();
    return ok;
}

// ---- Live Terrain

void editor::SplineEditor::setLiveTerrain(Project* project, bool live){
    if (live == liveTerrain){
        return;
    }
    liveTerrain = live;
    if (!live){
        if (liveSession.isActive()){
            liveSession.end();
        }
        return;
    }
    if (editing){
        std::string error;
        if (!liveSession.begin(project, sceneId, entity, error)){
            status = error;
        }
    }
}

void editor::SplineEditor::update(Project* project){
    if (editing && !splineOf(project, sceneId, entity)){
        // the spline went away (deleted, its scene closed)
        liveSession.cancel();
        editing = false;
        selection.clear();
    }
    if (drawing && !splineOf(project, drawSceneId, drawEntity)){
        cancelDraw();
    }
    if (editing){
        if (const SplineComponent* spline = splineOf(project, sceneId, entity)){
            validateSelection(spline->spline.knotCount());
        }
    }
    if (liveSession.isActive()){
        std::string error;
        liveSession.update(error);
        if (!error.empty()){
            status = error;
        }
    }
}
