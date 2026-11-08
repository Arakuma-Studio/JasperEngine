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
#include "particles/AnalyticParticles.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleModules.h"
#include "pixelart/lint/LintTypes.h"
#include "pixelart/particles/ParticleCapture.h"
#include "pixelart/particles/ParticleCaptureChecks.h"
#include "pixelart/particles/PixelParticlePresets.h"
#include "pixelart/particles/PixelPresetCreate.h"
#include "pixelart/PixelColor.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "subsystem/RenderSystem.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/AssetDatabase.h"
#include "util/ParticleEffectEdit.h"
#include "util/ParticleMigrationReport.h"
#include "util/Util.h"
#include "window/ParticleEffectOverlay.h"
#include "window/SceneWindow.h"

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
        Json item = {{"severity", d.severity == Diagnostic::Severity::Error ? "error" : "warning"}, {"path", d.path}, {"message", d.message}};
        // a typed finding (S49: analytic_unsupported, analytic_capacity, pixel_art_mesh, ramp_missing)
        if (!d.code.empty()) item["code"] = d.code;
        if (!d.fix.empty()) item["fix"] = d.fix;
        out.push_back(item);
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

// Analytic's rules (S49 R15) beside the codec's: the modules an Analytic effect cannot run are errors
void addAnalyticDiagnostics(const ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics) {
    if (document.simulationMode == particles::SimulationMode::Analytic) particles::AnalyticParticles::validate(document, diagnostics);
}

bool readTarget(const EffectTarget& target, ParticleEffectDocument& document, std::vector<Diagnostic>& diagnostics) {
    const bool read = target.entity != NULL_ENTITY
                          ? ParticleEffectEdit::read(target.scene->scene->getComponent<ParticleSystemComponent>(target.entity), document, &diagnostics)
                          : ParticleEffectEdit::readAsset(target.asset, document, &diagnostics);
    if (read) addAnalyticDiagnostics(document, diagnostics);
    return read;
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
        const size_t first = diagnostics.size();
        addAnalyticDiagnostics(document, diagnostics);
        for (size_t i = first; i < diagnostics.size(); i++) {
            if (diagnostics[i].severity == Diagnostic::Severity::Error) refused = true;
        }
        return !refused;
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

void replyEffect(const CallPtr& call, const EffectTarget& target, const std::string& text, bool changed) {
    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    readTarget(target, document, diagnostics);
    Json data = targetJson(target);
    data["changed"] = changed;
    data["modules"] = moduleKeys(document);
    data["diagnostics"] = diagnosticsJson(diagnostics);
    call->ok(text, data);
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
    std::vector<std::string> children;
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
        const std::vector<std::string>& pixel = pixelart::pixelParticlePresetNames();
        if (std::find(pixel.begin(), pixel.end(), preset) != pixel.end()) {
            // S49: on the project's copy of builtin:pixelfx-effects, its children beside it, scaled by the PPU
            pixelart::ProjectPixelPreset made;
            const std::string directory = fs::path(path).parent_path().generic_string();
            if (!pixelart::prepareProjectPixelPreset(*project, preset, directory, made, error)) {
                call->fail(error);
                return;
            }
            if (!made.writes.empty() && !ProjectFileWrites::writeUndoable(project, made.writes, error)) {
                call->fail(error);
                return;
            }
            document = made.document;
            children = made.children;
        } else if (!particlePresetEffect(Json::object(), preset, false, document, error)) {
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
    addAnalyticDiagnostics(document, diagnostics);
    const Guid guid = project->getAssetDatabase().getAssetGuid(absolute);
    Json data = Json{{"path", path}, {"guid", guid.str()}, {"modules", moduleKeys(document)}, {"diagnostics", diagnosticsJson(diagnostics)}};
    if (!children.empty()) data["children"] = children;
    data["next"] = "set_particle_effect plays it on a Particle System";
    call->ok("Created the particle effect " + path + " (one undo step of the project).", data);
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
    replyEffect(call, target, changed ? "Edited the effect (one undo step)." : "The patch changed nothing.", changed);
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
    replyEffect(call, target, std::string(changed ? (add ? "Added " : "Removed ") : "Nothing changed: ") + module->label + ".", changed);
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
    return {{"valid", s.valid}, {"particle_count", s.live}, {"capacity", s.capacity}, {"budget", s.budget}, {"emission_scale", s.emissionScale},
            {"ticks", s.ticks}, {"tick_count", s.tickCount}, {"time", s.time}, {"dropped_time", s.droppedTime},
            {"simulate_us", s.simulateMicros}, {"render_us", s.renderMicros}, {"culled", s.culled}, {"playing", s.playing},
            {"paused", s.paused}, {"emitting", s.emitting}, {"sorted_views", s.sortedViews}, {"drawn_views", s.drawnViews},
            {"rays_last_tick", s.raysLastTick}, {"bytes_per_particle", s.bytesPerParticle},
            {"simulation_mode", s.analytic ? "analytic" : "simulated"}, {"pixel", s.pixel}, {"analytic_slots", s.analyticSlots}};
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
    } else {
        call->fail("action must be play, pause, stop, clear, restart, emit, seek or step.");
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

// S49 R38: the effect alone at N times, a contact sheet at an integer scale, and its checks
void captureSheetTool(const CallPtr& call, const EffectTarget& target) {
    const Json& args = call->args();
    ParticleEffectDocument document;
    std::vector<Diagnostic> diagnostics;
    if (!readTarget(target, document, diagnostics)) {
        call->fail("The effect cannot be read (" + firstError(diagnostics) + ").", Json{{"diagnostics", diagnosticsJson(diagnostics)}});
        return;
    }
    const int frames = args.value("frames", 1);
    if (frames < 1 || frames > 64) {
        call->fail("frames is 1 to 64.");
        return;
    }
    int scale = 1;
    if (args.contains("scale")) {
        scale = args.value("scale", 1);
    } else if (target.scene && target.scene->scene) {
        // a pixel effect at the game's pixel scale (S41's)
        RenderSystem* render = target.scene->scene->getSystem<RenderSystem>().get();
        if (render && render->getPixelPerfectLayout() && (document.pixelArt.present || document.renderer.renderMode == particles::RenderMode::Pixel))
            scale = std::max(1, render->getPixelPerfectLayout()->scale);
    }
    if (scale < 1 || scale > 8) {
        call->fail("scale is a whole number from 1 to 8.");
        return;
    }
    jasperengine::pixelart::Rgba8 background{255, 0, 255, 255};
    if (args.contains("background") && !jasperengine::pixelart::parseHex(args.value("background", std::string()), background)) {
        call->fail("background is a colour \"#rrggbb\" (the default #ff00ff is a key colour the checks read).");
        return;
    }
    background.a = 255;
    const double start = args.value("time", 0.0);
    float lifeLo = 0.0f, lifeHi = 0.0f;
    document.main.startLifetime.range(lifeLo, lifeHi);
    // one loop, or the one-shot until its last particle is gone
    const double end = args.contains("time_end") ? args.value("time_end", 0.0)
                                                  : (document.looping ? start + document.duration : (double)document.duration + (double)lifeHi);
    if (start < 0.0 || end < start) {
        call->fail("time and time_end are seconds, time_end after time.");
        return;
    }
    pixelart::ParticleCaptureSettings settings;
    settings.document = document;
    uint32_t seed = document.seed;
    if (seed == 0 && target.entity != NULL_ENTITY) seed = target.scene->scene->getComponent<ParticleSystemComponent>(target.entity).seed;
    settings.seed = seed == 0 ? 1u : seed;
    settings.pixelsPerUnit = std::max(target.project->getPixelsPerUnit(), 1);
    settings.background = background;
    for (int i = 0; i < frames; i++) {
        // a loop's frames stop short of its end (the end is the start again); a one-shot's reach it
        const double span = document.looping ? (end - start) / frames : (frames > 1 ? (end - start) / (frames - 1) : 0.0);
        settings.times.push_back(start + span * i);
    }
    auto capture = std::make_shared<pixelart::ParticleCapture>();
    std::string error;
    if (!capture->begin(settings, error)) {
        call->fail("The effect cannot be captured: " + error + ".");
        return;
    }
    const bool checks = args.value("checks", false);
    const uint32_t budget = target.scene && target.scene->scene ? target.scene->scene->getSystem<ParticleSubsystem>()->getWorldStats().budget : 0u;
    // save_path: Bake to Sprite Sheet (R21): the frames as a strip PNG and its Aseprite json-hash beside it
    fs::path savePath;
    if (args.contains("save_path") && !args.value("save_path", std::string()).empty()) {
        savePath = FileUtils::pathFromUtf8(args.value("save_path", std::string()));
        if (savePath.is_relative()) savePath = target.project->getProjectPath() / savePath;
        if (savePath.extension() != ".png") {
            call->fail("save_path is a .png (its .json goes beside it).");
            return;
        }
    }
    Project* project = target.project;
    call->waitUntil([capture]() { return capture->step(); }, 120000, [call, capture, scale, background, checks, budget, savePath, project](bool timedOut) {
        if (timedOut || capture->failed()) {
            call->fail(timedOut ? "The capture did not finish within 120 s." : capture->error());
            return;
        }
        const pixelart::PixelImage sheet = pixelart::particleContactSheet(capture->frames(), capture->width(), capture->height(), scale, 0,
                                                                          background, true);
        Json frameList = Json::array();
        for (size_t i = 0; i < capture->frames().size(); i++) {
            const pixelart::CapturedFrame& f = capture->frames()[i];
            frameList.push_back({{"index", i + 1}, {"t", f.time}, {"particle_count", f.count}});
        }
        Json data = {{"frames", frameList}, {"scale", scale}, {"cell", {capture->width(), capture->height()}},
                     {"width", sheet.width()}, {"height", sheet.height()}, {"background", jasperengine::pixelart::toHex(background, false)}};
        std::string text = std::to_string(capture->frames().size()) + " frame(s) of " + std::to_string(capture->width()) + "x" +
                           std::to_string(capture->height()) + " base pixels at " + std::to_string(scale) + "x";
        if (checks) {
            const pixelart::lint::LintReport report = pixelart::particleCaptureChecks(*capture, budget ? budget : particles::defaultWorldBudget());
            data["checks"] = pixelart::lint::toJson(report);
            text += "; checks: " + std::to_string(report.pass) + " pass, " + std::to_string(report.warn) + " warn, " +
                    std::to_string(report.fail) + " fail, " + std::to_string(report.skip) + " skip";
        }
        if (!savePath.empty()) {
            const pixelart::PixelImage strip = pixelart::particleSpriteStrip(capture->frames(), capture->width(), capture->height(), scale, 0, background);
            fs::path jsonPath = savePath;
            jsonPath.replace_extension(".json");
            std::vector<WriteFilesCmd::Item> items;
            items.push_back({savePath, strip.encodePng()});
            items.push_back({jsonPath, pixelart::particleSpriteStripJson(FileUtils::pathToUtf8(savePath.filename()), capture->frames(), capture->width(),
                                                                          capture->height(), scale, 0)});
            std::string error;
            if (!ProjectFileWrites::writeUndoable(project, items, error, nullptr, nullptr, {}, true, "Bake to Sprite Sheet")) {
                call->fail("The sprite sheet could not be written: " + error);
                return;
            }
            data["saved"] = {FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(savePath)),
                             FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(jsonPath))};
            text += "; baked " + FileUtils::pathToUtf8(savePath.filename()) + " and its .json (one undo step)";
        }
        ToolResult reply = ToolResult::success(text + ".", data);
        const std::string png = sheet.encodePng();
        ToolImage image;
        image.data = Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size());
        reply.images.push_back(std::move(image));
        call->reply(std::move(reply));
    });
}

void captureTool(const CallPtr& call) {
    const Json& args = call->args();
    EffectTarget target;
    const bool sheet = args.contains("frames") || args.contains("time_end") || args.contains("scale") || args.contains("background") ||
                       args.contains("checks");
    if (!args.contains("entity") && !(sheet && args.contains("path"))) {
        call->fail("entity is required (a contact sheet takes path too).");
        return;
    }
    if (!resolveEffectTarget(call, target, false)) return;
    if (sheet) {
        captureSheetTool(call, target);
        return;
    }
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
                                               " particles), " + std::to_string(result.info.value("width", 0)) + "x" +
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

    std::vector<std::string> presetNames = particlePresetNames();
    for (const std::string& name : pixelart::pixelParticlePresetNames()) presetNames.push_back(name);
    add("create_particle_effect", "Create Particle Effect",
        "A new .particles asset from a create_particles preset or an effect JSON (the keyed form get_particle_effect returns), "
        "one undo step of the project. set_particle_effect plays it on a Particle System.",
        object({{"path", assetPath("The new file (.particles).")},
                {"preset", stringEnum("", presetNames)},
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
        "rotationBySpeed, externalForces, noise, orbit, collision, subEmitters, textureSheetAnimation, trails, customData, setOverAge, pixelArt.",
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
        "Play, pause, stop (keeps the particles), clear, restart, emit count particles, seek to time (restart and simulate) or "
        "step ticks. In Edit mode the Scene view's Particle Effect preview runs the system. Returns its statistics.",
        object({{"entity", entityRef()}, {"scene", sceneRef()},
                {"action", stringEnum("", {"play", "pause", "stop", "clear", "restart", "emit", "seek", "step"})},
                {"count", integer("", 1, 100000)}, {"time", number("Seconds.")}, {"ticks", integer("", 1, 100000)}},
               {"entity", "action"}),
        controlTool, false);
    add("get_particle_stats", "Particle Statistics",
        "Live statistics of one Particle System (entity) or every one of the scene: particle_count, capacity, budget share, "
        "ticks, dropped time, simulate/render microseconds, culled, sorted views; and the world's totals.",
        object({{"entity", entityRef()}, {"scene", sceneRef()}}), statsTool, true);
    add("capture_particles", "Capture Particle Effect",
        "Seek a Particle System to time (seconds) and capture the view (camera: scene or game) as an image; the system stays "
        "paused there. frames, scale, background or checks draw the effect alone instead: a labelled contact sheet of frames "
        "times to time_end (default one loop) at an integer scale, with pixel-art lint results.",
        object({{"entity", entityRef()}, {"path", assetPath("")}, {"scene", sceneRef()}, {"time", number("Seconds.")},
                {"camera", withDefault(stringEnum("", {"scene", "game"}), "scene")},
                {"max_width", withDefault(integer("", 0, 4096), 768)}, {"save_path", string("Also write the PNG here (frames: a sprite strip and its Aseprite .json).")},
                {"frames", integer("", 1, 64)}, {"time_end", number("")}, {"scale", integer("", 1, 8)},
                {"background", string("#rrggbb")}, {"checks", boolean("")}},
               {"time"}),
        captureTool, false);
    add("get_particle_migration_report", "Particle Migration Report",
        "The old particle systems the documents read since the project opened were converted from (S20): per document, one "
        "line per system (where its effect went, what was approximated).",
        object({}), migrationReportTool, true);
}

}
