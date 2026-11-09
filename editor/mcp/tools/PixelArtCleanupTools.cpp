// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// fix_pixel_art and lint_pixel_art (docs/specs/S46-pixel-art-cleanup.md R44-R47, §6.1, D3): the
// agent side of the Pixel Art Cleanup, on the one session layer the window and the CLI share
// (pixelart/cleanup/CleanupSession.h). A fix previews until output is given, and the write takes
// exactly the previewed pixels: one project undo step, then (apply) a second, scene step. The run
// is a job on a Background worker the call waits on (timeout_seconds); a cancelled run writes nothing.

#include "McpTools.h"

#include "Project.h"
#include "Scene.h"
#include "EditorHost.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/UndoOrigin.h"
#include "command/UndoTimeline.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ActionComponent.h"
#include "component/MeshComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/SpriteComponent.h"
#include "jobs/JobSystem.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpUndo.h"
#include "mcp/McpValues.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "pixelart/aseprite/AsepriteSpriteSetup.h"
#include "pixelart/cleanup/CleanupRecipe.h"
#include "pixelart/cleanup/CleanupSample.h"
#include "pixelart/cleanup/CleanupSession.h"
#include "pixelart/cleanup/CleanupSheetOutput.h"
#include "pixelart/lint/PixelLint.h"
#include "Catalog.h"
#include "util/Base64.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>

namespace jasperengine::editor::mcp {

namespace epx = jasperengine::editor::pixelart;
namespace cleanup = jasperengine::editor::pixelart::cleanup;

namespace pixelcleanuptools {
namespace {

using Problem = epx::PixelProblem;
namespace fs = std::filesystem;

// the call waits this long for a run (R37: timeout_seconds), then answers with the job
constexpr double kDefaultWaitSeconds = 60.0;
// a lint of more pixels than this runs as a job (R38's budgets are for sprites)
constexpr size_t kLintInlinePixels = 2'000'000;

Json problemJson(const Problem& p) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix.empty() ? std::string("see jasper://docs/pixel-art-cleanup") : p.fix}};
    if (!p.field.empty()) j["field"] = p.field;
    return j;
}

std::string problemsText(const std::string& headline, const std::vector<Problem>& problems, Json& list) {
    std::string text = headline;
    list = Json::array();
    for (const Problem& p : problems) {
        text += "\n- " + p.kind + ": " + p.message + (p.fix.empty() ? "" : " Fix: " + p.fix);
        list.push_back(problemJson(p));
    }
    return text;
}

void failCall(const CallPtr& call, const std::string& headline, const std::vector<Problem>& problems) {
    Json list;
    const std::string text = problemsText(headline, problems, list);
    call->fail(text, Json{{"errors", list}});
}

bool stringList(const Json& value, const std::string& key, std::vector<std::string>& out, std::vector<Problem>& problems) {
    if (!value.is_array()) {
        problems.push_back({"invalid_value", key, key + " must be a list of strings", "give strings"});
        return false;
    }
    for (const Json& item : value) {
        if (!item.is_string()) {
            problems.push_back({"invalid_value", key, key + " must hold strings, got " + item.dump(), "give strings"});
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

// source/entity shared by both tools; entity needs the selected scene
bool resolveEntityArg(const CallPtr& call, const Json& args, uint32_t& sceneId, Entity& entity, std::vector<Problem>& problems) {
    if (!args.contains("entity") || args["entity"].is_null()) return true;
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* scene = McpRefs::resolveScene(project, Json(), error);
    if (!scene || !scene->scene) {
        problems.push_back({"source_missing", "entity", error.empty() ? "no scene is open" : error, "open the scene that holds the Sprite"});
        return false;
    }
    entity = McpRefs::resolveEntity(scene, args["entity"], error);
    if (entity == NULL_ENTITY) {
        problems.push_back({"source_missing", "entity", error, "name a Sprite GameObject of the selected scene"});
        return false;
    }
    sceneId = scene->id;
    return true;
}

bool paletteArg(const Json& args, Json& out, std::vector<Problem>& problems) {
    if (!args.contains("palette") || args["palette"].is_null()) return true;
    if (!args["palette"].is_string() && !args["palette"].is_array()) {
        problems.push_back({"palette_missing", "palette", "palette must be a name, a .palette path or a list of \"#rrggbb\"", "e.g. \"sweetie-16\""});
        return false;
    }
    out = args["palette"];
    return true;
}

// ---- fix_pixel_art

struct FixState {
    cleanup::Prepared prepared;
    cleanup::Result result;
    cleanup::PreviewKind image = cleanup::PreviewKind::Compare;
    int maxWidth = cleanup::kDefaultPreviewWidth;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    fs::path root;                  // the project this run was prepared in
    // apply: a Sprite to set up from the written files
    bool apply = false;
    uint32_t applyScene = 0;
    Entity applyEntity = NULL_ENTITY;
    JobPtr job;
    std::string text;
    Json data;
    std::vector<ToolImage> images;
};

bool parseFixArgs(const CallPtr& call, cleanup::Spec& spec, FixState& state, std::vector<Problem>& problems) {
    const Json& args = call->args();
    if (args.contains("source")) {
        if (!args["source"].is_string()) problems.push_back({"source_missing", "source", "source must be a path", "a project-relative image or .aseprite path"});
        else spec.source = args["source"].get<std::string>();
    }
    if (!resolveEntityArg(call, args, spec.sceneId, spec.entity, problems)) return false;
    if (spec.source.empty() && spec.entity == NULL_ENTITY) problems.push_back({"source_missing", "source", "give source (an image or .aseprite) or entity (a Sprite)", "pass source"});
    if (args.contains("recipe") && !args["recipe"].is_null()) {
        if (!args["recipe"].is_object()) problems.push_back({"bad_recipe", "recipe", "recipe must be an object", "see jasper://docs/pixel-art-cleanup"});
        else spec.recipe = args["recipe"];
    }
    if (!paletteArg(args, spec.palette, problems)) return false;
    if (args.contains("sheet")) {
        const std::string sheet = args.value("sheet", "auto");
        if (sheet == "off") spec.sheet = cleanup::SheetArg::Off;
        else if (sheet == "on") spec.sheet = cleanup::SheetArg::On;
        else if (sheet != "auto") problems.push_back({"invalid_value", "sheet", "sheet is auto, off or on", "e.g. \"off\""});
    }
    spec.rerun = args.value("rerun", false);
    spec.overwrite = args.value("overwrite", false);
    spec.force = args.value("force", false);
    if (args.contains("format")) {
        const std::string format = args.value("format", "");
        if (format == "png") spec.format = cleanup::OutputFormat::Png;
        else if (format == "aseprite") spec.format = cleanup::OutputFormat::Aseprite;
        else problems.push_back({"invalid_value", "format", "format is png or aseprite", "\"png\" or \"aseprite\""});
    }
    if (args.contains("output") && !args["output"].is_null()) {
        const Json& output = args["output"];
        if (output.is_boolean()) {
            spec.output.write = output.get<bool>();
        } else if (output.is_string()) {
            spec.output.write = true;
            const std::string path = output.get<std::string>();
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            const bool png = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".png") == 0;
            const bool aseprite = lower.size() >= 9 && lower.compare(lower.size() - 9, 9, ".aseprite") == 0;
            if (png) spec.output.png = path;
            else if (aseprite) spec.output.aseprite = path;
            else problems.push_back({"output_outside_assets", "output", "output must end in .png or .aseprite", "a .png or .aseprite path inside the assets directory, or output: true"});
        } else if (output.is_object()) {
            spec.output.write = true;
            for (auto it = output.begin(); it != output.end(); ++it) {
                std::string* target = it.key() == "png" ? &spec.output.png : it.key() == "aseprite" ? &spec.output.aseprite : it.key() == "json" ? &spec.output.json : nullptr;
                if (!target || !it.value().is_string()) {
                    problems.push_back({"output_outside_assets", "output." + it.key(), "output holds {png, aseprite, json} paths", "e.g. {\"png\": \"assets/hero_clean.png\"}"});
                    continue;
                }
                *target = it.value().get<std::string>();
            }
        } else {
            problems.push_back({"output_outside_assets", "output", "output is true, a path or {png, aseprite, json}", "use output: true"});
        }
    }
    if (args.contains("image")) {
        if (!args["image"].is_string() || !cleanup::parsePreviewKind(args["image"].get<std::string>(), state.image)) {
            problems.push_back({"invalid_value", "image", "image is compare, result, grid, lost, lints or none", "e.g. \"compare\""});
        }
    }
    if (args.contains("max_width")) {
        if (!args["max_width"].is_number_integer() || args["max_width"].get<int>() < cleanup::kMinPreviewWidth || args["max_width"].get<int>() > cleanup::kMaxPreviewWidth) {
            problems.push_back({"invalid_value", "max_width", "max_width is a whole number 256...2048", "e.g. 1024"});
        } else {
            state.maxWidth = args["max_width"].get<int>();
        }
    }
    return problems.empty();
}

// apply: set the written files on a Sprite as one scene undo step
struct ApplyOutcome {
    bool ok = false;
    Problem problem;
    std::string undoName;
};

ApplyOutcome applyPngOutput(Project* project, SceneProject* sp, Entity entity, const std::string& assetPath, const cleanup::Result& result) {
    ApplyOutcome out;
    std::string error;
    const uint32_t sceneId = sp->id;
    const std::string name = "submeshes[0].material.baseColorTexture";
    PropertyData property = Catalog::findProperty(sp->scene, entity, ComponentType::MeshComponent, name, true);
    if (!property.ref) {
        out.problem = {"invalid_apply", "apply", "the GameObject has no mesh to hold a texture", "name a Sprite GameObject"};
        return out;
    }
    Command* textureCommand = McpValues::buildPropertyCommand(project, sceneId, entity, ComponentType::MeshComponent, name, property, Json(assetPath), error);
    if (!textureCommand) {
        out.problem = {"invalid_apply", "apply", error, "name a Sprite GameObject"};
        return out;
    }
    auto multi = std::make_unique<MultiPropertyCmd>();
    multi->addCommand(std::unique_ptr<Command>(textureCommand));
    const SpriteComponent& sprite = sp->scene->getComponent<SpriteComponent>(entity);
    const ComponentType cp = ComponentType::SpriteComponent;
    int frames = 0;
    const std::vector<cleanup::AtlasClip> clips = result.animation ? cleanup::atlasClips(*result.animation) : std::vector<cleanup::AtlasClip>();
    cleanup::Atlas atlas;
    int width = 0, height = 0;
    if (result.animation) {
        atlas = cleanup::pngSheet(clips);
        width = atlas.art.width;
        height = atlas.art.height;
    } else if (!result.frames.empty()) {
        width = result.frames.front().width();
        height = result.frames.front().height();
    }
    if (result.animation) {
        for (size_t i = 0; i < atlas.rects.size(); i++) {
            const cleanup::AtlasRect& r = atlas.rects[i];
            const std::string prefix = "framesRect[" + std::to_string(i) + "]";
            multi->addPropertyCmd<std::string>(project, sceneId, entity, cp, prefix + ".name", clips[r.clip].name + "-" + std::to_string(r.frame));
            multi->addPropertyCmd<Vector4>(project, sceneId, entity, cp, prefix + ".rect", Vector4((float)r.x, (float)r.y, (float)r.w, (float)r.h));
            frames++;
        }
        multi->addPropertyCmd<unsigned int>(project, sceneId, entity, cp, "numFramesRect", (unsigned int)atlas.rects.size());
    }
    if (width > 0 && height > 0) {
        const int shownW = result.animation && !atlas.rects.empty() ? atlas.rects[0].w : width;
        const int shownH = result.animation && !atlas.rects.empty() ? atlas.rects[0].h : height;
        multi->addPropertyCmd<unsigned int>(project, sceneId, entity, cp, "width", (unsigned int)shownW);
        multi->addPropertyCmd<unsigned int>(project, sceneId, entity, cp, "height", (unsigned int)shownH);
        const float u = !atlas.rects.empty() ? (float)atlas.rects[0].x / width : 0.0f;
        const float v = !atlas.rects.empty() ? (float)atlas.rects[0].y / height : 0.0f;
        multi->addPropertyCmd<Vector4>(project, sceneId, entity, ComponentType::MeshComponent, "submeshes[0].textureRect",
                                       Vector4(u, v, (float)shownW / width, (float)shownH / height));
    }
    (void)sprite;
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t start = history->mark();
    multi->setNoMerge();
    if (!history->addCommand(multi.release())) {
        out.problem = {"invalid_apply", "apply", "the editor rejected the change", "check the GameObject in the Inspector"};
        return out;
    }
    if (result.animation) {
        int firstFrame = 0;
        for (const cleanup::AtlasClip& clip : clips) {
            auto* create = new CreateEntityCmd(project, sceneId, clip.name, EntityCreationType::SPRITE_ANIMATION, entity);
            create->setNoMerge();
            if (!history->addCommand(create)) {
                while (history->position() > start && history->canUndo() && history->undo()) {}
                history->dropRedo();
                out.problem = {"invalid_apply", "apply", "could not create the Sprite Animation", "try again"};
                return out;
            }
            const Entity animation = create->getEntity();
            auto props = std::make_unique<MultiPropertyCmd>();
            const ComponentType animationType = ComponentType::SpriteAnimationComponent;
            for (size_t i = 0; i < clip.frames.size(); i++) {
                props->addPropertyCmd<int>(project, sceneId, animation, animationType, "frames[" + std::to_string(i) + "]", firstFrame + (int)i);
                props->addPropertyCmd<int>(project, sceneId, animation, animationType, "framesTime[" + std::to_string(i) + "]", i < clip.durationsMs.size() ? clip.durationsMs[i] : 100);
            }
            props->addPropertyCmd<unsigned int>(project, sceneId, animation, animationType, "framesSize", (unsigned int)clip.frames.size());
            props->addPropertyCmd<unsigned int>(project, sceneId, animation, animationType, "framesTimeSize", (unsigned int)clip.frames.size());
            props->addPropertyCmd<bool>(project, sceneId, animation, animationType, "loop", true);
            props->addPropertyCmd<Entity>(project, sceneId, animation, ComponentType::ActionComponent, "target", entity);
            props->setNoMerge();
            if (!history->addCommand(props.release())) {
                while (history->position() > start && history->canUndo() && history->undo()) {}
                history->dropRedo();
                out.problem = {"invalid_apply", "apply", "the editor rejected the Sprite Animation", "try again"};
                return out;
            }
            firstFrame += (int)clip.frames.size();
        }
    }
    history->groupSince(start);
    sp->isModified = true;
    out.ok = true;
    out.undoName = result.animation ? "Assign Cleaned Sprite Sheet" : "Assign Cleaned Sprite";
    return out;
}

ApplyOutcome applyAsepriteOutput(Project* project, SceneProject* sp, Entity entity, const std::string& assetPath) {
    ApplyOutcome out;
    epx::aseprite::SheetSpriteRequest request;
    request.sheet = assetPath;
    std::vector<Command*> commands;
    epx::aseprite::SheetSpriteResult setup;
    Problem problem;
    if (!epx::aseprite::sheetSpriteCommands(project, sp->id, entity, request, commands, setup, problem)) {
        out.problem = problem;
        return out;
    }
    if (!epx::aseprite::runAsOneStep(sp->id, std::move(commands))) {
        out.problem = {"invalid_apply", "apply", "the editor rejected the sprite change", "check the GameObject in the Inspector"};
        return out;
    }
    sp->isModified = true;
    out.ok = true;
    out.undoName = "Assign Cleaned Sprite Sheet";
    return out;
}

void finishFix(const std::shared_ptr<FixState>& state, McpContext& ctx, const std::optional<UndoOrigin>& origin, bool ranOk) {
    const JobPtr& job = state->job;
    Project* project = ctx.project();
    if (state->cancel->load() && job->cancelRequested()) {
        job->markCancelled("Cancelled: nothing was written.");
        return;
    }
    if (!ranOk) {
        job->fail("fix_pixel_art failed.", Json{{"errors", Json::array({problemJson(state->result.problem)})}});
        return;
    }
    if (!project || project->getProjectPath() != state->root) {
        job->fail("fix_pixel_art: another project was opened.", Json{{"errors", Json::array({problemJson({"project_changed", "", "another project was opened during the fix", "call fix_pixel_art again"})})}});
        return;
    }
    McpWriteScope writeScope;
    std::optional<UndoOriginScope> attribution;
    if (origin) attribution.emplace(*origin);

    Json data = cleanup::toJson(state->prepared, state->result);
    data["job_id"] = job->id();
    std::string text = epx::verdictName(state->result.report.verdict) + std::string(": ") + state->result.report.summary;
    const epx::PixelImage preview = cleanup::renderPreview(state->prepared, state->result, state->image, state->maxWidth);
    if (!preview.empty()) {
        const std::string png = preview.encodePng();
        state->images.push_back({Base64::encode(reinterpret_cast<const uint8_t*>(png.data()), png.size()), "image/png"});
        data["image"] = {{"kind", cleanup::previewKindName(state->image)}, {"width", preview.width()}, {"height", preview.height()}};
    }
    if (!state->prepared.spec.output.write) {
        if (state->result.frames.empty() && state->result.report.verdict == epx::Verdict::NeedsPerson) {
            data["next"] = "Nothing to write: set recipe.grid {kind: \"cells\", cell_width: 1-128}, or read jasper://docs/pixel-art-cleanup.";
        } else if (state->result.report.verdict == epx::Verdict::NeedsPerson) {
            data["next"] = "needs_person: look at the guards and the image; set recipe.grid or palette and preview again. output: true is refused without force: true.";
        } else {
            data["next"] = "Preview only, nothing written: look at the image, adjust recipe if it is wrong, then the same arguments plus output: true write exactly these pixels.";
        }
        text += " Preview only: nothing written.";
        state->data = std::move(data);
        state->text = text;
        job->succeed(text, state->data);
        return;
    }

    std::vector<cleanup::OutputFile> files;
    Problem problem;
    const std::vector<Problem> single;
    if (!cleanup::encodeOutputs(state->prepared, state->result, files, problem)) {
        Json list = Json::array({problemJson(problem)});
        job->fail(problem.kind + ": " + problem.message + (problem.fix.empty() ? "" : " Fix: " + problem.fix), Json{{"errors", list}});
        return;
    }
    if (state->apply) {
        SceneProject* sp = project->getScene(state->applyScene);
        if (!sp || !sp->scene || !sp->scene->isEntityCreated(state->applyEntity) || sp->playState != ScenePlayState::STOPPED) {
            Json list = Json::array({problemJson({"invalid_apply", "apply", sp ? "the GameObject to apply to is gone, or its scene is playing" : "the scene to apply to is gone", "stop play and name a Sprite GameObject"})});
            job->fail("Nothing was written:", Json{{"errors", list}});
            return;
        }
    }
    if (!cleanup::writeUndoable(project, files, problem)) {
        Json list = Json::array({problemJson(problem)});
        job->fail(problem.kind + ": " + problem.message + (problem.fix.empty() ? "" : " Fix: " + problem.fix), Json{{"errors", list}});
        return;
    }
    Json written = Json::array();
    std::string firstAseprite;
    std::string firstPng;
    for (const cleanup::OutputFile& file : files) {
        written.push_back({{"role", file.role}, {"path", file.path}});
        if (file.role == "aseprite" && firstAseprite.empty()) firstAseprite = file.path;
        if (file.role == "png" && firstPng.empty()) firstPng = file.path;
    }
    data["written"] = written;
    if (UndoTimeline::EntryView last = UndoTimeline::nextUndo(NULL_PROJECT_SCENE); last.entry && last.project) {
        data["undo"] = {{"id", last.entry->info.id}, {"name", last.entry->name()}};
    }
    text += " Wrote " + (firstAseprite.empty() ? firstPng : firstAseprite) + " as one undo step.";

    auto finishOk = [state, job, data, text]() mutable {
        state->data = std::move(data);
        state->text = text;
        job->succeed(text, state->data);
    };
    if (!state->apply) {
        finishOk();
        return;
    }
    // apply: the second, scene step; an .aseprite is applied once its import is in
    if (!firstAseprite.empty()) {
        std::string assetPath;
        std::string error;
        if (!McpValues::assetPathFromJson(project, firstAseprite, assetPath, error)) assetPath = firstAseprite;
        auto apply = [state, job, data, text, assetPath, sceneId = state->applyScene, entity = state->applyEntity, &ctx]() mutable {
            Project* current = ctx.project();
            SceneProject* sp = current ? current->getScene(sceneId) : nullptr;
            if (!sp || !sp->scene || !sp->scene->isEntityCreated(entity)) {
                data["apply_error"] = "the GameObject went away; the file was written (undo removes it)";
            } else {
                ApplyOutcome outcome = applyAsepriteOutput(current, sp, entity, assetPath);
                if (outcome.ok) data["applied"] = {{"undo", {{"scope", "scene"}, {"scene", sceneId}, {"name", outcome.undoName}}}};
                else data["apply_error"] = problemJson(outcome.problem);
            }
            state->data = std::move(data);
            state->text = text;
            job->succeed(text, state->data);
        };
        epx::aseprite::AsepriteImporter::get().whenImported(assetPath, apply);
        return;
    }
    SceneProject* sp = project->getScene(state->applyScene);
    std::string assetPath;
    std::string error;
    if (!McpValues::assetPathFromJson(project, firstPng, assetPath, error)) assetPath = firstPng;
    ApplyOutcome outcome = applyPngOutput(project, sp, state->applyEntity, assetPath, state->result);
    if (outcome.ok) data["applied"] = {{"undo", {{"scope", "scene"}, {"scene", state->applyScene}, {"name", outcome.undoName}}}};
    else data["apply_error"] = problemJson(outcome.problem);
    finishOk();
}

void fixPixelArtTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    auto state = std::make_shared<FixState>();
    cleanup::Spec spec;
    std::vector<Problem> problems;
    if (!parseFixArgs(call, spec, *state, problems)) {
        failCall(call, "fix_pixel_art did not run:", problems);
        return;
    }
    if (args.contains("apply") && !args["apply"].is_null()) {
        state->apply = true;
        if (!spec.output.write) {
            failCall(call, "fix_pixel_art did not run:", {{"invalid_apply", "apply", "apply assigns the written file: add output: true", "add output: true"}});
            return;
        }
        std::string error;
        SceneProject* scene = McpRefs::resolveScene(project, Json(), error);
        if (!scene || !scene->scene) {
            failCall(call, "fix_pixel_art did not run:", {{"invalid_apply", "apply", error, "open the Sprite's scene"}});
            return;
        }
        state->applyScene = scene->id;
        state->applyEntity = McpRefs::resolveEntity(scene, args["apply"], error);
        if (state->applyEntity == NULL_ENTITY) {
            failCall(call, "fix_pixel_art did not run:", {{"invalid_apply", "apply", error, "name a Sprite GameObject of the selected scene"}});
            return;
        }
        if (!scene->scene->findComponent<SpriteComponent>(state->applyEntity)) {
            failCall(call, "fix_pixel_art did not run:", {{"invalid_apply", "apply", scene->scene->getEntityName(state->applyEntity) + " is not a Sprite", "name a Sprite GameObject"}});
            return;
        }
        if (scene->playState != ScenePlayState::STOPPED) {
            failCall(call, "fix_pixel_art did not run:", {{"invalid_apply", "apply", "scene '" + scene->name + "' is playing", "stop it, then apply"}});
            return;
        }
    }
    double wait = kDefaultWaitSeconds;
    if (args.contains("timeout_seconds")) {
        if (!args["timeout_seconds"].is_number() || args["timeout_seconds"].get<double>() < 1.0 || args["timeout_seconds"].get<double>() > 3600.0) {
            failCall(call, "fix_pixel_art did not run:", {{"invalid_value", "timeout_seconds", "timeout_seconds is 1...3600", "e.g. 60"}});
            return;
        }
        wait = args["timeout_seconds"].get<double>();
    }

    std::optional<cleanup::Prepared> prepared = cleanup::prepare(project, spec, problems);
    if (!prepared) {
        failCall(call, "fix_pixel_art did not run:", problems);
        return;
    }
    state->prepared = std::move(*prepared);
    state->root = project->getProjectPath();
    state->job = ctx.jobs().create("fix_pixel_art", "Pixel Art Cleanup");
    state->job->setProgress(-1.0, "fixing " + state->prepared.sourcePath);
    state->job->setCancelHandler([cancel = state->cancel]() { *cancel = true; });

    McpContext* context = &ctx;
    const std::optional<UndoOrigin> origin = call->attributed() ? std::optional<UndoOrigin>(call->getUndoOrigin()) : std::nullopt;
    jobs::submit([state, context, origin]() {
        bool ok = true;
        try {
            state->result = cleanup::run(state->prepared, nullptr, state->cancel.get());
            if (!state->result.problem.empty()) ok = false;
        } catch (const std::exception& e) {
            state->result.problem = {"failed", "", e.what(), "call again"};
            ok = false;
        }
        getEditorHost().enqueueMainThreadTask([state, context, origin, ok]() {
            try {
                finishFix(state, *context, origin, ok);
            } catch (const std::exception& e) {
                if (state->job->status() == JobStatus::Running) {
                    state->job->fail(std::string("fix_pixel_art failed: ") + e.what(), Json{{"errors", Json::array({problemJson({"failed", "", e.what(), "call again"})})}});
                }
            }
        });
    }, {jobs::Lane::Background, jobs::GateClass::Default, "fix-pixel-art"});

    call->detach();
    replyWhenJobDone(call, state->job, true, wait, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        ToolResult reply = ToolResult::success(state->text.empty() ? finished.message() : state->text, result);
        reply.images = state->images;
        return reply;
    });
}

// ---- lint_pixel_art

struct LintState {
    cleanup::LintPrepared prepared;
    cleanup::LintOutcome outcome;
    bool image = false;
    int maxWidth = cleanup::kDefaultPreviewWidth;
};

ToolResult lintReply(const LintState& state) {
    Json data = cleanup::toJson(state.prepared, state.outcome);
    std::string text = epx::verdictName(state.outcome.verdict) + std::string(": ") + state.outcome.summary + ".";
    for (const epx::lint::LintResult& entry : state.outcome.report.results) {
        if (entry.status == epx::lint::LintStatus::Pass || entry.status == epx::lint::LintStatus::Skip) continue;
        text += "\n" + std::string(epx::lint::lintStatusName(entry.status)) + " " + entry.id + ": " + entry.message + (entry.fix.empty() ? "" : " Fix: " + entry.fix);
    }
    ToolResult reply = ToolResult::success(text, data);
    if (state.image) {
        const epx::PixelImage image = cleanup::renderLintImage(state.prepared, state.outcome, state.maxWidth);
        if (!image.empty()) {
            const std::string png = image.encodePng();
            reply.images.push_back({Base64::encode(reinterpret_cast<const uint8_t*>(png.data()), png.size()), "image/png"});
            reply.structured["image"] = {{"width", image.width()}, {"height", image.height()}};
        }
    }
    return reply;
}

void lintPixelArtTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    std::vector<Problem> problems;
    cleanup::LintSpec spec;
    auto state = std::make_shared<LintState>();
    if (args.contains("source")) {
        if (!args["source"].is_string()) problems.push_back({"source_missing", "source", "source must be a path", "a project-relative image or .aseprite path"});
        else spec.source = args["source"].get<std::string>();
    }
    resolveEntityArg(call, args, spec.sceneId, spec.entity, problems);
    if (args.contains("frames") && !args["frames"].is_null()) spec.frames = args["frames"];
    spec.layer = args.value("layer", "");
    if (args.contains("lints") && !args["lints"].is_null()) stringList(args["lints"], "lints", spec.lints, problems);
    paletteArg(args, spec.palette, problems);
    if (args.contains("rules") && !args["rules"].is_null()) {
        const Json& rules = args["rules"];
        if (!rules.is_object()) {
            problems.push_back({"invalid_value", "rules", "rules is {max_colors, sizes, transparent_background}", "e.g. {\"max_colors\": 16}"});
        } else {
            for (auto it = rules.begin(); it != rules.end(); ++it) {
                if (it.key() == "max_colors" && it.value().is_number_integer()) {
                    spec.rules.maxColors = it.value().get<int>();
                } else if (it.key() == "transparent_background" && it.value().is_boolean()) {
                    spec.rules.transparentBackground = it.value().get<bool>();
                } else if (it.key() == "sizes" && it.value().is_array()) {
                    for (const Json& size : it.value()) {
                        if (size.is_array() && size.size() == 2 && size[0].is_number_integer() && size[1].is_number_integer()) spec.rules.sizes.push_back({size[0].get<int>(), size[1].get<int>()});
                        else problems.push_back({"invalid_value", "rules.sizes", "sizes are [[w, h], ...]", "e.g. [[16, 16], [32, 32]]"});
                    }
                } else {
                    problems.push_back({"invalid_value", "rules." + it.key(), "rules holds max_colors (integer), sizes ([[w, h]]), transparent_background (boolean)", "e.g. {\"max_colors\": 16}"});
                }
            }
        }
    }
    if (args.contains("backdrops") && !args["backdrops"].is_null()) stringList(args["backdrops"], "backdrops", spec.backdrops, problems);
    if (args.contains("loop") && !args["loop"].is_null()) {
        if (!args["loop"].is_boolean()) problems.push_back({"invalid_value", "loop", "loop must be true or false", "true"});
        else spec.loop = args["loop"].get<bool>();
    }
    if (args.contains("image")) {
        if (!args["image"].is_boolean()) problems.push_back({"invalid_value", "image", "image must be true or false", "true"});
        else state->image = args["image"].get<bool>();
    }
    if (args.contains("fail_on")) {
        const std::string failOn = args.value("fail_on", "");
        if (failOn == "warn") spec.failOnWarn = true;
        else if (failOn != "fail") problems.push_back({"invalid_value", "fail_on", "fail_on is fail or warn", "\"fail\" or \"warn\""});
    }
    if (spec.source.empty() && spec.entity == NULL_ENTITY) problems.push_back({"source_missing", "source", "give source (an image or .aseprite) or entity (a Sprite)", "pass source"});
    if (!problems.empty()) {
        failCall(call, "lint_pixel_art did not run:", problems);
        return;
    }
    std::optional<cleanup::LintPrepared> prepared = cleanup::prepareLint(project, spec, problems);
    if (!prepared) {
        failCall(call, "lint_pixel_art did not run:", problems);
        return;
    }
    state->prepared = std::move(*prepared);
    size_t pixels = 0;
    for (const epx::PixelImage& frame : state->prepared.frames) pixels += (size_t)frame.width() * frame.height();
    if (pixels <= kLintInlinePixels) {
        state->outcome = cleanup::runLint(state->prepared);
        call->reply(lintReply(*state));
        return;
    }
    JobPtr job = ctx.jobs().create("lint_pixel_art", "Pixel Art Lint");
    jobs::submit([state, job]() {
        try {
            state->outcome = cleanup::runLint(state->prepared);
            job->succeed("lint done", Json::object());
        } catch (const std::exception& e) {
            job->fail(e.what());
        }
    }, {jobs::Lane::Background, jobs::GateClass::Default, "lint-pixel-art"});
    call->detach();
    replyWhenJobDone(call, job, true, kDefaultWaitSeconds, [state](const McpJob& finished) {
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), Json{{"job_id", finished.id()}});
        ToolResult reply = lintReply(*state);
        reply.structured["job_id"] = finished.id();
        return reply;
    });
}

}
}

void registerPixelArtCleanupTools(McpRegistry& registry) {
    using namespace schema;
    {
        ToolDef tool;
        tool.name = "fix_pixel_art";
        tool.title = "Fix Pixel Art";
        tool.description =
            "Cleans AI or loose pixel art: finds the grid, reads each cell, snaps to a palette, slices sheets into clips; verdict clean, fixable or needs_person. "
            "Without output it previews (image, nothing written); the same arguments plus output:true write it. Recipe keys: jasper://docs/pixel-art-cleanup. lint_pixel_art only checks.";
        const Json outputSchema = Json{{"type", {"boolean", "string", "object"}}};
        tool.inputSchema = object({
            {"source", string("")},
            {"entity", entityRef("Sprite as the source.")},
            {"recipe", anyObject("Partial recipe; unknown keys fail.")},
            {"palette", Json{{"type", {"string", "array"}}}},
            {"sheet", stringEnum("", {"auto", "off", "on"})},
            {"rerun", boolean("source is an output: re-run its stored recipe.")},
            {"output", outputSchema},
            {"format", stringEnum("", {"png", "aseprite"})},
            {"overwrite", boolean("")},
            {"force", boolean("Write a needs_person result, recorded as forced.")},
            {"apply", entityRef("On write: Sprite to give the result.")},
            {"image", stringEnum("", {"compare", "result", "grid", "lost", "lints", "none"})},
            {"max_width", integer("", cleanup::kMinPreviewWidth, cleanup::kMaxPreviewWidth)},
            {"timeout_seconds", number("Seconds the call waits before it answers with the job.", 1.0, 3600.0)}
        });
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.annotations.idempotent = false;
        tool.writesFiles = [](const Json& args) { return args.contains("output") && !args["output"].is_null() && args["output"] != Json(false); };
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Edit;
        tool.handler = pixelcleanuptools::fixPixelArtTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "lint_pixel_art";
        tool.title = "Lint Pixel Art";
        tool.description =
            "Checks an image, .aseprite (frames: tag or numbers; layer) or Sprite: craft lints and rule lints as PASS, WARN, FAIL or SKIP with the measured value, threshold and pixels, "
            "plus a verdict. Read-only. Never loosen a threshold to pass: fix the art (edit_sprite) or run fix_pixel_art.";
        std::vector<std::string> ids;
        for (const char* id : epx::lint::lintIds()) ids.emplace_back(id);
        tool.inputSchema = object({
            {"source", string("")},
            {"entity", entityRef("Sprite as the source.")},
            {"frames", Json{{"type", {"string", "array"}}}},
            {"layer", string("")},
            {"lints", array(stringEnum("", ids), "")},
            {"palette", Json{{"type", {"string", "array"}}}},
            {"rules", object({{"max_colors", integer("")}, {"sizes", array(array(integer(""), ""), "")}, {"transparent_background", boolean("")}})},
            {"backdrops", array(string(""), "")},
            {"loop", boolean("")},
            {"image", boolean("Frames with spots marked.")},
            {"fail_on", stringEnum("", {"fail", "warn"})}
        });
        tool.annotations.readOnly = true;
        tool.annotations.destructive = false;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Read;
        tool.handler = pixelcleanuptools::lintPixelArtTool;
        registry.addTool(std::move(tool));
    }
}

}
