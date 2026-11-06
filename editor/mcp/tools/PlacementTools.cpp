// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Physics Placement: place_with_physics (design spec
// docs/superpowers/specs/2026-10-23-physics-placement-design.md section 8.1), the batch side of
// editor/physics/PlacementSession, which the Scene view's Physics tool and the place-physics CLI
// share. The run is a job stepped on the main thread in slices of a few milliseconds per frame;
// a preview changes nothing, apply: true commits the same result as one undo step.
#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "component/Transform.h"
#include "physics/ConvexDecomposition.h"
#include "physics/PlacementSession.h"
#include "registry/TagManager.h"
#include "render/SceneRender.h"
#include "texsynth/SynthesisSession.h"
#include "window/SceneWindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace jasperengine::editor::mcp {

namespace {

// Wall time of stepping per editor frame: the editor stays responsive while a run goes on
constexpr double SLICE_MS = 4.0;
// A ray given as {origin, direction} reaches this far (a screen ray spans the camera's range)
constexpr float RAY_LENGTH = 1000.0f;
constexpr uint64_t MAX_SEED = 4294967295ULL;
// structuredContent beyond this lists the first entities only (an agent reads every byte of it)
constexpr size_t MAX_RESULT_BYTES = 256 * 1024;
// refusal and warning lines in the text; the rest is in structuredContent
constexpr size_t MAX_TEXT_LINES = 10;

std::string fixed(double v, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
    return buffer;
}

bool keysAllowed(const Json& object, const std::string& field, const std::set<std::string>& keys, std::string& error) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!keys.count(it.key())) {
            std::string list;
            for (const std::string& k : keys) list += (list.empty() ? "" : ", ") + k;
            error = field + " has an unknown key \"" + it.key() + "\" (allowed: " + list + ").";
            return false;
        }
    }
    return true;
}

bool numberArg(const Json& args, const char* key, double minimum, double maximum, double& out, std::string& error) {
    if (!args.contains(key) || args[key].is_null()) return true;
    const Json& v = args[key];
    if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < minimum || v.get<double>() > maximum) {
        error = std::string(key) + " must be a number from " + fixed(minimum, minimum == std::floor(minimum) ? 0 : 2) + " to " + fixed(maximum, maximum == std::floor(maximum) ? 0 : 2) +
                " (got " + v.dump() + ").";
        return false;
    }
    out = v.get<double>();
    return true;
}

bool boolArg(const Json& args, const char* key, bool& out, std::string& error) {
    if (!args.contains(key) || args[key].is_null()) return true;
    if (!args[key].is_boolean()) {
        error = std::string(key) + " must be true or false (got " + args[key].dump() + ").";
        return false;
    }
    out = args[key].get<bool>();
    return true;
}

bool vectorArg(const Json& value, const std::string& field, Vector3& out, std::string& error) {
    if (!McpValues::parseVector3(value, out) || !std::isfinite(out.x) || !std::isfinite(out.y) || !std::isfinite(out.z)) {
        error = field + " must be [x,y,z] (got " + value.dump() + ").";
        return false;
    }
    return true;
}

bool parseAxis(const Json& value, int& out) {
    if (!value.is_string()) return false;
    const std::string axis = value.get<std::string>();
    if (axis == "x") out = 0;
    else if (axis == "y") out = 1;
    else if (axis == "z") out = 2;
    else return false;
    return true;
}

// ignore_layers: names or indices 0-31 -> a mask
bool layerMaskArg(const Json& value, uint32_t& mask, std::string& error) {
    const Json list = value.is_array() ? value : Json::array({value});
    for (const Json& item : list) {
        int layer = -1;
        if (item.is_number_integer()) layer = item.get<int>();
        else if (item.is_string()) layer = TagManager::nameToLayer(item.get<std::string>());
        if (layer < 0 || layer >= MAX_LAYERS) {
            std::string layers;
            for (int i = 0; i < MAX_LAYERS; i++) {
                const std::string name = TagManager::layerToName(i);
                if (!name.empty()) layers += (layers.empty() ? "" : ", ") + std::to_string(i) + "=" + name;
            }
            error = "ignore_layers: " + item.dump() + " is not a layer (0-31 or a name). Layers: " + layers + ".";
            return false;
        }
        mask |= 1u << layer;
    }
    return true;
}

struct ScreenshotOptions {
    bool wanted = false;
    int maxWidth = 768;
    bool frame = true;
};

// The tool's arguments as a spec; false with a fix-it error. Every default is a
// default-constructed PhysicsPlacementSettings (never the panel's), so a call means the same
// thing in every editor.
bool specFromArgs(Project* project, SceneProject* sp, SceneWindow* sceneWindow, const Json& args, PlacementSpec& spec, ScreenshotOptions& shot,
                  bool& screenRay, std::string& error) {
    spec = PlacementSpec();
    spec.sceneId = sp->id;
    PhysicsPlacementSettings& settings = spec.settings;

    // movers
    bool useSelection = false;
    if (!boolArg(args, "use_selection", useSelection, error)) return false;
    const bool hasEntities = args.contains("entities") && !args["entities"].is_null();
    std::vector<Entity> selection;
    if (useSelection) {
        if (hasEntities) {
            error = "Pass entities or use_selection: true, not both.";
            return false;
        }
        selection = sp->selectedEntities;
        if (selection.empty()) {
            error = "Nothing is selected in \"" + sp->name + "\": pass entities (ids, paths or names) instead of use_selection.";
            return false;
        }
    } else {
        if (!hasEntities) {
            error = "entities is required: the GameObjects to place (ids, paths like \"Props/Crate\" or names), or use_selection: true "
                    "for the editor's selection.";
            return false;
        }
        if (!args["entities"].is_array() || args["entities"].empty()) {
            error = "entities must be a non-empty array of GameObject references (ids, paths or names).";
            return false;
        }
        if (!McpRefs::resolveEntities(sp, args["entities"], selection, error)) {
            error = "entities: " + error;
            return false;
        }
    }
    spec.entities = placementRoots(project, sp->id, selection);

    // mode
    const Json mode = args.contains("mode") && !args["mode"].is_null() ? args["mode"] : Json("drop");
    if (!mode.is_string() || !parsePlacementMode(mode.get<std::string>().c_str(), spec.mode)) {
        error = "mode must be drop, separate, move, snap, gather or distribute (got " + mode.dump() + ").";
        return false;
    }
    const std::string modeName = placementModeName(spec.mode);

    // target (move)
    if (args.contains("target") && !args["target"].is_null()) {
        if (spec.mode != PlacementMode::Move) {
            error = "target is only used by mode: move (got mode " + modeName + ").";
            return false;
        }
        const Json& target = args["target"];
        if (!target.is_object()) {
            error = "target must be {position | delta, rotation?, space?}, e.g. {\"delta\": [2, 0, 0]}.";
            return false;
        }
        if (!keysAllowed(target, "target", {"position", "rotation", "delta", "space"}, error)) return false;
        if (target.contains("position") && target.contains("delta")) {
            error = "target takes position (where the group centre goes) or delta (an offset), not both.";
            return false;
        }
        if (target.contains("position")) {
            if (!vectorArg(target["position"], "target.position", spec.targetPosition, error)) return false;
            spec.hasTargetPosition = true;
        }
        if (target.contains("delta")) {
            if (!vectorArg(target["delta"], "target.delta", spec.targetDelta, error)) return false;
            spec.hasTargetDelta = true;
        }
        if (target.contains("rotation")) {
            spec.targetRotation = Quaternion::IDENTITY;
            std::string why;
            if (!McpValues::parseQuaternion(target["rotation"], spec.targetRotation, &why)) {
                error = "target.rotation must be {\"euler\":[x,y,z]} degrees or a unit [x,y,z,w]: " + why + " (got " +
                        target["rotation"].dump() + ").";
                return false;
            }
            spec.hasTargetRotation = true;
        }
        const Json space = target.contains("space") ? target["space"] : Json("world");
        if (space != "world" && space != "local") {
            error = "target.space must be world or local (the first mover's parent space), got " + space.dump() + ".";
            return false;
        }
        spec.targetLocal = space == "local";
        if (!spec.hasTargetPosition && !spec.hasTargetDelta && !spec.hasTargetRotation) {
            error = "target needs position, delta and/or rotation, e.g. {\"delta\": [2, 0, 0]}.";
            return false;
        }
    } else if (spec.mode == PlacementMode::Move) {
        error = "mode move needs target: {position | delta, rotation?, space?}, e.g. {\"delta\": [2, 0, 0]}; the group slides there "
                "and stops at what it hits.";
        return false;
    }

    // ray (snap)
    screenRay = false;
    if (args.contains("ray") && !args["ray"].is_null()) {
        if (spec.mode != PlacementMode::Snap) {
            error = "ray is only used by mode: snap (got mode " + modeName + ").";
            return false;
        }
        const Json& ray = args["ray"];
        if (!ray.is_object()) {
            error = "ray must be {origin, direction} (world) or {screen: [x, y]} (a Scene view pixel).";
            return false;
        }
        if (!keysAllowed(ray, "ray", {"origin", "direction", "screen"}, error)) return false;
        if (ray.contains("screen")) {
            if (ray.contains("origin") || ray.contains("direction")) {
                error = "ray takes {screen: [x, y]} or {origin, direction}, not both.";
                return false;
            }
            Vector2 pixel;
            if (!McpValues::parseVector2(ray["screen"], pixel) || !std::isfinite(pixel.x) || !std::isfinite(pixel.y)) {
                error = "ray.screen must be [x, y] in Scene view pixels (got " + ray["screen"].dump() + ").";
                return false;
            }
            const int width = sceneWindow ? sceneWindow->getWidth(sp->id) : 0;
            const int height = sceneWindow ? sceneWindow->getHeight(sp->id) : 0;
            if (!sp->sceneRender || width <= 0 || height <= 0) {
                error = "ray.screen is a pixel of the Scene view of \"" + sp->name + "\", which has not been shown: show it first "
                        "(focus_window scene, or screenshot view scene), or pass {origin, direction}.";
                return false;
            }
            if (pixel.x < 0.0f || pixel.y < 0.0f || pixel.x >= width || pixel.y >= height) {
                error = "ray.screen " + ray["screen"].dump() + " is outside the Scene view (" + std::to_string(width) + "x" +
                        std::to_string(height) + " pixels, y down; a screenshot with max_width 0 has the same pixels).";
                return false;
            }
            screenRay = true;
            spec.snapRay = sp->sceneRender->screenToRay(pixel.x, pixel.y);
        } else {
            if (!ray.contains("origin") || !ray.contains("direction")) {
                error = "ray needs origin and direction (world space), e.g. {\"origin\": [0, 10, 0], \"direction\": [0, -1, 0]}, or "
                        "{screen: [x, y]}.";
                return false;
            }
            Vector3 origin, direction;
            if (!vectorArg(ray["origin"], "ray.origin", origin, error)) return false;
            if (!vectorArg(ray["direction"], "ray.direction", direction, error)) return false;
            if (direction.length() < 1e-6f) {
                error = "ray.direction must not be zero.";
                return false;
            }
            spec.snapRay = Ray(origin, direction.normalize() * RAY_LENGTH);
        }
        spec.hasSnapRay = true;
    } else if (spec.mode == PlacementMode::Snap) {
        error = "mode snap needs ray: {origin, direction} (world) or {screen: [x, y]} (a Scene view pixel); the movers land where it hits.";
        return false;
    }

    // axis (distribute), amount (gather)
    if (args.contains("axis") && !args["axis"].is_null()) {
        if (spec.mode != PlacementMode::Distribute) {
            error = "axis is only used by mode: distribute (got mode " + modeName + ").";
            return false;
        }
        if (!parseAxis(args["axis"], spec.axis)) {
            error = "axis must be x, y or z (got " + args["axis"].dump() + ").";
            return false;
        }
    } else if (spec.mode == PlacementMode::Distribute) {
        error = "mode distribute needs axis: x, y or z (the movers are spaced evenly along it between the outermost two).";
        return false;
    }
    if (args.contains("amount") && !args["amount"].is_null()) {
        if (spec.mode != PlacementMode::Gather) {
            error = "amount is only used by mode: gather (got mode " + modeName + ").";
            return false;
        }
        double amount = 1.0;
        if (!numberArg(args, "amount", 0.0, 1.0, amount, error)) return false;
        spec.amount = static_cast<float>(amount);
    }

    // simulation
    if (args.contains("gravity") && !args["gravity"].is_null()) {
        if (spec.mode == PlacementMode::Separate) {
            error = "separate runs without gravity: leave gravity out.";
            return false;
        }
        if (!vectorArg(args["gravity"], "gravity", spec.gravity, error)) return false;
    }
    settings.gravity = spec.gravity;
    if (args.contains("settle") && !args["settle"].is_null()) {
        if (spec.mode == PlacementMode::Separate) {
            error = "separate never settles under gravity: leave settle out.";
            return false;
        }
        if (!boolArg(args, "settle", spec.settle, error)) return false;
    }
    if (spec.mode == PlacementMode::Separate) spec.settle = false;

    // context
    if (args.contains("context") && !args["context"].is_null()) {
        const Json& context = args["context"];
        if (context == "auto") {
            spec.context = PlacementSpec::Context::Auto;
        } else if (context == "none") {
            spec.context = PlacementSpec::Context::None;
        } else if (context.is_array() && !context.empty()) {
            spec.context = PlacementSpec::Context::Entities;
            if (!McpRefs::resolveEntities(sp, context, spec.contextEntities, error)) {
                error = "context: " + error;
                return false;
            }
        } else {
            error = "context must be \"auto\" (what is around the movers), \"none\" (only the movers) or a non-empty array of "
                    "GameObjects (got " + context.dump() + ").";
            return false;
        }
    }
    double margin = spec.contextMargin;
    if (!numberArg(args, "context_margin", 0.0, 1000.0, margin, error)) return false;
    spec.contextMargin = static_cast<float>(margin);
    settings.regionMargin = spec.contextMargin;
    if (args.contains("ignore_layers") && !args["ignore_layers"].is_null()) {
        if (!layerMaskArg(args["ignore_layers"], settings.ignoreLayerMask, error)) return false;
    }
    if (!boolArg(args, "push_neighbours", settings.pushNeighbours, error)) return false;

    // colliders
    if (args.contains("colliders") && !args["colliders"].is_null()) {
        const Json& v = args["colliders"];
        if (!v.is_string() || !parsePlacementColliderPolicy(v.get<std::string>().c_str(), settings.colliders)) {
            error = "colliders must be auto, authored, generated or both (got " + v.dump() + ").";
            return false;
        }
    }
    if (args.contains("decompose") && !args["decompose"].is_null()) {
        const Json& v = args["decompose"];
        if (!v.is_string() || !parsePlacementDecomposition(v.get<std::string>().c_str(), settings.decomposition)) {
            error = "decompose must be never, cached or always (got " + v.dump() + ").";
            return false;
        }
    }

    // jitter, seed, limits, feel
    if (args.contains("jitter") && !args["jitter"].is_null()) {
        const Json& jitter = args["jitter"];
        if (!jitter.is_object()) {
            error = "jitter must be {position?, rotation_deg?}: the largest random offset (m) and turn (degrees) applied before the "
                    "first step.";
            return false;
        }
        if (!keysAllowed(jitter, "jitter", {"position", "rotation_deg"}, error)) return false;
        double position = 0.0, rotation = 0.0;
        if (!numberArg(jitter, "position", 0.0, 100.0, position, error)) {
            error = "jitter." + error;
            return false;
        }
        if (!numberArg(jitter, "rotation_deg", 0.0, 180.0, rotation, error)) {
            error = "jitter." + error;
            return false;
        }
        spec.jitter.position = static_cast<float>(position);
        spec.jitter.rotationDeg = static_cast<float>(rotation);
    }
    if (args.contains("seed") && !args["seed"].is_null()) {
        const Json& seed = args["seed"];
        if (!seed.is_number_unsigned() || seed.get<uint64_t>() > MAX_SEED) {
            error = "seed must be a whole number from 0 to 4294967295 (got " + seed.dump() + ").";
            return false;
        }
        spec.seed = static_cast<uint32_t>(seed.get<uint64_t>());
    }
    double maxSeconds = spec.maxSeconds;
    if (!numberArg(args, "max_seconds", 0.01, 120.0, maxSeconds, error)) return false;
    spec.maxSeconds = static_cast<float>(maxSeconds);
    settings.maxSettleSeconds = spec.maxSeconds;
    if (args.contains("feel") && !args["feel"].is_null()) {
        const Json& v = args["feel"];
        if (!v.is_string() || !parsePlacementFeel(v.get<std::string>().c_str(), settings.feel)) {
            error = "feel must be precise, balanced or fast (got " + v.dump() + ").";
            return false;
        }
    }

    // screenshot
    shot = ScreenshotOptions();
    if (args.contains("screenshot") && !args["screenshot"].is_null()) {
        const Json& value = args["screenshot"];
        if (value.is_boolean()) {
            shot.wanted = value.get<bool>();
        } else if (value.is_object()) {
            if (!keysAllowed(value, "screenshot", {"max_width", "frame"}, error)) return false;
            shot.wanted = true;
            double width = shot.maxWidth;
            if (!numberArg(value, "max_width", 0, 4096, width, error)) {
                error = "screenshot." + error;
                return false;
            }
            shot.maxWidth = static_cast<int>(width);
            if (!boolArg(value, "frame", shot.frame, error)) {
                error = "screenshot." + error;
                return false;
            }
        } else {
            error = "screenshot must be true or {max_width?, frame?} (a Scene view image framed on the movers).";
            return false;
        }
    }
    // a screen ray means a pixel of the view as it is: framing would move the camera, and the
    // same arguments would then aim elsewhere on the next call
    if (screenRay && !(args["screenshot"].is_object() && args["screenshot"].contains("frame"))) shot.frame = false;
    return true;
}

// ---- preview poses shown for a screenshot

// The previewed poses written into the scene's Transforms for the few frames a screenshot takes,
// without an undo step, and put back afterwards. Save and Play put them back first
// (App::stopTransientPreviews -> stopPlacementPreviews), so they never reach a file or a play
// session. A Transform someone changed meanwhile is left as it is.
struct ShownPreview {
    struct Pose {
        Entity entity = NULL_ENTITY;
        Vector3 originalPosition;
        Quaternion originalRotation;
        Vector3 previewPosition;
        Quaternion previewRotation;
    };
    uint32_t sceneId = 0;
    Scene* scene = nullptr;
    uint64_t generation = 0;
    std::vector<Pose> poses;
    bool active = false;
    bool withdrawn = false;   // put back early (save, play) before the capture finished
};

std::vector<std::shared_ptr<ShownPreview>>& shownPreviews() {
    static std::vector<std::shared_ptr<ShownPreview>> list;
    return list;
}

bool previewShownIn(uint32_t sceneId) {
    for (const auto& p : shownPreviews()) {
        if (p->active && p->sceneId == sceneId && p->generation == texsynth::projectGeneration()) return true;
    }
    return false;
}

void restorePreview(Project* project, ShownPreview& preview) {
    if (!preview.active) return;
    preview.active = false;
    auto& list = shownPreviews();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::shared_ptr<ShownPreview>& p) { return p.get() == &preview; }),
               list.end());
    // another project, or the scene closed: its Transforms are gone
    if (!project || preview.generation != texsynth::projectGeneration()) return;
    SceneProject* sp = project->getScene(preview.sceneId);
    if (!sp || sp->scene != preview.scene) return;
    for (const ShownPreview::Pose& pose : preview.poses) {
        Transform* t = sp->scene->findComponent<Transform>(pose.entity);
        if (!t || !(t->position == pose.previewPosition) || !(t->rotation == pose.previewRotation)) continue;
        t->position = pose.originalPosition;
        t->rotation = pose.originalRotation;
        t->needUpdate = true;
    }
    McpValues::refreshTransforms(sp);
    sp->needUpdateRender = true;
}

std::shared_ptr<ShownPreview> showPreview(SceneProject* sp, const PlacementResult& result) {
    auto preview = std::make_shared<ShownPreview>();
    preview->sceneId = sp->id;
    preview->scene = sp->scene;
    preview->generation = texsynth::projectGeneration();
    for (const PlacementEntityResult& e : result.entities) {
        Transform* t = sp->scene->findComponent<Transform>(e.entity);
        if (!t) continue;
        preview->poses.push_back({e.entity, t->position, t->rotation, e.afterLocalPosition, e.afterLocalRotation});
        t->position = e.afterLocalPosition;
        t->rotation = e.afterLocalRotation;
        t->needUpdate = true;
    }
    preview->active = true;
    shownPreviews().push_back(preview);
    McpValues::refreshTransforms(sp);
    sp->needUpdateRender = true;
    return preview;
}

// ---- previews an apply commits as they are

// What a scene looked like to a run: placementSceneEditStamp (its structure, history position and
// every GameObject's Transform, parent, components, layer and active flag), plus what the stamp
// can't see: the project and Scene object it belongs to, and a count of every command applied,
// undone or redone there (an undo then another edit of a component value lands on the same
// history position with the same stamp). Two equal states read the same scene.
struct SceneState {
    uint64_t generation = 0;
    Scene* scene = nullptr;
    uint64_t stamp = 0;
    uint64_t edits = 0;

    bool operator==(const SceneState& o) const {
        return generation == o.generation && scene == o.scene && stamp == o.stamp && edits == o.edits;
    }
};

std::map<size_t, uint64_t>& sceneEdits() {
    static std::map<size_t, uint64_t> edits;
    return edits;
}

// Counts every command applied, undone or redone per scene, chained after the editor's own
// listener (App sets it at startup, before any tool call)
void watchSceneEdits() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    std::function<void(size_t, bool)> previous = CommandHistory::onSceneModified;
    CommandHistory::onSceneModified = [previous](size_t sceneId, bool structural) {
        if (previous) previous(sceneId, structural);
        sceneEdits()[sceneId]++;
    };
}

SceneState sceneState(Project* project, SceneProject* sp) {
    SceneState state;
    state.generation = texsynth::projectGeneration();
    state.scene = sp->scene;
    state.stamp = placementSceneEditStamp(project, sp->id);
    auto edits = sceneEdits().find(sp->id);
    state.edits = edits != sceneEdits().end() ? edits->second : 0;
    return state;
}

// The last previews (at most MAX_STORED_PREVIEWS): an apply with the same arguments on the same,
// unchanged scene commits the stored result instead of simulating again. A preview may have asked
// for convex decompositions (decompose: cached) that land before the apply; simulating again
// would then build other shapes and give other poses.
constexpr size_t MAX_STORED_PREVIEWS = 8;

struct StoredPreview {
    uint32_t sceneId = 0;
    std::string fingerprint;
    SceneState state;
    PlacementResult result;
};

std::vector<StoredPreview>& storedPreviews() {
    static std::vector<StoredPreview> list;
    return list;
}

// Drops previews of another project, a closed scene or a scene whose structure changed
void pruneStoredPreviews(Project* project) {
    auto& list = storedPreviews();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const StoredPreview& p) {
        if (!project || p.state.generation != texsynth::projectGeneration()) return true;
        SceneProject* sp = project->getScene(p.sceneId);
        return !sp || sp->scene != p.state.scene || sp->structureVersion != p.result.sceneStructureVersion;
    }), list.end());
}

void storePreview(Project* project, uint32_t sceneId, const std::string& fingerprint, const SceneState& state,
                  const PlacementResult& result) {
    if (fingerprint.empty()) return;
    watchSceneEdits();
    pruneStoredPreviews(project);
    auto& list = storedPreviews();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const StoredPreview& p) {
        return p.sceneId == sceneId && p.fingerprint == fingerprint;
    }), list.end());
    list.insert(list.begin(), StoredPreview{sceneId, fingerprint, state, result});
    if (list.size() > MAX_STORED_PREVIEWS) list.resize(MAX_STORED_PREVIEWS);
}

// The stored preview of these arguments, taken out of the store, when the scene is as it was
std::optional<PlacementResult> takeStoredPreview(Project* project, SceneProject* sp, const std::string& fingerprint) {
    pruneStoredPreviews(project);
    if (fingerprint.empty()) return std::nullopt;
    auto& list = storedPreviews();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->sceneId != sp->id || it->fingerprint != fingerprint) continue;
        const bool unchanged = it->state == sceneState(project, sp);
        std::optional<PlacementResult> out;
        if (unchanged) out = it->result;
        list.erase(it);
        return out;
    }
    return std::nullopt;
}

// ---- place_with_physics

struct PlacementCall {
    McpContext* ctx = nullptr;
    CallPtr call;
    JobPtr job;
    PlacementSpec spec;
    uint32_t sceneId = 0;
    Scene* scene = nullptr;            // the scene the run reads: gone or replaced = abandon
    uint64_t generation = 0;           // texsynth::projectGeneration() when the call started
    uint64_t structureVersion = 0;
    bool apply = false;
    ScreenshotOptions screenshot;
    bool screenRay = false;
    enum class Phase { Start, Simulating, Finishing, Done } phase = Phase::Start;
    std::vector<Entity> decomposing;   // decompose: always: the movers whose parts get decomposed
    bool committed = false;
    std::unique_ptr<PlacementRun> run;
    std::optional<PlacementResult> fromPreview;   // apply: the stored preview it commits (no run)
    SceneState startState;                         // a preview: the scene it read
    std::string fingerprint;
    // filled when the job ends
    std::string text;
    std::vector<ToolImage> images;
};
using PlacementCallPtr = std::shared_ptr<PlacementCall>;

const PlacementResult& placementResult(const PlacementCall& state) {
    return state.fromPreview ? *state.fromPreview : state.run->result();
}

void scheduleTick(const PlacementCallPtr& state);

Json specEcho(const PlacementCall& state) {
    Json data = {{"applied", false}, {"mode", placementModeName(state.spec.mode)}, {"seed", state.spec.seed}};
    if (state.spec.mode == PlacementMode::Snap && state.spec.hasSnapRay) {
        data["ray"] = {{"origin", McpValues::toJson(state.spec.snapRay.getOrigin())},
                       {"direction", McpValues::toJson(state.spec.snapRay.getDirection())}};
    }
    return data;
}

// The scene the run reads is still there, unchanged, in the same project, out of Play mode.
// `busy`: it is being saved or loaded right now (wait a frame, neither step nor commit)
bool sceneStillValid(PlacementCall& state, bool& busy, std::string& why) {
    busy = false;
    Project* project = state.ctx->project();
    if (!project || state.generation != texsynth::projectGeneration()) {
        why = "Another project was opened during the simulation: nothing was changed. Call place_with_physics again in the open project.";
        return false;
    }
    SceneProject* sp = project->getScene(state.sceneId);
    if (!sp || sp->scene != state.scene) {
        why = "The scene closed during the simulation: nothing was changed. Open it and call place_with_physics again.";
        return false;
    }
    if (project->isAnyScenePlaying() || sp->playState == ScenePlayState::PLAYING || sp->playState == ScenePlayState::PAUSED) {
        why = "Play mode started during the simulation: nothing was changed. Stop play and call place_with_physics again.";
        return false;
    }
    busy = sp->playState != ScenePlayState::STOPPED;
    if (sp->structureVersion != state.structureVersion) {
        why = "Nothing was changed: the scene changed during the simulation (GameObjects or components added, removed or "
              "re-parented): run it again.";
        return false;
    }
    // the Scene view's Physics tool is in a gesture or settle there: its Transforms hold its
    // preview poses until it commits or cancels
    if (placementSceneBusy(project, state.sceneId)) {
        busy = true;
        return true;
    }
    // any other edit (a moved floor, an undo): what the run read is not the scene any more
    if (state.phase != PlacementCall::Phase::Start && !busy && !previewShownIn(state.sceneId) && !(sceneState(project, sp) == state.startState)) {
        why = "Nothing was changed: the scene changed during the simulation (a GameObject moved, an edit, undo or redo): run it again.";
        return false;
    }
    return true;
}

std::string summaryText(const PlacementCall& state, const PlacementResult& r, const std::string& hint) {
    size_t movers = 0, neighbours = 0, settled = 0;
    for (const PlacementEntityResult& e : r.entities) {
        (e.mover ? movers : neighbours)++;
        if (e.settled) settled++;
    }
    const std::string mode = placementModeName(r.mode);
    std::string text = (state.apply ? "Placed " : "Previewed ") + mode + " of " + std::to_string(movers) + " GameObject(s)";
    if (neighbours) text += " (+" + std::to_string(neighbours) + " pushed neighbour(s))";
    if (r.mode == PlacementMode::Separate) text += r.allSettled ? ": no overlaps left" : ": overlaps remain";
    else text += ": " + (r.allSettled ? std::string("all settled") : std::to_string(settled) + " of " + std::to_string(r.entities.size()) + " settled");
    text += " after " + fixed(r.simSeconds, 2) + " s simulated (" + std::to_string(r.steps) + " steps), state_hash " + r.stateHash + ".";
    if (state.apply) text += " Committed as one undo step.";
    else text += " Nothing was changed: the same arguments with apply: true commit exactly this (one undo step).";
    SceneProject* sp = state.ctx->project() ? state.ctx->project()->getScene(state.sceneId) : nullptr;
    for (size_t i = 0; i < r.refused.size() && i < MAX_TEXT_LINES; i++) {
        const PlacementRefusal& refusal = r.refused[i];
        text += "\nRefused " + (sp ? McpRefs::entityPath(sp, refusal.entity) : std::to_string(refusal.entity)) + ": " + refusal.reason + ".";
    }
    if (r.refused.size() > MAX_TEXT_LINES) text += "\n+" + std::to_string(r.refused.size() - MAX_TEXT_LINES) + " more refused (see refused).";
    if (!hint.empty()) text += "\nHint: " + hint;
    for (size_t i = 0; i < r.warnings.size() && i < MAX_TEXT_LINES; i++) text += "\nWarning: " + r.warnings[i];
    if (r.warnings.size() > MAX_TEXT_LINES) text += "\n+" + std::to_string(r.warnings.size() - MAX_TEXT_LINES) + " more warnings (see warnings).";
    return text;
}

// A screenshot at the end of a run: the job ends when the image is there or the capture gave up
struct PlacementCapture {
    PlacementCallPtr state;
    std::shared_ptr<ShownPreview> preview;   // a preview: its poses, in the scene until the image is read
    Json data;
    bool finished = false;

    void complete(bool ok, ToolImage image, const std::string& note) {
        if (finished) return;
        finished = true;
        const bool withdrawn = preview && preview->withdrawn;
        if (preview) restorePreview(state->ctx->project(), *preview);
        Json shot = {{"shows", state->apply ? "the committed poses" : withdrawn ? "the current scene" : "the previewed poses"}};
        if (ok) {
            state->images.push_back(std::move(image));
            state->text += state->apply ? "\nImage: the Scene view after the commit."
                           : withdrawn  ? "\nImage: the Scene view as it is (the preview poses were put back early for a save or Play)."
                                        : "\nImage: the Scene view showing the previewed poses (put back afterwards).";
            if (!note.empty()) shot["note"] = note;
        } else {
            shot["error"] = note;
            state->text += "\n(Screenshot failed: " + note + ")";
        }
        data["screenshot"] = shot;
        if (state->committed && state->job->cancelRequested()) {
            const std::string note = "cancel_job came after the commit: already committed; use undo to revert.";
            data["note"] = note;
            state->text += "\n" + note;
        }
        state->phase = PlacementCall::Phase::Done;
        state->run.reset();
        state->job->succeed(state->text, data);
    }
};
using PlacementCapturePtr = std::shared_ptr<PlacementCapture>;

// The call can end without the capture calling back (cancelled, timed out: its waits are
// dropped); the job must still end and the preview poses still go back
void watchCapture(const PlacementCapturePtr& capture) {
    getEditorHost().enqueueMainThreadTask([capture]() {
        // a continuation writes beneath the project, as the tool's run did
        McpWriteScope writeScope;
        if (capture->finished) return;
        const CallPtr& call = capture->state->call;
        if (call->completed() || call->cancelled()) {
            capture->complete(false, ToolImage(), "the call ended before the image was ready");
            return;
        }
        capture->state->ctx->app().requestRedraw();
        watchCapture(capture);
    });
}

// The Scene view framed on the movers; a preview shows its poses for the capture
void captureAndFinish(const PlacementCallPtr& state, Json data) {
    const CallPtr call = state->call;
    SceneProject* sp = state->ctx->project()->getScene(state->sceneId);
    auto capture = std::make_shared<PlacementCapture>();
    capture->state = state;
    capture->data = std::move(data);
    if (!state->apply) capture->preview = showPreview(sp, placementResult(*state));
    std::vector<Entity> movers;
    for (const PlacementEntityResult& e : placementResult(*state).entities) {
        if (e.mover) movers.push_back(e.entity);
    }
    watchCapture(capture);
    // a frame first, so the meshes' bounds follow the preview poses before framing on them
    call->afterFrames(1, [capture, call, movers]() {
        PlacementCall& s = *capture->state;
        Project* p = s.ctx->project();
        SceneProject* scene = p && s.generation == texsynth::projectGeneration() ? p->getScene(s.sceneId) : nullptr;
        if (!scene || scene->scene != s.scene) {
            capture->complete(false, ToolImage(), "the scene closed");
            return;
        }
        if (s.screenshot.frame && !movers.empty()) {
            if (SceneWindow* sw = s.ctx->sceneWindow()) sw->frameEntities(s.sceneId, s.sceneId, movers);
        }
        w3::CaptureRequest request;
        request.view = "scene";
        request.scene = s.sceneId;
        request.maxWidth = s.screenshot.maxWidth;
        w3::captureView(call, request, [capture](w3::CaptureResult result) {
            capture->complete(result.ok, std::move(result.image), result.ok ? result.note : result.error);
        });
    });
}

// A result over MAX_RESULT_BYTES keeps its first entities (and refusals) only, saying so: the
// rest of it (state_hash, counts, all_settled, warnings) still covers every body
void capEntities(Json& result) {
    if (result.dump().size() <= MAX_RESULT_BYTES) return;
    for (const char* key : {"entities", "refused"}) {
        if (!result.contains(key) || !result[key].is_array()) continue;
        Json& list = result[key];
        const size_t total = list.size();
        size_t bytes = 0;
        size_t kept = 0;
        for (; kept < total; kept++) {
            bytes += list[kept].dump().size() + 1;
            if (bytes > MAX_RESULT_BYTES / 2) break;
        }
        if (kept == total) continue;
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(kept), list.end());
        result[std::string(key) + "_total"] = total;
        result["truncated"] = std::string("only the first ") + std::to_string(kept) + " of " + std::to_string(total) + " " + key +
                              " are listed (the result would be over " + std::to_string(MAX_RESULT_BYTES / 1024) +
                              " KB); state_hash, all_settled, steps and warnings cover them all";
    }
}

// The run finished: commit when asked, build the result
void finishPlacement(const PlacementCallPtr& state) {
    JobPtr job = state->job;
    const PlacementResult& r = placementResult(*state);
    Json data = specEcho(*state);
    if (r.cancelled || job->cancelRequested()) {
        state->phase = PlacementCall::Phase::Done;
        state->run.reset();
        job->markCancelled("Physics placement cancelled; nothing was changed.");
        return;
    }
    if (!r.ok) {
        data["error"] = r.error;
        state->phase = PlacementCall::Phase::Done;
        const std::string message = "Physics placement failed: " + r.error + " Nothing was changed.";
        state->run.reset();
        job->fail(message, data);
        return;
    }
    Project* project = state->ctx->project();
    if (state->apply) {
        // sceneStillValid compared the scene with the run's start this frame; commitPlacement
        // checks its edit stamp, the committed Transforms and a busy Physics gesture itself
        std::string error;
        if (!commitPlacement(project, r, error)) {
            data["error"] = error;
            state->phase = PlacementCall::Phase::Done;
            state->run.reset();
            job->fail("Nothing was changed: " + error + ".", data);
            return;
        }
        state->committed = true;
    }
    Json result = placementResultJson(project, r, state->apply);
    if (!result.is_object()) result = Json::object();
    for (auto it = data.begin(); it != data.end(); ++it) {
        if (!result.contains(it.key())) result[it.key()] = it.value();
    }
    result["applied"] = state->apply;
    result["from_preview"] = state->fromPreview.has_value();
    capEntities(result);
    state->text = summaryText(*state, r, result.value("hint", r.hint));
    if (state->fromPreview) state->text += " (The stored preview of these arguments, committed without simulating again.)";
    if (!state->apply) {
        // kept for an apply with the same arguments, if the scene still reads the same then
        SceneProject* sp = project->getScene(state->sceneId);
        if (sp && sceneState(project, sp) == state->startState) storePreview(project, state->sceneId, state->fingerprint, state->startState, r);
    }
    // decompose: cached asks for missing decompositions and uses hulls meanwhile: once they are
    // in, a new simulation of the same arguments builds other shapes
    ConvexDecompositionCache* cache = placementDecompositionCache(project);
    if (!state->apply && state->spec.settings.decomposition == PlacementDecomposition::CACHED && cache && cache->pendingCount() > 0) {
        const std::string warning = "Convex decompositions are still being computed in the background: an apply right after this "
                                    "commits this preview, but a later run (or an apply after an edit) may use them and differ. "
                                    "For runs that repeat exactly pass decompose: always (waits for them) or never.";
        if (!result.contains("warnings") || !result["warnings"].is_array()) result["warnings"] = Json::array();
        result["warnings"].push_back(warning);
        state->text += "\nWarning: " + warning;
    }

    if (state->screenshot.wanted) {
        if (!state->call->completed() && !state->call->cancelled()) {
            state->phase = PlacementCall::Phase::Finishing;
            job->setProgress(1.0, "taking the screenshot");
            captureAndFinish(state, result);
            return;
        }
        result["screenshot"] = {{"error", "skipped: the call had already returned a job_id; take one with screenshot (view scene)"}};
    }
    state->phase = PlacementCall::Phase::Done;
    state->run.reset();
    job->succeed(state->text, result);
}

bool startRun(const PlacementCallPtr& state) {
    state->run = std::make_unique<PlacementRun>();
    if (state->run->start(state->ctx->project(), state->spec)) return true;
    Json data = specEcho(*state);
    const std::string error = state->run->result().error;
    data["error"] = error;
    state->phase = PlacementCall::Phase::Done;
    state->run.reset();
    state->job->fail("Can't place: " + error, data);
    return false;
}

void tick(const PlacementCallPtr& state) {
    JobPtr job = state->job;
    if (job->finished() || state->phase == PlacementCall::Phase::Done) {
        state->run.reset();
        return;
    }
    if (state->phase == PlacementCall::Phase::Finishing) return;   // the capture ends the job
    std::string why;
    bool busy = false;
    if (!sceneStillValid(*state, busy, why)) {
        state->phase = PlacementCall::Phase::Done;
        state->run.reset();
        job->fail(why, specEcho(*state));
        return;
    }
    if (job->cancelRequested()) {
        state->phase = PlacementCall::Phase::Done;
        state->run.reset();
        job->markCancelled("Physics placement cancelled; nothing was changed.");
        return;
    }
    // the scene is being saved, the Scene view's Physics tool or another call's screenshot has
    // preview poses in it: the run reads the scene (context is gathered as it goes), so it waits
    if (busy || previewShownIn(state->sceneId)) {
        if (placementSceneBusy(state->ctx->project(), state->sceneId)) {
            job->setProgress(0.0, "waiting for the Scene view's Physics gesture to end (its preview poses are in the scene)");
        }
        scheduleTick(state);
        return;
    }
    Project* project = state->ctx->project();
    switch (state->phase) {
        case PlacementCall::Phase::Start: {
            // the scene as the run reads it (here, not when the call came: the Physics tool or
            // a screenshot may have held preview poses until now)
            SceneProject* sp = project->getScene(state->sceneId);
            McpValues::refreshTransforms(sp);
            state->structureVersion = sp->structureVersion;
            state->startState = sceneState(project, sp);
            if (state->apply) state->fromPreview = takeStoredPreview(project, sp, state->fingerprint);
            if (state->fromPreview) {
                finishPlacement(state);
                return;
            }
            if (!startRun(state)) return;
            // decompose: always: the run waits for its movers' decompositions; they are counted
            // per mover for the progress (other decompositions in the cache are not waited for)
            ConvexDecompositionCache* cache = placementDecompositionCache(project);
            if (state->spec.settings.decomposition == PlacementDecomposition::ALWAYS && cache) {
                const PlacementWorldSettings settings = makePlacementWorldSettings(state->spec.settings, cache);
                for (Entity root : state->spec.entities) {
                    if (PlacementWorld::requestDecompositions(sp->scene, root, settings, nullptr, true) > 0) state->decomposing.push_back(root);
                }
            }
            state->phase = PlacementCall::Phase::Simulating;
            job->setProgress(0.0, "starting");
            break;
        }
        case PlacementCall::Phase::Simulating: {
            const bool done = state->run->advance(SLICE_MS);
            const std::string text = state->run->progressText();
            if (!done && !state->decomposing.empty() && text.rfind("waiting for colliders", 0) == 0) {
                const PlacementWorldSettings settings = makePlacementWorldSettings(state->spec.settings, placementDecompositionCache(project));
                Scene* scene = project->getScene(state->sceneId)->scene;
                size_t ready = 0;
                for (Entity root : state->decomposing) {
                    if (PlacementWorld::decompositionReady(scene, root, settings)) ready++;
                }
                job->setProgress(static_cast<double>(ready) / static_cast<double>(state->decomposing.size()),
                                 "Decomposing " + std::to_string(ready) + "/" + std::to_string(state->decomposing.size()));
            } else {
                job->setProgress(state->run->progress(), text);
            }
            if (done) {
                finishPlacement(state);
                return;
            }
            break;
        }
        case PlacementCall::Phase::Finishing:
        case PlacementCall::Phase::Done:
            return;
    }
    scheduleTick(state);
}

void scheduleTick(const PlacementCallPtr& state) {
    state->ctx->app().requestRedraw();
    getEditorHost().enqueueMainThreadTask([state]() {
        // a continuation writes beneath the project, as the tool's run did
        McpWriteScope writeScope;
        // whatever goes wrong, the job ends (the main-thread loop would swallow an exception and
        // leave it running forever)
        try {
            tick(state);
        } catch (const std::exception& e) {
            state->run.reset();
            if (state->job->status() == JobStatus::Running) state->job->fail(std::string("Physics placement failed: ") + e.what());
        } catch (...) {
            state->run.reset();
            if (state->job->status() == JobStatus::Running) state->job->fail("Physics placement failed.");
        }
    });
}

void placeWithPhysicsTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    std::string error;
    SceneProject* sp = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    if (!sp->scene) {
        call->fail(McpRefs::notOpenError(project, *sp));
        return;
    }
    if (sp->sceneType != SceneType::SCENE_3D) {
        call->fail("Physics placement works in 3D scenes; \"" + sp->name + "\" is a " + McpRefs::sceneTypeName(sp->sceneType) + " scene.");
        return;
    }
    if (sp->playState != ScenePlayState::STOPPED) {
        call->fail("\"" + sp->name + "\" is busy (loading, saving or playing); call again when it is stopped.");
        return;
    }
    auto state = std::make_shared<PlacementCall>();
    state->ctx = &ctx;
    state->call = call;
    if (!specFromArgs(project, sp, ctx.sceneWindow(), args, state->spec, state->screenshot, state->screenRay, error)) {
        call->fail(error);
        return;
    }
    bool wait = true;
    double timeout = 120.0;
    if (!boolArg(args, "apply", state->apply, error) || !boolArg(args, "wait", wait, error) ||
        !numberArg(args, "timeout_seconds", 1.0, 3600.0, timeout, error)) {
        call->fail(error);
        return;
    }
    const std::vector<std::string> problems = checkPlacementSpec(project, state->spec);
    if (!problems.empty()) {
        std::string text = "Can't place:";
        for (const std::string& p : problems) text += "\n- " + p;
        call->fail(text, Json{{"errors", problems}});
        return;
    }
    state->sceneId = sp->id;
    state->scene = sp->scene;
    state->generation = texsynth::projectGeneration();
    state->structureVersion = sp->structureVersion;
    // world poses of GameObjects made or moved this frame (a batch) are only computed between
    // frames otherwise
    McpValues::refreshTransforms(sp);
    watchSceneEdits();
    state->fingerprint = state->spec.fingerprint();

    const std::string title = std::string("Physics ") + placementModeName(state->spec.mode) + " of " +
                              std::to_string(state->spec.entities.size()) + " GameObject(s)" + (state->apply ? "" : " (preview)");
    state->job = ctx.jobs().create("place_with_physics", title);
    std::weak_ptr<PlacementCall> weak = state;
    state->job->setCancelHandler([weak]() {
        // once committed (an apply's screenshot) it is too late: the job ends with a note
        if (auto s = weak.lock()) {
            if (s->run && s->phase == PlacementCall::Phase::Simulating) s->run->cancel();
        }
    });
    state->job->setProgress(0.0, "starting");
    scheduleTick(state);

    // the job ends the call (replyWhenJobDone), also when the screenshot at its end completes
    // inside another of the call's deferred steps
    call->detach();
    replyWhenJobDone(call, state->job, wait, timeout, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(state->text.empty() ? finished.message() : state->text, result);
        reply.images = state->images;
        return reply;
    });
}

}

void stopPlacementPreviews(Project* project) {
    const std::vector<std::shared_ptr<ShownPreview>> shown = shownPreviews();
    for (const std::shared_ptr<ShownPreview>& preview : shown) {
        preview->withdrawn = true;
        restorePreview(project, *preview);
    }
}

void registerPlacementTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "place_with_physics";
    tool.title = "Place With Physics";
    tool.description =
        "Edit-time physics placement (the scene's own physics never runs): drop, separate, move, snap, gather or distribute "
        "GameObjects, then they settle (generated colliders, nothing added). Preview by default; the same arguments with "
        "apply: true commit exactly the preview (same state_hash, one undo step). Read hint and warnings. "
        "Guide: jasper://docs/physics.";

    // what each argument does: docs/mcp/guide/physics.md (Placing objects with physics)
    Json context;
    context["default"] = "auto";
    context["anyOf"] = Json::array({stringEnum("", {"auto", "none"}), entities()});
    Json screenshot;
    screenshot["anyOf"] = Json::array({Json{{"type", "boolean"}},
                                       object({{"max_width", integer("", 0, 4096)}, {"frame", boolean("")}})});
    tool.inputSchema = object({
        {"scene", sceneRef()},
        {"entities", entities("The movers.")},
        {"use_selection", withDefault(boolean(""), false)},
        {"mode", withDefault(stringEnum("", {"drop", "separate", "move", "snap", "gather", "distribute"}), "drop")},
        {"target", object({{"position", vec3("")}, {"delta", vec3("")}, {"rotation", any("")},
                           {"space", withDefault(stringEnum("", {"world", "local"}), "world")}})},
        {"ray", object({{"origin", vec3("")}, {"direction", vec3("")}, {"screen", vec2("")}})},
        {"axis", stringEnum("", {"x", "y", "z"})},
        {"amount", withDefault(number("", 0, 1), 1)},
        {"gravity", withDefault(vec3(""), Json::array({0, -9.81, 0}))},
        {"settle", withDefault(boolean(""), true)},
        {"context", context},
        {"context_margin", withDefault(number("", 0, 1000), 2)},
        {"ignore_layers", array(Json{{"type", {"integer", "string"}}}, "")},
        {"push_neighbours", withDefault(boolean(""), false)},
        {"colliders", withDefault(stringEnum("", {"auto", "authored", "generated", "both"}), "auto")},
        {"decompose", withDefault(stringEnum("", {"never", "cached", "always"}), "cached")},
        {"jitter", object({{"position", number("")}, {"rotation_deg", number("")}})},
        {"seed", withDefault(integer("", 0, 4294967295LL), 1)},
        {"max_seconds", withDefault(number("", 0.01, 120), 10)},
        {"feel", withDefault(stringEnum("", {"precise", "balanced", "fast"}), "balanced")},
        {"apply", withDefault(boolean(""), false)},
        {"screenshot", screenshot},
        {"wait", withDefault(boolean(""), true)},
        {"timeout_seconds", withDefault(number("", 1, 3600), 120)}
    });
    tool.allowedWhilePlaying = false;
    tool.activity = ToolActivity::Edit;
    tool.handler = placeWithPhysicsTool;
    registry.addTool(std::move(tool));
}

}
