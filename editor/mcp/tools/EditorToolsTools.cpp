// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S27: the editor tools (docs/specs/S27-editor-tools.md R42), tool group "editor_tools"
//
//   record_game         the Recorder as a job: a PNG sequence or an MP4 / MOV of the Game view
//   sample_view_color   one pixel of the Game or Scene view: hex, sRGB, linear
//   edit_swatches       the Color window's swatch libraries (user and project)
//   list_curve_presets  the curve presets (built-in, user, project)
//   evaluate_curve      a curve, min-max curve or gradient property at given times
//
// list_project_templates and import_project are core tools (ProjectTools.cpp's neighbours here,
// registered by registerHubTools), and create_project takes a template. registerHubTools also has
// clear_project_leftovers (S66), which works over the hub and is in this group.

#include "McpTools.h"
#include "render/SceneRender.h"

#include "App.h"
#include "AppSettings.h"
#include "Catalog.h"
#include "Project.h"
#include "component/ParticleSystemComponent.h"
#include "hub/HubPaths.h"
#include "hub/ProjectImporter.h"
#include "hub/TemplateCatalog.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "math/CurveJson.h"
#include "particles/ParticleEffectCodec.h"
#include "recorder/RecorderSession.h"
#include "pool/TextureDataPool.h"
#include "render/SystemRender.h"
#include "stream/CurveCodec.h"
#include "util/FileUtils.h"
#include "util/GraphicUtils.h"
#include "util/ParticleEffectEdit.h"
#include "widget/ColorMath.h"
#include "widget/ColorPickerWindow.h"
#include "widget/CurveField.h"
#include "widget/CurvePresets.h"
#include "widget/SwatchLibrary.h"
#include "window/GameWindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- record_game

void recordGameTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    recorder::RecorderSession& session = recorder::RecorderSession::get();
    if (session.active()) {
        call->fail("A recording is running (the Recorder window or another record_game): cancel_job it or wait for it.");
        return;
    }
    recorder::Settings settings = ctx.app().getRecorderWindow().settings();
    const std::string format = args.value("format", std::string("png"));
    settings.kind = format == "png" ? recorder::Kind::ImageSequence : recorder::Kind::Movie;
    settings.format = format == "mov" ? recorder::Format::Mov : recorder::Format::Mp4;
    const std::string quality = args.value("quality", std::string("high"));
    settings.quality = quality == "low" ? recorder::Quality::Low : (quality == "medium" ? recorder::Quality::Medium : recorder::Quality::High);
    settings.frameRate = std::clamp(args.value("fps", 30), recorder::kMinFrameRate, recorder::kMaxFrameRate);
    settings.captureAudio = args.value("audio", false);
    settings.exitPlayMode = args.value("exit_play", true);
    settings.recordOnPlay = false;
    if (args.contains("width") || args.contains("height")) {
        settings.resolution = recorder::Resolution::Custom;
        settings.customWidth = std::clamp(args.value("width", 1920), recorder::kMinSize, recorder::kMaxSize);
        settings.customHeight = std::clamp(args.value("height", 1080), recorder::kMinSize, recorder::kMaxSize);
    }
    // the frames: an interval from start_frame (frames, or seconds at fps)
    int64_t frames = args.value("frames", (int64_t)0);
    if (frames <= 0 && args.contains("seconds")) frames = (int64_t)std::llround(args["seconds"].get<double>() * settings.frameRate);
    if (frames <= 0) {
        call->fail("frames (or seconds) is required: how much to record.");
        return;
    }
    settings.mode = recorder::Mode::FrameInterval;
    settings.frameStart = std::max(0, args.value("start_frame", 0));
    settings.frameEnd = settings.frameStart + (int)frames - 1;
    if (args.contains("output") && args["output"].is_string() && !args["output"].get<std::string>().empty()) {
        fs::path output = FileUtils::pathFromUtf8(args["output"].get<std::string>());
        if (!output.is_absolute()) output = ctx.project()->getProjectPath() / output;
        if (settings.kind == recorder::Kind::ImageSequence) {
            settings.outputFolder = FileUtils::pathToUtf8(output);
            settings.sequenceName = "frame_<Frame>";
        } else {
            settings.outputFolder = FileUtils::pathToUtf8(output.parent_path());
            settings.movieName = output.stem().string();
            if (output.extension() == ".mov") settings.format = recorder::Format::Mov;
        }
    }
    std::string error;
    if (!session.start(settings, error)) {
        call->fail(error + (settings.kind == recorder::Kind::Movie ? " (format \"png\" needs no FFmpeg)." : "."));
        return;
    }
    JobPtr job = ctx.jobs().create("record", "Record " + std::to_string(frames) + " frame(s) to " + format);
    job->setCancelHandler([]() { recorder::RecorderSession::get().stop(); });
    session.setOnFinished([job, settings](const recorder::Result& result) {
        Json data = result.sidecarJson();
        Json files = Json::array();
        for (const fs::path& file : result.files) files.push_back(FileUtils::pathToGenericUtf8(file));
        if (settings.kind == recorder::Kind::ImageSequence) data["files"] = files;
        else if (result.ok) data["movie"] = FileUtils::pathToGenericUtf8(result.output);
        data["sidecar"] = FileUtils::pathToGenericUtf8(result.sidecar);
        if (result.overriddenFixedFps > 0.0) data["overrode_fixed_fps"] = result.overriddenFixedFps;
        // (a failed write is a failure, stopped or not)
        if (job->cancelRequested() && result.failedStep.empty()) {
            job->markCancelled("Stopped after " + std::to_string(result.frames) + " frame(s); the output is finalised.");
            return;
        }
        if (result.ok) {
            job->succeed("Recorded " + std::to_string(result.frames) + " frames to " + FileUtils::pathToGenericUtf8(result.output) + ".", data);
            return;
        }
        std::string message = "The recording failed: " + result.error + ".";
        // (in the text: of an error, that is all a client like Claude Code shows the model)
        if (!result.failedStep.empty()) {
            message += " The output path holds nothing and Play is still running: stop it (play action stop), fix the cause, then record again.";
        }
        job->fail(message, data);
    });
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 300.0), [](const McpJob& done) {
        Json result = done.result();
        result["job"] = done.toJson(10);
        return done.status() == JobStatus::Failed ? ToolResult::error(done.message(), result) : ToolResult::success(done.message(), result);
    });
}

// ---------------------------------------------------------------- sample_view_color

void sampleViewColorNow(const CallPtr& call);

// the pixel of a frame drawn with every shader and pipeline created (S32) and every texture
// loaded: it waits for them as screenshot does (up to 30 s), then fails naming them
void sampleViewColorTool(const CallPtr& call) {
    auto idle = []() { return SystemRender::isGpuCreationIdle() && TextureDataPool::getLoadingCount() == 0; };
    if (idle()) {
        sampleViewColorNow(call);
        return;
    }
    // idle, then two more frames drawn with them (the first draw adopts a finished texture)
    call->waitUntil(idle, 30000, [call](bool timedOut) {
        if (timedOut) {
            std::string pending = SystemRender::isGpuCreationIdle() ? "" : "shaders or pipelines being made";
            if (const size_t textures = TextureDataPool::getLoadingCount()) {
                std::string list;
                for (const std::string& id : TextureDataPool::getLoadingIds(8)) list += (list.empty() ? "" : ", ") + id;
                pending += (pending.empty() ? "" : "; ") + std::to_string(textures) + " texture load(s): " + list;
            }
            call->fail("No colour sampled: still loading after 30 s (a stand-in is never sampled): " + pending + ". Retry.");
            return;
        }
        call->afterFrames(2, [call]() { sampleViewColorNow(call); });
    });
}

void sampleViewColorNow(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    const std::string view = args.value("view", std::string("game"));
    Framebuffer* framebuffer = nullptr;
    if (view == "game") {
        framebuffer = ctx.gameWindow() ? ctx.gameWindow()->getFramebuffer() : nullptr;
    } else {
        SceneProject* scene = ctx.project()->getSelectedScene();
        framebuffer = scene && scene->sceneRender ? scene->sceneRender->getFramebuffer() : nullptr;
    }
    if (!framebuffer || !framebuffer->isCreated()) {
        call->fail("The " + view + " view has drawn nothing yet (open it, or take a screenshot of it first).");
        return;
    }
    const int x = args.value("x", -1), y = args.value("y", -1);
    const int width = (int)framebuffer->getWidth(), height = (int)framebuffer->getHeight();
    if (x < 0 || y < 0 || x >= width || y >= height) {
        call->fail("x, y must be inside the view's image: 0-" + std::to_string(width - 1) + ", 0-" + std::to_string(height - 1) +
                   " (pixels from the top left, as screenshot images).");
        return;
    }
    std::vector<uint8_t> texel;
    if (!GraphicUtils::readFramebufferRect(framebuffer, x, y, 1, 1, texel) || texel.size() < 4) {
        call->fail("The view could not be read back.");
        return;
    }
    const Vector3 srgb(colormath::fromByte(texel[0]), colormath::fromByte(texel[1]), colormath::fromByte(texel[2]));
    const Vector3 linear = colormath::srgbToLinear(srgb);
    const std::string hex = "#" + colormath::toHex(Vector4(srgb, 1.0f), false);
    Json data = {{"hex", hex},
                 {"srgb", Json::array({srgb.x, srgb.y, srgb.z})},
                 {"linear", Json::array({linear.x, linear.y, linear.z})},
                 {"bytes", Json::array({texel[0], texel[1], texel[2], texel[3]})},
                 {"view", view},
                 {"size", {{"width", width}, {"height", height}}}};
    call->ok(view + " view pixel (" + std::to_string(x) + ", " + std::to_string(y) + "): " + hex + ".", data);
}

// ---------------------------------------------------------------- edit_swatches

Json swatchJson(const Swatch& s) {
    const Vector3 shown = colormath::linearToSrgb(Vector3(std::clamp(s.linear.x, 0.0f, 1.0f), std::clamp(s.linear.y, 0.0f, 1.0f),
                                                          std::clamp(s.linear.z, 0.0f, 1.0f)));
    return {{"name", s.name}, {"linear", Json::array({s.linear.x, s.linear.y, s.linear.z, s.linear.w})},
            {"hex", "#" + colormath::toHex(Vector4(shown, s.linear.w), true)}, {"hdr", s.hdr}};
}

Json libraryJson(const SwatchLibrary& library) {
    Json swatches = Json::array();
    for (const Swatch& s : library.swatches) swatches.push_back(swatchJson(s));
    Json gradients = Json::array();
    for (const GradientPreset& g : library.gradients) gradients.push_back({{"name", g.name}, {"gradient", curvecodec::gradientToJson(g.gradient)}});
    Json j = {{"name", library.name}, {"scope", library.scope == LibraryScope::Project ? "project" : "user"},
              {"path", FileUtils::pathToGenericUtf8(library.path)}, {"read_only", library.readOnly},
              {"swatches", swatches}, {"gradients", gradients}};
    if (!library.warning.empty()) j["warning"] = library.warning;
    return j;
}

// a swatch colour of the MCP forms: "#RRGGBB[AA]" or {"srgb": [...]} / {"linear": [...]} and "hdr"
bool swatchColor(const Json& value, Swatch& swatch, std::string& error) {
    Vector4 srgb;
    bool hasAlpha = false;
    if (value.is_string() && colormath::fromHex(value.get<std::string>(), srgb, &hasAlpha)) {
        swatch.linear = Vector4(colormath::srgbToLinear(Vector3(srgb.x, srgb.y, srgb.z)), srgb.w);
        return true;
    }
    if (value.is_object() && (value.contains("srgb") || value.contains("linear"))) {
        const Json& list = value.contains("srgb") ? value["srgb"] : value["linear"];
        if (list.is_array() && (list.size() == 3 || list.size() == 4)) {
            float c[4] = {0, 0, 0, 1};
            for (size_t i = 0; i < list.size(); i++) {
                if (!list[i].is_number()) {
                    error = "colour components are numbers";
                    return false;
                }
                c[i] = list[i].get<float>();
            }
            Vector3 rgb(c[0], c[1], c[2]);
            if (value.contains("srgb")) rgb = colormath::srgbToLinear(rgb);
            if (value.contains("intensity") && value["intensity"].is_number()) {
                const float scale = std::exp2(value["intensity"].get<float>());
                rgb = Vector3(rgb.x * scale, rgb.y * scale, rgb.z * scale);
            }
            swatch.linear = Vector4(rgb, c[3]);
            swatch.hdr = value.value("hdr", rgb.x > 1.0f || rgb.y > 1.0f || rgb.z > 1.0f);
            return true;
        }
    }
    error = "color is \"#RRGGBB[AA]\" (sRGB), {\"srgb\": [r,g,b(,a)]} or {\"linear\": [r,g,b(,a)]} (with \"intensity\" stops, \"hdr\")";
    return false;
}

void editSwatchesTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    ui::ColorPickerWindow& window = ui::ColorPickerWindow::get();
    window.setProjectPath(ctx.project()->isEditorTempProject() ? fs::path() : ctx.project()->getProjectPath());
    window.reloadLibraries();
    const std::string action = args.value("action", std::string("list"));
    const std::string scope = args.value("scope", std::string("user"));
    const fs::path directory = scope == "project" ? window.projectLibraryDirectory() : window.userLibraryDirectory();
    if (directory.empty()) {
        call->fail("The project is not saved: its swatch libraries have no folder yet (scope \"user\" works).");
        return;
    }
    if (action == "list") {
        Json libraries = Json::array();
        for (const SwatchLibrary& library : window.libraries()) libraries.push_back(libraryJson(library));
        call->ok(std::to_string(libraries.size()) + " swatch librar" + (libraries.size() == 1 ? "y." : "ies."), Json{{"libraries", libraries}});
        return;
    }
    const std::string name = args.value("library", std::string());
    if (name.empty()) {
        call->fail("library is required: the library's name (edit_swatches action \"list\" lists them).");
        return;
    }
    std::string error;
    if (action == "create_library") {
        SwatchLibrary created;
        if (!swatches::createLibrary(directory, scope == "project" ? LibraryScope::Project : LibraryScope::User, name, created, error)) {
            call->fail(error + ".");
            return;
        }
        window.reloadLibraries();
        call->ok("Created the swatch library \"" + name + "\".", libraryJson(created));
        return;
    }
    SwatchLibrary library;
    const fs::path file = swatches::libraryPath(directory, name);
    if (!swatches::read(file, scope == "project" ? LibraryScope::Project : LibraryScope::User, library, error)) {
        call->fail("No " + scope + " swatch library \"" + name + "\" (" + error + ").");
        return;
    }
    if (action == "delete_library") {
        if (!swatches::deleteLibrary(library, error)) {
            call->fail(error + ".");
            return;
        }
        window.reloadLibraries();
        call->ok("Deleted the swatch library \"" + name + "\".", Json{{"deleted", FileUtils::pathToGenericUtf8(file)}});
        return;
    }
    if (action == "add") {
        Swatch swatch;
        swatch.name = args.value("name", std::string());
        if (!args.contains("color") || !swatchColor(args["color"], swatch, error)) {
            call->fail(error.empty() ? std::string("color is required.") : error + ".");
            return;
        }
        if (!swatches::add(library, swatch, error) || !swatches::write(library, error)) {
            call->fail(error + ".");
            return;
        }
    } else if (action == "remove") {
        if (!args.contains("index") || !args["index"].is_number_integer()) {
            call->fail("index is required: the swatch's place in the library (0-based).");
            return;
        }
        if (!swatches::remove(library, (size_t)std::max<int64_t>(0, args["index"].get<int64_t>()), error) || !swatches::write(library, error)) {
            call->fail(error + ".");
            return;
        }
    } else {
        call->fail("action is list, add, remove, create_library or delete_library.");
        return;
    }
    window.reloadLibraries();
    call->ok("\"" + name + "\" has " + std::to_string(library.swatches.size()) + " swatch(es).", libraryJson(library));
}

// ---------------------------------------------------------------- curves

void listCurvePresetsTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    ui::CurveWindow& window = ui::CurveWindow::get();
    window.setProjectPath(ctx.project()->isEditorTempProject() ? fs::path() : ctx.project()->getProjectPath());
    window.reloadLibraries();
    Json libraries = Json::array();
    size_t count = 0;
    for (const CurvePresetLibrary& library : window.libraries()) {
        Json presets = Json::array();
        for (const CurvePreset& preset : library.presets) presets.push_back({{"name", preset.name}, {"curve", curvecodec::curveToJson(preset.curve)}});
        count += library.presets.size();
        const char* scope = library.scope == CurveLibraryScope::BuiltIn ? "built_in" : (library.scope == CurveLibraryScope::Project ? "project" : "user");
        libraries.push_back({{"name", library.name}, {"scope", scope}, {"path", FileUtils::pathToGenericUtf8(library.path)},
                             {"read_only", library.readOnly}, {"presets", presets}});
    }
    call->ok(std::to_string(count) + " curve presets over [0,1]x[0,1] (a field maps them onto its range).", Json{{"libraries", libraries}});
}

void evaluateCurveTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    std::string error;
    SceneProject* scene = McpRefs::resolveScene(ctx.project(), args.value("scene", Json()), error);
    if (!scene) {
        call->fail(error);
        return;
    }
    const Entity entity = McpRefs::resolveEntity(scene, args.value("entity", Json()), error);
    if (entity == NULL_ENTITY) {
        call->fail(error);
        return;
    }
    ComponentType component;
    if (!McpRefs::resolveComponent(args.value("component", Json()), component, error)) {
        call->fail(error);
        return;
    }
    const std::string property = args.value("property", std::string());
    if (!args.contains("times") || !args["times"].is_array() || args["times"].empty()) {
        call->fail("times is required: an array of the times to evaluate at.");
        return;
    }
    const float random = std::clamp(args.value("random", 0.0f), 0.0f, 1.0f);
    std::vector<float> times;
    for (const Json& t : args["times"]) {
        if (!t.is_number()) {
            call->fail("times holds numbers.");
            return;
        }
        times.push_back(t.get<float>());
    }
    Json values = Json::array();
    std::string kind;
    // a component's Curve property
    const PropertyData data = Catalog::findProperty(scene->scene, entity, component, property);
    if (data.ref && data.type == PropertyType::Curve) {
        const Curve& curve = *static_cast<const Curve*>(data.ref);
        for (float t : times) values.push_back(curve.evaluate(t));
        kind = "curve";
    } else if (component == ComponentType::ParticleSystemComponent) {
        // a particle effect field by its keyed path ("sizeOverLifetime.size", "main.startColor")
        const ParticleSystemComponent* particles = scene->scene->findComponent<ParticleSystemComponent>(entity);
        particles::ParticleEffectDocument document;
        if (!particles || !ParticleEffectEdit::read(*particles, document, nullptr)) {
            call->fail("The GameObject's effect cannot be read (get_particle_effect shows why).");
            return;
        }
        nlohmann::ordered_json node = particles::ParticleEffectCodec::toKeyedJson(document);
        if (node.contains("modules")) {
            nlohmann::ordered_json flat = node;
            for (auto it = node["modules"].begin(); it != node["modules"].end(); ++it) flat[it.key()] = *it;
            node = flat;
        }
        size_t start = 0;
        while (start <= property.size()) {
            const size_t dot = property.find('.', start);
            const std::string part = property.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (!node.is_object() || !node.contains(part)) {
                call->fail("No effect field \"" + property + "\" (get_particle_effect lists the modules and their fields; unset fields keep "
                           "their defaults and are not listed).");
                return;
            }
            node = node[part];
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        const Json value = Json::parse(node.dump());
        std::string why;
        particles::MinMaxCurve minMax;
        particles::MinMaxGradient minMaxGradient;
        Curve curve;
        Gradient gradient;
        if (curvecodec::curveFromJson(value, curve, why)) {
            for (float t : times) values.push_back(curve.evaluate(t));
            kind = "curve";
        } else if (curvecodec::gradientFromJson(value, gradient, why)) {
            for (float t : times) {
                const Vector4 c = gradient.evaluate(t);
                values.push_back(Json::array({c.x, c.y, c.z, c.w}));
            }
            kind = "gradient";
        } else if (curvecodec::minMaxCurveFromJson(value, minMax, why)) {
            for (float t : times) values.push_back(minMax.evaluate(t, random));
            kind = "min_max_curve";
        } else if (curvecodec::minMaxGradientFromJson(value, minMaxGradient, why)) {
            for (float t : times) {
                const Vector4 c = minMaxGradient.evaluate(t, random);
                values.push_back(Json::array({c.x, c.y, c.z, c.w}));
            }
            kind = "min_max_gradient";
        } else {
            call->fail("\"" + property + "\" is not a curve, gradient, min-max curve or min-max gradient.");
            return;
        }
    } else {
        call->fail("\"" + property + "\" is not a curve property of " + McpRefs::componentName(component) +
                   " (curves: TweenComponent.curve, ForceFieldComponent.falloff; particle effect fields with component \"particlesystem\").");
        return;
    }
    call->ok(std::to_string(values.size()) + " value(s) of the " + kind + ".", Json{{"kind", kind}, {"times", args["times"]}, {"values", values}});
}

// ---------------------------------------------------------------- project templates and imports (core)

void listProjectTemplatesTool(const CallPtr& call) {
    std::vector<std::string> errors;
    const std::vector<hub::ProjectTemplate> list =
        hub::templates::all(hub::templates::userDirectory(AppSettings::getConfigDirectory()), &errors);
    Json templates = Json::array();
    for (const hub::ProjectTemplate& t : list) {
        Json entry = {{"id", t.id}, {"name", t.name}, {"description", t.description}, {"category", t.category},
                      {"built_in", t.builtIn}, {"start_scene", t.startScene}};
        if (!t.builtIn) entry["path"] = FileUtils::pathToGenericUtf8(t.directory);
        templates.push_back(std::move(entry));
    }
    Json data = {{"templates", templates}};
    if (!errors.empty()) data["errors"] = errors;
    call->ok(std::to_string(templates.size()) + " project template(s): create_project {template} makes a project from one.", data);
}

// A staging folder nothing removes by itself: the agent decides, as the Hub's user does (S66 R3)
Json leftoversJson(const std::vector<hub::staging::Leftover>& leftovers) {
    Json list = Json::array();
    for (const hub::staging::Leftover& leftover : leftovers) {
        list.push_back(Json{{"path", FileUtils::pathToGenericUtf8(leftover.folder)}, {"reason", leftover.reason}});
    }
    return list;
}

hub::staging::Scope leftoverScopeWith(const std::vector<fs::path>& folders) {
    hub::staging::Scope scope = ProjectHubWindow::leftoverScope();
    scope.parents.insert(scope.parents.end(), folders.begin(), folders.end());
    return scope;
}

// import_project's and create_project's results: the leftovers beside the new project and known to the hub
void reportLeftovers(const fs::path& parent, std::string& summary, Json& data) {
    const std::vector<hub::staging::Leftover> leftovers = hub::staging::inspect(leftoverScopeWith({parent})).leftovers;
    if (leftovers.empty()) return;
    // the paths in the text too: a client may show the model the text alone
    summary += " Kept as leftover(s):";
    for (const hub::staging::Leftover& leftover : leftovers) summary += " " + FileUtils::pathToGenericUtf8(leftover.folder);
    summary += ".";
    data["next"] = "Leftovers may hold the user's files: tell the user. Once they agree, clear_project_leftovers {remove} (tool_groups: editor_tools) deletes one.";
    data["leftovers"] = leftoversJson(leftovers);
}

void clearProjectLeftoversTool(const CallPtr& call) {
    const Json& args = call->args();
    std::vector<fs::path> requested;
    std::vector<fs::path> parents;
    if (args.contains("remove")) {
        for (const Json& path : args["remove"]) {
            const fs::path folder = FileUtils::pathFromUtf8(path.get<std::string>()).lexically_normal();
            if (!folder.is_absolute()) {
                call->fail("remove takes absolute paths: " + path.get<std::string>() + ".");
                return;
            }
            requested.push_back(folder);
            parents.push_back(folder.parent_path());
        }
    }
    App& app = call->ctx().app();
    hub::staging::Scope scope = leftoverScopeWith(parents);
    if (const Project* project = call->ctx().project(); project && !project->getProjectPath().empty()) scope.projects.push_back(project->getProjectPath());
    // a removal can take long: off the main thread, as the copies that made these folders
    auto work = std::make_shared<std::future<Json>>(std::async(std::launch::async, [scope, requested]() {
        hub::staging::Sweep swept = hub::staging::sweep(scope);
        Json removed = Json::array();
        for (const fs::path& folder : swept.removed) removed.push_back(FileUtils::pathToGenericUtf8(folder));
        Json failed = Json::array();
        for (const fs::path& folder : swept.failed) {
            failed.push_back(Json{{"path", FileUtils::pathToGenericUtf8(folder)}, {"error", "the removal failed; it is retried at the next sweep"}});
        }
        bool removedAny = false;
        for (const fs::path& folder : requested) {
            // its owner had died: the sweep has just removed it, as asked
            if (std::find(swept.removed.begin(), swept.removed.end(), folder) != swept.removed.end()) continue;
            const auto listed = std::find_if(swept.leftovers.begin(), swept.leftovers.end(), [&](const hub::staging::Leftover& l) { return l.folder == folder; });
            std::string error;
            if (listed == swept.leftovers.end()) {
                error = "not a leftover (see leftovers and in_use)";
            } else if (hub::staging::removeLeftover(folder, error)) {
                removed.push_back(FileUtils::pathToGenericUtf8(folder));
                removedAny = true;
                continue;
            }
            failed.push_back(Json{{"path", FileUtils::pathToGenericUtf8(folder)}, {"error", error}});
        }
        if (removedAny) swept = hub::staging::inspect(scope);
        Json inUse = Json::array();
        for (const fs::path& folder : swept.live) inUse.push_back(FileUtils::pathToGenericUtf8(folder));
        return Json{{"removed", removed}, {"leftovers", leftoversJson(swept.leftovers)}, {"in_use", inUse}, {"failed", failed}};
    }));
    App* appPtr = &app;
    call->waitUntil([work]() { return work->wait_for(std::chrono::seconds(0)) == std::future_status::ready; }, 600000,
                    [call, work, appPtr, count = requested.size()](bool timedOut) {
        if (timedOut) {
            call->fail("The removal did not finish in 10 minutes; it goes on: call again to see what is left.");
            return;
        }
        const Json data = work->get();
        appPtr->refreshHubLeftovers();
        const std::string summary = std::to_string(data["removed"].size()) + " staging folder(s) removed, " + std::to_string(data["leftovers"].size()) +
                                    " leftover(s) kept, " + std::to_string(data["in_use"].size()) + " in use by a running editor.";
        if (count > 0 && !data["failed"].empty()) {
            call->fail(summary + " Not removed: " + data["failed"].dump() + ".", data);
            return;
        }
        call->ok(summary, data);
    });
}

void importProjectTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    App& app = ctx.app();
    const Json& args = call->args();
    const std::string source = args.value("source", std::string());
    const fs::path parent = FileUtils::pathFromUtf8(args.value("path", std::string()));
    if (source.empty() || parent.empty() || !parent.is_absolute()) {
        call->fail("source (a .zip path or a Git URL) and path (the absolute folder to import into) are required.");
        return;
    }
    std::string why;
    if (!hub::ProjectImporter::isZipSource(source) && !hub::isAcceptedGitUrl(source, why)) {
        call->fail("source is a .zip file or a Git URL (https://, ssh://, user@host:path): " + why + ".");
        return;
    }
    if (app.isProjectLoading()) {
        call->fail("A project is loading; try again in a moment.");
        return;
    }
    auto importer = std::make_shared<hub::ProjectImporter>();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (!importer->start(source, parent, AppSettings::getVersionControlSettings().gitPath, why)) {
        call->fail(why + ".");
        return;
    }
    const double timeout = std::clamp(args.value("timeout_seconds", 300.0), 5.0, 3600.0);
    App* appPtr = &app;
    auto opened = std::make_shared<bool>(false);
    call->waitUntil([importer, appPtr, opened]() {
        if (!importer->done()) return false;
        if (!importer->succeeded()) return true;
        if (!*opened) {
            *opened = true;
            const fs::path dir = importer->projectDir();
            appPtr->requestProjectChange([appPtr, dir]() { appPtr->getProject()->loadProject(dir); });
            return false;
        }
        return !appPtr->isProjectLoading();
    }, (int)(timeout * 1000.0), [call, importer, parent](bool timedOut) {
        if (timedOut) {
            importer->cancel();
            call->fail("The import did not finish in time; it was cancelled (nothing is left behind).");
            return;
        }
        Json log = Json::array();
        for (const std::string& line : importer->logTail(20)) log.push_back(line);
        if (!importer->succeeded()) {
            std::string summary = "The import failed: " + importer->error() + ".";
            Json data = {{"log", log}};
            reportLeftovers(parent, summary, data);
            call->fail(summary, data);
            return;
        }
        Project* project = call->ctx().project();
        const fs::path dir = importer->projectDir();
        std::error_code same;
        const bool open = fs::equivalent(project->getProjectPath(), dir, same);
        std::string summary = std::string("Imported ") + FileUtils::pathToGenericUtf8(dir) + (open ? " and opened it." : "; it did not open: see read_console.");
        Json data = {{"path", FileUtils::pathToGenericUtf8(dir)}, {"opened", open}, {"log", log}};
        reportLeftovers(parent, summary, data);
        call->ok(summary, data);
    });
}

} // namespace

// the template branch of create_project (ProjectTools.cpp): true when it took the call
bool createProjectFromTemplateTool(const CallPtr& call, const fs::path& path, const std::string& name) {
    const Json& args = call->args();
    if (!args.contains("template") || !args["template"].is_string()) return false;
    const std::string templateId = args["template"].get<std::string>();
    App& app = call->ctx().app();
    std::string error;
    if (!app.createProjectFromTemplate(templateId, name, path, error)) {
        call->fail("Could not create the project: " + error + ". list_project_templates lists the templates.");
        return true;
    }
    const double timeout = std::clamp(args.value("timeout_seconds", 180.0), 5.0, 600.0);
    App* appPtr = &app;
    call->waitUntil([appPtr]() { return !appPtr->isCreatingFromTemplate() && !appPtr->isProjectLoading(); }, (int)(timeout * 1000.0),
                    [call, path, appPtr, templateId](bool timedOut) {
        if (timedOut) {
            call->fail("The project is still being created or loading; check editor_status in a moment.");
            return;
        }
        if (!appPtr->templateCreationError().empty()) {
            call->fail("Could not create the project: " + appPtr->templateCreationError() + ".");
            return;
        }
        Project* project = call->ctx().project();
        std::error_code ec;
        if (!fs::equivalent(project->getProjectPath(), path, ec)) {
            call->fail("The project was made in " + FileUtils::pathToGenericUtf8(path) + " but did not open: see read_console.");
            return;
        }
        Json scenes = Json::array();
        for (const SceneProject& scene : project->getScenes()) scenes.push_back(McpRefs::sceneJson(project, scene));
        std::string summary = "Created " + FileUtils::pathToGenericUtf8(path) + " from the template \"" + templateId + "\" and opened it.";
        Json data = {{"path", FileUtils::pathToGenericUtf8(path)}, {"name", project->getName()}, {"template", templateId}, {"scenes", scenes}};
        reportLeftovers(path.parent_path(), summary, data);
        call->ok(summary, data);
    });
    return true;
}

void registerEditorToolsTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "record_game";
        tool.title = "Record Game";
        tool.description =
            "The Recorder as a job: enters Play when needed and records frames start_frame.. of the Game view at a fixed fps "
            "(game time k/fps per frame, deterministic) to a PNG sequence or an MP4 (H.264) / MOV (ProRes) through the user's FFmpeg. "
            "Returns the sidecar (frames, fps, size, encoder, warnings, wallSeconds) and the files or movie. cancel_job stops and finalises.";
        tool.inputSchema = object({
            {"frames", integer("Frames to record.", 1, 100000)},
            {"seconds", number("Or game seconds (frames = seconds x fps).")},
            {"start_frame", withDefault(integer("Frame 0: Play's first.", 0, 1000000), 0)},
            {"fps", withDefault(integer("", 1, 240), 30)},
            {"width", integer("Default: the Recorder's resolution.", 16, 8192)},
            {"height", integer("", 16, 8192)},
            {"format", withDefault(stringEnum("", {"png", "mp4", "mov"}), "png")},
            {"quality", withDefault(stringEnum("", {"low", "medium", "high"}), "high")},
            {"audio", withDefault(boolean("Movies; mixed offline from Play's start."), false)},
            {"output", string("png: a folder; movie: a file. Default Recordings/.")},
            {"exit_play", withDefault(boolean(""), true)},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number(""), 300)},
        });
        tool.annotations.openWorld = true;
        tool.activity = ToolActivity::Control;
        tool.handler = recordGameTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "sample_view_color";
        tool.title = "Sample View Color";
        tool.description =
            "The eyedropper's exact read: the Game or Scene view's pixel at x, y (pixels from the top left, as screenshot) as "
            "{hex, srgb, linear}.";
        tool.inputSchema = object({
            {"view", withDefault(stringEnum("", {"game", "scene"}), "game")},
            {"x", integer("", 0, 16384)},
            {"y", integer("", 0, 16384)},
        }, {"x", "y"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = sampleViewColorTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_swatches";
        tool.title = "Edit Swatches";
        tool.description =
            "The Color window's swatch libraries (*.colors): list, add {color, name}, remove {index}, create_library, delete_library, "
            "in scope user (settings folder) or project (EditorSettings/Swatches). Colours: \"#RRGGBB[AA]\", {srgb} or {linear}.";
        tool.inputSchema = object({
            {"action", withDefault(stringEnum("", {"list", "add", "remove", "create_library", "delete_library"}), "list")},
            {"scope", withDefault(stringEnum("", {"user", "project"}), "user")},
            {"library", string("")},
            {"name", string("The swatch's name (add).")},
            {"color", Json{{"description", "\"#RRGGBB[AA]\", {\"srgb\": [r,g,b,a]} or {\"linear\": [...], \"intensity\": stops}"}}},
            {"index", integer("0-based (remove).", 0, 75)},
        });
        tool.handler = editSwatchesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "list_curve_presets";
        tool.title = "List Curve Presets";
        tool.description = "The curve presets: built-in, the user's and the project's (*.curves), each curve over [0,1]x[0,1] in curve JSON.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listCurvePresetsTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "evaluate_curve";
        tool.title = "Evaluate Curve";
        tool.description =
            "A curve property (TweenComponent.curve, ForceFieldComponent.falloff) or a particle effect field (component particlesystem, "
            "property \"sizeOverLifetime.size\") evaluated at times, as the runtime does; min-max values take `random` (0-1).";
        tool.inputSchema = object({
            {"scene", Json{{"description", "Default: the selected scene."}}},
            {"entity", Json{{"description", "id, path or name."}}},
            {"component", string("")},
            {"property", string("")},
            {"times", Json{{"type", "array"}, {"items", {{"type", "number"}}}}},
            {"random", withDefault(number("", 0, 1), 0)},
        }, {"entity", "component", "property", "times"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = evaluateCurveTool;
        registry.addTool(std::move(tool));
    }
}

void registerHubTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "list_project_templates";
        tool.title = "List Project Templates";
        tool.description = "The Project Hub's templates (built-in: empty, 3d, 2d, ui; and the user's), for create_project {template}.";
        tool.inputSchema = object({});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = listProjectTemplatesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "import_project";
        tool.title = "Import Project";
        tool.description =
            "The Project Hub's Import: a project from a .zip or a Git URL (https://, ssh://, user@host:path; git clone --depth 1, "
            "LFS pulled) into the folder `path`, then opens it. A failure leaves nothing behind.";
        tool.inputSchema = object({
            {"source", string("A .zip path or a Git URL.")},
            {"path", string("Absolute folder the project goes into.")},
            {"timeout_seconds", withDefault(number(""), 300)},
        }, {"source", "path"});
        tool.annotations.destructive = true;
        tool.annotations.openWorld = true;
        tool.activity = ToolActivity::ProjectSwitch;
        tool.allowedWhilePlaying = false;
        tool.handler = importProjectTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "clear_project_leftovers";
        tool.title = "Clear Project Leftovers";
        tool.group = "editor_tools";
        tool.description =
            "Removes what stopped imports and template copies left behind in their <name>.importing folders, beside known projects "
            "and in the parents of `remove`. A folder that may hold a user's files stays until `remove` names it: ask the user first.";
        tool.inputSchema = object({
            {"remove", array(string(""), "Absolute paths of listed leftovers to delete.")},
        });
        tool.annotations.destructive = true;
        tool.activity = ToolActivity::Control;
        tool.handler = clearProjectLeftoversTool;
        registry.addTool(std::move(tool));
    }
}

} // namespace jasperengine::editor::mcp
