// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// check_pixel_art (docs/specs/S42-pixel-art-mode.md R11-R14, R16, R22, §6.3): the pixel-art validator
// on the live project. The input is read on the main thread (closed scenes and metas from the
// input cache when unchanged); the rules run inline when the last check of this project was quick,
// else on an S06 Background worker as a job the call waits a second for. A fix previews its changes; the same
// call with output: true applies them as one undo step and checks again. image: true returns the
// first pixel-art scene's base-resolution frame (the Game view at Pixel Scale 1x), upscaled by
// whole numbers, with each finding on screen boxed and labelled F<n>.

#include "McpTools.h"
#include "mcp/McpCall.h"
#include "command/UndoOrigin.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/tools/PlayViewCommon.h"
#include "AppSettings.h"
#include "EditorHost.h"
#include "Project.h"
#include "pixelart/PixelArtCheck.h"
#include "pixelart/PixelArtFix.h"
#include "pixelart/PixelArtInput.h"
#include "pixelart/RgbaImage.h"
#include "util/Base64.h"
#include "window/GameWindow.h"
#include "jobs/JobSystem.h"

#include "component/CameraComponent.h"
#include "component/PixelPerfectCameraComponent.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <chrono>
#include <map>
#include <optional>

namespace jasperengine::editor::mcp {

namespace epx = jasperengine::editor::pixelart;
namespace px = jasperengine::pixelart;

namespace pixelarttools {
namespace {

using Problem = epx::PixelProblem;

// before a project's first measured check: past this much input (drawables, textures) the rules may
// take a second, so they run as a job
constexpr size_t kJobInput = 2000;
// a project whose last check's rules took longer than this runs them as a job
constexpr double kInlineSeconds = 0.25;
constexpr double kJobWaitSeconds = 1.0;

// the rules' time of each project's last check (main thread)
std::map<std::filesystem::path, double>& measuredSeconds() {
    static std::map<std::filesystem::path, double> seconds;
    return seconds;
}

epx::CheckResult timedRun(const epx::PixelArtCheckInput& input, const std::vector<std::string>& rules, double& seconds) {
    const auto start = std::chrono::steady_clock::now();
    epx::CheckResult result = epx::run(input, rules);
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

struct CheckArgs {
    epx::Scope scope;
    std::vector<std::string> rules;
    std::vector<std::string> fix;
    bool output = false;
    bool image = false;
    int maxWidth = 1024;
};

Json problemJson(const Problem& p) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix.empty() ? std::string("see jasper://docs/pixel-art") : p.fix}};
    if (!p.field.empty()) j["field"] = p.field;
    return j;
}

void failCall(const CallPtr& call, const std::string& headline, const std::vector<Problem>& problems) {
    std::string text = headline;
    Json list = Json::array();
    for (const Problem& p : problems) {
        text += "\n- " + p.kind + ": " + p.message + (p.fix.empty() ? "" : " Fix: " + p.fix);
        list.push_back(problemJson(p));
    }
    call->fail(text, Json{{"errors", list}});
}

bool stringList(const Json& args, const char* key, std::vector<std::string>& out, std::vector<Problem>& problems) {
    if (!args.contains(key) || args[key].is_null()) return true;
    const Json& value = args[key];
    if (value.is_string()) {
        out.push_back(value.get<std::string>());
        return true;
    }
    if (!value.is_array()) {
        problems.push_back({"invalid_value", key, std::string(key) + " must be a list of strings", "e.g. [\"" + std::string(key) + "\"]"});
        return false;
    }
    for (const Json& item : value) {
        if (!item.is_string()) {
            problems.push_back({"invalid_value", key, std::string(key) + " must hold strings, got " + item.dump(), "give strings"});
            return false;
        }
        out.push_back(item.get<std::string>());
    }
    return true;
}

bool parseArgs(const Json& args, bool playing, CheckArgs& out, std::vector<Problem>& problems) {
    stringList(args, "scenes", out.scope.scenes, problems);
    stringList(args, "paths", out.scope.paths, problems);
    stringList(args, "rules", out.rules, problems);
    stringList(args, "fix", out.fix, problems);
    for (const std::string& rule : out.rules) {
        if (!epx::isKnownRule(rule)) {
            std::string known;
            for (const std::string& id : epx::ruleIds())
                if (epx::isKnownRule(id)) known += (known.empty() ? "" : ", ") + id;
            problems.push_back({"unknown_rule", "rules", "\"" + rule + "\" is not a rule of this editor", "use one of " + known});
        }
    }
    if (args.contains("output") && !args["output"].is_boolean())
        problems.push_back({"invalid_value", "output", "output must be true or false", "output: true applies the fix"});
    else
        out.output = args.value("output", false);
    if (args.contains("image") && !args["image"].is_boolean())
        problems.push_back({"invalid_value", "image", "image must be true or false", "image: true returns the 1x frame"});
    else
        out.image = args.value("image", false);
    if (args.contains("max_width")) {
        if (!args["max_width"].is_number_integer() || args["max_width"].get<int>() < 256 || args["max_width"].get<int>() > 2048)
            problems.push_back({"invalid_value", "max_width", "max_width must be a whole number 256...2048", "e.g. 1280"});
        else
            out.maxWidth = args["max_width"].get<int>();
    }
    if (out.output && out.fix.empty())
        problems.push_back({"invalid_value", "output", "output applies a fix, and no fix was given",
                            "add fix: [\"all\"] (or keys, F<n>, rule ids), first without output to preview"});
    if (out.output && playing)
        problems.push_back({"not_while_playing", "output", "fixes are not applied while a scene plays (the check works)",
                            "stop play (play {action: \"stop\"}), then call again with output"});
    return problems.empty();
}

std::string countsText(const epx::CheckResult& result) {
    return std::to_string(result.count(epx::RuleStatus::Fail)) + " FAIL, " + std::to_string(result.count(epx::RuleStatus::Warn)) + " WARN";
}

size_t fixableCount(const epx::CheckResult& result) {
    size_t n = 0;
    for (const epx::Finding& finding : result.findings) n += finding.options.empty() ? 0 : 1;
    return n;
}

// ---- the image: the 1x frame with the findings boxed

// a 3 x 5 pixel font for the labels ("F" and digits), rows top first, 3 bits each
const uint8_t* glyphRows(char c) {
    static const uint8_t digits[10][5] = {{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
                                          {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};
    static const uint8_t letterF[5] = {7, 4, 6, 4, 4};
    if (c == 'F') return letterF;
    if (c >= '0' && c <= '9') return digits[c - '0'];
    return nullptr;
}

void fillRect(epx::RgbaImage& image, int x0, int y0, int x1, int y1, px::Rgba8 color) {
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(image.width, x1); y1 = std::min(image.height, y1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            image.set(x, y, color);
}

void drawLabel(epx::RgbaImage& image, int x, int y, const std::string& text, int scale, px::Rgba8 color) {
    const int width = (int)text.size() * 4 * scale + scale;
    fillRect(image, x, y, x + width, y + 7 * scale, {0, 0, 0, 255});
    for (size_t i = 0; i < text.size(); i++) {
        const uint8_t* rows = glyphRows(text[i]);
        if (!rows) continue;
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (rows[row] & (4 >> col))
                    fillRect(image, x + scale + ((int)i * 4 + col) * scale, y + scale + row * scale,
                             x + scale + ((int)i * 4 + col + 1) * scale, y + scale + (row + 1) * scale, color);
    }
}

void strokeRect(epx::RgbaImage& image, int x0, int y0, int x1, int y1, int thickness, px::Rgba8 color) {
    fillRect(image, x0, y0, x1, y0 + thickness, color);
    fillRect(image, x0, y1 - thickness, x1, y1, color);
    fillRect(image, x0, y0, x0 + thickness, y1, color);
    fillRect(image, x1 - thickness, y0, x1, y1, color);
}

struct ImageBox {
    std::string label;
    int x0, y0, x1, y1;     // base pixels, image rows top first
    bool fail;
};

// the findings of the scene on its frame: the world quad of each drawable through the camera's
// view-projection, in base pixels of the base image
std::vector<ImageBox> findingBoxes(Project* project, uint32_t sceneId, const epx::CheckResult& result, const std::string& scenePath,
                                   int baseWidth, int baseHeight, const epx::PixelArtCheckInput& input) {
    std::vector<ImageBox> boxes;
    SceneProject* sp = project->getScene(sceneId);
    if (!sp || !sp->scene || sp->mainCamera == NULL_ENTITY) return boxes;
    const CameraComponent* camera = sp->scene->findComponent<CameraComponent>(sp->mainCamera);
    if (!camera) return boxes;
    std::map<std::string, const epx::PixelArtDrawable*> drawables;
    for (const epx::PixelArtScene& scene : input.scenes)
        if (scene.path == scenePath)
            for (const epx::PixelArtDrawable& d : scene.drawables) drawables[d.entity] = &d;
    for (const epx::Finding& finding : result.findings) {
        if (finding.target.value("kind", "") != "entity" && finding.target.value("kind", "") != "reference") continue;
        if (finding.target.value("scene", "") != scenePath) continue;
        auto it = drawables.find(finding.target.value("entity", ""));
        if (it == drawables.end() || !it->second->hasTransform) continue;
        const epx::PixelArtDrawable& d = *it->second;
        const double ppu = d.pixelsPerUnit > 0 ? d.pixelsPerUnit : std::max(1, input.project.pixelsPerUnit);
        const float w = d.width > 0 ? d.width : 1.0f, h = d.height > 0 ? d.height : 1.0f;
        int minX = 1 << 30, minY = 1 << 30, maxX = -(1 << 30), maxY = -(1 << 30);
        for (int corner = 0; corner < 4; corner++) {
            const float cx = ((corner & 1) ? w : 0.0f) - d.pivot.x, cy = ((corner & 2) ? h : 0.0f) - d.pivot.y;
            const Vector3 local((float)(cx / ppu) * d.worldScale.x, (float)(cy / ppu) * d.worldScale.y, 0.0f);
            const Vector3 world = d.worldPosition + d.worldRotation * local;
            const Vector4 clip = camera->viewProjectionMatrix * Vector4(world.x, world.y, world.z, 1.0f);
            if (std::fabs(clip.w) < 1e-6f) continue;
            const float nx = clip.x / clip.w, ny = clip.y / clip.w;
            const int x = (int)std::floor((nx * 0.5f + 0.5f) * baseWidth + 0.5f);
            const int y = (int)std::floor((1.0f - (ny * 0.5f + 0.5f)) * baseHeight + 0.5f);
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
        }
        if (maxX < 0 || maxY < 0 || minX > baseWidth || minY > baseHeight || minX > maxX) continue;
        boxes.push_back({finding.id, minX, minY, std::max(maxX, minX + 1), std::max(maxY, minY + 1), finding.status == epx::RuleStatus::Fail});
    }
    return boxes;
}

// the capture of the Game view at 1x, cropped to the base image, scaled up and annotated
bool annotateFrame(const ToolImage& captured, int baseWidth, int baseHeight, const jasperengine::pixelart::PixelPerfectLayout& layout,
                   const std::vector<ImageBox>& boxes, int maxWidth, ToolImage& out, Json& info, std::string& error) {
    const std::vector<unsigned char> bytes = Base64::decode(captured.data);
    std::optional<epx::RgbaImage> frame = epx::decodeRgbaImage(bytes.data(), bytes.size(), &error);
    if (!frame) return false;
    const int x0 = (int)layout.viewport.getX(), y0 = (int)layout.viewport.getY();
    epx::RgbaImage base(baseWidth, baseHeight);
    for (int y = 0; y < baseHeight; y++)
        for (int x = 0; x < baseWidth; x++)
            if (x0 + x < frame->width && y0 + y < frame->height) base.set(x, y, frame->at(x0 + x, y0 + y));
    const int scale = std::max(1, maxWidth / std::max(1, baseWidth));
    epx::RgbaImage big(baseWidth * scale, baseHeight * scale);
    for (int y = 0; y < big.height; y++)
        for (int x = 0; x < big.width; x++)
            big.set(x, y, base.at(x / scale, y / scale));
    const int thickness = std::max(1, scale / 2);
    const int labelScale = std::max(1, scale / 2);
    for (const ImageBox& box : boxes) {
        const px::Rgba8 color = box.fail ? px::Rgba8{255, 48, 48, 255} : px::Rgba8{255, 200, 0, 255};
        strokeRect(big, box.x0 * scale, box.y0 * scale, box.x1 * scale, box.y1 * scale, thickness, color);
        drawLabel(big, box.x0 * scale, std::max(0, box.y0 * scale - 7 * labelScale), box.label, labelScale, color);
    }
    const std::string png = epx::encodeRgbaPng(big);
    out = ToolImage{Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"};
    info = {{"width", big.width}, {"height", big.height}, {"scale", scale}, {"base", Json::array({baseWidth, baseHeight})}, {"boxes", boxes.size()}};
    return true;
}

struct CheckState {
    CheckArgs args;
    std::shared_ptr<epx::PixelArtCheckInput> input;
    epx::CheckResult result;
    std::string failure;
    std::string text;
    Json data;
    std::vector<Problem> problems;
    JobPtr job;
    std::filesystem::path root;
};

// after the rules ran: the plan, its application and the re-check (main thread); false with problems
bool finishCheck(McpContext& ctx, CheckState& state) {
    Project* project = ctx.project();
    const epx::CheckResult& result = state.result;
    Json data = result.toJson();
    std::string text = "Pixel art check: " + std::string(epx::verdictName(result.verdict)) + ", " + countsText(result) + " (" +
                       std::to_string(result.rules.size()) + " rules).";
    const size_t fixable = fixableCount(result);
    std::string next;
    if (!state.args.fix.empty()) {
        const epx::FixPlan plan = epx::plan(result, state.args.fix);
        if (!plan.unknown.empty()) {
            for (const std::string& key : plan.unknown)
                state.problems.push_back({"not_found", "fix", "\"" + key + "\" matches no finding of the current state",
                                          "call check_pixel_art without fix to see the findings' keys, then fix by key, F<n>, rule id or \"all\""});
            return false;
        }
        for (const std::string& bad : plan.badOptions)
            state.problems.push_back({"invalid_value", "fix", "\"" + bad + "\": that finding has no such option", "use an option id of its fix.options"});
        if (!state.problems.empty()) return false;
        Json notFixable = Json::array();
        for (const std::string& key : plan.needsPerson) notFixable.push_back(key);
        if (!notFixable.empty()) data["not_fixable"] = notFixable;
        data["fix_plan"] = plan.toJson();
        if (!state.args.output) {
            text += " Fix plan: " + std::to_string(plan.fixes.size()) + " fix(es), " + std::to_string(plan.changeCount()) + " change(s); nothing changed.";
            next = plan.fixes.empty() ? "Nothing to apply: the selected findings need a person (needs_person)."
                                      : "Preview only, nothing changed: the same call with output: true applies these " +
                                            std::to_string(plan.changeCount()) + " change(s) as one undo step.";
        } else if (plan.fixes.empty()) {
            next = "Nothing applied: the selected findings need a person (needs_person).";
        } else {
            epx::FixApplyReport report;
            std::string error;
            const std::string stepName = epx::fixStepName(plan);
            if (!epx::applyFixPlan(*project, plan, stepName, report, error, &state.args.scope)) {
                if (error.rfind("stale: ", 0) == 0)
                    state.problems.push_back({"stale", "fix", "nothing changed: " + error.substr(7),
                                              "call check_pixel_art again and fix from its result"});
                else
                    state.problems.push_back({"apply_failed", "fix", "the fix could not be applied (nothing changed): " + error, "check the findings again"});
                return false;
            }
            std::vector<Problem> errors;
            const epx::PixelArtCheckInput after = epx::inputFromProject(*project, state.args.scope, errors);
            const epx::CheckResult again = epx::run(after, state.args.rules);
            data["applied"] = {{"undo_step", report.undo}, {"changed", report.changed}, {"recooking", report.recooking}};
            data["after"] = {{"verdict", epx::verdictName(again.verdict)},
                             {"counts", {{"fail", again.count(epx::RuleStatus::Fail)}, {"warn", again.count(epx::RuleStatus::Warn)}}}};
            text += " Applied " + std::to_string(plan.fixes.size()) + " fix(es) as \"" + stepName + "\"; after: " +
                    epx::verdictName(again.verdict) + ", " + countsText(again) + ".";
            next = report.savedScenes.empty() ? std::string() : "Scene edits are unsaved: save_scene keeps them.";
            if (again.verdict == epx::Verdict::NeedsPerson)
                next += std::string(next.empty() ? "" : " ") + "The findings left need a person: report them instead of changing scale or art.";
        }
    } else if (fixable > 0) {
        next = "check_pixel_art {fix: [\"all\"]} previews " + std::to_string(fixable) + " fix(es); add output: true to apply.";
    } else if (result.verdict == epx::Verdict::NeedsPerson) {
        next = "The findings need a person: report them (needs_person) instead of changing scale or art.";
    }
    if (!next.empty()) {
        data["next"] = next;
        text += " " + next;
    }
    state.data = data;
    state.text = text;
    return true;
}

void replyWithImage(const CallPtr& call, std::shared_ptr<CheckState> state) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    // the first pixel-art scene with findings on screen, else the first pixel-art scene
    std::string scenePath;
    for (const epx::PixelArtScene& scene : state->input->scenes)
        if (scene.pixelArt && scenePath.empty()) scenePath = scene.path;
    for (const epx::Finding& finding : state->result.findings) {
        const std::string scene = finding.target.value("scene", "");
        if (!scene.empty()) {
            scenePath = scene;
            break;
        }
    }
    uint32_t sceneId = NULL_PROJECT_SCENE;
    for (SceneProject& scene : project->getScenes())
        if (epx::scenePathOf(*project, scene) == scenePath) sceneId = scene.id;
    GameWindow* gw = ctx.gameWindow();
    if (scenePath.empty() || sceneId == NULL_PROJECT_SCENE || !gw) {
        state->data["image_note"] = "no pixel-art scene to draw";
        call->reply(ToolResult::success(state->text, state->data));
        return;
    }
    if (!project->getScene(sceneId)->scene) {
        std::string error;
        sceneId = project->openSceneNow(project->getProjectPath() / scenePath, false, UnsavedChoice::Cancel, &error);
        if (sceneId == NULL_PROJECT_SCENE) {
            state->data["image_note"] = "the scene could not be opened: " + error;
            call->reply(ToolResult::success(state->text, state->data));
            return;
        }
    }
    project->setSelectedSceneId(sceneId);
    const GameViewSettings previous = gw->getSettings();
    GameViewSettings oneX = previous;
    oneX.pixelScale = 1;
    gw->applySettings(oneX);
    w3::CaptureRequest request;
    request.view = "game";
    request.maxWidth = 0;
    w3::captureView(call, request, [call, state, sceneId, scenePath, previous, gw](w3::CaptureResult captured) {
        Project* project = call->ctx().project();
        jasperengine::pixelart::PixelPerfectLayout layout;
        int baseWidth = (int)project->getCanvasWidth(), baseHeight = (int)project->getCanvasHeight();
        const int frameWidth = captured.info.value("width", 0), frameHeight = captured.info.value("height", 0);
        SceneProject* sp = project->getScene(sceneId);
        if (sp && sp->scene && sp->mainCamera != NULL_ENTITY) {
            if (const PixelPerfectCameraComponent* ppc = sp->scene->findComponent<PixelPerfectCameraComponent>(sp->mainCamera)) {
                if (ppc->referenceWidth > 0) baseWidth = (int)ppc->referenceWidth;
                if (ppc->referenceHeight > 0) baseHeight = (int)ppc->referenceHeight;
            }
        }
        layout = jasperengine::pixelart::computePixelPerfectLayout(frameWidth, frameHeight, baseWidth, baseHeight,
                                                                  PixelUpscaleMode::INTEGER, 1, false, project->getPixelsPerUnit());
        const std::vector<ImageBox> boxes = findingBoxes(project, sceneId, state->result, scenePath, baseWidth, baseHeight, *state->input);
        gw->applySettings(previous);
        ToolResult reply = ToolResult::success(state->text, state->data);
        if (!captured.ok) {
            reply.structured["image_note"] = "the frame could not be captured: " + captured.error;
        } else {
            ToolImage image;
            Json info;
            std::string error;
            if (annotateFrame(captured.image, baseWidth, baseHeight, layout, boxes, state->args.maxWidth, image, info, error)) {
                reply.images.push_back(image);
                info["scene"] = scenePath;
                reply.structured["image"] = info;
            } else {
                reply.structured["image_note"] = "the frame could not be read: " + error;
            }
        }
        call->reply(std::move(reply));
    });
}

void checkPixelArtTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    auto state = std::make_shared<CheckState>();
    std::vector<Problem> problems;
    if (!parseArgs(call->args(), ctx.isPlaying(), state->args, problems)) {
        failCall(call, "check_pixel_art did not run:", problems);
        return;
    }
    state->input = std::make_shared<epx::PixelArtCheckInput>(epx::inputFromProject(*project, state->args.scope, problems));
    if (!problems.empty()) {
        failCall(call, "check_pixel_art did not run:", problems);
        return;
    }
    size_t size = state->input->textures.size();
    for (const epx::PixelArtScene& scene : state->input->scenes) size += scene.drawables.size();

    // by the measured time of the last check, the input size before the first
    const std::filesystem::path root = project->getProjectPath();
    auto measured = measuredSeconds().find(root);
    const bool quick = measured != measuredSeconds().end() ? measured->second <= kInlineSeconds : size <= kJobInput;
    if (quick || state->args.image) {
        double seconds = 0.0;
        state->result = timedRun(*state->input, state->args.rules, seconds);
        measuredSeconds()[root] = seconds;
        if (!finishCheck(ctx, *state)) {
            failCall(call, "check_pixel_art:", state->problems);
            return;
        }
        if (state->args.image) {
            replyWithImage(call, state);
            return;
        }
        call->reply(ToolResult::success(state->text, state->data));
        return;
    }

    // a large project: the rules on a Background worker, the rest back on the main thread
    state->root = root;
    state->job = ctx.jobs().create("check_pixel_art", std::string("Pixel art check") + (state->args.output ? " and fix" : ""));
    state->job->setProgress(-1.0, "checking " + std::to_string(size) + " drawables and textures");
    McpContext* context = &ctx;
    const std::optional<UndoOrigin> origin = call->attributed() ? std::optional<UndoOrigin>(call->getUndoOrigin()) : std::nullopt;
    jobs::submit([state, context, origin]() {
        double seconds = 0.0;
        try {
            state->result = timedRun(*state->input, state->args.rules, seconds);
        } catch (const std::exception& e) {
            state->failure = e.what();
        }
        getEditorHost().enqueueMainThreadTask([state, context, origin, seconds]() {
            measuredSeconds()[state->root] = seconds;
            McpWriteScope writeScope;
            std::optional<UndoOriginScope> attribution;
            if (origin) attribution.emplace(*origin);
            Project* current = context->project();
            if (!state->failure.empty())
                state->problems.push_back({"failed", "", "the check failed: " + state->failure, "call again"});
            else if (!current || current->getProjectPath() != state->root)
                state->problems.push_back({"project_changed", "", "another project was opened during the check", "call check_pixel_art again"});
            if (state->problems.empty() && finishCheck(*context, *state)) {
                state->job->succeed(state->text, state->data);
                return;
            }
            Json list = Json::array();
            std::string text = "check_pixel_art:";
            for (const Problem& p : state->problems) {
                list.push_back(problemJson(p));
                text += "\n- " + p.kind + ": " + p.message;
            }
            state->job->fail(text, Json{{"errors", list}});
        });
    }, {jobs::Lane::Background, jobs::GateClass::Default, "check-pixel-art"});
    call->detach();
    replyWhenJobDone(call, state->job, true, kJobWaitSeconds, [state](const McpJob& finished) {
        Json result = finished.result();
        result["job_id"] = finished.id();
        result["status"] = jobStatusName(finished.status());
        if (finished.status() != JobStatus::Succeeded) return ToolResult::error(finished.message(), result);
        return ToolResult::success(state->text.empty() ? finished.message() : state->text, result);
    });
}

}
}

void registerPixelArtTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "check_pixel_art";
    tool.title = "Check Pixel Art";
    tool.description =
        "Pixel-art rules on scenes and textures (filtering, texel size, integer scale, grid, rotation, fonts, palette, "
        "mipmaps/compression, particles, anti-aliasing, project mode): PASS/WARN/FAIL with measured and expected values, a verdict "
        "and fixes. fix previews the changes; the same call with output:true applies them as one undo step and re-checks. Rules and "
        "loop: jasper://docs/pixel-art.";
    std::vector<std::string> rules = epx::ruleIds();
    tool.inputSchema = object({
        {"scenes", array(string(""), "")},
        {"paths", array(string(""), "")},
        {"rules", array(stringEnum("", rules), "")},
        {"fix", array(string(""), "")},
        {"output", boolean("")},
        {"image", boolean("")},
        {"max_width", integer("", 256, 2048)}
    });
    tool.annotations.readOnly = false;
    tool.annotations.destructive = false;
    tool.annotations.idempotent = true;
    tool.writesFiles = [](const Json& args) { return args.value("output", false) == true; };
    // checks work while playing; the handler refuses output then (not_while_playing)
    tool.allowedWhilePlaying = true;
    tool.activity = ToolActivity::Edit;
    tool.handler = pixelarttools::checkPixelArtTool;
    registry.addTool(std::move(tool));
}

}
