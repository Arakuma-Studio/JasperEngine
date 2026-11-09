// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S26 (docs/specs/S26-scene-view.md R34): the Scene view's picking, camera bookmarks, layouts,
// measures, Layers filter and mesh raycasts for agents, through SceneViewController (what the
// Scene view's own toolbar and clicks use). Group editor_ux. set_scene_camera's S26 options are
// in ViewTools.cpp. Resource jasper://scene-view/{scene}.

#include "McpTools.h"
#include "PlayViewCommon.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "component/TerrainWorldComponent.h"
#include "object/Camera.h"
#include "picking/MeshRaycast.h"
#include "picking/PickCache.h"
#include "picking/SceneBvh.h"
#include "render/SceneRender.h"
#include "sceneview/SceneViewController.h"
#include "sceneview/SceneViewVisibility.h"
#include "terrain/FoliageTypes.h"
#include "terrainworld/TerrainWorldTrees.h"
#include "util/Base64.h"
#include "util/GraphicUtils.h"
#include "window/SceneWindow.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace jasperengine::editor::mcp {

namespace {

constexpr int READY_TIMEOUT_MS = 10000;

SceneViewController* controllerOf(McpContext& ctx) {
    SceneWindow* window = ctx.sceneWindow();
    return window ? &window->getViewController() : nullptr;
}

// The scene and its Scene view, shown and laid out, before then()
void withSceneView(const CallPtr& call, std::function<void(SceneProject& scene, SceneViewController& view)> then) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = w3::resolveSceneViewScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    const uint32_t sceneId = sp->id;
    w3::whenSceneViewShown(call, sceneId, true, READY_TIMEOUT_MS, [call, sceneId, then](bool ready, const std::string& err) {
        if (!ready) {
            call->fail(err);
            return;
        }
        SceneProject* scene = call->ctx().project()->getScene(sceneId);
        SceneViewController* view = controllerOf(call->ctx());
        if (!scene || !scene->sceneRender || !view) {
            call->fail("The scene closed.");
            return;
        }
        view->ensureApplied(*scene);
        then(*scene, *view);
    });
}

Json poseJson(const SceneCameraPose& pose) {
    return {{"position", McpValues::toJson(pose.position)}, {"target", McpValues::toJson(pose.target)},
            {"up", McpValues::toJson(pose.up)}, {"projection", pose.orthographic ? "orthographic" : "perspective"},
            {"ortho_size", pose.orthoSize}, {"fov", pose.fieldOfView}};
}

Json hitJson(Project* project, const ScenePickEntry& entry) {
    SceneProject* scene = project->getScene(entry.sceneId);
    Json out = {{"entity", entry.entity}, {"scene_id", entry.sceneId}, {"kind", pickKindName(entry.hit.kind)},
                {"submesh", entry.hit.submesh}, {"triangle", entry.hit.triangle}, {"instance", entry.hit.instance},
                {"point", McpValues::toJson(entry.hit.point)}, {"normal", McpValues::toJson(entry.hit.normal)},
                {"distance", entry.hit.distance}, {"pending", entry.hit.pending}};
    out["path"] = scene ? McpRefs::entityPath(scene, entry.entity) : std::string();
    if (entry.hit.kind == PickKind::TREE || entry.hit.kind == PickKind::DETAIL) {
        // T5 R35: the GameObject is the Terrain, these say what on it
        out["prototype"] = entry.hit.prototype;
        if (entry.hit.kind == PickKind::TREE) {
            // a Terrain World's tree as edit_terrain's remove_trees names it: tile and record (T4 R27)
            const bool world = scene && scene->scene && scene->scene->findComponent<TerrainWorldComponent>(entry.entity);
            out["tree_id"] = world ? terrainWorldTreeId(terrain::TileCoord::fromKey(terrain::treeIdRegion(entry.hit.foliageId)), terrain::treeIdRecord(entry.hit.foliageId))
                                   : entry.hit.foliageId;
        }
    }
    return out;
}

Json entryRefJson(Project* project, const ScenePickEntry& entry) {
    SceneProject* scene = project->getScene(entry.sceneId);
    return {{"entity", entry.entity}, {"scene_id", entry.sceneId},
            {"path", scene ? McpRefs::entityPath(scene, entry.entity) : std::string()}};
}

const char* actionName(ScenePickClick::Action action) {
    switch (action) {
        case ScenePickClick::Action::NONE: return "none";
        case ScenePickClick::Action::CLEAR: return "clear";
        case ScenePickClick::Action::SELECT: return "select";
        case ScenePickClick::Action::ADD: return "add";
        case ScenePickClick::Action::TOGGLE: return "toggle";
    }
    return "none";
}

// x/y (pixels of the full-size Scene view image) or u/v (0..1) of the active pane's view
bool screenPoint(const Json& args, const char* xKey, const char* yKey, const char* uKey, const char* vKey, SceneProject& scene,
                 float& x, float& y, std::string& error) {
    const float width = (float)std::max(scene.sceneRender->getViewWidth(), 1);
    const float height = (float)std::max(scene.sceneRender->getViewHeight(), 1);
    if (args.contains(xKey) && args.contains(yKey)) {
        x = args.value(xKey, 0.0f);
        y = args.value(yKey, 0.0f);
    } else if (args.contains(uKey) && args.contains(vKey)) {
        x = args.value(uKey, 0.0f) * width;
        y = args.value(vKey, 0.0f) * height;
    } else {
        error = std::string("Give ") + xKey + "/" + yKey + " (pixels of the Scene view image) or " + uKey + "/" + vKey + " (0..1).";
        return false;
    }
    if (x < 0.0f || y < 0.0f || x > width || y > height) {
        error = "The point is outside the Scene view (" + std::to_string((int)width) + "x" + std::to_string((int)height) + ").";
        return false;
    }
    return true;
}

// ---- pick_at

void pickAtTool(const CallPtr& call) {
    withSceneView(call, [call](SceneProject& scene, SceneViewController& view) {
        const Json& args = call->args();
        const uint32_t sceneId = scene.id;
        const int pane = args.value("pane", scene.viewState.activePane);
        if (pane < 0 || pane >= scene.viewState.paneCount()) {
            call->fail("pane must be 0.." + std::to_string(scene.viewState.paneCount() - 1) + " (set_scene_view_layout lists them).");
            return;
        }
        // another pane: it becomes the active one, drawn, then the point is its
        const int frames = pane == scene.viewState.activePane ? 0 : 3;
        view.setActivePane(scene, pane);
        call->afterFrames(frames, [call, sceneId]() {
            Project* project = call->ctx().project();
            SceneProject* sp = project->getScene(sceneId);
            SceneViewController* controller = controllerOf(call->ctx());
            if (!sp || !sp->sceneRender || !controller) {
                call->fail("The scene closed.");
                return;
            }
            const Json& a = call->args();
            float x = 0.0f, y = 0.0f;
            std::string error;
            if (!screenPoint(a, "x", "y", "u", "v", *sp, x, y, error)) {
                call->fail(error);
                return;
            }
            const std::string apply = a.value("apply", std::string("none"));
            if (apply != "none" && apply != "select" && apply != "click") {
                call->fail("apply must be none, select or click.");
                return;
            }
            SceneClickInput input;
            input.x = x;
            input.y = y;
            input.waitForBvh = true;
            input.shift = a.value("shift", false);
            input.ctrl = a.value("ctrl", false);
            if (a.contains("exact")) input.exactPick = a.value("exact", false);
            input.apply = apply != "none";
            if (apply == "select") {
                // a fresh click: no same-spot cycling
                controller->resetClickHistory(sceneId);
            }
            const SceneClickOutcome outcome = controller->click(sceneId, input);
            Json hits = Json::array();
            for (const ScenePickEntry& entry : outcome.result.hits) hits.push_back(hitJson(project, entry));
            Json candidates = Json::array();
            for (const ScenePickEntry& entry : outcome.result.candidates) candidates.push_back(entryRefJson(project, entry));
            Json data = {{"hits", hits}, {"candidates", candidates}, {"point", {x, y}}};
            data["would_select"] = outcome.click.target.entity != NULL_ENTITY ? entryRefJson(project, outcome.click.target) : Json(nullptr);
            data["action"] = actionName(outcome.click.action);
            data["cycle"] = {{"index", outcome.click.cycleIndex}, {"count", outcome.click.cycleCount}};
            if (input.apply) {
                Json selected = Json::array();
                const uint32_t selectionScene = project->getSelectedSceneForProperties();
                SceneProject* selScene = project->getScene(selectionScene);
                for (Entity entity : project->getSelectedEntities(selectionScene)) {
                    selected.push_back({{"entity", entity}, {"scene_id", selectionScene},
                                        {"path", selScene ? McpRefs::entityPath(selScene, entity) : std::string()}});
                }
                data["selected"] = selected;
            }
            std::string text = std::to_string(outcome.result.hits.size()) + " hit(s) at (" + std::to_string((int)x) + ", " +
                               std::to_string((int)y) + ")";
            if (outcome.click.target.entity != NULL_ENTITY) {
                text += "; " + std::string(input.apply ? "selected " : "a click selects ") + data["would_select"].value("path", std::string());
                if (outcome.click.cycleCount > 1) {
                    text += " (" + std::to_string(outcome.click.cycleIndex + 1) + "/" + std::to_string(outcome.click.cycleCount) + ")";
                }
            } else if (outcome.click.action == ScenePickClick::Action::CLEAR) {
                text += input.apply ? "; cleared the selection" : "; a click clears the selection";
            }
            call->ok(text + ".", data);
        });
    });
}

// ---- edit_scene_view_bookmarks

Json bookmarksJson(SceneProject& scene, SceneViewController& view) {
    Json list = Json::array();
    for (int slot = 0; slot < SceneViewState::BOOKMARK_SLOTS; slot++) {
        const CameraBookmark& bookmark = scene.viewState.bookmarks[(size_t)slot];
        Json item = {{"slot", slot}, {"name", SceneViewController::bookmarkName(scene.viewState, slot)}, {"defined", bookmark.defined}};
        if (bookmark.defined) {
            item["pose"] = poseJson(bookmark.pose);
            std::error_code ec;
            const std::filesystem::path preview = view.bookmarkPreviewPath(scene, slot);
            if (std::filesystem::exists(preview, ec)) item["preview_path"] = preview.generic_string();
        }
        list.push_back(item);
    }
    return list;
}

void bookmarkTool(const CallPtr& call) {
    withSceneView(call, [call](SceneProject& scene, SceneViewController& view) {
        const Json& args = call->args();
        const std::string action = args.value("action", std::string("list"));
        const int slot = args.value("slot", -1);
        const uint32_t sceneId = scene.id;
        std::string error;
        if (action == "list") {
            Json data = {{"bookmarks", bookmarksJson(scene, view)}};
            call->ok("Camera bookmarks of \"" + scene.name + "\".", data);
            return;
        }
        if (slot < 0 || slot >= SceneViewState::BOOKMARK_SLOTS) {
            call->fail("slot must be 0 (Home) to 9.");
            return;
        }
        if (action == "store") {
            // the preview reads the view as drawn: one more frame first
            scene.needUpdateRender = true;
            call->afterFrames(2, [call, sceneId, slot]() {
                SceneProject* sp = call->ctx().project()->getScene(sceneId);
                SceneViewController* controller = controllerOf(call->ctx());
                std::string err;
                if (!sp || !controller || !controller->storeBookmark(*sp, slot, err)) {
                    call->fail(err.empty() ? std::string("The scene closed.") : err);
                    return;
                }
                Json data = {{"bookmarks", bookmarksJson(*sp, *controller)}, {"slot", slot}};
                call->ok("Stored camera bookmark " + std::to_string(slot) + ".", data);
            });
            return;
        }
        bool ok = false;
        if (action == "recall") {
            ok = view.recallBookmark(scene, slot, error);
            view.finishTween(scene);
        } else if (action == "delete") {
            ok = view.deleteBookmark(scene, slot, error);
        } else if (action == "rename") {
            if (!args.contains("name")) {
                call->fail("rename needs name.");
                return;
            }
            ok = view.renameBookmark(scene, slot, args.value("name", std::string()), error);
        } else {
            call->fail("action must be list, store, recall, delete or rename.");
            return;
        }
        if (!ok) {
            call->fail(error);
            return;
        }
        Json data = {{"bookmarks", bookmarksJson(scene, view)}, {"slot", slot}, {"camera", w3::sceneViewCameraJson(&scene)}};
        w3::replyWithSceneShot(call, sceneId, "Camera bookmark " + std::to_string(slot) + ": " + action + ".", data);
    });
}

// ---- set_scene_view_layout

Json layoutJson(SceneProject& scene, SceneViewController& view) {
    Json panes = Json::array();
    for (int pane = 0; pane < scene.viewState.paneCount(); pane++) {
        const SceneViewPane& state = scene.viewState.panes[(size_t)pane];
        int width = 0, height = 0;
        view.paneViewSize(scene, pane, width, height);
        panes.push_back({{"pane", pane}, {"axis", SceneViewStateStream::axisName(state.axis)},
                         {"shading", state.shading == PaneShading::SHADED_SHADOWS ? "shaded_shadows" : "shaded"},
                         {"active", pane == scene.viewState.activePane}, {"size", {width, height}},
                         {"pose", poseJson(view.panePose(scene, pane))}, {"draws", view.paneDrawCount(scene, pane)}});
    }
    return {{"layout", SceneViewStateStream::layoutName(scene.viewState.layout)}, {"active_pane", scene.viewState.activePane},
            {"maximized_pane", scene.viewState.maximizedPane}, {"panes", panes}};
}

void layoutTool(const CallPtr& call) {
    withSceneView(call, [call](SceneProject& scene, SceneViewController& view) {
        const Json& args = call->args();
        const uint32_t sceneId = scene.id;
        if (args.contains("layout")) {
            SceneViewLayout layout;
            if (!args["layout"].is_string() || !SceneViewStateStream::parseLayout(args["layout"].get<std::string>(), layout)) {
                call->fail("layout must be single, split or quad.");
                return;
            }
            if (layout != SceneViewLayout::SINGLE && scene.sceneType != SceneType::SCENE_3D) {
                call->fail("Split and Quad are for 3D scenes.");
                return;
            }
            view.setLayout(scene, layout);
        }
        if (args.contains("pane_settings")) {
            for (const Json& entry : args["pane_settings"]) {
                const int pane = entry.value("pane", -1);
                if (pane < 0 || pane >= scene.viewState.paneCount()) {
                    call->fail("pane_settings: pane must be 0.." + std::to_string(scene.viewState.paneCount() - 1) + ".");
                    return;
                }
                if (entry.contains("axis")) {
                    SceneViewAxis axis;
                    if (!entry["axis"].is_string() || !SceneViewStateStream::parseAxis(entry["axis"].get<std::string>(), axis)) {
                        call->fail("pane_settings: axis must be free, top, bottom, front, back, left or right.");
                        return;
                    }
                    view.setPaneAxis(scene, pane, axis);
                    view.finishTween(scene);
                }
                if (entry.contains("shading")) {
                    view.setPaneShading(scene, pane, entry.value("shading", std::string("shaded")) == "shaded_shadows"
                                                         ? PaneShading::SHADED_SHADOWS : PaneShading::SHADED);
                }
            }
        }
        if (args.contains("active_pane")) {
            const int pane = args.value("active_pane", 0);
            if (pane < 0 || pane >= scene.viewState.paneCount()) {
                call->fail("active_pane must be 0.." + std::to_string(scene.viewState.paneCount() - 1) + ".");
                return;
            }
            view.setActivePane(scene, pane);
        }
        if (args.contains("maximized_pane")) {
            const int pane = args.value("maximized_pane", -1);
            if (pane != scene.viewState.maximizedPane) {
                view.toggleMaximized(scene, pane < 0 ? scene.viewState.maximizedPane : pane);
            }
        }
        scene.needUpdateRender = true;
        const bool capture = args.value("capture", false);
        // the panes laid out and drawn before they are read or captured
        call->afterFrames(4, [call, sceneId, capture]() {
            SceneProject* sp = call->ctx().project()->getScene(sceneId);
            SceneViewController* controller = controllerOf(call->ctx());
            if (!sp || !sp->sceneRender || !controller) {
                call->fail("The scene closed.");
                return;
            }
            Json data = layoutJson(*sp, *controller);
            ToolResult reply = ToolResult::success("Scene view layout of \"" + sp->name + "\": " + data["layout"].get<std::string>() + ".", data);
            if (capture) {
                for (int pane = 0; pane < sp->viewState.paneCount(); pane++) {
                    Framebuffer* framebuffer = pane == sp->viewState.activePane ? sp->sceneRender->getFramebuffer()
                                                                                 : controller->paneFramebuffer(*sp, pane);
                    std::vector<uint8_t> png;
                    if (framebuffer && GraphicUtils::encodeFramebufferPng(framebuffer, png, false, 1024)) {
                        ToolImage image;
                        image.data = Base64::encode(png.data(), png.size());
                        reply.images.push_back(std::move(image));
                    }
                }
            }
            call->reply(std::move(reply));
        });
    });
}

// ---- measure_scene_view

Json measureJson(const SceneMeasure& measure) {
    const Vector3 delta = measure.to - measure.from;
    return {{"id", measure.id}, {"from", McpValues::toJson(measure.from)}, {"to", McpValues::toJson(measure.to)},
            {"length", delta.length()}, {"delta", McpValues::toJson(delta)}};
}

void measureTool(const CallPtr& call) {
    withSceneView(call, [call](SceneProject& scene, SceneViewController& view) {
        const Json& args = call->args();
        const std::string action = args.value("action", std::string("list"));
        if (action == "list") {
            Json list = Json::array();
            for (const SceneMeasure& measure : scene.viewState.measures) list.push_back(measureJson(measure));
            call->ok(std::to_string(list.size()) + " measure(s) in \"" + scene.name + "\".", Json{{"measures", list}});
            return;
        }
        if (action == "clear") {
            view.clearMeasures(scene);
            call->ok("Cleared the measures of \"" + scene.name + "\".", Json{{"measures", Json::array()}});
            return;
        }
        if (action == "delete") {
            const uint32_t id = args.value("id", 0u);
            if (!view.deleteMeasure(scene, id)) {
                call->fail("No measure " + std::to_string(id) + " (action list shows them).");
                return;
            }
            call->ok("Deleted measure " + std::to_string(id) + ".", Json{{"id", id}});
            return;
        }
        if (action != "add") {
            call->fail("action must be add, list, delete or clear.");
            return;
        }
        Vector3 from, to;
        const std::string snapName = args.value("snap", std::string("surface"));
        MeasureSnap snap = MeasureSnap::SURFACE;
        if (snapName == "vertex") snap = MeasureSnap::VERTEX;
        else if (snapName == "grid") snap = MeasureSnap::GRID;
        else if (snapName == "none") snap = MeasureSnap::NONE;
        else if (snapName != "surface") {
            call->fail("snap must be surface, vertex, grid or none.");
            return;
        }
        auto endpoint = [&](const char* world, const char* screen, Vector3& out, std::string& error) {
            if (args.contains(world)) {
                if (!McpValues::parseVector3(args[world], out)) {
                    error = std::string(world) + " must be [x,y,z].";
                    return false;
                }
                return true;
            }
            if (args.contains(screen) && args[screen].is_array() && args[screen].size() == 2) {
                const float x = args[screen][0].get<float>();
                const float y = args[screen][1].get<float>();
                if (!view.measurePoint(scene, x, y, snap, out)) {
                    error = std::string("Nothing under ") + screen + ".";
                    return false;
                }
                return true;
            }
            error = std::string("add needs ") + world + " (world) or " + screen + " ([x,y] pixels of the Scene view image).";
            return false;
        };
        std::string error;
        if (!endpoint("from", "from_screen", from, error) || !endpoint("to", "to_screen", to, error)) {
            call->fail(error);
            return;
        }
        const uint32_t id = view.addMeasure(scene, from, to);
        const SceneMeasure& measure = scene.viewState.measures.back();
        Json data = measureJson(measure);
        call->ok("Measure " + std::to_string(id) + ": " + sceneview::formatLength((to - from).length(), scene.sceneType == SceneType::SCENE_3D) + ".",
                 data);
    });
}

// ---- set_scene_view_filter

Json filterJson(Project* project, SceneProject& scene) {
    Json hidden = Json::array(), locked = Json::array();
    for (Entity entity : scene.viewState.hiddenEntities) {
        if (scene.scene->isEntityCreated(entity)) hidden.push_back(McpRefs::entityJson(&scene, entity));
    }
    for (Entity entity : scene.viewState.pickLockedEntities) {
        if (scene.scene->isEntityCreated(entity)) locked.push_back(McpRefs::entityJson(&scene, entity));
    }
    (void)project;
    return {{"visible_layers", scene.viewState.layerFilter.visible}, {"pickable_layers", scene.viewState.layerFilter.pickable},
            {"hidden", hidden}, {"locked", locked}};
}

void filterTool(const CallPtr& call) {
    withSceneView(call, [call](SceneProject& scene, SceneViewController& view) {
        const Json& args = call->args();
        Project* project = call->ctx().project();
        const bool children = args.value("children", true);
        std::string error;
        // every reference first: a bad one changes nothing
        std::vector<std::pair<std::string, Entity>> changes;
        for (const char* key : {"hide", "show", "lock", "unlock"}) {
            if (!args.contains(key)) continue;
            for (const Json& ref : args[key]) {
                const Entity entity = w3::resolveLiveEntity(project, &scene, ref, error);
                if (entity == NULL_ENTITY) {
                    call->fail(error);
                    return;
                }
                changes.push_back({key, entity});
            }
        }
        if (args.contains("visible_layers") || args.contains("pickable_layers")) {
            const uint32_t visible = args.contains("visible_layers") ? args["visible_layers"].get<uint32_t>() : scene.viewState.layerFilter.visible;
            const uint32_t pickable = args.contains("pickable_layers") ? args["pickable_layers"].get<uint32_t>() : scene.viewState.layerFilter.pickable;
            view.setLayerFilter(scene, visible, pickable);
        }
        if (args.value("show_all", false)) {
            view.showAll(scene);
        }
        for (const auto& [key, entity] : changes) {
            if (key == "hide" || key == "show") sceneview::setHidden(scene, entity, key == "hide", children);
            else sceneview::setPickLocked(scene, entity, key == "lock", children);
        }
        project->saveWorkspaceFile();
        Json data = filterJson(project, scene);
        w3::replyWithSceneShot(call, scene.id, "Scene view filter of \"" + scene.name + "\": " + std::to_string(data["hidden"].size()) +
                                                   " hidden, " + std::to_string(data["locked"].size()) + " locked.", data);
    });
}

// ---- raycast_meshes

void raycastTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    SceneProject* sp = w3::resolveSceneViewScene(project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!sp) {
        call->fail(error);
        return;
    }
    Vector3 origin, direction;
    if (!args.contains("origin") || !McpValues::parseVector3(args["origin"], origin) || !args.contains("direction") ||
            !McpValues::parseVector3(args["direction"], direction)) {
        call->fail("origin and direction must be [x,y,z].");
        return;
    }
    if (!(direction.length() > 0.0f)) {
        call->fail("direction must not be zero.");
        return;
    }
    MeshRaycastOptions options;
    options.hitBackfaces = args.value("backfaces", false);
    const float maxDistance = args.value("max_distance", 1.0e6f);
    const uint32_t layerMask = args.contains("layer_mask") ? args["layer_mask"].get<uint32_t>() : LAYER_MASK_DEFAULT_RAYCAST;
    std::vector<MeshRaycastHit> hits;
    if (args.value("all", false)) {
        hits = MeshRaycast::raycastAll(sp->scene, origin, direction, maxDistance, layerMask, options);
    } else {
        const MeshRaycastHit hit = MeshRaycast::raycast(sp->scene, origin, direction, maxDistance, layerMask, options);
        if (hit.hit) hits.push_back(hit);
    }
    Json list = Json::array();
    for (const MeshRaycastHit& hit : hits) {
        list.push_back({{"entity", hit.entity}, {"path", McpRefs::entityPath(sp, hit.entity)}, {"kind", pickKindName(hit.kind)},
                        {"submesh", hit.submesh}, {"triangle", hit.triangle}, {"instance", hit.instance},
                        {"point", McpValues::toJson(hit.point)}, {"normal", McpValues::toJson(hit.normal)},
                        {"face_normal", McpValues::toJson(hit.faceNormal)}, {"barycentric", McpValues::toJson(hit.barycentric)},
                        {"distance", hit.distance}, {"pending", hit.pending}});
    }
    const MeshRaycastStats stats = MeshRaycast::getStats();
    Json data = {{"hits", list}, {"stats", {{"built", stats.built}, {"pending", stats.pending}, {"bytes", stats.bytes},
                                             {"build_ms_total", stats.buildMsTotal}, {"inline_waits", stats.inlineWaits}}}};
    call->ok(list.empty() ? std::string("No hit.") : std::to_string(list.size()) + " hit(s), nearest " + list[0].value("path", std::string()) + ".",
             data);
}

// ---- jasper://scene-view/{scene}

ResourceResult sceneViewResource(const std::string& uri, const std::map<std::string, std::string>& vars) {
    ResourceResult result;
    App& app = Backend::getApp();
    Project* project = app.getProject();
    SceneWindow* window = app.getSceneWindow();
    auto it = vars.find("scene");
    if (!project || !window || it == vars.end()) {
        result.ok = false;
        result.error = "No project or Scene view.";
        return result;
    }
    Json ref = it->second;
    if (!it->second.empty() && std::all_of(it->second.begin(), it->second.end(), [](char c) { return std::isdigit((unsigned char)c); })) {
        ref = std::stoul(it->second);
    }
    std::string error;
    SceneProject* sp = w3::resolveSceneViewScene(project, ref, error);
    if (!sp) {
        result.ok = false;
        result.error = error;
        return result;
    }
    SceneViewController& view = window->getViewController();
    view.ensureApplied(*sp);
    Json measures = Json::array();
    for (const SceneMeasure& measure : sp->viewState.measures) measures.push_back(measureJson(measure));
    const PickCacheStats cache = PickCache::get().stats();
    Json data = {{"scene", McpRefs::sceneJson(project, *sp)}, {"camera", w3::sceneViewCameraJson(sp)}, {"layout", layoutJson(*sp, view)},
                 {"bookmarks", bookmarksJson(*sp, view)}, {"measures", measures}, {"filters", filterJson(project, *sp)},
                 {"bvh_cache", {{"built", cache.built}, {"pending", cache.pending}, {"bytes", cache.bytes},
                                {"build_ms_total", cache.buildMsTotal}, {"disk_loads", cache.diskLoads}, {"inline_waits", cache.inlineWaits}}}};
    // a click waiting for a mesh's BVH (R33): the status bar's text, null when none
    const std::string waiting = view.pendingStatus(sp->id);
    data["pending_click"] = waiting.empty() ? Json(nullptr) : Json(waiting);
    if (SceneBvh* bvh = sp->scene ? sp->scene->findPickBvh() : nullptr) {
        const SceneBvh::Stats stats = bvh->getStats();
        data["scene_bvh"] = {{"leaves", stats.leaves}, {"builds", stats.builds}, {"refits", stats.refits}};
    }
    result.contents.push_back({uri, "application/json", data.dump(1), ""});
    return result;
}

} // namespace

void registerSceneViewTools(McpRegistry& registry) {
    using namespace schema;
    const Json viewScene = sceneRef("Default: playing, else selected.");
    const char* GROUP = "editor_ux";

    {
        ToolDef tool;
        tool.name = "pick_at";
        tool.group = GROUP;
        tool.title = "Pick at Scene View Point";
        tool.description =
            "What a Scene view click at x,y (pixels of the full-size scene screenshot) or u,v (0-1) hits: hits nearest first, "
            "the candidates a click cycles through, would_select. apply select: select that; click: the full click "
            "(root, drill, same-spot cycling).";
        tool.inputSchema = object({
            {"scene", viewScene},
            {"x", number("")}, {"y", number("")}, {"u", number("", 0, 1)}, {"v", number("", 0, 1)},
            {"pane", integer("Split/Quad pane (becomes active).", 0, 3)},
            {"apply", withDefault(stringEnum("", {"none", "select", "click"}), "none")},
            {"exact", boolean("Exact Pick: no roots. Default: the toolbar's.")},
            {"shift", boolean("Click adds.")}, {"ctrl", boolean("Click toggles.")}
        });
        tool.activity = ToolActivity::Control;
        tool.attributesUndo = true;
        tool.handler = pickAtTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_scene_view_bookmarks";
        tool.group = GROUP;
        tool.title = "Scene View Camera Bookmarks";
        tool.description =
            "Camera bookmarks of a Scene view (slot 0 Home, 1-9): list (poses, preview PNG paths), store the current view, "
            "recall, delete, rename. Per user, in the workspace.";
        tool.inputSchema = object({
            {"scene", viewScene},
            {"action", withDefault(stringEnum("", {"list", "store", "recall", "delete", "rename"}), "list")},
            {"slot", integer("", 0, 9)},
            {"name", string("rename")},
            {"screenshot", boolean("")}
        });
        tool.activity = ToolActivity::Control;
        tool.handler = bookmarkTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_scene_view_layout";
        tool.group = GROUP;
        tool.title = "Set Scene View Layout";
        tool.description =
            "Single, Split (Persp | Top) or Quad (Top, Front, Side, Persp) Scene view; per pane axis and shading. Returns the "
            "panes (axis, size, pose, draws). capture: an image per pane. No arguments: reads.";
        tool.inputSchema = object({
            {"scene", viewScene},
            {"layout", stringEnum("", {"single", "split", "quad"})},
            {"active_pane", integer("", 0, 3)},
            {"maximized_pane", integer("-1: none.", -1, 3)},
            {"pane_settings", array(object({{"pane", integer("", 0, 3)},
                                            {"axis", stringEnum("", {"free", "top", "bottom", "front", "back", "left", "right"})},
                                            {"shading", stringEnum("", {"shaded", "shaded_shadows"})}}, {"pane"}), "")},
            {"capture", boolean("")}
        });
        tool.activity = ToolActivity::Control;
        tool.handler = layoutTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "measure_scene_view";
        tool.group = GROUP;
        tool.title = "Scene View Measures";
        tool.description =
            "Measures of a Scene view (workspace data, world anchored): add from/to (world) or from_screen/to_screen "
            "([x,y] image pixels, snapped), list, delete id, clear. Returns id, from, to, length, delta.";
        tool.inputSchema = object({
            {"scene", viewScene},
            {"action", withDefault(stringEnum("", {"add", "list", "delete", "clear"}), "list")},
            {"from", vec3("")}, {"to", vec3("")},
            {"from_screen", vec2("")}, {"to_screen", vec2("")},
            {"snap", withDefault(stringEnum("", {"surface", "vertex", "grid", "none"}), "surface")},
            {"id", integer("delete")}
        });
        tool.activity = ToolActivity::Control;
        tool.handler = measureTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "set_scene_view_filter";
        tool.group = GROUP;
        tool.title = "Set Scene View Filter";
        tool.description =
            "What the Scene view shows and picks (Unity's Layers and SceneVisibility; editor only, per user): layer masks, "
            "hide/show and lock/unlock GameObjects (with children unless children false), show_all. No arguments: reads.";
        tool.inputSchema = object({
            {"scene", viewScene},
            {"visible_layers", integer("Mask.", 0, 4294967295LL)},
            {"pickable_layers", integer("Mask.", 0, 4294967295LL)},
            {"hide", entities()}, {"show", entities()}, {"lock", entities()}, {"unlock", entities()},
            {"children", withDefault(boolean(""), true)},
            {"show_all", boolean("")},
            {"screenshot", boolean("")}
        });
        tool.activity = ToolActivity::Control;
        tool.handler = filterTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "raycast_meshes";
        // the MCP face of Lua's MeshRaycast, beside the scripting tools
        tool.group = "scripting";
        tool.title = "Raycast Meshes";
        tool.description =
            "MeshRaycast (Lua's): triangle-exact ray against meshes, instances, terrains and UI, edit or play, no physics. "
            "distance in world units; all: every hit, nearest first. Returns BVH cache stats.";
        tool.inputSchema = object({
            {"scene", viewScene},
            {"origin", vec3("")}, {"direction", vec3("")},
            {"max_distance", number("", 0, 1.0e9)},
            {"layer_mask", integer("Default: all but Ignore Raycast.", 0, 4294967295LL)},
            {"all", boolean("")},
            {"backfaces", boolean("")}
        }, {"origin", "direction"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = raycastTool;
        registry.addTool(std::move(tool));
    }
    {
        ResourceTemplateDef def;
        def.uriTemplate = "jasper://scene-view/{scene}";
        def.name = "scene-view";
        def.title = "Scene View State";
        def.description = "A Scene view: camera, layout and panes, bookmarks, measures, filters, BVH cache stats.";
        def.mimeType = "application/json";
        def.read = sceneViewResource;
        registry.addResourceTemplate(std::move(def));
    }
}

}
