// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HTML/CSS UI (phase-4 spec §3.6): inspect_ui, assert_ui, create_ui_document, the
// jasper://examples/html-ui resources, and htmlui_mcp::elementCenter for send_input's target.
// The tools read live documents through the engine's HtmlUIInspector / HtmlUIAssertions (RmlUi
// stays inside the engine library); engine paths are asset-relative and are shown here
// project-relative (CONTRACTS "Paths").
#include "McpTools.h"
#include "HtmlUiShared.h"
#include "PlayViewCommon.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/McpValues.h"

#include "Catalog.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlUIAssertions.h"
#include "htmlui/HtmlUIInspector.h"
#include "object/ui/UIDocument.h"
#include "object/ui/UIItemsView.h"
#include "script/LuaBinding.h"
#include "subsystem/UIDocumentSystem.h"
#include "util/Base64.h"
#include "util/HtmlUiExamples.h"
#include "util/ProjectChecks.h"

#include "lua.hpp"

#include "stb_image.h"
#include "stb_image_write.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <sstream>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
namespace hui = ::jasperengine::htmlui;
namespace compat = ::jasperengine::htmlui::compat;

constexpr int LOAD_WAIT_MS = 5000;
constexpr size_t MAX_DIAGNOSTICS = 50;
constexpr size_t MAX_TEXT = 12000;
constexpr size_t MAX_FAILURES = 20;
constexpr size_t MAX_DOCUMENTS_LISTED = 10;
constexpr int DEFAULT_IMAGE_WIDTH = 768;

std::string lowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool has(const Json& args, const char* key) {
    return args.contains(key) && !args[key].is_null();
}

std::string number(double value, int decimals = 1) {
    if (!std::isfinite(value)) return "0";
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(decimals);
    out << value;
    std::string text = out.str();
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') text.pop_back();
        if (text.back() == '.') text.pop_back();
    }
    return text == "-0" ? "0" : text;
}

double round1(double value) {
    return std::round(value * 10.0) / 10.0;
}

Json boxInt(const hui::InspectRect& r) {
    return Json::array({std::lround(r.x), std::lround(r.y), std::lround(r.width), std::lround(r.height)});
}

Json box1(const hui::InspectRect& r) {
    return Json::array({round1(r.x), round1(r.y), round1(r.width), round1(r.height)});
}

Json edges1(const hui::InspectEdges& e) {
    return Json::array({round1(e.top), round1(e.right), round1(e.bottom), round1(e.left)});
}

// "[472,248 336x56]"
std::string boxText(const hui::InspectRect& r) {
    return "[" + std::to_string(std::lround(r.x)) + "," + std::to_string(std::lround(r.y)) + " " +
           std::to_string(std::lround(r.width)) + "x" + std::to_string(std::lround(r.height)) + "]";
}

std::string viewportText(double width, double height) {
    return std::to_string(std::lround(width)) + "x" + std::to_string(std::lround(height));
}

// An asset-relative engine path shown project-relative ("ui/menu.css" -> "assets/ui/menu.css")
std::string assetToProject(Project* project, const std::string& assetPath) {
    if (assetPath.empty()) return assetPath;
    return w4::projectRelative(project, (project->getAssetsPath() / FileUtils::pathFromUtf8(assetPath)).lexically_normal());
}

bool isRunning(ScenePlayState state) {
    return state == ScenePlayState::PLAYING || state == ScenePlayState::PAUSED;
}

bool isPlaying(Project* project) {
    return project && project->getPlaySessionSceneId() != NULL_PROJECT_SCENE;
}

// The scenes of the running game: the played scene and its runtime layers (child scenes)
std::vector<SceneProject*> runningScenes(Project* project) {
    std::vector<SceneProject*> out;
    const uint32_t playId = project->getPlaySessionSceneId();
    SceneProject* main = playId != NULL_PROJECT_SCENE ? project->getScene(playId) : nullptr;
    if (!main || !main->scene || !isRunning(main->playState)) return out;
    out.push_back(main);
    for (SceneProject* layer : project->getRunningRuntimeLayerScenes(playId)) {
        if (layer && layer->scene && std::find(out.begin(), out.end(), layer) == out.end()) out.push_back(layer);
    }
    return out;
}

// Where documents are looked for without a scene: the running scenes in play, else the open scenes
std::vector<SceneProject*> documentScenes(Project* project) {
    if (isPlaying(project)) return runningScenes(project);
    std::vector<SceneProject*> out;
    for (SceneProject* sp : project->getScenesAndPrefabStages()) {
        if (sp && sp->scene && sp->opened) out.push_back(sp);
    }
    return out;
}

UIDocumentComponent* documentComponent(SceneProject* sp, Entity entity) {
    return sp && sp->scene ? sp->scene->findComponent<UIDocumentComponent>(entity) : nullptr;
}

std::string documentHtml(Project* project, SceneProject* sp, Entity entity) {
    UIDocumentComponent* component = documentComponent(sp, entity);
    return component ? assetToProject(project, component->filename) : std::string();
}

// "Main/Menu (assets/ui/menu.html)"
std::string documentLabel(Project* project, SceneProject* sp, Entity entity, bool withScene) {
    const std::string html = documentHtml(project, sp, entity);
    return (withScene ? sp->name + "/" : std::string()) + McpRefs::entityPath(sp, entity) +
           " (" + (html.empty() ? std::string("no html") : html) + ")";
}

std::string documentList(Project* project, const std::vector<std::pair<SceneProject*, Entity>>& documents, bool withScene) {
    std::string text;
    for (size_t i = 0; i < documents.size() && i < MAX_DOCUMENTS_LISTED; i++) {
        text += (i ? ", " : "") + documentLabel(project, documents[i].first, documents[i].second, withScene);
    }
    if (documents.size() > MAX_DOCUMENTS_LISTED) text += " and " + std::to_string(documents.size() - MAX_DOCUMENTS_LISTED) + " more";
    return text;
}

std::vector<std::pair<SceneProject*, Entity>> documentsOf(SceneProject* sp) {
    std::vector<std::pair<SceneProject*, Entity>> out;
    if (!sp || !sp->scene) return out;
    for (Entity entity : hui::HtmlUIInspector::documents(sp->scene)) out.emplace_back(sp, entity);
    return out;
}

// ------------------------------------------------------------------ document resolution

struct DocTarget {
    uint32_t sceneId = NULL_PROJECT_SCENE;
    Entity entity = NULL_ENTITY;
};

SceneProject* targetScene(Project* project, const DocTarget& target) {
    SceneProject* sp = project ? project->getScene(target.sceneId) : nullptr;
    return sp && sp->scene ? sp : nullptr;
}

// scene + entity arguments → the document (spec §3.6, "Document resolution")
bool resolveDocument(McpContext& ctx, const Json& args, DocTarget& out, std::string& error) {
    Project* project = ctx.project();
    if (!project) {
        error = "No project is open.";
        return false;
    }
    const bool playing = isPlaying(project);
    const bool sceneGiven = has(args, "scene");
    const Json sceneArg = sceneGiven ? args["scene"] : Json();
    SceneProject* sp = playing ? w3::resolveRuntimeScene(project, sceneArg, error) : McpRefs::resolveScene(project, sceneArg, error);
    if (!sp) return false;
    if (!sp->scene) {
        error = McpRefs::notOpenError(project, *sp);
        return false;
    }
    const std::string where = playing ? "running scene" : "open scene";

    if (has(args, "entity")) {
        std::string entityError;
        Entity entity = playing ? w3::resolveLiveEntity(project, sp, args["entity"], entityError)
                                : McpRefs::resolveEntity(sp, args["entity"], entityError);
        if (entity == NULL_ENTITY && !sceneGiven) {
            // the GameObject may live in another scene than the selected / played one
            std::vector<std::pair<SceneProject*, Entity>> found;
            for (SceneProject* other : documentScenes(project)) {
                if (other == sp) continue;
                std::string ignored;
                const Entity e = playing ? w3::resolveLiveEntity(project, other, args["entity"], ignored)
                                         : McpRefs::resolveEntity(other, args["entity"], ignored);
                if (e != NULL_ENTITY && documentComponent(other, e)) found.emplace_back(other, e);
            }
            if (found.size() == 1) {
                sp = found[0].first;
                entity = found[0].second;
            } else if (found.size() > 1) {
                error = entityError + " Several " + where + "s have it: pass scene, one of: " + documentList(project, found, true) + ".";
                return false;
            }
        }
        if (entity == NULL_ENTITY) {
            error = entityError;
            return false;
        }
        if (!documentComponent(sp, entity)) {
            const auto documents = documentsOf(sp);
            error = "GameObject \"" + McpRefs::entityPath(sp, entity) + "\" has no UI Document component. UI Documents in scene \"" +
                    sp->name + "\": " + (documents.empty() ? std::string("none (create one with create_ui_document)")
                                                         : documentList(project, documents, false)) + ".";
            return false;
        }
        out.sceneId = sp->id;
        out.entity = entity;
        return true;
    }

    std::vector<std::pair<SceneProject*, Entity>> documents = documentsOf(sp);
    if (documents.empty() && !sceneGiven) {
        for (SceneProject* other : documentScenes(project)) {
            if (other == sp) continue;
            for (const auto& doc : documentsOf(other)) documents.push_back(doc);
        }
        if (documents.empty()) {
            error = "No UI Document in scene \"" + sp->name + "\" (or any " + where + "): create one with create_ui_document.";
            return false;
        }
    }
    if (documents.empty()) {
        error = "No UI Document in scene \"" + sp->name + "\": create one with create_ui_document.";
        return false;
    }
    if (documents.size() > 1) {
        error = "Several UI Documents: pass entity, one of: " + documentList(project, documents, true) + ".";
        return false;
    }
    out.sceneId = documents[0].first->id;
    out.entity = documents[0].second;
    return true;
}

// The document's error diagnostics, one per line ("assets/ui/x.html:3:5: message")
std::string loadErrors(Project* project, SceneProject* sp, Entity entity) {
    std::string text;
    UIDocumentSystem* system = sp->scene->getSystem<UIDocumentSystem>().get();
    if (!system) return text;
    for (const compat::Diagnostic& d : system->getDiagnostics(entity)) {
        if (d.severity != compat::Severity::Error) continue;
        std::string place = d.file.empty() ? documentHtml(project, sp, entity) : assetToProject(project, d.file);
        if (d.line) place += ":" + std::to_string(d.line) + (d.column ? ":" + std::to_string(d.column) : std::string());
        text += "\n" + place + ": " + d.message;
    }
    return text;
}

// Resolves the document and waits (up to 5 s) for its load, then runs `next` on the main thread
void withDocument(const CallPtr& call, std::function<void(const DocTarget&)> next) {
    DocTarget target;
    std::string error;
    if (!resolveDocument(call->ctx(), call->args(), target, error)) {
        call->fail(error);
        return;
    }
    auto state = [call, target]() {
        // 0 loaded, 1 waiting, 2 failed, 3 gone
        SceneProject* sp = targetScene(call->ctx().project(), target);
        UIDocumentComponent* component = documentComponent(sp, target.entity);
        if (!component) return 3;
        if (component->filename.empty()) return 2;
        UIDocumentSystem* system = sp->scene->getSystem<UIDocumentSystem>().get();
        if (system && system->isDocumentLoaded(target.entity) && !component->needReload) return 0;
        return component->loadFailed && !component->needReload ? 2 : 1;
    };
    auto finish = [call, target, next](int now) {
        Project* project = call->ctx().project();
        SceneProject* sp = targetScene(project, target);
        if (now == 0) {
            next(target);
            return;
        }
        if (now == 3) {
            call->fail("The UI Document is gone (its GameObject or scene was removed).");
            return;
        }
        UIDocumentComponent* component = documentComponent(sp, target.entity);
        if (component && component->filename.empty()) {
            call->fail("The UI Document of \"" + McpRefs::entityPath(sp, target.entity) + "\" has no html: set it with "
                       "set_properties {\"uidocument\": {\"filename\": \"assets/ui/<name>.html\"}}.");
            return;
        }
        call->fail("The document " + documentHtml(project, sp, target.entity) + " did not load:" +
                   loadErrors(project, sp, target.entity));
    };
    const int now = state();
    if (now != 1) {
        finish(now);
        return;
    }
    call->waitUntil([state]() { return state() != 1; }, LOAD_WAIT_MS, [state, finish](bool) {
        const int after = state();
        finish(after == 1 ? 2 : after);
    });
}

// ------------------------------------------------------------------ diagnostics and the document object

struct DiagnosticReport {
    std::vector<ProjectChecks::Finding> findings;   // errors and warnings, source order
    size_t errors = 0, warnings = 0, infos = 0;
};

DiagnosticReport liveDiagnostics(Project* project, const std::vector<compat::Diagnostic>& diagnostics) {
    DiagnosticReport report;
    const fs::path root = project->getProjectPath();
    const fs::path assets = project->getAssetsPath();
    for (const compat::Diagnostic& d : diagnostics) {
        if (d.severity == compat::Severity::Info) {
            report.infos++;
            continue;
        }
        if (d.severity == compat::Severity::Error) report.errors++;
        else report.warnings++;
        report.findings.push_back(ProjectChecks::uiFinding(root, assets, d));
    }
    ProjectChecks::sortFindings(report.findings);
    return report;
}

Json diagnosticJson(const ProjectChecks::Finding& f) {
    return {{"severity", f.severity}, {"code", f.code}, {"message", f.message}, {"suggestion", f.suggestion},
            {"path", f.path}, {"line", f.line}, {"col", f.col}};
}

// Adds "diagnostics" (at most 50, then diagnostics_truncated) and returns their text lines
std::string addDiagnostics(Json& data, const std::vector<ProjectChecks::Finding>& findings) {
    Json list = Json::array();
    std::string text;
    for (size_t i = 0; i < findings.size() && i < MAX_DIAGNOSTICS; i++) {
        list.push_back(diagnosticJson(findings[i]));
        text += "\n" + ProjectChecks::toText(findings[i]);
    }
    data["diagnostics"] = list;
    if (findings.size() > MAX_DIAGNOSTICS) {
        data["diagnostics_truncated"] = true;
        text += "\n... " + std::to_string(findings.size() - MAX_DIAGNOSTICS) + " more diagnostics";
    }
    return text;
}

struct DocumentView {
    hui::DocumentInfo info;
    DiagnosticReport diagnostics;
    Json json;            // the shared "document" object
    std::string header;   // the first text line
};

bool documentView(Project* project, SceneProject* sp, Entity entity, DocumentView& out, std::string& error) {
    if (!hui::HtmlUIInspector::documentInfo(sp->scene, entity, out.info, error)) return false;
    out.diagnostics = liveDiagnostics(project, out.info.diagnostics);
    const std::string html = assetToProject(project, out.info.html);
    out.json = {
        {"entity", McpRefs::entityJson(sp, entity)},
        {"scene", sp->id},
        {"html", html},
        {"viewport", Json::array({std::lround(out.info.width), std::lround(out.info.height)})},
        {"load_count", out.info.loadCount},
        {"render_target", out.info.renderToTexture ? "texture" : "screen"},
        {"sort_order", out.info.sortOrder},
        {"visible", out.info.visible},
        {"diagnostics_count", {{"error", out.diagnostics.errors}, {"warning", out.diagnostics.warnings},
                               {"info", out.diagnostics.infos}}}
    };
    out.header = html + " in \"" + sp->scene->getEntityName(entity) + "\" (scene \"" + sp->name + "\"), " +
                 viewportText(out.info.width, out.info.height) + (out.info.renderToTexture ? " texture" : "") +
                 (out.info.visible ? "" : ", hidden") + ", load " + std::to_string(out.info.loadCount) + ": " +
                 std::to_string(out.diagnostics.errors) + " errors, " + std::to_string(out.diagnostics.warnings) + " warnings";
    return true;
}

// ------------------------------------------------------------------ screenshots

struct Outline {
    hui::InspectRect rect;   // canvas px
    unsigned char rgb[3];
};

const unsigned char CYAN[3] = {0x00, 0xc8, 0xff};
const unsigned char RED[3] = {0xff, 0x3b, 0x30};
const unsigned char AMBER[3] = {0xff, 0xcc, 0x00};

Outline outline(const hui::InspectRect& rect, const unsigned char* rgb) {
    return Outline{rect, {rgb[0], rgb[1], rgb[2]}};
}

// Why the document cannot be shown in a Game view capture ("" when it can)
std::string imageUnavailable(Project* project, SceneProject* sp, Entity entity) {
    UIDocumentComponent* component = documentComponent(sp, entity);
    if (!component) return "the document is gone";
    if (component->renderMode == UIDocumentRenderMode::TEXTURE) return "the document renders to a texture, not to the Game view";
    if (isPlaying(project)) {
        const std::vector<SceneProject*> running = runningScenes(project);
        if (std::find(running.begin(), running.end(), sp) == running.end()) {
            return "scene \"" + sp->name + "\" is not running, so the Game view does not show it";
        }
        return {};
    }
    SceneProject* shown = w3::gameViewScene(project);
    if (shown != sp) {
        return "the Game view shows scene \"" + (shown ? shown->name : std::string("none")) + "\", not \"" + sp->name +
               "\" (select it, or play it)";
    }
    return {};
}

struct Pixels {
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
};

void drawOutline(Pixels& image, int x0, int y0, int x1, int y1, const unsigned char* rgb, int clipX0, int clipY0, int clipX1, int clipY1) {
    auto put = [&](int x, int y) {
        if (x < clipX0 || y < clipY0 || x > clipX1 || y > clipY1 || x < 0 || y < 0 || x >= image.width || y >= image.height) return;
        unsigned char* p = &image.rgb[(static_cast<size_t>(y) * image.width + x) * 3];
        p[0] = rgb[0];
        p[1] = rgb[1];
        p[2] = rgb[2];
    };
    for (int t = 0; t < 2; t++) {
        for (int x = x0; x <= x1; x++) {
            put(x, y0 + t);
            put(x, y1 - t);
        }
        for (int y = y0; y <= y1; y++) {
            put(x0 + t, y);
            put(x1 - t, y);
        }
    }
}

// Box filter down to width x height
Pixels downscale(const Pixels& source, int width, int height) {
    Pixels out;
    out.width = width;
    out.height = height;
    out.rgb.resize(static_cast<size_t>(width) * height * 3);
    const double fx = double(source.width) / width, fy = double(source.height) / height;
    for (int y = 0; y < height; y++) {
        const int sy0 = static_cast<int>(y * fy);
        const int sy1 = std::max(sy0 + 1, std::min(source.height, static_cast<int>((y + 1) * fy)));
        for (int x = 0; x < width; x++) {
            const int sx0 = static_cast<int>(x * fx);
            const int sx1 = std::max(sx0 + 1, std::min(source.width, static_cast<int>((x + 1) * fx)));
            unsigned long sum[3] = {0, 0, 0};
            unsigned long count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    const unsigned char* p = &source.rgb[(static_cast<size_t>(sy) * source.width + sx) * 3];
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    count++;
                }
            }
            unsigned char* q = &out.rgb[(static_cast<size_t>(y) * width + x) * 3];
            for (int c = 0; c < 3; c++) q[c] = static_cast<unsigned char>((sum[c] + count / 2) / std::max(1ul, count));
        }
    }
    return out;
}

// The capture cropped to the outlined rects (plus 24 px, at least 320 px wide, inside the canvas),
// outlined, scaled to maxWidth. info: {canvas_rect: [x,y,w,h] canvas px the image shows, width, height}
bool outlinedImage(const w3::CaptureResult& capture, const std::vector<Outline>& outlines, int maxWidth,
                   ToolImage& image, Json& info, std::string& error) {
    if (!capture.info.contains("canvas_rect_in_image") || !capture.info.contains("canvas")) {
        error = "the Game view capture has no canvas";
        return false;
    }
    const std::vector<unsigned char> png = Base64::decode(capture.image.data);
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 3);
    if (!data) {
        error = "the Game view capture could not be decoded";
        return false;
    }
    Pixels full;
    full.width = w;
    full.height = h;
    full.rgb.assign(data, data + static_cast<size_t>(w) * h * 3);
    stbi_image_free(data);

    const Json& r = capture.info["canvas_rect_in_image"];
    const double rx = r.value("x", 0.0), ry = r.value("y", 0.0), rw = r.value("width", 0.0), rh = r.value("height", 0.0);
    const double cw = capture.info["canvas"].value("width", 0.0), ch = capture.info["canvas"].value("height", 0.0);
    if (rw <= 0 || rh <= 0 || cw <= 0 || ch <= 0) {
        error = "the Game view shows no canvas";
        return false;
    }
    const double sx = rw / cw, sy = rh / ch;
    const int clipX0 = std::max(0, static_cast<int>(std::floor(rx))), clipY0 = std::max(0, static_cast<int>(std::floor(ry)));
    const int clipX1 = std::min(w - 1, static_cast<int>(std::ceil(rx + rw)) - 1);
    const int clipY1 = std::min(h - 1, static_cast<int>(std::ceil(ry + rh)) - 1);
    if (clipX1 < clipX0 || clipY1 < clipY0) {
        error = "the Game view shows no canvas";
        return false;
    }

    // union of the outlines in image px
    bool any = false;
    double ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
    for (const Outline& o : outlines) {
        const int x0 = static_cast<int>(std::floor(rx + o.rect.x * sx)), y0 = static_cast<int>(std::floor(ry + o.rect.y * sy));
        const int x1 = std::max(x0, static_cast<int>(std::ceil(rx + (o.rect.x + o.rect.width) * sx)) - 1);
        const int y1 = std::max(y0, static_cast<int>(std::ceil(ry + (o.rect.y + o.rect.height) * sy)) - 1);
        drawOutline(full, x0, y0, x1, y1, o.rgb, clipX0, clipY0, clipX1, clipY1);
        if (!any) {
            ux0 = x0; uy0 = y0; ux1 = x1; uy1 = y1;
            any = true;
        } else {
            ux0 = std::min<double>(ux0, x0); uy0 = std::min<double>(uy0, y0);
            ux1 = std::max<double>(ux1, x1); uy1 = std::max<double>(uy1, y1);
        }
    }
    int cropX0 = clipX0, cropY0 = clipY0, cropX1 = clipX1, cropY1 = clipY1;
    if (any) {
        double x0 = ux0 - 24, x1 = ux1 + 24, y0 = uy0 - 24, y1 = uy1 + 24;
        if (x1 - x0 + 1 < 320) {
            const double grow = (320 - (x1 - x0 + 1)) / 2.0;
            x0 -= grow;
            x1 += grow;
            // at an edge of the canvas the box moves inward instead of shrinking
            if (x0 < clipX0) {
                x1 += clipX0 - x0;
                x0 = clipX0;
            } else if (x1 > clipX1) {
                x0 -= x1 - clipX1;
                x1 = clipX1;
            }
        }
        cropX0 = std::max(clipX0, static_cast<int>(std::floor(x0)));
        cropX1 = std::min(clipX1, static_cast<int>(std::ceil(x1)));
        cropY0 = std::max(clipY0, static_cast<int>(std::floor(y0)));
        cropY1 = std::min(clipY1, static_cast<int>(std::ceil(y1)));
        if (cropX1 < cropX0 || cropY1 < cropY0) {   // every outline lies outside the canvas
            cropX0 = clipX0; cropY0 = clipY0; cropX1 = clipX1; cropY1 = clipY1;
        }
    }
    Pixels crop;
    crop.width = cropX1 - cropX0 + 1;
    crop.height = cropY1 - cropY0 + 1;
    crop.rgb.resize(static_cast<size_t>(crop.width) * crop.height * 3);
    for (int y = 0; y < crop.height; y++) {
        std::copy_n(&full.rgb[(static_cast<size_t>(cropY0 + y) * full.width + cropX0) * 3], static_cast<size_t>(crop.width) * 3,
                    &crop.rgb[static_cast<size_t>(y) * crop.width * 3]);
    }
    if (maxWidth > 0 && crop.width > maxWidth) {
        crop = downscale(crop, maxWidth, std::max(1, static_cast<int>(std::lround(double(crop.height) * maxWidth / crop.width))));
    }
    std::vector<unsigned char> encoded;
    auto append = [](void* context, void* bytes, int size) {
        auto* out = static_cast<std::vector<unsigned char>*>(context);
        const unsigned char* begin = static_cast<const unsigned char*>(bytes);
        out->insert(out->end(), begin, begin + size);
    };
    if (!stbi_write_png_to_func(append, &encoded, crop.width, crop.height, 3, crop.rgb.data(), crop.width * 3)) {
        error = "encoding the image failed";
        return false;
    }
    image.data = Base64::encode(encoded.data(), encoded.size());
    image.mimeType = "image/png";
    info = {
        {"canvas_rect", Json::array({round1((cropX0 - rx) / sx), round1((cropY0 - ry) / sy),
                                     round1((cropX1 - cropX0 + 1) / sx), round1((cropY1 - cropY0 + 1) / sy)})},
        {"width", crop.width},
        {"height", crop.height}
    };
    return true;
}

// Replies with `result` plus the outlined Game view image of the document (or a note why there is none)
void replyWithImage(const CallPtr& call, ToolResult result, const DocTarget& target, std::vector<Outline> outlines, int maxWidth) {
    Project* project = call->ctx().project();
    SceneProject* sp = targetScene(project, target);
    const std::string why = sp ? imageUnavailable(project, sp, target.entity) : std::string("the scene is gone");
    if (!why.empty()) {
        result.text += "\nNo image: " + why + ".";
        if (result.structured.is_object()) result.structured["image_note"] = why;
        call->reply(std::move(result));
        return;
    }
    w3::CaptureRequest request;
    request.view = "game";
    request.maxWidth = 0;
    auto pending = std::make_shared<ToolResult>(std::move(result));
    w3::captureView(call, request, [call, pending, outlines, maxWidth](w3::CaptureResult capture) {
        ToolResult reply = std::move(*pending);
        std::string error;
        ToolImage image;
        Json info;
        if (!capture.ok) error = capture.error;
        else if (!outlinedImage(capture, outlines, maxWidth, image, info, error)) {}
        if (!error.empty()) {
            reply.text += "\nNo image: " + error + ".";
            if (reply.structured.is_object()) reply.structured["image_note"] = error;
        } else {
            reply.images.push_back(std::move(image));
            if (reply.structured.is_object()) reply.structured["image"] = info;
        }
        call->reply(std::move(reply));
    });
}

int maxWidthArg(const Json& args) {
    return std::clamp(args.value("max_width", DEFAULT_IMAGE_WIDTH), 0, 4096);
}

bool readVec2(const Json& args, const char* key, double& x, double& y, std::string& error) {
    const Json& value = args[key];
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
        error = std::string(key) + " must be [x, y] numbers.";
        return false;
    }
    x = value[0].get<double>();
    y = value[1].get<double>();
    return true;
}

bool readViewport(const Json& value, double& width, double& height, std::string& error, const std::string& name) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number()) {
        error = name + " must be [width, height] in CSS px.";
        return false;
    }
    width = std::round(value[0].get<double>());
    height = std::round(value[1].get<double>());
    if (width < 1 || height < 1 || width > 16384 || height > 16384) {
        error = name + " must be 1..16384 px per side.";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ inspect_ui

const char* const FLAG_NAMES[] = {"overflow-x", "overflow-y", "clipped", "hidden", "transparent", "offscreen", "zero-size"};

std::vector<std::string> flagList(const hui::InspectFlags& f) {
    const bool values[] = {f.overflowX, f.overflowY, f.clipped, f.hidden, f.transparent, f.offscreen, f.zeroSize};
    std::vector<std::string> out;
    for (size_t i = 0; i < 7; i++) if (values[i]) out.push_back(FLAG_NAMES[i]);
    return out;
}

Json elementJson(const hui::InspectElement& e, const std::vector<hui::InspectStyle>* styles) {
    Json out = {{"i", e.index}, {"parent", e.parent}, {"depth", e.depth}, {"ref", e.ref}, {"tag", e.tag}, {"box", boxInt(e.border)}};
    if (!e.id.empty()) out["id"] = e.id;
    if (!e.classes.empty()) out["class"] = e.classes;
    if (!e.text.empty()) out["text"] = e.text;
    if (!e.pseudo.empty()) out["pseudo"] = e.pseudo;
    const std::vector<std::string> flags = flagList(e.flags);
    if (!flags.empty()) out["flags"] = flags;
    // the UI effects it draws with (phase 5, UIElement::getEffects); omitted when none
    if (!e.effects.empty()) {
        Json effects = Json::array();
        for (const hui::InspectEffect& effect : e.effects) {
            Json params = Json::object();
            for (const auto& [name, value] : effect.params) params[name] = value;
            Json entry = {{"property", effect.property}, {"index", effect.index}, {"name", effect.name}, {"form", effect.form},
                          {"params", params}};
            // phase 6: a stateful effect's simulation
            if (effect.stateful)
                entry["state"] = {{"bytes", effect.stateBytes}, {"steps", effect.steps}, {"time", effect.simTime}, {"paused", effect.paused},
                                  {"images", effect.stateImages}, {"replays", effect.replays}, {"replaying", effect.replaying},
                                  {"splats", effect.splats}, {"path", effect.pathSamples}};
            effects.push_back(std::move(entry));
        }
        out["effects"] = effects;
    }
    // a <video>'s state and its controls' rectangles (phase 6)
    if (e.media) {
        const hui::InspectMedia& m = *e.media;
        Json media = {{"currentSrc", m.currentSrc}, {"backend", m.backend}, {"currentTime", m.currentTime}, {"paused", m.paused}, {"ended", m.ended},
                      {"seeking", m.seeking}, {"loop", m.loop}, {"muted", m.muted}, {"volume", m.volume}, {"playbackRate", m.playbackRate},
                      {"readyState", m.readyState}, {"networkState", m.networkState}, {"videoWidth", m.videoWidth},
                      {"videoHeight", m.videoHeight}, {"frame", m.frame}, {"droppedFrames", m.droppedFrames},
                      {"decodeMs", std::round(m.decodeMs * 1000.0) / 1000.0}, {"memoryBytes", m.memoryBytes}, {"poster", m.showingPoster}};
        media["duration"] = m.duration >= 0.0 ? Json(m.duration) : Json(nullptr);
        if (m.errorCode != 0)
            media["error"] = {{"code", m.errorCode}, {"message", m.errorMessage}};
        if (m.controls) {
            media["controls"] = {{"visible", m.controlsVisible}, {"bar", boxInt(m.controlsBar)}, {"play", boxInt(m.playButton)},
                                 {"timeline", boxInt(m.timeline)}, {"mute", boxInt(m.muteButton)}};
        }
        out["media"] = media;
    }
    // S17: a virtual view's window (0-based indices)
    if (e.widget) {
        const hui::InspectWidget& w = *e.widget;
        out["widget"] = {{"kind", w.kind}, {"count", w.count}, {"first", w.first}, {"last", w.last}, {"pool", w.pool},
                         {"pool_bound", w.poolBound}, {"overscan", w.overscan}, {"lanes", w.lanes}, {"selection", w.selection},
                         {"selected", w.selected}, {"selected_keys", w.selectedKeys}, {"active", w.active},
                         {"scrollTop", round1(w.scrollTop)}, {"totalHeight", round1(w.totalHeight)}, {"source", w.source},
                         {"layout_boundary", w.layoutBoundary}, {"row_aliases", w.rowAliases}};
        if (!w.sortKey.empty()) out["widget"]["sort"] = {{"key", w.sortKey}, {"direction", w.sortDirection}};
    }
    if (styles && !styles->empty()) {
        Json map = Json::object();
        for (const hui::InspectStyle& s : *styles) map[s.property] = s.value;
        out["styles"] = map;
    }
    return out;
}

// S17: the document's tooltip, the drag in progress and the last reload
Json tooltipJson(const hui::InspectTooltip& t) {
    Json out = {{"visible", t.visible}};
    if (t.visible)
        out.update({{"source", t.source}, {"text", t.text}, {"rect", boxInt(t.rect)}, {"side", t.side}});
    return out;
}

Json dragJson(const hui::InspectDrag& d) {
    Json out = {{"active", d.active}};
    if (d.active)
        out.update({{"source", d.source}, {"target", d.target}, {"types", d.types}, {"allowed", d.allowed}, {"effect", d.effect},
                    {"programmatic", d.programmatic}});
    return out;
}

Json reloadJson(const hui::InspectReload& r) {
    if (r.mode.empty()) return nullptr;
    Json out = {{"mode", r.mode}, {"kept", r.kept}, {"updated", r.updated}, {"inserted", r.inserted}, {"removed", r.removed},
                {"replaced", r.replaced}, {"runtimeKept", r.runtimeKept}, {"scriptsRerun", r.scriptsRerun},
                {"ms", std::round(r.ms * 100.0) / 100.0}, {"causes", r.causes}};
    out["fallbackReason"] = r.fallbackReason.empty() ? Json(nullptr) : Json(r.fallbackReason);
    return out;
}

// "button#play.btn.primary "Play" [472,248 336x56] :hover !overflow-x fx:sheen {color: rgb(...)}"
std::string elementLine(const hui::InspectElement& e, const std::vector<hui::InspectStyle>* styles) {
    std::string line(static_cast<size_t>(std::max(0, e.depth)) * 2, ' ');
    line += e.tag;
    if (!e.id.empty()) line += "#" + e.id;
    for (const std::string& c : e.classes) line += "." + c;
    if (!e.text.empty()) line += " \"" + e.text + "\"";
    line += " " + boxText(e.border);
    for (const std::string& p : e.pseudo) line += " :" + p;
    for (const std::string& f : flagList(e.flags)) {
        const bool problem = f == "overflow-x" || f == "overflow-y" || f == "clipped";
        line += " " + std::string(problem ? "!" : "") + f;
    }
    // phase 6: a paused simulation is marked "~"
    for (const hui::InspectEffect& effect : e.effects) line += " fx:" + effect.name + (effect.stateful && effect.paused ? "~" : "");
    // S17: "list[0..12 of 10000, pool 22]"
    if (e.widget) {
        line += " " + e.widget->kind + "[" + std::to_string(e.widget->first) + ".." + std::to_string(e.widget->last) + " of " +
                std::to_string(e.widget->count) + ", pool " + std::to_string(e.widget->pool) + "]";
    }
    if (e.media) {
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), " media:%s %.3fs/%s ready%d", e.media->paused ? "paused" : "playing", e.media->currentTime,
                      e.media->duration >= 0.0 ? std::to_string(e.media->duration).substr(0, 5).c_str() : "?", e.media->readyState);
        line += buffer;
        if (e.media->errorCode != 0)
            line += " !error" + std::to_string(e.media->errorCode);
    }
    if (styles && !styles->empty()) {
        line += " {";
        for (size_t i = 0; i < styles->size(); i++) line += (i ? "; " : "") + (*styles)[i].property + ": " + (*styles)[i].value;
        line += "}";
    }
    return line;
}

// A style's source with the file shown project-relative
std::string styleSource(Project* project, const hui::InspectStyle& style) {
    if (style.file.empty()) return style.source;
    const std::string shown = assetToProject(project, style.file);
    if (style.source.compare(0, style.file.size(), style.file) == 0) return shown + style.source.substr(style.file.size());
    return style.source;
}

Json detailJson(Project* project, const hui::InspectDetail& d, std::string& text) {
    const hui::InspectBoxModel& b = d.box;
    Json styles = Json::array();
    text += "\n" + d.element.ref + " " + d.element.tag + " " + boxText(d.element.border) +
            "\n  content " + boxText(b.content) + ", padding " + boxText(b.padding) + ", margin " + boxText(b.margin) +
            "; padding [" + number(b.paddingEdges.top) + " " + number(b.paddingEdges.right) + " " + number(b.paddingEdges.bottom) +
            " " + number(b.paddingEdges.left) + "], border [" + number(b.borderEdges.top) + " " + number(b.borderEdges.right) + " " +
            number(b.borderEdges.bottom) + " " + number(b.borderEdges.left) + "]; scroll " + number(b.scrollWidth) + "x" +
            number(b.scrollHeight) + " in " + number(b.clientWidth) + "x" + number(b.clientHeight);
    for (const hui::InspectStyle& s : d.styles) {
        const std::string source = styleSource(project, s);
        Json item = {{"property", s.property}, {"value", s.value}, {"source", source}};
        if (!s.selector.empty()) item["selector"] = s.selector;
        styles.push_back(item);
        text += "\n  " + s.property + ": " + s.value + "  <- " + source + (s.selector.empty() ? "" : " (" + s.selector + ")");
    }
    Json attributes = Json::object();
    for (const auto& [name, value] : d.attributes) attributes[name] = value;
    // S16: primitive or mesh, and the raster of its own text
    text += "\n  paint: " + d.paint.path + (d.paint.reason.empty() ? std::string() : " (" + d.paint.reason + ")");
    Json paint = {{"path", d.paint.path}};
    if (!d.paint.reason.empty()) paint["reason"] = d.paint.reason;
    Json raster = nullptr;
    if (d.raster.hasText) {
        raster = {{"mode", d.raster.mode}, {"scale", std::round(d.raster.scale * 100.0) / 100.0}};
        text += "\n  raster: " + d.raster.mode + (d.raster.mode == "bitmap" && d.raster.scale > 0.0f ? " x" + number(d.raster.scale, 2) : std::string()) +
                (d.raster.mode == "stretched" ? " (settling)" : "");
    }
    Json out = {
        {"ref", d.element.ref},
        {"box", {{"content", box1(b.content)}, {"padding", box1(b.padding)}, {"border", box1(b.border)}, {"margin", box1(b.margin)},
                 {"padding_edges", edges1(b.paddingEdges)}, {"border_edges", edges1(b.borderEdges)},
                 {"margin_edges", edges1(b.marginEdges)},
                 {"scroll", {{"width", round1(b.scrollWidth)}, {"height", round1(b.scrollHeight)},
                             {"client_width", round1(b.clientWidth)}, {"client_height", round1(b.clientHeight)},
                             {"left", round1(b.scrollLeft)}, {"top", round1(b.scrollTop)}}}}},
        {"attributes", attributes},
        {"styles", styles},
        {"paint", paint}
    };
    if (!raster.is_null()) out["raster"] = raster;
    return out;
}

std::vector<std::string> stringList(const Json& value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const Json& item : value) if (item.is_string()) out.push_back(item.get<std::string>());
    return out;
}

// inspect_ui {file}: a .html/.css's diagnostics without a document (ProjectChecks::checkUiFiles, as check --ui)
void inspectFile(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    if (!args["file"].is_string()) {
        call->fail("file must be a project-relative .html, .htm or .css path.");
        return;
    }
    fs::path file;
    std::string error;
    if (!McpRefs::resolveProjectPath(project, args["file"].get<std::string>(), file, error, true)) {
        call->fail(error);
        return;
    }
    file = file.lexically_normal();
    const std::string rel = w4::projectRelative(project, file);
    std::error_code ec;
    if (!ProjectChecks::isUiFile(FileUtils::pathToGenericUtf8(file)) || !fs::is_regular_file(file, ec)) {
        call->fail(rel + " is not a .html, .htm or .css file.");
        return;
    }
    if (!project->isInsideAssetsPath(file)) {
        call->fail(rel + " is outside the assets directory (" + w4::projectRelative(project, project->getAssetsPath()) +
                   "/): UI documents and their stylesheets live there.");
        return;
    }
    const ProjectChecks::UiCheck check = ProjectChecks::checkUiFiles(project->getProjectPath(), {file});
    std::vector<ProjectChecks::Finding> findings = check.findings;
    ProjectChecks::sortFindings(findings);
    Json data = {{"file", rel}};
    const std::string diagnosticsText = addDiagnostics(data, findings);

    // the live documents reading it
    const fs::path assetRel = file.lexically_relative(project->getAssetsPath().lexically_normal());
    const std::string assetPath = FileUtils::pathToGenericUtf8(assetRel);
    Json using_ = Json::array();
    std::string usingText;
    std::vector<SceneProject*> scenes = documentScenes(project);
    for (SceneProject* sp : scenes) {
        UIDocumentSystem* system = sp->scene->getSystem<UIDocumentSystem>().get();
        for (Entity entity : hui::HtmlUIInspector::documents(sp->scene)) {
            UIDocumentComponent* component = documentComponent(sp, entity);
            bool reads = component && component->filename == assetPath;
            if (!reads && system) {
                for (const std::string& dependency : system->getDocumentFiles(entity)) reads = reads || dependency == assetPath;
            }
            if (!reads) continue;
            using_.push_back({{"scene", sp->id}, {"entity", McpRefs::entityJson(sp, entity)},
                              {"html", documentHtml(project, sp, entity)}});
            usingText += (usingText.empty() ? "" : ", ") + documentLabel(project, sp, entity, true);
        }
    }
    data["documents_using"] = using_;
    const size_t errors = ProjectChecks::countSeverity(findings, "error");
    std::string text = rel + ": " + std::to_string(errors) + " errors, " + std::to_string(findings.size() - errors) +
                       " warnings (check --ui); " + (usingText.empty() ? "no live document uses it." : "used by " + usingText + ".") +
                       diagnosticsText;
    call->ok(text, data);
}

void inspectUiTool(const CallPtr& call) {
    const Json& args = call->args();
    if (has(args, "file")) {
        std::vector<std::string> conflicts;
        for (const char* key : {"selector", "at", "detail", "styles", "models", "reload", "viewport", "screenshot", "entity", "render_stats"}) {
            if (has(args, key)) conflicts.push_back(key);
        }
        if (!conflicts.empty()) {
            std::string names;
            for (size_t i = 0; i < conflicts.size(); i++) names += (i ? ", " : "") + conflicts[i];
            call->fail("file inspects a file's diagnostics; drop " + names + " or pass entity instead.");
            return;
        }
        inspectFile(call);
        return;
    }
    const bool full = args.value("detail", std::string("tree")) == "full";
    if (has(args, "detail") && !args["detail"].is_string()) {
        call->fail("detail must be \"tree\" or \"full\".");
        return;
    }
    if (has(args, "detail") && args["detail"] != "tree" && args["detail"] != "full") {
        call->fail("detail must be \"tree\" or \"full\".");
        return;
    }
    const bool wantImage = args.value("screenshot", false);
    if (has(args, "viewport") && wantImage) {
        call->fail("screenshot shows the current layout; call again without viewport for the image.");
        return;
    }
    std::string error;
    double vw = 0, vh = 0, ax = 0, ay = 0;
    if (has(args, "viewport") && !readViewport(args["viewport"], vw, vh, error, "viewport")) {
        call->fail(error);
        return;
    }
    if (has(args, "at") && !readVec2(args, "at", ax, ay, error)) {
        call->fail(error);
        return;
    }
    if (has(args, "selector") && !args["selector"].is_string()) {
        call->fail("selector must be a CSS selector string.");
        return;
    }
    if (has(args, "styles") && !args["styles"].is_array()) {
        call->fail("styles must be an array of CSS property names, e.g. [\"color\", \"font-size\"].");
        return;
    }

    withDocument(call, [call, full, wantImage, vw, vh, ax, ay](const DocTarget& target) {
        const Json& args = call->args();
        Project* project = call->ctx().project();
        SceneProject* sp = targetScene(project, target);
        DocumentView view;
        std::string error;
        if (!documentView(project, sp, target.entity, view, error)) {
            call->fail(error);
            return;
        }
        const std::string selector = args.value("selector", std::string());
        const bool hitMode = has(args, "at");
        const int depth = has(args, "depth") ? std::clamp(args["depth"].get<int>(), 0, 16) : (full ? 0 : 3);
        size_t limit = static_cast<size_t>(std::clamp(args.value("limit", 200), 1, 1000));
        if (full) limit = std::min<size_t>(limit, 20);
        const std::vector<std::string> properties = stringList(args.value("styles", Json::array()));

        std::vector<hui::InspectElement> elements;
        std::vector<hui::InspectElement> matches;   // screenshot outlines
        std::vector<std::vector<hui::InspectStyle>> styles;
        std::vector<hui::InspectDetail> details;
        bool truncated = false;
        std::string failure;
        auto run = [&]() {
            Scene* scene = sp->scene;
            if (hitMode) {
                if (!hui::HtmlUIInspector::hitTest(scene, target.entity, static_cast<float>(ax), static_cast<float>(ay), elements, failure)) return;
                if (elements.size() > limit) {
                    elements.resize(limit);
                    truncated = true;
                }
                if (!elements.empty()) matches.push_back(elements.front());
            } else {
                if (!hui::HtmlUIInspector::query(scene, target.entity, selector, depth, limit, elements, truncated, failure)) return;
                if (wantImage) {
                    bool more = false;
                    if (!hui::HtmlUIInspector::query(scene, target.entity, selector, 0, 50, matches, more, failure)) return;
                }
            }
            if (!properties.empty() && !elements.empty()) {
                std::vector<std::string> refs;
                for (const hui::InspectElement& e : elements) refs.push_back(e.ref);
                if (!hui::HtmlUIInspector::styles(scene, target.entity, refs, properties, styles, failure)) return;
            } else if (!properties.empty()) {
                // validates the names even when nothing matched
                std::vector<std::vector<hui::InspectStyle>> ignored;
                if (!hui::HtmlUIInspector::styles(scene, target.entity, {}, properties, ignored, failure)) return;
            }
            if (full) {
                for (const hui::InspectElement& e : elements) {
                    hui::InspectDetail d;
                    if (!hui::HtmlUIInspector::detail(scene, target.entity, e.ref, properties, d, failure)) return;
                    details.push_back(std::move(d));
                }
            }
        };
        if (vw > 0) {
            if (!hui::HtmlUIInspector::withViewport(sp->scene, target.entity, static_cast<float>(vw), static_cast<float>(vh), run, error)) {
                call->fail(error);
                return;
            }
        } else {
            run();
        }
        if (!failure.empty()) {
            call->fail(failure);
            return;
        }

        Json data = {{"document", view.json}};
        if (vw > 0) data["document"]["laid_out_at"] = Json::array({std::lround(vw), std::lround(vh)});
        std::string text = view.header + (vw > 0 ? " (laid out at " + viewportText(vw, vh) + ", then restored)" : std::string());
        text += addDiagnostics(data, view.diagnostics.findings);

        Json list = Json::array();
        std::string tree;
        size_t shown = 0;
        for (size_t i = 0; i < elements.size(); i++) {
            const std::vector<hui::InspectStyle>* own = i < styles.size() ? &styles[i] : nullptr;
            list.push_back(elementJson(elements[i], own));
            if (text.size() + tree.size() < MAX_TEXT) {
                tree += "\n" + elementLine(elements[i], own);
                shown++;
            }
        }
        if (hitMode) {
            data["hit"] = list;
            if (elements.empty()) {
                text += "\nNothing at (" + number(ax) + ", " + number(ay) + "): the pointer passes through this document "
                        "(to documents below it, then the game).";
            } else {
                text += "\nAt (" + number(ax) + ", " + number(ay) + "), topmost first:";
            }
        } else {
            data["elements"] = list;
            data["truncated"] = truncated;
            if (elements.empty()) text += "\nNo element matches \"" + selector + "\".";
        }
        text += tree;
        if (shown < elements.size()) {
            text += "\n... (" + std::to_string(elements.size() - shown) + " more elements: narrow with selector/depth)";
        }
        if (truncated) {
            text += "\n(limit " + std::to_string(limit) + " reached: more elements exist; narrow with selector/depth" +
                    std::string(full ? ")" : " or raise limit)");
        }
        if (full) {
            Json detailList = Json::array();
            std::string detailText;
            for (const hui::InspectDetail& d : details) detailList.push_back(detailJson(project, d, detailText));
            data["details"] = detailList;
            if (text.size() + detailText.size() > MAX_TEXT * 2) detailText = detailText.substr(0, MAX_TEXT) + "\n... (details cut: see structuredContent)";
            text += detailText;
        }

        if (args.value("render_stats", false)) {
            std::vector<std::pair<std::string, uint32_t>> fields;
            std::string statsError;
            if (hui::HtmlUIInspector::renderStats(sp->scene, target.entity, fields, statsError)) {
                Json stats = Json::object();
                for (const auto& [name, value] : fields) stats[name] = value;
                data["render_stats"] = stats;
                text += "\nRender (last frame): " + std::to_string(stats.value("drawCalls", 0u)) + " draws (" +
                        std::to_string(stats.value("runsMerged", 0u)) + " merged, " + std::to_string(stats.value("instancesDrawn", 0u)) +
                        " instances), " + std::to_string(stats.value("stencilDraws", 0u)) + " stencil draws, " +
                        std::to_string(stats.value("layersPushed", 0u)) + " layers, glyph atlas " +
                        number((double)stats.value("atlasBytes", 0u) / (1024.0 * 1024.0)) + " MiB.";
            }
        }

        if (args.value("models", false)) {
            Json models = Json::object();
            lua_State* L = LuaBinding::getLuaState();
            if (!isPlaying(project) || !isRunning(sp->playState)) {
                text += "\nData models are filled by Lua, which runs only in play.";
            } else if (L) {
                for (const std::string& name : view.info.dataModels) {
                    const int top = lua_gettop(L);
                    if (hui::HtmlUIInspector::pushDataModel(sp->scene, target.entity, name, L)) {
                        models[name] = w3::luaToJson(project, L, -1, 6, 100);
                    }
                    lua_settop(L, top);
                }
                text += "\nData models: " + (models.empty() ? std::string("none (createDataModel makes one)") : models.dump());
            }
            data["models"] = models;
        }

        // S17: the tooltip and the drag, the last reload when asked
        data["tooltip"] = tooltipJson(view.info.tooltip);
        data["drag"] = dragJson(view.info.drag);
        if (view.info.tooltip.visible)
            text += "\nTooltip of " + view.info.tooltip.source + ": \"" + view.info.tooltip.text + "\"";
        if (view.info.drag.active) {
            text += "\nDragging " + view.info.drag.source + (view.info.drag.target.empty() ? std::string() : " over " + view.info.drag.target) +
                    (view.info.drag.allowed ? " (accepted, " + view.info.drag.effect + ")" : " (not accepted)");
        }
        if (args.value("reload", false)) {
            data["reload"] = reloadJson(view.info.reload);
            text += view.info.reload.mode.empty() ? std::string("\nNo hot reload yet.")
                                                  : "\nLast reload: " + view.info.reload.mode + ", " + std::to_string(view.info.reload.updated) +
                                                        " updated, " + std::to_string(view.info.reload.inserted) + " inserted, " +
                                                        std::to_string(view.info.reload.removed) + " removed, " +
                                                        std::to_string(view.info.reload.replaced) + " replaced" +
                                                        (view.info.reload.fallbackReason.empty() ? std::string() : " (" + view.info.reload.fallbackReason + ")");
        }

        ToolResult result = ToolResult::success(text, data);
        if (!wantImage) {
            call->reply(std::move(result));
            return;
        }
        std::vector<Outline> outlines;
        for (const hui::InspectElement& e : matches) {
            if (!e.flags.hidden && !e.flags.zeroSize) outlines.push_back(outline(e.border, CYAN));
        }
        replyWithImage(call, std::move(result), target, std::move(outlines), maxWidthArg(args));
    });
}

// ------------------------------------------------------------------ assert_ui

struct KindName {
    const char* name;
    hui::AssertKind kind;
};

const KindName ASSERT_KINDS[] = {
    {"exists", hui::AssertKind::Exists}, {"count", hui::AssertKind::Count}, {"visible", hui::AssertKind::Visible},
    {"text_fits", hui::AssertKind::TextFits}, {"no_overlap", hui::AssertKind::NoOverlap},
    {"within_parent", hui::AssertKind::WithinParent}, {"within_viewport", hui::AssertKind::WithinViewport},
    {"contrast", hui::AssertKind::Contrast}, {"min_size", hui::AssertKind::MinSize},
    {"virtual_window", hui::AssertKind::VirtualWindow}, {"tooltip", hui::AssertKind::Tooltip}, {"selected", hui::AssertKind::Selected}
};

std::vector<std::string> assertKindNames() {
    std::vector<std::string> names;
    for (const KindName& k : ASSERT_KINDS) names.push_back(k.name);
    return names;
}

bool parseCheck(const Json& item, size_t index, hui::AssertSpec& spec, std::string& error) {
    const std::string at = "checks[" + std::to_string(index) + "]";
    if (!item.is_object()) {
        error = at + " must be an object {type, selector, ...}.";
        return false;
    }
    const std::string type = item.value("type", std::string());
    bool known = false;
    for (const KindName& k : ASSERT_KINDS) {
        if (type == k.name) {
            spec.kind = k.kind;
            known = true;
        }
    }
    if (!known) {
        std::string names;
        for (const std::string& n : assertKindNames()) names += (names.empty() ? "" : ", ") + n;
        error = at + ".type must be one of " + names + ".";
        return false;
    }
    if (!item.contains("selector") || !item["selector"].is_string() || item["selector"].get<std::string>().empty()) {
        error = at + ".selector is required (a CSS selector).";
        return false;
    }
    spec.selector = item["selector"].get<std::string>();
    auto text = [&](const char* key, std::string& out) -> bool {
        if (!has(item, key)) return true;
        if (!item[key].is_string()) {
            error = at + "." + key + " must be a CSS selector string.";
            return false;
        }
        out = item[key].get<std::string>();
        return true;
    };
    auto integer = [&](const char* key, int& out) -> bool {
        if (!has(item, key)) return true;
        if (!item[key].is_number_integer() || item[key].get<long long>() < 0) {
            error = at + "." + key + " must be an integer >= 0.";
            return false;
        }
        out = static_cast<int>(std::min<long long>(item[key].get<long long>(), 100000));
        return true;
    };
    auto real = [&](const char* key, float& out, double low, double high) -> bool {
        if (!has(item, key)) return true;
        if (!item[key].is_number() || item[key].get<double>() < low || item[key].get<double>() > high) {
            error = at + "." + key + " must be a number " + number(low, 0) + ".." + number(high, 0) + ".";
            return false;
        }
        out = item[key].get<float>();
        return true;
    };
    auto flag = [&](const char* key, bool& out) -> bool {
        if (!has(item, key)) return true;
        if (!item[key].is_boolean()) {
            error = at + "." + key + " must be true or false.";
            return false;
        }
        out = item[key].get<bool>();
        return true;
    };
    if (!text("with", spec.with) || !text("container", spec.container) || !integer("count", spec.count) ||
        !integer("min", spec.min) || !integer("max", spec.max) || !real("tolerance", spec.tolerance, 0, 1000) ||
        !real("min_ratio", spec.minRatio, 1, 21) || !real("width", spec.minWidth, 0, 10000) ||
        !real("height", spec.minHeight, 0, 10000) || !flag("unobstructed", spec.unobstructed) ||
        !flag("allow_scroll", spec.allowScroll) || !flag("single_line", spec.singleLine)) {
        return false;
    }
    // virtual_window: max is the most rows the view may hold (default its own bound)
    if (spec.kind == hui::AssertKind::VirtualWindow)
        spec.maxPool = spec.max;
    if (has(item, "text")) {
        if (!item["text"].is_string()) {
            error = at + ".text must be a string.";
            return false;
        }
        spec.text = item["text"].get<std::string>();
    }
    if (spec.kind == hui::AssertKind::Selected && spec.count < 0 && spec.min < 0 && spec.max < 0) {
        error = at + ": selected needs count, min or max.";
        return false;
    }
    if (spec.kind == hui::AssertKind::Count && spec.count < 0 && spec.min < 0 && spec.max < 0) {
        error = at + ": count needs count, min or max.";
        return false;
    }
    if (has(item, "background")) {
        const Json& bg = item["background"];
        if (!bg.is_array() || bg.size() < 3 || bg.size() > 4 || !bg[0].is_number() || !bg[1].is_number() || !bg[2].is_number()) {
            error = at + ".background must be [r,g,b] 0-1 (sRGB), e.g. [0.5, 0.5, 0.5].";
            return false;
        }
        spec.hasBackground = true;
        for (int c = 0; c < 3; c++) spec.background[c] = std::clamp(bg[c].get<float>(), 0.0f, 1.0f);
    }
    return true;
}

Json rectJson(const hui::InspectRect& r) {
    return box1(r);
}

void assertUiTool(const CallPtr& call) {
    const Json& args = call->args();
    if (!args.contains("checks") || !args["checks"].is_array() || args["checks"].empty()) {
        call->fail("checks must be a non-empty array, e.g. [{\"type\": \"text_fits\", \"selector\": \".btn\"}, "
                   "{\"type\": \"no_overlap\", \"selector\": \".btn\"}].");
        return;
    }
    if (args["checks"].size() > 50) {
        call->fail("At most 50 checks per call.");
        return;
    }
    std::vector<hui::AssertSpec> specs;
    std::string error;
    for (size_t i = 0; i < args["checks"].size(); i++) {
        hui::AssertSpec spec;
        if (!parseCheck(args["checks"][i], i, spec, error)) {
            call->fail(error);
            return;
        }
        specs.push_back(spec);
    }
    std::vector<std::pair<double, double>> viewports;
    if (has(args, "viewports")) {
        if (!args["viewports"].is_array() || args["viewports"].empty() || args["viewports"].size() > 6) {
            call->fail("viewports must be 1 to 6 sizes [[w,h], ...] in CSS px.");
            return;
        }
        for (size_t i = 0; i < args["viewports"].size(); i++) {
            double w = 0, h = 0;
            if (!readViewport(args["viewports"][i], w, h, error, "viewports[" + std::to_string(i) + "]")) {
                call->fail(error);
                return;
            }
            viewports.emplace_back(w, h);
        }
    }
    const std::string mode = args.value("screenshot", std::string("on_failure"));
    if (mode != "on_failure" && mode != "always" && mode != "never") {
        call->fail("screenshot must be on_failure, always or never.");
        return;
    }

    withDocument(call, [call, specs, viewports, mode](const DocTarget& target) {
        const Json& args = call->args();
        Project* project = call->ctx().project();
        SceneProject* sp = targetScene(project, target);
        DocumentView view;
        std::string error;
        if (!documentView(project, sp, target.entity, view, error)) {
            call->fail(error);
            return;
        }
        const double currentW = std::round(view.info.width), currentH = std::round(view.info.height);
        std::vector<std::pair<double, double>> sizes = viewports;
        if (sizes.empty()) sizes.emplace_back(currentW, currentH);

        struct Run {
            size_t check;
            double width, height;
            bool current;
            hui::AssertResult result;
        };
        std::vector<Run> runs;
        std::vector<Outline> matchedOutlines;   // screenshot always: what the checks matched (current layout)
        for (const auto& size : sizes) {
            const double w = size.first, h = size.second;
            const bool current = w == currentW && h == currentH;
            auto runAll = [&]() {
                for (size_t i = 0; i < specs.size(); i++) {
                    runs.push_back({i, w, h, current, hui::HtmlUIAssertions::run(sp->scene, target.entity, specs[i])});
                }
            };
            if (current) {
                runAll();
            } else if (!hui::HtmlUIInspector::withViewport(sp->scene, target.entity, static_cast<float>(w), static_cast<float>(h),
                                                           runAll, error)) {
                call->fail(error);
                return;
            }
        }
        for (const Run& run : runs) {
            if (!run.result.error.empty()) {
                call->fail("checks[" + std::to_string(run.check) + "] (" + specs[run.check].selector + "): " + run.result.error);
                return;
            }
        }
        if (mode == "always") {
            for (const hui::AssertSpec& spec : specs) {
                for (const std::string& selector : {spec.selector, spec.with}) {
                    if (selector.empty()) continue;
                    std::vector<hui::InspectElement> found;
                    bool more = false;
                    std::string ignored;
                    if (!hui::HtmlUIInspector::query(sp->scene, target.entity, selector, 0, 50, found, more, ignored)) continue;
                    for (const hui::InspectElement& e : found) {
                        if (!e.flags.hidden && !e.flags.zeroSize) matchedOutlines.push_back(outline(e.border, CYAN));
                    }
                }
            }
        }

        Json checks = Json::array();
        size_t passed = 0;
        std::string lines;
        std::vector<Outline> failureOutlines;
        bool otherViewportFailures = false;
        for (const Run& run : runs) {
            const hui::AssertSpec& spec = specs[run.check];
            const std::string type = assertKindNames()[static_cast<size_t>(spec.kind)];
            const std::string at = viewportText(run.width, run.height);
            Json failures = Json::array();
            for (size_t f = 0; f < run.result.failures.size(); f++) {
                const hui::AssertFailure& failure = run.result.failures[f];
                if (f < MAX_FAILURES) {
                    Json item = {{"ref", failure.ref}, {"message", failure.message}, {"rect", rectJson(failure.rect)}};
                    if (!failure.otherRef.empty()) {
                        item["other_ref"] = failure.otherRef;
                        item["other_rect"] = rectJson(failure.otherRect);
                    }
                    failures.push_back(item);
                    lines += "\nFAIL " + type + " \"" + spec.selector + "\" @" + at + ": " + failure.ref + ": " + failure.message;
                }
                if (run.current) {
                    if (!failure.otherRef.empty()) failureOutlines.push_back(outline(failure.otherRect, AMBER));
                    failureOutlines.push_back(outline(failure.rect, RED));
                } else {
                    otherViewportFailures = true;
                }
            }
            if (run.result.failures.size() > MAX_FAILURES) {
                lines += "\n... " + std::to_string(run.result.failures.size() - MAX_FAILURES) + " more failures of " + type + " \"" +
                         spec.selector + "\" @" + at;
            }
            if (!run.result.inconclusive.empty()) {
                std::string refs;
                for (const std::string& ref : run.result.inconclusive) refs += (refs.empty() ? "" : ", ") + ref;
                lines += "\nINCONCLUSIVE " + type + " \"" + spec.selector + "\" @" + at + ": " + refs +
                         " (the backdrop is an image, a gradient or the transparent page: pass background)";
            }
            Json item = {{"index", run.check}, {"type", type}, {"selector", spec.selector},
                         {"viewport", Json::array({std::lround(run.width), std::lround(run.height)})},
                         {"passed", run.result.passed}, {"matched", run.result.matched}, {"failures", failures},
                         {"inconclusive", run.result.inconclusive}};
            if (run.result.failures.size() > MAX_FAILURES) item["failures_truncated"] = true;
            checks.push_back(item);
            if (run.result.passed) passed++;
        }
        const size_t total = runs.size();
        Json data = {{"document", view.json}, {"passed", passed == total}, {"failed", total - passed}, {"total", total},
                     {"checks", checks}};
        std::string text = "assert_ui: " + std::to_string(passed) + "/" + std::to_string(total) + " passed." + lines;
        ToolResult result = ToolResult::success(text, data);

        const bool currentFailures = !failureOutlines.empty();
        const bool image = mode == "always" || (mode == "on_failure" && currentFailures);
        if (otherViewportFailures && mode != "never") {
            result.text += "\nFailures at other viewports have no image (the Game view shows the current layout, " +
                           viewportText(currentW, currentH) + ").";
        }
        if (!image) {
            call->reply(std::move(result));
            return;
        }
        std::vector<Outline> outlines = std::move(matchedOutlines);
        outlines.insert(outlines.end(), failureOutlines.begin(), failureOutlines.end());
        replyWithImage(call, std::move(result), target, std::move(outlines), maxWidthArg(args));
    });
}

// ------------------------------------------------------------------ create_ui_document

bool endsWithHtml(const std::string& path) {
    const std::string lower = lowerCopy(path);
    return (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".html") == 0) ||
           (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".htm") == 0);
}

bool isExampleTemplate(const std::string& name) {
    const Json& manifest = HtmlUiExamples::manifest();
    if (!manifest.contains("examples") || !manifest["examples"].is_array()) return false;
    for (const Json& entry : manifest["examples"]) {
        if (entry.value("name", std::string()) == name) return entry.value("example", false);
    }
    return false;
}

void createUiDocumentTool(const CallPtr& call) {
    const Json& args = call->args();
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    std::string error;
    SceneProject* sp = McpRefs::resolveScene(project, has(args, "scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    if (!sp->scene) {
        call->fail(McpRefs::notOpenError(project, *sp));
        return;
    }
    if (sp->playState != ScenePlayState::STOPPED) {
        call->fail("Scene " + std::to_string(sp->id) + " is playing; stop play mode before editing it.");
        return;
    }
    const std::string assetsDir = w4::projectRelative(project, project->getAssetsPath());
    if (!has(args, "html") || !args["html"].is_string() || args["html"].get<std::string>().empty()) {
        call->fail("html is required: a project-relative .html in the assets directory, e.g. " + assetsDir + "/ui/menu.html.");
        return;
    }
    fs::path htmlAbs;
    if (!McpRefs::resolveProjectWritePath(project, args["html"].get<std::string>(), htmlAbs, error, false)) {
        call->fail(error);
        return;
    }
    htmlAbs = htmlAbs.lexically_normal();
    const std::string stem = FileUtils::pathToUtf8(htmlAbs.stem());
    if (!endsWithHtml(FileUtils::pathToUtf8(htmlAbs.filename()))) {
        call->fail("html must be an .html (or .htm) file, e.g. " + assetsDir + "/ui/" + (stem.empty() ? "menu" : stem) +
                   ".html; its .css is created next to it.");
        return;
    }
    if (!project->isInsideAssetsPath(htmlAbs) || htmlAbs.lexically_normal() == project->getAssetsPath().lexically_normal()) {
        call->fail("html must be inside the assets directory (" + assetsDir + "/), e.g. " + assetsDir + "/ui/" + stem + ".html.");
        return;
    }
    const std::string htmlRel = w4::projectRelative(project, htmlAbs);
    const std::string templateName = args.value("template", std::string("blank"));
    if (!HtmlUiExamples::has(templateName)) {
        std::string names;
        for (const std::string& n : HtmlUiExamples::names()) names += (names.empty() ? "" : ", ") + n;
        call->fail("Unknown template \"" + templateName + "\". Templates: " + names + ".");
        return;
    }
    if (has(args, "properties") && !args["properties"].is_object()) {
        call->fail("properties must be an object of UI Document properties, e.g. {\"renderMode\": \"texture\"}.");
        return;
    }
    if (has(args, "properties") && args["properties"].contains("filename")) {
        call->fail("Set the document with html, not properties.filename.");
        return;
    }
    Entity parent = NULL_ENTITY;
    if (has(args, "parent")) {
        parent = McpRefs::resolveEntity(sp, args["parent"], error);
        if (parent == NULL_ENTITY) {
            call->fail(error);
            return;
        }
    }
    const bool overwrite = args.value("overwrite", false);
    const bool wantScript = has(args, "script") ? args["script"].get<bool>() : isExampleTemplate(templateName);

    // the files (spec §3.7.4)
    const std::string luaDir = w4::projectRelative(project, project->getLuaPath());
    HtmlUiExamples::Instance instance;
    if (!HtmlUiExamples::instantiate(templateName, htmlRel, luaDir == "." ? std::string() : luaDir, instance, error)) {
        call->fail(error);
        return;
    }
    std::error_code ec;
    const bool htmlExists = fs::exists(htmlAbs, ec);
    std::vector<w4::WriteFilesCmd::Item> items;
    Json written = Json::array(), kept = Json::array();
    for (const auto& [rel, content] : instance.files) {
        const bool isLua = !instance.luaPath.empty() && rel == instance.luaPath;
        if (isLua && !wantScript) continue;
        if (!isLua && htmlExists && !overwrite) continue;   // an existing html: nothing from the template but its Lua
        const fs::path abs = (project->getProjectPath() / FileUtils::pathFromUtf8(rel)).lexically_normal();
        if (fs::exists(abs, ec) && !overwrite) {
            kept.push_back(rel);
            continue;
        }
        std::string why;
        if (w4::isEditorOwnedFile(project, abs, why)) {
            call->fail(rel + ": " + why);
            return;
        }
        items.push_back({abs, content});
        written.push_back(rel);
    }
    if (htmlExists && !overwrite) kept.insert(kept.begin(), htmlRel);

    // the GameObject first (no file yet: nothing loads), then the files, then its html
    CommandHistory* history = CommandHandle::get(sp->id);
    const size_t mark = history->mark();
    CommandHistory* projectHistory = project->getProjectCommandHistory();
    const size_t projectMark = projectHistory->position();
    auto rollback = [call, sp, history, mark, projectHistory, projectMark](const std::string& why) {
        const bool undone = history->position() > mark || projectHistory->position() > projectMark;
        // a refused undo leaves the position, so it ends a loop too
        while (history->position() > mark && history->canUndo() && history->undo()) {}
        while (projectHistory->position() > projectMark && projectHistory->canUndo() && projectHistory->undo()) {}
        sp->needUpdateRender = true;
        // a refused undo stops the rollback: what stayed is said (the history positions against the marks)
        const bool sceneLeft = history->position() > mark, projectLeft = projectHistory->position() > projectMark;
        if (sceneLeft || projectLeft) {
            std::string left = sceneLeft ? std::to_string(history->position() - mark) + " scene step(s)" : std::string();
            if (projectLeft) left += std::string(left.empty() ? "" : " and ") + std::to_string(projectHistory->position() - projectMark) + " project step(s)";
            const std::string refusal = projectLeft ? projectHistory->lastRefusal() : history->lastRefusal();
            call->fail(why + " The rollback stopped: " + left + " of this call could not be undone" +
                       (refusal.empty() ? std::string(".") : " (" + refusal + ")") + " Undo them with the undo tool once fixed.");
            return;
        }
        call->fail(undone ? "Nothing was created (the call was rolled back): " + why : why);
    };
    auto add = [history](Command* command) {
        command->setNoMerge();
        return history->addCommand(command);
    };

    const std::string name = has(args, "name") && args["name"].is_string() && !args["name"].get<std::string>().empty()
                                 ? args["name"].get<std::string>() : HtmlUiExamples::titleName(stem);
    auto* create = parent != NULL_ENTITY ? new CreateEntityCmd(project, sp->id, name, EntityCreationType::UI_DOCUMENT, parent)
                                         : new CreateEntityCmd(project, sp->id, name, EntityCreationType::UI_DOCUMENT);
    if (!add(create)) {
        rollback("creating the GameObject failed.");
        return;
    }
    const Entity entity = create->getEntity();

    const Json properties = args.value("properties", Json::object());
    if (has(args, "sort_order") || !properties.empty()) {
        auto multi = std::make_unique<MultiPropertyCmd>();
        Json values = properties;
        if (has(args, "sort_order")) values["sortOrder"] = args["sort_order"];
        for (auto it = values.begin(); it != values.end(); ++it) {
            PropertyData property = Catalog::findProperty(sp->scene, entity, ComponentType::UIDocumentComponent, it.key());
            if (!property.ref) {
                std::vector<std::string> names;
                for (const auto& item : Catalog::findEntityProperties(sp->scene, entity, ComponentType::UIDocumentComponent)) {
                    if (item.first.find('[') == std::string::npos && item.first != "filename") names.push_back(item.first);
                }
                std::string list;
                for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
                rollback("UI Document has no property \"" + it.key() + "\". Properties: " + list + ".");
                return;
            }
            std::string valueError;
            Command* command = McpValues::buildPropertyCommand(project, sp->id, entity, ComponentType::UIDocumentComponent,
                                                               it.key(), property, it.value(), valueError);
            if (!command) {
                rollback("UI Document." + it.key() + ": " + valueError);
                return;
            }
            multi->addCommand(std::unique_ptr<Command>(command));
        }
        if (!add(multi.release())) {
            rollback("setting the UI Document properties failed.");
            return;
        }
    }

    if (!items.empty() && !w4::writeFilesUndoable(ctx, items, error)) {
        rollback(error);
        return;
    }

    {
        PropertyData property = Catalog::findProperty(sp->scene, entity, ComponentType::UIDocumentComponent, "filename");
        std::string valueError;
        Command* command = property.ref ? McpValues::buildPropertyCommand(project, sp->id, entity, ComponentType::UIDocumentComponent,
                                                                          "filename", property, Json(htmlRel), valueError)
                                        : nullptr;
        if (!command || !add(command)) {
            rollback("setting the UI Document's html failed" + (valueError.empty() ? std::string(".") : ": " + valueError));
            return;
        }
    }

    const std::string luaPath = wantScript ? instance.luaPath : std::string();
    auto finish = [call, sp, entity, history, mark, rollback, htmlRel, templateName, written, kept, luaPath](const ToolResult* attached) {
        if (attached && attached->isError) {
            rollback("attach_script " + luaPath + ": " + attached->text);
            return;
        }
        history->groupSince(mark);
        const uint32_t sceneId = sp->id;
        auto loadedState = [call, sceneId, entity]() {
            SceneProject* s = call->ctx().project()->getScene(sceneId);
            UIDocumentComponent* component = documentComponent(s, entity);
            return !component || ((component->loaded || component->loadFailed) && !component->needReload);
        };
        call->waitUntil(loadedState, LOAD_WAIT_MS, [call, sceneId, entity, htmlRel, templateName, written, kept, luaPath](bool timedOut) {
            Project* project = call->ctx().project();
            SceneProject* s = project->getScene(sceneId);
            if (!s || !s->scene || !documentComponent(s, entity)) {
                call->fail("The new GameObject is gone (the scene changed meanwhile).");
                return;
            }
            DocumentView view;
            std::string error;
            Json data = {{"entity", McpRefs::entityJson(s, entity)}, {"scene", s->id}, {"html", htmlRel}, {"template", templateName},
                         {"files_written", written}, {"files_kept", kept},
                         {"script", luaPath.empty() ? Json(nullptr) : Json{{"path", luaPath}, {"attached", true}}}};
            std::string diagnosticsText;
            if (documentView(project, s, entity, view, error)) {
                data["document"] = view.json;
                diagnosticsText = addDiagnostics(data, view.diagnostics.findings);
            }
            std::string files;
            for (const Json& f : written) files += (files.empty() ? "" : ", ") + f.get<std::string>();
            std::string text = "Created \"" + s->scene->getEntityName(entity) + "\" (id " + std::to_string(entity) + ") in scene \"" +
                               s->name + "\" showing " + htmlRel + " (template " + templateName + ")" +
                               (files.empty() ? std::string() : "; wrote " + files) + ".";
            if (!kept.empty()) {
                std::string keptText;
                for (const Json& f : kept) keptText += (keptText.empty() ? "" : ", ") + f.get<std::string>();
                text += " Kept existing " + keptText + " (overwrite replaces them).";
            }
            if (!luaPath.empty()) text += " Script " + luaPath + " attached.";
            UIDocumentComponent* component = documentComponent(s, entity);
            if (component && component->loaded) {
                text += " Loaded: " + view.header.substr(view.header.rfind(": ") + 2) + "." + diagnosticsText;
            } else {
                text += std::string(" The document ") + (timedOut ? "was still loading after 5 s." : "did not load:") + diagnosticsText;
            }
            text += "\nUndo: undo (scene) removes the GameObject; undo scope project removes the files.";
            call->ok(text, data);
        });
    };

    if (luaPath.empty()) {
        finish(nullptr);
        return;
    }
    Json attachArgs = {{"scene", sp->id}, {"entity", entity}, {"path", luaPath}};
    call->server().runNested(call, "attach_script", attachArgs, [finish](const ToolResult& result) { finish(&result); });
}

// ------------------------------------------------------------------ resources

std::string mimeOf(const std::string& path) {
    const std::string ext = lowerCopy(FileUtils::pathToUtf8(FileUtils::pathFromUtf8(path).extension()));
    if (ext == ".html" || ext == ".htm") return "text/html";
    if (ext == ".css") return "text/css";
    if (ext == ".lua") return "text/x-lua";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".json") return "application/json";
    if (ext == ".woff") return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".otf") return "font/otf";
    if (ext == ".webm") return "video/webm";   // phase 6: video_menu's background (a blob)
    return "text/plain";
}

std::string examplesIndex() {
    const Json& manifest = HtmlUiExamples::manifest();
    const Json examples = manifest.value("examples", Json::array());
    size_t count = 0;
    for (const Json& entry : examples) if (entry.value("example", false)) count++;
    std::string text = "# HTML/CSS UI examples\n\n" + std::to_string(count) +
        " complete game UI examples plus templates, each a standalone .html + .css + Lua script (and assets), compat-clean "
        "and tested with the assert_ui checks listed. Read a file with jasper://examples/html-ui/<name>/<file>; every "
        "project also has them in .jasperengine/engine-api/html-ui-examples/. Guide: jasper://docs/html-ui.\n";
    for (const Json& entry : examples) {
        const std::string name = entry.value("name", std::string());
        text += "\n## " + name + ": " + entry.value("title", name) + (entry.value("example", false) ? "" : " (template)") + "\n\n";
        text += entry.value("description", std::string()) + "\n";
        if (entry.contains("demonstrates") && entry["demonstrates"].is_array() && !entry["demonstrates"].empty()) {
            std::string list;
            for (const Json& d : entry["demonstrates"]) if (d.is_string()) list += (list.empty() ? "" : ", ") + d.get<std::string>();
            text += "Demonstrates: " + list + ".\n";
        }
        std::string files;
        for (const char* key : {"html", "css", "lua"}) {
            if (entry.contains(key) && entry[key].is_string()) files += (files.empty() ? "" : ", ") + name + "/" + entry[key].get<std::string>();
        }
        for (const Json& asset : entry.value("assets", Json::array())) {
            if (asset.is_string()) files += ", " + name + "/" + asset.get<std::string>();
        }
        text += "Files: " + files + ".\n";
        std::string sizes;
        for (const Json& v : entry.value("viewports", Json::array())) {
            if (v.is_array() && v.size() == 2) sizes += (sizes.empty() ? "" : ", ") + v[0].dump() + "x" + v[1].dump();
        }
        text += "Checks (assert_ui" + (sizes.empty() ? std::string() : " at " + sizes) + "): `" +
                entry.value("checks", Json::array()).dump() + "`\n";
    }
    text += "\n## Use one\n\n`create_ui_document {\"html\": \"assets/ui/<stem>.html\", \"template\": \"<name>\"}` writes the template "
            "renamed to your document (`<stem>.css`, `<stem>_assets/`, the Lua script `<Stem>UI.lua` in the Lua directory, "
            "attached) and creates the GameObject; then run its checks with `assert_ui {\"checks\": [...], \"viewports\": [...]}`.\n";
    return text;
}

ResourceResult resourceText(const std::string& uri, const std::string& mime, const std::string& text) {
    ResourceResult result;
    result.contents.push_back({uri, mime, text, {}});
    return result;
}

} // namespace

// ------------------------------------------------------------------ send_input target (HtmlUiShared.h)

namespace htmlui_mcp {

bool elementCenter(McpContext& ctx, const Json& documentRef, const std::string& selector, double& x, double& y,
                   std::string& error, Json* found) {
    Project* project = ctx.project();
    if (!project) {
        error = "No project is open.";
        return false;
    }
    if (selector.empty()) {
        error = "target must be a CSS selector";
        return false;
    }
    const std::vector<SceneProject*> scenes = documentScenes(project);
    const bool playing = isPlaying(project);
    struct Candidate {
        SceneProject* sp;
        Entity entity;
        int sortOrder;
        size_t order;
    };
    std::vector<Candidate> candidates;
    if (!documentRef.is_null()) {
        std::vector<std::pair<SceneProject*, Entity>> matches;
        std::string firstError;
        for (SceneProject* sp : scenes) {
            std::string refError;
            const Entity entity = playing ? w3::resolveLiveEntity(project, sp, documentRef, refError) : McpRefs::resolveEntity(sp, documentRef, refError);
            if (entity != NULL_ENTITY && documentComponent(sp, entity)) matches.emplace_back(sp, entity);
            else if (firstError.empty()) firstError = refError;
        }
        if (matches.empty()) {
            error = "document " + documentRef.dump() + " is no UI Document of the " + (playing ? "running" : "open") + " scenes" +
                    (firstError.empty() ? std::string() : " (" + firstError + ")");
            return false;
        }
        if (matches.size() > 1) {
            error = "document " + documentRef.dump() + " matches several UI Documents: " + documentList(project, matches, true) +
                    "; pass its id";
            return false;
        }
        UIDocumentComponent* component = documentComponent(matches[0].first, matches[0].second);
        if (component->renderMode == UIDocumentRenderMode::TEXTURE) {
            error = "document " + documentLabel(project, matches[0].first, matches[0].second, true) +
                    " renders to a texture: its elements are not at canvas points (click the surface it is drawn on with x, y)";
            return false;
        }
        candidates.push_back({matches[0].first, matches[0].second, component->sortOrder, 0});
    } else {
        for (SceneProject* sp : scenes) {
            for (Entity entity : hui::HtmlUIInspector::documents(sp->scene)) {
                UIDocumentComponent* component = documentComponent(sp, entity);
                if (!component || component->renderMode == UIDocumentRenderMode::TEXTURE || component->filename.empty()) continue;
                candidates.push_back({sp, entity, component->sortOrder, candidates.size()});
            }
        }
        // the topmost document first (a higher sortOrder draws on top)
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.sortOrder > b.sortOrder; });
    }
    if (candidates.empty()) {
        error = std::string("no UI Document drawn on the screen in the ") + (playing ? "running" : "open") +
                " scenes to find \"" + selector + "\" in (texture-mode documents are skipped)";
        return false;
    }
    std::string searched;
    for (const Candidate& c : candidates) {
        std::vector<hui::InspectElement> matches;
        bool more = false;
        std::string why;
        if (!hui::HtmlUIInspector::query(c.sp->scene, c.entity, selector, 0, 1000, matches, more, why)) {
            // an invalid selector is invalid in every document
            if (why.find("is not valid") != std::string::npos) {
                error = why;
                return false;
            }
            searched += (searched.empty() ? "" : "; ") + documentLabel(project, c.sp, c.entity, true) + ": " + why;
            continue;
        }
        // the first visible match; else the first transparent one: a pointer still reaches an element
        // with opacity 0 (the invisible checkbox over a styled switch), as in a browser
        const hui::InspectElement* pick = nullptr;
        for (const hui::InspectElement& e : matches) {
            if (e.flags.hidden || e.flags.zeroSize) continue;
            if (!e.flags.transparent) {
                pick = &e;
                break;
            }
            if (!pick) pick = &e;
        }
        if (pick) {
            x = pick->border.x + pick->border.width / 2.0;
            y = pick->border.y + pick->border.height / 2.0;
            if (found) {
                *found = {{"scene", c.sp->id}, {"entity", McpRefs::entityJson(c.sp, c.entity)},
                          {"html", documentHtml(project, c.sp, c.entity)}, {"ref", pick->ref}};
            }
            return true;
        }
        why = matches.empty() ? "selector \"" + selector + "\" matched nothing"
                              : "selector \"" + selector + "\" matched " + std::to_string(matches.size()) + " element" +
                                    (matches.size() == 1 ? "" : "s") + ", none shown (display: none, visibility: hidden or zero size)";
        searched += (searched.empty() ? "" : "; ") + documentLabel(project, c.sp, c.entity, true) + ": " + why;
    }
    error = "no visible element matches \"" + selector + "\". UI Documents searched (topmost first): " + searched;
    return false;
}

} // namespace htmlui_mcp

// ------------------------------------------------------------------ registration

// ------------------------------------------------------------------ edit_ui_view (S17 R41)

// edit_ui_view {entity, selector, action, index|key|column}: drives a virtual view in play (runtime only,
// not undoable): scroll_to, select, activate, expand, collapse, sort. Indices are 0-based.
void editUiViewTool(const CallPtr& call) {
    const Json& args = call->args();
    static const char* const actions[] = {"scroll_to", "select", "activate", "expand", "collapse", "sort"};
    const std::string action = args.value("action", std::string());
    if (std::find(std::begin(actions), std::end(actions), action) == std::end(actions)) {
        call->fail("action must be scroll_to, select, activate, expand, collapse or sort.");
        return;
    }
    if (!args.contains("selector") || !args["selector"].is_string() || args["selector"].get<std::string>().empty()) {
        call->fail("selector is required: the view (e.g. \"#servers\").");
        return;
    }
    if (has(args, "index") && (!args["index"].is_number_integer() || args["index"].get<long long>() < 0)) {
        call->fail("index must be an item index >= 0 (0-based, as data-for's i).");
        return;
    }
    withDocument(call, [call, action](const DocTarget& target) {
        const Json& args = call->args();
        Project* project = call->ctx().project();
        SceneProject* sp = targetScene(project, target);
        if (!isPlaying(project) || !isRunning(sp->playState)) {
            call->fail("edit_ui_view acts on a playing document (its views' data come from Lua): start play first.");
            return;
        }
        UIDocument document(sp->scene, target.entity);
        const std::string selector = args["selector"].get<std::string>();
        UIItemsView view = document.querySelector(selector).getItemsView();
        if (!view.isValid()) {
            call->fail("\"" + selector + "\" is not a list-view, grid-view, tree-view or table-view of this document.");
            return;
        }
        const int count = view.getCount();
        int index = has(args, "index") ? static_cast<int>(args["index"].get<long long>()) : -1;
        const std::string key = has(args, "key") ? (args["key"].is_string() ? args["key"].get<std::string>() : args["key"].dump()) : std::string();
        if (index < 0 && !key.empty() && action != "expand" && action != "collapse") {
            index = view.indexOfKey(key) - 1;
            if (index < 0) {
                call->fail("no item has the key \"" + key + "\".");
                return;
            }
        }
        bool done = true;
        if (action == "sort") {
            const std::string column = args.value("column", std::string());
            done = view.sortBy(column, args.value("direction", std::string()));
            if (!done) {
                call->fail("\"" + column + "\" is no sortable column of the table.");
                return;
            }
        } else if (action == "expand" || action == "collapse") {
            const std::string node = !key.empty() ? key : (index >= 0 ? view.keyOf(index + 1) : std::string());
            if (node.empty()) {
                call->fail(action + " needs key or index.");
                return;
            }
            done = action == "expand" ? view.expand(node) : view.collapse(node);
        } else {
            if (index < 0 || index >= count) {
                call->fail(action + " needs an index of 0.." + std::to_string(count - 1) + " (or a key).");
                return;
            }
            if (action == "scroll_to")
                view.scrollToIndex(index + 1, args.value("align", std::string("nearest")));
            else if (action == "select")
                view.setActiveIndex(index + 1), view.setSelection({index + 1});
            else
                view.activate(index + 1);
        }
        const UIItemsViewState state = view.getState();
        Json data = {{"done", done}, {"count", state.count}, {"first", state.first - 1}, {"last", state.last - 1}, {"active", state.active - 1},
                     {"selected", Json::array()}};
        for (int selected : state.selected) data["selected"].push_back(selected - 1);
        call->ok(selector + ": " + action + (done ? "" : " (nothing to do)") + "; items " + std::to_string(state.first - 1) + ".." +
                 std::to_string(state.last - 1) + " of " + std::to_string(state.count) + " shown.", data);
    });
}

void registerHtmlUiTools(McpRegistry& registry) {
    using namespace schema;

    {
        ToolDef tool;
        tool.name = "inspect_ui";
        tool.title = "Inspect UI Document";
        tool.description =
            "A live HTML/CSS UI document: element tree (ref = reusable selector, box [x,y,w,h] canvas px = "
            "send_input coords, flags overflow/clipped/hidden/offscreen, effects), diagnostics; detail full: box model, "
            "styles + rule file:line. at: hit test. file: a .html/.css's diagnostics (= "
            "`jasperengine-editor check --ui`). Lua-made content: play only.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("UI Document (default: the only one).")},
            {"file", assetPath(".html/.css: diagnostics only.")},
            {"selector", string("Default: whole document.")},
            {"at", vec2("Hit test (canvas px): element stack.")},
            {"detail", withDefault(stringEnum("full: box model, attributes, styles + sources (max 20 elements).",
                                              {"tree", "full"}), "tree")},
            {"depth", integer("Descendant levels (default tree 3, full 0).", 0, 16)},
            {"styles", array(Json{{"type", "string"}}, "Computed CSS properties.")},
            {"limit", withDefault(integer("", 1, 1000), 200)},
            {"models", boolean("Data-model values (play).")},
            {"reload", boolean("Last hot reload's report.")},
            {"viewport", vec2("Lay out at [w,h], then restore.")},
            {"screenshot", boolean("Game view image, matches outlined.")},
            {"render_stats", boolean("Last frame's draw counters.")},
            {"max_width", w3::maxWidthArgSchema(4096, DEFAULT_IMAGE_WIDTH)}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Read;
        tool.handler = inspectUiTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "assert_ui";
        tool.title = "Assert UI Layout";
        tool.description =
            "Assert a live HTML/CSS UI document's layout: passed + each failure (element ref, numbers, rects), "
            "on failure an image with failing boxes outlined. Checks {type, selector}: exists, count and selected "
            "(count|min|max), visible (unobstructed), text_fits (allow_scroll, single_line), no_overlap (with), "
            "within_parent (container), within_viewport, contrast (min_ratio, background where the UI is "
            "transparent), min_size (width, height), virtual_window (max rows), tooltip (text). viewports: other "
            "sizes too. Zero matches fail.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("UI Document (default: the only one).")},
            {"checks", Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 50}, {"items", object({
                {"type", stringEnum("", assertKindNames())},
                {"selector", string("")},
                {"with", string("")},
                {"container", string("")},
                {"count", Json{{"type", "integer"}, {"minimum", 0}}},
                {"min", Json{{"type", "integer"}, {"minimum", 0}}},
                {"max", Json{{"type", "integer"}, {"minimum", 0}}},
                {"tolerance", Json{{"type", "number"}, {"minimum", 0}, {"description", "px, default 0.5."}}},
                {"min_ratio", number("Default WCAG AA.", 1, 21)},
                {"background", color("")},
                {"unobstructed", boolean("")},
                {"allow_scroll", boolean("")},
                {"single_line", boolean("")},
                {"width", Json{{"type", "number"}, {"minimum", 0}}},
                {"height", Json{{"type", "number"}, {"minimum", 0}}},
                {"text", string("")}
            }, {"type", "selector"})}}},
            {"viewports", array(vec2(""), "Up to 6 [w,h]; default: current.")},
            {"screenshot", withDefault(stringEnum("", {"on_failure", "always", "never"}), "on_failure")},
            {"max_width", w3::maxWidthArgSchema(4096, DEFAULT_IMAGE_WIDTH)}
        }, {"checks"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Read;
        tool.handler = assertUiTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "create_ui_document";
        tool.title = "Create UI Document";
        tool.description =
            "Create a GameObject with a UI Document showing an HTML/CSS file (one scene undo step). A missing html is made "
            "from template (blank or an example) with its .css, assets and Lua (script: attached); existing files are "
            "kept unless overwrite. Returns the files and the first load's diagnostics.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"parent", entityRef()},
            {"name", string("Default: from the file name.")},
            {"html", assetPath(".html in the assets dir.")},
            {"template", withDefault(stringEnum("", HtmlUiExamples::names()), "blank")},
            {"script", boolean("Attach the template's Lua (default: examples yes, blank no).")},
            {"sort_order", integer("", -32768, 32767)},
            {"overwrite", boolean("")},
            {"properties", anyObject("Other UI Document properties.")}
        }, {"html"});
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.allowedWhilePlaying = false;
        tool.activity = ToolActivity::Edit;
        tool.handler = createUiDocumentTool;
        registry.addTool(std::move(tool));
    }

    {
        ToolDef tool;
        tool.name = "edit_ui_view";
        tool.title = "Edit UI View";
        tool.description =
            "Drive a list-view, grid-view, tree-view or table-view of a playing HTML UI document (runtime only, not "
            "undoable): scroll_to (align), select, activate (its activate event), expand / collapse (tree), sort (table: "
            "its sort event, column, direction). index is 0-based; key names an item instead.";
        tool.inputSchema = object({
            {"scene", sceneRef()},
            {"entity", entityRef("With the UI Document (default: the only one).")},
            {"selector", string("The view, e.g. #servers.")},
            {"action", stringEnum("", {"scroll_to", "select", "activate", "expand", "collapse", "sort"})},
            {"index", integer("0-based item.", 0, 10000000)},
            {"key", string("An item's key instead of index.")},
            {"column", string("sort: the column key.")},
            {"direction", stringEnum("sort: default toggles.", {"asc", "desc"})},
            {"align", stringEnum("scroll_to: default nearest.", {"start", "center", "end", "nearest"})}
        }, {"selector", "action"});
        tool.annotations.readOnly = false;
        tool.annotations.destructive = false;
        tool.allowedWhilePlaying = true;
        tool.activity = ToolActivity::Control;
        tool.group = "html_ui";   // S17: a group tool (docs/mcp/conventions.md)
        tool.handler = editUiViewTool;
        registry.addTool(std::move(tool));
    }

    {
        ResourceDef index;
        index.uri = "jasper://examples/html-ui";
        index.name = "html-ui-examples";
        index.title = "HTML/CSS UI examples";
        index.description = "The HTML/CSS UI example library (menus, HUD, inventory, settings, dialogue, shop, toasts, loading): "
                            "what each shows, its files and its assert_ui checks.";
        index.mimeType = "text/markdown";
        index.read = [](const std::string& uri) { return resourceText(uri, "text/markdown", examplesIndex()); };
        registry.addResource(std::move(index));
    }
    {
        ResourceTemplateDef tmpl;
        tmpl.uriTemplate = "jasper://examples/html-ui/{+path}";
        tmpl.name = "html-ui-example-file";
        tmpl.title = "HTML/CSS UI example file";
        tmpl.description = "A file of the example library by path, e.g. jasper://examples/html-ui/main_menu/main_menu.css.";
        tmpl.mimeType = "text/plain";
        tmpl.read = [](const std::string& uri, const std::map<std::string, std::string>& vars) {
            const std::string path = vars.count("path") ? vars.at("path") : std::string();
            if (const HtmlUiExamples::File* file = HtmlUiExamples::find(path)) {
                const std::string mime = mimeOf(path);
                if (mime.rfind("video/", 0) == 0 || mime.rfind("font/", 0) == 0) {
                    ResourceResult result;
                    result.contents.push_back({uri, mime, {}, Base64::encode(reinterpret_cast<const unsigned char*>(file->data.data()),
                                                                             file->data.size())});
                    return result;
                }
                return resourceText(uri, mime, std::string(file->data));
            }
            std::string names;
            for (const std::string& n : HtmlUiExamples::names()) names += (names.empty() ? "" : ", ") + n;
            ResourceResult result;
            result.ok = false;
            result.error = "No example file \"" + path + "\" (examples: " + names + "; their files are listed in jasper://examples/html-ui).";
            return result;
        };
        registry.addResourceTemplate(std::move(tmpl));
    }
}

} // namespace jasperengine::editor::mcp
