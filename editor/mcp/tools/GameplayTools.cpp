// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Gameplay (docs/specs/S22-gameplay.md): the input state the game reads (levels, the frame's
// edges and deltas, touches, gamepads, pointer lock, the Input Actions' values), and the
// .inputactions assets read and edited as undo steps. The guide is docs/mcp/guide/input.md.
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Engine.h"
#include "Input.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/InputActionsEditCmd.h"
#include "input/InputActionAssetJson.h"
#include "input/InputActions.h"
#include "input/InputControlPath.h"
#include "object/environment/Water.h"
#include "component/MeshComponent.h"
#include "component/WaterExcluderComponent.h"
#include "component/WaterWakeComponent.h"
#include "pool/ShaderPool.h"
#include "subsystem/WaterInteractionSystem.h"
#include "mcp/tools/PlayViewCommon.h"
#include "io/FileData.h"
#include "util/ProjectChecks.h"

#include <filesystem>

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;
namespace fs = std::filesystem;

double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

Json vec2Json(const Vector2& v) {
    return Json::array({rounded(v.x), rounded(v.y)});
}

const char* touchPhaseName(TouchPhase phase) {
    switch (phase) {
        case TouchPhase::BEGAN: return "began";
        case TouchPhase::MOVED: return "moved";
        case TouchPhase::STATIONARY: return "stationary";
        case TouchPhase::ENDED: return "ended";
        case TouchPhase::CANCELED: return "canceled";
    }
    return "stationary";
}

const char* pointerLockName(PointerLockState state) {
    switch (state) {
        case PointerLockState::UNLOCKED: return "unlocked";
        case PointerLockState::PENDING: return "pending";
        case PointerLockState::LOCKED: return "locked";
    }
    return "unlocked";
}

Json modsJson(int mods) {
    Json list = Json::array();
    if (mods & D_MODIFIER_SHIFT) list.push_back("shift");
    if (mods & D_MODIFIER_CONTROL) list.push_back("ctrl");
    if (mods & D_MODIFIER_ALT) list.push_back("alt");
    if (mods & D_MODIFIER_SUPER) list.push_back("super");
    return list;
}

// held / down / up names of a range of controls, read from the frame's window
template <typename NameFn>
void controlEdgesJson(int first, int count, int controlBase, NameFn name, Json& held, Json& down, Json& up) {
    for (int i = first; i < first + count; i++) {
        const InputControlEdges edges = Input::getControlEdges(controlBase + i, false);
        if (edges.level) held.push_back(name(i));
        if (edges.presses > 0) down.push_back(name(i));
        if (edges.releases > 0) up.push_back(name(i));
    }
}

Json rawJson() {
    Json keysHeld = Json::array(), keysDown = Json::array(), keysUp = Json::array();
    controlEdgesJson(0, Input::KEY_CONTROLS, 0, [](int key) { return InputControlNames::keyName(key); },
                     keysHeld, keysDown, keysUp);
    Json buttonsHeld = Json::array(), buttonsDown = Json::array(), buttonsUp = Json::array();
    controlEdgesJson(0, Input::MOUSE_CONTROLS, Input::MOUSE_CONTROL_BASE,
                     [](int button) { return InputControlNames::mouseButtonName(button); },
                     buttonsHeld, buttonsDown, buttonsUp);
    return {
        {"keys", {{"held", keysHeld}, {"down", keysDown}, {"up", keysUp}}},
        {"mouse", {
            {"position", vec2Json(Input::getMousePosition())},
            {"delta", vec2Json(Input::getMouseDelta())},
            {"scroll_delta", vec2Json(Input::getMouseScrollDelta())},
            {"held", buttonsHeld}, {"down", buttonsDown}, {"up", buttonsUp},
            {"entered", Input::isMouseEntered()}
        }},
        {"any_key_down", Input::anyKeyDown()},
        {"modifiers", modsJson(Input::getModifiers())},
        {"pointer_lock", pointerLockName(Input::getPointerLockState())}
    };
}

Json touchesJson() {
    Json list = Json::array();
    for (size_t i = 0; i < Input::getTouchCount(); i++) {
        const Touch touch = Input::getTouch(i);
        list.push_back({
            {"pointer", touch.pointer},
            {"phase", touchPhaseName(touch.phase)},
            {"position", vec2Json(touch.position)},
            {"delta", vec2Json(touch.delta)},
            {"start_position", vec2Json(touch.startPosition)},
            {"time", rounded(touch.time)},
            {"began", touch.began},
            {"down", Input::getTouchDown(touch.pointer)},
            {"up", Input::getTouchUp(touch.pointer)},
            {"tap_count", touch.tapCount}
        });
    }
    return list;
}

Json gamepadsJson() {
    Json list = Json::array();
    for (size_t i = 0; i < Input::numGamepads(); i++) {
        const int id = Input::getGamepadId(i);
        const int slot = Input::getGamepadSlot(id);
        Json held = Json::array(), down = Json::array(), up = Json::array();
        controlEdgesJson(0, Input::GAMEPAD_BUTTONS, Input::gamepadControl(slot, 0),
                         [](int button) { return InputControlNames::gamepadButtonName(button); }, held, down, up);
        Json axes = Json::object();
        for (int axis = 0; axis < Input::GAMEPAD_AXES; axis++) {
            axes[InputControlNames::gamepadAxisName(axis)] = rounded(Input::getGamepadAxis(id, axis));
        }
        list.push_back({{"gamepad", id}, {"name", Input::getGamepadName(id)}, {"held", held}, {"down", down},
                        {"up", up}, {"axes", axes}});
    }
    return list;
}


Json toJson(const InputJson& value) {
    return Json::parse(value.dump());
}

const char* phaseName(InputActionPhase phase) {
    switch (phase) {
        case InputActionPhase::WAITING: return "waiting";
        case InputActionPhase::STARTED: return "started";
        case InputActionPhase::PERFORMED: return "performed";
        case InputActionPhase::CANCELED: return "canceled";
    }
    return "waiting";
}

Json actionsStateJson() {
    Json maps = Json::array();
    for (const InputActions::MapState& map : InputActions::getMapStates()) {
        Json actions = Json::array();
        for (InputAction* action : InputActions::getMap(map.name).getActions()) {
            const Vector2 value = action->readVector2();
            Json item = {
                {"name", action->getName()},
                {"type", inputActionTypeName(action->getType())},
                {"value", action->getType() == InputActionType::VECTOR2 ? vec2Json(value) : Json(rounded(value.x))},
                {"pressed", action->isPressed()},
                {"pressed_this_frame", action->wasPressedThisFrame()},
                {"released_this_frame", action->wasReleasedThisFrame()},
                {"phase", phaseName(action->getPhase())}
            };
            const std::string consumer = action->getConsumedBy();
            if (!consumer.empty()) item["consumed_by"] = consumer;
            actions.push_back(item);
        }
        Json item = {{"name", map.name}, {"priority", map.priority}, {"enabled", map.enabled}, {"actions", actions}};
        if (map.gamepad >= 0) item["gamepad"] = map.gamepad;
        if (map.blocked) item["blocked"] = true;
        maps.push_back(item);
    }
    // built_in_defaults: no actions asset, Unity's Input Manager defaults
    return {{"asset", InputActions::getAssetPath()}, {"loaded", InputActions::isLoaded()},
            {"built_in_defaults", InputActions::usesBuiltInDefaults()}, {"rebinding", InputActions::isRebinding()}, {"maps", maps}};
}

// the asset file of a call: its text (empty for a file that does not exist yet) and model
bool readInputActions(const CallPtr& call, fs::path& file, bool create, std::string& text, InputActionAssetDesc& asset,
                      bool& exists) {
    const std::string relative = call->args().value("path", std::string());
    std::string error;
    if (!McpRefs::resolveProjectPath(call->ctx().project(), relative, file, error, false)) {
        call->fail(error.empty() ? "path is required (project-relative, e.g. assets/input/player.inputactions)." : error);
        return false;
    }
    if (file.extension() != ".inputactions") {
        call->fail("path must be an .inputactions file.");
        return false;
    }
    std::error_code ec;
    exists = fs::exists(file, ec);
    if (!exists) {
        if (!create) {
            call->fail(relative + " does not exist (edit_input_actions creates it).");
            return false;
        }
        text.clear();
        asset = InputActionAssetDesc();
        return true;
    }
    if (!ProjectChecks::readFile(file, text)) {
        call->fail("Could not read " + relative + ".");
        return false;
    }
    if (!InputActionAssetDesc::parse(text, asset, error)) {
        call->fail(relative + " is not an input actions asset: " + error);
        return false;
    }
    return true;
}

// the asset as an agent reads it: the file's JSON plus each binding's display name
Json assetJson(const InputActionAssetDesc& asset) {
    Json out = toJson(inputActionAssetToJson(asset));
    Json names = Json::object();
    for (const InputActionMapDesc& map : asset.maps) {
        for (const InputActionDesc& action : map.actions) {
            for (const InputBindingDesc& binding : action.bindings) {
                if (binding.isComposite()) {
                    std::string name;
                    const std::vector<int> order = binding.composite == InputCompositeType::DPAD_2D ? std::vector<int>{0, 2, 1, 3}
                                                                                                     : std::vector<int>{0, 1};
                    for (int index : order) {
                        if (index >= (int)binding.parts.size()) continue;
                        if (!name.empty()) name += "/";
                        name += InputActions::getControlDisplayName(binding.parts[index].second.path, binding.parts[index].second.requiredMods);
                        names[binding.parts[index].second.id] = InputActions::getControlDisplayName(binding.parts[index].second.path,
                                                                                                    binding.parts[index].second.requiredMods);
                    }
                    names[binding.id] = name;
                } else {
                    names[binding.id] = InputActions::getControlDisplayName(binding.path, binding.requiredMods);
                }
            }
        }
    }
    return {{"asset", out}, {"display_names", names}};
}

std::string assetSummary(const InputActionAssetDesc& asset) {
    std::string text;
    for (const InputActionMapDesc& map : asset.maps) {
        text += "\n" + map.name + " (priority " + std::to_string(map.priority) + (map.enabled ? "" : ", disabled") +
                (map.exclusive ? ", exclusive" : "") + "):";
        for (const InputActionDesc& action : map.actions) {
            text += " " + action.name + "[" + inputActionTypeName(action.type) + ": ";
            for (size_t b = 0; b < action.bindings.size(); b++) {
                const InputBindingDesc& binding = action.bindings[b];
                if (b > 0) text += ", ";
                text += binding.isComposite() ? std::string(inputCompositeName(binding.composite)) : binding.path;
                text += " #" + binding.id;
            }
            text += "]";
        }
    }
    return text;
}

void getInputActionsTool(const CallPtr& call) {
    fs::path file;
    std::string text;
    InputActionAssetDesc asset;
    bool exists = false;
    if (!readInputActions(call, file, false, text, asset, exists)) return;
    Json data = assetJson(asset);
    std::error_code ec;
    const bool playing = InputActions::isLoaded() &&
                         fs::equivalent(FileData::getSystemPath(InputActions::getAssetPath()), file, ec);
    data["loaded_in_play"] = playing;
    call->ok("Input actions " + call->args().value("path", std::string()) + ":" + assetSummary(asset), data);
}

void editInputActionsTool(const CallPtr& call) {
    const Json& args = call->args();
    if (!args.contains("ops") || !args["ops"].is_array() || args["ops"].empty()) {
        call->fail("ops is a non-empty list of {op, ...}: add_map, add_input_manager, add_action, add_binding, add_composite, "
                   "add_part, set, remove, rename, move, duplicate.");
        return;
    }
    fs::path file;
    std::string before;
    InputActionAssetDesc asset;
    bool exists = false;
    if (!readInputActions(call, file, true, before, asset, exists)) return;
    std::vector<std::string> names;
    size_t index = 0;
    try {
        for (const Json& op : args["ops"]) {
            names.push_back(applyInputActionsOp(asset, InputJson::parse(op.dump())));
            index++;
        }
    } catch (const std::exception& e) {
        call->fail("ops[" + std::to_string(index) + "]: " + e.what() + " (nothing was changed).");
        return;
    }
    const std::string undoName = names.size() == 1 ? names.front() : "Edit Input Actions (" + std::to_string(names.size()) + " Changes)";
    Json data = assetJson(asset);
    data["applied"] = names.size();
    data["undo_name"] = undoName;
    data["created"] = !exists;
    if (args.value("dry_run", false)) {
        data["dry_run"] = true;
        call->ok("Dry run: " + std::to_string(names.size()) + " op(s) apply; nothing was written." + assetSummary(asset), data);
        return;
    }
    const std::string after = asset.save();
    Project* project = call->ctx().project();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    project->getProjectCommandHistory()->addCommandNoMerge(new InputActionsEditCmd(file, before, after, undoName, !exists));
    call->ok(undoName + " (" + std::to_string(names.size()) + " op(s))." + assetSummary(asset), data);
}

void getInputStateTool(const CallPtr& call) {
    const Json& args = call->args();
    std::set<std::string> fields;
    if (args.contains("fields") && args["fields"].is_array()) {
        for (const Json& field : args["fields"]) {
            if (field.is_string()) fields.insert(field.get<std::string>());
        }
    }
    const bool all = fields.empty();

    Json data = Json::object();
    data["game_frame"] = Engine::getGameFrames();
    std::string text = "Input of game frame " + std::to_string(Engine::getGameFrames());
    if (all || fields.count("raw")) {
        data["raw"] = rawJson();
        const Json& keys = data["raw"]["keys"];
        const Json& mouse = data["raw"]["mouse"];
        text += "\nkeys held " + keys["held"].dump() + " down " + keys["down"].dump() + " up " + keys["up"].dump();
        text += "\nmouse " + mouse["position"].dump() + " delta " + mouse["delta"].dump() + " scroll " +
                mouse["scroll_delta"].dump() + " held " + mouse["held"].dump() + " down " + mouse["down"].dump() +
                " up " + mouse["up"].dump() + "; pointer " + data["raw"]["pointer_lock"].get<std::string>();
    }
    if (all || fields.count("touches")) {
        data["touches"] = touchesJson();
        if (!data["touches"].empty()) text += "\ntouches " + data["touches"].dump();
    }
    if (all || fields.count("gamepads")) {
        data["gamepads"] = gamepadsJson();
        if (!data["gamepads"].empty()) text += "\ngamepads " + data["gamepads"].dump();
    }
    if (all || fields.count("actions")) {
        data["actions"] = actionsStateJson();
        if (InputActions::isLoaded()) text += "\nactions " + data["actions"]["maps"].dump();
        else text += "\nno Input Actions asset loaded";
    }
    if (text.size() > 8000) text = text.substr(0, 8000) + " ... (see structuredContent)";
    call->ok(text, data);
}

// sample_water's report: the Water Excluders (hulls), the Water Wakes, the wake map and which water
// draws take the excluder and wake variants ('Wex', 'Wwk')
std::string waterInteractionReport(SceneProject* scene, WaterInteractionSystem* interaction, Json& result) {
    Scene* s = scene->scene;
    auto bounds = [](const AABB& box) {
        return box.isNull() ? Json() : Json{{"min", Json::array({box.getMinimum().x, box.getMinimum().y, box.getMinimum().z})},
                                            {"max", Json::array({box.getMaximum().x, box.getMaximum().y, box.getMaximum().z})}};
    };
    static const char* const kShapeNames[] = {"box", "sphere", "convex_hull"};
    std::string text;
    Json excluders = Json::array();
    auto excluderArray = s->getComponentArray<WaterExcluderComponent>();
    for (size_t i = 0; i < excluderArray->size(); i++) {
        const WaterExcluderComponent& component = excluderArray->getComponentFromIndex(i);
        const Entity entity = excluderArray->getEntity(i);
        Json item = {{"entity", McpRefs::entityJson(scene, entity)}, {"shape", kShapeNames[(int)component.shape]}};
        bool active = false;
        AABB world;
        if (interaction) {
            for (const WaterInteractionSystem::Excluder& excluder : interaction->getExcluders()) {
                if (excluder.entity == entity) {
                    active = true;
                    world = excluder.worldBounds;
                }
            }
        }
        item["active"] = active;
        item["world_bounds"] = bounds(world);
        if (component.shape == WaterExcluderShape::CONVEX_HULL) {
            const WaterInteractionSystem::HullInfo hull = interaction ? interaction->getHullInfo(entity) : WaterInteractionSystem::HullInfo();
            item["hull_status"] = getWaterHullStatusName(hull.status);
            item["hull_faces"] = hull.faces;
        }
        excluders.push_back(item);
        text += "excluder " + s->getEntityName(entity) + (active ? ": active" : ": inactive") +
                (component.shape == WaterExcluderShape::CONVEX_HULL ? ", hull " + item["hull_status"].get<std::string>() : std::string()) + "\n";
    }
    Json wakes = Json::array();
    auto wakeArray = s->getComponentArray<WaterWakeComponent>();
    for (size_t i = 0; i < wakeArray->size(); i++) {
        const Entity entity = wakeArray->getEntity(i);
        const WaterInteractionSystem::EmitterInfo info = interaction ? interaction->getEmitterInfo(entity) : WaterInteractionSystem::EmitterInfo();
        wakes.push_back({{"entity", McpRefs::entityJson(scene, entity)}, {"emitting", info.emitting}, {"speed", info.speed},
                         {"water", info.water != NULL_ENTITY ? McpRefs::entityJson(scene, info.water) : Json()}});
        char line[160];
        std::snprintf(line, sizeof(line), "wake %s: %s, %.2f m/s\n", s->getEntityName(entity).c_str(), info.emitting ? "emitting" : "not on water", info.speed);
        text += line;
    }
    const WaterInteractionSystem::WakeMapInfo map = interaction ? interaction->getWakeMapInfo() : WaterInteractionSystem::WakeMapInfo();
    Json waters = Json::array();
    auto meshes = s->getComponentArray<MeshComponent>();
    for (size_t i = 0; i < meshes->size(); i++) {
        const MeshComponent& mesh = meshes->getComponentFromIndex(i);
        const Entity entity = meshes->getEntity(i);
        bool water = false;
        uint64_t properties = 0;
        for (unsigned int sub = 0; sub < mesh.numSubmeshes; sub++) {
            if (Water::isWaterShader(mesh.submeshes[sub].material.shader)) {
                water = true;
                properties |= mesh.submeshes[sub].shaderProperties;
            }
        }
        if (!water)
            continue;
        waters.push_back({{"entity", McpRefs::entityJson(scene, entity)}, {"exclusion", (properties & ShaderPool::getWaterExclusionProperty()) != 0},
                          {"wake", (properties & ShaderPool::getWaterWakeProperty()) != 0}});
    }
    result["excluders"] = excluders;
    result["wakes"] = wakes;
    result["wake_map"] = {{"active", map.active}, {"size", map.size}, {"area", map.area}, {"corner", Json::array({map.corner.x, map.corner.y})}};
    result["waters"] = waters;
    text += std::string("wake map: ") + (map.active ? "active" : "idle") + "\n";
    return text;
}

// sample_water: what the water is at points (Water.sample), the running scene while playing
void sampleWaterTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* scene = nullptr;
    if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE && !args.contains("scene")) {
        scene = w3::resolveRuntimeScene(project, Json(), error);
    } else {
        scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    }
    if (!scene) {
        call->fail(error);
        return;
    }
    if (!scene->scene) {
        call->fail(McpRefs::notOpenError(project, *scene));
        return;
    }
    const bool report = args.contains("report") && args["report"].is_boolean() && args["report"].get<bool>();
    if (args.contains("points") ? (!args["points"].is_array() || args["points"].empty()) : !report) {
        call->fail("points: a list of [x, y, z] world points (or report: true alone).");
        return;
    }
    auto vec = [](const Vector3& v) { return Json::array({v.x, v.y, v.z}); };
    Json samples = Json::array();
    std::string text;
    std::shared_ptr<WaterInteractionSystem> interaction = scene->scene->findSystem<WaterInteractionSystem>();
    const bool wakeMap = interaction && interaction->isWakeActive();
    const Json points = args.contains("points") ? args["points"] : Json::array();
    for (const Json& point : points) {
        if (!point.is_array() || point.size() != 3 || !point[0].is_number() || !point[1].is_number() || !point[2].is_number()) {
            call->fail("points: each one [x, y, z] numbers.");
            return;
        }
        const Vector3 p(point[0].get<float>(), point[1].get<float>(), point[2].get<float>());
        const WaterSample sample = Water::sample(scene->scene, p);
        Json item = {{"point", vec(p)}, {"in_water", sample.inWater}};
        if (sample.hasWater()) {
            item["surface_height"] = sample.surfaceHeight;
            item["normal"] = vec(sample.normal);
            item["flow_velocity"] = vec(sample.flowVelocity);
            item["volume"] = McpRefs::entityJson(scene, sample.volume);
            // the Water Excluder keeping it dry, the Water Wakes' map there (null: no wake running)
            item["excluded_by"] = sample.isExcluded() ? McpRefs::entityJson(scene, sample.excluder) : Json();
            item["wake"] = wakeMap ? Json{{"foam", sample.wakeFoam}, {"height", sample.wakeHeight}} : Json();
        } else {
            item["surface_height"] = nullptr;
        }
        samples.push_back(item);
        char line[200];
        if (sample.isExcluded())
            std::snprintf(line, sizeof(line), "(%g, %g, %g): dry (Water Excluder %s), surface %.6g\n", p.x, p.y, p.z,
                          scene->scene->getEntityName(sample.excluder).c_str(), sample.surfaceHeight);
        else if (sample.hasWater())
            std::snprintf(line, sizeof(line), "(%g, %g, %g): %s, surface %.6g\n", p.x, p.y, p.z, sample.inWater ? "in water" : "above", sample.surfaceHeight);
        else
            std::snprintf(line, sizeof(line), "(%g, %g, %g): no water\n", p.x, p.y, p.z);
        text += line;
    }
    Json result{{"scene_id", scene->id}, {"water_time", Water::getTime(scene->scene)}, {"shader_time", Engine::getShaderTime()},
                {"samples", samples}};
    if (report)
        text += waterInteractionReport(scene, interaction.get(), result);
    // water_time: the clock the waves run on (the scene's, Water.getTime); shader_time: the wall clock
    call->ok(text, result);
}

} // namespace

void registerGameplayTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "get_input_state";
        tool.title = "Get Input State";
        tool.description =
            "What the game reads from Input at the last frame that ran: levels (held), the frame's edges "
            "(down/up: getKeyDown/getKeyUp), mouse delta and scroll delta, pointer lock, touches with "
            "phases, gamepads, and the Input Actions maps (priority, enabled, blocked) with each action's value, "
            "phase and consumer. auto_pause + send_input frames_after 0: the frame the input landed in.";
        tool.inputSchema = object({
            {"fields", array(stringEnum("", {"raw", "touches", "gamepads", "actions"}), "Default: all.")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getInputStateTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "get_input_actions";
        tool.title = "Get Input Actions";
        tool.description =
            "An .inputactions asset (project-relative path): its maps, actions and bindings as the file has them (ids, "
            "paths, composites, groups) plus display_names by binding id, and whether Play runs with it.";
        tool.inputSchema = object({{"path", string("e.g. assets/input/player.inputactions")}}, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getInputActionsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "edit_input_actions";
        tool.title = "Edit Input Actions";
        tool.description =
            "Apply ops to an .inputactions asset as ONE undo step (creates the file if missing; live in Play): "
            "add_map {name, priority?, enabled?, consume?, exclusive?, ignore_when_ui_consumes?} | add_action {map, "
            "name, type BUTTON|AXIS|VECTOR2} | add_binding {action Map/Action, path keyboard/space, gamepad/a, "
            "gamepad/left_stick, mouse/delta, touch/primary/press, virtual/<name>..., groups?, required_mods?} | "
            "add_composite {action, composite axis1d|dpad2d, parts {up: path...}} | add_part {binding, part, path} | "
            "add_input_manager (Unity's 11 axes) | set {map|action|binding id, values} | remove | rename {map|action, "
            "name} | move {..., index} | duplicate. A bad op changes nothing.";
        tool.inputSchema = object({
            {"path", string("")},
            {"ops", Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 200}, {"items", anyObject("")}}},
            {"dry_run", withDefault(boolean(""), false)}
        }, {"path", "ops"});
        tool.handler = editInputActionsTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "sample_water";
        tool.title = "Sample Water";
        tool.description =
            "What the water is at world points (Lua Water.sample), at this frame's water_time: in_water, the surface "
            "height over the point with the waves the shader draws (null: no water there), its normal, the water's "
            "velocity (flow + the surface rising), the Water Volume (or library:water mesh), the Water Excluder keeping "
            "it dry and the Water Wakes there. report: the excluders' hulls, the wakes and which waters draw them. The "
            "running scene while playing.";
        tool.inputSchema = object({
            {"points", Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 256},
                            {"items", Json{{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", Json{{"type", "number"}}}}}}},
            {"report", boolean("")},
            {"scene", sceneRef()}
        }, {});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = sampleWaterTool;
        registry.addTool(std::move(tool));
    }
}

}
