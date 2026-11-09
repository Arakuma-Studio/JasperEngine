// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtToolsRuntime.h"

#include "Project.h"
#include "component/TitleArtRendererComponent.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/tools/TitleArtToolsShared.h"
#include "subsystem/TitleArtSystem.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtOverrides.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleParticlePresets.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp::titletools {

namespace {

    const char* modeName(TitleArtRenderMode mode){
        switch (mode){
            case TitleArtRenderMode::BAKED: return "baked";
            case TitleArtRenderMode::REALTIME: return "realtime";
            case TitleArtRenderMode::GLYPH_ATLAS: return "glyph_atlas";
        }
        return "baked";
    }

    struct Instance {
        SceneProject* scene = nullptr;
        Entity entity = NULL_ENTITY;
        TitleArtRendererComponent* title = nullptr;
    };

    // `entity` vs `path`: false with the call answered when the arguments are not an instance's
    bool resolveInstance(const CallPtr& call, Instance& out, bool& handled){
        const Json& args = call->args();
        const bool hasEntity = args.contains("entity") && !args["entity"].is_null();
        const bool hasPath = args.contains("path") && !args["path"].is_null() && !(args["path"].is_string() && args["path"].get<std::string>().empty());
        handled = true;
        if (hasEntity && hasPath){
            call->fail("bad_args: give `path` (a .titleart) or `entity` (a GameObject with a Title Art Renderer), not both.",
                       Json{{"kind", "bad_args"}, {"fix", "drop path to read the instance, or entity to read the document"}});
            return false;
        }
        if (!hasEntity && !hasPath){
            call->fail("bad_args: `path` (a .titleart) or `entity` (a GameObject with a Title Art Renderer) is required.",
                       Json{{"kind", "bad_args"}, {"fix", "pass path: \"assets/titles/logo.titleart\" or entity: \"Canvas/Logo\""}});
            return false;
        }
        if (!hasEntity){
            handled = false;
            return false;
        }
        Project* project = call->ctx().project();
        std::string error;
        SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!scene || !scene->scene){
            call->fail(error.empty() ? "The scene is not loaded." : error);
            return false;
        }
        // in Play, the running game's copy
        scene = McpRefs::runtimeScene(project, scene);
        const Entity entity = McpRefs::resolveEntity(scene, args["entity"], error);
        if (entity == NULL_ENTITY){
            call->fail(error);
            return false;
        }
        TitleArtRendererComponent* title = scene->scene->isComponentRegistered<TitleArtRendererComponent>()
                                           ? scene->scene->findComponent<TitleArtRendererComponent>(entity) : nullptr;
        if (!title){
            call->fail("\"" + scene->scene->getEntityName(entity) + "\" has no Title Art Renderer: add_component {component: \"titleart\"} "
                       "adds one (with an Image or a Sprite).");
            return false;
        }
        out.scene = scene;
        out.entity = entity;
        out.title = title;
        return true;
    }

    std::string hex(uint64_t value){
        char text[17];
        std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)value);
        return text;
    }

    Json valueJson(const TitleArtValue& value){
        switch (value.kind){
            case TitleArtValue::Kind::Number: return Json(value.number);
            case TitleArtValue::Kind::Bool: return Json(value.boolean);
            case TitleArtValue::Kind::String: return Json(value.text);
            case TitleArtValue::Kind::Color: {
                TitleArtOverride entry;
                entry.type = "color";
                entry.value = value.color;
                return titleArtOverrideToJson(entry);
            }
        }
        return nullptr;
    }

    // §6.7
    Json instanceJson(Project* project, const Instance& in){
        Scene* scene = in.scene->scene;
        TitleArtRendererComponent& title = *in.title;
        titleart::TitleArtInstance& instance = TitleArtSystem::instanceOf(title);
        const std::vector<std::string> names = TitleArtSystem::parameterNames(scene, in.entity);
        Json out = Json::object();
        out["entity"] = McpRefs::entityPath(in.scene, in.entity);
        out["title"] = title.title.path;
        out["mode"] = modeName(title.renderMode);
        const titleart::TitleArtSpace space = TitleArtSystem::spaceOf(scene, in.entity);
        out["space"] = space == titleart::TitleArtSpace::UI ? "ui" : space == titleart::TitleArtSpace::World ? "world" : "none";
        out["ready"] = instance.ready;
        out["pending"] = instance.pending;
        out["state_hash"] = hex(instance.liveStatus.stateHash);
        out["pixel_path"] = instance.pixelPath;
        out["failed"] = instance.failed;
        out["error"] = instance.error;
        Json overrides = Json::object();
        for (const TitleArtOverride& entry : title.overrides) overrides[entry.name] = titleArtOverrideToJson(entry);
        out["overrides"] = overrides;
        Json effective = Json::object();
        for (const std::string& name : names){
            TitleArtValue value;
            std::string error;
            if (TitleArtSystem::getParameter(scene, in.entity, name, value, error)) effective[name] = valueJson(value);
        }
        out["effective"] = effective;
        const titleart::TitleArtLiveStatus& s = instance.liveStatus;
        if (title.renderMode == TitleArtRenderMode::BAKED){
            out["display"] = Json{{"size", Json::array({instance.bake.frame.width, instance.bake.frame.height})},
                                  {"bytes", (size_t)instance.bake.frame.width * instance.bake.frame.height * 4}};
        }else{
            out["display"] = Json{{"size", Json::array({s.displayWidth, s.displayHeight})}, {"bytes", s.displayBytes}};
        }
        out["pool_bytes"] = titleart::TitleArtScheduler::get().poolBytes();
        out["last_render"] = s.lastFrames > 0 ? Json{{"frames", s.lastFrames}, {"gpu_ms_est", s.lastGpuMsEstimate}, {"gpu_ms", nullptr},
                                                     {"cpu_ms", s.lastCpuMs}, {"over_budget_steps", s.overBudgetSteps}} : Json(nullptr);
        out["animation"] = instance.animation.baseAnimation() ? Json(instance.animation.baseAnimation()->animation->name) : Json();
        out["animation_time"] = title.animationTime;
        out["live"] = Json{{"sweep", title.sweepEnabled}, {"glow", title.glowEnabled}, {"tint", title.tintEnabled}, {"glints", title.glintsEnabled},
                           {"live_time", title.liveTime}, {"anchors", instance.anchors.size()},
                           // R9: full renders and live composes of Exposed effect parameters, and why the last live change re-rendered
                           {"renders", s.renders}, {"composes", s.liveComposes}, {"fallback", s.liveFallback}, {"input_bytes", s.liveBytes}};
        Json missing = Json::array();
        if (!s.missingCharacters.empty()) missing.push_back(s.missingCharacters);
        out["characters"] = Json{{"missing", missing}};
        if (title.renderMode == TitleArtRenderMode::GLYPH_ATLAS){
            out["glyph_atlas"] = Json{{"unsafe", s.glyphUnsafe}, {"approximate", s.glyphApproximate}, {"fallback", s.glyphFallback},
                                      {"pages", s.atlasPages}};
        }else{
            out["glyph_atlas"] = nullptr;
        }
        // R71: the four presets as ready-to-call create_particles arguments
        Json particles = Json::array();
        const TitleParticleFrame frame = titleParticleFrame(project, in.scene->id, in.entity);
        for (TitleParticlePreset& preset : titleParticlePresets(frame)){
            Json args = preset.args;
            args["parent"] = McpRefs::entityPath(in.scene, in.entity);
            particles.push_back(Json{{"label", preset.label}, {"call", Json{{"tool", "create_particles"}, {"args", args}}}});
        }
        out["particles"] = particles;
        out["module"] = Json{{"built", titleart::TitleArtRuntime::isBuilt()}};
        return out;
    }

    std::string summary(const Json& state){
        return state.value("entity", std::string()) + ": " + state.value("title", std::string()) + " (" + state.value("mode", std::string()) + "), " +
               (state.value("ready", false) ? std::string("ready") : "pending " + state.value("pending", std::string())) +
               (state.value("failed", false) ? ", failed: " + state.value("error", std::string()) : std::string());
    }

    // waits for the instance's image of its current state (a job past the first frame)
    void waitForReady(const CallPtr& call, const Instance& in, double timeoutSeconds, const std::string& kind){
        Project* project = call->ctx().project();
        const uint32_t sceneId = in.scene->id;
        const Entity entity = in.entity;
        auto task = std::make_shared<FrameTask>();
        const auto start = std::chrono::steady_clock::now();
        task->tick = [project, sceneId, entity, timeoutSeconds, start, task](McpJob& job) {
            SceneProject* scene = project->getScene(sceneId);
            TitleArtRendererComponent* title = scene && scene->scene && scene->scene->isEntityCreated(entity)
                                               ? scene->scene->findComponent<TitleArtRendererComponent>(entity) : nullptr;
            if (!title){
                job.fail("The Title Art Renderer is gone.");
                return true;
            }
            Instance in{scene, entity, title};
            const titleart::TitleArtInstance& instance = TitleArtSystem::instanceOf(*title);
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (instance.ready || instance.failed || elapsed >= timeoutSeconds){
                Json state = instanceJson(project, in);
                if (!instance.ready && !instance.failed) state["timed_out"] = true;
                task->text = summary(state);
                job.succeed(task->text, state);
                return true;
            }
            job.setProgress(-1.0, instance.pending);
            return false;
        };
        runFrameTask(call, kind, "Title Art instance", task);
    }

}

bool getTitleInstance(const CallPtr& call){
    Instance in;
    bool handled = false;
    if (!resolveInstance(call, in, handled)) return handled;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    TitleArtSystem::parameterNames(in.scene->scene, in.entity);
    const bool waitReady = args.value("wait_ready", false);
    if (waitReady && !TitleArtSystem::instanceOf(*in.title).ready && in.title->renderMode != TitleArtRenderMode::BAKED){
        waitForReady(call, in, std::clamp(args.value("timeout_seconds", 10.0), 0.0, 120.0), "get_title");
        return true;
    }
    const Json state = instanceJson(project, in);
    call->ok(summary(state), state);
    return true;
}

bool renderTitleInstance(const CallPtr& call){
    Instance in;
    bool handled = false;
    if (!resolveInstance(call, in, handled)) return handled;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    const Json options = args.contains("options") && args["options"].is_object() ? args["options"] : Json::object();
    for (auto it = options.begin(); it != options.end(); ++it){
        if (it.key() != "overrides" && it.key() != "rerender" && it.key() != "live_time" && it.key() != "cost"){
            call->fail("bad_option: render_title {entity} takes options overrides, rerender, live_time and cost; '" + it.key() + "' is not one.");
            return true;
        }
    }
    titleart::TitleArtInstance& instance = TitleArtSystem::instanceOf(*in.title);
    if (options.value("rerender", false)){
        // R70: the live instance renders again (no file write, no undo) and the call returns once it is ready
        TitleArtSystem::refresh(in.scene->scene, in.entity);
        if (in.title->renderMode == TitleArtRenderMode::BAKED){
            const Json state = instanceJson(project, in);
            call->ok(summary(state) + " (Baked: nothing to render again)", state);
            return true;
        }
        waitForReady(call, in, std::clamp(args.value("timeout_seconds", 120.0), 0.0, 600.0), "render_title");
        return true;
    }

    // the state the game shows, or a what-if (nothing saved), rendered in the editor
    fs::path file;
    std::string relative, error;
    if (!resolveTitlePath(project, in.title->title.path, true, file, relative, error)){
        call->fail(error);
        return true;
    }
    std::shared_ptr<TitleArtDocument> document = openTitle(project, file, error);
    if (!document){
        call->fail(error);
        return true;
    }
    std::vector<TitleArtOverride> overrides = in.title->renderMode == TitleArtRenderMode::BAKED ? std::vector<TitleArtOverride>() : in.title->overrides;
    if (options.contains("overrides")){
        if (!options["overrides"].is_object()){
            call->fail("bad_option: options.overrides is an object of exposed names and values ({text: \"WIN\"}).");
            return true;
        }
        std::vector<titleart::TitleArtExposedParam> exposed;
        titleart::TitleArtRuntime::readExposed(in.title->title.path, exposed, &error);
        for (auto it = options["overrides"].begin(); it != options["overrides"].end(); ++it){
            const titleart::TitleArtExposedParam* param = nullptr;
            for (const titleart::TitleArtExposedParam& p : exposed) if (p.name == it.key()) param = &p;
            if (!param){
                std::string list;
                for (const titleart::TitleArtExposedParam& p : exposed) list += (list.empty() ? "" : ", ") + p.name;
                call->fail("bad_option: options.overrides." + it.key() + ": the title exposes no such parameter. Exposed: " + (list.empty() ? "none" : list) + ".");
                return true;
            }
            TitleArtOverride entry;
            entry.name = it.key();
            if (!titleArtOverrideFromJson(it.value(), param->type, entry, error)){
                call->fail("bad_option: options.overrides." + error);
                return true;
            }
            overrides.erase(std::remove_if(overrides.begin(), overrides.end(), [&](const TitleArtOverride& o){ return o.name == entry.name; }), overrides.end());
            overrides.push_back(entry);
        }
    }
    auto doc = std::make_shared<titleart::TitleArtDoc>(document->doc());
    titleart::applyFieldOverrides(*doc, titleart::titleArtFieldOverrides(*doc, overrides));
    RenderSpec spec;
    spec.file = file;
    spec.relative = relative;
    spec.doc = doc;
    spec.rev = document->revision();
    spec.current = false;
    spec.args = args;
    spec.args.erase("entity");
    spec.args.erase("scene");
    spec.args.erase("options");
    spec.args["path"] = relative;
    // the frame the companion shows (the bake's), unless a crop was asked
    if (!spec.args.contains("crop") && instance.bake.frame.width > 0){
        const TitleArtRectI& f = instance.bake.frame;
        spec.args["crop"] = Json::array({f.x, f.y, f.width, f.height});
    }
    Json extra = Json{{"entity", McpRefs::entityPath(in.scene, in.entity)}, {"mode", modeName(in.title->renderMode)},
                      {"live_time", options.value("live_time", (double)in.title->liveTime)}, {"live_layers_composited", false}};
    if (options.contains("cost")){
        // S83 R46 with entity: the instance's measured runtime cost (estimates where no timer runs)
        const titleart::TitleArtLiveStatus& s = instance.liveStatus;
        extra["runtime_cost"] = Json{{"frames_to_ready", s.lastFrames}, {"gpu_ms", s.lastGpuMsEstimate}, {"gpu_ms_measured", false},
                                     {"cpu_ms", s.lastCpuMs}, {"over_budget_steps", s.overBudgetSteps}, {"pool_bytes", titleart::TitleArtScheduler::get().poolBytes()},
                                     {"display_bytes", s.displayBytes}};
    }
    startRender(call, std::move(spec), extra, McpRefs::entityPath(in.scene, in.entity) + ": ");
    return true;
}

}
