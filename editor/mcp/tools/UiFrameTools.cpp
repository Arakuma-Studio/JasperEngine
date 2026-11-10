// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpTools.h"

#include "App.h"
#include "EditorHost.h"
#include "Project.h"
#include "Log.h"
#include "Scene.h"
#include "component/UIDocumentComponent.h"
#include "component/ScriptComponent.h"
#include "htmlui/HtmlUIClock.h"
#include "htmlui/HtmlUIDependencies.h"
#include "htmlui/anim/AnimEngine.h"
#include "htmlui/anim/AnimLua.h"
#include "htmlui/anim/AnimTargets.h"
#include "io/watch/FileWatchService.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "object/ui/UIDocument.h"
#include "pixelart/PixelImage.h"
#include "pool/TextureDataPool.h"
#include "pixelart/fx/ContactSheet.h"
#include "pixelart/fx/EffectCapture.h"
#include "recorder/RecorderSession.h"
#include "registry/EntityCloner.h"
#include "script/LuaBinding.h"
#include "script/ScriptRuntime.h"
#include "render/OffscreenDraw.h"
#include "render/SystemRender.h"
#include "subsystem/RenderSystem.h"
#include "subsystem/UIDocumentSystem.h"
#include "texture/Framebuffer.h"
#include "util/Base64.h"
#include "util/FileUtils.h"
#include "util/Scheduler.h"

#include "lua.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <exception>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

namespace jasperengine::editor::mcp {
namespace uiframes {
namespace {

namespace fs = std::filesystem;
namespace px = jasperengine::editor::pixelart;
namespace fx = jasperengine::editor::pixelart::fx;
using Clock = std::chrono::steady_clock;

thread_local std::optional<Clock::time_point> previewLuaDeadline;

void previewTimeoutHook(lua_State* state, lua_Debug*) {
    if (previewLuaDeadline && Clock::now() >= *previewLuaDeadline)
        luaL_error(state, "render_ui_frames: Lua exceeded 5 seconds; fix the loop or use bounded anime callbacks");
}

struct PreviewLuaBudget {
    lua_State* state = LuaBinding::getLuaState();
    uint64_t generation = LuaBinding::getLuaStateGeneration();
    lua_Hook previousHook = nullptr;
    int previousMask = 0;
    int previousCount = 0;
    std::optional<Clock::time_point> previousDeadline = previewLuaDeadline;

    PreviewLuaBudget() {
        if (!state) return;
        previousHook = lua_gethook(state);
        previousMask = lua_gethookmask(state);
        previousCount = lua_gethookcount(state);
        previewLuaDeadline = Clock::now() + std::chrono::seconds(5);
        lua_sethook(state, previewTimeoutHook, LUA_MASKCOUNT, 10000);
    }
    ~PreviewLuaBudget() {
        if (state && state == LuaBinding::getLuaState() && generation == LuaBinding::getLuaStateGeneration())
            lua_sethook(state, previousHook, previousMask, previousCount);
        previewLuaDeadline = previousDeadline;
    }
};

// A preview never changes the running edit-time clock, including the accumulator restored
// when a fixed clock is released. Existing anime roots are excluded from its manual seeks.
struct PreviewTime {
    std::optional<double> fixed = HtmlUIClock::getFixedTime();
    double running = HtmlUIClock::now();
    AnimEngine::ScopedTickPause animePause;

    explicit PreviewTime(double seconds, std::vector<std::shared_ptr<AnimTimer>>* created = nullptr) : animePause(created) { HtmlUIClock::setFixedTime(seconds); }
    ~PreviewTime() {
        HtmlUIClock::setFixedTime(fixed ? fixed : std::optional<double>(running));
        if (!fixed) HtmlUIClock::setFixedTime(std::nullopt);
    }
};

std::string encodeImage(const px::PixelImage& image) {
    const std::string png = image.encodePng();
    return Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size());
}

struct PreviewErrors {
    std::string message;
    LogListenerId listener = 0;

    PreviewErrors() {
        listener = Log::addListener([this](const LogEntry& entry) {
            if (entry.mainThread && entry.level >= LogLevel::Error && message.empty()) message = entry.message;
        }, LogDelivery::AnyThread);
    }
    ~PreviewErrors() { Log::removeListener(listener); }
};

struct DocumentSource {
    std::string filename;
    int order = 0;
};

void unpremultiplyFrame(px::PixelImage& image) {
    auto pixels = image.editPixels();
    for (size_t i = 0; i < pixels.size(); i += 4) {
        const int alpha = pixels[i + 3];
        for (size_t channel = 0; channel < 3; channel++)
            pixels[i + channel] = alpha ? (uint8_t)std::min(255, ((int)pixels[i + channel] * 255 + alpha / 2) / alpha) : 0;
    }
}

struct PreviewArgs {
    std::vector<double> times;
    std::string document;
    uint32_t scene = NULL_PROJECT_SCENE;
    int width = 1080;
    int height = 1350;
    int columns = 0;
    int fps = 60;
    bool frames = false;
    bool watch = false;
    fs::path output;
};

class PreviewRun;

std::shared_ptr<PreviewRun>& activePreview() {
    static std::shared_ptr<PreviewRun> run;
    return run;
}

class PreviewRun : public std::enable_shared_from_this<PreviewRun> {
public:
    Project* project = nullptr;
    fs::path projectPath;
    PreviewArgs args;
    JobPtr job;
    CallPtr firstCall;
    std::function<void()> requestFrame;
    bool cancelled = false;

    void stop() { cancelled = true; advance(); }

    void start() {
        if (args.watch) {
            WatchFilter filter;
            filter.root = projectPath;
            filter.deliverEditorWrites = true;
            subscription = FileWatchService::get().subscribe(std::move(filter),
                [weak = weak_from_this()](std::span<const FileChange> changes) {
                    const auto run = weak.lock();
                    if (!run || run->finished) return;
                    for (const FileChange& change : changes) {
                        if (change.kind == FileChangeKind::Rescan || run->dependencies.count(change.path.lexically_normal()) ||
                            run->dependencies.count(change.oldPath.lexically_normal())) {
                            run->dirty = true;
                            run->schedule();
                            break;
                        }
                    }
                });
        }
        schedule();
    }

    void schedule() {
        if (scheduled || finished) return;
        scheduled = true;
        scheduler().afterFrames(1, [self = shared_from_this()]() {
            self->scheduled = false;
            McpWriteScope writes;
            try { self->advance(); }
            catch (const std::exception& error) { self->fail(error.what()); }
        }, TimerOpts{.debugName = "render-ui-frames"});
        if (requestFrame) requestFrame();
    }

private:
    std::vector<DocumentSource> sources;
    std::vector<Entity> sourceEntities;
    std::vector<px::PixelImage> images;
    std::set<fs::path> dependencies;
    std::set<std::string> seenModules;
    FileWatchSubscription subscription;
    std::unique_ptr<Scene> scratch;
    int moduleCache = LUA_NOREF;
    std::vector<std::unique_ptr<UIDocument>> documents;
    std::set<AnimTimer*> ownedRoots;
    std::vector<std::shared_ptr<AnimTimer>> pendingRoots;
    struct PreviewRoot { std::shared_ptr<AnimTimer> timer; double createdAt = 0.0; };
    std::vector<PreviewRoot> roots;
    Framebuffer target;
    size_t index = 0;
    uint64_t revision = 0;
    bool dirty = true;
    bool scheduled = false;
    bool finished = false;
    bool scriptsStarted = false;
    bool sampled = false;
    bool sought = false;
    double sampledTime = 0.0;
    uint64_t stepIndex = 0;
    bool warmed = false;
    Clock::time_point began;

    void collectRoots(double seconds = 0.0) {
        for (const auto& timer : pendingRoots) {
            if (timer->getParent() || !ownedRoots.insert(timer.get()).second) continue;
            roots.push_back({timer, seconds});
            timer->paused = true;
        }
        pendingRoots.clear();
    }

    void collectModuleDependencies(bool baseline = false) {
        lua_State* state = LuaBinding::getLuaState();
        if (!state || moduleCache < 0) return;
        const int top = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, moduleCache);
        if (lua_istable(state, -1)) {
            lua_pushnil(state);
            while (lua_next(state, -2)) {
                if (lua_type(state, -2) == LUA_TSTRING) {
                    const std::string name = lua_tostring(state, -2);
                    if (seenModules.insert(name).second && !baseline) {
                        std::string relative = name;
                        std::replace(relative.begin(), relative.end(), '.', '/');
                        const fs::path module = FileUtils::pathFromUtf8(relative);
                        if (!module.is_absolute()) {
                            for (const fs::path& root : {project->getLuaPath(), projectPath}) {
                                for (const fs::path& suffix : {FileUtils::pathFromUtf8(relative + ".lua"), module / "init.lua"}) {
                                    std::error_code ec;
                                    const fs::path file = (root / suffix).lexically_normal();
                                    if (fs::is_regular_file(file, ec)) dependencies.insert(file);
                                }
                            }
                        }
                    }
                }
                lua_pop(state, 1);
            }
        }
        lua_settop(state, top);
    }

    void releaseScratch() {
        if (!scratch) return;
        PreviewLuaBudget budget;
        PreviewTime time(0.0, &pendingRoots);
        LuaBinding::ScopedModuleCache modules(moduleCache);
        AnimTargets::ScopedSceneTargets selectors(scratch.get());
        if (scriptsStarted) LuaBinding::cleanupLuaScripts(scratch.get());
        scriptsStarted = false;
        auto discardRoots = [&]() {
            collectRoots();
            for (size_t i = 0; i < roots.size(); i++) {
                const auto timer = roots[i].timer;
                // A cancel callback may create another root. Drain those roots too, with a
                // bound on user cleanup callbacks so an endless cancellation chain terminates.
                if (i < 4096) timer->cancel();
                else timer->paused = true;
                AnimEngine::instance().detachRoot(*timer);
                collectRoots();
            }
            if (roots.size() > 4096) Log::error("UI preview cleanup created over 4096 animation roots; reuse a timeline in cancellation callbacks.");
            roots.clear();
            ownedRoots.clear();
        };
        discardRoots();
        documents.clear();
        scratch.reset();
        // onUnload may have created a timer too; it belongs to this discarded document.
        discardRoots();
        AnimLua::releasePreviewRefs(moduleCache);
        LuaBinding::releaseLuaRef(moduleCache);
        moduleCache = LUA_NOREF;
        seenModules.clear();
        warmed = false;
        sampled = false;
        sought = false;
        sampledTime = 0.0;
        stepIndex = 0;
    }

    bool loadSources(std::string& error) {
        sources.clear();
        sourceEntities.clear();
        if (!args.document.empty()) {
            sources.push_back({args.document, 0});
        } else {
            SceneProject* source = project->getScene(args.scene);
            if (!source || !source->scene) {
                error = "The source scene was closed: open it again and retry render_ui_frames.";
                return false;
            }
            const auto components = source->scene->getComponentArray<UIDocumentComponent>();
            for (size_t i = 0; i < components->size(); i++) {
                const UIDocumentComponent& document = components->getComponentFromIndex(i);
                if (document.enabled && source->scene->isEntityActiveInHierarchy(components->getEntity(i)) &&
                    document.renderMode == UIDocumentRenderMode::SCREEN && !document.filename.empty()) {
                    sources.push_back({document.filename.path, document.sortOrder});
                    sourceEntities.push_back(components->getEntity(i));
                }
            }
        }
        if (sources.empty()) {
            error = "The scene has no enabled screen UI Documents: pass document with a project-relative HTML/RML asset.";
            return false;
        }
        dependencies.clear();
        if (args.document.empty()) {
            const SceneProject* source = project->getScene(args.scene);
            for (Entity entity : sourceEntities) {
                const ScriptComponent* scripts = source->scene->findComponent<ScriptComponent>(entity);
                if (!scripts) continue;
                for (const ScriptEntry& script : scripts->scripts) {
                    if (script.type != ScriptType::LUA || script.path.empty()) continue;
                    const fs::path path = FileUtils::pathFromUtf8(script.path.path);
                    dependencies.insert((projectPath / path).lexically_normal());
                    dependencies.insert((project->getLuaPath() / path).lexically_normal());
                }
            }
        }
        for (const DocumentSource& source : sources) {
            dependencies.insert((project->getAssetsPath() / FileUtils::pathFromUtf8(source.filename)).lexically_normal());
            for (const HtmlUIReference& reference : collectHtmlUIReferences(source.filename)) {
                if (!reference.remote && !reference.outsideAssets && !reference.absolute)
                    dependencies.insert((project->getAssetsPath() / FileUtils::pathFromUtf8(reference.path)).lexically_normal());
            }
        }
        return true;
    }

    bool makeScratch(std::string& error) {
        PreviewTime time(0.0, &pendingRoots);
        scratch = std::make_unique<Scene>(EntityPool::System);
        moduleCache = LuaBinding::createPreviewModuleCache();
        if (moduleCache < 0) { error = "Could not create a private Lua context: retry after the editor finishes loading."; return false; }
        collectModuleDependencies(true);
        LuaBinding::ScopedModuleCache modules(moduleCache);
        scratch->setBackgroundColor(0.0f, 0.0f, 0.0f, 0.0f);
        const auto system = scratch->getSystem<UIDocumentSystem>();
        system->setReferenceSize(args.width, args.height);
        AnimTargets::ScopedSceneTargets selectors(scratch.get());
        if (args.document.empty()) {
            SceneProject* source = project->getScene(args.scene);
            if (!source || !source->scene) { error = "The source scene was closed: reopen it and retry."; return false; }
            std::unordered_map<Entity, Entity> remap;
            const auto copies = EntityCloner::clone(source->scene, sourceEntities, scratch.get(), NULL_ENTITY, &remap);
            for (Entity entity : copies) {
                ScriptComponent* scripts = scratch->findComponent<ScriptComponent>(entity);
                if (!scripts) continue;
                for (ScriptEntry& script : scripts->scripts) {
                    for (ScriptProperty& property : script.properties) {
                        if (property.type != ScriptPropertyType::EntityReference) continue;
                        auto validate = [&](ScriptPropertyValue& value, bool isDefault) {
                            EntityReference* reference = std::get_if<EntityReference>(&value);
                            if (!reference || reference->entity == NULL_ENTITY) return true;
                            if (reference->sceneId != 0 && reference->sceneId != args.scene) {
                                error = "UI script property " + property.name + " references an external scene: keep preview references in this scene or use record_game.";
                                return false;
                            }
                            if (reference->sceneId != 0 || isDefault) {
                                const auto found = remap.find(reference->entity);
                                if (found == remap.end()) {
                                    error = "UI script property " + property.name + " references an object outside the preview UI: use a UI object in this scene or record_game.";
                                    return false;
                                }
                                reference->entity = found->second;
                                reference->sceneId = 0;
                            }
                            return true;
                        };
                        if (!validate(property.value, false) || !validate(property.defaultValue, true)) return false;
                    }
                }
            }
            for (Entity entity : copies) documents.push_back(std::make_unique<UIDocument>(scratch.get(), entity));
        } else {
            for (const DocumentSource& source : sources) {
                auto document = std::make_unique<UIDocument>(scratch.get());
                document->setFilename(source.filename);
                document->setSortOrder(source.order);
                documents.push_back(std::move(document));
            }
        }
        scratch->load();
        system->update(0.0);
        for (const auto& document : documents) {
            if (!document->isLoaded()) {
                error = "Could not load " + document->getFilename() + ": run check --ui and fix the document before rendering.";
                return false;
            }
        }
        scriptsStarted = true;
        const auto scriptComponents = scratch->getComponentArray<ScriptComponent>();
        for (size_t i = 0; i < scriptComponents->size(); i++) {
            for (const ScriptEntry& script : scriptComponents->getComponentFromIndex(i).scripts) {
                if (script.type == ScriptType::LUA) LuaBinding::invalidateScriptChunk(script.path.path);
            }
        }
        LuaBinding::initializeLuaScripts(scratch.get());
        for (size_t i = 0; i < scriptComponents->size(); i++) {
            for (const ScriptEntry& script : scriptComponents->getComponentFromIndex(i).scripts) {
                if (script.type == ScriptType::LUA && !script.instance) {
                    error = "Could not initialize UI script " + script.path.path + ": fix its Console error and retry.";
                    return false;
                }
            }
        }
        ScriptRuntime::flushStarts(scratch.get());
        collectModuleDependencies();
        collectRoots();
        for (const auto& document : documents) {
            for (const auto& diagnostic : document->getDiagnostics()) {
                if (diagnostic.severity == htmlui::compat::Severity::Error) {
                    error = diagnostic.file + ":" + std::to_string(diagnostic.line) + ": " + diagnostic.message + ". " + diagnostic.suggestion;
                    return false;
                }
            }
        }
        system->update(0.0);
        scratch->getSystem<RenderSystem>()->update(0.0);
        return true;
    }

    void advance() {
        if (finished) return;
        PreviewLuaBudget budget;
        PreviewErrors errors;
        auto failLogged = [&]() {
            if (errors.message.empty()) return false;
            fail("UI preview failed: " + errors.message + ". Fix the document or Lua script and retry.");
            return true;
        };
        if (cancelled || job->cancelRequested() || (firstCall && firstCall->cancelled())) {
            releaseScratch();
            job->markCancelled("UI frame preview stopped.");
            if (firstCall) firstCall->fail("UI frame preview cancelled.");
            finish();
            return;
        }
        if (project->getProjectPath() != projectPath || project->isPlaySessionActive() || recorder::RecorderSession::get().active() ||
            fx::EffectCapture::anyRunning()) {
            fail("The project changed or another capture/Play started: stop it and retry render_ui_frames.");
            return;
        }
        if (dirty && images.empty() && !scratch) {
            dirty = false;
            index = 0;
            began = Clock::now();
            std::string error;
            if (!loadSources(error)) { fail(error); return; }
        } else if (!scratch && images.empty() && index == args.times.size()) {
            return;
        }
        if (Clock::now() - began > std::chrono::seconds(120)) {
            fail("UI frames did not settle within 120 seconds: inspect the Console for asset or shader errors and retry.");
            return;
        }
        if (!Engine::isViewLoaded()) { schedule(); return; }
        if (!scratch) {
            std::string error;
            if (!makeScratch(error)) { if (!failLogged()) fail(error); return; }
            if (failLogged()) return;
            target.destroy();
            target.setWidth((unsigned int)args.width);
            target.setHeight((unsigned int)args.height);
            target.create();
            if (!target.isCreated()) { fail("Could not create the UI frame target: reduce width and height and retry."); return; }
            schedule();
            return;
        }
        PreviewTime time(sampled ? sampledTime : 0.0, &pendingRoots);
        LuaBinding::ScopedModuleCache modules(moduleCache);
        AnimTargets::ScopedSceneTargets selectors(scratch.get());
        ScriptRuntime::flushStarts(scratch.get());
        const auto system = scratch->getSystem<UIDocumentSystem>();
        if (!sampled) {
            // Warm all resources at the origin before advancing CSS, including fonts and images.
            system->update(0.0);
            scratch->getSystem<RenderSystem>()->update(0.0);
        } else {
            const auto sliceStart = Clock::now();
            auto advanceAnimations = [&](double seconds) {
                HtmlUIClock::setFixedTime(seconds);
                collectRoots(sampledTime);
                for (size_t i = 0; i < roots.size(); i++) {
                    const PreviewRoot root = roots[i];
                    if (!root.timer->getParent() && !root.timer->isCancelled() && !root.timer->isCompleted()) {
                        const double elapsed = seconds - root.createdAt;
                        const double position = root.timer->absoluteTime + elapsed * 1000.0 * root.timer->getSpeed() * AnimEngine::instance().speed;
                        root.timer->seek(position);
                    }
                    roots[i].createdAt = seconds;
                    collectRoots(seconds);
                    if (roots.size() > 4096) return false;
                }
                AnimEngine::instance().flushValues();
                system->update(seconds - sampledTime);
                ScriptRuntime::flushStarts(scratch.get());
                collectRoots(seconds);
                sampledTime = seconds;
                return true;
            };
            if (!sought) {
                if (!advanceAnimations(0.0)) { fail("The document created over 4096 animation roots: reuse a timeline and retry."); return; }
                if (failLogged()) return;
                sought = true;
            }
            // Intermediate callbacks can start CSS transitions or new timers. Evaluate the
            // same frame history each run, then sample the final fractional frame exactly.
            while (sampledTime < args.times[index]) {
                const double next = std::min((double)(++stepIndex) / args.fps, args.times[index]);
                if (!advanceAnimations(next)) { fail("The document created over 4096 animation roots: reuse a timeline and retry."); return; }
                if (failLogged()) return;
                if (sampledTime < args.times[index] && Clock::now() - sliceStart > std::chrono::milliseconds(8)) {
                    collectModuleDependencies();
                    schedule();
                    return;
                }
            }
            collectModuleDependencies();
            system->update(0.0);
            scratch->getSystem<RenderSystem>()->update(0.0);
        }
        Engine::ViewLayer layer;
        layer.scene = scratch.get();
        layer.camera = scratch->getCamera();
        layer.viewRect = Rect(0.0f, 0.0f, (float)args.width, (float)args.height);
        layer.canvasWidth = (float)args.width;
        layer.canvasHeight = (float)args.height;
        std::vector<uint8_t> rgba;
        std::string waiting;
        auto readyAfterDraw = [&system](std::string& pending) {
            if (!system->isAllLoaded() || !SystemRender::isGpuCreationIdle() || TextureDataPool::getLoadingCount() != 0) {
                pending = "UI documents, textures or rendering resources";
                return false;
            }
            return true;
        };
        const OffscreenDraw drawn = drawOffscreen(CONTACT_SHEET_VIEW_ID, target, {layer}, warmed, rgba, waiting, nullptr, readyAfterDraw);
        if (failLogged()) return;
        if (drawn == OffscreenDraw::Wait) {
            job->setProgress((double)index / (double)args.times.size(), "Waiting for " + waiting);
            schedule();
            return;
        }
        if (!sampled) {
            sampled = true;
            schedule();
            return;
        }
        px::PixelImage image(args.width, args.height);
        std::copy(rgba.begin(), rgba.end(), image.editPixels().begin());
        images.push_back(std::move(image));
        releaseScratch();
        if (failLogged()) return;
        index++;
        job->setProgress((double)index / (double)args.times.size(), "Rendered " + std::to_string(index) + " UI frame(s)");
        if (index < args.times.size()) { schedule(); return; }
        publish();
    }

    void publish() {
        Json files = Json::array();
        std::vector<ToolImage> returnedImages;
        if (args.frames) {
            std::error_code ec;
            const std::string directoryRefusal = McpRefs::writeEscape(project, args.output);
            if (!directoryRefusal.empty()) { fail(directoryRefusal); return; }
            fs::create_directories(args.output, ec);
            if (ec) { fail("Could not create output directory: " + ec.message() + ". Choose another out path."); return; }
            for (size_t i = 0; i < images.size(); i++) {
                const fs::path output = args.output / ("frame_" + std::to_string(i) + ".png");
                const std::string refusal = McpRefs::writeEscape(project, output);
                if (!refusal.empty()) { fail(refusal); return; }
                px::PixelImage straight = images[i];
                unpremultiplyFrame(straight);
                if (!straight.savePng(output)) { fail("Could not write " + FileUtils::pathToUtf8(output) + ": check available disk space and permissions."); return; }
                files.push_back(FileUtils::pathToGenericUtf8(output));
                if (firstCall) returnedImages.push_back({encodeImage(straight), "image/png"});
            }
        } else {
            fx::SheetSettings settings;
            settings.columns = args.columns > 0 ? args.columns : (int)std::ceil(std::sqrt((double)images.size()));
            settings.scale = 1;
            settings.maxWidth = settings.columns * args.width + settings.columns - 1;
            std::vector<const px::PixelImage*> cells;
            for (const auto& image : images) cells.push_back(&image);
            px::PixelImage sheet = fx::composeSheet(cells, args.times, settings);
            const std::string refusal = McpRefs::writeEscape(project, args.output);
            if (!refusal.empty()) { fail(refusal); return; }
            if (!sheet.savePng(args.output)) { fail("Could not write the UI sheet: check its parent directory, disk space and permissions."); return; }
            files.push_back(FileUtils::pathToGenericUtf8(args.output));
            if (firstCall) returnedImages.push_back({encodeImage(sheet), "image/png"});
        }
        revision++;
        Json data{{"files", files}, {"times", args.times}, {"width", args.width}, {"height", args.height}, {"fps", args.fps}, {"revision", revision}};
        std::string message = "Rendered " + std::to_string(images.size()) + " UI frame(s).";
        if (args.watch) {
            data["job_id"] = job->id();
            data["status"] = "running";
            data["watching"] = true;
            data["next"] = "Saving a document or one of its dependencies regenerates the output; cancel_job stops watching.";
            job->log("Revision " + std::to_string(revision) + ": " + files.dump());
            job->setProgress(-1.0, "Watching UI source files (revision " + std::to_string(revision) + ")");
        } else {
            job->succeed(message, data);
        }
        if (firstCall) {
            ToolResult result = ToolResult::success(message, data);
            result.images = std::move(returnedImages);
            firstCall->reply(std::move(result));
            firstCall.reset();
        }
        images.clear();
        if (!args.watch) finish();
        else if (dirty) schedule();
    }

    void fail(const std::string& error) {
        releaseScratch();
        job->fail(error);
        if (firstCall) { firstCall->fail(error); firstCall.reset(); }
        finish();
    }

    void finish() {
        finished = true;
        subscription.reset();
        target.destroy();
        if (activePreview().get() == this) activePreview().reset();
    }
};

void renderUiFrames(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& input = call->args();
    if (activePreview()) { call->fail("A UI frame preview is active: cancel_job " + activePreview()->job->id() + " before starting another."); return; }
    if (call->ctx().isPlaying() || recorder::RecorderSession::get().active() || fx::EffectCapture::anyRunning()) {
        call->fail("Stop Play and other recordings/previews before render_ui_frames; use record_game for a running game.");
        return;
    }
    PreviewArgs args;
    args.times = input.at("times").get<std::vector<double>>();
    if (args.times.empty() || args.times.size() > 64 || std::any_of(args.times.begin(), args.times.end(), [](double t) { return !std::isfinite(t) || t < 0.0 || t > 3600.0; })) {
        call->fail("times must contain 1 to 64 finite seconds in [0, 3600].");
        return;
    }
    args.width = input.value("width", (int)project->getCanvasWidth());
    args.height = input.value("height", (int)project->getCanvasHeight());
    if (args.width < 16 || args.width > 4096 || args.height < 16 || args.height > 4096 ||
        (int64_t)args.width * args.height * (int64_t)args.times.size() > fx::kCaptureMaxPixels) {
        call->fail("Use width/height from 16 to 4096 and at most 32 million pixels across all frames; reduce the size or times count.");
        return;
    }
    args.fps = input.value("fps", 60);
    if (args.fps < 1 || args.fps > 240) { call->fail("fps must be 1 to 240."); return; }
    args.columns = input.value("columns", 0);
    if (args.columns < 0 || args.columns > 8) { call->fail("columns must be 0 (automatic) or 1 to 8."); return; }
    args.frames = input.value("layout", std::string("sheet")) == "frames";
    args.watch = input.value("watch", false);
    if (args.watch && (!FileWatchService::get().isAttached() || FileWatchService::get().stats().state == FileWatchService::State::Off)) {
        call->fail("watch needs the editor file watcher: restart with --file-watcher=auto, then retry.");
        return;
    }
    if (input.contains("document")) {
        fs::path absolute;
        std::string error;
        if (!McpRefs::resolveProjectPath(project, input["document"].get<std::string>(), absolute, error, true)) { call->fail(error); return; }
        if (!project->isInsideAssetsPath(absolute)) { call->fail("document must be an HTML/RML file in the project's assets directory."); return; }
        const std::string extension = absolute.extension().string();
        if (extension != ".html" && extension != ".htm" && extension != ".rml") { call->fail("document must have extension .html, .htm or .rml."); return; }
        args.document = FileUtils::pathToGenericUtf8(absolute.lexically_relative(project->getAssetsPath()));
    } else {
        std::string error;
        SceneProject* source = McpRefs::resolveScene(project, input.value("scene", Json()), error);
        if (!source || !source->scene) { call->fail(source ? McpRefs::notOpenError(project, *source) : error); return; }
        args.scene = source->id;
    }
    std::string error;
    const std::string output = input.value("out", std::string(args.frames ? "ui-frames" : "ui-frames.png"));
    if (!McpRefs::resolveProjectWritePath(project, output, args.output, error)) { call->fail(error); return; }
    const std::string protectedPath = McpRefs::protectedPlace(project, args.output);
    if (!protectedPath.empty()) { call->fail(protectedPath); return; }
    if (!args.frames && args.output.extension() != ".png") { call->fail("A sheet out path must end in .png; use layout frames for a directory."); return; }
    auto run = std::make_shared<PreviewRun>();
    run->requestFrame = [&app = call->ctx().app()]() { app.requestFrame(); };
    run->project = project;
    run->projectPath = project->getProjectPath();
    run->args = std::move(args);
    run->job = call->ctx().jobs().create("ui-frames", "Render UI frames");
    if (input.value("wait", true)) {
        run->firstCall = call;
        call->detach();
    } else {
        call->ok("UI frame preview started.", {{"job_id", run->job->id()}, {"status", "running"}});
    }
    run->job->setCancelHandler([weak = std::weak_ptr<PreviewRun>(run)]() {
        if (const auto held = weak.lock()) { held->cancelled = true; held->schedule(); }
    });
    activePreview() = run;
    run->start();
}

} // namespace
} // namespace uiframes

void stopUiFramePreviews() {
    const auto run = uiframes::activePreview();
    if (run) run->stop();
}

void registerUiFrameTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "render_ui_frames";
    tool.title = "Render UI Frames";
    tool.group = "html_ui";
    tool.description = "Preview HTML UI CSS and anime at exact seconds without Play, as PNGs or a labelled sheet. Defaults to the scene's screen documents. watch regenerates on source saves until cancel_job. Use record_game for gameplay.";
    tool.inputSchema = object({
        {"scene", sceneRef()},
        {"document", assetPath("HTML/RML; overrides scene.")},
        {"times", array(number("", 0, 3600), "1-64 times in seconds; order kept.")},
        {"out", string("Project-relative PNG, or frames directory.")},
        {"layout", stringEnum("Default sheet.", {"sheet", "frames"})},
        {"width", integer("Default canvas.", 16, 4096)},
        {"height", integer("Default canvas.", 16, 4096)},
        {"columns", integer("0 auto.", 0, 8)},
        {"fps", integer("Callback step; default 60.", 1, 240)},
        {"watch", boolean("Regenerate on saves.")},
        {"wait", boolean("Default true; false returns job_id.")}
    }, {"times"});
    tool.annotations = {false, false, false};
    tool.activity = ToolActivity::Control;
    tool.allowedWhilePlaying = false;
    tool.handler = uiframes::renderUiFrames;
    registry.addTool(std::move(tool));
}

} // namespace jasperengine::editor::mcp
