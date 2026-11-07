// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Particle effects for agents (docs/specs/S20-particles.md R30): .particles assets and the
// inline effects of Particle Systems as JSON, edited by RFC 7386 merge patches on the keyed
// form ({"modules": {"emission": {"rateOverTime": 20}}}) through the same undoable path as the
// Inspector (ParticleEffectEdit), playback control in Play and in Edit mode (the Scene view's
// Particle Effect preview), live statistics, a capture at a seeked time and the migration report.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/ParticleShared.h"
#include "mcp/tools/PlayViewCommon.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ParticleEffectAssetCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ParticleSystemComponent.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleModules.h"
#include "subsystem/ParticleSubsystem.h"
#include "util/AssetDatabase.h"
#include "util/ParticleEffectEdit.h"
#include "util/ParticleMigrationReport.h"
#include "util/Util.h"
#include "window/ParticleEffectOverlay.h"
#include "window/SceneWindow.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
using particles::Diagnostic;
using particles::ParticleEffectCodec;
using particles::ParticleEffectDocument;
using particles::ParticleModules;

using Ordered = nlohmann::ordered_json;

Json toMcp(const Ordered& value) {
    return Json::parse(value.dump());
}

Ordered toOrdered(const Json& value) {
    return Ordered::parse(value.dump());
}

Json diagnosticsJson(const std::vector<Diagnostic>& diagnostics) {
    Json out = Json::array();
    for (const Diagnostic& d : diagnostics) {
        out.push_back({{"severity", d.severity == Diagnostic::Severity::Error ? "error" : "warning"}, {"path", d.path}, {"message", d.message}});
    }
    return out;
}

std::string firstError(const std::vector<Diagnostic>& diagnostics) {
    for (const Diagnostic& d : diagnostics) {
        if (d.severity == Diagnostic::Severity::Error) return d.path + ": " + d.message;
    }
    return diagnostics.empty() ? std::string() : diagnostics.front().path + ": " + diagnostics.front().message;
}

// What a tool works on: an asset (path) or a Particle System (entity, in a scene)
struct EffectTarget {
    Project* project = nullptr;
    SceneProject* scene = nullptr;
    Entity entity = NULL_ENTITY;
    std::string asset;      // assets-relative; the entity's asset when it has one
    bool inlineEffect = false;
};

// An assets-relative .particles path from a project- or assets-relative argument
bool resolveEffectPath(Project* project, const std::string& value, bool mustExist, std::string& out, std::string& error) {
    if (value.empty()) {
        error = "path is required: a .particles file (project-relative, e.g. \"assets/fx/fire.particles\").";
        return false;
    }
    if (!Util::isParticleEffectFile(value)) {
        error = "\"" + value + "\" is not a .particles file.";
        return false;
    }
    fs::path absolute;
    if (!McpRefs::resolveProjectPath(project, value, absolute, error, false)) return false;
    if (!project->isInsideAssetsPath(absolute.string())) {
        // an assets-relative path ("fx/fire.particles") as the components store it
        absolute = project->resolveAssetPath(value);
    }
    if (!project->isInsideAssetsPath(absolute.string())) {
        error = "\"" + value + "\" is outside the assets folder.";
        return false;
    }
    if (mustExist && !fs::exists(absolute)) {
        error = "No effect at \"" + value + "\" (create_particle_effect makes one).";
        return false;
    }
    out = project->normalizeToAssetsRelative(absolute).generic_string();
    return true;
}

// path, or entity (with scene); `editing` refuses a playing scene's GameObjects
bool resolveEffectTarget(const CallPtr& call, EffectTarget& target, bool editing) {
    const Json& args = call->args();
    target.project = call->ctx().project();
    std::string error;
    if (args.contains("path") && !args["path"].is_null()) {
        if (!resolveEffectPath(target.project, args.value("path", std::string()), true, target.asset, error)) {
            call->fail(error);
            return false;
        }
        return true;
    }
    if (!args.contains("entity")) {
        call->fail("Give path (a .particles asset) or entity (a GameObject with a Particle System).");
        return false;
    }
    target.scene = McpRefs::resolveScene(target.project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!target.scene) {
        call->fail(error);
        return false;
    }
    if (!target.scene->scene) {
        call->fail(McpRefs::notOpenError(target.project, *target.scene));
        return false;
    }
    target.entity = McpRefs::resolveEntity(target.scene, args["entity"], error);
    if (target.entity == NULL_ENTITY) {
        call->fail(error);
        return false;
    }
    ParticleSystemComponent* particles = target.scene->scene->findComponent<ParticleSystemComponent>(target.entity);
    if (!particles) {
        call->fail("\"" + target.scene->scene->getEntityName(target.entity) + "\" has no Particle System (create_particles adds one).");
        return false;
    }
    if (editing && target.scene->playState != ScenePlayState::STOPPED) {
        call->fail("Scene " + std::to_string(target.scene->id) + " is playing; stop play mode before editing its effects.");
        return false;
    }
    target.inlineEffect = particles->effect.empty();
    if (!target.inlineEffect) target.asset = particles->effect.path;
    return true;
}

bool readTarget(const EffectTarget& target, ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics) {
    if (target.entity != NULL_ENTITY) {
        return ParticleEffectEdit::read(target.scene->scene->getComponent<ParticleSystemComponent>(target.entity), document, &diagnostics);
    }
    return ParticleEffectEdit::readAsset(target.asset, document, &diagnostics);
}

Json targetJson(const EffectTarget& target) {
    Json out = Json::object();
    if (target.entity != NULL_ENTITY) out["entity"] = McpRefs::entityJson(target.scene, target.entity);
    out["source"] = target.inlineEffect ? "inline" : "asset";
    if (!target.asset.empty()) out["path"] = target.asset;
    return out;
}

Json moduleKeys(const ParticleEffectDocument& document) {
    Json out = Json::array();
    for (const particles::ModuleDesc& m : ParticleModules::all()) {
        if (m.isPresent(document)) out.push_back(m.key);
    }
    return out;
}

// Applies `patch` to the target's effect as one undo step (the scene's for a GameObject, the
// project's for an asset); the diagnostics of a refused patch come back in `diagnostics`
bool editTarget(const CallPtr& call, const EffectTarget& target, const Ordered& patch, const std::string& undoName,
                std::vector<Diagnostic>& diagnostics, bool& changed) {
    changed = false;
    bool refused = false;
    auto edit = [&](ParticleEffectDocument& document) {
        if (!ParticleEffectCodec::applyMergePatch(document, patch, diagnostics)) {
            refused = true;
            return false;
        }
        return true;
    };
    Command* command = target.entity != NULL_ENTITY ? ParticleEffectEdit::build(target.project, target.scene->id, {target.entity}, edit)
                                                    : ParticleEffectEdit::buildAsset(target.project, target.asset, edit);
    if (refused) {
        delete command;
        call->fail("The patch was refused (" + firstError(diagnostics) + "); nothing changed.", Json{{"diagnostics", diagnosticsJson(diagnostics)}});
        return false;
    }
    if (!command) return true;
    std::string error;
    const bool ran = target.entity != NULL_ENTITY ? runSceneCommand(target.scene->id, command, error, undoName)
                                                  : runProjectCommand(target.project, command, error, undoName);
    if (!ran) {
        call->fail(error);
        return false;
    }
    changed = true;
    return true;
}

// `applied`: what the patch's own reading said; the effect as it is now can no longer say that
void replyEffect(const CallPtr& call, const EffectTarget& target, const std::string& text, bool changed, const std::vector<Diagnostic>& applied = {}) {
    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    readTarget(target, document, diagnostics);
    for (const Diagnostic& d : applied) {
        const auto same = [&](const Diagnostic& other) { return other.severity == d.severity && other.path == d.path && other.message == d.message; };
        if (std::none_of(diagnostics.begin(), diagnostics.end(), same)) diagnostics.push_back(d);
    }
    Json data = targetJson(target);
    data["changed"] = changed;
    data["modules"] = moduleKeys(document);
    data["diagnostics"] = diagnosticsJson(diagnostics);
    call->ok(diagnostics.empty() ? text : text + " " + std::to_string(diagnostics.size()) + " diagnostic(s), first: " + firstError(diagnostics), data);
}

// ------------------------------------------------------------------ assets

void createEffectTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string path, error;
    if (!resolveEffectPath(project, args.value("path", std::string()), false, path, error)) {
        call->fail(error);
        return;
    }
    const fs::path absolute = project->resolveAssetPath(path);
    if (fs::exists(absolute) && !args.value("overwrite", false)) {
        call->fail("\"" + path + "\" exists (overwrite: true replaces it; edit_particle_effect edits it).");
        return;
    }

    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    if (args.contains("effect") && !args["effect"].is_null()) {
        auto effect = toOrdered(args["effect"]);
        // an agent's effect may leave the schema version out: this editor's
        if (effect.is_object() && !effect.contains("version")) effect["version"] = document.version;
        if (!ParticleEffectCodec::fromJson(effect, document, diagnostics)) {
            call->fail("effect is not a valid effect (" + firstError(diagnostics) + ").", Json{{"diagnostics", diagnosticsJson(diagnostics)}});
            return;
        }
    } else if (args.contains("preset")) {
        const std::string preset = args.value("preset", std::string());
        if (!particlePresetEffect(Json::object(), preset, false, document, error)) {
            call->fail(error);
            return;
        }
    }
    const std::string text = ParticleEffectCodec::write(document);

    Command* command = nullptr;
    if (fs::exists(absolute)) {
        std::string before;
        ParticleEffectDocument current;
        if (ParticleEffectEdit::readAsset(path, current)) before = ParticleEffectCodec::write(current);
        command = new ParticleEffectAssetCmd(project, path, before.empty() ? std::string("{}\n") : before, text);
    } else {
        command = new ParticleEffectAssetCmd(project, path, std::string(), text, Guid::parse(AssetDatabase::generateGuid()));
    }
    if (!runProjectCommand(project, command, error, "Could not write " + path + ".")) {
        call->fail(error);
        return;
    }
    diagnostics.clear();
    ParticleEffectCodec::validate(document, diagnostics);
    const Guid guid = project->getAssetDatabase().getAssetGuid(absolute);
    call->ok("Created the particle effect " + path + " (one undo step of the project).",
             Json{{"path", path}, {"guid", guid.str()}, {"modules", moduleKeys(document)}, {"diagnostics", diagnosticsJson(diagnostics)}});
}

void getEffectTool(const CallPtr& call) {
    EffectTarget target;
    if (!resolveEffectTarget(call, target, false)) return;
    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    if (!readTarget(target, document, diagnostics)) {
        call->fail("The effect asset " + target.asset + " cannot be read (" + firstError(diagnostics) + ").", Json{{"diagnostics", diagnosticsJson(diagnostics)}});
        return;
    }
    Json data = targetJson(target);
    data["effect"] = toMcp(ParticleEffectCodec::toKeyedJson(document));
    data["modules"] = moduleKeys(document);
    data["diagnostics"] = diagnosticsJson(diagnostics);
    std::string text = std::string(target.inlineEffect ? "Inline effect" : "Effect " + target.asset) + ": " +
                       std::to_string(data["modules"].size()) + " modules, " + std::to_string(diagnostics.size()) + " diagnostics. "
                       "The effect is the keyed form edit_particle_effect patches (modules keyed by type, defaults left out).";
    call->ok(text, data);
}

void editEffectTool(const CallPtr& call) {
    const Json& args = call->args();
    if (!args.contains("patch") || !args["patch"].is_object()) {
        call->fail("patch is a JSON merge patch (RFC 7386) of the keyed effect: {\"modules\": {\"emission\": {\"rateOverTime\": 20}}, \"duration\": 3}; "
                   "null removes a key (a module: removes it).");
        return;
    }
    EffectTarget target;
    if (!resolveEffectTarget(call, target, true)) return;
    std::vector<Diagnostic> diagnostics;
    bool changed = false;
    if (!editTarget(call, target, toOrdered(args["patch"]), "Edit Particle Effect", diagnostics, changed)) return;
    replyEffect(call, target, changed ? "Edited the effect (one undo step)." : "The patch changed nothing.", changed, diagnostics);
}

void moduleTool(const CallPtr& call, bool add) {
    const std::string key = call->args().value("module", std::string());
    const particles::ModuleDesc* module = ParticleModules::find(key);
    if (!module) {
        std::string keys;
        for (const particles::ModuleDesc& m : ParticleModules::all()) keys += (keys.empty() ? "" : ", ") + std::string(m.key);
        call->fail("Unknown module \"" + key + "\". Modules: " + keys + ".");
        return;
    }
    if (!add && module->alwaysPresent) {
        call->fail(std::string(module->label) + " cannot be removed (an effect always has it); edit_particle_effect can disable it.");
        return;
    }
    EffectTarget target;
    if (!resolveEffectTarget(call, target, true)) return;
    Ordered patch = Ordered::object();
    patch["modules"] = Ordered::object();
    if (add) {
        patch["modules"][key] = call->args().contains("fields") && call->args()["fields"].is_object() ? toOrdered(call->args()["fields"]) : Ordered::object();
        patch["modules"][key]["enabled"] = true;
    } else {
        patch["modules"][key] = nullptr;
    }
    std::vector<Diagnostic> diagnostics;
    bool changed = false;
    if (!editTarget(call, target, patch, add ? "Add Module" : "Remove Module", diagnostics, changed)) return;
    replyEffect(call, target, std::string(changed ? (add ? "Added " : "Removed ") : "Nothing changed: ") + module->label + ".", changed, diagnostics);
}

void addModuleTool(const CallPtr& call) { moduleTool(call, true); }
void removeModuleTool(const CallPtr& call) { moduleTool(call, false); }

void assignTool(const CallPtr& call) {
    const Json& args = call->args();
    EffectTarget target;
    if (!args.contains("entity")) {
        call->fail("entity is required.");
        return;
    }
    if (!resolveEffectTarget(call, target, true)) return;
    Project* project = target.project;
    const std::string value = args.value("effect", std::string());
    AssetRef ref;
    std::string error;
    if (!value.empty() && value != "inline") {
        std::string path;
        if (!resolveEffectPath(project, value, true, path, error)) {
            call->fail(error);
            return;
        }
        ref = project->makeAssetRef(path, AssetPathBase::Assets);
    }
    MultiPropertyCmd* multi = new MultiPropertyCmd();
    // keeping what plays: an asset dropped for the inline effect copies the asset's
    if (ref.empty() && !target.inlineEffect && args.value("copy_to_inline", true)) {
        ParticleEffectDocument document;
        if (ParticleEffectEdit::readAsset(target.asset, document)) {
            multi->addPropertyCmd<ParticleEffectDocument>(project, target.scene->id, target.entity, ComponentType::ParticleSystemComponent, "inlineEffect", document);
        }
    }
    multi->addPropertyCmd<AssetRef>(project, target.scene->id, target.entity, ComponentType::ParticleSystemComponent, "effect", ref);
    if (!runSceneCommand(target.scene->id, multi, error, "Could not assign the effect.")) {
        call->fail(error);
        return;
    }
    EffectTarget after;
    resolveEffectTarget(call, after, false);
    replyEffect(call, after, ref.empty() ? "The Particle System plays its inline effect." : "The Particle System plays " + ref.path + ".", true);
}

// ------------------------------------------------------------------ playback

Json statsJson(ParticleSubsystem& particles, Entity entity) {
    const ParticleSystemStats s = particles.getStats(entity);
    // as text: a 64-bit number does not survive every JSON reader
    char stateHash[17];
    std::snprintf(stateHash, sizeof(stateHash), "%016llx", static_cast<unsigned long long>(particles.getStateHash(entity)));
    return {{"valid", s.valid}, {"seed", s.seed}, {"state_hash", stateHash}, {"particle_count", s.live}, {"capacity", s.capacity}, {"budget", s.budget}, {"emission_scale", s.emissionScale},
            {"ticks", s.ticks}, {"tick_count", s.tickCount}, {"time", s.time}, {"dropped_time", s.droppedTime},
            {"simulate_us", s.simulateMicros}, {"render_us", s.renderMicros}, {"culled", s.culled}, {"playing", s.playing},
            {"paused", s.paused}, {"emitting", s.emitting}, {"sorted_views", s.sortedViews}, {"drawn_views", s.drawnViews},
            {"rays_last_tick", s.raysLastTick}, {"bytes_per_particle", s.bytesPerParticle}};
}

void replyControl(const CallPtr& call, const EffectTarget& target, ParticleSubsystem& particles, const std::string& action, bool editMode) {
    Json data = targetJson(target);
    data["action"] = action;
    data["edit_mode"] = editMode;
    data["stats"] = statsJson(particles, target.entity);
    call->ok(action + ": " + std::to_string(data["stats"].value("particle_count", 0)) + " particles at " +
             std::to_string(data["stats"].value("time", 0.0)) + " s" + (editMode ? " (Edit mode: the Scene view's preview runs it)." : "."), data);
}

ParticleEffectOverlay* overlayOf(const CallPtr& call) {
    SceneWindow* window = call->ctx().sceneWindow();
    return window ? &window->getParticleOverlay() : nullptr;
}

void controlTool(const CallPtr& call) {
    const Json& args = call->args();
    EffectTarget target;
    if (!args.contains("entity")) {
        call->fail("entity is required.");
        return;
    }
    if (!resolveEffectTarget(call, target, false)) return;
    const std::string action = args.value("action", std::string());
    Scene* scene = target.scene->scene;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    const Entity entity = target.entity;
    // Edit mode: the Scene view's preview runs the system (it steps no systems itself)
    const bool editMode = target.scene->playState == ScenePlayState::STOPPED;
    ParticleEffectOverlay* overlay = editMode ? overlayOf(call) : nullptr;
    if (overlay) overlay->addTarget(target.scene->id, entity);

    if (action == "play") {
        particles->play(entity);
        if (overlay) overlay->play(target.scene->id);
    } else if (action == "pause") {
        particles->pause(entity);
        if (overlay) overlay->pause(target.scene->id);
    } else if (action == "stop" || action == "clear") {
        particles->stop(entity, action == "clear" || args.value("clear", false));
        if (overlay && action == "clear") overlay->pause(target.scene->id);
    } else if (action == "restart") {
        particles->restart(entity);
        if (overlay) overlay->play(target.scene->id);
    } else if (action == "emit") {
        const int count = args.value("count", 1);
        if (count < 1 || count > 100000) {
            call->fail("count is 1 to 100000.");
            return;
        }
        particles->emit(entity, (uint32_t)count);
    } else if (action == "seek") {
        if (!args.contains("time") || !args["time"].is_number() || args["time"].get<double>() < 0.0) {
            call->fail("time (seconds, >= 0) is required for seek.");
            return;
        }
        const double time = args["time"].get<double>();
        if (overlay) {
            // the preview's scrub: from the nearest checkpoint, over frames, then held there
            overlay->seek(target.project, target.scene, time);
            const uint32_t sceneId = target.scene->id;
            call->waitUntil([overlay, sceneId]() { return !overlay->isSeeking(sceneId); }, 60000, [call, target, particles, action](bool timedOut) {
                if (timedOut) {
                    call->fail("The seek did not finish within 60 s.");
                    return;
                }
                replyControl(call, target, *particles, action, true);
            });
            return;
        }
        particles->seek(entity, time);
    } else if (action == "step") {
        const int ticks = args.value("ticks", 1);
        if (ticks < 1 || ticks > 100000) {
            call->fail("ticks is 1 to 100000.");
            return;
        }
        particles->step(entity, (uint32_t)ticks);
        if (overlay) overlay->pause(target.scene->id);
    } else if (action == "trigger") {
        const std::string name = args.value("name", std::string());
        if (name.empty()) {
            call->fail("name (the trigger's, as the effect's triggers or Trigger sub-emitters name it) is required for trigger.");
            return;
        }
        particles->sendTrigger(entity, name);
        // what the trigger left playing (a play, or sub-emitters fired from a stopped system) runs in the preview
        if (overlay && particles->isPlaying(entity)) overlay->play(target.scene->id);
    } else {
        call->fail("action must be play, pause, stop, clear, restart, emit, seek, step or trigger.");
        return;
    }
    target.scene->needUpdateRender = true;
    replyControl(call, target, *particles, action, editMode);
}

void statsTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sceneProject) {
        call->fail(error);
        return;
    }
    if (!sceneProject->scene) {
        call->fail(McpRefs::notOpenError(project, *sceneProject));
        return;
    }
    Scene* scene = sceneProject->scene;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    Json systems = Json::array();
    if (args.contains("entity")) {
        const Entity entity = McpRefs::resolveEntity(sceneProject, args["entity"], error);
        if (entity == NULL_ENTITY) {
            call->fail(error);
            return;
        }
        if (!scene->findComponent<ParticleSystemComponent>(entity)) {
            call->fail("\"" + scene->getEntityName(entity) + "\" has no Particle System.");
            return;
        }
        Json item = statsJson(*particles, entity);
        item["entity"] = McpRefs::entityJson(sceneProject, entity);
        systems.push_back(item);
    } else {
        auto list = scene->getComponentArray<ParticleSystemComponent>();
        for (size_t i = 0; i < list->size(); i++) {
            Json item = statsJson(*particles, list->getEntity(i));
            item["entity"] = McpRefs::entityJson(sceneProject, list->getEntity(i));
            systems.push_back(item);
        }
    }
    const particles::WorldStats& world = particles->getWorldStats();
    Json data = {{"systems", systems},
                 {"world", {{"emitters", world.emitters}, {"particle_count", world.live}, {"budget", world.budget}, {"ticks", world.ticks},
                            {"units", world.units}, {"simulate_us", world.simulateMicros}, {"over_budget", world.overBudget}}}};
    call->ok(std::to_string(systems.size()) + " particle system(s), " + std::to_string(world.live) + " live particles in the world.", data);
}

void captureTool(const CallPtr& call) {
    const Json& args = call->args();
    EffectTarget target;
    if (!args.contains("entity")) {
        call->fail("entity is required.");
        return;
    }
    if (!resolveEffectTarget(call, target, false)) return;
    const double time = args.value("time", 0.0);
    if (time < 0.0) {
        call->fail("time is seconds >= 0.");
        return;
    }
    const std::string view = args.value("camera", std::string("scene"));
    if (view != "scene" && view != "game") {
        call->fail("camera must be scene (the Scene view's camera) or game (the scene's game camera).");
        return;
    }
    std::shared_ptr<ParticleSubsystem> particles = target.scene->scene->getSystem<ParticleSubsystem>();
    // the state at `time`, held (the Edit-mode preview pauses on it)
    particles->seek(target.entity, time);
    if (target.scene->playState == ScenePlayState::STOPPED) {
        if (ParticleEffectOverlay* overlay = overlayOf(call)) {
            overlay->addTarget(target.scene->id, target.entity);
            overlay->pause(target.scene->id);
        }
    } else {
        particles->pause(target.entity);
    }
    target.scene->needUpdateRender = true;

    w3::CaptureRequest request;
    request.view = view;
    request.scene = target.scene->id;
    request.maxWidth = std::clamp(args.value("max_width", 768), 0, 4096);
    request.savePath = args.value("save_path", std::string());
    const Json stats = statsJson(*particles, target.entity);
    w3::captureView(call, request, [call, time, stats](w3::CaptureResult result) {
        if (!result.ok) {
            call->fail(result.error);
            return;
        }
        Json data = result.info;
        data["time"] = time;
        data["stats"] = stats;
        ToolResult reply = ToolResult::success("The effect at " + std::to_string(time) + " s (" + std::to_string(stats.value("particle_count", 0)) +
                                               " particles, seed " + std::to_string(stats.value("seed", 0u)) + ", state_hash " +
                                               stats.value("state_hash", std::string()) + "), " + std::to_string(result.info.value("width", 0)) + "x" +
                                               std::to_string(result.info.value("height", 0)) + ".", data);
        reply.images.push_back(std::move(result.image));
        call->reply(std::move(reply));
    });
}

void migrationReportTool(const CallPtr& call) {
    const std::vector<ParticleMigrationReport::Entry> entries = ParticleMigrationReport::entries();
    Json documents = Json::object();
    for (const ParticleMigrationReport::Entry& entry : entries) documents[entry.document].push_back(entry.line);
    Json list = Json::array();
    for (auto it = documents.begin(); it != documents.end(); ++it) list.push_back({{"document", it.key()}, {"conversions", it.value()}});
    call->ok(entries.empty() ? "No old particle system was converted since the project opened."
                             : std::to_string(entries.size()) + " old particle system(s) converted in " + std::to_string(list.size()) +
                                   " document(s); saving writes them in the new format.",
             Json{{"count", entries.size()}, {"documents", list}});
}

} // namespace

void registerParticleTools(McpRegistry& registry) {
    using namespace schema;
    const Json target = {{"path", assetPath("A .particles asset.")}, {"entity", entityRef("A GameObject with a Particle System (its asset, else its inline effect).")},
                         {"scene", sceneRef()}};
    auto withTarget = [&](Json extra) {
        Json properties = target;
        for (auto it = extra.begin(); it != extra.end(); ++it) properties[it.key()] = it.value();
        return properties;
    };
    auto add = [&](const char* name, const char* title, const std::string& description, Json schemaJson, std::function<void(CallPtr)> handler, bool readOnly) {
        ToolDef tool;
        tool.name = name;
        tool.title = title;
        tool.description = description;
        tool.inputSchema = std::move(schemaJson);
        tool.annotations.readOnly = readOnly;
        tool.handler = handler;
        registry.addTool(std::move(tool));
    };

    add("create_particle_effect", "Create Particle Effect",
        "A new .particles asset from a create_particles preset or an effect JSON (the keyed form get_particle_effect returns), "
        "one undo step of the project. set_particle_effect plays it on a Particle System.",
        object({{"path", assetPath("The new file (.particles).")},
                {"preset", stringEnum("", particlePresetNames())},
                {"effect", anyObject("The whole effect (modules keyed by type).")},
                {"overwrite", withDefault(boolean(""), false)}}, {"path"}),
        createEffectTool, false);
    add("get_particle_effect", "Get Particle Effect",
        "An effect as JSON (keyed: {duration, looping, ..., modules: {main, emission, shape, ...}}, defaults left out), the "
        "modules present and its validation diagnostics [{severity, path, message}].",
        object(withTarget(Json::object()), std::vector<std::string>{}), getEffectTool, true);
    add("edit_particle_effect", "Edit Particle Effect",
        "Apply a JSON merge patch (RFC 7386) to an effect's keyed form as one undo step: {\"modules\": {\"emission\": "
        "{\"rateOverTime\": 20}}}; null removes a key (a module: removes it). A value is a number, or {mode: twoConstants, "
        "constant, constantMax}, {mode: curve, curve: [[t, v], ...], multiplier}; colours [r, g, b, a] sRGB. An asset's edit "
        "changes every system using it. A patch that leaves the effect invalid is refused with its diagnostics.",
        object(withTarget({{"patch", anyObject("The merge patch.")}}), std::vector<std::string>{"patch"}), editEffectTool, false);
    add("add_particle_module", "Add Particle Module",
        "Add (or re-enable) a module of an effect, with optional fields: emission, shape, startVelocity, velocityOverLifetime, "
        "limitVelocity, forceOverLifetime, colorOverLifetime, colorBySpeed, sizeOverLifetime, sizeBySpeed, rotationOverLifetime, "
        "rotationBySpeed, externalForces, noise, orbit, collision, subEmitters, textureSheetAnimation, trails, customData, setOverAge.",
        object(withTarget({{"module", string("")}, {"fields", anyObject("The module's fields.")}}), std::vector<std::string>{"module"}),
        addModuleTool, false);
    add("remove_particle_module", "Remove Particle Module",
        "Remove a module from an effect (Main and Renderer stay: disable them through edit_particle_effect).",
        object(withTarget({{"module", string("")}}), std::vector<std::string>{"module"}), removeModuleTool, false);
    add("set_particle_effect", "Assign Particle Effect",
        "Make a Particle System play a .particles asset (effect: its path) or its inline effect (effect: \"inline\" or empty; "
        "copy_to_inline keeps the asset's effect as the inline one).",
        object({{"entity", entityRef()}, {"scene", sceneRef()}, {"effect", string("")}, {"copy_to_inline", withDefault(boolean(""), true)}},
               {"entity", "effect"}),
        assignTool, false);
    add("control_particles", "Control Particle System",
        "Play, pause, stop (keeps the particles), clear, restart, emit count particles, seek to time (restart and simulate), "
        "step ticks, or trigger name (the effect's triggers and Trigger sub-emitters). In Edit mode the Scene view's Particle "
        "Effect preview runs the system. Returns its statistics.",
        object({{"entity", entityRef()}, {"scene", sceneRef()},
                {"action", stringEnum("", {"play", "pause", "stop", "clear", "restart", "emit", "seek", "step", "trigger"})},
                {"count", integer("", 1, 100000)}, {"time", number("Seconds.")}, {"ticks", integer("", 1, 100000)}, {"name", string("")}},
               {"entity", "action"}),
        controlTool, false);
    add("get_particle_stats", "Particle Statistics",
        "Live statistics of one Particle System (entity) or every one of the scene: particle_count, capacity, budget share, "
        "ticks, dropped time, simulate/render microseconds, culled, sorted views; and the world's totals.",
        object({{"entity", entityRef()}, {"scene", sceneRef()}}), statsTool, true);
    add("capture_particles", "Capture Particle Effect",
        "Seek a Particle System to time (seconds) and capture the view (camera: scene or game) as an image; the system stays "
        "paused there.",
        object({{"entity", entityRef()}, {"scene", sceneRef()}, {"time", number("Seconds.")},
                {"camera", withDefault(stringEnum("", {"scene", "game"}), "scene")},
                {"max_width", withDefault(integer("", 0, 4096), 768)}, {"save_path", string("Also write the PNG here.")}},
               {"entity", "time"}),
        captureTool, false);
    add("get_particle_migration_report", "Particle Migration Report",
        "The old particle systems the documents read since the project opened were converted from (S20): per document, one "
        "line per system (where its effect went, what was approximated).",
        object({}), migrationReportTool, true);
}

}
