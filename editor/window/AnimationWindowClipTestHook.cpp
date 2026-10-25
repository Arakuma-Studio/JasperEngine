// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The clip mode's part of the MCP window test hook (focus_window {window: "animation",
// animation: {...}}, JASPER_MCP_TEST_HOOKS=1 only; AnimationClipAppHooks::windowTestHook, set by
// AnimationWindowClipMode.cpp, which handles `entity`, `entity_animation`, `clip` and `root`).
// The actions run the same functions the window's buttons, menus and shortcuts run, in this
// order: state, time / frame, preview, play, expand, select / box, drag, ease, delete, copy,
// paste, duplicate, key, add_key, add_property, set_value, remove_track, rebind, frame_rate,
// snap_keys, time_format, preview_model, add_event, select_event, event, drag_event,
// delete_event, create_clip, extract, view, curve_visible, frame_curves, curve_drag, record,
// gizmo, inspector, morph, lattice_drag, input. The result (`viewport_input` of focus_window) is
// stateJson(): mode, clip, state, rows, missing, selection, time, preview, events, view, curves,
// recording...
// `input` queues real key events (Space, K, comma...) handled by the focused window on the next
// frame. T3c: the record sources run through the same code as the Scene view and the Inspector:
// `gizmo` through SceneRender::recordGizmoTransform (else the ObjectTransformCmd the drag builds),
// `inspector` / `morph` through the Inspector's Transform / Blend Shapes row functions, and
// `lattice_drag` through LatticeEditorOps' continuous edit (the Scene view's lattice point drag).

#include "AnimationWindowClip.h"
#include "AnimationWindowClipUtil.h"

#include "AnimationWindowClipCurves.h"
#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "component/AnimationPlayerComponent.h"
#include "command/CommandHandle.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/PropertyCmd.h"
#include "lattice/LatticeEditorOps.h"
#include "render/SceneRender.h"
#include "util/EntityPath.h"
#include "window/InspectorWindow.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using Json = nlohmann::json;

namespace {

    const char* const KNOWN_KEYS[] = {
        "entity", "entity_animation", "clip", "root", "state", "time", "frame", "preview", "play", "expand", "select", "add",
        "box", "drag", "ease", "delete", "copy", "paste", "duplicate", "key", "add_key", "add_property", "set_value",
        "remove_track", "rebind", "frame_rate", "snap_keys", "time_format", "preview_model", "add_event", "select_event",
        "event", "drag_event", "delete_event", "create_clip", "extract", "input", "list_paths", "event_editor", "entity_play",
        "entity_mode_entries", "entity_move_frame", "brief", "view", "curve_visible", "frame_curves", "curve_drag", "record",
        "gizmo", "inspector", "morph", "lattice_drag", "curve_select", "drop"};

    Json argJson(const AnimationEventArg& arg){
        if (std::holds_alternative<double>(arg)) return std::get<double>(arg);
        if (std::holds_alternative<std::string>(arg)) return std::get<std::string>(arg);
        return std::get<bool>(arg);
    }

    bool argsFromJson(const Json& json, std::vector<AnimationEventArg>& out, std::string& error){
        out.clear();
        if (!json.is_array()){
            error = "args: an array of numbers, strings and bools";
            return false;
        }
        for (const Json& value : json){
            if (value.is_boolean()) out.push_back(AnimationEvent::boolArg(value.get<bool>()));
            else if (value.is_number()) out.push_back(AnimationEvent::numberArg(value.get<double>()));
            else if (value.is_string()) out.push_back(AnimationEvent::stringArg(value.get<std::string>()));
            else{
                error = "args: numbers, strings and bools only";
                return false;
            }
        }
        return true;
    }

}

Json AnimationWindowClip::stateJson(SceneProject* sceneProject, bool brief){
    (void)sceneProject;
    AnimationClipController& c = controller();
    refreshReport();
    refreshSelection();
    std::shared_ptr<AnimationClipDocument> doc = document();
    SceneProject* bound = boundScene();
    const Entity root = c.getRoot();
    Json out = Json::object();
    const bool rootAlive = bound && root != NULL_ENTITY && bound->scene->isEntityCreated(root);
    out["mode"] = "clip";
    out["root"] = rootAlive ? Json(bound->scene->getEntityName(root)) : Json();
    out["has_player"] = c.hasPlayer();
    out["states"] = c.getStateNames();
    out["state"] = c.getStateName().empty() ? Json() : Json(c.getStateName());
    out["clip"] = doc ? Json(doc->path()) : Json();
    out["read_only"] = doc && doc->isReadOnly();
    out["targetless"] = doc && root == NULL_ENTITY;
    out["preview_model"] = doc ? Json(doc->data().previewModel) : Json();
    out["empty_state"] = !doc && rootAlive;
    if (!doc && rootAlive) out["empty_text"] = "To begin animating " + bound->scene->getEntityName(root) + ", create an Animation Clip.";
    out["time"] = c.getTime();
    out["frame"] = (int)std::lround(c.getTime() * frameRate());
    out["frame_rate"] = frameRate();
    out["length"] = doc ? doc->data().effectiveLength() : 0.0f;
    out["preview"] = c.isPreviewing();
    out["playing"] = c.isPlaying();
    out["time_format"] = showFrames ? "frames" : "seconds";
    out["takes_keys"] = c.takesKeys();
    // responsiveness: the CPU time of the clip mode's draw (moving average, worst) and frames
    out["draw_ms"] = drawMs;
    out["draw_ms_max"] = drawMsMax;
    out["draw_frames"] = drawFrames;
    out["frame_count"] = ImGui::GetFrameCount();
    out["preview_blocker"] = previewBlocker(project->getSelectedScene());
    out["preview_on"] = previewModelOf();
    out["undo_to_project"] = c.newestStepIsClipEdit();
    {
        const Entity previewRoot = c.getPreviewRoot();
        SceneProject* previewScene = c.getPreviewSceneId() ? project->getScene(c.getPreviewSceneId()) : nullptr;
        out["preview_root"] = previewScene && previewRoot != NULL_ENTITY && previewScene->scene->isEntityCreated(previewRoot)
            ? Json(previewScene->scene->getEntityName(previewRoot)) : Json();
    }
    out["notice"] = notice;
    out["notice_warning"] = noticeWarning;
    Json rows = Json::array();
    Json missing = Json::array();
    if (doc){
        const ClipData& data = doc->data();
        for (const Row& row : buildRows()){
            if (row.kind == Row::Kind::Summary) continue;
            const ClipTrackData& track = data.tracks[(size_t)row.track];
            Json entry = {{"label", row.label}, {"track", row.track}, {"missing", row.missing}};
            if (row.kind == Row::Kind::Component){
                entry["component"] = row.component;
            }else{
                entry["path"] = track.path;
                entry["channel"] = anim::channelName(track.channel);
                Json times = Json::array();
                if (!brief){
                    for (const ClipKey& key : track.keys) times.push_back(key.time);
                }
                entry["keys"] = times;
                entry["key_count"] = track.keys.size();
                entry["value"] = sampledValue((size_t)row.track, c.getTime());
                entry["pending"] = c.hasPendingValue((size_t)row.track);
                entry["expanded"] = expanded.count(track.path + "|" + anim::channelName(track.channel)) > 0;
                if (!row.message.empty()) entry["message"] = row.message;
                if (row.missing) missing.push_back(row.label);
            }
            rows.push_back(entry);
        }
        Json events = Json::array();
        for (size_t i = 0; i < data.events.size(); i++){
            Json args = Json::array();
            for (const AnimationEventArg& arg : data.events[i].args) args.push_back(argJson(arg));
            std::string handler = "unchecked";
            for (const EventReport& entry : report.events){
                if (entry.event != i) continue;
                handler = entry.status == EventHandlerStatus::Found ? "found" : entry.status == EventHandlerStatus::NotFound ? "missing" : "unchecked";
            }
            events.push_back({{"time", data.events[i].time}, {"function", data.events[i].function}, {"args", args}, {"handler", handler}});
        }
        out["events"] = events;
        resolveSelectedEvent();
        out["selected_event"] = selectedEvent;
        out["script_functions"] = scriptFunctions();
        out["bindings"] = {{"resolved", report.resolved}, {"total", report.total}, {"legacy_model_layout", report.legacyModelLayout}};
    }
    out["rows"] = rows;
    out["missing"] = missing;
    Json selected = Json::array();
    out["selection_count"] = selectedRefs.size();
    if (doc && !brief){
        for (const AnimationClipController::KeyRef& ref : selectedRefs){
            const ClipTrackData& track = doc->data().tracks[ref.track];
            selected.push_back({{"track", ref.track}, {"key", ref.key}, {"path", track.path},
                                {"channel", anim::channelName(track.channel)}, {"time", track.keys[ref.key].time}});
        }
    }
    out["selection"] = selected;
    out["clipboard"] = clipboard.size();
    // T3c: the view, the curves (in the curves view), Record
    out["view"] = view == View::Curves ? "curves" : "dopesheet";
    out["label_width"] = labelWidth;
    if (view == View::Curves){
        out["curves"] = curvesJson();
        out["curve_range"] = {curveMin, curveMax};
    }
    out["recording"] = c.isRecording();
    out["record_status"] = recordStatus(true);
    out["record_status_short"] = recordStatus(false);
    return out;
}

bool AnimationWindowClip::checkHookKeys(const Json& hook, bool clipActions, std::string& error){
    for (auto it = hook.begin(); it != hook.end(); ++it){
        bool known = false;
        for (const char* key : KNOWN_KEYS) known = known || it.key() == key;
        if (!known){
            std::string valid;
            for (const char* key : KNOWN_KEYS) valid += (valid.empty() ? "" : ", ") + std::string(key);
            error = it.key() + ": unknown key (valid: " + valid + ")";
            return false;
        }
        const bool windowKey = it.key() == "entity" || it.key() == "entity_animation" || it.key() == "clip" || it.key() == "root" ||
                               it.key() == "entity_play" || it.key() == "entity_mode_entries" ||
                               it.key() == "entity_move_frame" || it.key() == "brief" || it.key() == "drop";
        if (!clipActions && !windowKey){
            error = it.key() + ": the window is in entity mode (select a GameObject with an Animation Player, or open a clip)";
            return false;
        }
    }
    return true;
}

bool AnimationWindowClip::testHook(SceneProject* sceneProject, const Json& hook, Json& out, std::string& error){
    AnimationClipController& c = controller();
    refreshBinding();
    auto fail = [&](const std::string& key, const std::string& message){
        error = key + ": " + message;
        return false;
    };
    std::string failure;
    // a key of the open clip: {track, time} or {path, channel, time}
    auto keyIdOf = [&](const Json& json, KeyId& id) -> bool{
        std::shared_ptr<AnimationClipDocument> doc = document();
        if (!doc || !json.is_object() || !json.contains("time") || !json["time"].is_number()) return false;
        id.time = json["time"].get<float>();
        if (json.contains("track")){
            if (!json["track"].is_number_integer()) return false;
            const int track = json["track"].get<int>();
            if (track < 0 || track >= (int)doc->data().tracks.size()) return false;
            id.path = doc->data().tracks[(size_t)track].path;
            id.channel = doc->data().tracks[(size_t)track].channel;
        }else{
            if (!json.contains("path") || !json["path"].is_string() || !json.contains("channel") || !json["channel"].is_string()) return false;
            id.path = json["path"].get<std::string>();
            if (!anim::channelFromName(json["channel"].get<std::string>(), id.channel)) return false;
        }
        AnimationClipController::KeyRef ref;
        if (!AnimationClipController::findKey(doc->data(), id.path, id.channel, id.time, ref)) return false;
        id.time = doc->data().tracks[ref.track].keys[ref.key].time;
        return true;
    };
    auto trackIndex = [&](const Json& json, int& out) -> bool{
        std::shared_ptr<AnimationClipDocument> doc = document();
        if (!doc || !json.is_number_integer()) return false;
        out = json.get<int>();
        return out >= 0 && out < (int)doc->data().tracks.size();
    };

    if (hook.contains("state")){
        if (!hook["state"].is_string()) return fail("state", "a state name of the bound Animation Player");
        if (!openState(hook["state"].get<std::string>(), &failure)) return fail("state", failure);
    }
    if (hook.contains("time")){
        if (!hook["time"].is_number()) return fail("time", "seconds (number)");
        seek(hook["time"].get<float>());
    }
    if (hook.contains("frame")){
        if (!hook["frame"].is_number_integer()) return fail("frame", "a frame number");
        seek((float)hook["frame"].get<int>() / frameRate());
    }
    if (hook.contains("preview")){
        if (!hook["preview"].is_boolean()) return fail("preview", "true starts the preview, false stops it");
        if (hook["preview"].get<bool>() != c.isPreviewing()){
            if (hook["preview"].get<bool>()){
                if (!startPreview(sceneProject, &failure)) return fail("preview", failure);
            }else{
                stopPreview();
            }
        }
    }
    if (hook.contains("play")){
        if (!hook["play"].is_boolean()) return fail("play", "true plays, false pauses");
        if (hook["play"].get<bool>() != c.isPlaying()){
            if (hook["play"].get<bool>() && !c.isPreviewing() && !startPreview(sceneProject, &failure)) return fail("play", failure);
            togglePlay(sceneProject);
        }
    }
    if (hook.contains("expand")){
        if (!hook["expand"].is_array()) return fail("expand", "track indices to unfold (x/y/z, blend shapes)");
        std::shared_ptr<AnimationClipDocument> doc = document();
        for (const Json& value : hook["expand"]){
            int track = -1;
            if (!trackIndex(value, track)) return fail("expand", "no such track");
            const ClipTrackData& data = doc->data().tracks[(size_t)track];
            expanded.insert(data.path + "|" + anim::channelName(data.channel));
        }
    }
    if (hook.contains("select")){
        if (!hook["select"].is_array()) return fail("select", "an array of keys {track, time} or {path, channel, time}");
        std::vector<KeyId> ids;
        for (const Json& value : hook["select"]){
            KeyId id;
            if (!keyIdOf(value, id)) return fail("select", "no key " + value.dump());
            ids.push_back(id);
        }
        selectKeys(ids, hook.value("add", false));
    }
    if (hook.contains("box")){
        const Json& box = hook["box"];
        if (!box.is_object() || !box.contains("rows") || !box.contains("times") || !box["rows"].is_array() || box["rows"].size() != 2 ||
            !box["times"].is_array() || box["times"].size() != 2){
            return fail("box", "{rows: [first, last] (0 = the summary row), times: [from, to]}");
        }
        boxSelect(box["rows"][0].get<int>(), box["rows"][1].get<int>(), box["times"][0].get<float>(), box["times"][1].get<float>(),
                  hook.value("add", false));
    }
    if (hook.contains("drag")){
        // a mouse drag of the selection by `drag` seconds (frame snapped, the grabbed key = the first)
        if (!hook["drag"].is_number()) return fail("drag", "seconds (number)");
        refreshSelection();
        if (selection.empty()) return fail("drag", "no keys selected");
        dragGrabTime = selection.front().time;
        if (!beginDrag(&failure)) return fail("drag", failure);
        // the pointer moves in steps, like a real drag, then the total
        const float total = hook["drag"].get<float>();
        for (int step = 1; step <= 4; step++){
            if (!updateDrag(total * (float)step / 4.0f, &failure)) return fail("drag", failure);
        }
        endDrag();
    }
    if (hook.contains("ease")){
        EaseType ease;
        if (!hook["ease"].is_string() || !anim::easeFromName(hook["ease"].get<std::string>(), ease)) return fail("ease", "an ease name (linear, step, cubic_in_out...)");
        if (!setEase(ease, &failure)) return fail("ease", failure);
    }
    if (hook.value("delete", false) && !deleteSelected(&failure)) return fail("delete", failure);
    if (hook.value("copy", false)) copySelected();
    if (hook.contains("paste")){
        const float at = hook["paste"].is_number() ? hook["paste"].get<float>() : c.getTime();
        if (!paste(at, &failure)) return fail("paste", failure);
    }
    if (hook.value("duplicate", false) && !duplicateSelected(&failure)) return fail("duplicate", failure);
    if (hook.contains("key")){
        const std::string mode = hook["key"].is_string() ? hook["key"].get<std::string>() : std::string();
        bool ok = false;
        if (mode == "all") ok = keyAt(false, &failure);
        else if (mode == "modified") ok = keyAt(true, &failure);
        else return fail("key", "\"all\" (K) or \"modified\" (Shift+K)");
        if (!ok) return fail("key", failure);
    }
    if (hook.contains("add_key")){
        // the context menu's Add Key: {track (-1 / absent: all), time}
        const Json& add = hook["add_key"];
        if (!add.is_object() || !add.contains("time") || !add["time"].is_number()) return fail("add_key", "{track?, time}");
        const int track = add.contains("track") ? add["track"].get<int>() : -1;
        if (!addKeysAt(track, add["time"].get<float>(), &failure)) return fail("add_key", failure);
    }
    if (hook.contains("add_property")){
        const Json& add = hook["add_property"];
        ClipChannel channel;
        if (!add.is_object() || !add.contains("path") || !add["path"].is_string() || !add.contains("channel") || !add["channel"].is_string() ||
            !anim::channelFromName(add["channel"].get<std::string>(), channel)){
            return fail("add_property", "{path, channel: position|rotation|scale|morph|lattice}");
        }
        size_t track = 0;
        // as the Add Property popup: the key lands on the Samples grid
        c.setTime(snapToFrame(c.getTime()));
        if (!c.addProperty(add["path"].get<std::string>(), channel, &track, &failure)) return fail("add_property", failure);
        std::shared_ptr<AnimationClipDocument> doc = document();
        if (doc && track < doc->data().tracks.size()){
            expanded.insert(doc->data().tracks[track].path + "|" + anim::channelName(doc->data().tracks[track].channel));
        }
    }
    if (hook.contains("set_value")){
        // the value column: {track, component, value} (rotation components are Euler degrees)
        const Json& set = hook["set_value"];
        int track = -1;
        if (!set.is_object() || !set.contains("track") || !trackIndex(set["track"], track) || !set.contains("component") ||
            !set["component"].is_number_integer() || !set.contains("value") || !set["value"].is_number()){
            return fail("set_value", "{track, component, value}");
        }
        if (!setComponentValue((size_t)track, set["component"].get<int>(), set["value"].get<float>(), true, &failure)) return fail("set_value", failure);
    }
    if (hook.contains("remove_track")){
        int track = -1;
        if (!trackIndex(hook["remove_track"], track)) return fail("remove_track", "no such track");
        if (!c.removeTrack((size_t)track, &failure)) return fail("remove_track", failure);
        selection.clear();
    }
    if (hook.contains("rebind")){
        const Json& rebind = hook["rebind"];
        int track = -1;
        if (!rebind.is_object() || !rebind.contains("track") || !trackIndex(rebind["track"], track) || !rebind.contains("path") ||
            !rebind["path"].is_string()){
            return fail("rebind", "{track, path}");
        }
        if (!c.retargetTrack((size_t)track, rebind["path"].get<std::string>(), &failure)) return fail("rebind", failure);
        const size_t users = findClipUsers(project, document()->path()).size();
        setNotice("Rebound to '" + rebind["path"].get<std::string>() + "' (changes the clip for " + std::to_string(users) +
                  (users == 1 ? " user" : " users") + ")", false);
        reportRevision = ~uint64_t(0);
    }
    if (hook.contains("frame_rate")){
        if (!hook["frame_rate"].is_number()) return fail("frame_rate", "samples per second (1..240)");
        if (!setFrameRate(hook["frame_rate"].get<float>(), &failure)) return fail("frame_rate", failure);
    }
    if (hook.value("snap_keys", false) && !snapKeysToFrames(&failure)) return fail("snap_keys", failure);
    if (hook.contains("time_format")){
        const std::string format = hook["time_format"].is_string() ? hook["time_format"].get<std::string>() : std::string();
        if (format != "frames" && format != "seconds") return fail("time_format", "\"frames\" or \"seconds\"");
        showFrames = format == "frames";
    }
    if (hook.contains("preview_model")){
        if (!hook["preview_model"].is_string()) return fail("preview_model", "a model or prefab path");
        if (!setPreviewModel(hook["preview_model"].get<std::string>(), &failure)) return fail("preview_model", failure);
    }
    if (hook.contains("add_event")){
        const Json& add = hook["add_event"];
        if (!add.is_object()) return fail("add_event", "{time?, function?, args?}");
        std::vector<AnimationEventArg> args;
        if (add.contains("args") && !argsFromJson(add["args"], args, failure)) return fail("add_event", failure);
        const float at = add.contains("time") && add["time"].is_number() ? add["time"].get<float>() : snapToFrame(c.getTime());
        if (!addEvent(at, add.value("function", std::string()), args, &failure)) return fail("add_event", failure);
        openEventEditorDelay = 4;
    }
    if (hook.contains("select_event")){
        std::shared_ptr<AnimationClipDocument> doc = document();
        if (!hook["select_event"].is_number_integer() || !doc || hook["select_event"].get<int>() < -1 ||
            hook["select_event"].get<int>() >= (int)doc->data().events.size()){
            return fail("select_event", "an event index (-1: none)");
        }
        selectEvent(hook["select_event"].get<int>());
    }
    if (hook.contains("event")){
        // the event editor: {function?, args?, time?} of the selected event
        std::shared_ptr<AnimationClipDocument> doc = document();
        const Json& edit = hook["event"];
        resolveSelectedEvent();
        if (!doc || selectedEvent < 0 || selectedEvent >= (int)doc->data().events.size()) return fail("event", "no event selected");
        if (!edit.is_object()) return fail("event", "{function?, args?, time?}");
        ClipEventData event = doc->data().events[(size_t)selectedEvent];
        if (edit.contains("function")) event.function = edit["function"].get<std::string>();
        if (edit.contains("time")) event.time = edit["time"].get<float>();
        if (edit.contains("args") && !argsFromJson(edit["args"], event.args, failure)) return fail("event", failure);
        if (!editEvent(selectedEvent, event, "", &failure)) return fail("event", failure);
    }
    if (hook.contains("drag_event")){
        // a drag of the selected event marker by seconds (frame snapped, one merged step)
        std::shared_ptr<AnimationClipDocument> doc = document();
        if (!hook["drag_event"].is_number()) return fail("drag_event", "seconds (number)");
        resolveSelectedEvent();
        if (!doc || selectedEvent < 0 || selectedEvent >= (int)doc->data().events.size()) return fail("drag_event", "no event selected");
        const float base = doc->data().events[(size_t)selectedEvent].time;
        eventMergeKey = "clipevent#" + std::to_string(++eventDragSerial);
        const float total = hook["drag_event"].get<float>();
        for (int step = 1; step <= 4; step++){
            const float at = snapToFrame(std::max(0.0f, base + total * (float)step / 4.0f));
            if (!moveEvent(selectedEvent, at, eventMergeKey, &failure)) return fail("drag_event", failure);
        }
    }
    if (hook.value("delete_event", false)){
        // as Delete with the event selected (nothing selected: nothing deleted)
        resolveSelectedEvent();
        if (selectedEvent < 0) return fail("delete_event", "no event selected");
        if (!deleteEvent(selectedEvent, &failure)) return fail("delete_event", failure);
    }
    if (hook.value("event_editor", false)) openEventEditorDelay = 4;
    if (hook.contains("create_clip")){
        // the empty state's Create... / the dropdown's Create New Clip... without the save dialog
        if (!hook["create_clip"].is_string()) return fail("create_clip", "the new .anim path (project-relative)");
        if (!createNewClip(sceneProject, hook["create_clip"].get<std::string>(), &failure)) return fail("create_clip", failure);
    }
    if (hook.contains("extract")){
        if (!hook["extract"].is_string()) return fail("extract", "the new .anim path (project-relative)");
        if (!extract(sceneProject, hook["extract"].get<std::string>(), &failure)) return fail("extract", failure);
    }
    // ---- T3c: curves view, Record and the record sources ----
    Json extra = Json::object();
    auto entitiesOf = [&](const Json& json, std::vector<Entity>& out) -> bool{
        out.clear();
        std::vector<Json> names;
        if (json.is_string()) names.push_back(json);
        else if (json.is_array()) names.assign(json.begin(), json.end());
        else return false;
        for (const Json& name : names){
            if (!name.is_string()) return false;
            const std::vector<Entity> found = EntityPath::resolve(*sceneProject, name.get<std::string>());
            if (found.size() != 1) return false;
            out.push_back(found.front());
        }
        return !out.empty();
    };
    auto vectorOf = [](const Json& json, Vector3& out) -> bool{
        if (!json.is_array() || json.size() != 3) return false;
        for (const Json& v : json) if (!v.is_number()) return false;
        out = Vector3(json[0].get<float>(), json[1].get<float>(), json[2].get<float>());
        return true;
    };
    if (hook.contains("view")){
        const std::string name = hook["view"].is_string() ? hook["view"].get<std::string>() : std::string();
        if (name != "curves" && name != "dopesheet") return fail("view", "\"dopesheet\" or \"curves\"");
        view = name == "curves" ? View::Curves : View::Dopesheet;
        saveSettings();
    }
    if (hook.contains("curve_visible")){
        // a curve's visibility toggle: {track, component, visible}
        const Json& toggle = hook["curve_visible"];
        int track = -1;
        if (!toggle.is_object() || !toggle.contains("track") || !trackIndex(toggle["track"], track) || !toggle.contains("component") ||
            !toggle["component"].is_number_integer() || !toggle.contains("visible") || !toggle["visible"].is_boolean()){
            return fail("curve_visible", "{track, component, visible}");
        }
        setCurveVisible(document()->data().tracks[(size_t)track], toggle["component"].get<int>(), toggle["visible"].get<bool>());
    }
    if (hook.contains("record")){
        if (!hook["record"].is_boolean()) return fail("record", "true starts Record (and the preview), false stops it");
        if (!setRecording(sceneProject, hook["record"].get<bool>(), &failure)) return fail("record", failure);
    }
    if (hook.contains("gizmo")){
        // a transform gizmo drag in 4 steps from the current local value to the given one:
        // {entities, tool: move|rotate|scale, value: [x, y, z] (rotate: Euler degrees)}; each step
        // goes through SceneRender::recordGizmoTransform, else the ObjectTransformCmd the drag builds
        const Json& drag = hook["gizmo"];
        std::vector<Entity> entities;
        Vector3 target;
        const std::string tool = drag.is_object() && drag.contains("tool") && drag["tool"].is_string() ? drag["tool"].get<std::string>() : std::string();
        if (!drag.is_object() || !drag.contains("entities") || !entitiesOf(drag["entities"], entities) || !drag.contains("value") ||
            !vectorOf(drag["value"], target) || (tool != "move" && tool != "rotate" && tool != "scale")){
            return fail("gizmo", "{entities: [names], tool: move|rotate|scale, value: [x, y, z], select?}");
        }
        SceneRender* render = sceneProject->sceneRender;
        if (!render) return fail("gizmo", "the scene has no Scene view");
        // the gizmo works on the selection (select: false keeps the current one: selecting a
        // GameObject outside the bound hierarchy rebinds the window, which ends Record)
        if (drag.value("select", true)) project->replaceSelectedEntities(sceneProject->id, entities);
        const GizmoSelected gizmo = tool == "move" ? GizmoSelected::TRANSLATE : tool == "rotate" ? GizmoSelected::ROTATE : GizmoSelected::SCALE;
        std::vector<Transform> start;
        for (Entity entity : entities) start.push_back(sceneProject->scene->getComponent<Transform>(entity));
        Command* last = nullptr;
        for (int step = 1; step <= 4; step++){
            const float f = (float)step / 4.0f;
            // as mouseDragEvent: whether Record takes the selection's drag, once per event
            const bool claimed = render->gizmoRecordClaims(sceneProject->id, gizmo, entities);
            for (size_t i = 0; i < entities.size(); i++){
                Vector3 position = start[i].position, scale = start[i].scale;
                Quaternion rotation = start[i].rotation;
                if (gizmo == GizmoSelected::TRANSLATE) position = start[i].position + (target - start[i].position) * f;
                if (gizmo == GizmoSelected::SCALE) scale = start[i].scale + (target - start[i].scale) * f;
                if (gizmo == GizmoSelected::ROTATE){
                    const std::vector<float> xyzw = clipui::xyzwOf(target);
                    rotation = Quaternion::slerp(f, start[i].rotation, quaternionFromXYZW(xyzw.data()));
                }
                const Matrix4 local = Matrix4::translateMatrix(position) * rotation.getRotationMatrix() * Matrix4::scaleMatrix(scale);
                if (Command* command = render->buildObjectTransformDrag(project, sceneProject->id, entities[i], local, gizmo, claimed)){
                    if (CommandHandle::get(sceneProject->id)->addCommand(command)) last = command;
                }
            }
        }
        // the release (as the Scene view's: ends the merge and the record gesture)
        if (last) last->setNoMerge();
        render->mouseReleaseEvent(0.0f, 0.0f);
    }
    if (hook.contains("inspector")){
        // an Inspector Transform row edit: {entities, property: position|rotation|scale, value: [x, y, z]
        // (rotation: Euler degrees)}, through the rows' Record path, else their PropertyCmd
        const Json& edit = hook["inspector"];
        std::vector<Entity> entities;
        Vector3 value;
        const std::string property = edit.is_object() && edit.contains("property") && edit["property"].is_string() ? edit["property"].get<std::string>() : std::string();
        if (!edit.is_object() || !edit.contains("entities") || !entitiesOf(edit["entities"], entities) || !edit.contains("value") ||
            !vectorOf(edit["value"], value) || (property != "position" && property != "rotation" && property != "scale")){
            return fail("inspector", "{entities: [names], property: position|rotation|scale, value: [x, y, z]}");
        }
        InspectorWindow* inspector = Backend::getApp().getInspectorWindow();
        if (!inspector) return fail("inspector", "no Inspector");
        const Quaternion rotation(value.x, value.y, value.z, RotationOrder::ZYX);
        auto valueOf = [&](Entity){
            if (property != "rotation") return std::vector<float>{value.x, value.y, value.z};
            std::vector<float> xyzw(4);
            quaternionToXYZW(rotation, xyzw.data());
            return xyzw;
        };
        if (!inspector->recordRowEdit(sceneProject, ComponentType::Transform, property, entities, valueOf)){
            Command* last = nullptr;
            for (Entity entity : entities){
                Command* command = property == "rotation"
                    ? (Command*)new PropertyCmd<Quaternion>(project, sceneProject->id, entity, ComponentType::Transform, property, rotation)
                    : (Command*)new PropertyCmd<Vector3>(project, sceneProject->id, entity, ComponentType::Transform, property, value);
                if (CommandHandle::get(sceneProject->id)->addCommand(command)) last = command;
            }
            if (last) last->setNoMerge();
        }
        inspector->finishRecordRow();
    }
    if (hook.contains("morph")){
        // a Blend Shapes row (MeshComponent morph weight) let go at `value`: {entities, index, value}
        const Json& edit = hook["morph"];
        std::vector<Entity> entities;
        if (!edit.is_object() || !edit.contains("entities") || !entitiesOf(edit["entities"], entities) || !edit.contains("index") ||
            !edit["index"].is_number_integer() || !edit.contains("value") || !edit["value"].is_number()){
            return fail("morph", "{entities: [names], index, value}");
        }
        InspectorWindow* inspector = Backend::getApp().getInspectorWindow();
        if (!inspector) return fail("morph", "no Inspector");
        // a slider drag: two steps, then the release
        const int index = edit["index"].get<int>();
        const float value = edit["value"].get<float>();
        const MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entities.front());
        const float from = mesh && index >= 0 && index < MAX_MORPHTARGETS ? mesh->morphWeights[index] : 0.0f;
        if (!inspector->setMorphWeight(sceneProject, entities, index, from + (value - from) * 0.5f) ||
            !inspector->setMorphWeight(sceneProject, entities, index, value)){
            inspector->finishMorphEdit();
            return fail("morph", "no mesh with blend shapes to edit (index 0.." + std::to_string(MAX_MORPHTARGETS - 1) + ")");
        }
        inspector->finishMorphEdit();
    }
    if (hook.contains("lattice_drag")){
        // a Scene view drag of lattice points in 4 steps: {entity, points: [i...], offsets: [[x, y, z]...]},
        // through LatticeEditorOps' continuous edit and its end (the drag's release)
        const Json& drag = hook["lattice_drag"];
        std::vector<Entity> entities;
        if (!drag.is_object() || !drag.contains("entity") || !entitiesOf(drag["entity"], entities) || entities.size() != 1 ||
            !drag.contains("points") || !drag["points"].is_array() || !drag.contains("offsets") || !drag["offsets"].is_array() ||
            drag["points"].size() != drag["offsets"].size()){
            return fail("lattice_drag", "{entity, points: [index...], offsets: [[x, y, z]...]}");
        }
        const LatticeComponent* lattice = sceneProject->scene->findComponent<LatticeComponent>(entities.front());
        if (!lattice) return fail("lattice_drag", "the GameObject has no Lattice");
        std::vector<int> points;
        std::vector<Vector3> targets, starts;
        for (size_t i = 0; i < drag["points"].size(); i++){
            Vector3 offset;
            if (!drag["points"][i].is_number_integer() || !vectorOf(drag["offsets"][i], offset)) return fail("lattice_drag", "points are indices, offsets [x, y, z]");
            const int point = drag["points"][i].get<int>();
            points.push_back(point);
            targets.push_back(offset);
            starts.push_back(point >= 0 && (size_t)point < lattice->offsets.size() ? lattice->offsets[(size_t)point] : Vector3(0, 0, 0));
        }
        for (int step = 1; step <= 4; step++){
            std::vector<Vector3> offsets;
            for (size_t i = 0; i < points.size(); i++) offsets.push_back(starts[i] + (targets[i] - starts[i]) * ((float)step / 4.0f));
            if (!LatticeEditorOps::setOffsetsContinuous(project, sceneProject->id, entities.front(), points, offsets, &failure)){
                LatticeEditorOps::endContinuousEdit(sceneProject->id);
                return fail("lattice_drag", failure);
            }
        }
        LatticeEditorOps::endContinuousEdit(sceneProject->id);
    }
    if (hook.contains("curve_select")){
        // clicks on curve points: [{track, time, component}] (+ add: Shift)
        const Json& picks = hook["curve_select"];
        if (!picks.is_array()) return fail("curve_select", "an array of {track, time, component}");
        std::vector<CurveKeyId> points;
        for (const Json& pick : picks){
            KeyId id;
            if (!keyIdOf(pick, id) || !pick.contains("component") || !pick["component"].is_number_integer()) return fail("curve_select", "no point " + pick.dump());
            points.push_back({id, pick["component"].get<int>()});
        }
        selectCurvePoints(points, hook.value("add", false));
    }
    if (hook.contains("curve_drag")){
        // a mouse drag of a key point in the curves view: {track, time, component, time_delta?,
        // value_delta?} (the point is selected first unless it is, as a click; 4 pointer steps)
        const Json& drag = hook["curve_drag"];
        KeyId id;
        int track = -1;
        if (!drag.is_object() || !drag.contains("track") || !trackIndex(drag["track"], track) || !keyIdOf(drag, id) ||
            !drag.contains("component") || !drag["component"].is_number_integer()){
            return fail("curve_drag", "{track, time, component, time_delta?, value_delta?}");
        }
        refreshSelection();
        refreshCurveSelection();
        AnimationClipController::KeyRef ref;
        AnimationClipController::findKey(document()->data(), id.path, id.channel, id.time, ref);
        if (!curvePointSelected(ref.track, ref.key, drag["component"].get<int>())) selectCurvePoints({{id, drag["component"].get<int>()}}, false);
        dragGrabTime = id.time;
        if (!beginCurveDrag(track, drag["component"].get<int>(), &failure)) return fail("curve_drag", failure);
        const float timeDelta = drag.value("time_delta", 0.0f);
        const float valueDelta = drag.value("value_delta", 0.0f);
        for (int step = 1; step <= 4; step++){
            if (!updateCurveDrag(timeDelta * (float)step / 4.0f, valueDelta * (float)step / 4.0f, &failure)) return fail("curve_drag", failure);
        }
        endDrag();
    }
    if (hook.contains("frame_curves")){
        // F / A in the curves view: "selected" or "all"
        const std::string which = hook["frame_curves"].is_string() ? hook["frame_curves"].get<std::string>() : std::string();
        if (which != "selected" && which != "all") return fail("frame_curves", "\"selected\" (F) or \"all\" (A)");
        frameKeys(which == "selected", 800.0f);
        frameCurveValues(which == "selected");
    }
    if (hook.contains("input")){
        // real key events: [{key: "space"|"k"|"comma"|"period"|"delete"|"c"|"v"|"d"|"f"|"a", shift?, ctrl?, alt?}]
        const Json& input = hook["input"];
        if (!input.is_array()) return fail("input", "an array of {key, shift?, ctrl?, alt?}");
        ImGuiIO& io = ImGui::GetIO();
        for (const Json& event : input){
            const std::string name = event.value("key", std::string());
            ImGuiKey key = ImGuiKey_None;
            if (name == "space") key = ImGuiKey_Space;
            else if (name == "k") key = ImGuiKey_K;
            else if (name == "comma") key = ImGuiKey_Comma;
            else if (name == "period") key = ImGuiKey_Period;
            else if (name == "delete") key = ImGuiKey_Delete;
            else if (name == "c") key = ImGuiKey_C;
            else if (name == "v") key = ImGuiKey_V;
            else if (name == "d") key = ImGuiKey_D;
            else if (name == "f") key = ImGuiKey_F;
            else if (name == "a") key = ImGuiKey_A;
            else if (name == "z") key = ImGuiKey_Z;
            else if (name == "y") key = ImGuiKey_Y;
            else if (name == "backspace") key = ImGuiKey_Backspace;
            else if (name == "f2") key = ImGuiKey_F2;
            else if (name == "r") key = ImGuiKey_R;
            else return fail("input", "unknown key '" + name + "'");
            const bool ctrl = event.value("ctrl", false), shift = event.value("shift", false), alt = event.value("alt", false);
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
            if (alt) io.AddKeyEvent(ImGuiMod_Alt, true);
            io.AddKeyEvent(key, true);
            io.AddKeyEvent(key, false);
            if (alt) io.AddKeyEvent(ImGuiMod_Alt, false);
            if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        }
    }
    out = stateJson(sceneProject, hook.value("brief", false));
    for (auto it = extra.begin(); it != extra.end(); ++it) out[it.key()] = it.value();
    if (hook.value("list_paths", false)){
        // what Add Property offers: the addressable paths under the bound (or previewed) root
        Json paths = Json::array();
        Scene* scene = valueScene();
        if (scene && valueRoot() != NULL_ENTITY){
            for (const auto& entry : anim::descendantPaths(scene, valueRoot())) paths.push_back(entry.first);
        }
        out["paths"] = paths;
    }
    return true;
}
