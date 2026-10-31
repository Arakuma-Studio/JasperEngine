// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// edit_lattice (spec 2026-10-23-lattice-deformer-design.md §10.2): one tool for lattice
// (free-form deformation) cages. Every edit goes through LatticeEditorOps, the path the Scene
// view, the inspectors and the context menu take, so an agent's edit is undoable, recorded as
// a prefab override and shown live exactly like a person's. Reads (summary, points, samples,
// warnings) use the C++ solver, the math the GPU runs.
#include "McpTools.h"
#include "LatticeShared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/LatticeComponent.h"
#include "component/LatticeFollowerComponent.h"
#include "component/LatticeModifierComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/PolygonComponent.h"
#include "component/SpriteComponent.h"
#include "component/TilemapComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "lattice/LatticeEditorOps.h"
#include "lattice/LatticePresets.h"
#include "lattice/LatticeSolver.h"
#include "lattice/LatticeTypes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

bool lattice_mcp::targetsPending(Project* project, uint32_t sceneId, const std::vector<Entity>& targets) {
    SceneProject* sp = project ? project->getScene(sceneId) : nullptr;
    if (!sp || !sp->scene) return false;
    Scene* scene = sp->scene;
    auto pending = [scene, sceneId](Entity entity) {
        if (!scene->isEntityCreated(entity)) return false;
        if (SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity); sprite && sprite->needUpdateSprite) return true;
        if (TilemapComponent* tilemap = scene->findComponent<TilemapComponent>(entity); tilemap && tilemap->needUpdateTilemap) return true;
        if (PolygonComponent* polygon = scene->findComponent<PolygonComponent>(entity); polygon && polygon->needUpdatePolygon) return true;
        if (ModelComponent* model = scene->findComponent<ModelComponent>(entity); model && model->needUpdateModel) return true;
        return ModelLoadCmd::isLoadPending(sceneId, entity);
    };
    for (Entity target : targets) {
        if (pending(target)) return true;
        for (Entity entity : sp->entities) {
            if (entity != target && scene->isEntityCreated(entity) && scene->isParentOf(target, entity) && pending(entity)) return true;
        }
    }
    return false;
}

namespace {

// at most this many sample points per call, and listed cells per warning
constexpr size_t MAX_SAMPLES = 4096;
constexpr size_t MAX_LISTED_CELLS = 16;

std::string lowerText(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

bool has(const Json& args, const char* key) {
    return args.is_object() && args.contains(key) && !args[key].is_null();
}

bool fail(std::string& error, const std::string& where, const std::string& message) {
    error = where.empty() ? message : where + ": " + message;
    return false;
}

std::string vecText(const Vector3& v) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "[%g, %g, %g]", v.x, v.y, v.z);
    return buffer;
}

std::string label(Scene* scene, Entity entity) {
    const std::string name = scene->isEntityCreated(entity) ? scene->getEntityName(entity) : std::string();
    return "\"" + name + "\" [" + std::to_string(entity) + "]";
}

// One undo step for everything a call pushes on the scene history (WorldTools' UndoStep):
// LatticeEditorOps pushes each edit with setNoMerge, commit() groups them, rollback() undoes
// what the call did when a later part of it fails.
class UndoStep {
public:
    explicit UndoStep(uint32_t sceneId): history(CommandHandle::get(sceneId)), start(history->mark()) {}

    bool add(Command* command) {
        command->setNoMerge();
        return history->addCommand(command);
    }
    bool changed() const { return history->position() > start; }
    void commit() { history->groupSince(start); }
    void rollback() {
        while (history->position() > start && history->canUndo()) history->undo();
    }

private:
    CommandHistory* history;
    size_t start;
};

// ------------------------------------------------------------------ lattice state helpers

struct LatticeView {
    Scene* scene = nullptr;
    Entity entity = NULL_ENTITY;
    LatticeComponent* component = nullptr;
    Matrix4 model;          // the lattice Transform's world matrix
    Matrix4 inverse;        // world -> lattice local
    bool invertible = false;

    int rx() const { return component->resolutionX; }
    int ry() const { return component->resolutionY; }
    int rz() const { return component->resolutionZ; }
    int count() const { return rx() * ry() * rz(); }
    Vector3 rest(int index) const {
        return lattice::restPosition(rx(), ry(), rz(), index % rx(), (index / rx()) % ry(), index / (rx() * ry()));
    }
    Vector3 offset(int index) const {
        return index >= 0 && static_cast<size_t>(index) < component->offsets.size() ? component->offsets[static_cast<size_t>(index)]
                                                                                       : Vector3(0, 0, 0);
    }
    Vector3 local(int index) const { return rest(index) + offset(index); }
};

bool viewOf(Scene* scene, Entity entity, LatticeView& view) {
    view.scene = scene;
    view.entity = entity;
    view.component = scene->findComponent<LatticeComponent>(entity);
    Transform* transform = scene->findComponent<Transform>(entity);
    if (!view.component || !transform) return false;
    view.model = transform->modelMatrix;
    const float det = view.model.linear().determinant();
    view.invertible = std::isfinite(det) && det != 0.0f;
    view.inverse = view.invertible ? view.model.inverse() : Matrix4();
    return true;
}

Json ijkJson(const LatticeView& view, int index) {
    return Json::array({index % view.rx(), (index / view.rx()) % view.ry(), index / (view.rx() * view.ry())});
}

// the entity has something a lattice deforms (terrains are never targets)
bool hasMesh(Scene* scene, Entity entity) {
    return scene->findComponent<MeshComponent>(entity) && !scene->findComponent<TerrainComponent>(entity);
}

bool isLattice(Scene* scene, Entity entity) {
    return entity != NULL_ENTITY && scene->isEntityCreated(entity) && scene->findComponent<LatticeComponent>(entity);
}

// the meshes a modifier on `target` reaches: itself, and its mesh descendants with includeChildren
std::vector<Entity> meshesReached(SceneProject* sp, Entity target, bool includeChildren) {
    Scene* scene = sp->scene;
    std::vector<Entity> meshes;
    if (hasMesh(scene, target)) meshes.push_back(target);
    if (includeChildren) {
        for (Entity entity : sp->entities) {
            if (entity != target && scene->isEntityCreated(entity) && hasMesh(scene, entity) && scene->isParentOf(target, entity)) {
                meshes.push_back(entity);
            }
        }
    }
    return meshes;
}

// lattices that deform `mesh`, as LatticeRender stacks them (spec §2.1): its own bindings in
// order, then those of each ancestor with includeChildren, nearest first; deleted, non-lattice
// and disabled lattices do not count
int latticesOnMesh(Scene* scene, Entity mesh) {
    int count = 0;
    // a lattice a nearer modifier binds is not applied again (LatticeRender)
    std::vector<Entity> nearer;
    auto countOf = [scene, &nearer](const LatticeModifierComponent& modifier) {
        int n = 0;
        if (!modifier.enabled) return 0;
        std::vector<Entity> level;
        for (const LatticeBinding& binding : modifier.bindings) {
            if (!isLattice(scene, binding.lattice)) continue;
            if (std::find(nearer.begin(), nearer.end(), binding.lattice) != nearer.end()) continue;
            if (scene->getComponent<LatticeComponent>(binding.lattice).enabled) {
                n++;
                level.push_back(binding.lattice);
            }
        }
        nearer.insert(nearer.end(), level.begin(), level.end());
        return n;
    };
    if (LatticeModifierComponent* own = scene->findComponent<LatticeModifierComponent>(mesh)) count += countOf(*own);
    Transform* transform = scene->findComponent<Transform>(mesh);
    for (int hops = 0; transform && transform->parent != NULL_ENTITY && hops < 4096; hops++) {
        const Entity parent = transform->parent;
        if (LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(parent)) {
            if (modifier->includeChildren) count += countOf(*modifier);
        }
        transform = scene->findComponent<Transform>(parent);
    }
    return count;
}

// the nearest ancestor of `target` whose enabled modifier hands `lattice` down (includeChildren),
// NULL_ENTITY when none: the target already inherits that lattice
Entity inheritedFrom(Scene* scene, Entity target, Entity lattice) {
    Transform* transform = scene->findComponent<Transform>(target);
    for (int hops = 0; transform && transform->parent != NULL_ENTITY && hops < 4096; hops++) {
        const Entity parent = transform->parent;
        if (LatticeModifierComponent* modifier = scene->findComponent<LatticeModifierComponent>(parent)) {
            if (modifier->enabled && modifier->includeChildren) {
                for (const LatticeBinding& binding : modifier->bindings) {
                    if (binding.lattice == lattice) return parent;
                }
            }
        }
        transform = scene->findComponent<Transform>(parent);
    }
    return NULL_ENTITY;
}

// ------------------------------------------------------------------ names

const char* AXIS_NAMES[] = {"x", "y", "z"};

bool parseAxis(const Json& value, lattice::LatticeAxis& out, const std::string& where, std::string& error) {
    if (value.is_string()) {
        const std::string name = lowerText(value.get<std::string>());
        for (int a = 0; a < 3; a++) {
            if (name == AXIS_NAMES[a]) {
                out = static_cast<lattice::LatticeAxis>(a);
                return true;
            }
        }
    }
    return fail(error, where, value.dump() + " is not an axis. Use one of: x, y, z.");
}

bool parseEnum(const char* table, const Json& value, int& out, const std::string& where, std::string& error) {
    const McpValues::EnumOptions* options = McpValues::enumTable(table);
    std::string why;
    if (!options || !McpValues::enumFromJson(value, *options, out, why)) {
        std::vector<std::string> names;
        if (options) {
            for (const McpValues::EnumOption& option : *options) names.push_back(option.name);
        }
        return fail(error, where, value.dump() + " is not valid. Use one of: " + joinNames(names) + ".");
    }
    return true;
}

std::string enumName(const char* table, int value) {
    if (const McpValues::EnumOptions* options = McpValues::enumTable(table)) {
        for (const McpValues::EnumOption& option : *options) {
            if (option.value == value) return option.name;
        }
    }
    return std::to_string(value);
}

bool readVec3(const Json& value, Vector3& out, const std::string& where, std::string& error) {
    if (!McpValues::parseVector3(value, out) || !std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z)) {
        return fail(error, where, "must be [x, y, z] (finite numbers).");
    }
    return true;
}

bool readNumber(const Json& value, float& out, const std::string& where, std::string& error) {
    if (!value.is_number() || !std::isfinite(value.get<double>())) return fail(error, where, "must be a number.");
    out = value.get<float>();
    return true;
}

bool readName(const Json& value, std::string& out, const std::string& where, std::string& error) {
    if (!value.is_string() || value.get<std::string>().empty()) return fail(error, where, "must be a non-empty name.");
    out = value.get<std::string>();
    return true;
}

// unknown keys of an object are mistakes worth naming (a typo would otherwise be ignored)
bool checkKeys(const Json& object, const std::vector<std::string>& valid, const std::string& where, std::string& error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(valid.begin(), valid.end(), it.key()) == valid.end()) {
            return fail(error, where, "unknown key \"" + it.key() + "\". Keys: " + joinNames(valid) + ".");
        }
    }
    return true;
}

// ------------------------------------------------------------------ selection

struct Face { const char* name; int axis; bool positive; };
const std::vector<Face>& faces() {
    static const std::vector<Face> table = {
        {"+x", 0, true}, {"-x", 0, false}, {"+y", 1, true}, {"-y", 1, false}, {"+z", 2, true}, {"-z", 2, false},
        {"right", 0, true}, {"left", 0, false}, {"top", 1, true}, {"bottom", 1, false}, {"front", 2, true}, {"back", 2, false}
    };
    return table;
}

std::string faceNames() {
    std::vector<std::string> names;
    for (const Face& face : faces()) names.push_back(face.name);
    return joinNames(names);
}

bool parseFace(const Json& value, const LatticeView& view, std::vector<int>& out, const std::string& where, std::string& error) {
    if (value.is_string()) {
        const std::string name = lowerText(value.get<std::string>());
        for (const Face& face : faces()) {
            if (name == face.name) {
                out = lattice::selectFace(view.rx(), view.ry(), view.rz(), static_cast<lattice::LatticeAxis>(face.axis), face.positive);
                return true;
            }
        }
    }
    return fail(error, where, value.dump() + " is not a face. Use one of: " + faceNames() + ".");
}

// one axis of {i, j, k}: an index or an inclusive [lo, hi]
bool parseRange(const Json& value, int resolution, int& lo, int& hi, const std::string& where, std::string& error) {
    const std::string range = "0.." + std::to_string(resolution - 1);
    if (value.is_number_integer()) {
        lo = hi = value.get<int>();
    } else if (value.is_array() && value.size() == 2 && value[0].is_number_integer() && value[1].is_number_integer()) {
        lo = value[0].get<int>();
        hi = value[1].get<int>();
    } else {
        return fail(error, where, "must be an index or [lo, hi] (" + range + ").");
    }
    if (lo < 0 || hi >= resolution || lo > hi) {
        return fail(error, where, value.dump() + " is out of range: this axis has points " + range + ".");
    }
    return true;
}

// a point as [i, j, k] or a flat index
bool parsePoint(const Json& value, const LatticeView& view, int& index, const std::string& where, std::string& error) {
    if (value.is_number_integer()) {
        index = value.get<int>();
        if (index < 0 || index >= view.count()) {
            return fail(error, where, "index " + std::to_string(index) + " is out of range: the lattice has " +
                        std::to_string(view.count()) + " points (0.." + std::to_string(view.count() - 1) + ").");
        }
        return true;
    }
    if (value.is_array() && value.size() == 3 && value[0].is_number_integer() && value[1].is_number_integer() &&
        value[2].is_number_integer()) {
        const int i = value[0].get<int>(), j = value[1].get<int>(), k = value[2].get<int>();
        if (i < 0 || i >= view.rx() || j < 0 || j >= view.ry() || k < 0 || k >= view.rz()) {
            return fail(error, where, value.dump() + " is out of range: the resolution is " + std::to_string(view.rx()) + "x" +
                        std::to_string(view.ry()) + "x" + std::to_string(view.rz()) + " (i 0.." + std::to_string(view.rx() - 1) +
                        ", j 0.." + std::to_string(view.ry() - 1) + ", k 0.." + std::to_string(view.rz() - 1) + ").");
        }
        index = lattice::pointIndex(view.rx(), view.ry(), i, j, k);
        return true;
    }
    return fail(error, where, "a point is [i, j, k] or a flat index, not " + value.dump() + ".");
}

std::vector<int> intersect(const std::vector<int>& a, const std::vector<int>& b) {
    std::vector<int> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

std::vector<int> sortedUnique(std::vector<int> points) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    return points;
}

std::string cageBoundsText(const LatticeView& view) {
    AABB bounds;
    for (int index = 0; index < view.count(); index++) bounds.merge(view.model * view.local(index));
    return "the points span world " + vecText(bounds.getMinimum()) + " to " + vecText(bounds.getMaximum());
}

const char* SELECT_KEYS_TEXT = "all, exterior, interior, selected, a face (+x, -x, +y, -y, +z, -z, right, left, top, bottom, "
                               "front, back), [points], or {face, i, j, k, points, box, sphere}";

// The points `value` selects (ascending, unique). Empty selections are an error: an op that
// moves nothing is almost always a mistake (a box that misses the cage, an index range typo).
bool parseSelect(const Json& value, const LatticeView& view, uint32_t sceneId, std::vector<int>& out,
                 const std::string& where, std::string& error) {
    const int rx = view.rx(), ry = view.ry(), rz = view.rz();
    if (value.is_null()) {
        out = lattice::selectAll(rx, ry, rz);
        return true;
    }
    if (value.is_string()) {
        const std::string name = lowerText(value.get<std::string>());
        if (name == "all") {
            out = lattice::selectAll(rx, ry, rz);
        } else if (name == "exterior") {
            out = lattice::selectExterior(rx, ry, rz);
        } else if (name == "interior") {
            out = lattice::selectInterior(rx, ry, rz);
            if (out.empty()) {
                return fail(error, where, "\"interior\" is empty at resolution " + std::to_string(rx) + "x" + std::to_string(ry) + "x" +
                            std::to_string(rz) + ": every point is on the outside (raise the resolution to 3+ per axis).");
            }
        } else if (name == "selected") {
            out = sortedUnique(LatticeEditorOps::getSelectedPoints(sceneId, view.entity));
            out.erase(std::remove_if(out.begin(), out.end(), [&view](int index) { return index >= view.count(); }), out.end());
            if (out.empty()) return fail(error, where, "\"selected\": no points of this lattice are selected in the Scene view.");
        } else {
            bool isFace = false;
            for (const Face& face : faces()) isFace = isFace || name == face.name;
            if (!isFace) return fail(error, where, value.dump() + " is not a selection. Use " + std::string(SELECT_KEYS_TEXT) + ".");
            if (!parseFace(value, view, out, where, error)) return false;
        }
        return true;
    }
    if (value.is_array()) {
        std::vector<int> points;
        for (size_t n = 0; n < value.size(); n++) {
            int index = 0;
            if (!parsePoint(value[n], view, index, where + "[" + std::to_string(n) + "]", error)) return false;
            points.push_back(index);
        }
        out = sortedUnique(points);
        if (out.empty()) return fail(error, where, "the point list is empty.");
        return true;
    }
    if (!value.is_object() || value.empty()) {
        return fail(error, where, "must be " + std::string(SELECT_KEYS_TEXT) + ".");
    }
    if (!checkKeys(value, {"face", "i", "j", "k", "points", "box", "sphere"}, where, error)) return false;

    std::vector<int> result = lattice::selectAll(rx, ry, rz);
    if (has(value, "face")) {
        std::vector<int> face;
        if (!parseFace(value["face"], view, face, where + ".face", error)) return false;
        result = intersect(result, face);
    }
    if (has(value, "i") || has(value, "j") || has(value, "k")) {
        int lo[3] = {-1, -1, -1}, hi[3] = {-1, -1, -1};
        const int resolution[3] = {rx, ry, rz};
        const char* keys[3] = {"i", "j", "k"};
        for (int a = 0; a < 3; a++) {
            if (has(value, keys[a]) && !parseRange(value[keys[a]], resolution[a], lo[a], hi[a], where + "." + keys[a], error)) return false;
        }
        result = intersect(result, lattice::selectRange(rx, ry, rz, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]));
    }
    if (has(value, "points")) {
        const Json& list = value["points"];
        if (!list.is_array() || list.empty()) return fail(error, where + ".points", "must be a non-empty list of [i, j, k] or flat indices.");
        std::vector<int> points;
        for (size_t n = 0; n < list.size(); n++) {
            int index = 0;
            if (!parsePoint(list[n], view, index, where + ".points[" + std::to_string(n) + "]", error)) return false;
            points.push_back(index);
        }
        result = intersect(result, sortedUnique(points));
    }
    if (has(value, "box") || has(value, "sphere")) {
        if (!view.invertible) return fail(error, where, "box and sphere are world-space tests: the lattice's transform has zero scale.");
    }
    if (has(value, "box")) {
        const Json& box = value["box"];
        Vector3 min, max;
        if (!box.is_object() || !has(box, "min") || !has(box, "max")) return fail(error, where + ".box", "must be {min: [x,y,z], max: [x,y,z]} (world).");
        if (!checkKeys(box, {"min", "max"}, where + ".box", error)) return false;
        if (!readVec3(box["min"], min, where + ".box.min", error) || !readVec3(box["max"], max, where + ".box.max", error)) return false;
        result = intersect(result, lattice::selectInWorldBox(rx, ry, rz, view.component->offsets, view.model, min, max));
    }
    if (has(value, "sphere")) {
        const Json& sphere = value["sphere"];
        Vector3 center;
        float radius = 0;
        if (!sphere.is_object() || !has(sphere, "center") || !has(sphere, "radius")) {
            return fail(error, where + ".sphere", "must be {center: [x,y,z], radius} (world).");
        }
        if (!checkKeys(sphere, {"center", "radius"}, where + ".sphere", error)) return false;
        if (!readVec3(sphere["center"], center, where + ".sphere.center", error) ||
            !readNumber(sphere["radius"], radius, where + ".sphere.radius", error)) return false;
        if (radius < 0) return fail(error, where + ".sphere.radius", "must be >= 0.");
        result = intersect(result, lattice::selectInWorldSphere(rx, ry, rz, view.component->offsets, view.model, center, radius));
    }
    if (result.empty()) {
        return fail(error, where, "selects no points (" + cageBoundsText(view) + ").");
    }
    out = result;
    return true;
}

// ------------------------------------------------------------------ the call

struct Session {
    CallPtr call;
    Project* project = nullptr;
    SceneProject* sp = nullptr;
    uint32_t sceneId = 0;
    Entity entity = NULL_ENTITY;
    Json opResults = Json::array();
    Json notes = Json::array();        // creation notes (targets left unbound)

    Scene* scene() const { return sp->scene; }
    // the lattice as it is now (world matrices recomputed after edits: the transform system
    // only runs between frames)
    bool view(LatticeView& out, bool refresh = true) const {
        if (refresh && sp->playState == ScenePlayState::STOPPED) McpValues::refreshTransforms(sp);
        return viewOf(sp->scene, entity, out);
    }
};

bool resolveTarget(const Session& s, const Json& value, Entity& out, const std::string& where, std::string& error) {
    std::string why;
    out = McpRefs::resolveEntity(s.sp, value, why);
    if (out == NULL_ENTITY) return fail(error, where, why);
    return true;
}

bool resolveTargets(const Session& s, const Json& value, std::vector<Entity>& out, const std::string& where, std::string& error) {
    std::string why;
    if (!McpRefs::resolveEntities(s.sp, value, out, why)) return fail(error, where, why);
    return true;
}

std::vector<Entity> sceneLattices(SceneProject* sp) {
    std::vector<Entity> list;
    for (Entity entity : sp->entities) {
        if (sp->scene->isEntityCreated(entity) && sp->scene->findComponent<LatticeComponent>(entity) &&
            sp->scene->findComponent<Transform>(entity)) {
            list.push_back(entity);
        }
    }
    return list;
}

std::string latticeListText(SceneProject* sp) {
    std::vector<std::string> names;
    for (Entity entity : sceneLattices(sp)) {
        if (names.size() == 12) {
            names.push_back("...");
            break;
        }
        names.push_back(McpRefs::entityPath(sp, entity) + " [" + std::to_string(entity) + "]");
    }
    return names.empty() ? std::string("the scene has no lattice (create one with edit_lattice create or create_entity type lattice)")
                         : "lattices: " + joinNames(names);
}

// `lattice`, else the single selected lattice, else the scene's only one
bool resolveLattice(Session& s, std::string& error) {
    const Json& args = s.call->args();
    Scene* scene = s.scene();
    if (has(args, "lattice")) {
        std::string why;
        s.entity = McpRefs::resolveEntity(s.sp, args["lattice"], why);
        if (s.entity == NULL_ENTITY) return fail(error, "lattice", why);
        if (!scene->findComponent<LatticeComponent>(s.entity) || !scene->findComponent<Transform>(s.entity)) {
            return fail(error, "lattice", label(scene, s.entity) + " is not a lattice (no Lattice component); " + latticeListText(s.sp) + ".");
        }
        return true;
    }
    const std::vector<Entity> selected = s.project->getSelectedEntities(s.sp->id);
    if (selected.size() == 1 && scene->isEntityCreated(selected[0]) && scene->findComponent<LatticeComponent>(selected[0]) &&
        scene->findComponent<Transform>(selected[0])) {
        s.entity = selected[0];
        return true;
    }
    const std::vector<Entity> all = sceneLattices(s.sp);
    if (all.size() == 1) {
        s.entity = all[0];
        return true;
    }
    return fail(error, "lattice", all.empty() ? "the scene has no lattice: pass create (or create_entity type lattice)."
                                              : "pass lattice (no single lattice is selected); " + latticeListText(s.sp) + ".");
}

// LatticeEditorOps reports its own reason; the op path says where it came from
bool opFailed(std::string& error, const std::string& where, const std::string& why) {
    return fail(error, where, why.empty() ? std::string("the edit did not apply (see the console).") : why);
}

// ------------------------------------------------------------------ create

bool runCreate(Session& s, const Json& create, std::string& error) {
    const std::string where = "create";
    if (!create.is_object()) return fail(error, where, "must be an object {targets?, name?, resolution?, fit_padding?}.");
    if (!checkKeys(create, {"targets", "name", "resolution", "fit_padding"}, where, error)) return false;

    std::vector<Entity> targets;
    if (has(create, "targets") && !resolveTargets(s, create["targets"], targets, where + ".targets", error)) return false;
    std::string name = "Lattice";
    if (has(create, "name") && !readName(create["name"], name, where + ".name", error)) return false;
    for (Entity target : targets) {
        if (!s.scene()->findComponent<Transform>(target)) {
            return fail(error, where + ".targets", label(s.scene(), target) + " has no Transform, so it cannot be measured or deformed.");
        }
    }

    std::string why;
    s.entity = LatticeEditorOps::createLattice(s.project, s.sceneId, targets, Vector3(0, 0, 0), name, &why);
    if (s.entity == NULL_ENTITY) return opFailed(error, where, why);

    // createLattice binds the targets that have a mesh (or mesh children) and skips the rest
    std::vector<Entity> bound;
    for (Entity target : targets) {
        LatticeModifierComponent* modifier = s.scene()->findComponent<LatticeModifierComponent>(target);
        bool isBound = false;
        if (modifier) {
            for (const LatticeBinding& binding : modifier->bindings) isBound = isBound || binding.lattice == s.entity;
        }
        if (isBound) {
            bound.push_back(target);
        } else {
            s.notes.push_back(label(s.scene(), target) + " has no mesh (nor do its children): not bound, not measured.");
        }
    }

    if (has(create, "fit_padding")) {
        float padding = 0;
        if (!readNumber(create["fit_padding"], padding, where + ".fit_padding", error)) return false;
        if (padding < 0) return fail(error, where + ".fit_padding", "must be >= 0 (a fraction of the size: 0.02 = 2%).");
        if (bound.empty()) return fail(error, where + ".fit_padding", "needs targets with a mesh to fit around.");
        McpValues::refreshTransforms(s.sp);
        if (!LatticeEditorOps::fitTo(s.project, s.sceneId, s.entity, bound, padding, &why)) return opFailed(error, where + ".fit_padding", why);
    }
    if (has(create, "resolution")) {
        const Json& r = create["resolution"];
        if (!r.is_array() || r.size() != 3 || !r[0].is_number_integer() || !r[1].is_number_integer() || !r[2].is_number_integer()) {
            return fail(error, where + ".resolution", "must be [x, y, z] integers, 2..16 each.");
        }
        if (!LatticeEditorOps::setResolution(s.project, s.sceneId, s.entity, r[0].get<int>(), r[1].get<int>(), r[2].get<int>(), &why)) {
            return opFailed(error, where + ".resolution", why);
        }
    }
    s.opResults.push_back(Json{{"op", "create"}, {"bound", static_cast<int>(bound.size())}});
    return true;
}

// ------------------------------------------------------------------ setters

bool runSetters(Session& s, UndoStep& step, std::string& error) {
    const Json& args = s.call->args();
    std::string why;
    if (has(args, "resolution")) {
        const Json& r = args["resolution"];
        if (!r.is_array() || r.size() != 3 || !r[0].is_number_integer() || !r[1].is_number_integer() || !r[2].is_number_integer()) {
            return fail(error, "resolution", "must be [x, y, z] integers, 2..16 each.");
        }
        if (!LatticeEditorOps::setResolution(s.project, s.sceneId, s.entity, r[0].get<int>(), r[1].get<int>(), r[2].get<int>(), &why)) {
            return opFailed(error, "resolution", why);
        }
    }
    // the settings are plain properties, edited as the inspector edits them (PropertyCmd)
    std::vector<McpValues::Change> changes;
    auto change = [&changes](const char* property, const Json& value) {
        McpValues::Change c;
        c.component = ComponentType::LatticeComponent;
        c.property = property;
        c.value = value;
        changes.push_back(c);
    };
    if (has(args, "interpolation")) {
        int value = 0;
        if (!parseEnum("lattice_interpolation", args["interpolation"], value, "interpolation", error)) return false;
        change("interpolation", enumName("lattice_interpolation", value));
    }
    if (has(args, "outside")) {
        int value = 0;
        if (!parseEnum("lattice_outside", args["outside"], value, "outside", error)) return false;
        change("outside", enumName("lattice_outside", value));
    }
    if (has(args, "fade_distance")) {
        float value = 0;
        if (!readNumber(args["fade_distance"], value, "fade_distance", error)) return false;
        if (value < 0) return fail(error, "fade_distance", "must be >= 0 (lattice local units; 0 = no fade, a hard edge).");
        change("fadeDistance", value);
    }
    if (!changes.empty()) {
        auto* multi = new MultiPropertyCmd();
        if (!McpValues::appendChangeCommands(s.project, s.sp, {s.entity}, changes, *multi, why)) {
            delete multi;
            return opFailed(error, "interpolation/outside/fade_distance", why);
        }
        if (!step.add(multi)) return opFailed(error, "interpolation/outside/fade_distance", "");
    }
    return true;
}

// ------------------------------------------------------------------ ops

// set: points [{ijk | index, offset | position, space}]
bool opSet(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "points"}, where, error)) return false;
    if (!has(op, "points") || !op["points"].is_array() || op["points"].empty()) {
        return fail(error, where + ".points", "must be a non-empty list of {ijk: [i,j,k] | index, offset | position: [x,y,z], space}.");
    }
    LatticeView view;
    s.view(view);
    std::vector<int> points;
    std::vector<Vector3> offsets;
    const Json& list = op["points"];
    for (size_t n = 0; n < list.size(); n++) {
        const std::string at = where + ".points[" + std::to_string(n) + "]";
        const Json& item = list[n];
        if (!item.is_object()) return fail(error, at, "must be {ijk | index, offset | position, space}.");
        if (!checkKeys(item, {"ijk", "index", "offset", "position", "space"}, at, error)) return false;
        if (has(item, "ijk") == has(item, "index")) return fail(error, at, "give one of ijk [i, j, k] or index.");
        if (has(item, "offset") == has(item, "position")) return fail(error, at, "give one of offset or position.");
        int index = 0;
        if (has(item, "ijk")) {
            if (!item["ijk"].is_array() || !parsePoint(item["ijk"], view, index, at + ".ijk", error)) {
                return error.empty() ? fail(error, at + ".ijk", "must be [i, j, k].") : false;
            }
        } else if (!item["index"].is_number_integer() || !parsePoint(item["index"], view, index, at + ".index", error)) {
            return error.empty() ? fail(error, at + ".index", "must be a flat index.") : false;
        }
        bool world = false;
        if (has(item, "space")) {
            const std::string space = item["space"].is_string() ? lowerText(item["space"].get<std::string>()) : "";
            if (space != "local" && space != "world") return fail(error, at + ".space", item["space"].dump() + " is not valid. Use one of: local, world.");
            world = space == "world";
        }
        if (world && !view.invertible) return fail(error, at + ".space", "world needs an invertible lattice transform (its scale is zero).");
        Vector3 value;
        const char* key = has(item, "offset") ? "offset" : "position";
        if (!readVec3(item[key], value, at + "." + key, error)) return false;
        Vector3 offset;
        if (has(item, "offset")) {
            offset = world ? view.inverse.linear() * value : value;
        } else {
            offset = (world ? view.inverse * value : value) - view.rest(index);
        }
        points.push_back(index);
        offsets.push_back(offset);
    }
    std::string why;
    if (!LatticeEditorOps::setOffsets(s.project, s.sceneId, s.entity, points, offsets, &why)) return opFailed(error, where, why);
    result["points"] = static_cast<int>(sortedUnique(points).size());
    return true;
}

// move: select, offset, rotate (degrees), scale, pivot, space, falloff
bool opMove(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "select", "offset", "rotate", "scale", "pivot", "space", "falloff"}, where, error)) return false;
    if (!has(op, "offset") && !has(op, "rotate") && !has(op, "scale")) {
        return fail(error, where, "give offset [x,y,z], rotate [x,y,z] (degrees) and/or scale (number or [x,y,z]).");
    }
    LatticeView view;
    s.view(view);
    std::vector<int> points;
    if (!parseSelect(has(op, "select") ? op["select"] : Json(), view, s.sceneId, points, where + ".select", error)) return false;

    bool world = false;
    if (has(op, "space")) {
        const std::string space = op["space"].is_string() ? lowerText(op["space"].get<std::string>()) : "";
        if (space != "local" && space != "world") return fail(error, where + ".space", op["space"].dump() + " is not valid. Use one of: local, world.");
        world = space == "world";
    }
    if (world && !view.invertible) return fail(error, where + ".space", "world needs an invertible lattice transform (its scale is zero).");

    Vector3 offset(0, 0, 0), scale(1, 1, 1);
    Quaternion rotation;
    if (has(op, "offset") && !readVec3(op["offset"], offset, where + ".offset", error)) return false;
    if (has(op, "rotate")) {
        Vector3 euler;
        if (!readVec3(op["rotate"], euler, where + ".rotate", error)) return false;
        if (!McpValues::parseQuaternion(Json{{"euler", McpValues::toJson(euler)}}, rotation)) {
            return fail(error, where + ".rotate", "must be [x, y, z] degrees.");
        }
    }
    if (has(op, "scale")) {
        if (op["scale"].is_number()) {
            float uniform = 1;
            if (!readNumber(op["scale"], uniform, where + ".scale", error)) return false;
            scale = Vector3(uniform, uniform, uniform);
        } else if (!readVec3(op["scale"], scale, where + ".scale", error)) {
            return false;
        }
    }

    // the pivot, in the move's space
    Vector3 centroid(0, 0, 0);
    for (int index : points) centroid += view.local(index);
    centroid = centroid / static_cast<float>(points.size());
    Vector3 pivot = world ? view.model * centroid : centroid;
    if (has(op, "pivot")) {
        const Json& p = op["pivot"];
        const std::string name = p.is_string() ? lowerText(p.get<std::string>()) : "";
        if (name == "lattice") {
            pivot = world ? view.model * Vector3(0, 0, 0) : Vector3(0, 0, 0);
        } else if (name != "center") {
            if (p.is_string()) return fail(error, where + ".pivot", p.dump() + " is not valid. Use center, lattice or [x, y, z] (in space).");
            if (!readVec3(p, pivot, where + ".pivot", error)) return false;
        }
    }

    LatticeFalloff falloff;
    if (has(op, "falloff")) {
        const Json& f = op["falloff"];
        if (!f.is_object() || !has(f, "radius")) {
            return fail(error, where + ".falloff", "must be {radius (lattice local units, 1 = the cage size), curve}.");
        }
        if (!checkKeys(f, {"radius", "curve"}, where + ".falloff", error)) return false;
        if (!readNumber(f["radius"], falloff.radius, where + ".falloff.radius", error)) return false;
        if (falloff.radius < 0) return fail(error, where + ".falloff.radius", "must be >= 0 (lattice local units).");
        falloff.enabled = true;
        if (has(f, "curve")) {
            static const std::vector<std::pair<const char*, LatticeFalloffCurve>> curves = {
                {"smooth", LatticeFalloffCurve::SMOOTH}, {"linear", LatticeFalloffCurve::LINEAR},
                {"sharp", LatticeFalloffCurve::SHARP}, {"constant", LatticeFalloffCurve::CONSTANT}};
            const std::string curve = f["curve"].is_string() ? lowerText(f["curve"].get<std::string>()) : "";
            bool found = false;
            for (const auto& item : curves) {
                if (curve == item.first) {
                    falloff.curve = item.second;
                    found = true;
                }
            }
            if (!found) return fail(error, where + ".falloff.curve", f["curve"].dump() + " is not valid. Use one of: smooth, linear, sharp, constant.");
        }
    }

    // p' = pivot + offset + R S (p - pivot), in the move's space; LatticeEditorOps takes it in
    // lattice local space about a local pivot: T with p' = pivotL + T (p - pivotL)
    const Matrix4 linear = rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
    Matrix4 local;
    Vector3 localPivot = pivot;
    if (world) {
        const Matrix4 worldMap = Matrix4::translateMatrix(pivot + offset) * linear * Matrix4::translateMatrix(-pivot);
        localPivot = view.inverse * pivot;
        local = Matrix4::translateMatrix(-localPivot) * view.inverse * worldMap * view.model * Matrix4::translateMatrix(localPivot);
    } else {
        local = Matrix4::translateMatrix(offset) * linear;
    }
    std::string why;
    if (!LatticeEditorOps::transformPoints(s.project, s.sceneId, s.entity, points, local, localPivot, falloff, &why)) {
        return opFailed(error, where, why);
    }
    result["points"] = static_cast<int>(points.size());
    return true;
}

bool opReset(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "select"}, where, error)) return false;
    LatticeView view;
    s.view(view);
    std::vector<int> points;
    if (!parseSelect(has(op, "select") ? op["select"] : Json(), view, s.sceneId, points, where + ".select", error)) return false;
    std::string why;
    if (!LatticeEditorOps::resetPoints(s.project, s.sceneId, s.entity, points, &why)) return opFailed(error, where, why);
    result["points"] = static_cast<int>(points.size());
    return true;
}

bool opPreset(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "name", "axis", "toward", "amount", "seed", "mode", "select"}, where, error)) return false;
    lattice::PresetParams params;
    const std::string names = joinNames(lattice::presetNames());
    if (!has(op, "name") || !op["name"].is_string() || !lattice::presetFromName(lowerText(op["name"].get<std::string>()), params.preset)) {
        return fail(error, where + ".name", (has(op, "name") ? op["name"].dump() + " is not a preset. " : std::string("is required. ")) +
                    "Use one of: " + names + ".");
    }
    params.axis = lattice::LatticeAxis::Y;
    if (has(op, "axis") && !parseAxis(op["axis"], params.axis, where + ".axis", error)) return false;
    // bend and shear need another axis: the next one by default
    params.toward = static_cast<lattice::LatticeAxis>((static_cast<int>(params.axis) + 1) % 3);
    // "-x": toward -x (the amount negated)
    bool negative = false;
    if (has(op, "toward")) {
        Json toward = op["toward"];
        if (toward.is_string() && toward.get<std::string>().size() == 2 && (toward.get<std::string>()[0] == '-' || toward.get<std::string>()[0] == '+')) {
            negative = toward.get<std::string>()[0] == '-';
            toward = toward.get<std::string>().substr(1);
        }
        std::string ignored;
        if (!parseAxis(toward, params.toward, where + ".toward", ignored)) {
            return fail(error, where + ".toward", op["toward"].dump() + " is not an axis. Use one of: x, y, z, -x, -y, -z.");
        }
    }
    const bool directed = params.preset == lattice::LatticePreset::BEND || params.preset == lattice::LatticePreset::SHEAR;
    if (directed && params.toward == params.axis) {
        const int a = static_cast<int>(params.axis);
        return fail(error, where + ".toward", std::string(lattice::presetName(params.preset)) + " needs a toward axis other than its axis (" +
                    AXIS_NAMES[a] + "): use " + AXIS_NAMES[(a + 1) % 3] + " or " + AXIS_NAMES[(a + 2) % 3] + " (or -" +
                    AXIS_NAMES[(a + 1) % 3] + ", -" + AXIS_NAMES[(a + 2) % 3] + ").");
    }
    if (!has(op, "amount")) {
        return fail(error, where + ".amount", "is required: squash 0..0.95, stretch/taper/bulge a fraction, twist/bend degrees, "
                    "shear and noise local units.");
    }
    if (!readNumber(op["amount"], params.amount, where + ".amount", error)) return false;
    if (negative && directed) params.amount = -params.amount;
    if (has(op, "seed")) {
        if (!op["seed"].is_number_integer() || op["seed"].get<long long>() < 0 || op["seed"].get<long long>() > 0xFFFFFFFFLL) {
            return fail(error, where + ".seed", "must be an integer 0..4294967295.");
        }
        params.seed = static_cast<unsigned int>(op["seed"].get<long long>());
    }
    if (has(op, "mode")) {
        const std::string mode = op["mode"].is_string() ? lowerText(op["mode"].get<std::string>()) : "";
        if (mode != "add" && mode != "replace") return fail(error, where + ".mode", op["mode"].dump() + " is not valid. Use one of: add, replace.");
        params.replace = mode == "replace";
    }
    LatticeView view;
    s.view(view);
    std::vector<int> points;
    if (!parseSelect(has(op, "select") ? op["select"] : Json(), view, s.sceneId, points, where + ".select", error)) return false;
    std::string why;
    if (!LatticeEditorOps::applyPreset(s.project, s.sceneId, s.entity, params, points, &why)) return opFailed(error, where, why);
    result["points"] = static_cast<int>(points.size());
    return true;
}

bool opMirror(Session& s, const Json& op, const std::string& where, Json&, std::string& error) {
    if (!checkKeys(op, {"op", "axis", "from"}, where, error)) return false;
    lattice::LatticeAxis axis = lattice::LatticeAxis::X;
    if (!has(op, "axis")) return fail(error, where + ".axis", "is required: x, y or z.");
    if (!parseAxis(op["axis"], axis, where + ".axis", error)) return false;
    bool fromPositive = true;
    if (has(op, "from")) {
        const std::string from = op["from"].is_string() ? op["from"].get<std::string>() : "";
        if (from != "+" && from != "-") return fail(error, where + ".from", op["from"].dump() + " is not valid. Use \"+\" (copy + onto -) or \"-\".");
        fromPositive = from == "+";
    }
    std::string why;
    if (!LatticeEditorOps::mirror(s.project, s.sceneId, s.entity, axis, fromPositive, &why)) return opFailed(error, where, why);
    return true;
}

bool opFit(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "targets", "padding"}, where, error)) return false;
    std::vector<Entity> targets;
    if (has(op, "targets")) {
        if (!resolveTargets(s, op["targets"], targets, where + ".targets", error)) return false;
    } else {
        targets = LatticeEditorOps::findTargets(s.scene(), s.entity);
        if (targets.empty()) return fail(error, where + ".targets", "the lattice has no targets yet: pass targets to fit around.");
    }
    float padding = 0.02f;
    if (has(op, "padding")) {
        if (!readNumber(op["padding"], padding, where + ".padding", error)) return false;
        if (padding < 0) return fail(error, where + ".padding", "must be >= 0 (a fraction of the size: 0.02 = 2%).");
    }
    McpValues::refreshTransforms(s.sp);
    std::string why;
    if (!LatticeEditorOps::fitTo(s.project, s.sceneId, s.entity, targets, padding, &why)) return opFailed(error, where, why);
    result["targets"] = static_cast<int>(targets.size());
    return true;
}

bool opBind(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "target", "weight", "stage", "submesh", "mask", "include_children"}, where, error)) return false;
    if (!has(op, "target")) return fail(error, where + ".target", "is required: the GameObject to deform.");
    Entity target = NULL_ENTITY;
    if (!resolveTarget(s, op["target"], target, where + ".target", error)) return false;

    // defaults: the existing binding's settings (bind again to change one), else the defaults;
    // include_children: the modifier's value, else on for a GameObject without a mesh of its own
    LatticeBinding binding;
    bool includeChildren = !hasMesh(s.scene(), target);
    if (LatticeModifierComponent* modifier = s.scene()->findComponent<LatticeModifierComponent>(target)) {
        includeChildren = modifier->includeChildren;
        for (const LatticeBinding& existing : modifier->bindings) {
            if (existing.lattice == s.entity) binding = existing;
        }
    }
    if (has(op, "weight")) {
        if (!readNumber(op["weight"], binding.weight, where + ".weight", error)) return false;
        if (binding.weight < 0 || binding.weight > 2) return fail(error, where + ".weight", "must be 0..2 (1 = the full deformation).");
    }
    if (has(op, "stage")) {
        int value = 0;
        if (!parseEnum("lattice_stage", op["stage"], value, where + ".stage", error)) return false;
        binding.stage = static_cast<LatticeStage>(value);
    }
    if (has(op, "submesh")) {
        if (!op["submesh"].is_number_integer() || op["submesh"].get<int>() < -1) {
            return fail(error, where + ".submesh", "must be -1 (all) or a submesh index.");
        }
        binding.submesh = op["submesh"].get<int>();
    }
    if (has(op, "mask")) {
        int value = 0;
        if (!parseEnum("lattice_mask", op["mask"], value, where + ".mask", error)) return false;
        binding.mask = static_cast<LatticeMaskChannel>(value);
    }
    if (has(op, "include_children")) {
        if (!op["include_children"].is_boolean()) return fail(error, where + ".include_children", "must be true or false.");
        includeChildren = op["include_children"].get<bool>();
    }
    std::string why;
    if (!LatticeEditorOps::bind(s.project, s.sceneId, s.entity, target, binding, includeChildren, &why)) return opFailed(error, where, why);
    // a child that already inherits the lattice: its own binding replaces the inherited one there
    // (each lattice deforms a mesh once), which is how one child is tuned or excluded (weight 0)
    if (const Entity from = inheritedFrom(s.scene(), target, s.entity); from != NULL_ENTITY) {
        const std::string note = label(s.scene(), target) + " also inherits this lattice from " + label(s.scene(), from) +
            " (include_children): its own binding (weight " + Json(McpValues::number(binding.weight)).dump() + ") replaces that one for it" +
            (binding.weight == 0 ? ", so it is excluded." : ". weight 0 excludes it; unbind it to inherit again.");
        result["overrides"] = McpRefs::entityJson(s.sp, from);
        s.notes.push_back(note);
    }
    return true;
}

bool opUnbind(Session& s, const Json& op, const std::string& where, Json&, std::string& error) {
    if (!checkKeys(op, {"op", "target"}, where, error)) return false;
    if (!has(op, "target")) return fail(error, where + ".target", "is required.");
    Entity target = NULL_ENTITY;
    if (!resolveTarget(s, op["target"], target, where + ".target", error)) return false;
    std::string why;
    if (!LatticeEditorOps::unbind(s.project, s.sceneId, s.entity, target, &why)) {
        // a child reached through its parent's include_children has no binding of its own to remove
        if (const Entity from = inheritedFrom(s.scene(), target, s.entity); from != NULL_ENTITY) {
            why = label(s.scene(), target) + " has no binding of its own: it inherits this lattice from " + label(s.scene(), from) +
                  " (include_children). To exclude it, bind it with weight 0 ({op: bind, target, weight: 0}: its own binding "
                  "replaces the inherited one); to free the whole group, unbind " + label(s.scene(), from) + ".";
        }
        return opFailed(error, where, why);
    }
    return true;
}

bool opFollow(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "target", "rest"}, where, error)) return false;
    if (!has(op, "target")) return fail(error, where + ".target", "is required: the GameObject that follows the deformed surface (a prop, a light).");
    Entity target = NULL_ENTITY, rest = NULL_ENTITY;
    if (!resolveTarget(s, op["target"], target, where + ".target", error)) return false;
    if (has(op, "rest") && !resolveTarget(s, op["rest"], rest, where + ".rest", error)) return false;
    std::string why;
    if (!LatticeEditorOps::follow(s.project, s.sceneId, s.entity, target, rest, &rest, &why)) return opFailed(error, where, why);
    result["target"] = McpRefs::entityJson(s.sp, target);
    result["rest"] = McpRefs::entityJson(s.sp, rest);
    return true;
}

bool opPose(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "save", "apply", "delete", "weight"}, where, error)) return false;
    const int actions = (has(op, "save") ? 1 : 0) + (has(op, "apply") ? 1 : 0) + (has(op, "delete") ? 1 : 0);
    if (actions != 1) return fail(error, where, "give exactly one of save, apply or delete (the pose name).");
    const char* action = has(op, "save") ? "save" : has(op, "apply") ? "apply" : "delete";
    std::string name;
    if (!readName(op[action], name, where + "." + action, error)) return false;
    float weight = 1;
    if (has(op, "weight")) {
        if (std::string(action) != "apply") return fail(error, where + ".weight", "is for apply only.");
        if (!readNumber(op["weight"], weight, where + ".weight", error)) return false;
    }
    std::string why;
    bool ok = false;
    if (std::string(action) == "save") {
        ok = LatticeEditorOps::savePose(s.project, s.sceneId, s.entity, name, &why);
    } else if (std::string(action) == "apply") {
        ok = LatticeEditorOps::applyPose(s.project, s.sceneId, s.entity, name, weight, &why);
    } else {
        ok = LatticeEditorOps::deletePose(s.project, s.sceneId, s.entity, name, &why);
    }
    if (!ok) return opFailed(error, where + "." + action, why);
    result["pose"] = name;
    return true;
}

bool opBake(Session& s, const Json& op, const std::string& where, Json& result, std::string& error) {
    if (!checkKeys(op, {"op", "target"}, where, error)) return false;
    Entity target = NULL_ENTITY;
    if (has(op, "target")) {
        if (!resolveTarget(s, op["target"], target, where + ".target", error)) return false;
    } else {
        const std::vector<Entity> targets = LatticeEditorOps::findTargets(s.scene(), s.entity);
        if (targets.size() != 1) {
            std::vector<std::string> names;
            for (Entity t : targets) names.push_back(label(s.scene(), t));
            return fail(error, where + ".target", targets.empty() ? std::string("the lattice has no targets to bake.")
                                                                  : "is required: the lattice deforms " + joinNames(names) + ".");
        }
        target = targets[0];
    }
    std::string path, note, why;
    // canBake gives the inspector's reason (a generated mesh, no bakeable binding...) before any
    // work, on what the bake reads: the scene with its previews restored
    LatticeEditorOps::restoreForBake(s.sceneId, s.scene(), target);
    if (!LatticeEditorOps::canBake(s.scene(), target, &why)) {
        return opFailed(error, where + ".target", label(s.scene(), target) + " cannot be baked: " + why);
    }
    if (!LatticeEditorOps::bake(s.project, s.sceneId, target, &path, &note, &why)) return opFailed(error, where, why);
    if (LatticeEditorOps::findTargets(s.scene(), s.entity).empty()) {
        note += std::string(note.empty() ? "" : " ") + "The lattice deforms nothing now: delete it (delete_entities) unless you bind it again.";
    }
    result["target"] = McpRefs::entityJson(s.sp, target);
    result["path"] = path;
    if (!note.empty()) result["note"] = note;
    return true;
}

using OpFn = bool (*)(Session&, const Json&, const std::string&, Json&, std::string&);

const std::vector<std::pair<const char*, OpFn>>& opTable() {
    static const std::vector<std::pair<const char*, OpFn>> table = {
        {"set", opSet}, {"move", opMove}, {"reset", opReset}, {"preset", opPreset}, {"mirror", opMirror},
        {"fit", opFit}, {"bind", opBind}, {"unbind", opUnbind}, {"follow", opFollow}, {"pose", opPose}, {"bake", opBake}
    };
    return table;
}

bool runOps(Session& s, const Json& ops, std::string& error) {
    if (!ops.is_array()) return fail(error, "ops", "must be a list of {op, ...}.");
    std::vector<std::string> names;
    for (const auto& item : opTable()) names.push_back(item.first);
    for (size_t n = 0; n < ops.size(); n++) {
        const std::string where = "ops[" + std::to_string(n) + "]";
        const Json& op = ops[n];
        if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) {
            return fail(error, where, "must be {op: " + joinNames(names) + ", ...}.");
        }
        const std::string name = lowerText(op["op"].get<std::string>());
        OpFn fn = nullptr;
        for (const auto& item : opTable()) {
            if (name == item.first) fn = item.second;
        }
        if (!fn) return fail(error, where + ".op", op["op"].dump() + " is not an op. Use one of: " + joinNames(names) + ".");
        Json result = {{"op", name}};
        if (!fn(s, op, where, result, error)) return false;
        s.opResults.push_back(result);
    }
    return true;
}

// ------------------------------------------------------------------ the read

Json warningsOf(const Session& s, const LatticeView& view) {
    Json warnings = Json::array();
    Scene* scene = s.scene();
    auto warn = [&warnings](const char* kind, const std::string& message, Json extra = Json::object()) {
        extra["kind"] = kind;
        extra["message"] = message;
        warnings.push_back(extra);
    };

    if (!view.component->enabled) warn("disabled", "The lattice is disabled: it deforms nothing until enabled (set_properties lattice enabled).");
    if (!view.invertible) warn("zero_scale", "The lattice's transform has zero scale: it deforms nothing (give it a non-zero scale).");

    const std::vector<std::array<int, 3>> folded = lattice::findFoldedCells(*view.component);
    if (!folded.empty()) {
        Json cells = Json::array();
        std::string text;
        for (size_t c = 0; c < folded.size() && c < MAX_LISTED_CELLS; c++) {
            cells.push_back(Json::array({folded[c][0], folded[c][1], folded[c][2]}));
            text += (c ? ", (" : "(") + std::to_string(folded[c][0]) + "," + std::to_string(folded[c][1]) + "," + std::to_string(folded[c][2]) + ")";
        }
        if (folded.size() > MAX_LISTED_CELLS) text += ", ...";
        warn("folded_cells", std::to_string(folded.size()) + " cell(s) folded inside out (stretch <= 0; the cell's lower corner " + text +
             "): the mesh turns inside out there. Fix: move those points less far past their neighbours, reset them, or "
             "spread the change with a falloff or a higher resolution.", Json{{"cells", cells}, {"count", folded.size()}});
    }

    const std::vector<Entity> targets = LatticeEditorOps::findTargets(scene, s.entity);
    if (targets.empty()) {
        warn("no_targets", "The lattice deforms nothing yet: bind a GameObject (op bind) or create it with create.targets.");
    }
    // followers: one without a rest stays put; one whose rest is outside a fading cage gets
    // less of the deformation (a prop on top of a squashed box floats above it)
    auto followers = scene->getComponentArray<LatticeFollowerComponent>();
    for (size_t f = 0; f < followers->size(); f++) {
        const LatticeFollowerComponent& follower = followers->getComponentFromIndex(f);
        const Entity entity = followers->getEntity(f);
        if (follower.lattice != s.entity || !follower.enabled) continue;
        Transform* rest = (follower.rest != NULL_ENTITY && scene->isEntityCreated(follower.rest))
            ? scene->findComponent<Transform>(follower.rest) : nullptr;
        if (!rest) {
            warn("follower_no_rest", label(scene, entity) + " follows this lattice but has no rest, so it stays put: follow it again "
                 "({op: follow, target}) to give it one where it stands.", Json{{"entity", McpRefs::entityJson(s.sp, entity)}});
            continue;
        }
        if (!view.invertible || view.component->outside != LatticeOutside::FADE) continue;
        const Vector3 u = view.inverse * rest->worldPosition + Vector3(0.5f, 0.5f, 0.5f);
        const Vector3 inside(std::clamp(u.x, 0.0f, 1.0f), std::clamp(u.y, 0.0f, 1.0f), std::clamp(u.z, 0.0f, 1.0f));
        const float d = (u - inside).length();
        if (d <= 1e-4f) continue;
        const float fade = view.component->fadeDistance;
        const float q = fade > 0 ? std::min(d / fade, 1.0f) : 1.0f;
        const float share = 1.0f - q * q * (3.0f - 2.0f * q);
        char percent[16];
        std::snprintf(percent, sizeof(percent), "%.0f%%", share * 100.0f);
        warn("follower_outside", label(scene, entity) + "'s rest is outside the cage, where the deformation fades: it follows only " +
             percent + " of it. Set outside: \"extend\" (the border deformation continues outward), or fit the cage around it too.",
             Json{{"entity", McpRefs::entityJson(s.sp, entity)}, {"share", McpValues::number(share)}});
    }

    std::set<Entity> crowded;
    for (Entity target : targets) {
        const LatticeModifierComponent& modifier = scene->getComponent<LatticeModifierComponent>(target);
        for (size_t b = 0; b < modifier.bindings.size(); b++) {
            if (!isLattice(scene, modifier.bindings[b].lattice)) {
                warn("missing_lattice", label(scene, target) + " bindings[" + std::to_string(b) + "] names no lattice (deleted, or not a "
                     "lattice): remove it (set_properties latticemodifier bindings) or point it at a lattice.",
                     Json{{"target", McpRefs::entityJson(s.sp, target)}, {"binding", b}});
            }
        }
        const std::vector<Entity> meshes = meshesReached(s.sp, target, modifier.includeChildren);
        if (meshes.empty()) {
            const bool childMeshes = !meshesReached(s.sp, target, true).empty();
            warn("no_mesh", label(scene, target) + (childMeshes
                 ? " has no mesh of its own and include_children is off: bind it again with include_children: true."
                 : " has no mesh (nor do its children): nothing of it is deformed; bind the GameObject that draws."),
                 Json{{"target", McpRefs::entityJson(s.sp, target)}});
        }
        for (Entity mesh : meshes) {
            const int count = latticesOnMesh(scene, mesh);
            if (count > LATTICE_MAX_PER_MESH && crowded.insert(mesh).second) {
                warn("too_many_lattices", label(scene, mesh) + " is reached by " + std::to_string(count) + " lattices; only the first " +
                     std::to_string(LATTICE_MAX_PER_MESH) + " apply (its own bindings in order, then inherited ones): unbind some.",
                     Json{{"entity", McpRefs::entityJson(s.sp, mesh)}, {"count", count}});
            }
        }
    }
    return warnings;
}

Json describe(const Session& s, const LatticeView& view, bool points, const std::vector<Vector3>& samples, Json& warnings) {
    Scene* scene = s.scene();
    const LatticeComponent& c = *view.component;
    Json data = {
        {"scene_id", s.sp->id},
        {"lattice", McpRefs::entityJson(s.sp, s.entity)},
        {"resolution", Json::array({c.resolutionX, c.resolutionY, c.resolutionZ})},
        {"interpolation", enumName("lattice_interpolation", static_cast<int>(c.interpolation))},
        {"outside", enumName("lattice_outside", static_cast<int>(c.outside))},
        {"fade_distance", McpValues::number(c.fadeDistance)},
        {"enabled", c.enabled}
    };

    Json targets = Json::array();
    for (Entity target : LatticeEditorOps::findTargets(scene, s.entity)) {
        const LatticeModifierComponent& modifier = scene->getComponent<LatticeModifierComponent>(target);
        for (const LatticeBinding& binding : modifier.bindings) {
            if (binding.lattice != s.entity) continue;
            Json entry = {
                {"entity", McpRefs::entityJson(s.sp, target)},
                {"stage", enumName("lattice_stage", static_cast<int>(binding.stage))},
                {"weight", McpValues::number(binding.weight)},
                {"meshes", static_cast<int>(meshesReached(s.sp, target, modifier.includeChildren).size())}
            };
            if (binding.mask != LatticeMaskChannel::NONE) entry["mask"] = enumName("lattice_mask", static_cast<int>(binding.mask));
            if (binding.submesh >= 0) entry["submesh"] = binding.submesh;
            targets.push_back(entry);
            break;
        }
    }
    data["targets"] = targets;
    Json followers = Json::array();
    auto followerArray = scene->getComponentArray<LatticeFollowerComponent>();
    for (size_t f = 0; f < followerArray->size(); f++) {
        const LatticeFollowerComponent& follower = followerArray->getComponentFromIndex(f);
        if (follower.lattice != s.entity) continue;
        const bool hasRest = follower.rest != NULL_ENTITY && scene->isEntityCreated(follower.rest);
        followers.push_back(Json{{"entity", McpRefs::entityJson(s.sp, followerArray->getEntity(f))},
                                 {"rest", hasRest ? McpRefs::entityJson(s.sp, follower.rest) : Json(nullptr)}});
    }
    if (!followers.empty()) data["followers"] = followers;

    int moved = 0;
    float maxLocal = 0, maxWorld = 0;
    Json list = Json::array();
    const Matrix3 linear = view.model.linear();
    for (int index = 0; index < view.count(); index++) {
        const Vector3 offset = view.offset(index);
        if (offset == Vector3(0, 0, 0)) continue;
        moved++;
        maxLocal = std::max(maxLocal, offset.length());
        maxWorld = std::max(maxWorld, (linear * offset).length());
        if (points) {
            list.push_back(Json{{"ijk", ijkJson(view, index)}, {"index", index}, {"offset", McpValues::toJson(offset)},
                                {"world", McpValues::toJson(view.model * view.local(index))}});
        }
    }
    data["moved_points"] = moved;
    data["point_count"] = view.count();
    data["max_offset"] = Json{{"local", McpValues::number(maxLocal)}, {"world", McpValues::number(maxWorld)}};
    Json poses = Json::array();
    for (const LatticePose& pose : c.poses) poses.push_back(pose.name);
    data["poses"] = poses;
    if (points) data["points"] = list;

    if (!samples.empty()) {
        std::vector<Vector3> padded;
        lattice::buildPaddedGrid(c, padded);
        Json out = Json::array();
        for (const Vector3& point : samples) {
            const Vector3 deformed = lattice::deformWorldPoint(c, padded, view.model, point, 1.0f);
            const float stretch = lattice::worldJacobian(c, padded, view.model, point, 1.0f).determinant();
            out.push_back(Json{{"point", McpValues::toJson(point)}, {"deformed", McpValues::toJson(deformed)},
                               {"stretch", McpValues::number(stretch)}});
        }
        data["samples"] = out;
    }

    warnings = warningsOf(s, view);
    data["warnings"] = warnings;
    return data;
}

void editLatticeRun(const CallPtr& call, bool waited);

void editLatticeTool(const CallPtr& call) {
    editLatticeRun(call, false);
}

void editLatticeRun(const CallPtr& call, bool waited) {
    const Json& args = call->args();
    Session s;
    s.call = call;
    s.project = call->ctx().project();
    std::string error;
    s.sp = McpRefs::resolveScene(s.project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!s.sp || !s.sp->scene) {
        call->fail(error.empty() ? "The scene is not loaded." : error);
        return;
    }
    s.sceneId = s.sp->id;

    const bool edits = has(args, "create") || has(args, "resolution") || has(args, "interpolation") || has(args, "outside") ||
                       has(args, "fade_distance") || (has(args, "ops") && !(args["ops"].is_array() && args["ops"].empty()));
    if (edits && s.sp->playState != ScenePlayState::STOPPED) {
        call->fail("Scene " + std::to_string(s.sp->id) + " is playing; stop play mode before editing it. Play-mode changes are "
                   "lost at stop anyway: to try one in the running game use eval_lua (object(\"<cage>\", \"Lattice\"):applyPreset("
                   "\"squash\", \"y\", 0.3), setOffset...); edit_lattice without edits reads the running cage.");
        return;
    }
    if (!edits) {
        // reads follow the running game (the Play copy of a child scene)
        s.sp = McpRefs::runtimeScene(s.project, s.sp);
        s.sceneId = s.sp->id;
    }

    std::string detail = "summary";
    if (has(args, "detail")) {
        detail = args["detail"].is_string() ? lowerText(args["detail"].get<std::string>()) : "";
        if (detail != "summary" && detail != "points") {
            call->fail("detail: " + args["detail"].dump() + " is not valid. Use one of: summary, points.");
            return;
        }
    }
    std::vector<Vector3> samples;
    if (has(args, "sample")) {
        const Json& list = args["sample"];
        if (!list.is_array() || list.size() > MAX_SAMPLES) {
            call->fail("sample: must be a list of world points [[x, y, z], ...] (at most " + std::to_string(MAX_SAMPLES) + ").");
            return;
        }
        for (size_t n = 0; n < list.size(); n++) {
            Vector3 point;
            if (!readVec3(list[n], point, "sample[" + std::to_string(n) + "]", error)) {
                call->fail(error);
                return;
            }
            samples.push_back(point);
        }
    }
    if (has(args, "create") && has(args, "lattice")) {
        call->fail("Pass lattice (edit an existing one) or create (make a new one), not both.");
        return;
    }
    // targets made or changed by the previous calls may not have built their geometry yet (it
    // happens in the next frame): fit to them once they have
    if (!waited && has(args, "create") && args["create"].is_object() && has(args["create"], "targets")) {
        std::vector<Entity> targets;
        std::string ignored;
        if (McpRefs::resolveEntities(s.sp, args["create"]["targets"], targets, ignored) &&
            lattice_mcp::targetsPending(s.project, s.sceneId, targets)) {
            Project* project = s.project;
            const uint32_t sceneId = s.sceneId;
            call->waitUntil([project, sceneId, targets]() { return !lattice_mcp::targetsPending(project, sceneId, targets); },
                            lattice_mcp::TARGETS_WAIT_MS, [call](bool) { editLatticeRun(call, true); });
            return;
        }
    }

    UndoStep step(s.sceneId);
    auto failCall = [&step, &call](const std::string& message) {
        const bool undone = step.changed();
        step.rollback();
        call->fail(undone ? "Nothing was changed (edit_lattice is one undo step and was rolled back): " + message : message);
    };
    if (has(args, "create")) {
        if (!runCreate(s, args["create"], error)) {
            failCall(error);
            return;
        }
    } else if (!resolveLattice(s, error)) {
        call->fail(error);
        return;
    }
    if (!runSetters(s, step, error) || (has(args, "ops") && !runOps(s, args["ops"], error))) {
        failCall(error);
        return;
    }
    const bool changed = step.changed();
    if (changed) step.commit();

    LatticeView view;
    if (!s.view(view)) {
        call->fail("The lattice is gone (an op deleted it?).");
        return;
    }
    Json warnings;
    Json data = describe(s, view, detail == "points", samples, warnings);
    if (edits) data["ops"] = s.opResults;
    if (!s.notes.empty()) data["notes"] = s.notes;

    const LatticeComponent& c = *view.component;
    std::string text = std::string(has(args, "create") ? "Created" : changed ? "Edited" : "Lattice") + " " + label(s.scene(), s.entity) + " " +
                       std::to_string(c.resolutionX) + "x" + std::to_string(c.resolutionY) + "x" + std::to_string(c.resolutionZ) + " " +
                       data["interpolation"].get<std::string>() + "/" + data["outside"].get<std::string>() + ": " +
                       std::to_string(data["moved_points"].get<int>()) + " of " + std::to_string(view.count()) + " points moved, " +
                       std::to_string(data["targets"].size()) + " target(s)" + (changed ? " (one undo step)." : ".");
    for (const Json& op : s.opResults) {
        if (op.contains("path")) text += "\nBaked " + op["path"].get<std::string>() + (op.contains("note") ? ": " + op["note"].get<std::string>() : "");
    }
    for (const Json& note : s.notes) text += "\nNote: " + note.get<std::string>();
    for (const Json& warning : warnings) text += "\nWarning: " + warning["message"].get<std::string>();
    call->ok(text, data);
}

} // namespace

void registerLatticeTools(McpRegistry& registry) {
    using namespace schema;
    const Json integerItem = Json{{"type", "integer"}};
    const Json resolution = Json{{"type", "array"}, {"items", integerItem}, {"minItems", 3}, {"maxItems", 3}};

    ToolDef tool;
    tool.name = "edit_lattice";
    tool.title = "Edit Lattice";
    tool.description =
        "Lattice (FFD cage) edits as in the Scene view; one undo step, undone on any error. lattice: "
        "default: selected/only one; create fits one around targets, bound. Then resolution (resamples), "
        "interpolation, outside, fade_distance, then ops in order. Offsets: lattice-local (unit cube). "
        "Returns targets, moved_points, max_offset, warnings; sample: world points -> deformed, stretch. No "
        "edits: a read (play too). Op fields, select grammar, results: jasper://docs/lattice.";
    tool.inputSchema = object({
        {"scene", sceneRef()},
        {"lattice", entityRef()},
        {"create", object({{"targets", entities()}, {"name", string("")}, {"resolution", resolution},
                           {"fit_padding", number("")}})},
        {"resolution", resolution},
        {"interpolation", stringEnum("", {"sharp", "smooth", "cubic"})},
        {"outside", stringEnum("", {"extend", "fade"})},
        {"fade_distance", number("")},
        {"ops", array(Json{{"type", "object"}, {"properties", {{"op", stringEnum("", {"set", "move", "reset", "preset", "mirror",
                                                                                         "fit", "bind", "unbind", "follow", "pose", "bake"})}}},
                           {"required", {"op"}}, {"additionalProperties", true}}, "")},   // each op names its own keys
        {"sample", array(Json{{"type", "array"}}, "")},
        {"detail", stringEnum("", {"summary", "points"})}
    });
    tool.handler = editLatticeTool;
    registry.addTool(std::move(tool));
}

}
