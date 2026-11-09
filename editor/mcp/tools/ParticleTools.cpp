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
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"
#include "mcp/tools/ParticleShared.h"
#include "mcp/tools/PlayViewCommon.h"
#include "mcp/tools/W4Shared.h"

#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/ParticleEffectAssetCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ParticleSystemComponent.h"
#include "particles/AnalyticParticles.h"
#include "io/watch/FileWatchService.h"
#include "particles/ParticleEffectPreview.h"
#include "particles/ParticlePreviewSource.h"
#include "particles/ParticleSheetBake.h"
#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleEffectLibrary.h"
#include "particles/ParticleModules.h"
#include "pixelart/particles/PixelParticlePresets.h"
#include "pixelart/particles/PixelPresetCreate.h"
#include "pixelart/PixelColor.h"
#include "subsystem/ParticleSubsystem.h"
#include "subsystem/ParticleSystemState.h"
#include "subsystem/RenderSystem.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "texsynth/SynthesisSession.h"
#include "util/AssetDatabase.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/QaChecks.h"
#include "util/SafeWrite.h"
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

const char* fieldTypeName(particles::FieldType type) {
    switch (type) {
        case particles::FieldType::Bool: return "bool";
        case particles::FieldType::Int: return "int";
        case particles::FieldType::UInt: return "uint";
        case particles::FieldType::Float: return "float";
        case particles::FieldType::Enum: return "enum";
        case particles::FieldType::Mask: return "mask";
        case particles::FieldType::Vector2: return "vec2";
        case particles::FieldType::Vector3: return "vec3";
        case particles::FieldType::Vector4: return "vec4";
        case particles::FieldType::Color: return "color";
        case particles::FieldType::String: return "string";
        case particles::FieldType::MinMaxCurve: return "min_max_curve";
        case particles::FieldType::MinMaxGradient: return "min_max_gradient";
        case particles::FieldType::Curve: return "curve";
        case particles::FieldType::Gradient: return "gradient";
        case particles::FieldType::Struct: return "struct";
        case particles::FieldType::Array: return "array";
        case particles::FieldType::IntArray: return "int_array";
        case particles::FieldType::StringArray: return "string_array";
    }
    return "unknown";
}

// a field's default as the effect JSON writes it (null for the structured kinds)
Json fieldDefault(const particles::FieldDesc& f, const void* defaults) {
    if (!defaults) return nullptr;
    const void* v = f.field(defaults);
    switch (f.type) {
        case particles::FieldType::Bool: return *static_cast<const bool*>(v);
        case particles::FieldType::Int: return *static_cast<const int*>(v);
        case particles::FieldType::UInt:
        case particles::FieldType::Mask: return *static_cast<const uint32_t*>(v);
        case particles::FieldType::Float: return ParticleEffectCodec::jsonFloat(*static_cast<const float*>(v));
        case particles::FieldType::Enum: {
            const particles::EnumOption* option = f.findOption(*static_cast<const uint8_t*>(v));
            return option ? Json(option->json) : Json(nullptr);
        }
        case particles::FieldType::Vector2: {
            const Vector2& x = *static_cast<const Vector2*>(v);
            return Json::array({ParticleEffectCodec::jsonFloat(x.x), ParticleEffectCodec::jsonFloat(x.y)});
        }
        case particles::FieldType::Vector3: {
            const Vector3& x = *static_cast<const Vector3*>(v);
            return Json::array({ParticleEffectCodec::jsonFloat(x.x), ParticleEffectCodec::jsonFloat(x.y), ParticleEffectCodec::jsonFloat(x.z)});
        }
        case particles::FieldType::Vector4:
        case particles::FieldType::Color: {
            const Vector4& x = *static_cast<const Vector4*>(v);
            return Json::array({ParticleEffectCodec::jsonFloat(x.x), ParticleEffectCodec::jsonFloat(x.y), ParticleEffectCodec::jsonFloat(x.z),
                                ParticleEffectCodec::jsonFloat(x.w)});
        }
        case particles::FieldType::String: return *static_cast<const std::string*>(v);
        case particles::FieldType::MinMaxCurve: return toMcp(ParticleEffectCodec::minMaxCurveToJson(*static_cast<const particles::MinMaxCurve*>(v)));
        case particles::FieldType::MinMaxGradient: return toMcp(ParticleEffectCodec::minMaxGradientToJson(*static_cast<const particles::MinMaxGradient*>(v)));
        default: return nullptr;
    }
}

Json structFieldsJson(const particles::StructDesc& desc) {
    Json fields = Json::array();
    for (const particles::FieldDesc& f : desc.fields) {
        if (f.flags & particles::FieldFlag_Hidden) continue;
        Json item = {{"key", f.key}, {"label", f.label}, {"type", fieldTypeName(f.type)}, {"default", fieldDefault(f, desc.defaults)}};
        if (f.minValue > -1.0e38f) item["min"] = ParticleEffectCodec::jsonFloat(f.minValue);
        if (f.maxValue < 1.0e38f) item["max"] = ParticleEffectCodec::jsonFloat(f.maxValue);
        if (f.options) {
            Json options = Json::array();
            for (uint8_t o = 0; o < f.optionCount; o++) options.push_back(f.options[o].json);
            item["options"] = options;
        }
        if (f.element) item["fields"] = structFieldsJson(*f.element);
        if (f.tooltip) item["tooltip"] = f.tooltip;
        fields.push_back(item);
    }
    return fields;
}

// get_particle_effect's module_fields (S91 R30): each named module's fields, defaults and ranges
// from the table the Inspector and the codec use ("effect": the document's own fields)
bool moduleFieldsJson(const Json& keys, Json& out, std::string& error) {
    if (!keys.is_array() || keys.empty()) {
        error = "module_fields is a list of module keys (e.g. [\"emission\", \"shape\"]), or [\"effect\"] for the effect's own fields.";
        return false;
    }
    out = Json::object();
    for (const Json& key : keys) {
        const std::string name = key.is_string() ? key.get<std::string>() : std::string();
        if (name == "effect") {
            out[name] = {{"label", "Effect"}, {"fields", structFieldsJson(particles::ParticleModules::documentFields())}};
            continue;
        }
        const particles::ModuleDesc* module = ParticleModules::find(name);
        if (!module) {
            std::string all = "effect";
            for (const particles::ModuleDesc& m : ParticleModules::all()) all += ", " + std::string(m.key);
            error = "module_fields: no module \"" + name + "\". Modules: " + all + ".";
            return false;
        }
        out[name] = {{"label", module->label}, {"fields", structFieldsJson(module->fields)}};
    }
    return true;
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
    std::vector<std::string> children;
    // a pixel preset writes its palette copy and children too: one undo step with the effect
    CommandHistory* history = project->getProjectCommandHistory();
    std::unique_ptr<UndoGroupScope> holdGroup;
    size_t groupMark = 0;
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
            if (!made.writes.empty() && history) {
                holdGroup = std::make_unique<UndoGroupScope>(history);
                groupMark = history->mark();
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
    if (holdGroup)
        history->groupSince(groupMark, "Create Particle Effect " + fs::path(path).filename().generic_string());
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
    const Json& args = call->args();
    if (args.contains("module_fields") && !args.contains("path") && !args.contains("entity")) {
        // the fields alone, without an effect
        Json fields;
        std::string error;
        if (!moduleFieldsJson(args["module_fields"], fields, error)) {
            call->fail(error);
            return;
        }
        call->ok(std::to_string(fields.size()) + " module(s): their fields, defaults and ranges (the Inspector's table).", Json{{"module_fields", fields}});
        return;
    }
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
    if (call->args().contains("module_fields")) {
        Json fields;
        std::string error;
        if (!moduleFieldsJson(call->args()["module_fields"], fields, error)) {
            call->fail(error);
            return;
        }
        data["module_fields"] = fields;
    }
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
    // a dry run predicts the edit, so it is refused where the edit is (a playing scene)
    if (!resolveEffectTarget(call, target, true)) return;
    if (args.value("dry_run", false)) {
        // the patch on a copy: what it would do, nothing changed and no undo step
        ParticleEffectDocument document;
        std::vector<Diagnostic> diagnostics;
        if (!readTarget(target, document, diagnostics)) {
            call->fail("The effect asset " + target.asset + " cannot be read (" + firstError(diagnostics) + ").", Json{{"diagnostics", diagnosticsJson(diagnostics)}});
            return;
        }
        ParticleEffectDocument patched = document;
        diagnostics.clear();
        if (!ParticleEffectCodec::applyMergePatch(patched, toOrdered(args["patch"]), diagnostics)) {
            call->fail("The patch would be refused (" + firstError(diagnostics) + "); nothing changed.", Json{{"diagnostics", diagnosticsJson(diagnostics)}});
            return;
        }
        const bool changes = ParticleEffectCodec::write(patched) != ParticleEffectCodec::write(document);
        Json data = targetJson(target);
        data["dry_run"] = true;
        data["changed"] = false;
        data["would_change"] = changes;
        data["effect"] = toMcp(ParticleEffectCodec::toKeyedJson(patched));
        data["modules"] = moduleKeys(patched);
        data["diagnostics"] = diagnosticsJson(diagnostics);
        call->ok(std::string(changes ? "The patch would apply" : "The patch would change nothing") + " (dry run: nothing changed). " +
                 std::to_string(diagnostics.size()) + " diagnostic(s)" + (diagnostics.empty() ? "." : ", first: " + firstError(diagnostics)), data);
        return;
    }
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
            {"rays_last_tick", s.raysLastTick}, {"bytes_per_particle", s.bytesPerParticle},
            {"simulation_mode", s.analytic ? "analytic" : "simulated"}, {"pixel", s.pixel}, {"analytic_slots", s.analyticSlots}};
}

void replyControl(const CallPtr& call, const EffectTarget& target, ParticleSubsystem& particles, const std::string& action, bool editMode,
                  const std::vector<Entity>& systems) {
    Json data = targetJson(target);
    data["action"] = action;
    data["edit_mode"] = editMode;
    if (target.entity != NULL_ENTITY && target.scene->scene->findComponent<ParticleSystemComponent>(target.entity)) {
        data["stats"] = statsJson(particles, target.entity);
    }
    int total = 0;
    if (systems.size() > 1 || !data.contains("stats")) {
        Json list = Json::array();
        for (Entity entity : systems) {
            Json item = statsJson(particles, entity);
            item["entity"] = McpRefs::entityJson(target.scene, entity);
            total += item.value("particle_count", 0);
            list.push_back(item);
        }
        data["systems"] = list;
    } else {
        total = data["stats"].value("particle_count", 0);
    }
    const double time = systems.empty() ? 0.0 : particles.getTime(systems.front());
    call->ok(action + ": " + std::to_string(total) + " particles" + (systems.size() > 1 ? " in " + std::to_string(systems.size()) + " systems" : std::string()) +
             " at " + std::to_string(time) + " s" + (editMode ? " (Edit mode: the Scene view's preview runs it)." : "."), data);
}

// the Particle Systems a control or capture acts on: the GameObject's own, or with_children every
// one of its subtree (S91 R31: a multi-system effect plays, seeks and captures as a whole)
bool controlTargets(const CallPtr& call, EffectTarget& target, std::vector<Entity>& systems) {
    const Json& args = call->args();
    if (!args.contains("entity")) {
        call->fail("entity is required.");
        return false;
    }
    if (!args.value("with_children", false)) {
        if (!resolveEffectTarget(call, target, false)) return false;
        systems = {target.entity};
        return true;
    }
    std::string error;
    target.project = call->ctx().project();
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
    systems = ParticlePreviewSource::subtreeSystems(target.scene->scene, target.entity);
    if (systems.empty()) {
        call->fail("\"" + target.scene->scene->getEntityName(target.entity) + "\" has no Particle System in its subtree (create_particles adds one).");
        return false;
    }
    if (ParticleSystemComponent* own = target.scene->scene->findComponent<ParticleSystemComponent>(target.entity)) {
        target.inlineEffect = own->effect.empty();
        if (!target.inlineEffect) target.asset = own->effect.path;
    }
    return true;
}

ParticleEffectOverlay* overlayOf(const CallPtr& call) {
    SceneWindow* window = call->ctx().sceneWindow();
    return window ? &window->getParticleOverlay() : nullptr;
}

void controlTool(const CallPtr& call) {
    const Json& args = call->args();
    EffectTarget target;
    std::vector<Entity> systems;
    if (!controlTargets(call, target, systems)) return;
    const std::string action = args.value("action", std::string());
    Scene* scene = target.scene->scene;
    std::shared_ptr<ParticleSubsystem> particles = scene->getSystem<ParticleSubsystem>();
    // Edit mode: the Scene view's preview runs the systems (it steps no systems itself)
    const bool editMode = target.scene->playState == ScenePlayState::STOPPED;
    ParticleEffectOverlay* overlay = editMode ? overlayOf(call) : nullptr;
    if (overlay) {
        for (Entity entity : systems) overlay->addTarget(target.scene->id, entity);
    }

    if (action == "play") {
        for (Entity entity : systems) particles->play(entity);
        if (overlay) overlay->play(target.scene->id);
    } else if (action == "pause") {
        for (Entity entity : systems) particles->pause(entity);
        if (overlay) overlay->pause(target.scene->id);
    } else if (action == "stop" || action == "clear") {
        for (Entity entity : systems) particles->stop(entity, action == "clear" || args.value("clear", false));
        if (overlay && action == "clear") overlay->pause(target.scene->id);
    } else if (action == "restart") {
        for (Entity entity : systems) particles->restart(entity);
        if (overlay) overlay->play(target.scene->id);
    } else if (action == "emit") {
        const int count = args.value("count", 1);
        if (count < 1 || count > 100000) {
            call->fail("count is 1 to 100000.");
            return;
        }
        for (Entity entity : systems) particles->emit(entity, (uint32_t)count);
    } else if (action == "seek") {
        if (!args.contains("time") || !args["time"].is_number() || args["time"].get<double>() < 0.0) {
            call->fail("time (seconds, >= 0) is required for seek.");
            return;
        }
        const double time = args["time"].get<double>();
        if (overlay) {
            // the preview's scrub (every system it previews in the scene): from the nearest
            // checkpoint, over frames, then held there
            overlay->seek(target.project, target.scene, time);
            const uint32_t sceneId = target.scene->id;
            call->waitUntil([overlay, sceneId]() { return !overlay->isSeeking(sceneId); }, 60000, [call, target, particles, action, systems](bool timedOut) {
                if (timedOut) {
                    call->fail("The seek did not finish within 60 s.");
                    return;
                }
                replyControl(call, target, *particles, action, true, systems);
            });
            return;
        }
        for (Entity entity : systems) particles->seek(entity, time);
    } else if (action == "step") {
        const int ticks = args.value("ticks", 1);
        if (ticks < 1 || ticks > 100000) {
            call->fail("ticks is 1 to 100000.");
            return;
        }
        for (Entity entity : systems) particles->step(entity, (uint32_t)ticks);
        if (overlay) overlay->pause(target.scene->id);
    } else if (action == "trigger") {
        const std::string name = args.value("name", std::string());
        if (name.empty()) {
            call->fail("name (the trigger's, as the effect's triggers or Trigger sub-emitters name it) is required for trigger.");
            return;
        }
        bool playing = false;
        for (Entity entity : systems) {
            particles->sendTrigger(entity, name);
            playing = playing || particles->isPlaying(entity);
        }
        // what the trigger left playing (a play, or sub-emitters fired from a stopped system) runs in the preview
        if (overlay && playing) overlay->play(target.scene->id);
    } else {
        call->fail("action must be play, pause, stop, clear, restart, emit, seek, step or trigger.");
        return;
    }
    target.scene->needUpdateRender = true;
    replyControl(call, target, *particles, action, editMode, systems);
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
    std::vector<Entity> systems;
    if (!controlTargets(call, target, systems)) return;
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
    for (Entity entity : systems) particles->seek(entity, time);
    if (target.scene->playState == ScenePlayState::STOPPED) {
        if (ParticleEffectOverlay* overlay = overlayOf(call)) {
            for (Entity entity : systems) overlay->addTarget(target.scene->id, entity);
            overlay->pause(target.scene->id);
        }
    } else {
        for (Entity entity : systems) particles->pause(entity);
    }
    target.scene->needUpdateRender = true;

    w3::CaptureRequest request;
    request.view = view;
    request.scene = target.scene->id;
    request.maxWidth = std::clamp(args.value("max_width", 768), 0, 4096);
    request.savePath = args.value("save_path", std::string());
    Json stats = statsJson(*particles, systems.front());
    if (systems.size() > 1) {
        // the subtree's whole: its particles, and each system's seed and state
        int total = 0;
        Json list = Json::array();
        for (Entity entity : systems) {
            Json item = statsJson(*particles, entity);
            item["entity"] = McpRefs::entityJson(target.scene, entity);
            total += item.value("particle_count", 0);
            list.push_back(item);
        }
        stats["particle_count"] = total;
        stats["systems"] = list;
    }
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

// ------------------------------------------------------------------ preview and bake (S91)

// the runs in flight: kept alive until they end (a run is never destroyed inside its own callback)
std::vector<std::shared_ptr<ParticleEffectPreview>>& activePreviews() {
    static std::vector<std::shared_ptr<ParticleEffectPreview>> runs;
    return runs;
}

std::string projectRelative(Project* project, const fs::path& absolute) {
    return absolute.lexically_relative(project->getProjectPath()).generic_string();
}

// The systems of path or entity (a whole subtree) and the request's common arguments; `scope`
// names the effect for the fix rounds
bool previewRequest(const CallPtr& call, const Json& args, ParticlePreviewRequest& request, std::string& scope) {
    Project* project = call->ctx().project();
    std::string error;
    if (args.contains("path") && !args["path"].is_null()) {
        std::string asset;
        if (!resolveEffectPath(project, args.value("path", std::string()), true, asset, error) ||
            !ParticlePreviewSource::fromAsset(asset, request.systems, error)) {
            call->fail(error);
            return false;
        }
        scope = asset;
    } else if (args.contains("entity")) {
        SceneProject* sceneProject = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!sceneProject) {
            call->fail(error);
            return false;
        }
        if (!sceneProject->scene) {
            call->fail(McpRefs::notOpenError(project, *sceneProject));
            return false;
        }
        const Entity entity = McpRefs::resolveEntity(sceneProject, args["entity"], error);
        if (entity == NULL_ENTITY) {
            call->fail(error);
            return false;
        }
        if (!ParticlePreviewSource::fromSubtree(sceneProject->scene, entity, request.systems, error)) {
            call->fail(error);
            return false;
        }
        scope = ParticlePreviewSource::subtreeScope(*sceneProject, entity);
    } else {
        call->fail("Give path (a .particles asset) or entity (a GameObject: its Particle Systems and every one beneath it).");
        return false;
    }
    if (args.contains("reference")) {
        fs::path file;
        if (!McpRefs::resolveProjectPath(project, args.value("reference", std::string()), file, error, true) ||
            !PoseImage::readPng(FileUtils::pathToUtf8(file), request.reference)) {
            if (error.empty()) error = "reference is not an image the editor reads (PNG, JPG).";
            error = "reference: " + error;
        }
    }
    // the run arguments, thresholds, rounds and validation: the same code as the window's panel
    if (!error.empty() || !ParticlePreviewSource::complete(project, args, scope, request, error)) {
        call->fail(error);
        return false;
    }
    return true;
}

// a save_path: a project-relative .png
bool imageTarget(const CallPtr& call, const char* key, fs::path& target) {
    const Json& args = call->args();
    if (!args.contains(key)) return true;
    const std::string value = args[key].is_string() ? args[key].get<std::string>() : std::string();
    std::string error;
    if (FileUtils::pathFromUtf8(value).extension() != ".png") {
        call->fail(std::string(key) + " must be a project-relative .png path.");
        return false;
    }
    if (!McpRefs::resolveProjectWritePath(call->ctx().project(), value, target, error, false)) {
        call->fail(std::string(key) + ": " + error);
        return false;
    }
    return true;
}

bool writeImage(Project* project, const fs::path& target, const std::vector<uint8_t>& png, std::string& error) {
    std::error_code ec;
    // the call's own write and the folders it makes: not outside changes for the watcher
    fs::path created = target;
    while (created.has_parent_path() && created.parent_path() != created && !fs::exists(created.parent_path(), ec)) created = created.parent_path();
    ScopedExpectedWrite expected(created, created != target);
    if (!safewrite::writeFileResolvedBeneath(McpRefs::rootFor(project, target), target, std::string(png.begin(), png.end()), error)) return false;
    for (fs::path dir = target.parent_path(); created != target; dir = dir.parent_path()) {
        expected.reported(dir);
        if (dir == created || !dir.has_parent_path() || dir.parent_path() == dir) break;
    }
    expected.reported(target);
    return true;
}

// The run as a job: `done` builds the reply from the finished run on the main thread
void runPreviewJob(const CallPtr& call, ParticlePreviewRequest request, const std::string& kind, const std::string& title,
                   std::function<ToolResult(ParticleEffectPreview& run)> done) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    const bool wait = args.value("wait", true);
    const double timeout = args.value("timeout_seconds", 120.0);
    auto run = std::make_shared<ParticleEffectPreview>(std::move(request));
    activePreviews().push_back(run);
    JobPtr job = ctx.jobs().create(kind, title);
    auto reply = std::make_shared<ToolResult>();
    std::weak_ptr<ParticleEffectPreview> weak = run;
    job->setCancelHandler([weak]() {
        if (auto r = weak.lock()) r->cancel();
    });
    job->setProgress(0.0, "simulating");
    run->setOnFinished([weak, job, reply, done]() {
        auto r = weak.lock();
        if (!r) return;
        if (r->isFailed()) {
            if (job->cancelRequested()) job->markCancelled("Cancelled; nothing was written.");
            else job->fail(r->getError());
        } else {
            // the run ends from the editor's frame, not inside a tool call: its writes (save_path, the
            // bake's files and their undo step) stay beneath the project as the call's own would, and
            // whatever goes wrong ends the job (the frame loop would swallow it)
            McpWriteScope writeScope;
            try {
                *reply = done(*r);
            } catch (const std::exception& e) {
                *reply = ToolResult::error(std::string("The preview failed, nothing was written: ") + e.what());
            }
            Json result = reply->structured.is_object() ? reply->structured : Json::object();
            if (reply->isError) job->fail(reply->text, result);
            else job->succeed(reply->text, result);
        }
        // released after this frame: never inside its own callback
        getEditorHost().enqueueMainThreadTask([weak]() {
            auto& runs = activePreviews();
            runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const std::shared_ptr<ParticleEffectPreview>& p) { return p == weak.lock(); }),
                       runs.end());
        });
    });
    run->start();
    call->detach();
    replyWhenJobDone(call, job, wait, timeout, [reply](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) {
            ToolResult failure = ToolResult::error(reply->text.empty() ? finished.message() : reply->text, result);
            failure.images = reply->images;
            return failure;
        }
        ToolResult out = *reply;
        out.structured = result;
        return out;
    });
}

std::string previewText(const std::string& scope, const Json& report) {
    const Json& count = report["metrics"]["count"];
    char head[256];
    // (the scope apart: a deep subtree's path must not cut the verdict off)
    std::snprintf(head, sizeof(head), ": %llu ticks of %.4g s (%.3g s), peak %u particles at %.3g s, emitted %llu. Verdict %s.",
                  (unsigned long long)report.value("ticks", (uint64_t)0), report.value("tick_seconds", 0.0), report.value("seconds", 0.0),
                  count.value("peak", 0u), count.value("peak_at", 0.0), (unsigned long long)count.value("emitted", (uint64_t)0),
                  report.value("verdict", "").c_str());
    std::string text = scope + head;
    text += " Seeds:";
    for (const Json& s : report["seeds"]) text += " " + s.value("system", "") + "=" + std::to_string(s.value("seed", 0u));
    text += ", state_hash " + report.value("state_hash", "") + ".";
    for (const Json& check : report["checks"]) text += "\n" + qa::rowLine(check);
    const Json& cost = report["metrics"]["cost"];
    text += "\nCost on " + cost.value("platform", "") + ": " + Json(cost["sim_ms"]).dump() + " ms simulation + " + Json(cost["prep_ms"]).dump() +
            " ms render prep a frame (estimate, not measured on a device).";
    text += "\nNot simulated: colliders, force fields, the world budget, triggers.";
    if (!report["thresholds"]["changed"].empty()) text += "\nThresholds changed from their defaults: " + report["thresholds"]["changed"].dump() + ".";
    text += "\n" + report.value("guidance", "");
    return text;
}

void previewEffectTool(const CallPtr& call) {
    ParticlePreviewRequest request;
    std::string scope;
    if (!previewRequest(call, call->args(), request, scope)) return;
    fs::path saveTarget;
    if (!imageTarget(call, "save_path", saveTarget)) return;
    const std::string failOn = call->args().value("fail_on", std::string());
    if (!failOn.empty() && failOn != "fail" && failOn != "warn") {
        call->fail("fail_on is fail or warn.");
        return;
    }
    Project* project = call->ctx().project();
    // the project it was asked in: a run finishing after a switch records and writes nothing
    const uint64_t generation = texsynth::projectGeneration();
    const fs::path root = project->getProjectPath();
    runPreviewJob(call, std::move(request), "preview_particle_effect", "Preview Particle Effect " + scope,
                  [scope, saveTarget, failOn, project, generation, root](ParticleEffectPreview& run) {
        if (generation != texsynth::projectGeneration() || project->getProjectPath() != root) {
            return ToolResult::error("Another project was opened during the preview of " + scope + "; nothing was written.");
        }
        Json data = run.getReport();
        ParticlePreviewSource::rounds().record(scope, data["checks"]);
        std::string text = previewText(scope, data);
        data["next"] = data.value("guidance", "");
        ToolResult reply;
        std::vector<uint8_t> png;
        if (!run.getImage().isEmpty() && run.getImage().encodePng(png)) {
            data["image"] = {{"width", run.getImage().getWidth()}, {"height", run.getImage().getHeight()},
                             {"cells", "left to right at the cells' times; labels: t=<seconds> <live particles>"}};
            if (!saveTarget.empty()) {
                std::string error;
                if (!writeImage(project, saveTarget, png, error)) return ToolResult::error("Could not write the filmstrip to " + projectRelative(project, saveTarget) + ": " + error);
                data["saved_to"] = projectRelative(project, saveTarget);
                text += "\nSaved to " + projectRelative(project, saveTarget) + ".";
            }
            reply.images.push_back({Base64::encode(png.data(), png.size()), "image/png"});
        } else if (!saveTarget.empty()) {
            text += "\nframes 0: no filmstrip, nothing saved.";
        }
        reply.text = text;
        reply.structured = data;
        reply.isError = qa::reaches(data.value("verdict", ""), failOn);
        return reply;
    });
}

void bakeSheetTool(const CallPtr& call) {
    const Json& args = call->args();
    ParticlePreviewRequest base;
    std::string scope;
    const int columns = args.value("columns", 4), rows = args.value("rows", 4), cellSize = args.value("cell_size", 128);
    std::string error = ParticleSheetBake::validate(columns, rows, cellSize);
    if (!error.empty()) {
        call->fail(error);
        return;
    }
    // the preview's own checks of path, entity, seed, seconds and view (a sheet faces the camera: front)
    Json previewArgs = args;
    if (!previewArgs.contains("view")) previewArgs["view"] = "front";
    previewArgs["frames"] = columns * rows;
    if (!previewRequest(call, previewArgs, base, scope)) return;
    std::string output;
    const bool write = args.contains("output");
    if (write) {
        output = args.value("output", std::string());
        if (output.size() > 4 && (output.substr(output.size() - 4) == ".png")) output.resize(output.size() - 4);
        if (output.size() > 10 && (output.substr(output.size() - 10) == ".particles")) output.resize(output.size() - 10);
        // refused now, not after the run: a path out of the project (a link on the way included)
        fs::path absolute;
        for (const char* extension : {".png", ".particles", ".json"}) {
            if (!McpRefs::resolveProjectWritePath(call->ctx().project(), output + extension, absolute, error, false)) {
                call->fail("output: " + error);
                return;
            }
        }
    }
    const bool overwrite = args.value("overwrite", false);
    // S49: the frames' Aseprite json-hash beside the sheet (a texture's Sprite Mode slices it)
    const bool aseprite = args.value("aseprite", false);
    ParticlePreviewRequest request = ParticleSheetBake::sheetRequest(base, columns, rows, cellSize);
    bool additive = true;
    bool looping = false;
    for (const ParticlePreviewSystem& s : request.systems) {
        additive = additive && s.document.renderer.blend == particles::ParticleBlend::Additive;
        looping = looping || s.document.looping;
    }
    Project* project = call->ctx().project();
    McpContext* ctx = &call->ctx();
    // the project it was asked in: a bake finishing after a switch writes nothing (bake_procedural_material's rule)
    const uint64_t generation = texsynth::projectGeneration();
    const fs::path root = project->getProjectPath();
    runPreviewJob(call, std::move(request), "bake_particle_sheet", (write ? "Bake Sprite Sheet " : "Preview Sprite Sheet ") + scope,
                  [=](ParticleEffectPreview& run) {
        if (generation != texsynth::projectGeneration() || project->getProjectPath() != root) {
            return ToolResult::error("Another project was opened while " + scope + " baked; nothing was written.");
        }
        const PoseImage sheet = ParticleSheetBake::composeSheet(run.getCells(), columns, rows);
        const Json& report = run.getReport();
        const double seconds = report.value("seconds", 0.0);
        // the pixel view's cells are its base pixels at its scale, not cell_size
        const int cell = run.getCells().empty() ? cellSize : run.getCells().front().getWidth();
        Json data = {{"columns", columns}, {"rows", rows}, {"cell_size", cell}, {"seconds", seconds}, {"frame", report["frame"]},
                     {"seeds", report["seeds"]}, {"state_hash", report["state_hash"]}, {"verdict", report["verdict"]}, {"checks", report["checks"]},
                     {"alpha", "straight; additive light without coverage keeps its brightest channel as alpha"}};
        std::string text = scope + ": " + std::to_string(columns) + "x" + std::to_string(rows) + " frames of " + std::to_string(cell) + " px over " +
                           report["seconds"].dump() + " s, straight alpha.";
        ToolResult reply;
        std::vector<uint8_t> png;
        sheet.encodePng(png);
        if (write) {
            const particles::ParticleEffectDocument effect =
                ParticleSheetBake::sheetEffect(std::string(), columns, rows, seconds, run.getFrameWorldSize(), run.getFrameCenter(), looping, additive);
            std::vector<WriteFilesCmd::Item> items;
            std::vector<std::string> written;
            std::string why;
            if (!ParticleSheetBake::sheetFiles(project, output, sheet, effect, overwrite, items, written, why) ||
                (aseprite && !ParticleSheetBake::asepriteFile(project, output, columns, rows, cell, cell, seconds, overwrite, items, written, why)) ||
                !w4::writeFilesUndoable(*ctx, std::move(items), why, {}, "Bake Sprite Sheet")) {
                return ToolResult::error("Nothing was written: " + why);
            }
            data["written"] = written;
            data["undo"] = {{"scope", "project"}};
            data["next"] = "set_particle_effect plays " + written[1] + " on a Particle System.";
            text += " Wrote " + written[0] + " and " + written[1] + (aseprite ? " and " + written[2] : std::string()) + " (Texture Sheet Animation " +
                    std::to_string(columns) + "x" + std::to_string(rows) + ") as one undo step \"Bake Sprite Sheet\".";
        } else {
            data["next"] = "The same arguments plus output write the sheet and an effect playing it (one undo step).";
            text += " Nothing written (no output).";
        }
        reply.images.push_back({Base64::encode(png.data(), png.size()), "image/png"});
        reply.text = text;
        reply.structured = data;
        return reply;
    });
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
        object(withTarget({{"module_fields", array(string(""), "Modules (or \"effect\") whose fields, defaults and ranges to list.")}}),
               std::vector<std::string>{}), getEffectTool, true);
    add("edit_particle_effect", "Edit Particle Effect",
        "Apply a JSON merge patch (RFC 7386) to an effect's keyed form as one undo step: {\"modules\": {\"emission\": "
        "{\"rateOverTime\": 20}}}; null removes a key (a module: removes it). A value is a number, or {mode: twoConstants, "
        "constant, constantMax}, {mode: curve, curve: [[t, v], ...], multiplier}; colours [r, g, b, a] sRGB. An asset's edit "
        "changes every system using it. A patch that leaves the effect invalid is refused with its diagnostics.",
        object(withTarget({{"patch", anyObject("The merge patch.")}, {"dry_run", withDefault(boolean("Validate only: no change, no undo step."), false)}}),
               std::vector<std::string>{"patch"}), editEffectTool, false);
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
        "Play, pause, stop (keeps the particles), clear, restart, emit count particles, seek to time (restart and simulate), "
        "step ticks, or trigger name (the effect's triggers and Trigger sub-emitters). In Edit mode the Scene view's Particle "
        "Effect preview runs the system. Returns its statistics.",
        object({{"entity", entityRef()}, {"scene", sceneRef()},
                {"action", stringEnum("", {"play", "pause", "stop", "clear", "restart", "emit", "seek", "step", "trigger"})},
                {"count", integer("", 1, 100000)}, {"time", number("Seconds.")}, {"ticks", integer("", 1, 100000)}, {"name", string("")},
                {"with_children", withDefault(boolean("Every Particle System of the subtree."), false)}},
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
                {"max_width", withDefault(integer("", 0, 4096), 768)}, {"save_path", string("Also write the PNG here.")},
                {"with_children", withDefault(boolean("Seek the subtree's systems too."), false)}},
               {"entity", "time"}),
        captureTool, false);
    add("get_particle_migration_report", "Particle Migration Report",
        "The old particle systems the documents read since the project opened were converted from (S20): per document, one "
        "line per system (where its effect went, what was approximated).",
        object({}), migrationReportTool, true);

    // S91: the deterministic preview with its checks, and the sprite-sheet bake on the same run
    const Json run = {{"path", assetPath("A .particles asset.")}, {"entity", entityRef("A GameObject and its subtree.")},
                      {"scene", sceneRef()}, {"seed", integer("", 0, 2147483647)},
                      {"seconds", number("Default: one play.", 0, 60)}};
    auto withRun = [&](Json extra) {
        Json properties = run;
        for (auto it = extra.begin(); it != extra.end(); ++it) properties[it.key()] = it.value();
        return properties;
    };
    {
        ToolDef tool;
        tool.name = "preview_particle_effect";
        tool.title = "Preview Particle Effect";
        tool.description =
            "Run an effect (path, or entity: its subtree) deterministically in a private scene, changing nothing: a filmstrip "
            "(frames, default 6; 0: none), metrics and checks with fix hints. Cost is an estimate, never measured on a device. "
            "capture_particles shows it in its scene instead.";
        tool.inputSchema = object(withRun({
            {"times", array(number(""), "Seconds, instead of frames.")},
            {"frames", withDefault(integer("", 0, 24), 6)},
            {"view", withDefault(stringEnum("", {"three_quarter", "front", "side", "top", "pixel"}), "three_quarter")},
            {"background", withDefault(stringEnum("", {"dark", "light", "checker"}), "dark")},
            {"overdraw_map", withDefault(boolean("Adds a GPU heat row."), false)},
            {"platform", withDefault(stringEnum("", {"mobile", "web", "desktop"}), "mobile")},
            {"fail_on", stringEnum("", {"fail", "warn"})},
            {"reference", string("Image shown as the last cell.")},
            {"save_path", string("Also write the PNG.")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 1, 3600), 120)}}), std::vector<std::string>{});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.writesFiles = [](const Json& args) { return args.contains("save_path"); };
        tool.handler = previewEffectTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "bake_particle_sheet";
        tool.title = "Bake Sprite Sheet";
        tool.description =
            "Bake an effect's run into a sprite sheet PNG (columns x rows frames, straight alpha) and a .particles playing it as a "
            "flipbook. Without output nothing is written: the sheet image. With output both files, one undo step. Check the "
            "effect with preview_particle_effect first.";
        tool.inputSchema = object(withRun({
            {"columns", withDefault(integer("", 1, 16), 4)},
            {"rows", withDefault(integer("", 1, 16), 4)},
            {"cell_size", withDefault(integer("Pixels.", 16, 1024), 128)},
            {"view", withDefault(stringEnum("pixel: the base-pixel grid (cells: base pixels x scale).", {"front", "three_quarter", "side", "top", "pixel"}), "front")},
            {"scale", integer("pixel view: screen pixels a base pixel.", 1, 8)},
            {"aseprite", withDefault(boolean("Also <output>.json, Aseprite json-hash."), false)},
            {"output", string("Project-relative, no extension: writes .png and .particles.")},
            {"overwrite", withDefault(boolean(""), false)},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 1, 3600), 120)}}), std::vector<std::string>{});
        tool.writesFiles = [](const Json& args) { return args.contains("output"); };
        tool.allowedWhilePlaying = false;
        tool.activity = ToolActivity::Edit;
        tool.attributesUndo = true;
        tool.handler = bakeSheetTool;
        registry.addTool(std::move(tool));
    }
}

}
