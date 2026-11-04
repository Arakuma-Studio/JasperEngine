// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor tests' driver of the Animation window: AnimationWindowTestAccess and
// AnimationWindowClipTestAccess, the friends AnimationWindow.h and AnimationWindowClip.h declare
// (the PlaySessionTestAccess pattern of Project.h: the editor has no test hook or test-only member
// of its own). AnimationWindowTestAccess::drive runs the window's selection, entity_animation,
// clip, drop and entity_move_frame actions, then the clip mode's: AnimationWindowClipTestAccess::
// drive runs, by name, the same functions the window's buttons, menus and shortcuts run, in this
// order: state, time / frame, preview, play, select / box, drag, ease, delete, copy, paste,
// duplicate, key, add_key, add_property, set_value, remove_track, rebind, frame_rate, snap_keys,
// time_format, preview_model, add_event, select_event, event, drag_event, delete_event,
// create_clip, extract, view, curve_visible, frame_curves, curve_select, curve_drag, record, gizmo,
// inspector, morph, lattice_drag; `state` is the window's state as JSON: mode, clip, state, rows,
// missing, selection, time, preview, events, view, curves, recording... (entity mode: its lanes and
// stored tracks; entity_mode_entries: the dropdown's clip entries). The record sources run through
// the same code as the Scene view and the Inspector: `gizmo` through SceneRender::
// buildObjectTransformDrag (recordGizmoTransform, else the ObjectTransformCmd the drag builds),
// `inspector` / `morph` through the Inspector's Transform / Blend Shapes row functions (an
// InspectorWindow of the test's project, setDriverInspector), and `lattice_drag` through
// LatticeEditorOps' continuous edit (the Scene view's lattice point drag). Every function is
// inline: any test file may include it.

#pragma once

#include "window/AnimationWindowClip.h"
#include "component/Transform.h"
#include "window/AnimationWindowClipUtil.h"
#include "window/AnimationWindowClipCurves.h"
#include "window/AnimationWindow.h"

#include "Out.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "command/CommandHandle.h"
#include "command/type/ObjectTransformCmd.h"
#include "command/type/PropertyCmd.h"
#include "lattice/LatticeEditorOps.h"
#include "render/SceneRender.h"
#include "util/EntityPath.h"
#include "util/FileUtils.h"
#include "window/InspectorWindow.h"

#include "json.hpp"

#include <cmath>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using Json = nlohmann::json;

// the driver's helpers (inline: the header may be included by several test files)
namespace animwindowdriver {

    inline InspectorWindow*& driverInspectorSlot(){
        static InspectorWindow* inspector = nullptr;
        return inspector;
    }

    // the Inspector whose row functions `inspector` and `morph` run (one of the test's project)
    inline void setDriverInspector(InspectorWindow* inspector){ driverInspectorSlot() = inspector; }
    inline InspectorWindow* driverInspector(){ return driverInspectorSlot(); }

    inline const char* const KNOWN_KEYS[] = {
        "entity", "entity_animation", "clip", "root", "state", "time", "frame", "preview", "play", "select", "add", "box",
        "drag", "ease", "delete", "copy", "paste", "duplicate", "key", "add_key", "add_property", "set_value", "remove_track",
        "rebind", "frame_rate", "snap_keys", "time_format", "preview_model", "add_event", "select_event", "event",
        "drag_event", "delete_event", "create_clip", "extract", "list_paths", "entity_mode_entries", "entity_move_frame",
        "brief", "view", "curve_visible", "frame_curves", "curve_drag", "record", "gizmo", "inspector", "morph",
        "lattice_drag", "curve_select", "drop"};

    inline Json argJson(const AnimationEventArg& arg){
        if (std::holds_alternative<double>(arg)) return std::get<double>(arg);
        if (std::holds_alternative<std::string>(arg)) return std::get<std::string>(arg);
        return std::get<bool>(arg);
    }

    inline bool argsFromJson(const Json& json, std::vector<AnimationEventArg>& out, std::string& error){
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

    inline bool checkDriverKeys(const Json& hook, bool clipActions, std::string& error){
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
                                   it.key() == "entity_mode_entries" ||
                                   it.key() == "entity_move_frame" || it.key() == "brief" || it.key() == "drop";
            if (!clipActions && !windowKey){
                error = it.key() + ": the window is in entity mode (select a GameObject with an Animation Player, or open a clip)";
                return false;
            }
        }
        return true;
    }

}

using namespace animwindowdriver;

namespace jasperengine::editor {

    // the clip mode's actions by name (the functions its buttons, menus and shortcuts run) and its
    // state as JSON
    struct AnimationWindowClipTestAccess {
        static bool drive(AnimationWindowClip& self, SceneProject* sceneProject, const Json& actions, Json& out, std::string& error);
        static Json state(AnimationWindowClip& self, SceneProject* sceneProject, bool brief = false);
        // the curves list of state: {track, component, label, visible, key_values, selected}
        static Json curves(const AnimationWindowClip& self);
    };

    // the window's actions (selection, entity Animation, clip, drop and entity frame
    // move), then the clip mode's
    struct AnimationWindowTestAccess {
        static bool drive(AnimationWindow& self, const Json& actions, Json& out, std::string& error);
        // the explicit lane move (a frame drag released on `lane`)
        static bool moveFrameToLane(AnimationWindow& self, SceneProject* sceneProject, size_t frameIndex, uint32_t lane, std::string* error);
    };

}

inline Json AnimationWindowClipTestAccess::curves(const AnimationWindowClip& self){
    nlohmann::json out = nlohmann::json::array();
    std::shared_ptr<AnimationClipDocument> doc = self.document();
    if (!doc) return out;
    const ClipData& data = doc->data();
    for (size_t t = 0; t < data.tracks.size(); t++){
        const ClipTrackData& track = data.tracks[t];
        const std::vector<std::string> names = track.channel == ClipChannel::Morph ? self.componentLabels(t) : std::vector<std::string>();
        const std::vector<Vector3>& eulers = self.trackEulers(t);
        const size_t count = clipcurves::componentCount(track);
        for (size_t c = 0; c < count; c++){
            nlohmann::json values = nlohmann::json::array();
            nlohmann::json selected = nlohmann::json::array();
            for (size_t k = 0; k < track.keys.size() && k < 512; k++){
                const std::vector<float> keyed = clipcurves::keyValues(track, k, eulers);
                values.push_back(c < keyed.size() ? keyed[c] : 0.0f);
                if (self.curvePointSelected(t, k, (int)c)) selected.push_back(track.keys[k].time);
            }
            out.push_back({{"track", t}, {"component", c}, {"label", clipcurves::componentLabel(track, c, names)},
                           {"visible", self.curveVisible(track, (int)c)}, {"key_values", values}, {"selected", selected}});
        }
    }
    return out;
}

inline Json AnimationWindowClipTestAccess::state(AnimationWindowClip& self, SceneProject* sceneProject, bool brief){
    (void)sceneProject;
    AnimationClipController& c = self.controller();
    self.refreshReport();
    self.refreshSelection();
    std::shared_ptr<AnimationClipDocument> doc = self.document();
    SceneProject* bound = self.boundScene();
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
    out["frame"] = (int)std::lround(c.getTime() * self.frameRate());
    out["frame_rate"] = self.frameRate();
    out["length"] = doc ? doc->data().effectiveLength() : 0.0f;
    out["preview"] = c.isPreviewing();
    out["playing"] = c.isPlaying();
    out["time_format"] = self.showFrames ? "frames" : "seconds";
    out["takes_keys"] = c.takesKeys();
    // responsiveness: the CPU time of the clip mode's draw (moving average, worst) and frames
    out["draw_ms"] = self.drawMs;
    out["draw_ms_max"] = self.drawMsMax;
    out["draw_frames"] = self.drawFrames;
    out["frame_count"] = ImGui::GetFrameCount();
    out["preview_blocker"] = self.previewBlocker(self.project->getSelectedScene());
    out["preview_on"] = self.previewModelOf();
    out["undo_to_project"] = c.newestStepIsClipEdit();
    {
        const Entity previewRoot = c.getPreviewRoot();
        SceneProject* previewScene = c.getPreviewSceneId() ? self.project->getScene(c.getPreviewSceneId()) : nullptr;
        out["preview_root"] = previewScene && previewRoot != NULL_ENTITY && previewScene->scene->isEntityCreated(previewRoot)
            ? Json(previewScene->scene->getEntityName(previewRoot)) : Json();
    }
    out["notice"] = self.notice;
    out["notice_warning"] = self.noticeWarning;
    Json rows = Json::array();
    Json missing = Json::array();
    if (doc){
        const ClipData& data = doc->data();
        for (const AnimationWindowClip::Row& row : self.buildRows()){
            if (row.kind == AnimationWindowClip::Row::Kind::Summary) continue;
            const ClipTrackData& track = data.tracks[(size_t)row.track];
            Json entry = {{"label", row.label}, {"track", row.track}, {"missing", row.missing}};
            if (row.kind == AnimationWindowClip::Row::Kind::Component){
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
                entry["value"] = self.sampledValue((size_t)row.track, c.getTime());
                entry["pending"] = c.hasPendingValue((size_t)row.track);
                entry["expanded"] = self.expanded.count(track.path + "|" + anim::channelName(track.channel)) > 0;
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
            for (const EventReport& entry : self.report.events){
                if (entry.event != i) continue;
                handler = entry.status == EventHandlerStatus::Found ? "found" : entry.status == EventHandlerStatus::NotFound ? "missing" : "unchecked";
            }
            events.push_back({{"time", data.events[i].time}, {"function", data.events[i].function}, {"args", args}, {"handler", handler}});
        }
        out["events"] = events;
        self.resolveSelectedEvent();
        out["selected_event"] = self.selectedEvent;
        out["script_functions"] = self.scriptFunctions();
        out["bindings"] = {{"resolved", self.report.resolved}, {"total", self.report.total}, {"legacy_model_layout", self.report.legacyModelLayout}};
    }
    out["rows"] = rows;
    out["missing"] = missing;
    Json selected = Json::array();
    out["selection_count"] = self.selectedRefs.size();
    if (doc && !brief){
        for (const AnimationClipController::KeyRef& ref : self.selectedRefs){
            const ClipTrackData& track = doc->data().tracks[ref.track];
            selected.push_back({{"track", ref.track}, {"key", ref.key}, {"path", track.path},
                                {"channel", anim::channelName(track.channel)}, {"time", track.keys[ref.key].time}});
        }
    }
    out["selection"] = selected;
    out["clipboard"] = self.clipboard.size();
    // T3c: the view, the curves (in the curves view), Record
    out["view"] = self.view == AnimationWindowClip::View::Curves ? "curves" : "dopesheet";
    out["label_width"] = self.labelWidth;
    if (self.view == AnimationWindowClip::View::Curves){
        out["curves"] = curves(self);
        out["curve_range"] = {self.curveMin, self.curveMax};
    }
    out["recording"] = c.isRecording();
    out["record_status"] = self.recordStatus(true);
    out["record_status_short"] = self.recordStatus(false);
    return out;
}


inline bool AnimationWindowClipTestAccess::drive(AnimationWindowClip& self, SceneProject* sceneProject, const Json& hook, Json& out, std::string& error){
    AnimationClipController& c = self.controller();
    self.refreshBinding();
    auto fail = [&](const std::string& key, const std::string& message){
        error = key + ": " + message;
        return false;
    };
    std::string failure;
    // a key of the open clip: {track, time} or {path, channel, time}
    auto keyIdOf = [&](const Json& json, AnimationWindowClip::KeyId& id) -> bool{
        std::shared_ptr<AnimationClipDocument> doc = self.document();
        if (!doc || !json.is_object() || !json.contains("time") || !json["time"].is_number()) return false;
        id.time = json["time"].get<float>();
        if (json.contains("track")){
            if (!json["track"].is_number_integer()) return false;
            const int track = json["track"].get<int>();
            if (track < 0 || track >= (int)doc->data().tracks.size()) return false;
            // the track's own identity (property tracks: their component and property)
            const ClipTrackData& like = doc->data().tracks[(size_t)track];
            AnimationClipController::KeyRef ref;
            if (!AnimationClipController::findKey(doc->data(), like, id.time, ref)) return false;
            id = AnimationWindowClip::keyIdOf(like, doc->data().tracks[ref.track].keys[ref.key].time);
            return true;
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
        std::shared_ptr<AnimationClipDocument> doc = self.document();
        if (!doc || !json.is_number_integer()) return false;
        out = json.get<int>();
        return out >= 0 && out < (int)doc->data().tracks.size();
    };

    if (hook.contains("state")){
        if (!hook["state"].is_string()) return fail("state", "a state name of the bound Animation Player");
        if (!self.openState(hook["state"].get<std::string>(), &failure)) return fail("state", failure);
    }
    if (hook.contains("time")){
        if (!hook["time"].is_number()) return fail("time", "seconds (number)");
        self.seek(hook["time"].get<float>());
    }
    if (hook.contains("frame")){
        if (!hook["frame"].is_number_integer()) return fail("frame", "a frame number");
        self.seek((float)hook["frame"].get<int>() / self.frameRate());
    }
    if (hook.contains("preview")){
        if (!hook["preview"].is_boolean()) return fail("preview", "true starts the preview, false stops it");
        if (hook["preview"].get<bool>() != c.isPreviewing()){
            if (hook["preview"].get<bool>()){
                if (!self.startPreview(sceneProject, &failure)) return fail("preview", failure);
            }else{
                self.stopPreview();
            }
        }
    }
    if (hook.contains("play")){
        if (!hook["play"].is_boolean()) return fail("play", "true plays, false pauses");
        if (hook["play"].get<bool>() != c.isPlaying()){
            if (hook["play"].get<bool>() && !c.isPreviewing() && !self.startPreview(sceneProject, &failure)) return fail("play", failure);
            self.togglePlay(sceneProject);
        }
    }
    if (hook.contains("select")){
        if (!hook["select"].is_array()) return fail("select", "an array of keys {track, time} or {path, channel, time}");
        std::vector<AnimationWindowClip::KeyId> ids;
        for (const Json& value : hook["select"]){
            AnimationWindowClip::KeyId id;
            if (!keyIdOf(value, id)) return fail("select", "no key " + value.dump());
            ids.push_back(id);
        }
        self.selectKeys(ids, hook.value("add", false));
    }
    if (hook.contains("box")){
        const Json& box = hook["box"];
        if (!box.is_object() || !box.contains("rows") || !box.contains("times") || !box["rows"].is_array() || box["rows"].size() != 2 ||
            !box["times"].is_array() || box["times"].size() != 2){
            return fail("box", "{rows: [first, last] (0 = the summary row), times: [from, to]}");
        }
        self.boxSelect(box["rows"][0].get<int>(), box["rows"][1].get<int>(), box["times"][0].get<float>(), box["times"][1].get<float>(),
                  hook.value("add", false));
    }
    if (hook.contains("drag")){
        // a mouse drag of the selection by `drag` seconds (frame snapped, the grabbed key = the first)
        if (!hook["drag"].is_number()) return fail("drag", "seconds (number)");
        self.refreshSelection();
        if (self.selection.empty()) return fail("drag", "no keys selected");
        self.dragGrabTime = self.selection.front().time;
        if (!self.beginDrag(&failure)) return fail("drag", failure);
        // the pointer moves in steps, like a real drag, then the total
        const float total = hook["drag"].get<float>();
        for (int step = 1; step <= 4; step++){
            if (!self.updateDrag(total * (float)step / 4.0f, &failure)) return fail("drag", failure);
        }
        self.endDrag();
    }
    if (hook.contains("ease")){
        EaseType ease;
        if (!hook["ease"].is_string() || !anim::easeFromName(hook["ease"].get<std::string>(), ease)) return fail("ease", "an ease name (linear, step, cubic_in_out...)");
        if (!self.setEase(ease, &failure)) return fail("ease", failure);
    }
    if (hook.value("delete", false) && !self.deleteSelected(&failure)) return fail("delete", failure);
    if (hook.value("copy", false)) self.copySelected();
    if (hook.contains("paste")){
        const float at = hook["paste"].is_number() ? hook["paste"].get<float>() : c.getTime();
        if (!self.paste(at, &failure)) return fail("paste", failure);
    }
    if (hook.value("duplicate", false) && !self.duplicateSelected(&failure)) return fail("duplicate", failure);
    if (hook.contains("key")){
        const std::string mode = hook["key"].is_string() ? hook["key"].get<std::string>() : std::string();
        bool ok = false;
        if (mode == "all") ok = self.keyAt(false, &failure);
        else if (mode == "modified") ok = self.keyAt(true, &failure);
        else return fail("key", "\"all\" (K) or \"modified\" (Shift+K)");
        if (!ok) return fail("key", failure);
    }
    if (hook.contains("add_key")){
        // the context menu's Add Key: {track (-1 / absent: all), time}
        const Json& add = hook["add_key"];
        if (!add.is_object() || !add.contains("time") || !add["time"].is_number()) return fail("add_key", "{track?, time}");
        const int track = add.contains("track") ? add["track"].get<int>() : -1;
        if (!self.addKeysAt(track, add["time"].get<float>(), &failure)) return fail("add_key", failure);
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
        c.setTime(self.snapToFrame(c.getTime()));
        if (!c.addProperty(add["path"].get<std::string>(), channel, &track, &failure)) return fail("add_property", failure);
        std::shared_ptr<AnimationClipDocument> doc = self.document();
        if (doc && track < doc->data().tracks.size()){
            self.expanded.insert(doc->data().tracks[track].path + "|" + anim::channelName(doc->data().tracks[track].channel));
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
        if (!self.setComponentValue((size_t)track, set["component"].get<int>(), set["value"].get<float>(), true, &failure)) return fail("set_value", failure);
    }
    if (hook.contains("remove_track")){
        int track = -1;
        if (!trackIndex(hook["remove_track"], track)) return fail("remove_track", "no such track");
        if (!c.removeTrack((size_t)track, &failure)) return fail("remove_track", failure);
        self.selection.clear();
    }
    if (hook.contains("rebind")){
        const Json& rebind = hook["rebind"];
        int track = -1;
        if (!rebind.is_object() || !rebind.contains("track") || !trackIndex(rebind["track"], track) || !rebind.contains("path") ||
            !rebind["path"].is_string()){
            return fail("rebind", "{track, path}");
        }
        if (!c.retargetTrack((size_t)track, rebind["path"].get<std::string>(), &failure)) return fail("rebind", failure);
        const size_t users = findClipUsers(self.project, self.document()->path()).size();
        self.setNotice("Rebound to '" + rebind["path"].get<std::string>() + "' (changes the clip for " + std::to_string(users) +
                  (users == 1 ? " user" : " users") + ")", false);
        self.reportRevision = ~uint64_t(0);
    }
    if (hook.contains("frame_rate")){
        if (!hook["frame_rate"].is_number()) return fail("frame_rate", "samples per second (1..240)");
        if (!self.setFrameRate(hook["frame_rate"].get<float>(), &failure)) return fail("frame_rate", failure);
    }
    if (hook.value("snap_keys", false) && !self.snapKeysToFrames(&failure)) return fail("snap_keys", failure);
    if (hook.contains("time_format")){
        const std::string format = hook["time_format"].is_string() ? hook["time_format"].get<std::string>() : std::string();
        if (format != "frames" && format != "seconds") return fail("time_format", "\"frames\" or \"seconds\"");
        self.showFrames = format == "frames";
    }
    if (hook.contains("preview_model")){
        if (!hook["preview_model"].is_string()) return fail("preview_model", "a model or prefab path");
        if (!self.setPreviewModel(hook["preview_model"].get<std::string>(), &failure)) return fail("preview_model", failure);
    }
    if (hook.contains("add_event")){
        const Json& add = hook["add_event"];
        if (!add.is_object()) return fail("add_event", "{time?, function?, args?}");
        std::vector<AnimationEventArg> args;
        if (add.contains("args") && !argsFromJson(add["args"], args, failure)) return fail("add_event", failure);
        const float at = add.contains("time") && add["time"].is_number() ? add["time"].get<float>() : self.snapToFrame(c.getTime());
        if (!self.addEvent(at, add.value("function", std::string()), args, &failure)) return fail("add_event", failure);
        self.openEventEditor = true;
    }
    if (hook.contains("select_event")){
        std::shared_ptr<AnimationClipDocument> doc = self.document();
        if (!hook["select_event"].is_number_integer() || !doc || hook["select_event"].get<int>() < -1 ||
            hook["select_event"].get<int>() >= (int)doc->data().events.size()){
            return fail("select_event", "an event index (-1: none)");
        }
        self.selectEvent(hook["select_event"].get<int>());
    }
    if (hook.contains("event")){
        // the event editor: {function?, args?, time?} of the selected event
        std::shared_ptr<AnimationClipDocument> doc = self.document();
        const Json& edit = hook["event"];
        self.resolveSelectedEvent();
        if (!doc || self.selectedEvent < 0 || self.selectedEvent >= (int)doc->data().events.size()) return fail("event", "no event selected");
        if (!edit.is_object()) return fail("event", "{function?, args?, time?}");
        ClipEventData event = doc->data().events[(size_t)self.selectedEvent];
        if (edit.contains("function")) event.function = edit["function"].get<std::string>();
        if (edit.contains("time")) event.time = edit["time"].get<float>();
        if (edit.contains("args") && !argsFromJson(edit["args"], event.args, failure)) return fail("event", failure);
        if (!self.editEvent(self.selectedEvent, event, "", &failure)) return fail("event", failure);
    }
    if (hook.contains("drag_event")){
        // a drag of the selected event marker by seconds (frame snapped, one merged step)
        std::shared_ptr<AnimationClipDocument> doc = self.document();
        if (!hook["drag_event"].is_number()) return fail("drag_event", "seconds (number)");
        self.resolveSelectedEvent();
        if (!doc || self.selectedEvent < 0 || self.selectedEvent >= (int)doc->data().events.size()) return fail("drag_event", "no event selected");
        const float base = doc->data().events[(size_t)self.selectedEvent].time;
        self.eventMergeKey = "clipevent#" + std::to_string(++self.eventDragSerial);
        const float total = hook["drag_event"].get<float>();
        for (int step = 1; step <= 4; step++){
            const float at = self.snapToFrame(std::max(0.0f, base + total * (float)step / 4.0f));
            if (!self.moveEvent(self.selectedEvent, at, self.eventMergeKey, &failure)) return fail("drag_event", failure);
        }
    }
    if (hook.value("delete_event", false)){
        // as Delete with the event selected (nothing selected: nothing deleted)
        self.resolveSelectedEvent();
        if (self.selectedEvent < 0) return fail("delete_event", "no event selected");
        if (!self.deleteEvent(self.selectedEvent, &failure)) return fail("delete_event", failure);
    }
    if (hook.contains("create_clip")){
        // the empty state's Create... / the dropdown's Create New Clip... without the save dialog
        if (!hook["create_clip"].is_string()) return fail("create_clip", "the new .anim path (project-relative)");
        if (!self.createNewClip(sceneProject, hook["create_clip"].get<std::string>(), &failure)) return fail("create_clip", failure);
    }
    if (hook.contains("extract")){
        if (!hook["extract"].is_string()) return fail("extract", "the new .anim path (project-relative)");
        if (!self.extract(sceneProject, hook["extract"].get<std::string>(), &failure)) return fail("extract", failure);
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
        self.view = name == "curves" ? AnimationWindowClip::View::Curves : AnimationWindowClip::View::Dopesheet;
        self.saveSettings();
    }
    if (hook.contains("curve_visible")){
        // a curve's visibility toggle: {track, component, visible}
        const Json& toggle = hook["curve_visible"];
        int track = -1;
        if (!toggle.is_object() || !toggle.contains("track") || !trackIndex(toggle["track"], track) || !toggle.contains("component") ||
            !toggle["component"].is_number_integer() || !toggle.contains("visible") || !toggle["visible"].is_boolean()){
            return fail("curve_visible", "{track, component, visible}");
        }
        self.setCurveVisible(self.document()->data().tracks[(size_t)track], toggle["component"].get<int>(), toggle["visible"].get<bool>());
    }
    if (hook.contains("record")){
        if (!hook["record"].is_boolean()) return fail("record", "true starts Record (and the preview), false stops it");
        if (!self.setRecording(sceneProject, hook["record"].get<bool>(), &failure)) return fail("record", failure);
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
        if (drag.value("select", true)) self.project->replaceSelectedEntities(sceneProject->id, entities);
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
                if (Command* command = render->buildObjectTransformDrag(self.project, sceneProject->id, entities[i], local, gizmo, claimed)){
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
        InspectorWindow* inspector = driverInspector();
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
                    ? (Command*)new PropertyCmd<Quaternion>(self.project, sceneProject->id, entity, ComponentType::Transform, property, rotation)
                    : (Command*)new PropertyCmd<Vector3>(self.project, sceneProject->id, entity, ComponentType::Transform, property, value);
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
        InspectorWindow* inspector = driverInspector();
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
            if (!LatticeEditorOps::setOffsetsContinuous(self.project, sceneProject->id, entities.front(), points, offsets, &failure)){
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
        std::vector<AnimationWindowClip::CurveKeyId> points;
        for (const Json& pick : picks){
            AnimationWindowClip::KeyId id;
            if (!keyIdOf(pick, id) || !pick.contains("component") || !pick["component"].is_number_integer()) return fail("curve_select", "no point " + pick.dump());
            points.push_back({id, pick["component"].get<int>()});
        }
        self.selectCurvePoints(points, hook.value("add", false));
    }
    if (hook.contains("curve_drag")){
        // a mouse drag of a key point in the curves view: {track, time, component, time_delta?,
        // value_delta?} (the point is selected first unless it is, as a click; 4 pointer steps)
        const Json& drag = hook["curve_drag"];
        AnimationWindowClip::KeyId id;
        int track = -1;
        if (!drag.is_object() || !drag.contains("track") || !trackIndex(drag["track"], track) || !keyIdOf(drag, id) ||
            !drag.contains("component") || !drag["component"].is_number_integer()){
            return fail("curve_drag", "{track, time, component, time_delta?, value_delta?}");
        }
        self.refreshSelection();
        self.refreshCurveSelection();
        AnimationClipController::KeyRef ref;
        AnimationClipController::findKey(self.document()->data(), id.path, id.channel, id.time, ref);
        if (!self.curvePointSelected(ref.track, ref.key, drag["component"].get<int>())) self.selectCurvePoints({{id, drag["component"].get<int>()}}, false);
        self.dragGrabTime = id.time;
        if (!self.beginCurveDrag(track, drag["component"].get<int>(), &failure)) return fail("curve_drag", failure);
        const float timeDelta = drag.value("time_delta", 0.0f);
        const float valueDelta = drag.value("value_delta", 0.0f);
        for (int step = 1; step <= 4; step++){
            if (!self.updateCurveDrag(timeDelta * (float)step / 4.0f, valueDelta * (float)step / 4.0f, &failure)) return fail("curve_drag", failure);
        }
        self.endDrag();
    }
    if (hook.contains("frame_curves")){
        // F / A in the curves view: "selected" or "all"
        const std::string which = hook["frame_curves"].is_string() ? hook["frame_curves"].get<std::string>() : std::string();
        if (which != "selected" && which != "all") return fail("frame_curves", "\"selected\" (F) or \"all\" (A)");
        self.frameKeys(which == "selected", 800.0f);
        self.frameCurveValues(which == "selected");
    }
    out = state(self, sceneProject, hook.value("brief", false));
    for (auto it = extra.begin(); it != extra.end(); ++it) out[it.key()] = it.value();
    if (hook.value("list_paths", false)){
        // what Add Property offers: the addressable paths under the bound (or previewed) root
        Json paths = Json::array();
        Scene* scene = self.valueScene();
        if (scene && self.valueRoot() != NULL_ENTITY){
            for (const auto& entry : anim::descendantPaths(scene, self.valueRoot())) paths.push_back(entry.first);
        }
        out["paths"] = paths;
    }
    return true;
}

// ---- the entity mode's part (AnimationWindow) ----

inline bool AnimationWindowTestAccess::moveFrameToLane(AnimationWindow& self, SceneProject* sceneProject, size_t frameIndex, uint32_t lane, std::string* error){
    Scene* scene = sceneProject ? sceneProject->scene : nullptr;
    AnimationComponent* anim = scene && self.selectedEntity != NULL_ENTITY && scene->isEntityCreated(self.selectedEntity)
        ? scene->findComponent<AnimationComponent>(self.selectedEntity) : nullptr;
    if (!anim || frameIndex >= anim->actions.size()){
        if (error) *error = "no such frame of the shown Animation";
        return false;
    }
    self.autoAssignTracks(*anim, sceneProject);
    // a frame drag released on `lane` at the same time
    self.isDraggingFrame = true;
    self.draggingFrameIndex = (int)frameIndex;
    self.dragStartTime = anim->actions[frameIndex].startTime;
    self.dragStartTrack = self.laneOf(*anim, frameIndex);
    self.draggingLane = (int)lane;
    self.finishTimelineDrag(scene, sceneProject, true);
    return true;
}

inline bool AnimationWindowTestAccess::drive(AnimationWindow& self, const nlohmann::json& hook, nlohmann::json& out, std::string& error){
    if (!hook.is_object()){
        error = "an object {entity, clip, root, state, time, preview, select, drag, ...} (see AnimationWindowClipTest.cpp)";
        return false;
    }
    if (!checkDriverKeys(hook, true, error)) return false;
    SceneProject* sceneProject = self.project->getSelectedScene();
    if (!sceneProject || !sceneProject->scene){
        error = "no scene is open";
        return false;
    }
    auto resolve = [&](const nlohmann::json& ref, Entity& entity) -> bool{
        if (!ref.is_string()){
            error = "a GameObject name or path (string)";
            return false;
        }
        const std::vector<Entity> found = EntityPath::resolve(*sceneProject, ref.get<std::string>());
        if (found.size() != 1){
            error = found.empty() ? "no GameObject '" + ref.get<std::string>() + "'" : "'" + ref.get<std::string>() + "' is ambiguous";
            return false;
        }
        entity = found.front();
        return true;
    };
    self.setOpen(true);
    // the selection decides the mode, as a click in the Hierarchy does
    if (hook.contains("entity")){
        if (hook["entity"].is_null()){
            self.project->clearSelectedEntities(sceneProject->id);
        }else{
            Entity entity = NULL_ENTITY;
            if (!resolve(hook["entity"], entity)){
                error = "entity: " + error;
                return false;
            }
            self.project->replaceSelectedEntities(sceneProject->id, {entity});
        }
        self.applySelection(sceneProject, self.project->getSelectedEntities(sceneProject->id));
    }
    if (hook.contains("entity_animation")){
        Entity animation = NULL_ENTITY;
        if (!resolve(hook["entity_animation"], animation)){
            error = "entity_animation: " + error;
            return false;
        }
        self.openEntityAnimation(sceneProject->id, animation);
    }
    // the Project window's double-click (target-less) or the Inspector's open (with root)
    if (hook.contains("clip")){
        if (!hook["clip"].is_string()){
            error = "clip: a .anim path or \"<model>#<clip>\"";
            return false;
        }
        Entity root = NULL_ENTITY;
        if (hook.contains("root") && !resolve(hook["root"], root)){
            error = "root: " + error;
            return false;
        }
        self.stopTransientPreviews();
        self.clipModeActive = true;
        if (!self.clipMode->open(sceneProject->id, root, hook["clip"].get<std::string>(), &error)){
            error = "clip: " + error;
            return false;
        }
    }
    // .anim files dropped on the window (project-relative or absolute paths)
    if (hook.contains("drop")){
        std::vector<std::string> paths;
        const nlohmann::json& drop = hook["drop"];
        for (const nlohmann::json& path : drop.is_array() ? drop : nlohmann::json::array({drop})){
            if (!path.is_string()){
                error = "drop: a path or a list of paths of the files dropped";
                return false;
            }
            const std::filesystem::path given = editor::FileUtils::pathFromUtf8(path.get<std::string>());
            paths.push_back(FileUtils::pathToGenericUtf8((given.is_absolute() ? given : self.project->getProjectPath() / given).lexically_normal()));
        }
        if (!self.dropClipFiles(sceneProject, paths, &error)){
            error = "drop: " + error;
            return false;
        }
    }
    if (hook.contains("entity_move_frame")){
        const nlohmann::json& move = hook["entity_move_frame"];
        if (self.clipModeActive || !move.is_object() || !move.contains("index") || !move.contains("lane") ||
            !move["index"].is_number_integer() || !move["lane"].is_number_integer() || move["lane"].get<int>() < 0){
            error = "entity_move_frame: {index, lane} of the shown entity Animation";
            return false;
        }
        if (!moveFrameToLane(self, self.project->getScene(self.selectedSceneId), (size_t)move["index"].get<int>(), (uint32_t)move["lane"].get<int>(), &error)){
            error = "entity_move_frame: " + error;
            return false;
        }
    }
    if (self.clipModeActive){
        try{
            if (!AnimationWindowClipTestAccess::drive(*self.clipMode, sceneProject, hook, out, error)) return false;
        }catch (const nlohmann::json::exception& e){
            // a value of the wrong type
            error = std::string("bad value: ") + e.what();
            return false;
        }
    }else{
        if (!checkDriverKeys(hook, false, error)) return false;
        out = AnimationWindowClipTestAccess::state(*self.clipMode, sceneProject);
        // entity mode: the clip state is not shown; name the entity Animation
        out["mode"] = "entity";
        Scene* scene = sceneProject->scene;
        out["entity_animation"] = self.selectedEntity != NULL_ENTITY && scene->isEntityCreated(self.selectedEntity)
            ? nlohmann::json(scene->getEntityName(self.selectedEntity)) : nlohmann::json();
        out["entity_preview"] = self.isPreviewing;
        Scene* shown = self.project->getScene(self.selectedSceneId) ? self.project->getScene(self.selectedSceneId)->scene : nullptr;
        nlohmann::json lanes = nlohmann::json::array();
        nlohmann::json tracks = nlohmann::json::array();
        if (shown && self.selectedEntity != NULL_ENTITY && shown->isEntityCreated(self.selectedEntity)){
            if (AnimationComponent* anim = shown->findComponent<AnimationComponent>(self.selectedEntity)){
                self.autoAssignTracks(*anim, self.project->getScene(self.selectedSceneId));
                for (size_t i = 0; i < anim->actions.size(); i++){
                    lanes.push_back(self.laneOf(*anim, i));
                    tracks.push_back(anim->actions[i].track);
                }
            }
        }
        // the lanes shown (view only) and the stored ActionFrame::track values
        out["entity_lanes"] = lanes;
        out["entity_tracks"] = tracks;
    }
    if (hook.value("entity_mode_entries", false)){
        // the entity mode dropdown's clip entries for the scene selection (R31)
        const std::vector<Entity> selected = self.project->getSelectedEntities(sceneProject->id);
        out["entity_mode_entries"] = selected.empty() ? std::vector<std::string>() : self.clipMode->entityModeEntries(sceneProject, selected.front());
    }
    return true;
}
